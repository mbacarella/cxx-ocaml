// Port of lambda/translprim.ml (TYPECHECKER.md stage 10).
//
// Deviations: Config.with_frame_pointers is read only in native code, where
// it is false; the error printers belong to the error-report stage.

#include "cppcaml/typing/translprim.hpp"
#include "cppcaml/typing/location.hpp"

#include <algorithm>
#include <stdexcept>

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/hashtbl.hpp"
#include "cppcaml/typing/matching.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/typeopt.hpp"

namespace cppcaml::typing::translprim {

namespace tt = typedtree;
using namespace lambda;
using lam_t = cppcaml::typing::lambda::lambda;
using ValueKind = cppcaml::typing::lambda::ValueKind;
using debuginfo::to_location;
using PK = Primitive::K;

namespace {

// Config
constexpr bool config_flat_float_array = true;
constexpr bool config_with_frame_pointers = false;

}  // namespace

// ---- Insertion of debugging events -------------------------------------------

lam_t event_before(const ScopedLocation& loc, const tt::Expression* exp, lam_t lam) {
  if (as<Lstaticraise>(lam)) return lam;
  if (clflags::debug && !clflags::native_code)
    return levent(lam, make<LambdaEvent>(LambdaEvent{loc, EventKind::Lev_before, nullptr, nullptr, exp->exp_env}));
  return lam;
}

lam_t event_after(const ScopedLocation& loc, const tt::Expression* exp, lam_t lam) {
  if (clflags::debug && !clflags::native_code)
    return levent(lam,
                  make<LambdaEvent>(LambdaEvent{loc, EventKind::Lev_after, exp->exp_type, nullptr, exp->exp_env}));
  return lam;
}

namespace {

enum class Comparison { Equal, Not_equal, Less_equal, Less_than, Greater_equal, Greater_than, Compare };
enum class ComparisonKind {
  Compare_generic, Compare_ints, Compare_floats, Compare_strings, Compare_bytes, Compare_nativeints,
  Compare_int32s, Compare_int64s
};
enum class LocKind { Loc_FILE, Loc_LINE, Loc_MODULE, Loc_LOC, Loc_POS, Loc_FUNCTION };
enum class AtomicKind { Ref, Field, Loc };
enum class AtomicOp { Load, Exchange, Cas, Faa };

struct Prim {
  enum class Kind {
    Primitive, External, Sys_argv, Comparison, Raise, Raise_with_backtrace, Todo, Lazy_force, Loc, Send,
    Send_self, Send_cache, Frame_pointers, Identity, Apply, Revapply, Atomic, Atomic_index, Check_array_bound
  };
  Kind kind;
  Primitive p{PK::Pignore};  // Primitive
  long arity = 0;            // Primitive
  const PrimitiveDescription* ext = nullptr;  // External
  Comparison comp = Comparison::Equal;        // Comparison
  ComparisonKind ck = ComparisonKind::Compare_generic;
  RaiseKind raise = RaiseKind::Raise_regular;  // Raise
  LocKind loc = LocKind::Loc_FILE;             // Loc
  AtomicOp op = AtomicOp::Load;                // Atomic
  AtomicKind ak = AtomicKind::Ref;
};

Prim k(Prim::Kind kd) { return Prim{kd}; }
Prim P(const Primitive& p, long arity) {
  Prim r{Prim::Kind::Primitive};
  r.p = p;
  r.arity = arity;
  return r;
}
Prim cmp(Comparison c, ComparisonKind kd) {
  Prim r{Prim::Kind::Comparison};
  r.comp = c;
  r.ck = kd;
  return r;
}
Prim rz(RaiseKind rk) {
  Prim r{Prim::Kind::Raise};
  r.raise = rk;
  return r;
}
Prim lc(LocKind l) {
  Prim r{Prim::Kind::Loc};
  r.loc = l;
  return r;
}
Prim at(AtomicOp op, AtomicKind kd) {
  Prim r{Prim::Kind::Atomic};
  r.op = op;
  r.ak = kd;
  return r;
}

// Lambda.primitive builders
Primitive pr(PK kd) { return prim(kd); }
Primitive pfield(long n, ImmediateOrPointer ptr, MutableFlag mut) {
  Primitive p = prim(PK::Pfield);
  p.n = n;
  p.ptr = ptr;
  p.mut = mut;
  return p;
}
Primitive psetfield(long n, ImmediateOrPointer ptr, InitializationOrAssignment init) {
  Primitive p = prim(PK::Psetfield);
  p.n = n;
  p.ptr = ptr;
  p.init = init;
  return p;
}
Primitive pmakeblock(long tag, MutableFlag mut) {
  Primitive p = prim(PK::Pmakeblock);
  p.n = tag;
  p.mut = mut;
  return p;
}
Primitive pctconst(CompileTimeConstant c) {
  Primitive p = prim(PK::Pctconst);
  p.ctconst = c;
  return p;
}
Primitive pn(PK kd, long n) {
  Primitive p = prim(kd);
  p.n = n;
  return p;
}
Primitive psafe(PK kd, IsSafe s) {
  Primitive p = prim(kd);
  p.safe = s;
  return p;
}
Primitive picmp(IntegerComparison c) {
  Primitive p = prim(PK::Pintcomp);
  p.icmp = c;
  return p;
}
Primitive ppcmp(PhysicalComparison c) {
  Primitive p = prim(PK::Pphyscomp);
  p.pcmp = c;
  return p;
}
Primitive pfcmp(FloatComparison c) {
  Primitive p = prim(PK::Pfloatcomp);
  p.fcmp = c;
  return p;
}
Primitive parr(PK kd, ArrayKind a) {
  Primitive p = prim(kd);
  p.array = a;
  return p;
}
Primitive pbi(PK kd, BoxedInteger bi) {
  Primitive p = prim(kd);
  p.bi = bi;
  return p;
}
Primitive pbisafe(PK kd, BoxedInteger bi) {
  Primitive p = pbi(kd, bi);
  p.safe = IsSafe::Safe;
  return p;
}
Primitive pcvt(BoxedInteger a, BoxedInteger b) {
  Primitive p = prim(PK::Pcvtbint);
  p.bi = a;
  p.bi2 = b;
  return p;
}
Primitive pbicmp(BoxedInteger bi, IntegerComparison c) {
  Primitive p = prim(PK::Pbintcomp);
  p.bi = bi;
  p.icmp = c;
  return p;
}
Primitive pba(PK kd, bool unsafe, long n) {
  Primitive p = prim(kd);
  p.unsafe = unsafe;
  p.n = n;
  p.ba_kind = BigarrayKind::Pbigarray_unknown;
  p.ba_layout = BigarrayLayout::Pbigarray_unknown_layout;
  return p;
}
Primitive pu(PK kd, bool unsafe) {
  Primitive p = prim(kd);
  p.unsafe = unsafe;
  return p;
}
Primitive pccall(const PrimitiveDescription* d) {
  Primitive p = prim(PK::Pccall);
  p.ccall = d;
  return p;
}
Primitive praise(RaiseKind rk) {
  Primitive p = prim(PK::Praise);
  p.raise = rk;
  return p;
}

// Primitive.simple ~name ~arity ~alloc
const PrimitiveDescription* simple(std::string_view name, long arity, bool alloc) {
  ZoneScope perm(permanent_zone());
  std::vector<NativeRepr> reprs(static_cast<std::size_t>(arity));
  return make<PrimitiveDescription>(
      PrimitiveDescription{zstr(name), arity, alloc, zstr(""), slice(reprs), NativeRepr{}});
}

// used_primitives = Hashtbl.create 7 : (Path.t, Location.t) Hashtbl.t.
// Its fold order reaches output: Translmod.required_globals adds the paths'
// heads to an Ident.Set in that order, the set keeping the first of equal
// idents -- whose name string the .cmo's cu_required_compunits carries.
// So the table is OCaml's (hashtbl.hpp), keyed by Hashtbl.hash on the path
// value (caml_hash 10 100 0 over Path.t / Ident.t's block layout) and
// compared structurally (compare p p' = 0).
struct PathKey {
  Path::t p;
  bool operator==(const PathKey& o) const { return path::compare(p, o.p) == 0; }
};
struct HashPath {
  // a block's header as caml_hash mixes it (Cleanhd_hd: wosize lsl 10 lor tag)
  static std::uint32_t mix_header(std::uint32_t h, std::uint32_t wosize, std::uint32_t tag) {
    return hashtbl::detail::mix_uint32(h, (wosize << 10) | tag);
  }
  long operator()(const PathKey& k) const {
    namespace d = hashtbl::detail;
    // the queue of caml_hash: a path, an ident, a string, an int, or an
    // extra_ty's Pcstr_ty block
    struct Item {
      enum class K : std::uint8_t { Path, Ident, Str, Int, Cstr } k;
      const void* x = nullptr;
      std::string_view s;
      long i = 0;
    };
    std::vector<Item> q;
    q.reserve(16);
    q.push_back({Item::K::Path, k.p});
    const std::size_t sz = 100;
    long num = 10;
    std::uint32_t h = 0;
    auto push = [&](Item it) {
      if (q.size() < sz) q.push_back(it);
    };
    for (std::size_t rd = 0; rd < q.size() && num > 0; ++rd) {
      Item it = q[rd];
      switch (it.k) {
        case Item::K::Int:
          h = d::mix_intnat(h, 2 * static_cast<std::int64_t>(it.i) + 1);  // Val_long
          --num;
          break;
        case Item::K::Str:
          h = d::mix_string(h, std::string(it.s));
          --num;
          break;
        case Item::K::Cstr:  // Pcstr_ty of string
          h = mix_header(h, 1, 0);
          push({Item::K::Str, nullptr, it.s});
          break;
        case Item::K::Path: {
          auto* p = static_cast<Path::t>(it.x);
          switch (p->kind) {
            case Path::Kind::Pident:
              h = mix_header(h, 1, 0);
              push({Item::K::Ident, p->id});
              break;
            case Path::Kind::Pdot:
              h = mix_header(h, 2, 1);
              push({Item::K::Path, p->p1});
              push({Item::K::Str, nullptr, p->s});
              break;
            case Path::Kind::Papply:
              h = mix_header(h, 2, 2);
              push({Item::K::Path, p->p1});
              push({Item::K::Path, p->p2});
              break;
            case Path::Kind::Pextra_ty:
              h = mix_header(h, 2, 3);
              push({Item::K::Path, p->p1});
              if (p->extra == Path::Extra::Pcstr_ty) push({Item::K::Cstr, nullptr, p->s});
              else push({Item::K::Int, nullptr, {}, 0});  // Pext_ty
              break;
          }
          break;
        }
        case Item::K::Ident: {
          auto* id = static_cast<Ident::t>(it.x);
          switch (id->kind) {
            case Ident::Kind::Local:  // Local of { name; stamp }
              h = mix_header(h, 2, 0);
              push({Item::K::Str, nullptr, id->name_});
              push({Item::K::Int, nullptr, {}, id->stamp_});
              break;
            case Ident::Kind::Scoped:  // Scoped of { name; stamp; scope }
              h = mix_header(h, 3, 1);
              push({Item::K::Str, nullptr, id->name_});
              push({Item::K::Int, nullptr, {}, id->stamp_});
              push({Item::K::Int, nullptr, {}, id->scope_});
              break;
            case Ident::Kind::Global:  // Global of string
              h = mix_header(h, 1, 2);
              push({Item::K::Str, nullptr, id->name_});
              break;
            case Ident::Kind::Predef:  // Predef of { name; stamp }
              h = mix_header(h, 2, 3);
              push({Item::K::Str, nullptr, id->name_});
              push({Item::K::Int, nullptr, {}, id->stamp_});
              break;
            case Ident::Kind::Unscoped:  // not a path head a primitive reaches
              throw std::logic_error("Translprim.used_primitives: Unscoped ident");
          }
          break;
        }
      }
    }
    return d::final_mix(h);
  }
};
hashtbl::Hashtbl<PathKey, Location, HashPath> used_primitives{7};

void add_used_primitive(const Location& loc, env::t env, Path::t path) {
  if (!path || path->kind != Path::Kind::Pdot) return;
  Path::t p = env::normalize_value_path(&loc, env, path);
  Ident::t unit = path::head(p);
  if (ident::global(unit) && !used_primitives.mem(PathKey{p})) used_primitives.add(PathKey{p}, loc);
}

const ArrayKind gen_array_kind = config_flat_float_array ? ArrayKind::Pgenarray : ArrayKind::Paddrarray;

const PrimitiveDescription* prim_sys_argv() {
  static const PrimitiveDescription* d = simple("caml_sys_argv", 1, true);
  return d;
}
const PrimitiveDescription* prim_atomic_exchange() {
  static const PrimitiveDescription* d = simple("caml_atomic_exchange_field", 3, false);
  return d;
}
const PrimitiveDescription* prim_atomic_cas() {
  static const PrimitiveDescription* d = simple("caml_atomic_cas_field", 4, false);
  return d;
}

const std::unordered_map<std::string_view, Prim>& primitives_table() {
  using IP = ImmediateOrPointer;
  using MF = MutableFlag;
  using IA = InitializationOrAssignment;
  using CT = CompileTimeConstant;
  using AK = ArrayKind;
  using BI = BoxedInteger;
  using IC = IntegerComparison;
  using FC = FloatComparison;
  using K = Prim::Kind;
  using C = Comparison;
  using CK = ComparisonKind;
  static const std::unordered_map<std::string_view, Prim> t = {
      {"%identity", k(K::Identity)},
      {"%bytes_to_string", P(pr(PK::Pbytes_to_string), 1)},
      {"%bytes_of_string", P(pr(PK::Pbytes_of_string), 1)},
      {"%ignore", P(pr(PK::Pignore), 1)},
      {"%revapply", k(K::Revapply)},
      {"%apply", k(K::Apply)},
      {"%loc_LOC", lc(LocKind::Loc_LOC)},
      {"%loc_FILE", lc(LocKind::Loc_FILE)},
      {"%loc_LINE", lc(LocKind::Loc_LINE)},
      {"%loc_POS", lc(LocKind::Loc_POS)},
      {"%loc_MODULE", lc(LocKind::Loc_MODULE)},
      {"%loc_FUNCTION", lc(LocKind::Loc_FUNCTION)},
      {"%field0", P(pfield(0, IP::Pointer, MF::Mutable), 1)},
      {"%field1", P(pfield(1, IP::Pointer, MF::Mutable), 1)},
      {"%setfield0", P(psetfield(0, IP::Pointer, IA::Assignment), 2)},
      {"%setfield1", P(psetfield(1, IP::Pointer, IA::Assignment), 2)},
      {"%makeblock", P(pmakeblock(0, MF::Immutable), 1)},
      {"%makemutable", P(pmakeblock(0, MF::Mutable), 1)},
      {"%raise", rz(RaiseKind::Raise_regular)},
      {"%reraise", rz(RaiseKind::Raise_reraise)},
      {"%raise_notrace", rz(RaiseKind::Raise_notrace)},
      {"%raise_with_backtrace", k(K::Raise_with_backtrace)},
      {"%sequand", P(pr(PK::Psequand), 2)},
      {"%sequor", P(pr(PK::Psequor), 2)},
      {"%boolnot", P(pr(PK::Pnot), 1)},
      {"%big_endian", P(pctconst(CT::Big_endian), 1)},
      {"%backend_type", P(pctconst(CT::Backend_type), 1)},
      {"%word_size", P(pctconst(CT::Word_size), 1)},
      {"%int_size", P(pctconst(CT::Int_size), 1)},
      {"%max_wosize", P(pctconst(CT::Max_wosize), 1)},
      {"%ostype_unix", P(pctconst(CT::Ostype_unix), 1)},
      {"%ostype_win32", P(pctconst(CT::Ostype_win32), 1)},
      {"%ostype_cygwin", P(pctconst(CT::Ostype_cygwin), 1)},
      {"%standard_library_default", P(pctconst(CT::Standard_library_default), 1)},
      {"%frame_pointers", k(K::Frame_pointers)},
      {"%negint", P(pr(PK::Pnegint), 1)},
      {"%succint", P(pn(PK::Poffsetint, 1), 1)},
      {"%predint", P(pn(PK::Poffsetint, -1), 1)},
      {"%addint", P(pr(PK::Paddint), 2)},
      {"%subint", P(pr(PK::Psubint), 2)},
      {"%mulint", P(pr(PK::Pmulint), 2)},
      {"%divint", P(psafe(PK::Pdivint, IsSafe::Safe), 2)},
      {"%modint", P(psafe(PK::Pmodint, IsSafe::Safe), 2)},
      {"%andint", P(pr(PK::Pandint), 2)},
      {"%orint", P(pr(PK::Porint), 2)},
      {"%xorint", P(pr(PK::Pxorint), 2)},
      {"%lslint", P(pr(PK::Plslint), 2)},
      {"%lsrint", P(pr(PK::Plsrint), 2)},
      {"%asrint", P(pr(PK::Pasrint), 2)},
      {"%eq", P(ppcmp(PhysicalComparison::CPeq), 2)},
      {"%noteq", P(ppcmp(PhysicalComparison::CPneq), 2)},
      {"%ltint", P(picmp(IC::Clt), 2)},
      {"%leint", P(picmp(IC::Cle), 2)},
      {"%gtint", P(picmp(IC::Cgt), 2)},
      {"%geint", P(picmp(IC::Cge), 2)},
      {"%incr", P(pn(PK::Poffsetref, 1), 1)},
      {"%decr", P(pn(PK::Poffsetref, -1), 1)},
      {"%intoffloat", P(pr(PK::Pintoffloat), 1)},
      {"%floatofint", P(pr(PK::Pfloatofint), 1)},
      {"%negfloat", P(pr(PK::Pnegfloat), 1)},
      {"%absfloat", P(pr(PK::Pabsfloat), 1)},
      {"%addfloat", P(pr(PK::Paddfloat), 2)},
      {"%subfloat", P(pr(PK::Psubfloat), 2)},
      {"%mulfloat", P(pr(PK::Pmulfloat), 2)},
      {"%divfloat", P(pr(PK::Pdivfloat), 2)},
      {"%eqfloat", P(pfcmp(FC::CFeq), 2)},
      {"%noteqfloat", P(pfcmp(FC::CFneq), 2)},
      {"%ltfloat", P(pfcmp(FC::CFlt), 2)},
      {"%lefloat", P(pfcmp(FC::CFle), 2)},
      {"%gtfloat", P(pfcmp(FC::CFgt), 2)},
      {"%gefloat", P(pfcmp(FC::CFge), 2)},
      {"%string_length", P(pr(PK::Pstringlength), 1)},
      {"%string_safe_get", P(pr(PK::Pstringrefs), 2)},
      {"%string_safe_set", P(pr(PK::Pbytessets), 3)},
      {"%string_unsafe_get", P(pr(PK::Pstringrefu), 2)},
      {"%string_unsafe_set", P(pr(PK::Pbytessetu), 3)},
      {"%bytes_length", P(pr(PK::Pbyteslength), 1)},
      {"%bytes_safe_get", P(pr(PK::Pbytesrefs), 2)},
      {"%bytes_safe_set", P(pr(PK::Pbytessets), 3)},
      {"%bytes_unsafe_get", P(pr(PK::Pbytesrefu), 2)},
      {"%bytes_unsafe_set", P(pr(PK::Pbytessetu), 3)},
      {"%array_length", P(parr(PK::Parraylength, gen_array_kind), 1)},
      {"%array_safe_get", P(parr(PK::Parrayrefs, gen_array_kind), 2)},
      {"%array_safe_set", P(parr(PK::Parraysets, gen_array_kind), 3)},
      {"%array_unsafe_get", P(parr(PK::Parrayrefu, gen_array_kind), 2)},
      {"%array_unsafe_set", P(parr(PK::Parraysetu, gen_array_kind), 3)},
      {"%check_array_bound", k(K::Check_array_bound)},
      {"%obj_size", P(parr(PK::Parraylength, gen_array_kind), 1)},
      {"%obj_field", P(parr(PK::Parrayrefu, gen_array_kind), 2)},
      {"%obj_set_field", P(parr(PK::Parraysetu, gen_array_kind), 3)},
      {"%floatarray_length", P(parr(PK::Parraylength, AK::Pfloatarray), 1)},
      {"%floatarray_safe_get", P(parr(PK::Parrayrefs, AK::Pfloatarray), 2)},
      {"%floatarray_safe_set", P(parr(PK::Parraysets, AK::Pfloatarray), 3)},
      {"%floatarray_unsafe_get", P(parr(PK::Parrayrefu, AK::Pfloatarray), 2)},
      {"%floatarray_unsafe_set", P(parr(PK::Parraysetu, AK::Pfloatarray), 3)},
      {"%obj_is_int", P(pr(PK::Pisint), 1)},
      {"%lazy_force", k(K::Lazy_force)},
      {"%nativeint_of_int", P(pbi(PK::Pbintofint, BI::Pnativeint), 1)},
      {"%nativeint_to_int", P(pbi(PK::Pintofbint, BI::Pnativeint), 1)},
      {"%nativeint_neg", P(pbi(PK::Pnegbint, BI::Pnativeint), 1)},
      {"%nativeint_add", P(pbi(PK::Paddbint, BI::Pnativeint), 2)},
      {"%nativeint_sub", P(pbi(PK::Psubbint, BI::Pnativeint), 2)},
      {"%nativeint_mul", P(pbi(PK::Pmulbint, BI::Pnativeint), 2)},
      {"%nativeint_div", P(pbisafe(PK::Pdivbint, BI::Pnativeint), 2)},
      {"%nativeint_mod", P(pbisafe(PK::Pmodbint, BI::Pnativeint), 2)},
      {"%nativeint_and", P(pbi(PK::Pandbint, BI::Pnativeint), 2)},
      {"%nativeint_or", P(pbi(PK::Porbint, BI::Pnativeint), 2)},
      {"%nativeint_xor", P(pbi(PK::Pxorbint, BI::Pnativeint), 2)},
      {"%nativeint_lsl", P(pbi(PK::Plslbint, BI::Pnativeint), 2)},
      {"%nativeint_lsr", P(pbi(PK::Plsrbint, BI::Pnativeint), 2)},
      {"%nativeint_asr", P(pbi(PK::Pasrbint, BI::Pnativeint), 2)},
      {"%int32_of_int", P(pbi(PK::Pbintofint, BI::Pint32), 1)},
      {"%int32_to_int", P(pbi(PK::Pintofbint, BI::Pint32), 1)},
      {"%int32_neg", P(pbi(PK::Pnegbint, BI::Pint32), 1)},
      {"%int32_add", P(pbi(PK::Paddbint, BI::Pint32), 2)},
      {"%int32_sub", P(pbi(PK::Psubbint, BI::Pint32), 2)},
      {"%int32_mul", P(pbi(PK::Pmulbint, BI::Pint32), 2)},
      {"%int32_div", P(pbisafe(PK::Pdivbint, BI::Pint32), 2)},
      {"%int32_mod", P(pbisafe(PK::Pmodbint, BI::Pint32), 2)},
      {"%int32_and", P(pbi(PK::Pandbint, BI::Pint32), 2)},
      {"%int32_or", P(pbi(PK::Porbint, BI::Pint32), 2)},
      {"%int32_xor", P(pbi(PK::Pxorbint, BI::Pint32), 2)},
      {"%int32_lsl", P(pbi(PK::Plslbint, BI::Pint32), 2)},
      {"%int32_lsr", P(pbi(PK::Plsrbint, BI::Pint32), 2)},
      {"%int32_asr", P(pbi(PK::Pasrbint, BI::Pint32), 2)},
      {"%int64_of_int", P(pbi(PK::Pbintofint, BI::Pint64), 1)},
      {"%int64_to_int", P(pbi(PK::Pintofbint, BI::Pint64), 1)},
      {"%int64_neg", P(pbi(PK::Pnegbint, BI::Pint64), 1)},
      {"%int64_add", P(pbi(PK::Paddbint, BI::Pint64), 2)},
      {"%int64_sub", P(pbi(PK::Psubbint, BI::Pint64), 2)},
      {"%int64_mul", P(pbi(PK::Pmulbint, BI::Pint64), 2)},
      {"%int64_div", P(pbisafe(PK::Pdivbint, BI::Pint64), 2)},
      {"%int64_mod", P(pbisafe(PK::Pmodbint, BI::Pint64), 2)},
      {"%int64_and", P(pbi(PK::Pandbint, BI::Pint64), 2)},
      {"%int64_or", P(pbi(PK::Porbint, BI::Pint64), 2)},
      {"%int64_xor", P(pbi(PK::Pxorbint, BI::Pint64), 2)},
      {"%int64_lsl", P(pbi(PK::Plslbint, BI::Pint64), 2)},
      {"%int64_lsr", P(pbi(PK::Plsrbint, BI::Pint64), 2)},
      {"%int64_asr", P(pbi(PK::Pasrbint, BI::Pint64), 2)},
      {"%nativeint_of_int32", P(pcvt(BI::Pint32, BI::Pnativeint), 1)},
      {"%nativeint_to_int32", P(pcvt(BI::Pnativeint, BI::Pint32), 1)},
      {"%int64_of_int32", P(pcvt(BI::Pint32, BI::Pint64), 1)},
      {"%int64_to_int32", P(pcvt(BI::Pint64, BI::Pint32), 1)},
      {"%int64_of_nativeint", P(pcvt(BI::Pnativeint, BI::Pint64), 1)},
      {"%int64_to_nativeint", P(pcvt(BI::Pint64, BI::Pnativeint), 1)},
      {"%caml_ba_ref_1", P(pba(PK::Pbigarrayref, false, 1), 2)},
      {"%caml_ba_ref_2", P(pba(PK::Pbigarrayref, false, 2), 3)},
      {"%caml_ba_ref_3", P(pba(PK::Pbigarrayref, false, 3), 4)},
      {"%caml_ba_set_1", P(pba(PK::Pbigarrayset, false, 1), 3)},
      {"%caml_ba_set_2", P(pba(PK::Pbigarrayset, false, 2), 4)},
      {"%caml_ba_set_3", P(pba(PK::Pbigarrayset, false, 3), 5)},
      {"%caml_ba_unsafe_ref_1", P(pba(PK::Pbigarrayref, true, 1), 2)},
      {"%caml_ba_unsafe_ref_2", P(pba(PK::Pbigarrayref, true, 2), 3)},
      {"%caml_ba_unsafe_ref_3", P(pba(PK::Pbigarrayref, true, 3), 4)},
      {"%caml_ba_unsafe_set_1", P(pba(PK::Pbigarrayset, true, 1), 3)},
      {"%caml_ba_unsafe_set_2", P(pba(PK::Pbigarrayset, true, 2), 4)},
      {"%caml_ba_unsafe_set_3", P(pba(PK::Pbigarrayset, true, 3), 5)},
      {"%caml_ba_dim_1", P(pn(PK::Pbigarraydim, 1), 1)},
      {"%caml_ba_dim_2", P(pn(PK::Pbigarraydim, 2), 1)},
      {"%caml_ba_dim_3", P(pn(PK::Pbigarraydim, 3), 1)},
      {"%caml_string_get16", P(pu(PK::Pstring_load_16, false), 2)},
      {"%caml_string_get16u", P(pu(PK::Pstring_load_16, true), 2)},
      {"%caml_string_get32", P(pu(PK::Pstring_load_32, false), 2)},
      {"%caml_string_get32u", P(pu(PK::Pstring_load_32, true), 2)},
      {"%caml_string_get64", P(pu(PK::Pstring_load_64, false), 2)},
      {"%caml_string_get64u", P(pu(PK::Pstring_load_64, true), 2)},
      {"%caml_string_set16", P(pu(PK::Pbytes_set_16, false), 3)},
      {"%caml_string_set16u", P(pu(PK::Pbytes_set_16, true), 3)},
      {"%caml_string_set32", P(pu(PK::Pbytes_set_32, false), 3)},
      {"%caml_string_set32u", P(pu(PK::Pbytes_set_32, true), 3)},
      {"%caml_string_set64", P(pu(PK::Pbytes_set_64, false), 3)},
      {"%caml_string_set64u", P(pu(PK::Pbytes_set_64, true), 3)},
      {"%caml_bytes_get16", P(pu(PK::Pbytes_load_16, false), 2)},
      {"%caml_bytes_get16u", P(pu(PK::Pbytes_load_16, true), 2)},
      {"%caml_bytes_get32", P(pu(PK::Pbytes_load_32, false), 2)},
      {"%caml_bytes_get32u", P(pu(PK::Pbytes_load_32, true), 2)},
      {"%caml_bytes_get64", P(pu(PK::Pbytes_load_64, false), 2)},
      {"%caml_bytes_get64u", P(pu(PK::Pbytes_load_64, true), 2)},
      {"%caml_bytes_set16", P(pu(PK::Pbytes_set_16, false), 3)},
      {"%caml_bytes_set16u", P(pu(PK::Pbytes_set_16, true), 3)},
      {"%caml_bytes_set32", P(pu(PK::Pbytes_set_32, false), 3)},
      {"%caml_bytes_set32u", P(pu(PK::Pbytes_set_32, true), 3)},
      {"%caml_bytes_set64", P(pu(PK::Pbytes_set_64, false), 3)},
      {"%caml_bytes_set64u", P(pu(PK::Pbytes_set_64, true), 3)},
      {"%caml_bigstring_get16", P(pu(PK::Pbigstring_load_16, false), 2)},
      {"%caml_bigstring_get16u", P(pu(PK::Pbigstring_load_16, true), 2)},
      {"%caml_bigstring_get32", P(pu(PK::Pbigstring_load_32, false), 2)},
      {"%caml_bigstring_get32u", P(pu(PK::Pbigstring_load_32, true), 2)},
      {"%caml_bigstring_get64", P(pu(PK::Pbigstring_load_64, false), 2)},
      {"%caml_bigstring_get64u", P(pu(PK::Pbigstring_load_64, true), 2)},
      {"%caml_bigstring_set16", P(pu(PK::Pbigstring_set_16, false), 3)},
      {"%caml_bigstring_set16u", P(pu(PK::Pbigstring_set_16, true), 3)},
      {"%caml_bigstring_set32", P(pu(PK::Pbigstring_set_32, false), 3)},
      {"%caml_bigstring_set32u", P(pu(PK::Pbigstring_set_32, true), 3)},
      {"%caml_bigstring_set64", P(pu(PK::Pbigstring_set_64, false), 3)},
      {"%caml_bigstring_set64u", P(pu(PK::Pbigstring_set_64, true), 3)},
      {"%bswap16", P(pr(PK::Pbswap16), 1)},
      {"%bswap_int32", P(pbi(PK::Pbbswap, BI::Pint32), 1)},
      {"%bswap_int64", P(pbi(PK::Pbbswap, BI::Pint64), 1)},
      {"%bswap_native", P(pbi(PK::Pbbswap, BI::Pnativeint), 1)},
      {"%int_as_pointer", P(pr(PK::Pint_as_pointer), 1)},
      {"%opaque", P(pr(PK::Popaque), 1)},
      {"%sys_argv", k(K::Sys_argv)},
      {"%send", k(K::Send)},
      {"%sendself", k(K::Send_self)},
      {"%sendcache", k(K::Send_cache)},
      {"%equal", cmp(C::Equal, CK::Compare_generic)},
      {"%notequal", cmp(C::Not_equal, CK::Compare_generic)},
      {"%lessequal", cmp(C::Less_equal, CK::Compare_generic)},
      {"%lessthan", cmp(C::Less_than, CK::Compare_generic)},
      {"%greaterequal", cmp(C::Greater_equal, CK::Compare_generic)},
      {"%greaterthan", cmp(C::Greater_than, CK::Compare_generic)},
      {"%compare", cmp(C::Compare, CK::Compare_generic)},
      {"%atomic_load", at(AtomicOp::Load, AtomicKind::Ref)},
      {"%atomic_exchange", at(AtomicOp::Exchange, AtomicKind::Ref)},
      {"%atomic_cas", at(AtomicOp::Cas, AtomicKind::Ref)},
      {"%atomic_fetch_add", at(AtomicOp::Faa, AtomicKind::Ref)},
      {"%atomic_load_field", at(AtomicOp::Load, AtomicKind::Field)},
      {"%atomic_exchange_field", at(AtomicOp::Exchange, AtomicKind::Field)},
      {"%atomic_cas_field", at(AtomicOp::Cas, AtomicKind::Field)},
      {"%atomic_fetch_add_field", at(AtomicOp::Faa, AtomicKind::Field)},
      {"%atomic_load_loc", at(AtomicOp::Load, AtomicKind::Loc)},
      {"%atomic_exchange_loc", at(AtomicOp::Exchange, AtomicKind::Loc)},
      {"%atomic_cas_loc", at(AtomicOp::Cas, AtomicKind::Loc)},
      {"%atomic_fetch_add_loc", at(AtomicOp::Faa, AtomicKind::Loc)},
      {"%atomic_unsafe_index", k(K::Atomic_index)},
      {"%runstack", P(pr(PK::Prunstack), 3)},
      {"%reperform", P(pr(PK::Preperform), 2)},
      {"%perform", P(pr(PK::Pperform), 1)},
      {"%resume", P(pr(PK::Presume), 3)},
      {"%dls_get", P(pr(PK::Pdls_get), 1)},
      {"%poll", P(pr(PK::Ppoll), 1)},
      {"%todo", k(K::Todo)},
  };
  return t;
}

Prim lookup_primitive(const Location& loc, const PrimitiveDescription* p) {
  auto& t = primitives_table();
  auto it = t.find(p->prim_name);
  if (it != t.end()) return it->second;
  if (!p->prim_name.empty() && p->prim_name[0] == '%')
    throw Error(loc, Error::Kind::Unknown_builtin_primitive, std::string(p->prim_name));
  Prim r{Prim::Kind::External};
  r.ext = p;
  return r;
}

Prim lookup_primitive_and_mark_used(const Location& loc, const PrimitiveDescription* p, env::t env,
                                    Path::t path) {
  Prim r = lookup_primitive(loc, p);
  if (r.kind == Prim::Kind::External) add_used_primitive(loc, env, path);
  return r;
}

bool simplify_constant_constructor(Comparison c) { return c == Comparison::Equal || c == Comparison::Not_equal; }

// the greatest lower bound in the semilattice of array kinds
ArrayKind glb_array_type(ArrayKind t1, ArrayKind t2) {
  using AK = ArrayKind;
  if (t1 == AK::Pfloatarray && (t2 == AK::Paddrarray || t2 == AK::Pintarray)) return t1;
  if ((t1 == AK::Paddrarray || t1 == AK::Pintarray) && t2 == AK::Pfloatarray) return t1;
  if (t1 == AK::Pgenarray) return t2;
  if (t2 == AK::Pgenarray) return t1;
  if (t1 == AK::Paddrarray) return t2;
  if (t2 == AK::Paddrarray) return t1;
  return t1;  // Pintarray, Pintarray | Pfloatarray, Pfloatarray
}

bool is_array_prim(PK kd) {
  return kd == PK::Parraylength || kd == PK::Parrayrefu || kd == PK::Parraysetu || kd == PK::Parrayrefs ||
         kd == PK::Parraysets;
}

// Specialize a primitive from available type information.
std::optional<Prim> specialize_primitive(env::t env, TypeExpr* ty, bool has_constant_constructor,
                                         const Prim& prim) {
  using IP = ImmediateOrPointer;
  std::vector<TypeExpr*> param_tys;
  {
    TypeExpr *p1, *rhs;
    if (typeopt::is_function_type(env, ty, &p1, &rhs)) {
      TypeExpr *p2, *rhs2;
      if (typeopt::is_function_type(env, rhs, &p2, &rhs2))
        param_tys = {p1, p2};
      else
        param_tys = {p1};
    }
  }
  if (prim.kind == Prim::Kind::Primitive) {
    const Primitive& p = prim.p;
    if (p.kind == PK::Psetfield && p.ptr == IP::Pointer && param_tys.size() == 2) {
      if (typeopt::maybe_pointer_type(env, param_tys[1]) == IP::Pointer) return std::nullopt;
      Prim r = prim;
      r.p.ptr = IP::Immediate;
      return r;
    }
    if (p.kind == PK::Pfield && p.ptr == IP::Pointer) {
      // try strength reduction based on the *result type*
      IP is_int = IP::Pointer;
      TypeExpr *p1, *rhs;
      if (typeopt::is_function_type(env, ty, &p1, &rhs)) is_int = typeopt::maybe_pointer_type(env, rhs);
      Prim r = prim;
      r.p.ptr = is_int;
      return r;
    }
    if (p.kind == PK::Parraylength && param_tys.size() == 1) {
      ArrayKind array_type = glb_array_type(p.array, typeopt::array_type_kind(env, param_tys[0]));
      if (p.array == array_type) return std::nullopt;
      Prim r = prim;
      r.p.array = array_type;
      return r;
    }
    if (is_array_prim(p.kind) && p.kind != PK::Parraylength && !param_tys.empty()) {
      ArrayKind array_type = glb_array_type(p.array, typeopt::array_type_kind(env, param_tys[0]));
      if (p.array == array_type) return std::nullopt;
      Prim r = prim;
      r.p.array = array_type;
      return r;
    }
    if ((p.kind == PK::Pbigarrayref || p.kind == PK::Pbigarrayset) &&
        p.ba_kind == BigarrayKind::Pbigarray_unknown &&
        p.ba_layout == BigarrayLayout::Pbigarray_unknown_layout && !param_tys.empty()) {
      typeopt::BigarrayKindLayout kl = typeopt::bigarray_type_kind_and_layout(env, param_tys[0]);
      if (kl.kind == BigarrayKind::Pbigarray_unknown && kl.layout == BigarrayLayout::Pbigarray_unknown_layout)
        return std::nullopt;
      Prim r = prim;
      r.p.ba_kind = kl.kind;
      r.p.ba_layout = kl.layout;
      return r;
    }
    if (p.kind == PK::Pmakeblock && !p.shape.some) {
      std::vector<ValueKind> shape;
      for (TypeExpr* f : param_tys) shape.push_back(typeopt::value_kind(env, f));
      bool useful = false;
      for (const ValueKind& knd : shape)
        if (!equal_value_kind(knd, ValueKind::gen())) useful = true;
      if (!useful) return std::nullopt;
      Prim r = prim;
      r.p.shape = BlockShape{true, slice(shape)};
      return r;
    }
    return std::nullopt;
  }
  if (prim.kind == Prim::Kind::Comparison && prim.ck == ComparisonKind::Compare_generic && !param_tys.empty()) {
    TypeExpr* p1 = param_tys[0];
    const predef::Paths& pp = predef::paths();
    auto with = [&](ComparisonKind ck) {
      Prim r = prim;
      r.ck = ck;
      return std::optional<Prim>(r);
    };
    if (has_constant_constructor && simplify_constant_constructor(prim.comp))
      return with(ComparisonKind::Compare_ints);
    if (typeopt::is_base_type(env, p1, pp.int_) || typeopt::is_base_type(env, p1, pp.char_) ||
        typeopt::maybe_pointer_type(env, p1) == IP::Immediate)
      return with(ComparisonKind::Compare_ints);
    if (typeopt::is_base_type(env, p1, pp.float_)) return with(ComparisonKind::Compare_floats);
    if (typeopt::is_base_type(env, p1, pp.string)) return with(ComparisonKind::Compare_strings);
    if (typeopt::is_base_type(env, p1, pp.bytes)) return with(ComparisonKind::Compare_bytes);
    if (typeopt::is_base_type(env, p1, pp.nativeint)) return with(ComparisonKind::Compare_nativeints);
    if (typeopt::is_base_type(env, p1, pp.int32)) return with(ComparisonKind::Compare_int32s);
    if (typeopt::is_base_type(env, p1, pp.int64)) return with(ComparisonKind::Compare_int64s);
    return std::nullopt;
  }
  return std::nullopt;
}

// the C comparison primitives: [kind][comparison]
const PrimitiveDescription* caml_cmp(ComparisonKind ck, Comparison c) {
  static const char* names[7] = {"equal", "notequal", "lessequal", "lessthan", "greaterequal", "greaterthan",
                                 "compare"};
  static std::unordered_map<std::string, const PrimitiveDescription*> cache;
  std::string prefix = ck == ComparisonKind::Compare_generic   ? "caml_"
                       : ck == ComparisonKind::Compare_strings ? "caml_string_"
                                                               : "caml_bytes_";
  std::string name = prefix + names[static_cast<int>(c)];
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  const PrimitiveDescription* d = simple(name, 2, ck == ComparisonKind::Compare_generic);
  cache.emplace(name, d);
  return d;
}

Primitive comparison_primitive(Comparison comparison, ComparisonKind kind) {
  using CK = ComparisonKind;
  using IC = IntegerComparison;
  using FC = FloatComparison;
  using BI = BoxedInteger;
  static const IC icmps[6] = {IC::Ceq, IC::Cne, IC::Cle, IC::Clt, IC::Cge, IC::Cgt};
  static const FC fcmps[6] = {FC::CFeq, FC::CFneq, FC::CFle, FC::CFlt, FC::CFge, FC::CFgt};
  int c = static_cast<int>(comparison);
  bool is_compare = comparison == Comparison::Compare;
  auto bint = [&](BI bi) { return is_compare ? pbi(PK::Pcompare_bints, bi) : pbicmp(bi, icmps[c]); };
  switch (kind) {
    case CK::Compare_generic:
    case CK::Compare_strings:
    case CK::Compare_bytes: return pccall(caml_cmp(kind, comparison));
    case CK::Compare_ints: return is_compare ? prim(PK::Pcompare_ints) : picmp(icmps[c]);
    case CK::Compare_floats: return is_compare ? prim(PK::Pcompare_floats) : pfcmp(fcmps[c]);
    case CK::Compare_nativeints: return bint(BI::Pnativeint);
    case CK::Compare_int32s: return bint(BI::Pint32);
    case CK::Compare_int64s: return bint(BI::Pint64);
  }
  throw std::logic_error("comparison_primitive");
}

// Filename.basename (Unix)
std::string filename_basename(std::string_view name) {
  if (name.empty()) return ".";
  long n = static_cast<long>(name.size()) - 1;
  while (n >= 0 && name[n] == '/') n--;
  if (n < 0) return std::string(name.substr(0, 1));
  long p = n + 1;
  while (n >= 0 && name[n] != '/') n--;
  return std::string(name.substr(n + 1, p - n - 1));
}

// String.escaped
std::string string_escaped(std::string_view s) {
  std::string r;
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\b': r += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') {
          r += static_cast<char>(c);
        } else {
          char buf[5];
          std::snprintf(buf, sizeof buf, "\\%03u", c);
          r += buf;
        }
    }
  }
  return r;
}

const StructuredConstant* const_immstring(std::string_view s) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_immstring;
  c->s = zborrow(s);  // Const_immstring s: the string itself
  return c;
}
const StructuredConstant* const_block(long tag, std::vector<const StructuredConstant*> fields) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_block;
  c->i = tag;
  c->fields = slice(fields);
  return c;
}

lam_t lambda_of_loc(LocKind kind, const ScopedLocation& sloc) {
  Location loc = to_location(sloc);
  const Position& loc_start = loc.loc_start;
  // Location.get_pos_info
  std::string_view file = loc_start.pos_fname;
  long lnum = loc_start.pos_lnum;
  long cnum = loc_start.pos_cnum - loc_start.pos_bol;
  // an absolute file: Location.rewrite_absolute_path (BUILD_PATH_PREFIX_MAP;
  // the string itself when no prefix matches)
  if (!file.empty() && file[0] == '/') {
    std::string r = location::rewrite_absolute_path(std::string(file));
    if (r != file) file = zstr(r);
  }
  long enum_ = loc.loc_end.pos_cnum - loc_start.pos_cnum + cnum;
  switch (kind) {
    case LocKind::Loc_POS:
      return lconst(const_block(0, {const_immstring(file), const_int(lnum), const_int(cnum), const_int(enum_)}));
    case LocKind::Loc_FILE: return lconst(const_immstring(file));
    case LocKind::Loc_MODULE: {
      std::string filename = filename_basename(std::string(file));
      std::string name = env::get_current_unit_name();
      // Env.get_current_unit_name () is Unit_info's modname string itself,
      // which the .cmo's cu_name shares
      if (!name.empty()) return lconst(const_immstring(uid::unit_name_string(name)));
      return lconst(const_immstring("//" + filename + "//"));
    }
    case LocKind::Loc_LOC: {
      std::string s = "File \"" + string_escaped(file) + "\", line " + std::to_string(lnum) + ", characters " +
                      std::to_string(cnum) + "-" + std::to_string(enum_);
      return lconst(const_immstring(s));
    }
    case LocKind::Loc_LINE: return lconst(const_int(lnum));
    case LocKind::Loc_FUNCTION: return lconst(const_immstring(debuginfo::string_of_scoped_location(sloc)));
  }
  throw std::logic_error("lambda_of_loc");
}

long atomic_arity(AtomicOp op, AtomicKind kind) {
  long arity_of_op = op == AtomicOp::Load ? 1 : op == AtomicOp::Cas ? 3 : 2;
  long extra_kind_arity = kind == AtomicKind::Field ? 1 : 0;
  return arity_of_op + extra_kind_arity;
}

lam_t lambda_of_atomic(std::string_view prim_name, const ScopedLocation& loc, AtomicOp op, AtomicKind kind,
                       Slice<lam_t> args) {
  if (static_cast<long>(args.size()) != atomic_arity(op, kind))
    throw Error(to_location(loc), Error::Kind::Wrong_arity_builtin_primitive, std::string(prim_name));
  Primitive prim = op == AtomicOp::Load       ? pr(PK::Patomic_load)
                   : op == AtomicOp::Exchange ? pccall(prim_atomic_exchange())
                   : op == AtomicOp::Cas      ? pccall(prim_atomic_cas())
                                              : pr(PK::Patomic_fetch_add);
  std::vector<lam_t> rest(args.begin() + 1, args.end());
  switch (kind) {
    case AtomicKind::Ref: {
      std::vector<lam_t> a{args[0], lconst(const_int(0))};
      a.insert(a.end(), rest.begin(), rest.end());
      return lprim(prim, slice(a), loc);
    }
    case AtomicKind::Field: return lprim(prim, args, loc);
    case AtomicKind::Loc: {
      lam_t loc_arg = args[0];
      if (auto* lp = as<Lprim>(loc_arg); lp && lp->p.kind == PK::Pmakeblock && lp->args.size() == 2) {
        std::vector<lam_t> a{lp->args[0], lp->args[1]};
        a.insert(a.end(), rest.begin(), rest.end());
        return lprim(prim, slice(a), loc);
      }
      Ident::t varg = Ident::create_local(OCAML_LIT("atomic_arg"));
      lam_t ptr = lprim(pfield(0, ImmediateOrPointer::Pointer, MutableFlag::Immutable), slice({lvar(varg)}), loc);
      lam_t ofs = lprim(pfield(1, ImmediateOrPointer::Immediate, MutableFlag::Immutable), slice({lvar(varg)}), loc);
      std::vector<lam_t> a{ptr, ofs};
      a.insert(a.end(), rest.begin(), rest.end());
      return llet(LetKind::Strict, ValueKind::gen(), varg, loc_arg, lprim(prim, slice(a), loc));
    }
  }
  throw std::logic_error("lambda_of_atomic");
}

lam_t check_array_bound(const ScopedLocation& loc, lam_t array, lam_t idx) {
  lam_t len = lprim(parr(PK::Parraylength, ArrayKind::Pgenarray), slice({array}), loc);
  return lprim(pr(PK::Pcheckbound), slice({len, idx}), loc);
}

const PrimitiveDescription* caml_restore_raw_backtrace() {
  static const PrimitiveDescription* d = simple("caml_restore_raw_backtrace", 2, false);
  return d;
}

IdentSet try_ids;

// arg_exps: nullptr = None
lam_t raise_todo(const ScopedLocation& loc, lam_t arg, const Slice<const tt::Expression*>* arg_exps) {
  lam_t todo_exn_id = transl_extension_path(debuginfo::loc_unknown(), env::initial(), predef::paths().todo);
  Location l = to_location(loc);
  std::string fname(l.loc_start.pos_fname);
  long line = l.loc_start.pos_lnum;
  if (arg_exps) {
    if (arg_exps->size() != 1) throw std::logic_error("Translprim.raise_todo");
    arg = event_after(loc, (*arg_exps)[0], arg);
  }
  return lsequence(
      arg, lprim(praise(RaiseKind::Raise_regular),
                 slice({lprim(pmakeblock(0, MutableFlag::Immutable),
                              slice({todo_exn_id, lconst(const_block(0, {const_immstring(fname), const_int(line)}))}),
                              loc)}),
                 loc));
}

lam_t lambda_of_prim(std::string_view prim_name, const Prim& prim, const ScopedLocation& loc, Slice<lam_t> args,
                     const Slice<const tt::Expression*>* arg_exps) {
  using K = Prim::Kind;
  std::size_t n = args.size();
  auto wrong_arity = [&]() -> lam_t {
    throw Error(to_location(loc), Error::Kind::Wrong_arity_builtin_primitive, std::string(prim_name));
  };
  switch (prim.kind) {
    case K::Primitive:
      if (prim.arity == static_cast<long>(n)) return lprim(prim.p, args, loc);
      return wrong_arity();
    case K::Sys_argv:
      if (n == 0) return lprim(pccall(prim_sys_argv()), slice({lconst(const_int(0))}), loc);
      return wrong_arity();
    case K::External: return lprim(pccall(prim.ext), args, loc);
    case K::Comparison:
      if (n == 2) return lprim(comparison_primitive(prim.comp, prim.ck), args, loc);
      return wrong_arity();
    case K::Raise: {
      if (n != 1) return wrong_arity();
      lam_t arg = args[0];
      RaiseKind kind = prim.raise;
      if (kind == RaiseKind::Raise_regular)
        if (auto* v = as<Lvar>(arg); v && try_ids.count(v->id)) kind = RaiseKind::Raise_reraise;
      if (arg_exps) {
        if (arg_exps->size() != 1) throw std::logic_error("Translprim.lambda_of_prim");
        arg = event_after(loc, (*arg_exps)[0], arg);
      }
      return lprim(praise(kind), slice({arg}), loc);
    }
    case K::Raise_with_backtrace: {
      if (n != 2) return wrong_arity();
      Ident::t vexn = Ident::create_local(OCAML_LIT("exn"));
      lam_t raise_arg = lvar(vexn);
      if (arg_exps) {
        if (arg_exps->size() != 2) throw std::logic_error("Translprim.lambda_of_prim");
        raise_arg = event_after(loc, (*arg_exps)[0], lvar(vexn));
      }
      return llet(LetKind::Strict, ValueKind::gen(), vexn, args[0],
                  lsequence(lprim(pccall(caml_restore_raw_backtrace()), slice({lvar(vexn), args[1]}), loc),
                            lprim(praise(RaiseKind::Raise_reraise), slice({raise_arg}), loc)));
    }
    case K::Todo:
      if (n == 1) return raise_todo(loc, args[0], arg_exps);
      return wrong_arity();
    case K::Lazy_force:
      if (n == 1) return matching::inline_lazy_force(args[0], loc);
      return wrong_arity();
    case K::Loc:
      if (n == 0) return lambda_of_loc(prim.loc, loc);
      if (n == 1) {
        lam_t lam = lambda_of_loc(prim.loc, loc);
        return lprim(pmakeblock(0, MutableFlag::Immutable), slice({lam, args[0]}), loc);
      }
      return wrong_arity();
    case K::Send:
      if (n == 2) return lsend(MethKind::Public, args[1], args[0], {}, loc);
      return wrong_arity();
    case K::Send_self:
      if (n == 2) return lsend(MethKind::Self, args[1], args[0], {}, loc);
      return wrong_arity();
    case K::Send_cache:
      if (n != 4) return wrong_arity();
      // Cached mode only works in the native backend
      if (clflags::native_code) return lsend(MethKind::Cached, args[1], args[0], slice({args[2], args[3]}), loc);
      return lsend(MethKind::Public, args[1], args[0], {}, loc);
    case K::Frame_pointers:
      if (n == 0) return lconst(const_int(clflags::native_code && config_with_frame_pointers ? 1 : 0));
      return wrong_arity();
    case K::Identity:
      if (n == 1) return args[0];
      return wrong_arity();
    case K::Apply:
    case K::Revapply: {
      if (n != 2) return wrong_arity();
      lam_t func = prim.kind == K::Apply ? args[0] : args[1];
      lam_t arg = prim.kind == K::Apply ? args[1] : args[0];
      LambdaApply ap;
      ap.ap_func = func;
      ap.ap_args = slice({arg});
      ap.ap_loc = loc;
      return lapply(ap);
    }
    case K::Atomic: return lambda_of_atomic(prim_name, loc, prim.op, prim.ak, args);
    case K::Atomic_index:
      if (n == 2) return make_atomic_loc(loc, args[0], args[1]);
      return wrong_arity();
    case K::Check_array_bound:
      if (n == 2) return check_array_bound(loc, args[0], args[1]);
      return wrong_arity();
  }
  throw std::logic_error("lambda_of_prim");
}

bool lambda_primitive_needs_event_after(const Primitive& p) {
  switch (p.kind) {
    case PK::Pduprecord: case PK::Pccall: case PK::Pfloatofint: case PK::Pnegfloat: case PK::Pabsfloat:
    case PK::Paddfloat: case PK::Psubfloat: case PK::Pmulfloat: case PK::Pdivfloat: case PK::Pstringrefs:
    case PK::Pbytesrefs: case PK::Pbytessets: case PK::Pduparray: case PK::Parrayrefs: case PK::Parraysets:
    case PK::Pbintofint: case PK::Pcvtbint: case PK::Pnegbint: case PK::Paddbint: case PK::Psubbint:
    case PK::Pmulbint: case PK::Pdivbint: case PK::Pmodbint: case PK::Pandbint: case PK::Porbint:
    case PK::Pxorbint: case PK::Plslbint: case PK::Plsrbint: case PK::Pasrbint: case PK::Pbintcomp:
    case PK::Pcompare_bints: case PK::Pbigarrayref: case PK::Pbigarrayset: case PK::Pbigarraydim:
    case PK::Pstring_load_16: case PK::Pstring_load_32: case PK::Pstring_load_64: case PK::Pbytes_load_16:
    case PK::Pbytes_load_32: case PK::Pbytes_load_64: case PK::Pbytes_set_16: case PK::Pbytes_set_32:
    case PK::Pbytes_set_64: case PK::Pbigstring_load_16: case PK::Pbigstring_load_32:
    case PK::Pbigstring_load_64: case PK::Pbigstring_set_16: case PK::Pbigstring_set_32:
    case PK::Pbigstring_set_64: case PK::Prunstack: case PK::Pperform: case PK::Preperform: case PK::Presume:
    case PK::Pbbswap: case PK::Ppoll:
      return true;
    case PK::Pmakearray: return p.array == ArrayKind::Pgenarray;
    case PK::Parrayrefu:
    case PK::Parraysetu: return p.array == ArrayKind::Pgenarray || p.array == ArrayKind::Pfloatarray;
    default: return false;
  }
}

// Determine if a primitive should be surrounded by an "after" debug event
bool primitive_needs_event_after(const Prim& prim) {
  using K = Prim::Kind;
  switch (prim.kind) {
    case K::Primitive: return lambda_primitive_needs_event_after(prim.p);
    case K::Comparison: return lambda_primitive_needs_event_after(comparison_primitive(prim.comp, prim.ck));
    case K::External: case K::Sys_argv: return true;
    case K::Lazy_force: case K::Send: case K::Send_self: case K::Send_cache: case K::Apply: case K::Revapply:
      return true;
    default: return false;
  }
}

}  // namespace

void add_exception_ident(Ident::t id) { try_ids.insert(id); }
void remove_exception_ident(Ident::t id) { try_ids.erase(id); }

void clear_used_primitives() { used_primitives.clear(); }
// Hashtbl.fold (fun path _ acc -> path :: acc) used_primitives []: the
// buckets from index 0, each in order, consed -- the reverse of that walk
std::vector<Path::t> get_used_primitives() {
  std::vector<Path::t> r;
  for (auto& [k, loc] : used_primitives.to_seq()) r.push_back(k.p);
  std::reverse(r.begin(), r.end());
  return r;
}

void check_primitive_arity(const Location& loc, const PrimitiveDescription* p) {
  using K = Prim::Kind;
  Prim prim = lookup_primitive(loc, p);
  long a = p->prim_arity;
  bool ok = false;
  switch (prim.kind) {
    case K::Primitive: ok = prim.arity == a; break;
    case K::External: ok = true; break;
    case K::Sys_argv: ok = a == 0; break;
    case K::Comparison: ok = a == 2; break;
    case K::Raise: ok = a == 1; break;
    case K::Raise_with_backtrace: ok = a == 2; break;
    case K::Todo: ok = a == 1; break;
    case K::Lazy_force: ok = a == 1; break;
    case K::Loc: ok = a == 1 || a == 0; break;
    case K::Send: case K::Send_self: ok = a == 2; break;
    case K::Send_cache: ok = a == 4; break;
    case K::Frame_pointers: ok = a == 0; break;
    case K::Identity: ok = a == 1; break;
    case K::Apply: case K::Revapply: ok = a == 2; break;
    case K::Atomic: ok = a == atomic_arity(prim.op, prim.ak); break;
    case K::Atomic_index: ok = a == 2; break;
    case K::Check_array_bound: ok = a == 2; break;
  }
  if (!ok) throw Error(loc, Error::Kind::Wrong_arity_builtin_primitive, std::string(p->prim_name));
}

// Eta-expand a primitive
lam_t transl_primitive(const ScopedLocation& loc, const PrimitiveDescription* p, env::t env, TypeExpr* ty,
                       Path::t path) {
  Prim prim = lookup_primitive_and_mark_used(to_location(loc), p, env, path);
  bool has_constant_constructor = false;
  if (std::optional<Prim> s = specialize_primitive(env, ty, has_constant_constructor, prim)) prim = *s;
  // make_params n = (Ident.create_local "prim", Pgenval) :: make_params (n-1):
  // the tail first, so the last parameter is created first
  long n = p->prim_arity > 0 ? p->prim_arity : 0;
  std::vector<Param> params(static_cast<std::size_t>(n));
  for (long i = n; i-- > 0;) params[i] = Param{Ident::create_local(OCAML_LIT("prim")), ValueKind::gen()};
  std::vector<lam_t> args;
  for (const Param& pa : params) args.push_back(lvar(pa.id));
  lam_t body = lambda_of_prim(p->prim_name, prim, loc, slice(args), nullptr);
  if (params.empty()) return body;
  return lfunction(FunctionKind::Curried, slice(params), ValueKind::gen(), body, default_stub_attribute(), loc);
}

lam_t transl_primitive_application(const ScopedLocation& loc, const PrimitiveDescription* p, env::t env,
                                   TypeExpr* ty, Path::t path, const tt::Expression* exp, Slice<lam_t> args,
                                   Slice<const tt::Expression*> arg_exps) {
  Prim prim = lookup_primitive_and_mark_used(to_location(loc), p, env, path);
  auto constant_like = [](const tt::Expression* e) {
    if (auto* c = tt::as<tt::Texp_construct>(e->exp_desc))
      return c->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_constant;
    if (auto* v = tt::as<tt::Texp_variant>(e->exp_desc)) return v->arg == nullptr;
    return false;
  };
  bool has_constant_constructor =
      arg_exps.size() == 2 && (constant_like(arg_exps[1]) || constant_like(arg_exps[0]));
  if (std::optional<Prim> s = specialize_primitive(env, ty, has_constant_constructor, prim)) prim = *s;
  lam_t lam = lambda_of_prim(p->prim_name, prim, loc, args, &arg_exps);
  if (primitive_needs_event_after(prim) && exp) lam = event_after(loc, exp, lam);
  return lam;
}

}  // namespace cppcaml::typing::translprim
