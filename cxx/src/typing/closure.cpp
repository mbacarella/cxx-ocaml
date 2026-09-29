// Port of middle_end/closure/closure.ml and closure_middle_end.ml.  See
// closure.hpp.
//
// OCaml evaluates a constructor's, a tuple's and an application's arguments
// right to left; the port evaluates them in the same order wherever an
// argument has effects (fresh identifiers, raise counts, constant labels,
// warnings): the comments "right to left" mark those places.  (A tuple
// matched on at once is not built: `match (a, b) with` evaluates a, then b.)
#include "cppcaml/typing/closure.hpp"

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>

#include "cppcaml/typing/arg_helper.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/convert_primitives.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/printclambda.hpp"
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/switch.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::closure {

using namespace clambda;
namespace L = lambda;
using P = clambda::Primitive;
using PK = clambda::Primitive::K;
using Approx = const ValueApproximation*;
using AK = ValueApproximation::Kind;
using SCK = UStructuredConstant::Kind;

namespace {

// ---- the backend (Arch, amd64) ---------------------------------------------------------------
constexpr long size_int = 8;
constexpr bool big_endian = false;
constexpr const char* target_os_type = "Unix";  // Config.target_os_type

[[noreturn]] void fatal_error(const std::string& s) { throw std::runtime_error(s); }
[[noreturn]] void no_phantom_lets() { fatal_error("Closure does not support phantom let generation"); }

struct IdentCmp {
  int operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b); }
};
struct LongCmp {
  int operator()(long a, long b) const { return a < b ? -1 : a > b ? 1 : 0; }
};
using VMap = PMap<Ident::t, ulambda, IdentCmp>;      // ulambda V.Map.t
using FEnv = PMap<Ident::t, Approx, IdentCmp>;       // value_approximation V.Map.t
using VSet = PMap<Ident::t, bool, IdentCmp>;         // V.Set.t
using IntMap = PMap<long, long, LongCmp>;            // Int.Map

// ---- OCaml's integers ------------------------------------------------------------------------
long wrap(std::uint64_t x) { return static_cast<long>(x << 1) >> 1; }  // to a 63-bit int
long iadd(long a, long b) { return wrap(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b)); }
long isub(long a, long b) { return wrap(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)); }
long imul(long a, long b) { return wrap(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)); }
long ineg(long a) { return wrap(0 - static_cast<std::uint64_t>(a)); }
long idiv(long a, long b) { return b == -1 ? ineg(a) : a / b; }
long imod(long a, long b) { return b == -1 ? 0 : a % b; }
// int_of_float (cvttsd2si, then tagging)
long int_of_float(double f) {
  std::int64_t r;
  if (std::isnan(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0)
    r = std::numeric_limits<std::int64_t>::min();
  else
    r = static_cast<std::int64_t>(f);
  return wrap(static_cast<std::uint64_t>(r));
}
std::int64_t add64(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}
std::int64_t sub64(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}
std::int64_t mul64(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}
std::int64_t neg64(std::int64_t a) { return static_cast<std::int64_t>(0 - static_cast<std::uint64_t>(a)); }
std::int64_t div64(std::int64_t a, std::int64_t b) { return b == -1 ? neg64(a) : a / b; }
std::int64_t rem64(std::int64_t a, std::int64_t b) { return b == -1 ? 0 : a % b; }
std::int64_t to32(std::int64_t a) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(a)); }

// ---- constructors ----------------------------------------------------------------------------
Approx value_const(const UConstant& c) {
  auto* a = make<ValueApproximation>();
  a->kind = AK::Value_const;
  a->c = c;
  return a;
}
// Value_tuple of a fresh array (mutable: Closure's global_approx)
Approx value_tuple(const std::vector<Approx>& v) {
  auto* a = make<ValueApproximation>();
  a->kind = AK::Value_tuple;
  a->tuple = slice(v);
  return a;
}
Approx value_closure(FunctionDescription* fd, Approx res) {
  auto* a = make<ValueApproximation>();
  a->kind = AK::Value_closure;
  a->fundesc = fd;
  a->res = res;
  return a;
}
Approx value_global_field(std::string_view sym, long i) {
  auto* a = make<ValueApproximation>();
  a->kind = AK::Value_global_field;
  a->sym = sym;
  a->field = i;
  return a;
}
const UStructuredConstant* sc_float(double f) {
  auto* c = make<UStructuredConstant>();
  c->kind = SCK::Uconst_float;
  c->f = f;
  return c;
}
const UStructuredConstant* sc_boxed(SCK k, std::int64_t i) {
  auto* c = make<UStructuredConstant>();
  c->kind = k;
  c->i = i;
  return c;
}
const UStructuredConstant* sc_block(long tag, const std::vector<UConstant>& fields) {
  auto* c = make<UStructuredConstant>();
  c->kind = SCK::Uconst_block;
  c->tag = tag;
  c->fields = slice(fields);
  return c;
}
const UStructuredConstant* sc_float_array(const std::vector<double>& fs) {
  auto* c = make<UStructuredConstant>();
  c->kind = SCK::Uconst_float_array;
  c->floats = slice(fs);
  return c;
}
const UStructuredConstant* sc_string(std::string_view s) {
  auto* c = make<UStructuredConstant>();
  c->kind = SCK::Uconst_string;
  c->s = s;
  return c;
}

VarWithProvenance vp_rename(const VarWithProvenance& v) { return {ident::rename(v.var), v.provenance}; }
L::ValueKind pgenval() { return L::ValueKind{}; }

template <class T>
Slice<T> sl(const std::vector<T>& v) {
  return slice(v);
}
template <class T>
std::vector<T> vec(Slice<T> s) {
  return std::vector<T>(s.begin(), s.end());
}

struct UA {  // ulambda * value_approximation
  ulambda u;
  Approx a;
};

// ---- Auxiliaries for accessing globals -------------------------------------------------------
// We change the name of the global to the name of the corresponding asm
// symbol.  This is done here and no longer in Cmmgen so that approximations
// stored in .cmx files contain the right names if the -for-pack option is
// active.
ulambda getglobal(const debuginfo::t& dbg, Ident::t id) {
  P p{PK::Pread_symbol};
  p.sym = compilenv::symbol_for_global(id);
  return uprim(p, {}, dbg);
}

// Check if a variable occurs in a [clambda] term.
bool occurs(Var var, ulambda u);
bool occurs_list(Var var, Slice<ulambda> l) {
  for (ulambda u : l)
    if (occurs(var, u)) return true;
  return false;
}
bool occurs(Var var, ulambda u) {
  switch (u->kind) {
    case UK::Uvar: return ident::same(static_cast<const Uvar*>(u)->id, var);
    case UK::Uconst: return false;
    case UK::Udirect_apply: return occurs_list(var, static_cast<const Udirect_apply*>(u)->args);
    case UK::Ugeneric_apply: {
      auto* x = static_cast<const Ugeneric_apply*>(u);
      return occurs(var, x->f) || occurs_list(var, x->args);
    }
    case UK::Uclosure: return occurs_list(var, static_cast<const Uclosure*>(u)->fv);
    case UK::Uoffset: return occurs(var, static_cast<const Uoffset*>(u)->l);
    case UK::Ulet: {
      auto* x = static_cast<const Ulet*>(u);
      return occurs(var, x->arg) || occurs(var, x->body);
    }
    case UK::Uphantom_let: no_phantom_lets();
    case UK::Uprim: return occurs_list(var, static_cast<const Uprim*>(u)->args);
    case UK::Uswitch: {
      auto* x = static_cast<const Uswitch*>(u);
      return occurs(var, x->arg) || occurs_list(var, x->sw.us_actions_consts) ||
             occurs_list(var, x->sw.us_actions_blocks);
    }
    case UK::Ustringswitch: {
      auto* x = static_cast<const Ustringswitch*>(u);
      if (occurs(var, x->arg)) return true;
      for (auto& c : x->cases)
        if (occurs(var, c.action)) return true;
      return x->def && occurs(var, x->def);
    }
    case UK::Ustaticfail: return occurs_list(var, static_cast<const Ustaticfail*>(u)->args);
    case UK::Ucatch: {
      auto* x = static_cast<const Ucatch*>(u);
      return occurs(var, x->body) || occurs(var, x->handler);
    }
    case UK::Utrywith: {
      auto* x = static_cast<const Utrywith*>(u);
      return occurs(var, x->body) || occurs(var, x->handler);
    }
    case UK::Uifthenelse: {
      auto* x = static_cast<const Uifthenelse*>(u);
      return occurs(var, x->cond) || occurs(var, x->ifso) || occurs(var, x->ifnot);
    }
    case UK::Usequence: {
      auto* x = static_cast<const Usequence*>(u);
      return occurs(var, x->l1) || occurs(var, x->l2);
    }
    case UK::Uwhile: {
      auto* x = static_cast<const Uwhile*>(u);
      return occurs(var, x->cond) || occurs(var, x->body);
    }
    case UK::Ufor: {
      auto* x = static_cast<const Ufor*>(u);
      return occurs(var, x->lo) || occurs(var, x->hi) || occurs(var, x->body);
    }
    case UK::Uassign: {
      auto* x = static_cast<const Uassign*>(u);
      return ident::same(x->id, var) || occurs(var, x->e);
    }
    case UK::Usend: {
      auto* x = static_cast<const Usend*>(u);
      return occurs(var, x->met) || occurs(var, x->obj) || occurs_list(var, x->args);
    }
    case UK::Uunreachable: return false;
  }
  return false;
}

// Determine whether the estimated size of a clambda term is below some
// threshold
long prim_size(const P& prim, Slice<ulambda> args) {
  long nargs = static_cast<long>(args.size());
  switch (prim.kind) {
    case PK::Pread_symbol: return 1;
    case PK::Pmakeblock: return 5 + nargs;
    case PK::Pmakelazyblock: return 6;
    case PK::Pfield: return 1;
    case PK::Psetfield:
      switch (prim.init) {
        case L::InitializationOrAssignment::Root_initialization: return 1;  // never causes a write barrier hit
        default: return prim.ptr == L::ImmediateOrPointer::Pointer ? 4 : 1;
      }
    case PK::Pfloatfield: return 1;
    case PK::Psetfloatfield: return 1;
    case PK::Pduprecord: return 10 + nargs;
    case PK::Pccall: return (prim.ccall->prim_alloc ? 10 : 4) + nargs;
    case PK::Praise: return 4;
    case PK::Pstringlength: return 5;
    case PK::Pbyteslength: return 5;
    case PK::Pstringrefs: return 6;
    case PK::Pbytesrefs:
    case PK::Pbytessets: return 6;
    case PK::Pmakearray: return 5 + nargs;
    case PK::Parraylength: return prim.array == L::ArrayKind::Pgenarray ? 6 : 2;
    case PK::Parrayrefu: return prim.array == L::ArrayKind::Pgenarray ? 12 : 2;
    case PK::Parraysetu: return prim.array == L::ArrayKind::Pgenarray ? 16 : 4;
    case PK::Parrayrefs: return prim.array == L::ArrayKind::Pgenarray ? 18 : 8;
    case PK::Parraysets: return prim.array == L::ArrayKind::Pgenarray ? 22 : 10;
    case PK::Pbigarrayref:
    case PK::Pbigarrayset: return 4 + prim.n * 6;
    default: return 2;  // arithmetic and comparisons
  }
}

// Very raw approximation of switch cost
struct Exit {};
struct Sizer {
  long size = 0;
  long threshold;
  void list(Slice<ulambda> l) {
    for (ulambda u : l) lam(u);
  }
  void lam(ulambda u) {
    if (size > threshold) throw Exit{};
    switch (u->kind) {
      case UK::Uvar: return;
      case UK::Uconst: ++size; return;
      case UK::Udirect_apply:
        size += 4;
        list(static_cast<const Udirect_apply*>(u)->args);
        return;
      case UK::Ugeneric_apply: {
        auto* x = static_cast<const Ugeneric_apply*>(u);
        size += 6;
        lam(x->f);
        list(x->args);
        return;
      }
      case UK::Uclosure: throw Exit{};  // inlining would duplicate function definitions
      case UK::Uoffset:
        ++size;
        lam(static_cast<const Uoffset*>(u)->l);
        return;
      case UK::Ulet: {
        auto* x = static_cast<const Ulet*>(u);
        lam(x->arg);
        lam(x->body);
        return;
      }
      case UK::Uphantom_let: no_phantom_lets();
      case UK::Uprim: {
        auto* x = static_cast<const Uprim*>(u);
        size += prim_size(x->p, x->args);
        list(x->args);
        return;
      }
      case UK::Uswitch: {
        auto* x = static_cast<const Uswitch*>(u);
        if (x->sw.us_actions_consts.size() > 1) size += 5;
        if (x->sw.us_actions_blocks.size() > 1) size += 5;
        lam(x->arg);
        list(x->sw.us_actions_consts);
        list(x->sw.us_actions_blocks);
        return;
      }
      case UK::Ustringswitch: {
        auto* x = static_cast<const Ustringswitch*>(u);
        lam(x->arg);
        // as ifthenelse
        for (auto& c : x->cases) {
          size += 2;
          lam(c.action);
        }
        if (x->def) lam(x->def);
        return;
      }
      case UK::Ustaticfail: list(static_cast<const Ustaticfail*>(u)->args); return;
      case UK::Ucatch: {
        auto* x = static_cast<const Ucatch*>(u);
        ++size;
        lam(x->body);
        lam(x->handler);
        return;
      }
      case UK::Utrywith: {
        auto* x = static_cast<const Utrywith*>(u);
        size += 8;
        lam(x->body);
        lam(x->handler);
        return;
      }
      case UK::Uifthenelse: {
        auto* x = static_cast<const Uifthenelse*>(u);
        size += 2;
        lam(x->cond);
        lam(x->ifso);
        lam(x->ifnot);
        return;
      }
      case UK::Usequence: {
        auto* x = static_cast<const Usequence*>(u);
        lam(x->l1);
        lam(x->l2);
        return;
      }
      case UK::Uwhile: {
        auto* x = static_cast<const Uwhile*>(u);
        size += 2;
        lam(x->cond);
        lam(x->body);
        return;
      }
      case UK::Ufor: {
        auto* x = static_cast<const Ufor*>(u);
        size += 4;
        lam(x->lo);
        lam(x->hi);
        lam(x->body);
        return;
      }
      case UK::Uassign:
        ++size;
        lam(static_cast<const Uassign*>(u)->e);
        return;
      case UK::Usend: {
        auto* x = static_cast<const Usend*>(u);
        size += 8;
        lam(x->met);
        lam(x->obj);
        list(x->args);
        return;
      }
      case UK::Uunreachable: return;
    }
  }
};
bool lambda_smaller(ulambda lam, long threshold) {
  Sizer s{0, threshold};
  try {
    s.lam(lam);
    return s.size <= threshold;
  } catch (const Exit&) {
    return false;
  }
}

bool is_pure_prim(const P& p) {
  using semantics_of_primitives::Effects;
  return semantics_of_primitives::for_primitive(p).first != Effects::Arbitrary_effects;
}

// Check if a clambda term is ``pure'', that is without side-effects *and*
// not containing function definitions (Pure terms may still read mutable
// state)
bool is_pure(ulambda u) {
  switch (u->kind) {
    case UK::Uvar:
    case UK::Uconst: return true;
    case UK::Uprim: {
      auto* x = static_cast<const Uprim*>(u);
      if (!is_pure_prim(x->p)) return false;
      for (ulambda a : x->args)
        if (!is_pure(a)) return false;
      return true;
    }
    case UK::Uoffset: return is_pure(static_cast<const Uoffset*>(u)->l);
    case UK::Ulet: {
      auto* x = static_cast<const Ulet*>(u);
      return x->mut == MutableFlag::Immutable && is_pure(x->arg) && is_pure(x->body);
    }
    default: return false;
  }
}

// ---- Simplify primitive operations on known arguments ---------------------------------------
UA make_const(const UConstant& c) { return {uconst(c), value_const(c)}; }
UA make_const_ref(const UStructuredConstant* c) {
  return make_const(uconst_ref(compilenv::new_structured_constant(c, true), c));
}
UA make_const_int(long n) { return make_const(uconst_int(n)); }
UA make_const_bool(bool b) { return make_const_int(b ? 1 : 0); }

template <class T>
UA make_integer_comparison(L::IntegerComparison cmp, T x, T y) {
  using C = L::IntegerComparison;
  switch (cmp) {
    case C::Ceq: return make_const_bool(x == y);
    case C::Cne: return make_const_bool(x != y);
    case C::Clt: return make_const_bool(x < y);
    case C::Cgt: return make_const_bool(x > y);
    case C::Cle: return make_const_bool(x <= y);
    case C::Cge: return make_const_bool(x >= y);
  }
  return make_const_bool(false);
}
UA make_float_comparison(L::FloatComparison cmp, double x, double y) {
  using C = L::FloatComparison;
  switch (cmp) {
    case C::CFeq: return make_const_bool(x == y);
    case C::CFneq: return make_const_bool(!(x == y));
    case C::CFlt: return make_const_bool(x < y);
    case C::CFnlt: return make_const_bool(!(x < y));
    case C::CFgt: return make_const_bool(x > y);
    case C::CFngt: return make_const_bool(!(x > y));
    case C::CFle: return make_const_bool(x <= y);
    case C::CFnle: return make_const_bool(!(x <= y));
    case C::CFge: return make_const_bool(x >= y);
    case C::CFnge: return make_const_bool(!(x >= y));
  }
  return make_const_bool(false);
}
UA make_const_float(double n) { return make_const_ref(sc_float(n)); }
UA make_const_natint(std::int64_t n) { return make_const_ref(sc_boxed(SCK::Uconst_nativeint, n)); }
UA make_const_int32(std::int64_t n) { return make_const_ref(sc_boxed(SCK::Uconst_int32, to32(n))); }
UA make_const_int64(std::int64_t n) { return make_const_ref(sc_boxed(SCK::Uconst_int64, n)); }

// Value_const (Uconst_int n)
bool const_int(Approx a, long& n) {
  if (a->kind != AK::Value_const || a->c.kind != UConstant::Kind::Uconst_int) return false;
  n = a->c.i;
  return true;
}
// Value_const (Uconst_ref (_, Some c)) with c of kind k
const UStructuredConstant* const_ref(Approx a, SCK k) {
  if (a->kind != AK::Value_const || a->c.kind != UConstant::Kind::Uconst_ref || !a->c.sc) return nullptr;
  return a->c.sc->kind == k ? a->c.sc : nullptr;
}

// The [fpc] parameter is true if constant propagation of floating-point
// computations is allowed
UA simplif_arith_prim_pure(bool fpc, const P& p, Slice<ulambda> args, const std::vector<Approx>& approxs,
                           const debuginfo::t& dbg) {
  using BI = BoxedInteger;
  UA default_{uprim(p, args, dbg), value_unknown()};
  auto bi = [&](PK k, BI b) { return p.kind == k && p.bi == b; };
  long n1, n2;
  const UStructuredConstant *c1, *c2;
  // int (or enumerated type)
  if (approxs.size() == 1 && const_int(approxs[0], n1)) {
    switch (p.kind) {
      case PK::Pnot: return make_const_bool(n1 == 0);
      case PK::Pnegint: return make_const_int(ineg(n1));
      case PK::Poffsetint: return make_const_int(iadd(p.n, n1));
      case PK::Pfloatofint:
        if (fpc) return make_const_float(static_cast<double>(n1));
        return default_;
      case PK::Pbintofint:
        switch (p.bi) {
          case BI::Pnativeint: return make_const_natint(n1);
          case BI::Pint32: return make_const_int32(n1);
          case BI::Pint64: return make_const_int64(n1);
        }
        return default_;
      case PK::Pbswap16: return make_const_int(((n1 & 0xff) << 8) | ((n1 & 0xff00) >> 8));
      default: return default_;
    }
  }
  // int (or enumerated type), int (or enumerated type)
  if (approxs.size() == 2 && const_int(approxs[0], n1) && const_int(approxs[1], n2)) {
    bool shift_ok = 0 <= n2 && n2 < 8 * size_int;
    switch (p.kind) {
      case PK::Psequand: return make_const_bool(n1 != 0 && n2 != 0);
      case PK::Psequor: return make_const_bool(n1 != 0 || n2 != 0);
      case PK::Paddint: return make_const_int(iadd(n1, n2));
      case PK::Psubint: return make_const_int(isub(n1, n2));
      case PK::Pmulint: return make_const_int(imul(n1, n2));
      case PK::Pdivint:
        if (n2 != 0) return make_const_int(idiv(n1, n2));
        return default_;
      case PK::Pmodint:
        if (n2 != 0) return make_const_int(imod(n1, n2));
        return default_;
      case PK::Pandint: return make_const_int(n1 & n2);
      case PK::Porint: return make_const_int(n1 | n2);
      case PK::Pxorint: return make_const_int(n1 ^ n2);
      case PK::Plslint:
        if (shift_ok) return make_const_int(wrap(static_cast<std::uint64_t>(n1) << n2));
        return default_;
      case PK::Plsrint:
        if (shift_ok) return make_const_int(wrap((static_cast<std::uint64_t>(n1) & 0x7fffffffffffffffULL) >> n2));
        return default_;
      case PK::Pasrint:
        if (shift_ok) return make_const_int(n1 >> n2);
        return default_;
      case PK::Pintcomp: return make_integer_comparison(p.icmp, n1, n2);
      default: return default_;
    }
  }
  // float
  if (approxs.size() == 1 && fpc && (c1 = const_ref(approxs[0], SCK::Uconst_float))) {
    double f1 = c1->f;
    switch (p.kind) {
      case PK::Pintoffloat: return make_const_int(int_of_float(f1));
      case PK::Pnegfloat: return make_const_float(-f1);
      case PK::Pabsfloat: return make_const_float(std::fabs(f1));
      default: return default_;
    }
  }
  // float, float
  if (approxs.size() == 2 && fpc && (c1 = const_ref(approxs[0], SCK::Uconst_float)) &&
      (c2 = const_ref(approxs[1], SCK::Uconst_float))) {
    double f1 = c1->f, f2 = c2->f;
    switch (p.kind) {
      case PK::Paddfloat: return make_const_float(f1 + f2);
      case PK::Psubfloat: return make_const_float(f1 - f2);
      case PK::Pmulfloat: return make_const_float(f1 * f2);
      case PK::Pdivfloat: return make_const_float(f1 / f2);
      case PK::Pfloatcomp: return make_float_comparison(p.fcmp, f1, f2);
      default: return default_;
    }
  }
  // boxed integers: nativeint, int32, int64
  for (auto [k, b, bits] : {std::tuple{SCK::Uconst_nativeint, BI::Pnativeint, 64},
                            std::tuple{SCK::Uconst_int32, BI::Pint32, 32},
                            std::tuple{SCK::Uconst_int64, BI::Pint64, 64}}) {
    auto mk = [&, b = b](std::int64_t n) {
      switch (b) {
        case BI::Pnativeint: return make_const_natint(n);
        case BI::Pint32: return make_const_int32(n);
        default: return make_const_int64(n);
      }
    };
    // one argument
    if (approxs.size() == 1 && (c1 = const_ref(approxs[0], k))) {
      std::int64_t n = c1->i;
      if (bi(PK::Pintofbint, b)) return make_const_int(wrap(static_cast<std::uint64_t>(n)));
      if (p.kind == PK::Pcvtbint && p.bi == b) {
        if (b == BI::Pnativeint && p.bi2 == BI::Pint32) return make_const_int32(n);
        if (b == BI::Pnativeint && p.bi2 == BI::Pint64) return make_const_int64(n);
        if (b == BI::Pint32 && p.bi2 == BI::Pnativeint) return make_const_natint(n);
        if (b == BI::Pint32 && p.bi2 == BI::Pint64) return make_const_int64(n);
        if (b == BI::Pint64 && p.bi2 == BI::Pint32) return make_const_int32(n);
        if (b == BI::Pint64 && p.bi2 == BI::Pnativeint) return make_const_natint(n);
        return default_;
      }
      if (bi(PK::Pnegbint, b)) return mk(neg64(n));
      return default_;
    }
    // two arguments of the kind
    if (approxs.size() == 2 && (c1 = const_ref(approxs[0], k)) && (c2 = const_ref(approxs[1], k))) {
      std::int64_t m1 = c1->i, m2 = c2->i;
      if (p.bi != b) return default_;
      switch (p.kind) {
        case PK::Paddbint: return mk(add64(m1, m2));
        case PK::Psubbint: return mk(sub64(m1, m2));
        case PK::Pmulbint: return mk(mul64(m1, m2));
        case PK::Pdivbint:
          if (m2 != 0) return mk(div64(m1, m2));
          return default_;
        case PK::Pmodbint:
          if (m2 != 0) return mk(rem64(m1, m2));
          return default_;
        case PK::Pandbint: return mk(m1 & m2);
        case PK::Porbint: return mk(m1 | m2);
        case PK::Pxorbint: return mk(m1 ^ m2);
        case PK::Pbintcomp: return make_integer_comparison(p.icmp, m1, m2);
        default: return default_;
      }
    }
    // the kind, int
    if (approxs.size() == 2 && (c1 = const_ref(approxs[0], k)) && const_int(approxs[1], n2)) {
      std::int64_t m1 = c1->i;
      long width = bits == 32 ? 32 : 8 * size_int;
      if (b == BI::Pint64) width = 64;
      if (p.bi != b || !(0 <= n2 && n2 < width)) return default_;
      std::uint64_t mask = bits == 32 ? 0xffffffffULL : ~0ULL;
      switch (p.kind) {
        case PK::Plslbint: return mk(static_cast<std::int64_t>(static_cast<std::uint64_t>(m1) << n2));
        case PK::Plsrbint: return mk(static_cast<std::int64_t>((static_cast<std::uint64_t>(m1) & mask) >> n2));
        case PK::Pasrbint: return mk(m1 >> n2);
        default: return default_;
      }
    }
  }
  // TODO: Pbbswap
  // Catch-all
  return default_;
}

Approx field_approx(long n, Approx a) {
  if (a->kind == AK::Value_tuple && n < static_cast<long>(a->tuple.size())) return a->tuple[n];
  if (const UStructuredConstant* c = const_ref(a, SCK::Uconst_block); c && n < static_cast<long>(c->fields.size()))
    return value_const(c->fields[n]);
  return value_unknown();
}

bool is_makeblock(ulambda u) {
  auto* x = as<Uprim>(u);
  return x && x->p.kind == PK::Pmakeblock;
}

UA simplif_prim_pure(bool fpc, const P& p, Slice<ulambda> args, const std::vector<Approx>& approxs,
                     const debuginfo::t& dbg) {
  // Block construction
  if (p.kind == PK::Pmakeblock && p.mut == MutableFlag::Immutable) {
    std::vector<UConstant> fields;
    bool all_const = true;
    for (Approx a : approxs) {
      if (a->kind != AK::Value_const) {
        all_const = false;
        break;
      }
      fields.push_back(a->c);
    }
    if (all_const) {
      const UStructuredConstant* cst = sc_block(p.n, fields);
      std::string_view name = compilenv::new_structured_constant(cst, true);
      return make_const(uconst_ref(name, cst));
    }
    return {uprim(p, args, dbg), value_tuple(approxs)};
  }
  // Field access
  if (p.kind == PK::Pfield) {
    if (approxs.size() == 1)
      if (const UStructuredConstant* c = const_ref(approxs[0], SCK::Uconst_block);
          c && p.n < static_cast<long>(c->fields.size()))
        return make_const(c->fields[p.n]);
    if (args.size() == 1 && is_makeblock(args[0]) && approxs.size() == 1) {
      auto* mb = static_cast<const Uprim*>(args[0]);
      // This case is particularly useful for removing allocations for
      // optional parameters
      if (p.n < static_cast<long>(mb->args.size())) return {mb->args[p.n], field_approx(p.n, approxs[0])};
    }
  }
  // Strings
  if ((p.kind == PK::Pstringlength || p.kind == PK::Pbyteslength) && approxs.size() == 1)
    if (const UStructuredConstant* c = const_ref(approxs[0], SCK::Uconst_string))
      return make_const_int(static_cast<long>(c->s.size()));
  // Kind test
  if (p.kind == PK::Pisint) {
    // This case is particularly useful for removing allocations for
    // optional parameters
    if (args.size() == 1 && is_makeblock(args[0])) return make_const_bool(false);
    if (approxs.size() == 1) {
      Approx a1 = approxs[0];
      if (a1->kind == AK::Value_const)
        return make_const_bool(a1->c.kind == UConstant::Kind::Uconst_int);
      if (a1->kind == AK::Value_closure || a1->kind == AK::Value_tuple) return make_const_bool(false);
      return {uprim(p, args, dbg), value_unknown()};
    }
  }
  // Catch-all
  return simplif_arith_prim_pure(fpc, p, args, approxs, dbg);
}

UA simplif_prim(bool fpc, const P& p, Slice<ulambda> args, const std::vector<Approx>& approxs,
                const debuginfo::t& dbg) {
  bool all_pure = true;
  for (ulambda a : args)
    if (!is_pure(a)) {
      all_pure = false;
      break;
    }
  if (all_pure) return simplif_prim_pure(fpc, p, args, approxs, dbg);
  // XXX : always return the same approxs as simplif_prim_pure?
  Approx approx = p.kind == PK::Pmakeblock && p.mut == MutableFlag::Immutable ? value_tuple(approxs)
                                                                               : value_unknown();
  return {uprim(p, args, dbg), approx};
}

// Substitute variables in a [ulambda] term (a body of an inlined function)
// and perform some more simplifications on integer primitives.  Also
// perform alpha-conversion on let-bound identifiers to avoid clashes with
// locally-generated identifiers, and refresh raise counts in order to avoid
// clashes with inlined code from other modules.  The variables must not be
// assigned in the term.  This is used to substitute "trivial" arguments for
// parameters during inline expansion, and also for the translation of let
// rec over functions.

Approx approx_ulam(ulambda u) {
  if (auto* c = as<Uconst>(u)) return value_const(c->c);
  return value_unknown();
}

std::optional<ulambda> find_action(Slice<long> idxs, Slice<ulambda> acts, long tag) {
  if (0 <= tag && tag < static_cast<long>(idxs.size())) {
    long idx = idxs[tag];
    return acts[idx];
  }
  // Can this happen?
  return std::nullopt;
}

debuginfo::t subst_debuginfo(const debuginfo::t& loc, const debuginfo::t& dbg) {
  if (clflags::debug) return debuginfo::inline_(loc, dbg);
  return dbg;
}

struct Subst {
  const debuginfo::t& loc;
  bool fpc;

  ulambda operator()(const VMap& sb, const std::optional<IntMap>& rn, ulambda u) const { return go(sb, rn, u); }
  std::vector<ulambda> map(const VMap& sb, const std::optional<IntMap>& rn, Slice<ulambda> l) const {
    std::vector<ulambda> r;  // List.map: left to right
    for (ulambda u : l) r.push_back(go(sb, rn, u));
    return r;
  }
  ulambda go(const VMap& sb, const std::optional<IntMap>& rn, ulambda ulam) const {
    switch (ulam->kind) {
      case UK::Uvar: {
        if (const ulambda* v = sb.find_opt(static_cast<const Uvar*>(ulam)->id)) return *v;
        return ulam;
      }
      case UK::Uconst: return ulam;
      case UK::Udirect_apply: {
        auto* x = static_cast<const Udirect_apply*>(ulam);
        debuginfo::t dbg = subst_debuginfo(loc, x->dbg);
        return udirect_apply(x->f, sl(map(sb, rn, x->args)), dbg);
      }
      case UK::Ugeneric_apply: {
        auto* x = static_cast<const Ugeneric_apply*>(ulam);
        debuginfo::t dbg = subst_debuginfo(loc, x->dbg);
        // right to left
        auto args = map(sb, rn, x->args);
        ulambda f = go(sb, rn, x->f);
        return ugeneric_apply(f, sl(args), dbg);
      }
      case UK::Uclosure: {
        // Question: should we rename function labels as well?  Otherwise,
        // there is a risk that function labels are not globally unique.
        // This should not happen in the current system because:
        // - Inlined function bodies contain no Uclosure nodes
        //   (cf. function [lambda_smaller])
        // - When we substitute offsets for idents bound by let rec in
        //   [close], case [Lletrec], we discard the original let rec body
        //   and use only the substituted term.
        auto* x = static_cast<const Uclosure*>(ulam);
        return uclosure(x->funs, sl(map(sb, rn, x->fv)));
      }
      case UK::Uoffset: {
        auto* x = static_cast<const Uoffset*>(ulam);
        return uoffset(go(sb, rn, x->l), x->ofs);
      }
      case UK::Ulet: {
        auto* x = static_cast<const Ulet*>(ulam);
        VarWithProvenance id2 = vp_rename(x->id);
        // right to left
        ulambda body = go(sb.add(x->id.var, uvar(id2.var)), rn, x->body);
        ulambda arg = go(sb, rn, x->arg);
        return ulet(x->mut, x->k, id2, arg, body);
      }
      case UK::Uphantom_let: no_phantom_lets();
      case UK::Uprim: {
        auto* x = static_cast<const Uprim*>(ulam);
        std::vector<ulambda> sargs = map(sb, rn, x->args);
        debuginfo::t dbg = subst_debuginfo(loc, x->dbg);
        std::vector<Approx> approxs;
        for (ulambda a : sargs) approxs.push_back(approx_ulam(a));
        return simplif_prim(fpc, x->p, sl(sargs), approxs, dbg).u;
      }
      case UK::Uswitch: {
        auto* x = static_cast<const Uswitch*>(ulam);
        ulambda sarg = go(sb, rn, x->arg);
        // Unfortunately, we cannot easily deal with the case of a
        // constructed block (makeblock) bound to a local identifier.  This
        // would require to keep track of local let bindings (at least their
        // approximations) in this substitute function.
        std::optional<ulambda> action;
        if (auto* c = as<Uconst>(sarg)) {
          if (c->c.kind == UConstant::Kind::Uconst_ref && c->c.sc && c->c.sc->kind == SCK::Uconst_block)
            action = find_action(x->sw.us_index_blocks, x->sw.us_actions_blocks, c->c.sc->tag);
          else if (c->c.kind == UConstant::Kind::Uconst_int)
            action = find_action(x->sw.us_index_consts, x->sw.us_actions_consts, c->c.i);
        }
        if (action) return go(sb, rn, *action);
        USwitch sw = x->sw;
        // right to left
        sw.us_actions_blocks = sl(map(sb, rn, x->sw.us_actions_blocks));
        sw.us_actions_consts = sl(map(sb, rn, x->sw.us_actions_consts));
        return uswitch(sarg, sw, x->dbg);
      }
      case UK::Ustringswitch: {
        auto* x = static_cast<const Ustringswitch*>(ulam);
        // right to left
        ulambda d = x->def ? go(sb, rn, x->def) : nullptr;
        std::vector<UStringCase> cases;
        for (auto& c : x->cases) cases.push_back({c.s, go(sb, rn, c.action)});
        ulambda arg = go(sb, rn, x->arg);
        return ustringswitch(arg, sl(cases), d);
      }
      case UK::Ustaticfail: {
        auto* x = static_cast<const Ustaticfail*>(ulam);
        long nfail = x->i;
        if (rn) {
          const long* v = rn->find_opt(nfail);
          if (!v) fatal_error("Closure.split_list: invalid nfail (" + std::to_string(nfail) + ")");
          nfail = *v;
        }
        return ustaticfail(nfail, sl(map(sb, rn, x->args)));
      }
      case UK::Ucatch: {
        auto* x = static_cast<const Ucatch*>(ulam);
        long nfail = x->i;
        std::optional<IntMap> rn2 = rn;
        if (rn) {
          long new_nfail = L::next_raise_count();
          rn2 = rn->add(nfail, new_nfail);
          nfail = new_nfail;
        }
        std::vector<UParam> ids2;
        for (auto& p : x->vars) ids2.push_back({vp_rename(p.var), p.kind});
        // List.fold_right2 (the last binding added first)
        VMap sb2 = sb;
        for (std::size_t k = ids2.size(); k-- > 0;) sb2 = sb2.add(x->vars[k].var.var, uvar(ids2[k].var.var));
        // right to left
        ulambda handler = go(sb2, rn2, x->handler);
        ulambda body = go(sb, rn2, x->body);
        return ucatch(nfail, sl(ids2), body, handler);
      }
      case UK::Utrywith: {
        auto* x = static_cast<const Utrywith*>(ulam);
        VarWithProvenance id2 = vp_rename(x->exn);
        // right to left
        ulambda handler = go(sb.add(x->exn.var, uvar(id2.var)), rn, x->handler);
        ulambda body = go(sb, rn, x->body);
        return utrywith(body, id2, handler);
      }
      case UK::Uifthenelse: {
        auto* x = static_cast<const Uifthenelse*>(ulam);
        ulambda su1 = go(sb, rn, x->cond);
        if (auto* c = as<Uconst>(su1); c && c->c.kind == UConstant::Kind::Uconst_int) {
          if (c->c.i != 0) return go(sb, rn, x->ifso);
          return go(sb, rn, x->ifnot);
        }
        // right to left
        ulambda ifnot = go(sb, rn, x->ifnot);
        ulambda ifso = go(sb, rn, x->ifso);
        return uifthenelse(su1, ifso, ifnot);
      }
      case UK::Usequence: {
        auto* x = static_cast<const Usequence*>(ulam);
        // right to left
        ulambda l2 = go(sb, rn, x->l2);
        ulambda l1 = go(sb, rn, x->l1);
        return usequence(l1, l2);
      }
      case UK::Uwhile: {
        auto* x = static_cast<const Uwhile*>(ulam);
        // right to left
        ulambda body = go(sb, rn, x->body);
        ulambda cond = go(sb, rn, x->cond);
        return uwhile(cond, body);
      }
      case UK::Ufor: {
        auto* x = static_cast<const Ufor*>(ulam);
        VarWithProvenance id2 = vp_rename(x->id);
        // right to left
        ulambda body = go(sb.add(x->id.var, uvar(id2.var)), rn, x->body);
        ulambda hi = go(sb, rn, x->hi);
        ulambda lo = go(sb, rn, x->lo);
        return ufor(id2, lo, hi, x->dir, body);
      }
      case UK::Uassign: {
        auto* x = static_cast<const Uassign*>(ulam);
        Var id2 = x->id;
        if (const ulambda* v = sb.find_opt(x->id)) {
          auto* uv = as<Uvar>(*v);
          if (!uv) fatal_error("Closure.substitute: Uassign");
          id2 = uv->id;
        }
        return uassign(id2, go(sb, rn, x->e));
      }
      case UK::Usend: {
        auto* x = static_cast<const Usend*>(ulam);
        debuginfo::t dbg = subst_debuginfo(loc, x->dbg);
        // right to left
        auto args = map(sb, rn, x->args);
        ulambda obj = go(sb, rn, x->obj);
        ulambda met = go(sb, rn, x->met);
        return usend(x->k, met, obj, sl(args), dbg);
      }
      case UK::Uunreachable: return uunreachable();
    }
    return ulam;
  }
};

ulambda substitute(const debuginfo::t& loc, bool fpc, const VMap& sb, const std::optional<IntMap>& rn, ulambda u) {
  return Subst{loc, fpc}(sb, rn, u);
}

// ---- environments ----------------------------------------------------------------------------
struct ClosureEntry {  // Free_variable of int | Function of int
  bool is_function;
  long pos;
};
using Entries = PMap<Ident::t, ClosureEntry, IdentCmp>;
struct ClosureEnv {  // Not_in_closure | In_closure {entries; env_param; env_pos}
  bool in_closure = false;
  Entries entries;
  Var env_param = nullptr;
  long env_pos = 0;
};
struct CEnv {
  ClosureEnv cenv;
  FEnv fenv;
  VSet mutable_vars;
};

// Perform an inline expansion:
//
// If [f p = body], substitute [f a] by [let p = a in body].
//
// Under certain conditions, further simplifications are possible (we use
// the terminology of [Semantics_of_primitives], applied to terms of the
// Clambda language):
//
// - [f a] is equivalent to [body[a/p]] if [a] has no effects and no
//   coeffects.  However, we only want to do this rewriting if [body[a/p]]
//   does not increase the size of [body]. Since this is hard to decide in
//   general, as an approximation, only consider the case when [a] is an
//   immutable variable or a constant.
//
// - [f a] is equivalent to [body] if [p] does not occur in [body] and [a]
//   has only generative effects.
//
// - In general [f a] is equivalent to [a; body] if [p] does not occur in
//   [body].

// Approximates "no effects and no coeffects"
bool is_substituable(const VSet& mutable_vars, ulambda u) {
  switch (u->kind) {
    case UK::Uvar: return !mutable_vars.mem(static_cast<const Uvar*>(u)->id);
    case UK::Uconst: return true;
    case UK::Uoffset: return is_substituable(mutable_vars, static_cast<const Uoffset*>(u)->l);
    default: return false;
  }
}

// Approximates "only generative effects"
bool is_erasable(ulambda u) { return u->kind == UK::Uclosure || is_pure(u); }

ulambda bind_params(const CEnv& env, const L::ScopedLocation& loc, const FunctionDescription* fdesc,
                    Slice<VarWithProvenance> params0, Slice<ulambda> args0, ulambda funct, ulambda body0) {
  bool fpc = fdesc->fun_float_const_prop;
  // Reverse parameters and arguments to preserve right-to-left evaluation
  // order (PR#2910).
  std::vector<VarWithProvenance> params(params0.begin(), params0.end());
  std::vector<ulambda> args(args0.begin(), args0.end());
  std::reverse(params.begin(), params.end());
  std::reverse(args.begin(), args.end());
  ulambda body = body0;
  // Ensure funct is evaluated after args
  if (!params.empty() && !fdesc->fun_closed) {
    VarWithProvenance my_closure = params.front();
    params.erase(params.begin());
    params.push_back(my_closure);
    args.push_back(funct);
  } else if (!is_pure(funct)) {
    body = usequence(funct, body);
  }
  if (params.size() != args.size()) fatal_error("Closure.bind_params");
  std::function<ulambda(const VMap&, std::size_t)> aux = [&](const VMap& subst, std::size_t k) -> ulambda {
    if (k == params.size()) return substitute(debuginfo::from_location(loc), fpc, subst, IntMap{}, body);
    const VarWithProvenance& p1 = params[k];
    ulambda a1 = args[k];
    if (is_substituable(env.mutable_vars, a1)) return aux(subst.add(p1.var, a1), k + 1);
    VarWithProvenance p1b = vp_rename(p1);
    ulambda u1, u2;
    auto* mb = as<Uprim>(a1);
    if (ident::name(p1.var) == "*opt*" && mb && mb->p.kind == PK::Pmakeblock && mb->p.n == 0 &&
        mb->p.mut == MutableFlag::Immutable && mb->args.size() == 1) {
      // This parameter corresponds to an optional parameter, and although
      // it is used twice pushing the expression down actually allows us to
      // remove the allocation as it will appear once under a Pisint
      // primitive and once under a Pfield primitive (see
      // [simplif_prim_pure])
      u1 = mb->args[0];
      u2 = uprim(mb->p, sl(std::vector<ulambda>{uvar(p1b.var)}), mb->dbg);
    } else {
      u1 = a1;
      u2 = uvar(p1b.var);
    }
    ulambda body2 = aux(subst.add(p1.var, u2), k + 1);
    if (occurs(p1.var, body)) return ulet(MutableFlag::Immutable, pgenval(), p1b, u1, body2);
    if (is_erasable(a1)) return body2;
    return usequence(a1, body2);
  };
  return aux(VMap{}, 0);
}

using ArgsFn = std::function<ulambda(Slice<ulambda>)>;
ulambda bind_args_right_to_left(const CEnv& env, Slice<ulambda> args, const ArgsFn& fn0) {
  std::vector<ulambda> prev_args_rev;
  ArgsFn fn = fn0;
  for (ulambda arg : args) {
    if (is_substituable(env.mutable_vars, arg)) {
      prev_args_rev.push_back(arg);
    } else {
      Var id = Ident::create_local(OCAML_LIT("arg"));
      ArgsFn inner = fn;
      fn = [inner, id, arg](Slice<ulambda> a) {
        return ulet(MutableFlag::Immutable, pgenval(), vp(id), arg, inner(a));
      };
      prev_args_rev.push_back(uvar(id));
    }
  }
  return fn(sl(prev_args_rev));
}

void warning_if_forced_inline(const L::ScopedLocation& loc, const L::InlineAttribute& attribute,
                              const std::string& warning) {
  if (attribute.kind == L::InlineAttribute::Kind::Always_inline)
    location::prerr_warning(debuginfo::to_location(loc),
                            warnings::Warning::with_s(warnings::Warning::K::Inlining_impossible, warning));
}

// Generate a direct application
ulambda direct_apply(const CEnv& env, const FunctionDescription* fundesc, ulambda ufunct, Slice<ulambda> uargs,
                     const L::ScopedLocation& loc, const L::InlineAttribute& attribute) {
  if (attribute.kind == L::InlineAttribute::Kind::Never_inline || !fundesc->has_inline) {
    debuginfo::t dbg = debuginfo::from_location(loc);
    warning_if_forced_inline(loc, attribute, "Function information unavailable");
    if (fundesc->fun_closed && is_pure(ufunct)) return udirect_apply(fundesc->fun_label, uargs, dbg);
    if (!fundesc->fun_closed && is_substituable(env.mutable_vars, ufunct)) {
      std::vector<ulambda> a = vec(uargs);
      a.push_back(ufunct);
      return udirect_apply(fundesc->fun_label, sl(a), dbg);
    }
    return bind_args_right_to_left(env, uargs, [&](Slice<ulambda> app_args) -> ulambda {
      if (fundesc->fun_closed) return usequence(ufunct, udirect_apply(fundesc->fun_label, app_args, dbg));
      Var clos = Ident::create_local(OCAML_LIT("clos"));
      std::vector<ulambda> a = vec(app_args);
      a.push_back(uvar(clos));
      return ulet(MutableFlag::Immutable, pgenval(), vp(clos), ufunct, udirect_apply(fundesc->fun_label, sl(a), dbg));
    });
  }
  return bind_params(env, loc, fundesc, fundesc->inline_params, uargs, ufunct, fundesc->inline_body);
}

// Add [Value_integer] info to the approximation of an application
Approx strengthen_approx(ulambda appl, Approx approx) {
  Approx a = approx_ulam(appl);
  return a->kind == AK::Value_const ? a : approx;
}

// If a term has approximation Value_integer and is pure, replace it by an
// integer constant
UA check_constant_result(ulambda ulam, Approx approx) {
  if (approx->kind == AK::Value_const && is_pure(ulam)) return make_const(approx->c);
  if (approx->kind == AK::Value_global_field && is_pure(ulam)) {
    if (auto* f = as<Uprim>(ulam); f && f->p.kind == PK::Pfield && f->args.size() == 1)
      if (auto* r = as<Uprim>(f->args[0]); r && r->p.kind == PK::Pread_symbol) return {ulam, approx};
    P rs{PK::Pread_symbol};
    rs.sym = approx->sym;
    ulambda glb = uprim(rs, {}, debuginfo::none());
    P fld{PK::Pfield};
    fld.n = approx->field;
    fld.ptr = L::ImmediateOrPointer::Pointer;
    fld.mut = MutableFlag::Immutable;
    return {uprim(fld, sl(std::vector<ulambda>{glb}), debuginfo::none()), approx};
  }
  return {ulam, approx};
}

// Evaluate an expression with known value for its side effects only, or
// discard it if it's pure
UA sequence_constant_expr(ulambda ulam1, const UA& res2) {
  if (is_pure(ulam1)) return res2;
  return {usequence(ulam1, res2.u), res2.a};
}

// Maintain the approximation of the global structure being defined
Approx* global_approx = nullptr;  // the array of the Value_tuple
long global_approx_size = 0;

// Maintain the nesting depth for functions
long function_nesting_depth = 0;
constexpr long excessive_function_nesting_depth = 5;

// Uncurry an expression and explicitate closures.  Also return the
// approximation of the expression.  The approximation environment [fenv]
// maps idents to approximations.  Idents not bound in [fenv] approximate to
// [Value_unknown].  The closure environment [cenv] maps idents to [ulambda]
// terms.  It is used to substitute environment accesses for free
// identifiers.

struct NotClosed {};

UA close_approx_var(const CEnv& env, Ident::t id) {
  const Approx* found = env.fenv.find_opt(id);
  Approx approx = found ? *found : value_unknown();
  if (approx->kind == AK::Value_const) return make_const(approx->c);
  if (!env.cenv.in_closure) return {uvar(id), approx};
  const ClosureEntry* e = env.cenv.entries.find_opt(id);
  ulambda subst;
  if (!e) {
    subst = uvar(id);
  } else if (!e->is_function) {
    P fld{PK::Pfield};
    fld.n = e->pos - env.cenv.env_pos;
    fld.ptr = L::ImmediateOrPointer::Pointer;
    fld.mut = MutableFlag::Immutable;
    subst = uprim(fld, sl(std::vector<ulambda>{uvar(env.cenv.env_param)}), debuginfo::none());
  } else {
    subst = uoffset(uvar(env.cenv.env_param), e->pos - env.cenv.env_pos);
  }
  return {subst, approx};
}

ulambda close_var(const CEnv& env, Ident::t id) { return close_approx_var(env, id).u; }

UA close(const CEnv& env, L::lambda lam);
UA close_prim(const CEnv& env, const L::Lprim* x);
std::vector<ulambda> close_list(const CEnv& env, Slice<L::lambda> l);
std::pair<ulambda, std::vector<std::pair<Ident::t, std::pair<long, Approx>>>> close_functions(
    const CEnv& env, Slice<L::RecBinding> fun_defs);
UA close_one_function(const CEnv& env, Ident::t id, const L::LFunction* funct);
struct SwitchResult {
  std::vector<long> index;
  std::vector<ulambda> actions;
  std::function<ulambda(ulambda)> hs;
};
SwitchResult close_switch(const CEnv& env, Slice<L::SwitchCase> cases, long num_keys, L::lambda def);

const UStructuredConstant* transl_const_str(const UStructuredConstant* c);
UConstant transl_const(const L::StructuredConstant* cst) {
  using CK = L::StructuredConstant::Kind;
  auto str = [](const UStructuredConstant* c) {
    std::string_view name = compilenv::new_structured_constant(c, true);
    return uconst_ref(name, c);
  };
  auto float_of_string = [](std::string_view s) {
    double d = 0;
    if (!arg_helper::float_of_string_opt(std::string(s), d)) fatal_error("float_of_string");
    return d;
  };
  switch (cst->kind) {
    case CK::Const_int: return uconst_int(cst->i);
    case CK::Const_char: return uconst_int(cst->i);
    case CK::Const_block: {
      std::vector<UConstant> fields;  // List.map: left to right
      for (const L::StructuredConstant* f : cst->fields) fields.push_back(transl_const(f));
      return str(sc_block(cst->i, fields));
    }
    case CK::Const_float_array: {
      // constant float arrays are really immutable
      std::vector<double> fs;
      for (std::string_view s : cst->floats) fs.push_back(float_of_string(s));
      return str(sc_float_array(fs));
    }
    case CK::Const_immstring: return str(sc_string(cst->s));
    case CK::Const_float: return str(sc_float(float_of_string(cst->s)));
    case CK::Const_int32: return str(sc_boxed(SCK::Uconst_int32, cst->boxed));
    case CK::Const_int64: return str(sc_boxed(SCK::Uconst_int64, cst->boxed));
    case CK::Const_nativeint: return str(sc_boxed(SCK::Uconst_nativeint, cst->boxed));
  }
  fatal_error("Closure.transl_const");
}

UA close_apply(const CEnv& env, const L::Lapply* ap) {
  const L::LambdaApply& a = ap->ap;
  const L::ScopedLocation& loc = a.ap_loc;
  const L::InlineAttribute& attribute = a.ap_inlined;
  long nargs = static_cast<long>(a.ap_args.size());
  // `match (close env funct, close_list env args) with`: a tuple scrutinee
  // is not built, its components are let-bound left to right (Translcore)
  UA f = close(env, a.ap_func);
  std::vector<ulambda> uargs = close_list(env, a.ap_args);
  ulambda ufunct = f.u;
  if (f.a->kind == AK::Value_closure) {
    FunctionDescription* fundesc = f.a->fundesc;
    Approx approx_res = f.a->res;
    if (uargs.size() == 1 && is_makeblock(uargs[0]) &&
        static_cast<long>(static_cast<const Uprim*>(uargs[0])->args.size()) == -fundesc->fun_arity) {
      ulambda app = direct_apply(env, fundesc, ufunct, static_cast<const Uprim*>(uargs[0])->args, loc, attribute);
      return {app, strengthen_approx(app, approx_res)};
    }
    if (nargs == fundesc->fun_arity) {
      ulambda app = direct_apply(env, fundesc, ufunct, sl(uargs), loc, attribute);
      return {app, strengthen_approx(app, approx_res)};
    }
    if (nargs < fundesc->fun_arity) {
      Approx fapprox = f.a;
      std::vector<std::pair<Var, ulambda>> first_args;
      for (ulambda arg : uargs) first_args.push_back({Ident::create_local(OCAML_LIT("arg")), arg});
      std::vector<Var> final_args;
      for (long k = 0; k < fundesc->fun_arity - nargs; ++k) final_args.push_back(Ident::create_local(OCAML_LIT("arg")));
      std::vector<L::lambda> internal_args;
      for (auto& [arg1, _] : first_args) internal_args.push_back(L::lvar(arg1));
      for (Var arg : final_args) internal_args.push_back(L::lvar(arg));
      Var funct_var = Ident::create_local(OCAML_LIT("funct"));
      CEnv env2{env.cenv, env.fenv.add(funct_var, fapprox), env.mutable_vars};
      std::vector<L::Param> params;
      for (Var v : final_args) params.push_back({v, pgenval()});
      L::LambdaApply inner;
      inner.ap_loc = loc;
      inner.ap_func = L::lvar(funct_var);
      inner.ap_args = sl(internal_args);
      inner.ap_tailcall = L::TailcallAttribute::Default_tailcall;
      inner.ap_inlined = L::InlineAttribute{};
      inner.ap_specialised = L::SpecialiseAttribute::Default_specialise;
      UA nf = close(env2, L::lfunction(L::FunctionKind::Curried, sl(params), pgenval(), L::lapply(inner),
                                       L::default_function_attribute(), loc));
      ulambda new_fun = ulet(MutableFlag::Immutable, pgenval(), vp(funct_var), ufunct, nf.u);
      for (auto& [arg1, arg2] : first_args) new_fun = ulet(MutableFlag::Immutable, pgenval(), vp(arg1), arg2, new_fun);
      warning_if_forced_inline(loc, attribute, "Partial application");
      return {new_fun, nf.a};
    }
    if (fundesc->fun_arity > 0 && nargs > fundesc->fun_arity) {
      std::vector<std::pair<Var, ulambda>> args;
      for (ulambda arg : uargs) args.push_back({Ident::create_local(OCAML_LIT("arg")), arg});
      std::vector<ulambda> first_args, rem_args;
      for (std::size_t k = 0; k < args.size(); ++k)
        (static_cast<long>(k) < fundesc->fun_arity ? first_args : rem_args).push_back(uvar(args[k].first));
      debuginfo::t dbg = debuginfo::from_location(loc);
      warning_if_forced_inline(loc, attribute, "Over-application");
      ulambda body = ugeneric_apply(direct_apply(env, fundesc, ufunct, sl(first_args), loc, attribute),
                                    sl(rem_args), dbg);
      for (auto& [id, defining_expr] : args)
        body = ulet(MutableFlag::Immutable, pgenval(), vp(id), defining_expr, body);
      return {body, value_unknown()};
    }
  }
  debuginfo::t dbg = debuginfo::from_location(loc);
  warning_if_forced_inline(loc, attribute, "Unknown function");
  return {bind_args_right_to_left(env, sl(uargs),
                                  [&](Slice<ulambda> a2) { return ugeneric_apply(ufunct, a2, dbg); }),
          value_unknown()};
}

UA close(const CEnv& env, L::lambda lam) {
  switch (lam->kind) {
    case L::LK::Lvar: return close_approx_var(env, static_cast<const L::Lvar*>(lam)->id);
    case L::LK::Lmutvar: return {uvar(static_cast<const L::Lmutvar*>(lam)->id), value_unknown()};
    case L::LK::Lconst: return make_const(transl_const(static_cast<const L::Lconst*>(lam)->c));
    case L::LK::Lfunction:
      return close_one_function(env, Ident::create_local(OCAML_LIT("fun")), static_cast<const L::Lfunction*>(lam)->f);
    // We convert [f a] to [let a' = a in let f' = f in fun b c -> f' a' b c]
    // when fun_arity > nargs
    case L::LK::Lapply: return close_apply(env, static_cast<const L::Lapply*>(lam));
    case L::LK::Lsend: {
      auto* x = static_cast<const L::Lsend*>(lam);
      ulambda umet = close(env, x->met).u;
      ulambda uobj = close(env, x->obj).u;
      debuginfo::t dbg = debuginfo::from_location(x->loc);
      return {usend(x->k, umet, uobj, sl(close_list(env, x->args)), dbg), value_unknown()};
    }
    case L::LK::Llet: {
      auto* x = static_cast<const L::Llet*>(lam);
      UA d = x->arg->kind == L::LK::Lfunction
                 ? close_one_function(env, x->id, static_cast<const L::Lfunction*>(x->arg)->f)
                 : close(env, x->arg);
      CEnv env2{env.cenv, env.fenv.add(x->id, d.a), env.mutable_vars};
      if (d.a->kind == AK::Value_const && (x->str == L::LetKind::Alias || is_pure(d.u))) return close(env2, x->body);
      UA b = close(env2, x->body);
      return {ulet(MutableFlag::Immutable, x->k, vp(x->id), d.u, b.u), b.a};
    }
    case L::LK::Lmutlet: {
      auto* x = static_cast<const L::Lmutlet*>(lam);
      ulambda ulam = x->arg->kind == L::LK::Lfunction
                         ? close_one_function(env, x->id, static_cast<const L::Lfunction*>(x->arg)->f).u
                         : close(env, x->arg).u;
      CEnv env2{env.cenv, env.fenv, env.mutable_vars.add(x->id, true)};
      UA b = close(env2, x->body);
      return {ulet(MutableFlag::Mutable, x->k, vp(x->id), ulam, b.u), b.a};
    }
    case L::LK::Lletrec: {
      auto* x = static_cast<const L::Lletrec*>(lam);
      auto [clos, infos] = close_functions(env, x->decl);
      Var clos_ident = Ident::create_local(OCAML_LIT("clos"));
      FEnv fenv_body = env.fenv;
      for (std::size_t k = infos.size(); k-- > 0;) fenv_body = fenv_body.add(infos[k].first, infos[k].second.second);
      UA b = close(CEnv{env.cenv, fenv_body, env.mutable_vars}, x->body);
      VMap sb;
      for (std::size_t k = infos.size(); k-- > 0;)
        sb = sb.add(infos[k].first, uoffset(uvar(clos_ident), infos[k].second.first));
      return {ulet(MutableFlag::Immutable, pgenval(), vp(clos_ident), clos,
                   substitute(debuginfo::none(), clflags::float_const_prop, sb, std::nullopt, b.u)),
              b.a};
    }
    case L::LK::Lprim: return close_prim(env, static_cast<const L::Lprim*>(lam));
    case L::LK::Lswitch: {
      auto* x = static_cast<const L::Lswitch*>(lam);
      const L::LambdaSwitch& sw = x->sw;
      auto fn = [&](L::lambda fail) -> UA {
        ulambda uarg = close(env, x->arg).u;
        SwitchResult c = close_switch(env, sw.sw_consts, sw.sw_numconsts, fail);
        SwitchResult b = close_switch(env, sw.sw_blocks, sw.sw_numblocks, fail);
        USwitch us{sl(c.index), sl(c.actions), sl(b.index), sl(b.actions)};
        ulambda ulam = uswitch(uarg, us, debuginfo::from_location(x->loc));
        return {c.hs(b.hs(ulam)), value_unknown()};
      };
      // NB: failaction might get copied, thus it should be some Lstaticraise
      L::lambda fail = sw.sw_failaction;
      if (!fail || fail->kind == L::LK::Lstaticraise) return fn(fail);
      if ((sw.sw_numconsts - static_cast<long>(sw.sw_consts.size())) +
              (sw.sw_numblocks - static_cast<long>(sw.sw_blocks.size())) >
          1) {
        long i = L::next_raise_count();
        ulambda ubody = fn(L::lstaticraise(i, {})).u;
        ulambda uhandler = close(env, fail).u;
        return {ucatch(i, {}, ubody, uhandler), value_unknown()};
      }
      return fn(fail);
    }
    case L::LK::Lstringswitch: {
      auto* x = static_cast<const L::Lstringswitch*>(lam);
      ulambda uarg = close(env, x->arg).u;
      std::vector<UStringCase> usw;
      for (auto& c : x->cases) usw.push_back({c.s, close(env, c.action).u});
      ulambda ud = x->def ? close(env, x->def).u : nullptr;
      return {ustringswitch(uarg, sl(usw), ud), value_unknown()};
    }
    case L::LK::Lstaticraise: {
      auto* x = static_cast<const L::Lstaticraise*>(lam);
      return {ustaticfail(x->i, sl(close_list(env, x->args))), value_unknown()};
    }
    case L::LK::Lstaticcatch: {
      auto* x = static_cast<const L::Lstaticcatch*>(lam);
      ulambda ubody = close(env, x->body).u;
      ulambda uhandler = close(env, x->handler).u;
      std::vector<UParam> vars;
      for (auto& p : x->params) vars.push_back({vp(p.id), p.kind});
      return {ucatch(x->i, sl(vars), ubody, uhandler), value_unknown()};
    }
    case L::LK::Ltrywith: {
      auto* x = static_cast<const L::Ltrywith*>(lam);
      ulambda ubody = close(env, x->body).u;
      ulambda uhandler = close(env, x->handler).u;
      return {utrywith(ubody, vp(x->exn), uhandler), value_unknown()};
    }
    case L::LK::Lifthenelse: {
      auto* x = static_cast<const L::Lifthenelse*>(lam);
      UA a = close(env, x->cond);
      long n;
      if (const_int(a.a, n)) return sequence_constant_expr(a.u, close(env, n == 0 ? x->ifnot : x->ifso));
      ulambda uifso = close(env, x->ifso).u;
      ulambda uifnot = close(env, x->ifnot).u;
      return {uifthenelse(a.u, uifso, uifnot), value_unknown()};
    }
    case L::LK::Lsequence: {
      auto* x = static_cast<const L::Lsequence*>(lam);
      ulambda ulam1 = close(env, x->l1).u;
      UA b = close(env, x->l2);
      return {usequence(ulam1, b.u), b.a};
    }
    case L::LK::Lwhile: {
      auto* x = static_cast<const L::Lwhile*>(lam);
      ulambda ucond = close(env, x->cond).u;
      ulambda ubody = close(env, x->body).u;
      return {uwhile(ucond, ubody), value_unknown()};
    }
    case L::LK::Lfor: {
      auto* x = static_cast<const L::Lfor*>(lam);
      ulambda ulo = close(env, x->lo).u;
      ulambda uhi = close(env, x->hi).u;
      ulambda ubody = close(env, x->body).u;
      return {ufor(vp(x->id), ulo, uhi, x->dir, ubody), value_unknown()};
    }
    case L::LK::Lassign: {
      auto* x = static_cast<const L::Lassign*>(lam);
      return {uassign(x->id, close(env, x->e).u), value_unknown()};
    }
    case L::LK::Levent: return close(env, static_cast<const L::Levent*>(lam)->l);
    case L::LK::Lifused: fatal_error("Closure.close: Lifused");
  }
  fatal_error("Closure.close");
}

UA close_prim(const CEnv& env, const L::Lprim* x) {
  using LPK = L::Primitive::K;
  const L::Primitive& lp = x->p;
  // Compile-time constants
  if (lp.kind == LPK::Pctconst && x->args.size() == 1) {
    L::lambda arg = x->args[0];
    auto cst = [&](const UA& c) {
      ulambda uarg = close(env, arg).u;
      Var id = Ident::create_local(OCAML_LIT("dummy"));
      return UA{ulet(MutableFlag::Immutable, pgenval(), vp(id), uarg, c.u), c.a};
    };
    using CT = L::CompileTimeConstant;
    switch (lp.ctconst) {
      case CT::Big_endian: return cst(make_const_bool(big_endian));
      case CT::Word_size: return cst(make_const_int(8 * size_int));
      case CT::Int_size: return cst(make_const_int(8 * size_int - 1));
      case CT::Max_wosize: return cst(make_const_int((1L << ((8 * size_int) - 10)) - 1));
      case CT::Ostype_unix: return cst(make_const_bool(std::string_view(target_os_type) == "Unix"));
      case CT::Ostype_win32: return cst(make_const_bool(std::string_view(target_os_type) == "Win32"));
      case CT::Ostype_cygwin: return cst(make_const_bool(std::string_view(target_os_type) == "Cygwin"));
      case CT::Backend_type: return cst(make_const_int(0));  // tag 0 is the same as Native here
      case CT::Standard_library_default: {
        compilenv::need_stdlib_location();
        debuginfo::t dbg = debuginfo::from_location(x->loc);
        std::string_view id = ident::name(compilenv::stdlib_symbol_name());
        P rs{PK::Pread_symbol};
        rs.sym = id;
        return {uprim(rs, {}, dbg), value_const(uconst_ref(id, nullptr))};
      }
    }
  }
  if (lp.kind == LPK::Pignore && x->args.size() == 1) {
    // [make_const_int 0] is inlined when ocamlopt itself is compiled: its
    // [Uconst_int 0] is one statically allocated block, shared by every unit
    // (visible in the .cmx approximations)
    static const UConstant ignore_const = uconst_int(0);
    UA c = make_const(ignore_const);
    return {usequence(close(env, x->args[0]).u, c.u), c.a};
  }
  if ((lp.kind == LPK::Pbytes_to_string || lp.kind == LPK::Pbytes_of_string) && x->args.size() == 1)
    return close(env, x->args[0]);
  if (lp.kind == LPK::Pgetglobal && x->args.empty()) {
    debuginfo::t dbg = debuginfo::from_location(x->loc);
    // right to left
    Approx a = compilenv::global_approx(lp.id);
    return check_constant_result(getglobal(dbg, lp.id), a);
  }
  if (lp.kind == LPK::Pfield && x->args.size() == 1) {
    UA a = close(env, x->args[0]);
    debuginfo::t dbg = debuginfo::from_location(x->loc);
    P fld{PK::Pfield};
    fld.n = lp.n;
    fld.ptr = lp.ptr;
    fld.mut = lp.mut;
    return check_constant_result(uprim(fld, sl(std::vector<ulambda>{a.u}), dbg), field_approx(lp.n, a.a));
  }
  if (lp.kind == LPK::Psetfield && x->args.size() == 2) {
    auto* g = L::as<L::Lprim>(x->args[0]);
    if (g && g->p.kind == LPK::Pgetglobal && g->args.empty()) {
      UA a = close(env, x->args[1]);
      if (a.a->kind != AK::Value_unknown) global_approx[lp.n] = a.a;
      debuginfo::t dbg = debuginfo::from_location(x->loc);
      P sf{PK::Psetfield};
      sf.n = lp.n;
      sf.ptr = lp.ptr;
      sf.init = lp.init;
      return {uprim(sf, sl(std::vector<ulambda>{getglobal(dbg, g->p.id), a.u}), dbg), value_unknown()};
    }
  }
  if (lp.kind == LPK::Praise && x->args.size() == 1) {
    ulambda ulam = close(env, x->args[0]).u;
    debuginfo::t dbg = debuginfo::from_location(x->loc);
    P r{PK::Praise};
    r.raise = lp.raise;
    return {uprim(r, sl(std::vector<ulambda>{ulam}), dbg), value_unknown()};
  }
  if (lp.kind == LPK::Pmakearray && x->args.empty()) {
    // [Uconst_block (0, [])]: a constant block of ocamlopt's own code
    static const UStructuredConstant* empty_block = sc_block(0, {});
    return make_const_ref(empty_block);
  }
  P p = convert_primitives::convert(lp);
  debuginfo::t dbg = debuginfo::from_location(x->loc);
  // close_list_approx: left to right
  std::vector<ulambda> ulams;
  std::vector<Approx> approxs;
  for (L::lambda a : x->args) {
    UA r = close(env, a);
    ulams.push_back(r.u);
    approxs.push_back(r.a);
  }
  return simplif_prim(clflags::float_const_prop, p, sl(ulams), approxs, dbg);
}

std::vector<ulambda> close_list(const CEnv& env, Slice<L::lambda> l) {
  std::vector<ulambda> r;
  for (L::lambda lam : l) r.push_back(close(env, lam).u);
  return r;
}

// Build a shared closure for a set of mutually recursive functions
struct UncurriedDef {
  Ident::t id;
  Slice<L::Param> params;
  L::ValueKind return_;
  L::lambda body;
  FunctionDescription* fundesc;
  debuginfo::t dbg;
};

std::pair<ulambda, std::vector<std::pair<Ident::t, std::pair<long, Approx>>>> close_functions(
    const CEnv& env, Slice<L::RecBinding> fun_defs0) {
  // Split functions with optional arguments and default values into a
  // wrapper function (likely to be inlined) and an inner function (never
  // inlined).
  //
  // However, if the user forces inlining of the function, this is
  // counterproductive; we want the whole function to be inlined, not just
  // the wrapper. So we disable the split when inlining is forced. Cf #12526
  std::vector<L::RecBinding> fun_defs;
  if (fun_defs0.size() == 1 && fun_defs0[0].def->attr.inline_.kind == L::InlineAttribute::Kind::Always_inline) {
    fun_defs = vec(fun_defs0);
  } else {
    for (const L::RecBinding& b : fun_defs0) {
      const L::LFunction* f = b.def;
      for (const L::RecBinding& s :
           simplif::split_default_wrapper(b.id, f->kind, f->params, f->return_, f->body, f->attr, f->loc))
        fun_defs.push_back(s);
    }
  }
  L::InlineAttribute inline_attribute =
      fun_defs.size() == 1 ? fun_defs[0].def->attr.inline_
                           : L::InlineAttribute{};  // Default_inline: recursive functions can't be inlined
  // Update and check nesting depth
  ++function_nesting_depth;
  bool initially_closed = function_nesting_depth < excessive_function_nesting_depth;
  // Determine the free variables of the functions
  L::IdentSet fvset = L::free_variables(L::lletrec(sl(fun_defs), L::lambda_unit()));
  std::vector<Ident::t> fv(fvset.begin(), fvset.end());
  // Build the function descriptors for the functions.  Initially all
  // functions are assumed not to need their environment parameter.
  std::vector<UncurriedDef> uncurried_defs;
  for (const L::RecBinding& b : fun_defs) {
    const L::LFunction* f = b.def;
    std::string_view label = compilenv::make_symbol(std::string_view(zstr(ident::unique_name(b.id))));
    long arity = static_cast<long>(f->params.size());
    auto* fundesc = make<FunctionDescription>();
    fundesc->fun_label = label;
    fundesc->fun_arity = f->kind == L::FunctionKind::Tupled ? -arity : arity;
    fundesc->fun_closed = initially_closed;
    fundesc->has_inline = false;
    fundesc->fun_float_const_prop = clflags::float_const_prop;
    fundesc->fun_poll = f->attr.poll;
    uncurried_defs.push_back({b.id, f->params, f->return_, f->body, fundesc, debuginfo::from_location(f->loc)});
  }
  // Build an approximate fenv for compiling the functions
  FEnv fenv_rec = env.fenv;
  for (std::size_t k = uncurried_defs.size(); k-- > 0;)
    fenv_rec = fenv_rec.add(uncurried_defs[k].id, value_closure(uncurried_defs[k].fundesc, value_unknown()));
  // Determine the offsets of each function's closure in the shared block
  long env_pos = -1;
  std::vector<long> clos_offsets;
  for (const UncurriedDef& d : uncurried_defs) {
    long pos = env_pos + 1;
    env_pos = env_pos + 1 + (d.fundesc->fun_arity != 1 ? 3 : 2);
    clos_offsets.push_back(pos);
  }
  long fv_pos = env_pos;
  // This reference will be set to false if the hypothesis that a function
  // does not use its environment parameter is invalidated.
  bool useless_env = initially_closed;
  Entries cenv_entries;
  for (std::size_t k = fv.size(); k-- > 0;)
    cenv_entries = cenv_entries.add(fv[k], ClosureEntry{false, fv_pos + static_cast<long>(k)});
  for (std::size_t k = uncurried_defs.size(); k-- > 0;)
    cenv_entries = cenv_entries.add(uncurried_defs[k].id, ClosureEntry{true, clos_offsets[k]});
  // Translate each function definition
  auto clos_fundef = [&](const UncurriedDef& d, long fenv_pos) {
    Var env_param = Ident::create_local(OCAML_LIT("env"));
    ClosureEnv cenv_body{true, cenv_entries, env_param, fenv_pos};
    UA b = close(CEnv{cenv_body, fenv_rec, env.mutable_vars}, d.body);
    if (useless_env && occurs(env_param, b.u)) throw NotClosed{};
    std::vector<L::Param> fun_params = vec(d.params);
    if (!useless_env) fun_params.push_back({env_param, pgenval()});
    auto* f = make<UFunction>();
    f->label = d.fundesc->fun_label;
    f->arity = d.fundesc->fun_arity;
    std::vector<UParam> ps;
    for (auto& p : fun_params) ps.push_back({vp(p.id), p.kind});
    f->params = sl(ps);
    f->return_ = d.return_;
    f->body = b.u;
    f->dbg = d.dbg;
    f->env = env_param;
    f->poll = d.fundesc->fun_poll;
    // give more chance of function with default parameters (i.e. their
    // wrapper functions) to be inlined
    long n = 0;
    for (auto& p : fun_params) n += ident::name(p.id) == "*opt*" ? 8 : 1;
    long threshold;
    switch (inline_attribute.kind) {
      case L::InlineAttribute::Kind::Default_inline: {
        double inline_threshold = arg_helper::get(0, clflags::inline_threshold);
        double magic_scale_constant = 8.;
        threshold = int_of_float(inline_threshold * magic_scale_constant) + n;
        break;
      }
      case L::InlineAttribute::Kind::Always_inline:
      case L::InlineAttribute::Kind::Hint_inline: threshold = switch_::ocaml_max_int; break;
      case L::InlineAttribute::Kind::Never_inline: threshold = switch_::ocaml_min_int; break;
      default: fatal_error("Closure.close_functions: Unroll");
    }
    std::vector<VarWithProvenance> inline_params;
    for (auto& p : fun_params) inline_params.push_back(vp(p.id));
    if (lambda_smaller(b.u, threshold)) {
      d.fundesc->has_inline = true;
      d.fundesc->inline_params = sl(inline_params);
      d.fundesc->inline_body = b.u;
    }
    return std::pair<const UFunction*, std::pair<Ident::t, std::pair<long, Approx>>>{
        f, {d.id, {fenv_pos, value_closure(d.fundesc, b.a)}}};
  };
  // Translate all function definitions.
  using Info = std::pair<const UFunction*, std::pair<Ident::t, std::pair<long, Approx>>>;
  auto map2 = [&]() {
    std::vector<Info> r;
    for (std::size_t k = 0; k < uncurried_defs.size(); ++k) r.push_back(clos_fundef(uncurried_defs[k], clos_offsets[k]));
    return r;
  };
  std::vector<Info> clos_info_list;
  if (initially_closed) {
    compilenv::Snapshot snap = compilenv::snapshot();
    try {
      clos_info_list = map2();
    } catch (const NotClosed&) {
      // If the hypothesis that the environment parameters are useless has
      // been invalidated, then set [fun_closed] to false in all
      // descriptions and recompile
      compilenv::backtrack(snap);  // PR#6337
      for (const UncurriedDef& d : uncurried_defs) {
        d.fundesc->fun_closed = false;
        d.fundesc->has_inline = false;
        d.fundesc->inline_params = {};
        d.fundesc->inline_body = nullptr;
      }
      useless_env = false;
      clos_info_list = map2();
    }
  } else {
    // Excessive closure nesting: assume environment parameter is used
    clos_info_list = map2();
  }
  // Update nesting depth
  --function_nesting_depth;
  // Return the Uclosure node and the list of all identifiers defined, with
  // offsets and approximations.
  std::vector<const UFunction*> clos;
  std::vector<std::pair<Ident::t, std::pair<long, Approx>>> infos;
  for (auto& [f, info] : clos_info_list) {
    clos.push_back(f);
    infos.push_back(info);
  }
  std::vector<ulambda> fvs;
  if (!useless_env)
    for (Ident::t id : fv) fvs.push_back(close_var(env, id));
  return {uclosure(sl(clos), sl(fvs)), infos};
}

// Same, for one non-recursive function
UA close_one_function(const CEnv& env, Ident::t id, const L::LFunction* funct) {
  auto [clos, infos] = close_functions(env, sl(std::vector<L::RecBinding>{{id, funct}}));
  if (!infos.empty() && ident::same(id, infos[0].first)) return {clos, infos[0].second.second};
  fatal_error("Closure.close_one_function");
}

// Close a switch
struct StoredLambda {
  using t = L::lambda;
  using key = L::lambda;
  static std::optional<key> make_key(const t& l) { return L::make_key(l); }
  static bool same_key(const key& a, const key& b) { return L::equal_lambda(a, b); }
};
using Storer = switch_::Store<StoredLambda>;

SwitchResult close_switch(const CEnv& env, Slice<L::SwitchCase> cases, long num_keys, L::lambda def) {
  long ncases = static_cast<long>(cases.size());
  std::vector<long> index(static_cast<std::size_t>(num_keys), 0);
  Storer store;
  // First default case
  if (def && ncases < num_keys)
    if (store.act_store(def) != 0) fatal_error("Closure.close_switch");
  // Then all other cases
  for (const L::SwitchCase& c : cases) index[static_cast<std::size_t>(c.key)] = store.act_store(c.action);
  // Explicit sharing with catch/exit, as switcher compilation may later
  // unshare
  auto acts = store.act_get_shared();
  std::function<ulambda(ulambda)> hs = [](ulambda e) { return e; };
  // Compile actions
  std::vector<ulambda> actions;
  for (auto& a : acts) {
    auto* sr = L::as<L::Lstaticraise>(a.act);
    if (!a.shared || (sr && sr->args.empty())) {
      actions.push_back(close(env, a.act).u);
    } else {
      ulambda ulam = close(env, a.act).u;
      long i = L::next_raise_count();
      std::function<ulambda(ulambda)> ohs = hs;
      hs = [ohs, i, ulam](ulambda e) { return ucatch(i, {}, ohs(e), ulam); };
      actions.push_back(ustaticfail(i, {}));
    }
  }
  if (actions.empty()) return {{}, {}, hs};  // May happen when default is None
  return {index, actions, hs};
}

// Collect exported symbols for structured constants
struct Collect {
  void approx(Approx a) {
    switch (a->kind) {
      case AK::Value_closure:
        approx(a->res);
        if (a->fundesc->has_inline) ulam(a->fundesc->inline_body);
        return;
      case AK::Value_tuple:
        for (Approx x : a->tuple) approx(x);
        return;
      case AK::Value_const: constant(a->c); return;
      default: return;
    }
  }
  void constant(const UConstant& c) {
    if (c.kind != UConstant::Kind::Uconst_ref) return;
    if (c.sc) {
      compilenv::add_exported_constant(c.sym);
      structured_constant(c.sc);
    } else if (c.sym != ident::name(compilenv::stdlib_symbol_name())) {
      // Only generated in one context
      fatal_error("Closure.collect_exported_structured_constants");
    }
  }
  void structured_constant(const UStructuredConstant* c) {
    if (c->kind == SCK::Uconst_block)
      for (const UConstant& u : c->fields) constant(u);
    else if (c->kind == SCK::Uconst_closure)
      fatal_error("Closure.collect_exported_structured_constants");  // Cannot be generated
  }
  void list(Slice<ulambda> l) {
    for (ulambda u : l) ulam(u);
  }
  void ulam(ulambda u) {
    switch (u->kind) {
      case UK::Uvar: return;
      case UK::Uconst: constant(static_cast<const Uconst*>(u)->c); return;
      case UK::Udirect_apply: list(static_cast<const Udirect_apply*>(u)->args); return;
      case UK::Ugeneric_apply: {
        auto* x = static_cast<const Ugeneric_apply*>(u);
        ulam(x->f);
        list(x->args);
        return;
      }
      case UK::Uclosure: {
        auto* x = static_cast<const Uclosure*>(u);
        for (const UFunction* f : x->funs) ulam(f->body);
        list(x->fv);
        return;
      }
      case UK::Uoffset: ulam(static_cast<const Uoffset*>(u)->l); return;
      case UK::Ulet: {
        auto* x = static_cast<const Ulet*>(u);
        ulam(x->arg);
        ulam(x->body);
        return;
      }
      case UK::Uphantom_let: no_phantom_lets();
      case UK::Uprim: list(static_cast<const Uprim*>(u)->args); return;
      case UK::Uswitch: {
        auto* x = static_cast<const Uswitch*>(u);
        ulam(x->arg);
        list(x->sw.us_actions_consts);
        list(x->sw.us_actions_blocks);
        return;
      }
      case UK::Ustringswitch: {
        auto* x = static_cast<const Ustringswitch*>(u);
        ulam(x->arg);
        for (auto& c : x->cases) ulam(c.action);
        if (x->def) ulam(x->def);
        return;
      }
      case UK::Ustaticfail: list(static_cast<const Ustaticfail*>(u)->args); return;
      case UK::Ucatch: {
        auto* x = static_cast<const Ucatch*>(u);
        ulam(x->body);
        ulam(x->handler);
        return;
      }
      case UK::Utrywith: {
        auto* x = static_cast<const Utrywith*>(u);
        ulam(x->body);
        ulam(x->handler);
        return;
      }
      case UK::Usequence: {
        auto* x = static_cast<const Usequence*>(u);
        ulam(x->l1);
        ulam(x->l2);
        return;
      }
      case UK::Uwhile: {
        auto* x = static_cast<const Uwhile*>(u);
        ulam(x->cond);
        ulam(x->body);
        return;
      }
      case UK::Uifthenelse: {
        auto* x = static_cast<const Uifthenelse*>(u);
        ulam(x->cond);
        ulam(x->ifso);
        ulam(x->ifnot);
        return;
      }
      case UK::Ufor: {
        auto* x = static_cast<const Ufor*>(u);
        ulam(x->lo);
        ulam(x->hi);
        ulam(x->body);
        return;
      }
      case UK::Uassign: ulam(static_cast<const Uassign*>(u)->e); return;
      case UK::Usend: {
        auto* x = static_cast<const Usend*>(u);
        ulam(x->met);
        ulam(x->obj);
        list(x->args);
        return;
      }
      case UK::Uunreachable: return;
    }
  }
};

}  // namespace

void reset() {
  global_approx = nullptr;
  global_approx_size = 0;
  function_nesting_depth = 0;
}

// The entry point
ulambda intro(long size, L::lambda lam) {
  reset();
  std::string_view id = compilenv::make_symbol(std::nullopt);
  std::vector<Approx> ga;
  for (long i = 0; i < size; ++i) ga.push_back(value_global_field(id, i));
  Approx tuple = value_tuple(ga);
  global_approx = const_cast<Approx*>(tuple->tuple.p);
  global_approx_size = size;
  compilenv::set_global_approx(tuple);
  UA r = close(CEnv{ClosureEnv{}, FEnv{}, VSet{}}, lam);
  bool opaque = clflags::opaque || env::is_imported_opaque(std::string(compilenv::current_unit_name()));
  if (opaque)
    compilenv::set_global_approx(value_unknown());
  else
    Collect{}.approx(tuple);
  global_approx = nullptr;
  global_approx_size = 0;
  return r.u;
}

}  // namespace cppcaml::typing::closure

namespace cppcaml::typing::closure_middle_end {

using namespace clambda;

WithConstants lambda_to_clambda(format::Formatter& ppf, const lambda::Program& program, lambda::lambda code) {
  ulambda clambda = closure::intro(program.main_module_block_size, code);
  auto* provenance = make<USymbolProvenance>(
      USymbolProvenance{{}, Path::pident(Ident::create_persistent(compilenv::current_unit_name()))});
  PreallocatedBlock preallocated_block;
  preallocated_block.symbol = compilenv::make_symbol(std::nullopt);
  preallocated_block.exported = true;
  preallocated_block.tag = 0;
  preallocated_block.fields = slice(std::vector<std::optional<UConstantBlockField>>(
      static_cast<std::size_t>(program.main_module_block_size), std::nullopt));
  preallocated_block.provenance = provenance;
  std::vector<PreallocatedConstant> constants = compilenv::structured_constants();
  compilenv::clear_structured_constants();
  WithConstants r{clambda, {preallocated_block}, constants};
  // raw_clambda_dump_if
  if (clflags::dump_rawclambda || clflags::dump_clambda) {
    format::fprintf(ppf, "@.clambda:@.");
    printclambda::clambda(ppf, clambda);
    for (const PreallocatedConstant& c : constants) {
      format::fprintf(ppf, "%s:@ ", c.symbol);
      printclambda::print_structured_constant(ppf, c.definition);
      format::fprintf(ppf, "@.");
    }
  }
  if (clflags::dump_cmm) format::fprintf(ppf, "@.cmm:@.");
  return r;
}

}  // namespace cppcaml::typing::closure_middle_end
