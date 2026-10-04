// Port of asmcomp/cmmgen.ml.  See cmmgen.hpp.
//
// OCaml evaluates a constructor's, a list literal's and an application's
// arguments right to left: [add_int_caml (transl env arg1) (transl env
// arg2)] translates arg2 first.  Translation has effects (fresh variables,
// raise counts, constant symbols, lifted functions), so the port evaluates
// in that order; "right to left" marks the places.
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/cmmgen.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"
#include "cppcaml/typing/compilenv.hpp"

namespace cppcaml::typing::cmmgen {

using namespace cmm;
using namespace cmm_helpers;
using clambda::ulambda;
using clambda::UK;
using PK = clambda::Primitive::K;
using OK = Operation::K;
using MC = MemoryChunk;
using SCK = clambda::UStructuredConstant::Kind;
namespace L = lambda;
namespace CL = clambda;

namespace {

[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }

struct IdentCmp {
  int operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b); }
};
struct LongCmp {
  int operator()(long a, long b) const { return a < b ? -1 : a > b ? 1 : 0; }
};

// ---- Environments used for translation to Cmm ------------------------------------------------
struct BoxedNumber {  // Boxed_float of Debuginfo.t | Boxed_integer of boxed_integer * Debuginfo.t
  bool is_float;
  BoxedInteger bi = BoxedInteger::Pnativeint;
  Dbg dbg;
};

using NotifyFn = std::shared_ptr<const std::function<void(const std::vector<expression>&)>>;
struct Env {
  PMap<Ident::t, std::pair<Var, BoxedNumber>, IdentCmp> unboxed_ids;
  PMap<Ident::t, bool, IdentCmp> mutable_ids;
  PMap<long, NotifyFn, LongCmp> notify_catch;
  Var environment_param = nullptr;  // option
};

Env create_env(Var environment_param) {
  Env e;
  e.environment_param = environment_param;
  return e;
}

const std::pair<Var, BoxedNumber>* is_unboxed_id(Var id, const Env& env) { return env.unboxed_ids.find_opt(id); }
Env add_unboxed_id(Var id, Var unboxed_id, const BoxedNumber& bn, const Env& env) {
  Env e = env;
  e.unboxed_ids = env.unboxed_ids.add(id, {unboxed_id, bn});
  return e;
}
bool is_mutable_id(Var id, const Env& env) { return env.mutable_ids.mem(id); }
Env add_mutable_id(Var id, const Env& env) {
  Env e = env;
  e.mutable_ids = env.mutable_ids.add(id, true);
  return e;
}
Env add_notify_catch(long n, NotifyFn f, const Env& env) {
  Env e = env;
  e.notify_catch = env.notify_catch.add(n, std::move(f));
  return e;
}
void notify_catch(long i, const Env& env, const std::vector<expression>& l) {
  if (const NotifyFn* f = env.notify_catch.find_opt(i)) (**f)(l);
}

// Description of the "then" and "else" continuations in [transl_if]. If the
// "then" continuation is true and the "else" continuation is false then we
// can use the condition directly as the result. Similarly, if the "then"
// continuation is false and the "else" continuation is true then we can use
// the negation of the condition directly as the result.
enum class ThenElse { Then_true_else_false, Then_false_else_true, Unknown };
ThenElse invert_then_else(ThenElse t) {
  switch (t) {
    case ThenElse::Then_true_else_false: return ThenElse::Then_false_else_true;
    case ThenElse::Then_false_else_true: return ThenElse::Then_true_else_false;
    default: return ThenElse::Unknown;
  }
}

MutableFlag mut_from_env(const Env& env, expression ptr) {
  if (!env.environment_param) return MutableFlag::Mutable;
  // Loads from the current function's closure are immutable.
  if (auto* v = as<Cvar>(ptr); v && ident::same(env.environment_param, v->id)) return MutableFlag::Immutable;
  return MutableFlag::Mutable;
}

// Minimum of two [mutable_flag] values, assuming [Immutable < Mutable].
MutableFlag min_mut(MutableFlag x, MutableFlag y) {
  return x == MutableFlag::Immutable || y == MutableFlag::Immutable ? MutableFlag::Immutable : MutableFlag::Mutable;
}

expression get_field(const Env& env, L::ImmediateOrPointer imm_or_pointer, MutableFlag mut, expression ptr, long n,
                     const Dbg& dbg) {
  MutableFlag m = min_mut(mut, mut_from_env(env, ptr));
  MC memory_chunk = imm_or_pointer == L::ImmediateOrPointer::Immediate ? MC::Word_int : MC::Word_val;
  return get_field_gen(m, ptr, n, dbg, memory_chunk);
}

// Translate structured constants to Cmm data items
expression transl_constant(const Dbg& dbg, const CL::UConstant& c) {
  if (c.kind == CL::UConstant::Kind::Uconst_int) return int_const(dbg, c.i);
  if (c.sc) cmmgen_state::add_structured_constant(c.sym, c.sc);
  return cconst_symbol(c.sym, dbg);
}

// emit_constant cst cont
void emit_constant(const CL::UConstant& c, std::vector<DataItem>& out) {
  if (c.kind == CL::UConstant::Kind::Uconst_int) out.push_back(cint_const(c.i));
  else out.push_back(data_sym(DataItem::K::Csymbol_address, c.sym));
}

std::vector<DataItem> emit_structured_constant(const Symb& symb, const CL::UStructuredConstant* cst,
                                               std::vector<DataItem> cont) {
  switch (cst->kind) {
    case SCK::Uconst_float: return emit_float_constant(symb, cst->f, std::move(cont));
    case SCK::Uconst_string: return emit_string_constant(symb, cst->s, std::move(cont));
    case SCK::Uconst_int32: return emit_int32_constant(symb, cst->i, std::move(cont));
    case SCK::Uconst_int64: return emit_int64_constant(symb, cst->i, std::move(cont));
    case SCK::Uconst_nativeint: return emit_nativeint_constant(symb, cst->i, std::move(cont));
    case SCK::Uconst_block: {
      std::vector<DataItem> fields;  // List.fold_right emit_constant csts cont
      for (const CL::UConstant& c : cst->fields) emit_constant(c, fields);
      fields.insert(fields.end(), cont.begin(), cont.end());
      return emit_block(symb, block_header(cst->tag, static_cast<long>(cst->fields.size())), fields);
    }
    case SCK::Uconst_float_array: return emit_float_array_constant(symb, cst->floats, std::move(cont));
    case SCK::Uconst_closure: {
      cmmgen_state::add_constant(cst->s, cmmgen_state::Constant{true, symb.second, cst->funs, cst->fields, {}});
      for (const CL::UFunction* f : cst->funs) cmmgen_state::add_function(f);
      return cont;
    }
  }
  return cont;
}

// Boxed integers
std::vector<DataItem> box_int_constant(std::string_view sym, BoxedInteger bi, std::int64_t n) {
  Symb s{sym, cmmgen_state::IsGlobal::Local};
  switch (bi) {
    case BoxedInteger::Pnativeint: return emit_nativeint_constant(s, n, {});
    case BoxedInteger::Pint32:
      return emit_int32_constant(s, static_cast<std::int32_t>(static_cast<std::uint32_t>(n)), {});
    default: return emit_int64_constant(s, n, {});
  }
}

expression box_int(const Dbg& dbg, BoxedInteger bi, expression arg) {
  if (auto* c = as<Cconst_int>(arg)) {
    std::string_view sym = compilenv::new_const_symbol();
    cmmgen_state::add_data_items(box_int_constant(sym, bi, c->n));
    return cconst_symbol(sym, dbg);
  }
  if (auto* c = as<Cconst_natint>(arg)) {
    std::string_view sym = compilenv::new_const_symbol();
    cmmgen_state::add_data_items(box_int_constant(sym, bi, c->n));
    return cconst_symbol(sym, dbg);
  }
  return box_int_gen(dbg, bi, arg);
}

// Boxed numbers
Machtype typ_of_boxed_number(const BoxedNumber& bn) { return bn.is_float ? typ_float() : typ_int(); }

bool equal_boxed_number(const BoxedNumber& a, const BoxedNumber& b) {
  if (a.is_float || b.is_float) return a.is_float && b.is_float;
  return a.bi == b.bi;
}

expression box_number(const BoxedNumber& bn, expression arg) {
  if (bn.is_float) return box_float(bn.dbg, arg);
  return box_int(bn.dbg, bn.bi, arg);
}

// Returns the unboxed representation of a boxed float or integer.  For
// Pint32 on 64-bit archs, the high 32 bits of the result are undefined.
expression unbox_number(const Dbg& dbg, const BoxedNumber& bn, expression arg) {
  if (bn.is_float) return unbox_float(bn.dbg, arg);
  if (bn.bi == BoxedInteger::Pint32) return low_32(dbg, unbox_int(dbg, BoxedInteger::Pint32, arg));
  return unbox_int(dbg, bn.bi, arg);
}

// Auxiliary functions for optimizing "let" of boxed numbers (floats and
// boxed integers
struct UnboxedNumberKind {  // No_unboxing | Boxed of boxed_number * bool | No_result
  enum class K { No_unboxing, Boxed, No_result } k;
  BoxedNumber bn{};
  bool free = false;  // true: boxed form available at no cost
};
UnboxedNumberKind no_unboxing() { return {UnboxedNumberKind::K::No_unboxing}; }
UnboxedNumberKind boxed(const BoxedNumber& bn, bool free) { return {UnboxedNumberKind::K::Boxed, bn, free}; }

// A value kind [vk] is compatible with a boxed-number kind [bk] if the
// boxing operation [bk] returns a value that may live in the value kind
// [vk].
bool compatible_kind(const L::ValueKind& vk, const UnboxedNumberKind& bk) {
  if (bk.k != UnboxedNumberKind::K::Boxed) return true;
  using VK = L::ValueKind::Kind;
  switch (vk.kind) {
    case VK::Pgenval: return true;
    case VK::Pintval: return false;
    case VK::Pfloatval: return bk.bn.is_float;
    case VK::Pboxedintval: return !bk.bn.is_float && bk.bn.bi == vk.bi;
  }
  return false;
}

// Given unboxed_number_kind from two branches of the code, returns the
// resulting unboxed_number_kind.
//
// If [strict=false], one knows that the type of the expression is an
// unboxable number, and we decide to return an unboxed value if this
// indeed eliminates at least one allocation.
//
// If [strict=true], we need to ensure that all possible branches return an
// unboxable number (of the same kind).  This could not be the case in
// presence of GADTs.
UnboxedNumberKind join_unboxed_number_kind(bool strict, const UnboxedNumberKind& k1, const UnboxedNumberKind& k2) {
  using K = UnboxedNumberKind::K;
  if (k1.k == K::Boxed && k2.k == K::Boxed && equal_boxed_number(k1.bn, k2.bn))
    return boxed(k1.bn, k1.free && k2.free);
  if (k1.k == K::No_result) return k2;  // if a branch never returns, it is safe to unbox it
  if (k2.k == K::No_result) return k1;
  if (!strict && k1.k == K::No_unboxing) return k2;
  if (!strict && k2.k == K::No_unboxing) return k1;
  return no_unboxing();
}

// [is_unboxed_number_cmm ~strict ~kind cmm] computes an unboxed number kind
// for the value returned by the expression [cmm].
UnboxedNumberKind is_unboxed_number_cmm(bool strict, const L::ValueKind& kind, expression cmm) {
  UnboxedNumberKind r{UnboxedNumberKind::K::No_result};
  auto notify = [&](const UnboxedNumberKind& k) {
    if (compatible_kind(kind, k)) r = join_unboxed_number_kind(strict, r, k);
  };
  std::function<void(expression)> aux = [&](expression e) {
    if (auto* a = as<Cop>(e); a && a->op.kind == OK::Calloc) {
      auto* hdr = a->args.size() >= 1 ? as<Cconst_natint>(a->args[0]) : nullptr;
      if (a->args.size() == 2 && hdr && hdr->n == float_header) {
        notify(boxed(BoxedNumber{true, BoxedInteger::Pnativeint, a->dbg}, false));
        return;
      }
      auto* ops = a->args.size() == 3 ? as<Cconst_symbol>(a->args[1]) : nullptr;
      if (hdr && ops) {
        if (hdr->n == boxedintnat_header && ops->s == caml_nativeint_ops)
          notify(boxed(BoxedNumber{false, BoxedInteger::Pnativeint, a->dbg}, false));
        else if (hdr->n == boxedint32_header && ops->s == caml_int32_ops)
          notify(boxed(BoxedNumber{false, BoxedInteger::Pint32, a->dbg}, false));
        else if (hdr->n == boxedint64_header && ops->s == caml_int64_ops)
          notify(boxed(BoxedNumber{false, BoxedInteger::Pint64, a->dbg}, false));
        else
          notify(no_unboxing());
        return;
      }
    }
    if (auto* s = as<Cconst_symbol>(e)) {
      const CL::UStructuredConstant* c = cmmgen_state::structured_constant_of_sym(s->s);
      if (c && c->kind == SCK::Uconst_float) notify(boxed(BoxedNumber{true, BoxedInteger::Pnativeint, {}}, true));
      else if (c && c->kind == SCK::Uconst_nativeint)
        notify(boxed(BoxedNumber{false, BoxedInteger::Pnativeint, {}}, true));
      else if (c && c->kind == SCK::Uconst_int32) notify(boxed(BoxedNumber{false, BoxedInteger::Pint32, {}}, true));
      else if (c && c->kind == SCK::Uconst_int64) notify(boxed(BoxedNumber{false, BoxedInteger::Pint64, {}}, true));
      else notify(no_unboxing());
      return;
    }
    if (!iter_shallow_tail(aux, e)) notify(no_unboxing());
  };
  aux(cmm);
  return r;
}

Machtype machtype_of_value_kind(const L::ValueKind& k) {
  return k.kind == L::ValueKind::Kind::Pintval ? typ_int() : typ_val();
}

std::vector<CatchParam> catch_params(Slice<CL::UParam> ps) {
  std::vector<CatchParam> r;
  for (auto& p : ps) r.push_back({p.var, machtype_of_value_kind(p.kind)});
  return r;
}

// ---- Translate an expression -----------------------------------------------------------------
expression transl(const Env& env, ulambda e);
std::vector<expression> transl_list(const Env& env, Slice<ulambda> l) {
  std::vector<expression> r;  // List.map: left to right
  for (ulambda u : l) r.push_back(transl(env, u));
  return r;
}
expression transl_prim_1(const Env& env, const CL::Primitive& p, ulambda arg, const Dbg& dbg);
expression transl_prim_2(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, const Dbg& dbg);
expression transl_prim_3(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, ulambda arg3,
                         const Dbg& dbg);
expression transl_prim_4(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, ulambda arg3,
                         ulambda arg4, const Dbg& dbg);
expression transl_catch(const Env& env, long nfail, Slice<CL::UParam> ids, ulambda body, ulambda handler,
                        const Dbg& dbg);
expression transl_make_array(const Dbg& dbg, const Env& env, L::ArrayKind kind, Slice<ulambda> args);
expression transl_ccall(const Env& env, const PrimitiveDescription* prim, Slice<ulambda> args, const Dbg& dbg);
expression transl_unbox_float(const Dbg& dbg, const Env& env, ulambda exp);
expression transl_unbox_int(const Dbg& dbg, const Env& env, BoxedInteger bi, ulambda exp);
expression transl_unbox_int_low(const Dbg& dbg, const Env& env, BoxedInteger bi, ulambda e);
expression transl_unbox_sized(CL::MemoryAccessSize size, const Dbg& dbg, const Env& env, ulambda exp);
expression transl_let(const Env& env, MutableFlag str, const L::ValueKind& kind, const VarWithProvenance& id,
                      ulambda exp, const std::function<expression(const Env&)>& transl_body);
expression make_catch(long ncatch, expression body, expression handler, const Dbg& dbg);
expression transl_if(const Env& env, ThenElse approx, const Dbg& dbg, ulambda cond, const Dbg& then_dbg,
                     expression then_, const Dbg& else_dbg, expression else_);
expression transl_sequand(const Env& env, ThenElse approx, const Dbg& arg1_dbg, ulambda arg1, const Dbg& arg2_dbg,
                          ulambda arg2, const Dbg& then_dbg, expression then_, const Dbg& else_dbg,
                          expression else_);
expression transl_sequor(const Env& env, ThenElse approx, const Dbg& arg1_dbg, ulambda arg1, const Dbg& arg2_dbg,
                         ulambda arg2, const Dbg& then_dbg, expression then_, const Dbg& else_dbg, expression else_);
expression transl_switch(const Dbg& dbg, const Env& env, expression arg, Slice<long> index, Slice<ulambda> cases);

bool uconst_int_is(ulambda u, long n) {
  auto* c = CL::as<CL::Uconst>(u);
  return c && c->c.kind == CL::UConstant::Kind::Uconst_int && c->c.i == n;
}

expression transl_closure(const Env& env, const CL::Uclosure* x) {
  Slice<const CL::UFunction*> fundecls = x->funs;
  if (x->fv.empty()) {
    std::string_view sym = compilenv::new_const_symbol();
    cmmgen_state::add_constant(sym, cmmgen_state::Constant{true, cmmgen_state::IsGlobal::Local, fundecls, {}, {}});
    for (const CL::UFunction* f : fundecls) cmmgen_state::add_function(f);
    Dbg dbg = fundecls.empty() ? debuginfo::none() : fundecls[0]->dbg;
    return cconst_symbol(sym, dbg);
  }
  long startenv = fundecls_size(fundecls);
  // transl_fundecls pos fundecls: add_function in order, the cons chain
  // built from its tail (the closure variables translated last in the
  // recursion, then each function's curry symbol from the last function
  // back)
  std::function<std::vector<expression>(long, std::size_t)> transl_fundecls = [&](long pos, std::size_t k) {
    if (k == fundecls.size()) return transl_list(env, x->fv);
    const CL::UFunction* f = fundecls[k];
    cmmgen_state::add_function(f);
    const Dbg& dbg = f->dbg;
    std::vector<expression> without_header;
    if (f->arity == 1 || f->arity == 0) {
      std::vector<expression> rest = transl_fundecls(pos + 3, k + 1);
      without_header = {cconst_symbol(f->label, dbg), alloc_closure_info(f->arity, startenv - pos, dbg)};
      without_header.insert(without_header.end(), rest.begin(), rest.end());
    } else {
      std::vector<expression> rest = transl_fundecls(pos + 4, k + 1);
      expression info = alloc_closure_info(f->arity, startenv - pos, dbg);
      std::string_view curry = curry_function_sym(f->arity);
      without_header = {cconst_symbol(curry, dbg), info, cconst_symbol(f->label, dbg)};
      without_header.insert(without_header.end(), rest.begin(), rest.end());
    }
    if (pos == 0) return without_header;
    without_header.insert(without_header.begin(), alloc_infix_header(pos, f->dbg));
    return without_header;
  };
  Dbg dbg = fundecls.empty() ? debuginfo::none() : fundecls[0]->dbg;
  // #11482, #12481: the 'clos_vars' may be arbitrary expressions and may
  // invoke the GC, which would be able to observe the partially-filled
  // block. This is safe because 'make_alloc' evaluates and fills fields
  // from left to right, and does not call a GC between the allocation and
  // filling fields. So the closure metadata, which comes before the closure
  // variables, will always have been written before a GC can happen.
  return make_alloc(dbg, 247 /* Obj.closure_tag */, transl_fundecls(0, 0));
}

expression transl_prim(const Env& env, const CL::Uprim* x) {
  const Dbg& dbg = x->dbg;
  Slice<ulambda> args = x->args;
  CL::Primitive p = simplif_primitive(x->p);
  switch (p.kind) {
    case PK::Pread_symbol:
      if (args.empty()) return cconst_symbol(p.sym, dbg);
      break;
    case PK::Pmakeblock:
      if (args.empty()) fatal("Cmmgen.transl: Pmakeblock []");
      return make_alloc(dbg, p.n, transl_list(env, args));
    case PK::Pccall: return transl_ccall(env, p.ccall, args, dbg);
    case PK::Pduparray:
      if (args.size() == 1) {
        if (auto* m = CL::as<CL::Uprim>(args[0]); m && m->p.kind == PK::Pmakearray) {
          // We arrive here in two cases: 1. When using Closure, all the
          // time.  2. When using Flambda, [...]
          if (p.array != m->p.array) fatal("Cmmgen.transl: Pduparray");
          return transl_make_array(dbg, env, p.array, m->args);
        }
        return transl_ccall(env, primitive_simple("caml_obj_dup", 1, true), args, dbg);
      }
      break;
    case PK::Pmakearray:
      if (args.empty()) fatal("Pmakearray is not allowed for an empty array");
      return transl_make_array(dbg, env, p.array, args);
    case PK::Pbigarrayref:
      if (!args.empty()) {
        // bigarray_get unsafe elt_kind layout (transl env arg1) (List.map
        // (transl env) argl) dbg: right to left
        std::vector<expression> argl;
        for (std::size_t k = 1; k < args.size(); ++k) argl.push_back(transl(env, args[k]));
        expression a1 = transl(env, args[0]);
        expression elt = bigarray_get(p.unsafe, p.ba_kind, p.ba_layout, a1, argl, dbg);
        using BK = L::BigarrayKind;
        switch (p.ba_kind) {
          case BK::Pbigarray_float16: return box_float(dbg, float_of_float16(dbg, elt));
          case BK::Pbigarray_float32:
          case BK::Pbigarray_float64: return box_float(dbg, elt);
          case BK::Pbigarray_complex32:
          case BK::Pbigarray_complex64: return elt;
          case BK::Pbigarray_int32: return box_int(dbg, BoxedInteger::Pint32, elt);
          case BK::Pbigarray_int64: return box_int(dbg, BoxedInteger::Pint64, elt);
          case BK::Pbigarray_native_int: return box_int(dbg, BoxedInteger::Pnativeint, elt);
          case BK::Pbigarray_caml_int: return tag_int(elt, dbg);
          case BK::Pbigarray_sint8:
          case BK::Pbigarray_uint8:
          case BK::Pbigarray_sint16:
          case BK::Pbigarray_uint16: return tag_int(elt, dbg);
          case BK::Pbigarray_unknown: fatal("Cmmgen.transl: Pbigarray_unknown");
        }
      }
      break;
    case PK::Pbigarrayset:
      if (!args.empty()) {
        ulambda argnewval = args[args.size() - 1];
        using BK = L::BigarrayKind;
        // right to left: the new value, the indexes, the array
        expression newval;
        switch (p.ba_kind) {
          case BK::Pbigarray_float16: newval = float16_of_float(dbg, transl_unbox_float(dbg, env, argnewval)); break;
          case BK::Pbigarray_float32:
          case BK::Pbigarray_float64: newval = transl_unbox_float(dbg, env, argnewval); break;
          case BK::Pbigarray_complex32:
          case BK::Pbigarray_complex64: newval = transl(env, argnewval); break;
          case BK::Pbigarray_int32: newval = transl_unbox_int(dbg, env, BoxedInteger::Pint32, argnewval); break;
          case BK::Pbigarray_int64: newval = transl_unbox_int(dbg, env, BoxedInteger::Pint64, argnewval); break;
          case BK::Pbigarray_native_int:
            newval = transl_unbox_int(dbg, env, BoxedInteger::Pnativeint, argnewval);
            break;
          case BK::Pbigarray_caml_int: newval = untag_int(transl(env, argnewval), dbg); break;
          case BK::Pbigarray_sint8:
          case BK::Pbigarray_uint8:
          case BK::Pbigarray_sint16:
          case BK::Pbigarray_uint16:
            newval = ignore_high_bit_int(untag_int(transl(env, argnewval), dbg));
            break;
          case BK::Pbigarray_unknown: fatal("Cmmgen.transl: Pbigarray_unknown");
        }
        std::vector<expression> argidx;
        for (std::size_t k = 1; k + 1 < args.size(); ++k) argidx.push_back(transl(env, args[k]));
        expression a1 = transl(env, args[0]);
        return return_unit(dbg, bigarray_set(p.unsafe, p.ba_kind, p.ba_layout, a1, argidx, newval, dbg));
      }
      break;
    case PK::Pbigarraydim:
      if (args.size() == 1) {
        long dim_ofs = 4 + p.n;
        return tag_int(mk_load_mut(MC::Word_int, field_address(transl(env, args[0]), dim_ofs, dbg), dbg), dbg);
      }
      break;
    case PK::Pintcomp:
      if (args.size() == 2 && uconst_int_is(args[1], 0)) {
        if (auto* c = CL::as<CL::Uprim>(args[0]); c && c->args.size() == 2) {
          if (c->p.kind == PK::Pcompare_ints) {
            auto* u = static_cast<const CL::Uprim*>(CL::uprim(p, c->args, dbg));
            return transl(env, u);
          }
          if (c->p.kind == PK::Pcompare_bints) {
            CL::Primitive q{PK::Pbintcomp};
            q.bi = c->p.bi;
            q.icmp = p.icmp;
            return transl(env, CL::uprim(q, c->args, dbg));
          }
        }
      }
      break;
    default: break;
  }
  switch (args.size()) {
    case 1: return transl_prim_1(env, p, args[0], dbg);
    case 2: return transl_prim_2(env, p, args[0], args[1], dbg);
    case 3: return transl_prim_3(env, p, args[0], args[1], args[2], dbg);
    case 4: return transl_prim_4(env, p, args[0], args[1], args[2], args[3], dbg);
    default: fatal("Cmmgen.transl:prim, wrong arity");
  }
}

expression transl(const Env& env, ulambda e) {
  switch (e->kind) {
    case UK::Uvar: {
      Var id = static_cast<const CL::Uvar*>(e)->id;
      if (const auto* u = is_unboxed_id(id, env)) {
        expression var = is_mutable_id(u->first, env) ? cvar_mut(u->first) : cvar(u->first);
        return box_number(u->second, var);
      }
      return is_mutable_id(id, env) ? cvar_mut(id) : cvar(id);
    }
    case UK::Uconst: return transl_constant(debuginfo::none(), static_cast<const CL::Uconst*>(e)->c);
    case UK::Uclosure: return transl_closure(env, static_cast<const CL::Uclosure*>(e));
    case UK::Uoffset: {
      auto* x = static_cast<const CL::Uoffset*>(e);
      // produces a valid Caml value, pointing just after an infix header
      expression ptr = transl(env, x->l);
      return ptr_offset(ptr, x->ofs, debuginfo::none());
    }
    case UK::Udirect_apply: {
      auto* x = static_cast<const CL::Udirect_apply*>(e);
      std::vector<expression> args = transl_list(env, x->args);
      return direct_apply(x->f, args, x->dbg);
    }
    case UK::Ugeneric_apply: {
      auto* x = static_cast<const CL::Ugeneric_apply*>(e);
      expression clos = transl(env, x->f);
      std::vector<expression> args = transl_list(env, x->args);
      return generic_apply(mut_from_env(env, clos), clos, args, x->dbg);
    }
    case UK::Usend: {
      auto* x = static_cast<const CL::Usend*>(e);
      expression met = transl(env, x->met);
      expression obj = transl(env, x->obj);
      std::vector<expression> args = transl_list(env, x->args);
      return send(x->k, met, obj, args, x->dbg);
    }
    case UK::Ulet: {
      auto* x = static_cast<const CL::Ulet*>(e);
      return transl_let(env, x->mut, x->k, x->id, x->arg, [x](const Env& env) { return transl(env, x->body); });
    }
    case UK::Uphantom_let: fatal("Cmmgen.transl: Uphantom_let");
    case UK::Uprim: return transl_prim(env, static_cast<const CL::Uprim*>(e));
    // Control structures
    case UK::Uswitch: {
      auto* x = static_cast<const CL::Uswitch*>(e);
      const CL::USwitch& s = x->sw;
      const Dbg& dbg = x->dbg;
      // As in the bytecode interpreter, only matching against constants
      // can be checked
      if (s.us_index_blocks.empty()) {
        // make_switch (Tagged (transl env arg)) index (Array.map ..) dbg:
        // right to left
        std::vector<SwitchCase> actions;
        for (ulambda a : s.us_actions_consts) actions.push_back({transl(env, a), dbg});
        expression arg = transl(env, x->arg);
        return make_switch(true, arg, s.us_index_consts, actions, dbg);
      }
      if (s.us_index_consts.empty())
        return bind("switch", transl(env, x->arg), [&](expression arg) {
          return transl_switch(dbg, env, get_tag(arg, dbg), s.us_index_blocks, s.us_actions_blocks);
        });
      return bind("switch", transl(env, x->arg), [&](expression arg) {
        // right to left
        expression blocks = transl_switch(dbg, env, get_tag(arg, dbg), s.us_index_blocks, s.us_actions_blocks);
        expression consts = transl_switch(dbg, env, untag_int(arg, dbg), s.us_index_consts, s.us_actions_consts);
        return cifthenelse(cop(op(OK::Cand), {arg, cconst_int(1, dbg)}, dbg), dbg, consts, dbg, blocks, dbg);
      });
    }
    case UK::Ustringswitch: {
      auto* x = static_cast<const CL::Ustringswitch*>(e);
      Dbg dbg = debuginfo::none();
      return bind("switch", transl(env, x->arg), [&](expression arg) {
        // right to left: the cases, then the default
        std::vector<std::pair<std::string_view, expression>> sw;
        for (auto& c : x->cases) sw.push_back({c.s, transl(env, c.action)});
        expression d = x->def ? transl(env, x->def) : nullptr;
        return strmatch_compile(dbg, arg, d, sw);
      });
    }
    case UK::Ustaticfail: {
      auto* x = static_cast<const CL::Ustaticfail*>(e);
      std::vector<expression> cargs = transl_list(env, x->args);
      notify_catch(x->i, env, cargs);
      return cexit(x->i, slice(cargs));
    }
    case UK::Ucatch: {
      auto* x = static_cast<const CL::Ucatch*>(e);
      Dbg dbg = debuginfo::none();
      if (x->vars.empty()) {
        // make_catch nfail (transl env body) (transl env handler) dbg:
        // right to left
        expression handler = transl(env, x->handler);
        expression body = transl(env, x->body);
        return make_catch(x->i, body, handler, dbg);
      }
      return transl_catch(env, x->i, x->vars, x->body, x->handler, dbg);
    }
    case UK::Utrywith: {
      auto* x = static_cast<const CL::Utrywith*>(e);
      // right to left
      expression handler = transl(env, x->handler);
      expression body = transl(env, x->body);
      return ctrywith(body, x->exn, handler, debuginfo::none());
    }
    case UK::Uifthenelse: {
      auto* x = static_cast<const CL::Uifthenelse*>(e);
      Dbg none = debuginfo::none();
      expression ifso = transl(env, x->ifso);
      expression ifnot = transl(env, x->ifnot);
      ThenElse approx = ThenElse::Unknown;
      if (is_cint_eq(ifso, 1) && is_cint_eq(ifnot, 3)) approx = ThenElse::Then_false_else_true;
      else if (is_cint_eq(ifso, 3) && is_cint_eq(ifnot, 1)) approx = ThenElse::Then_true_else_false;
      return transl_if(env, approx, none, x->cond, none, ifso, none, ifnot);
    }
    case UK::Usequence: {
      auto* x = static_cast<const CL::Usequence*>(e);
      // right to left
      expression e2 = transl(env, x->l2);
      expression e1 = remove_unit(transl(env, x->l1));
      return csequence(e1, e2);
    }
    case UK::Uwhile: {
      auto* x = static_cast<const CL::Uwhile*>(e);
      Dbg dbg = debuginfo::none();
      long raise_num = L::next_raise_count();
      // right to left: the body, the condition, then create_loop
      expression body = remove_unit(transl(env, x->body));
      expression t = transl_if(env, ThenElse::Unknown, dbg, x->cond, dbg, body, dbg, cexit(raise_num, {}));
      return return_unit(dbg, ccatch(raise_num, {}, create_loop(t, dbg), ctuple({}), dbg));
    }
    case UK::Ufor: {
      auto* x = static_cast<const CL::Ufor*>(e);
      Dbg dbg = debuginfo::none();
      bool upto = x->dir == parsetree::DirectionFlag::Upto;
      IntegerComparison tst = upto ? IntegerComparison::Cgt : IntegerComparison::Clt;
      OK inc = upto ? OK::Caddi : OK::Csubi;
      long raise_num = L::next_raise_count();
      VarWithProvenance id_prev{Ident::create_local("*id_prev*"), nullptr};
      Env env2 = add_mutable_id(x->id.var, env);
      Var id = x->id.var;
      // Clet_mut (id, typ_int, transl env low, bind "bound" (transl env
      // high) (fun high -> ..)): right to left
      expression hi = transl(env2, x->hi);
      expression loop_ = bind("bound", hi, [&](expression high) {
        expression body = remove_unit(transl(env2, x->body));
        expression step = clet(
            id_prev, cvar_mut(id),
            csequence(cassign(id, cop(op(inc), {cvar_mut(id), cconst_int(2, dbg)}, dbg)),
                      cifthenelse(cop(ccmpi(IntegerComparison::Ceq), {cvar(id_prev.var), high}, dbg), dbg,
                                  cexit(raise_num, {}), dbg, ctuple({}), dbg)));
        expression loop = create_loop(csequence(body, step), dbg);
        return ccatch(raise_num, {},
                      cifthenelse(cop(ccmpi(tst), {cvar_mut(id), high}, dbg), dbg, cexit(raise_num, {}), dbg, loop, dbg),
                      ctuple({}), dbg);
      });
      expression lo = transl(env2, x->lo);
      return return_unit(dbg, clet_mut(x->id, typ_int(), lo, loop_));
    }
    case UK::Uassign: {
      auto* x = static_cast<const CL::Uassign*>(e);
      Dbg dbg = debuginfo::none();
      expression cexp = transl(env, x->e);
      if (const auto* u = is_unboxed_id(x->id, env))
        return return_unit(dbg, cassign(u->first, unbox_number(dbg, u->second, cexp)));
      return return_unit(dbg, cassign(x->id, cexp));
    }
    case UK::Uunreachable: {
      Dbg dbg = debuginfo::none();
      return mk_load_mut(MC::Word_int, cconst_int(0, dbg), dbg);
    }
  }
  fatal("Cmmgen.transl");
}

expression transl_catch(const Env& env, long nfail, Slice<CL::UParam> ids0, ulambda body0, ulambda handler,
                        const Dbg& dbg) {
  struct Id {
    VarWithProvenance id;
    L::ValueKind kind;
    std::shared_ptr<UnboxedNumberKind> u;
  };
  std::vector<Id> ids;
  for (auto& p : ids0) ids.push_back({p.var, p.kind, std::make_shared<UnboxedNumberKind>(UnboxedNumberKind{UnboxedNumberKind::K::No_result})});
  // Translate the body, and while doing so, collect the "unboxing type" for
  // each argument.
  auto report = std::make_shared<const std::function<void(const std::vector<expression>&)>>(
      [ids](const std::vector<expression>& args) {
        for (std::size_t k = 0; k < ids.size() && k < args.size(); ++k) {
          using VK = L::ValueKind::Kind;
          bool strict = !(ids[k].kind.kind == VK::Pfloatval || ids[k].kind.kind == VK::Pboxedintval);
          *ids[k].u = join_unboxed_number_kind(strict, *ids[k].u, is_unboxed_number_cmm(strict, ids[k].kind, args[k]));
        }
      });
  Env env_body = add_notify_catch(nfail, report, env);
  expression body = transl(env_body, body0);
  // List.fold_right: from the last id
  Env new_env = env;
  bool changed = false;
  std::vector<std::function<expression(expression)>> rewrite(ids.size());
  std::vector<CatchParam> new_ids(ids.size());
  for (std::size_t k = ids.size(); k-- > 0;) {
    const UnboxedNumberKind& u = *ids[k].u;
    if (u.k == UnboxedNumberKind::K::Boxed && !u.free) {
      Var unboxed_id = Ident::create_local(ident::name(ids[k].id.var));
      new_env = add_unboxed_id(ids[k].id.var, unboxed_id, u.bn, new_env);
      changed = true;
      BoxedNumber bn = u.bn;
      rewrite[k] = [bn](expression x) { return unbox_number(debuginfo::none(), bn, x); };
      new_ids[k] = {{unboxed_id, nullptr}, typ_of_boxed_number(bn)};
    } else {
      rewrite[k] = [](expression x) { return x; };
      new_ids[k] = {ids[k].id, machtype_of_value_kind(ids[k].kind)};
    }
  }
  if (!changed) {
    // No unboxing
    expression h = transl(env, handler);
    return ccatch(nfail, slice(new_ids), body, h, dbg);
  }
  // allocate new "nfail" to catch errors more easily
  long new_nfail = L::next_raise_count();
  // Rewrite the body to unbox the call sites
  std::function<expression(expression)> aux = [&](expression e) -> expression {
    expression c = map_shallow(aux, e);
    if (auto* x = as<Cexit>(c); x && x->n == nfail) {
      std::vector<expression> el;
      for (std::size_t k = 0; k < x->args.size(); ++k) el.push_back(rewrite[k](x->args[k]));
      return cexit(new_nfail, slice(el));
    }
    return c;
  };
  expression body2 = aux(body);
  expression h = transl(new_env, handler);
  return ccatch(new_nfail, slice(new_ids), body2, h, dbg);
}

expression transl_make_array(const Dbg& dbg, const Env& env, L::ArrayKind kind, Slice<ulambda> args) {
  switch (kind) {
    case L::ArrayKind::Pgenarray:
      return cop(cextcall("caml_array_of_uniform_array", typ_val(), {}, true), {make_alloc(dbg, 0, transl_list(env, args))},
                 dbg);
    case L::ArrayKind::Paddrarray:
    case L::ArrayKind::Pintarray: return make_alloc(dbg, 0, transl_list(env, args));
    case L::ArrayKind::Pfloatarray: {
      std::vector<expression> l;
      for (ulambda a : args) l.push_back(transl_unbox_float(dbg, env, a));
      return make_float_alloc(dbg, 254 /* Obj.double_array_tag */, l);
    }
  }
  fatal("Cmmgen.transl_make_array");
}

expression transl_ccall(const Env& env, const PrimitiveDescription* prim, Slice<ulambda> args, const Dbg& dbg) {
  using NK = NativeRepr::Kind;
  std::vector<Exttype> tys;
  std::vector<expression> cargs;
  // transl_args: each argument's type and translation, left to right
  std::size_t k = 0;
  for (; k < prim->prim_native_repr_args.size() && k < args.size(); ++k) {
    const NativeRepr& r = prim->prim_native_repr_args[k];
    ulambda arg = args[k];
    switch (r.kind) {
      case NK::Same_as_ocaml_repr:
        tys.push_back(Exttype::XInt);
        cargs.push_back(transl(env, arg));
        break;
      case NK::Unboxed_float:
        tys.push_back(Exttype::XFloat);
        cargs.push_back(transl_unbox_float(dbg, env, arg));
        break;
      case NK::Unboxed_integer:
        tys.push_back(r.bi == BoxedInteger::Pnativeint ? Exttype::XInt
                      : r.bi == BoxedInteger::Pint32   ? Exttype::XInt32
                                                       : Exttype::XInt64);
        cargs.push_back(transl_unbox_int(dbg, env, r.bi, arg));
        break;
      case NK::Untagged_immediate:
        tys.push_back(Exttype::XInt);
        cargs.push_back(untag_int(transl(env, arg), dbg));
        break;
    }
  }
  if (k < prim->prim_native_repr_args.size()) fatal("Cmmgen.transl_ccall");
  // We don't require the two lists to be of the same length as
  // [default_prim] always sets the arity to [0].
  for (; k < args.size(); ++k) {
    tys.push_back(Exttype::XInt);
    cargs.push_back(transl(env, args[k]));
  }
  Machtype typ_res;
  switch (prim->prim_native_repr_res.kind) {
    case NK::Same_as_ocaml_repr: typ_res = typ_val(); break;
    case NK::Unboxed_float: typ_res = typ_float(); break;
    default: typ_res = typ_int(); break;
  }
  std::string_view native_name = prim->prim_native_name.empty() ? prim->prim_name : prim->prim_native_name;
  expression call = cop(cextcall(native_name, typ_res, slice(tys), prim->prim_alloc), slice(cargs), dbg);
  switch (prim->prim_native_repr_res.kind) {
    case NK::Same_as_ocaml_repr: return call;
    case NK::Unboxed_float: return box_float(dbg, call);
    case NK::Unboxed_integer: return box_int(dbg, prim->prim_native_repr_res.bi, call);
    case NK::Untagged_immediate: return tag_int(call, dbg);
  }
  return call;
}

expression transl_prim_1(const Env& env, const CL::Primitive& p, ulambda arg, const Dbg& dbg) {
  switch (p.kind) {
    // Generic operations
    case PK::Popaque: return opaque(transl(env, arg), dbg);
    // Heap operations
    case PK::Pmakelazyblock: return make_alloc(dbg, L::tag_of_lazy_tag(p.lazy_tag), {transl(env, arg)});
    case PK::Pfield: return get_field(env, p.ptr, p.mut, transl(env, arg), p.n, dbg);
    case PK::Pfloatfield: {
      expression ptr = transl(env, arg);
      return box_float(dbg, floatfield(p.n, ptr, dbg));
    }
    case PK::Pint_as_pointer: return int_as_pointer(transl(env, arg), dbg);
    // Exceptions
    case PK::Praise: return raise_prim(p.raise, transl(env, arg), dbg);
    // Integer operations
    case PK::Pnegint: return negint(transl(env, arg), dbg);
    case PK::Poffsetint: return offsetint(p.n, transl(env, arg), dbg);
    case PK::Poffsetref: return offsetref(p.n, transl(env, arg), dbg);
    // Floating-point operations
    case PK::Pfloatofint:
      return box_float(dbg, cop(op(OK::Cfloatofint), {untag_int(transl(env, arg), dbg)}, dbg));
    case PK::Pintoffloat: return tag_int(cop(op(OK::Cintoffloat), {transl_unbox_float(dbg, env, arg)}, dbg), dbg);
    case PK::Pnegfloat: return box_float(dbg, cop(op(OK::Cnegf), {transl_unbox_float(dbg, env, arg)}, dbg));
    case PK::Pabsfloat: return box_float(dbg, cop(op(OK::Cabsf), {transl_unbox_float(dbg, env, arg)}, dbg));
    // String operations
    case PK::Pstringlength:
    case PK::Pbyteslength: return tag_int(string_length(transl(env, arg), dbg), dbg);
    // Array operations
    case PK::Parraylength: return arraylength(p.array, transl(env, arg), dbg);
    // Boolean operations
    case PK::Pnot:
      return transl_if(env, ThenElse::Then_false_else_true, dbg, arg, dbg, cconst_int(1, dbg), dbg,
                       cconst_int(3, dbg));
    // Test integer/block
    case PK::Pisint: return tag_int(cop(op(OK::Cand), {transl(env, arg), cconst_int(1, dbg)}, dbg), dbg);
    // Boxed integers
    case PK::Pbintofint: return box_int(dbg, p.bi, untag_int(transl(env, arg), dbg));
    case PK::Pintofbint: return tag_int(transl_unbox_int(dbg, env, p.bi, arg), dbg);
    case PK::Pcvtbint: return box_int(dbg, p.bi2, transl_unbox_int(dbg, env, p.bi, arg));
    case PK::Pnegbint:
      return box_int(dbg, p.bi,
                     cop(op(OK::Csubi), {cconst_int(0, dbg), transl_unbox_int(dbg, env, p.bi, arg)}, dbg));
    case PK::Pbbswap: return box_int(dbg, p.bi, bbswap(p.bi, transl_unbox_int(dbg, env, p.bi, arg), dbg));
    case PK::Pbswap16:
      return tag_int(bswap16(ignore_high_bit_int(untag_int(transl(env, arg), dbg)), dbg), dbg);
    case PK::Pperform: {
      expression cont = make_alloc(dbg, 245 /* Obj.cont_tag */, {int_const(dbg, 0), int_const(dbg, 0)});
      return cop(capply(typ_val()), {cconst_symbol("caml_perform", dbg), transl(env, arg), cont}, dbg);
    }
    case PK::Pdls_get: return cop(op(OK::Cdls_get), {transl(env, arg)}, dbg);
    case PK::Ppoll: {
      // Csequence (remove_unit (transl env arg), return_unit ..): the poll
      // is a constant
      expression a = remove_unit(transl(env, arg));
      return csequence(a, return_unit(dbg, cop(op(OK::Cpoll), {}, dbg)));
    }
    default: fatal("Cmmgen.transl_prim_1");
  }
}

// f (transl env arg1) (transl env arg2): right to left
#define T2(f) \
  do { \
    expression b = transl(env, arg2); \
    expression a = transl(env, arg1); \
    return f(a, b, dbg); \
  } while (0)

expression transl_prim_2(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, const Dbg& dbg) {
  switch (p.kind) {
    // Heap operations
    case PK::Pfield_computed: T2(addr_array_ref);
    case PK::Psetfield: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return setfield(p.n, p.ptr, p.init, a, b, dbg);
    }
    case PK::Psetfloatfield: {
      expression ptr = transl(env, arg1);
      expression float_val = transl_unbox_float(dbg, env, arg2);
      return setfloatfield(p.n, p.init, ptr, float_val, dbg);
    }
    case PK::Patomic_load: {
      expression ptr = transl(env, arg1);
      expression ofs = transl(env, arg2);
      return mk_load_atomic(MC::Word_val, field_address_computed(ptr, ofs, dbg), dbg);
    }
    // Boolean operations
    case PK::Psequand: {
      Dbg dbg2 = debuginfo::none();
      return transl_sequand(env, ThenElse::Then_true_else_false, dbg, arg1, dbg2, arg2, dbg, cconst_int(3, dbg), dbg2,
                            cconst_int(1, dbg));
    }
    case PK::Psequor: {
      Dbg dbg2 = debuginfo::none();
      return transl_sequor(env, ThenElse::Then_true_else_false, dbg, arg1, dbg2, arg2, dbg, cconst_int(3, dbg), dbg2,
                           cconst_int(1, dbg));
    }
    // Integer operations
    case PK::Paddint: T2(add_int_caml);
    case PK::Psubint: T2(sub_int_caml);
    case PK::Pmulint: T2(mul_int_caml);
    case PK::Pdivint: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return div_int_caml(p.safe, a, b, dbg);
    }
    case PK::Pmodint: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return mod_int_caml(p.safe, a, b, dbg);
    }
    case PK::Pandint: T2(and_int_caml);
    case PK::Porint: T2(or_int_caml);
    case PK::Pxorint: T2(xor_int_caml);
    case PK::Plslint: T2(lsl_int_caml);
    case PK::Plsrint: T2(lsr_int_caml);
    case PK::Pasrint: T2(asr_int_caml);
    case PK::Pintcomp: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return int_comp_caml(p.icmp, a, b, dbg);
    }
    case PK::Pcompare_ints: {
      // Compare directly on tagged ints
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return mk_compare_ints(dbg, a, b);
    }
    case PK::Pcompare_bints: {
      expression a1 = transl_unbox_int(dbg, env, p.bi, arg1);
      expression a2 = transl_unbox_int(dbg, env, p.bi, arg2);
      return mk_compare_ints(dbg, a1, a2);
    }
    case PK::Pcompare_floats: {
      expression a1 = transl_unbox_float(dbg, env, arg1);
      expression a2 = transl_unbox_float(dbg, env, arg2);
      return mk_compare_floats(dbg, a1, a2);
    }
    case PK::Pisout: T2(transl_isout);
    // Float operations
    case PK::Paddfloat:
    case PK::Psubfloat:
    case PK::Pmulfloat:
    case PK::Pdivfloat: {
      OK k = p.kind == PK::Paddfloat   ? OK::Caddf
             : p.kind == PK::Psubfloat ? OK::Csubf
             : p.kind == PK::Pmulfloat ? OK::Cmulf
                                       : OK::Cdivf;
      // a list literal: right to left
      expression b = transl_unbox_float(dbg, env, arg2);
      expression a = transl_unbox_float(dbg, env, arg1);
      return box_float(dbg, cop(op(k), {a, b}, dbg));
    }
    case PK::Pfloatcomp: {
      expression b = transl_unbox_float(dbg, env, arg2);
      expression a = transl_unbox_float(dbg, env, arg1);
      return tag_int(cop(ccmpf(p.fcmp), {a, b}, dbg), dbg);
    }
    // String operations
    case PK::Pstringrefu:
    case PK::Pbytesrefu: T2(stringref_unsafe);
    case PK::Pstringrefs:
    case PK::Pbytesrefs: T2(stringref_safe);
    case PK::Pstring_load:
    case PK::Pbytes_load: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return string_load(p.size, p.safe, a, b, dbg);
    }
    case PK::Pbigstring_load: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return bigstring_load(p.size, p.safe, a, b, dbg);
    }
    // Array operations
    case PK::Parrayrefu: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return arrayref_unsafe(p.array, a, b, dbg);
    }
    case PK::Parrayrefs: {
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return arrayref_safe(p.array, a, b, dbg);
    }
    // Boxed integers
    case PK::Paddbint:
    case PK::Psubbint:
    case PK::Pmulbint: {
      expression b = transl_unbox_int_low(dbg, env, p.bi, arg2);
      expression a = transl_unbox_int_low(dbg, env, p.bi, arg1);
      expression r = p.kind == PK::Paddbint   ? add_int(a, b, dbg)
                     : p.kind == PK::Psubbint ? sub_int(a, b, dbg)
                                              : mul_int(a, b, dbg);
      return box_int(dbg, p.bi, r);
    }
    case PK::Pdivbint: {
      expression b = transl_unbox_int(dbg, env, p.bi, arg2);
      expression a = transl_unbox_int(dbg, env, p.bi, arg1);
      return box_int(dbg, p.bi, safe_div_bi(p.safe, a, b, p.bi, dbg));
    }
    case PK::Pmodbint: {
      expression b = transl_unbox_int(dbg, env, p.bi, arg2);
      expression a = transl_unbox_int(dbg, env, p.bi, arg1);
      return box_int(dbg, p.bi, safe_mod_bi(p.safe, a, b, p.bi, dbg));
    }
    case PK::Pandbint:
    case PK::Porbint:
    case PK::Pxorbint: {
      OK k = p.kind == PK::Pandbint ? OK::Cand : p.kind == PK::Porbint ? OK::Cor : OK::Cxor;
      expression b = transl_unbox_int_low(dbg, env, p.bi, arg2);
      expression a = transl_unbox_int_low(dbg, env, p.bi, arg1);
      return box_int(dbg, p.bi, cop(op(k), {a, b}, dbg));
    }
    case PK::Plslbint: {
      expression b = untag_int(transl(env, arg2), dbg);
      expression a = transl_unbox_int_low(dbg, env, p.bi, arg1);
      return box_int(dbg, p.bi, lsl_int(a, b, dbg));
    }
    case PK::Plsrbint: {
      expression b = untag_int(transl(env, arg2), dbg);
      expression a = make_unsigned_int(p.bi, transl_unbox_int(dbg, env, p.bi, arg1), dbg);
      return box_int(dbg, p.bi, lsr_int(a, b, dbg));
    }
    case PK::Pasrbint: {
      expression b = untag_int(transl(env, arg2), dbg);
      expression a = transl_unbox_int(dbg, env, p.bi, arg1);
      return box_int(dbg, p.bi, asr_int(a, b, dbg));
    }
    case PK::Pbintcomp: {
      expression b = transl_unbox_int(dbg, env, p.bi, arg2);
      expression a = transl_unbox_int(dbg, env, p.bi, arg1);
      return tag_int(cop(ccmpi(p.icmp), {a, b}, dbg), dbg);
    }
    default: fatal("Cmmgen.transl_prim_2");
  }
}
#undef T2

expression transl_prim_3(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, ulambda arg3,
                         const Dbg& dbg) {
  switch (p.kind) {
    // Heap operations
    case PK::Psetfield_computed: {
      expression c = transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return setfield_computed(p.ptr, p.init, a, b, c, dbg);
    }
    // String operations
    case PK::Pbytessetu:
    case PK::Pbytessets: {
      expression c = transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return p.kind == PK::Pbytessetu ? bytesset_unsafe(a, b, c, dbg) : bytesset_safe(a, b, c, dbg);
    }
    // Array operations
    case PK::Parraysetu:
    case PK::Parraysets: {
      expression newval =
          p.array == L::ArrayKind::Pfloatarray ? transl_unbox_float(dbg, env, arg3) : transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return p.kind == PK::Parraysetu ? arrayset_unsafe(p.array, a, b, newval, dbg)
                                      : arrayset_safe(p.array, a, b, newval, dbg);
    }
    case PK::Pbytes_set:
    case PK::Pbigstring_set: {
      expression c = transl_unbox_sized(p.size, dbg, env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return p.kind == PK::Pbytes_set ? bytes_set(p.size, p.safe, a, b, c, dbg)
                                      : bigstring_set(p.size, p.safe, a, b, c, dbg);
    }
    // Effects
    case PK::Prunstack: {
      expression c = transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return cop(capply(typ_val()), {cconst_symbol("caml_runstack", dbg), a, b, c}, dbg);
    }
    case PK::Preperform: {
      expression c = transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return cop(capply(typ_val()), {cconst_symbol("caml_reperform", dbg), a, b, c}, dbg);
    }
    default: fatal("Cmmgen.transl_prim_3");
  }
}

expression transl_prim_4(const Env& env, const CL::Primitive& p, ulambda arg1, ulambda arg2, ulambda arg3,
                         ulambda arg4, const Dbg& dbg) {
  switch (p.kind) {
    case PK::Presume: {
      expression d = transl(env, arg4);
      expression c = transl(env, arg3);
      expression b = transl(env, arg2);
      expression a = transl(env, arg1);
      return cop(capply(typ_val()), {cconst_symbol("caml_resume", dbg), a, b, c, d}, dbg);
    }
    default: fatal("Cmmgen.transl_prim_4");
  }
}

expression transl_unbox_float(const Dbg& dbg, const Env& env, ulambda exp) {
  return unbox_float(dbg, transl(env, exp));
}
expression transl_unbox_int(const Dbg& dbg, const Env& env, BoxedInteger bi, ulambda exp) {
  return unbox_int(dbg, bi, transl(env, exp));
}
// transl_unbox_int, but may return garbage in upper bits
expression transl_unbox_int_low(const Dbg& dbg, const Env& env, BoxedInteger bi, ulambda e) {
  expression r = transl_unbox_int(dbg, env, bi, e);
  return bi == BoxedInteger::Pint32 ? low_32(dbg, r) : r;
}
expression transl_unbox_sized(CL::MemoryAccessSize size, const Dbg& dbg, const Env& env, ulambda exp) {
  switch (size) {
    case CL::MemoryAccessSize::Sixteen: return ignore_high_bit_int(untag_int(transl(env, exp), dbg));
    case CL::MemoryAccessSize::Thirty_two: return transl_unbox_int(dbg, env, BoxedInteger::Pint32, exp);
    default: return transl_unbox_int(dbg, env, BoxedInteger::Pint64, exp);
  }
}

expression transl_let(const Env& env, MutableFlag str, const L::ValueKind& kind, const VarWithProvenance& id,
                      ulambda exp, const std::function<expression(const Env&)>& transl_body) {
  using VK = L::ValueKind::Kind;
  Dbg dbg = debuginfo::none();
  expression cexp = transl(env, exp);
  UnboxedNumberKind unboxing;
  // If [id] is a mutable variable (introduced to eliminate a local
  // reference) and it contains a type of unboxable numbers, then force
  // unboxing.  Indeed, if not boxed, each assignment to the variable might
  // require some boxing, but such local references are often used in loops
  // and we really want to avoid repeated boxing.
  if (str == MutableFlag::Mutable && kind.kind == VK::Pfloatval)
    unboxing = boxed(BoxedNumber{true, BoxedInteger::Pnativeint, dbg}, false);
  else if (str == MutableFlag::Mutable && kind.kind == VK::Pboxedintval)
    unboxing = boxed(BoxedNumber{false, kind.bi, dbg}, false);
  else if (kind.kind == VK::Pfloatval || kind.kind == VK::Pboxedintval)
    // It would be safe to always unbox in this case, but we do it only if
    // this indeed allows us to get rid of some allocations in the bound
    // expression.
    unboxing = is_unboxed_number_cmm(false, kind, cexp);
  else if (kind.kind == VK::Pgenval)
    // Here we don't know statically that the bound expression evaluates to
    // an unboxable number type.  We need to be stricter and ensure that all
    // possible branches in the expression return a boxed value (of the same
    // kind).  Indeed, with GADTs, different branches could return different
    // types.
    unboxing = is_unboxed_number_cmm(true, kind, cexp);
  else
    unboxing = no_unboxing();
  if (unboxing.k != UnboxedNumberKind::K::Boxed || unboxing.free) {
    // N.B. [body] must still be traversed even if [exp] will never return:
    // there may be constant closures inside that need lifting out.
    if (str == MutableFlag::Immutable) return clet(id, cexp, transl_body(env));
    if (kind.kind == VK::Pintval) return clet_mut(id, typ_int(), cexp, transl_body(add_mutable_id(id.var, env)));
    return clet_mut(id, typ_val(), cexp, transl_body(add_mutable_id(id.var, env)));
  }
  const BoxedNumber& bn = unboxing.bn;
  Var unboxed_id = Ident::create_local(ident::name(id.var));
  VarWithProvenance v{unboxed_id, nullptr};
  expression cexp2 = unbox_number(dbg, bn, cexp);
  auto body = [&](const Env& env) { return transl_body(add_unboxed_id(id.var, unboxed_id, bn, env)); };
  if (str == MutableFlag::Immutable) return clet(v, cexp2, body(env));
  return clet_mut(v, typ_of_boxed_number(bn), cexp2, body(add_mutable_id(unboxed_id, env)));
}

expression make_catch(long ncatch, expression body, expression handler, const Dbg& dbg) {
  if (auto* e = as<Cexit>(body); e && e->args.empty() && e->n == ncatch) return handler;
  return ccatch(ncatch, {}, body, handler, dbg);
}

bool is_shareable_cont(expression exp) {
  auto* e = as<Cexit>(exp);
  return e && e->args.empty();
}

expression make_shareable_cont(const Dbg& dbg, const std::function<expression(expression)>& mk, expression exp) {
  if (is_shareable_cont(exp)) return mk(exp);
  long nfail = L::next_raise_count();
  return make_catch(nfail, mk(cexit(nfail, {})), exp, dbg);
}

expression transl_if(const Env& env, ThenElse approx, const Dbg& dbg, ulambda cond, const Dbg& then_dbg,
                     expression then_, const Dbg& else_dbg, expression else_) {
  Dbg none = debuginfo::none();
  if (uconst_int_is(cond, 0)) return else_;
  if (uconst_int_is(cond, 1)) return then_;
  if (auto* ite = CL::as<CL::Uifthenelse>(cond); ite && uconst_int_is(ite->ifnot, 0))
    // CR mshinwell: These Debuginfos will flow through from Clambda
    return transl_sequand(env, approx, none, ite->cond, none, ite->ifso, then_dbg, then_, else_dbg, else_);
  if (auto* l = CL::as<CL::Ulet>(cond))
    return transl_let(env, l->mut, l->k, l->id, l->arg, [&](const Env& env) {
      return transl_if(env, approx, dbg, l->body, then_dbg, then_, else_dbg, else_);
    });
  if (auto* p = CL::as<CL::Uprim>(cond); p && p->p.kind == PK::Psequand && p->args.size() == 2)
    return transl_sequand(env, approx, p->dbg, p->args[0], p->dbg, p->args[1], then_dbg, then_, else_dbg, else_);
  if (auto* ite = CL::as<CL::Uifthenelse>(cond); ite && uconst_int_is(ite->ifso, 1))
    return transl_sequor(env, approx, none, ite->cond, none, ite->ifnot, then_dbg, then_, else_dbg, else_);
  if (auto* p = CL::as<CL::Uprim>(cond); p && p->p.kind == PK::Psequor && p->args.size() == 2)
    return transl_sequor(env, approx, p->dbg, p->args[0], p->dbg, p->args[1], then_dbg, then_, else_dbg, else_);
  if (auto* p = CL::as<CL::Uprim>(cond); p && p->p.kind == PK::Pnot && p->args.size() == 1)
    return transl_if(env, invert_then_else(approx), dbg, p->args[0], else_dbg, else_, then_dbg, then_);
  if (auto* ite = CL::as<CL::Uifthenelse>(cond); ite && uconst_int_is(ite->cond, 1))
    return transl_if(env, approx, none, ite->ifso, then_dbg, then_, else_dbg, else_);
  if (auto* ite = CL::as<CL::Uifthenelse>(cond); ite && uconst_int_is(ite->cond, 0))
    return transl_if(env, approx, none, ite->ifnot, then_dbg, then_, else_dbg, else_);
  if (auto* ite = CL::as<CL::Uifthenelse>(cond)) {
    return make_shareable_cont(
        then_dbg,
        [&](expression shareable_then) {
          return make_shareable_cont(
              else_dbg,
              [&](expression shareable_else) {
                // mk_if_then_else inner_dbg (test_bool ..) ifso_dbg (..)
                // ifnot_dbg (..): right to left
                expression ifnot = transl_if(env, approx, none, ite->ifnot, then_dbg, shareable_then, else_dbg,
                                             shareable_else);
                expression ifso =
                    transl_if(env, approx, none, ite->ifso, then_dbg, shareable_then, else_dbg, shareable_else);
                expression c = test_bool(none, transl(env, ite->cond));
                return mk_if_then_else(none, c, none, ifso, none, ifnot);
              },
              else_);
        },
        then_);
  }
  switch (approx) {
    case ThenElse::Then_true_else_false: return transl(env, cond);
    case ThenElse::Then_false_else_true: return mk_not(dbg, transl(env, cond));
    default: return mk_if_then_else(dbg, test_bool(dbg, transl(env, cond)), then_dbg, then_, else_dbg, else_);
  }
}

expression transl_sequand(const Env& env, ThenElse approx, const Dbg& arg1_dbg, ulambda arg1, const Dbg& arg2_dbg,
                          ulambda arg2, const Dbg& then_dbg, expression then_, const Dbg& else_dbg,
                          expression else_) {
  return make_shareable_cont(
      else_dbg,
      [&](expression shareable_else) {
        // the inner transl_if is an argument of the outer: first
        expression inner = transl_if(env, approx, arg2_dbg, arg2, then_dbg, then_, else_dbg, shareable_else);
        return transl_if(env, ThenElse::Unknown, arg1_dbg, arg1, arg2_dbg, inner, else_dbg, shareable_else);
      },
      else_);
}

expression transl_sequor(const Env& env, ThenElse approx, const Dbg& arg1_dbg, ulambda arg1, const Dbg& arg2_dbg,
                         ulambda arg2, const Dbg& then_dbg, expression then_, const Dbg& else_dbg, expression else_) {
  return make_shareable_cont(
      then_dbg,
      [&](expression shareable_then) {
        expression inner = transl_if(env, approx, arg2_dbg, arg2, then_dbg, shareable_then, else_dbg, else_);
        return transl_if(env, ThenElse::Unknown, arg1_dbg, arg1, then_dbg, shareable_then, arg2_dbg, inner);
      },
      then_);
}

// This assumes that [arg] can be safely discarded if it is not used.
expression transl_switch(const Dbg& dbg, const Env& env, expression arg, Slice<long> index, Slice<ulambda> cases) {
  switch (cases.size()) {
    case 0: fatal("Cmmgen.transl_switch");
    case 1: return transl(env, cases[0]);
    default: return transl_switch_clambda(dbg, arg, index, transl_list(env, cases));
  }
}

// Translate a function definition
Phrase transl_function(const CL::UFunction* f) {
  Env env = create_env(f->env);
  expression cmm_body = transl(env, f->body);
  auto* fd = make<Fundecl>();
  fd->fun_name = f->label;
  fd->fun_args = slice(catch_params(f->params));
  fd->fun_body = cmm_body;
  if (!clflags::optimize_for_speed)
    fd->fun_codegen_options = slice(std::vector<CodegenOption>{CodegenOption::Reduce_code_size});
  fd->fun_poll = f->poll;
  fd->fun_dbg = f->dbg;
  Phrase p;
  p.fn = fd;
  return p;
}

// Translate all function definitions
void transl_all_functions_(std::set<std::string_view>& already_translated,
                           std::vector<std::pair<Dbg, Phrase>>& cont) {
  while (const CL::UFunction* f = cmmgen_state::next_function()) {
    if (already_translated.count(f->label)) continue;
    already_translated.insert(f->label);
    cont.push_back({f->dbg, transl_function(f)});  // consed: reversed by the caller
  }
}

std::vector<Phrase> transl_all_functions(std::vector<Phrase> cont) {
  std::set<std::string_view> already_translated;
  std::vector<std::pair<Dbg, Phrase>> translated;  // newest first
  while (!cmmgen_state::no_more_functions()) transl_all_functions_(already_translated, translated);
  std::reverse(translated.begin(), translated.end());
  // Sort functions according to source position
  std::stable_sort(translated.begin(), translated.end(),
                   [](const auto& a, const auto& b) { return debuginfo::compare(a.first, b.first) < 0; });
  std::vector<Phrase> r;
  for (auto& [_, p] : translated) r.push_back(std::move(p));
  r.insert(r.end(), std::make_move_iterator(cont.begin()), std::make_move_iterator(cont.end()));
  return r;
}

// Emit all structured constants
// the phrases [added] consed onto [cont] one by one (so in reverse order)
std::vector<Phrase> prepend_consed(std::vector<Phrase> added, std::vector<Phrase> cont) {
  std::reverse(added.begin(), added.end());
  added.insert(added.end(), std::make_move_iterator(cont.begin()), std::make_move_iterator(cont.end()));
  return added;
}

std::vector<Phrase> transl_clambda_constants(const std::vector<CL::PreallocatedConstant>& constants,
                                             std::vector<Phrase> cont) {
  std::vector<Phrase> added;
  for (auto& c : constants) {
    Symb symb{c.symbol, c.exported ? cmmgen_state::IsGlobal::Global : cmmgen_state::IsGlobal::Local};
    Phrase p;
    p.data = emit_structured_constant(symb, c.definition, {});
    added.push_back(std::move(p));
  }
  return prepend_consed(std::move(added), std::move(cont));
}

std::vector<Phrase> emit_cmm_data_items_for_constants(std::vector<Phrase> cont) {
  std::vector<Phrase> added;
  for (auto& [symbol, cst] : cmmgen_state::get_and_clear_constants()) {
    Phrase p;
    if (cst.is_closure) {
      std::vector<DataItem> clos_vars;  // List.fold_right emit_constant clos_vars []
      for (const CL::UConstant& c : cst.clos_vars) emit_constant(c, clos_vars);
      p.data = emit_constant_closure({symbol, cst.global}, cst.fundecls, clos_vars, {});
    } else {
      p.data = cdefine_symbol({symbol, cst.global});
      p.data.insert(p.data.end(), cst.table.begin(), cst.table.end());
    }
    added.push_back(std::move(p));
  }
  Phrase items;
  items.data = cmmgen_state::get_and_clear_data_items();
  added.push_back(std::move(items));
  return prepend_consed(std::move(added), std::move(cont));
}

}  // namespace

// Translate a compilation unit
std::vector<Phrase> compunit(const closure_middle_end::WithConstants& c) {
  if (!cmmgen_state::no_more_functions()) fatal("Cmmgen.compunit");
  cmmgen_state::set_structured_constants(c.constants);
  expression init_code = transl(Env{}, c.code);
  auto* entry = make<Fundecl>();
  entry->fun_name = compilenv::make_symbol(std::string_view("entry"));
  entry->fun_body = init_code;
  // This function is often large and run only once.  Compilation time
  // matter more than runtime.  See MPR#7630
  if constexpr (config::flambda)
    entry->fun_codegen_options = slice(std::vector<CodegenOption>{CodegenOption::Reduce_code_size, CodegenOption::No_CSE});
  else
    entry->fun_codegen_options = slice(std::vector<CodegenOption>{CodegenOption::Reduce_code_size});
  entry->fun_poll = L::PollAttribute::Default_poll;
  Phrase p1;
  p1.fn = entry;
  std::vector<Phrase> c1;
  c1.push_back(std::move(p1));
  std::vector<Phrase> c2 = transl_clambda_constants(c.constants, std::move(c1));
  std::vector<Phrase> c3 = transl_all_functions(std::move(c2));
  cmmgen_state::set_structured_constants({});
  std::vector<Phrase> c4 = emit_preallocated_blocks(c.blocks, std::move(c3));
  return emit_cmm_data_items_for_constants(std::move(c4));
}

}  // namespace cppcaml::typing::cmmgen
