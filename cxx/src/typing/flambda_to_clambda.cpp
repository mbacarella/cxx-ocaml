// Port of middle_end/flambda/flambda_to_clambda.ml (see
// flambda_to_clambda.hpp).
//
// The backend variables (Idents) are created in ocamlopt's order: OCaml
// evaluates a constructor's arguments right to left, so a [Let]'s body is
// converted before its defining expression, an [If_then_else]'s [ifnot]
// before its [ifso], a handler before its body.  Their stamps are in the
// -dclambda dumps.
#include "cppcaml/typing/flambda_to_clambda.hpp"

#include <algorithm>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/closure_offsets.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/initialize_symbol_to_let_symbol.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/switch.hpp"
#include "cppcaml/typing/un_anf.hpp"

namespace cppcaml::typing::flambda_to_clambda {

namespace F = flambda;
using namespace clambda;
using F::EK;
using F::NK;

namespace {

[[noreturn]] void fatal(const std::function<void(format::Formatter&)>& msg) {
  format::Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}

struct ForOneOrMoreUnits {
  variable::Map<long> fun_offset_table;  // Closure_id.Map
  variable::Map<long> fv_offset_table;   // Var_within_closure.Map
  variable::Set constant_closures;       // Closure_id.Set
  variable::Set closures;                // Closure_id.Set
};

struct T {
  ForOneOrMoreUnits current_unit;
  ForOneOrMoreUnits imported_units;
  format::Formatter* ppf_dump;
  symbol::Map<const UStructuredConstant*> constants_for_instrumentation;
};

bool in_current_unit(variable::t v) {
  return compilation_unit::equal(variable::get_compilation_unit(v), compilenv::current_compilation_unit());
}

long get_fun_offset(const T& t, variable::t closure_id) {
  const variable::Map<long>& table =
      in_current_unit(closure_id) ? t.current_unit.fun_offset_table : t.imported_units.fun_offset_table;
  if (const long* o = table.find_opt(closure_id)) return *o;
  fatal([&](format::Formatter& f) {
    format::fprintf(f, "Flambda_to_clambda: missing offset for closure %a", pr(variable::print, closure_id));
  });
}

long get_fv_offset(const T& t, variable::t var_within_closure) {
  const variable::Map<long>& table =
      in_current_unit(var_within_closure) ? t.current_unit.fv_offset_table : t.imported_units.fv_offset_table;
  if (const long* o = table.find_opt(var_within_closure)) return *o;
  fatal([&](format::Formatter& f) {
    format::fprintf(f, "Flambda_to_clambda: missing offset for variable %a", pr(variable::print, var_within_closure));
  });
}

bool is_function_constant(const T& t, variable::t closure_id) {
  if (t.current_unit.closures.mem(closure_id)) return t.current_unit.constant_closures.mem(closure_id);
  if (t.imported_units.closures.mem(closure_id)) return t.imported_units.constant_closures.mem(closure_id);
  fatal([&](format::Formatter& f) {
    format::fprintf(f, "Flambda_to_clambda: missing closure %a", pr(variable::print, closure_id));
  });
}

// Instrumentation of closure and field accesses to try to catch compiler
// bugs (-clambda-checks): a check primitive given the access and a
// constant string, the named expression printed
UConstant instrumentation_constant(T& t, F::named named_opt) {
  std::string str = "<none>";
  if (named_opt) {
    format::Formatter f;
    F::print_named(f, named_opt);
    f.print_flush();  // (Format.asprintf)
    str = f.contents();
  }
  std::string_view sym = compilenv::new_const_symbol();
  symbol::t sym2 = symbol::of_global_linkage(compilation_unit::get_current_exn(), sym);
  UStructuredConstant u{UStructuredConstant::Kind::Uconst_string};
  u.s = zstr(str);
  t.constants_for_instrumentation = t.constants_for_instrumentation.add(sym2, make<UStructuredConstant>(u));
  return uconst_ref(sym, nullptr);
}
ulambda check_closure(T& t, ulambda ulam, F::named named) {
  if (!clflags::clambda_checks) return ulam;
  Primitive p = prim(Primitive::K::Pccall);
  p.ccall = cmm_helpers::primitive_simple(OCAML_LIT("caml_check_value_is_closure"), 2, false);
  UConstant c = instrumentation_constant(t, named);
  return uprim(p, slice(std::vector<ulambda>{ulam, uconst(c)}), debuginfo::none());
}
ulambda check_field(T& t, ulambda ulam, long pos, F::named named_opt) {
  if (!clflags::clambda_checks) return ulam;
  Primitive p = prim(Primitive::K::Pccall);
  p.ccall = cmm_helpers::primitive_simple(OCAML_LIT("caml_check_field_access"), 3, false);
  UConstant c = instrumentation_constant(t, named_opt);
  return uprim(p, slice(std::vector<ulambda>{ulam, uconst(uconst_int(pos)), uconst(c)}), debuginfo::none());
}
F::named var_named(variable::t v) { return F::n_expr(F::var(v)); }

struct Env {
  variable::Map<ulambda> subst;
  variable::Map<Var> var;
  variable::Map<Var> mutable_var;  // Mutable_variable.Map
  symbol::Map<allocated_const::t> allocated_constant_for_symbol;

  Env add_subst(variable::t id, ulambda s) const {
    Env e = *this;
    e.subst = subst.add(id, s);
    return e;
  }
  std::pair<Var, Env> add_fresh_ident(variable::t v) const {
    Var id = Ident::create_local(v->name);
    Env e = *this;
    e.var = var.add(v, id);
    return {id, e};
  }
  std::pair<Var, Env> add_fresh_mutable_ident(variable::t mut_var) const {
    Var id = Ident::create_local(mut_var->name);
    Env e = *this;
    e.mutable_var = mutable_var.add(mut_var, id);
    return {id, e};
  }
  Env add_allocated_const(symbol::t sym, allocated_const::t c) const {
    Env e = *this;
    e.allocated_constant_for_symbol = allocated_constant_for_symbol.add(sym, c);
    return e;
  }
  Env keep_only_symbols() const {
    Env e;
    e.allocated_constant_for_symbol = allocated_constant_for_symbol;
    return e;
  }
};

ulambda subst_var(const Env& env, variable::t var) {
  if (const ulambda* s = env.subst.find_opt(var)) return *s;
  if (const Var* id = env.var.find_opt(var)) return uvar(*id);
  fatal([&](format::Formatter& f) {
    format::fprintf(f, "Flambda_to_clambda: unbound variable %a@.", pr(variable::print, var));
  });
}
Slice<ulambda> subst_vars(const Env& env, Slice<variable::t> vars) {
  std::vector<ulambda> r;
  for (variable::t v : vars) r.push_back(subst_var(env, v));
  return slice(r);
}

ulambda build_uoffset(ulambda ulam, long offset) {
  if (offset == 0) return ulam;
  return uoffset(ulam, offset);
}

const UStructuredConstant* to_clambda_allocated_constant(allocated_const::t c) {
  using K = AllocatedConst::Kind;
  using SK = UStructuredConstant::Kind;
  UStructuredConstant u{SK::Uconst_float};
  switch (c->kind) {
    case K::Float: u.f = c->f; break;
    case K::Int32: u.kind = SK::Uconst_int32; u.i = c->i; break;
    case K::Int64: u.kind = SK::Uconst_int64; u.i = c->i; break;
    case K::Nativeint: u.kind = SK::Uconst_nativeint; u.i = c->i; break;
    case K::Immutable_string: case K::String: u.kind = SK::Uconst_string; u.s = c->s; break;
    case K::Immutable_float_array: case K::Float_array: u.kind = SK::Uconst_float_array; u.floats = c->floats; break;
  }
  return make<UStructuredConstant>(u);
}

const UStructuredConstant* to_uconst_symbol(const Env& env, symbol::t sym) {
  const allocated_const::t* c = env.allocated_constant_for_symbol.find_opt(sym);
  if (!c) return nullptr;  // CR-soon mshinwell: Try to make this an error.
  using K = AllocatedConst::Kind;
  switch ((*c)->kind) {
    case K::Float: case K::Int32: case K::Int64: case K::Nativeint: return to_clambda_allocated_constant(*c);
    default: return nullptr;
  }
}

UConstant to_clambda_symbol_prime(const Env& env, symbol::t sym) {
  std::string_view lbl = symbol::label(sym);
  return uconst_ref(lbl, to_uconst_symbol(env, sym));
}
ulambda to_clambda_symbol(const Env& env, symbol::t sym) { return uconst(to_clambda_symbol_prime(env, sym)); }

UConstant to_clambda_const(const Env& env, const F::BlockField& c) {
  if (c.sym) return to_clambda_symbol_prime(env, c.sym);
  return uconst_int(c.c.n);  // Int i, or Char c's code
}

Primitive field_prim(long pos) {
  Primitive p = prim(Primitive::K::Pfield);
  p.n = pos;
  p.ptr = lambda::ImmediateOrPointer::Pointer;
  p.mut = MutableFlag::Mutable;
  return p;
}

struct Converter {
  T& t;

  ulambda to_clambda(const Env& env, F::t flam);
  ulambda to_clambda_named(const Env& env, variable::t var, F::named n);
  std::pair<Slice<long>, Slice<ulambda>> to_clambda_switch(const Env& env, Slice<F::SwitchCase> cases,
                                                           const F::IntSet& num_keys, F::t default_);
  ulambda to_clambda_direct_apply(variable::t func, Slice<variable::t> args, variable::t direct_func,
                                  const debuginfo::t& dbg, const Env& env);
  ulambda to_clambda_set_of_closures(const Env& env, const F::SetOfClosures* set);
  const UStructuredConstant* to_clambda_closed_set_of_closures(const Env& env, symbol::t symbol,
                                                               const F::SetOfClosures* set);
  // (fold_right over the parameters: the last one's ident made first)
  std::pair<Env, std::vector<Var>> fresh_params(Env env, Slice<Parameter> params) {
    std::vector<Var> ids(params.size());
    for (std::size_t k = params.size(); k-- > 0;) {
      auto [id, e] = env.add_fresh_ident(params[k].var);
      env = e;
      ids[k] = id;
    }
    return {env, ids};
  }
};

ulambda Converter::to_clambda(const Env& env, F::t flam) {
  switch (flam->kind) {
    case EK::Var: return subst_var(env, F::as<F::Var>(flam)->var);
    case EK::Let: {
      auto* l = F::as<F::Let>(flam);
      // TODO: synthesize proper value_kind
      auto [id, env_body] = env.add_fresh_ident(l->var);
      ulambda body = to_clambda(env_body, l->body);
      ulambda def = to_clambda_named(env, l->var, l->defining_expr);
      return ulet(MutableFlag::Immutable, lambda::ValueKind::gen(), vp(id), def, body);
    }
    case EK::Let_mutable: {
      auto* l = F::as<F::Let_mutable>(flam);
      auto [id, env_body] = env.add_fresh_mutable_ident(l->var);
      ulambda def = subst_var(env, l->initial_value);
      return ulet(MutableFlag::Mutable, l->contents_kind, vp(id), def, to_clambda(env_body, l->body));
    }
    case EK::Apply: {
      auto* a = F::as<F::Apply>(flam);
      // The closure _parameter_ of the function is added by cmmgen.  At the
      // call site, for a direct call, the closure argument must be
      // explicitly added (by [to_clambda_direct_apply]).  (flambda_to_clambda.ml)
      if (a->call_kind.direct) return to_clambda_direct_apply(a->func, a->args, a->call_kind.direct, a->dbg, env);
      ulambda callee = subst_var(env, a->func);
      Slice<ulambda> args = subst_vars(env, a->args);
      return ugeneric_apply(check_closure(t, callee, var_named(a->func)), args, a->dbg);
    }
    case EK::Switch: {
      auto* sw = F::as<F::Switch>(flam);
      // Check that the [failaction] may be duplicated.  If this is not the
      // case, share it through a static raise / static catch.
      if (sw->failaction && !F::as<F::Static_raise>(sw->failaction)) {
        static_exception::t exn = static_exception::create();
        F::t raise = F::static_raise(exn, {});
        F::t sw2 = F::switch_(sw->scrutinee, sw->numconsts, sw->consts, sw->numblocks, sw->blocks, raise);
        return to_clambda(env, F::static_catch(exn, {}, sw2, sw->failaction));
      }
      auto [const_index, const_actions] = to_clambda_switch(env, sw->consts, sw->numconsts, sw->failaction);
      auto [block_index, block_actions] = to_clambda_switch(env, sw->blocks, sw->numblocks, sw->failaction);
      USwitch u{const_index, const_actions, block_index, block_actions};
      return uswitch(subst_var(env, sw->scrutinee), u, debuginfo::none());  // debug info will be added by GPR#855
    }
    case EK::String_switch: {
      auto* ss = F::as<F::String_switch>(flam);
      ulambda arg = subst_var(env, ss->scrutinee);
      std::vector<UStringCase> sw;  // List.map: in order
      for (const F::StringCase& c : ss->cases) sw.push_back({c.s, to_clambda(env, c.action)});
      ulambda def = ss->def ? to_clambda(env, ss->def) : nullptr;
      return ustringswitch(arg, slice(sw), def);
    }
    case EK::Static_raise: {
      auto* r = F::as<F::Static_raise>(flam);
      return ustaticfail(r->exn, subst_vars(env, r->args));
    }
    case EK::Static_catch: {
      auto* c = F::as<F::Static_catch>(flam);
      Env env_handler = env;
      std::vector<UParam> ids(c->vars.size());
      for (std::size_t k = c->vars.size(); k-- > 0;) {  // List.fold_right
        auto [id, e] = env_handler.add_fresh_ident(c->vars[k].var);
        env_handler = e;
        ids[k] = UParam{vp(id), c->vars[k].kind};
      }
      ulambda handler = to_clambda(env_handler, c->handler);
      ulambda body = to_clambda(env, c->body);
      return ucatch(c->exn, slice(ids), body, handler);
    }
    case EK::Try_with: {
      auto* tw = F::as<F::Try_with>(flam);
      auto [id, env_handler] = env.add_fresh_ident(tw->var);
      ulambda handler = to_clambda(env_handler, tw->handler);
      ulambda body = to_clambda(env, tw->body);
      return utrywith(body, vp(id), handler);
    }
    case EK::If_then_else: {
      auto* i = F::as<F::If_then_else>(flam);
      ulambda ifnot = to_clambda(env, i->ifnot);
      ulambda ifso = to_clambda(env, i->ifso);
      return uifthenelse(subst_var(env, i->cond), ifso, ifnot);
    }
    case EK::While: {
      auto* w = F::as<F::While>(flam);
      ulambda body = to_clambda(env, w->body);
      ulambda cond = to_clambda(env, w->cond);
      return uwhile(cond, body);
    }
    case EK::For: {
      auto* f = F::as<F::For>(flam);
      auto [id, env_body] = env.add_fresh_ident(f->bound_var);
      ulambda body = to_clambda(env_body, f->body);
      return ufor(vp(id), subst_var(env, f->from_value), subst_var(env, f->to_value), f->direction, body);
    }
    case EK::Assign: {
      auto* a = F::as<F::Assign>(flam);
      const Var* id = env.mutable_var.find_opt(a->being_assigned);
      if (!id)
        fatal([&](format::Formatter& f) {
          format::fprintf(f, "Unbound mutable variable %a in [Assign]: %a", pr(variable::print, a->being_assigned),
                          pr(F::print, flam));
        });
      return uassign(*id, subst_var(env, a->new_value));
    }
    case EK::Send: {
      auto* s = F::as<F::Send>(flam);
      return usend(s->meth_kind, subst_var(env, s->meth), subst_var(env, s->obj), subst_vars(env, s->args), s->dbg);
    }
    case EK::Proved_unreachable: return uunreachable();
  }
  misc::fatal_error("Flambda_to_clambda.to_clambda");
}

ulambda Converter::to_clambda_named(const Env& env, variable::t var, F::named n) {
  switch (n->kind) {
    case NK::Symbol: return to_clambda_symbol(env, F::as<F::NSymbol>(n)->sym);
    case NK::Const: return uconst(uconst_int(F::as<F::NConst>(n)->c.n));
    case NK::Allocated_const:
      fatal([&](format::Formatter& f) {
        format::fprintf(f,
                        "[Allocated_const] should have been lifted to a [Let_symbol] construction before "
                        "[Flambda_to_clambda]: %a = %a",
                        pr(variable::print, var), pr(F::print_named, n));
      });
    case NK::Read_mutable: {
      variable::t mut_var = F::as<F::NRead_mutable>(n)->var;
      if (const Var* id = env.mutable_var.find_opt(mut_var)) return uvar(*id);
      fatal([&](format::Formatter& f) {
        format::fprintf(f, "Unbound mutable variable %a in [Read_mutable]: %a", pr(variable::print, mut_var),
                        pr(F::print_named, n));
      });
    }
    case NK::Read_symbol_field: {
      auto* r = F::as<F::NRead_symbol_field>(n);
      return uprim(field_prim(r->field), slice(std::vector<ulambda>{to_clambda_symbol(env, r->sym)}), debuginfo::none());
    }
    case NK::Set_of_closures: return to_clambda_set_of_closures(env, F::as<F::NSet_of_closures>(n)->set);
    case NK::Project_closure: {
      // Note that we must use [build_uoffset] to ensure that we do not
      // generate a [Uoffset] construction in the event that the offset is
      // zero, otherwise we might break pattern matches in Cmmgen (in
      // particular for the compilation of "let rec").
      const projection::ProjectClosure& pc = F::as<F::NProject_closure>(n)->p;
      long ofs = get_fun_offset(t, pc.closure_id);
      ulambda inner = check_closure(t, subst_var(env, pc.set_of_closures), var_named(pc.set_of_closures));
      return check_closure(t, build_uoffset(inner, ofs), n);
    }
    case NK::Move_within_set_of_closures: {
      const projection::MoveWithinSetOfClosures& m = F::as<F::NMove_within_set_of_closures>(n)->m;
      long ofs = get_fun_offset(t, m.move_to) - get_fun_offset(t, m.start_from);
      ulambda inner = check_closure(t, subst_var(env, m.closure), var_named(m.closure));
      return check_closure(t, build_uoffset(inner, ofs), n);
    }
    case NK::Project_var: {
      const projection::ProjectVar& pv = F::as<F::NProject_var>(n)->p;
      ulambda ulam = subst_var(env, pv.closure);
      long fun_offset = get_fun_offset(t, pv.closure_id);
      long var_offset = get_fv_offset(t, pv.var);
      long pos = var_offset - fun_offset;
      ulambda checked = check_field(t, check_closure(t, ulam, var_named(pv.closure)), pos, n);
      return uprim(field_prim(pos), slice(std::vector<ulambda>{checked}), debuginfo::none());
    }
    case NK::Prim: {
      auto* p = F::as<F::NPrim>(n);
      using K = Primitive::K;
      if (p->prim->kind == K::Pfield && p->args.size() == 1)
        return uprim(*p->prim, slice(std::vector<ulambda>{check_field(t, subst_var(env, p->args[0]), p->prim->n, nullptr)}),
                     p->dbg);
      if (p->prim->kind == K::Psetfield && p->args.size() == 2) {
        ulambda v = subst_var(env, p->args[1]);
        ulambda b = check_field(t, subst_var(env, p->args[0]), p->prim->n, nullptr);
        return uprim(*p->prim, slice(std::vector<ulambda>{b, v}), p->dbg);
      }
      return uprim(*p->prim, subst_vars(env, p->args), p->dbg);
    }
    case NK::Expr: return to_clambda(env, F::as<F::NExpr>(n)->expr);
  }
  misc::fatal_error("Flambda_to_clambda.to_clambda_named");
}

std::pair<Slice<long>, Slice<ulambda>> Converter::to_clambda_switch(const Env& env, Slice<F::SwitchCase> cases,
                                                                    const F::IntSet& num_keys_set, F::t default_) {
  long num_keys = num_keys_set.is_empty() ? 0 : *num_keys_set.max_elt() + 1;
  ::cppcaml::typing::switch_::Store<flambda_utils::SwitchStorerPolicy> store;
  long default_action = default_ && static_cast<long>(cases.size()) < num_keys ? store.act_store(default_) : -1;
  std::vector<long> index(static_cast<std::size_t>(num_keys), default_action);
  long smallest_key = num_keys;
  for (const F::SwitchCase& c : cases) {
    index[static_cast<std::size_t>(c.key)] = store.act_store(c.action);
    smallest_key = std::min(c.key, smallest_key);
  }
  if (smallest_key < num_keys) {
    long action = index[static_cast<std::size_t>(smallest_key)];
    for (long& act : index) {
      if (act >= 0) action = act;
      else act = action;
    }
  }
  std::vector<ulambda> actions;  // Array.map: in order
  for (F::t a : store.act_get()) actions.push_back(to_clambda(env, a));
  if (actions.empty()) return {{}, {}};  // May happen when [default] is [None].
  return {slice(index), slice(actions)};
}

ulambda Converter::to_clambda_direct_apply(variable::t func, Slice<variable::t> args, variable::t direct_func,
                                           const debuginfo::t& dbg, const Env& env) {
  bool closed = is_function_constant(t, direct_func);
  std::string_view label = compilenv::function_label(direct_func);
  std::vector<ulambda> uargs;
  for (variable::t v : args) uargs.push_back(subst_var(env, v));
  // Remove the closure argument if the closure is closed.  (Note that the
  // closure argument is always a variable, so we can be sure we are not
  // dropping any side effects.)
  if (!closed) uargs.push_back(subst_var(env, func));
  return udirect_apply(label, slice(uargs), dbg);
}

ulambda Converter::to_clambda_set_of_closures(const Env& env, const F::SetOfClosures* set) {
  std::vector<std::pair<variable::t, const F::FunctionDeclaration*>> all_functions;  // Map.bindings
  set->function_decls->funs.iter(
      [&](variable::t id, const F::FunctionDeclaration* d) { all_functions.emplace_back(id, d); });
  Var env_var = Ident::create_local(OCAML_LIT("env"));
  std::vector<const UFunction*> funs;  // List.map: in order
  for (const auto& [closure_id, function_decl] : all_functions) {
    long fun_offset = *t.current_unit.fun_offset_table.find_opt(closure_id);
    // Inside the body of the function, we cannot access variables declared
    // outside, so start with a suitably clean environment.
    Env fenv = env.keep_only_symbols();
    // Add the Clambda expressions for the free variables of the function to
    // the environment.
    set->free_vars.iter([&](variable::t id, const F::SpecialisedTo&) {
      const long* var_offset = t.current_unit.fv_offset_table.find_opt(id);
      if (!var_offset)
        fatal([&](format::Formatter& f) {
          format::fprintf(f,
                          "Clambda.to_clambda_set_of_closures: offset for free variable %a is unknown.  Set of "
                          "closures: %a",
                          pr(variable::print, id), pr(F::print_set_of_closures, set));
        });
      long pos = *var_offset - fun_offset;
      fenv = fenv.add_subst(id, uprim(field_prim(pos), slice(std::vector<ulambda>{uvar(env_var)}), debuginfo::none()));
    });
    // Add the Clambda expressions for all functions defined in the current
    // set of closures to the environment.
    for (const auto& [id, _] : all_functions) {
      long offset = *t.current_unit.fun_offset_table.find_opt(id);
      fenv = fenv.add_subst(id, uoffset(uvar(env_var), offset - fun_offset));
    }
    auto [env_body, params] = fresh_params(fenv, function_decl->params);
    std::vector<UParam> ps;
    for (Var p : params) ps.push_back({vp(p), lambda::ValueKind::gen()});
    ps.push_back({vp(env_var), lambda::ValueKind::gen()});
    UFunction u;
    u.body = to_clambda(env_body, function_decl->body);
    u.label = compilenv::function_label(closure_id);
    u.arity = flambda_utils::function_arity(function_decl);
    u.params = slice(ps);
    u.return_ = lambda::ValueKind::gen();
    u.dbg = function_decl->dbg;
    u.env = env_var;
    u.poll = function_decl->poll;
    funs.push_back(make<UFunction>(u));
  }
  std::vector<ulambda> free_vars;
  set->free_vars.iter([&](variable::t, const F::SpecialisedTo& s) { free_vars.push_back(subst_var(env, s.var)); });
  return uclosure(slice(funs), slice(free_vars));
}

const UStructuredConstant* Converter::to_clambda_closed_set_of_closures(const Env& env, symbol::t symbol,
                                                                        const F::SetOfClosures* set) {
  std::vector<std::pair<variable::t, const F::FunctionDeclaration*>> functions;  // Map.bindings
  set->function_decls->funs.iter([&](variable::t id, const F::FunctionDeclaration* d) { functions.emplace_back(id, d); });
  std::vector<const UFunction*> ufunct;  // List.map: in order
  for (const auto& [id, function_decl] : functions) {
    // All that we need in the environment, for translating one closure from
    // a closed set of closures, is the substitutions for variables bound to
    // the various closures in the set.  Such closures will always be
    // referenced via symbols.
    Env fenv = env.keep_only_symbols();
    for (const auto& [var, _] : functions)
      fenv = fenv.add_subst(var, to_clambda_symbol(fenv, compilenv::closure_symbol(var)));
    auto [env_body, params] = fresh_params(fenv, function_decl->params);
    ulambda body = un_anf::apply(symbol, *t.ppf_dump, to_clambda(env_body, function_decl->body));
    std::vector<UParam> ps;
    for (Var p : params) ps.push_back({vp(p), lambda::ValueKind::gen()});
    UFunction u;
    u.label = compilenv::function_label(id);
    u.arity = flambda_utils::function_arity(function_decl);
    u.params = slice(ps);
    u.return_ = lambda::ValueKind::gen();
    u.body = body;
    u.dbg = function_decl->dbg;
    u.env = nullptr;
    u.poll = function_decl->poll;
    ufunct.push_back(make<UFunction>(u));
  }
  UStructuredConstant c{UStructuredConstant::Kind::Uconst_closure};
  c.funs = slice(ufunct);
  c.s = symbol::label(symbol);
  return make<UStructuredConstant>(c);
}

}  // namespace

Result convert(format::Formatter& ppf_dump, const F::Program& program, const export_info::Transient& exported_transient) {
  ForOneOrMoreUnits current_unit;
  current_unit.closures = flambda_utils::make_closure_map(program).keys();
  current_unit.constant_closures = flambda_utils::all_lifted_constant_closures(program);
  closure_offsets::Result offsets = closure_offsets::compute(program);
  current_unit.fun_offset_table = offsets.function_offsets;
  current_unit.fv_offset_table = offsets.free_variable_offsets;
  ForOneOrMoreUnits imported_units;
  const export_info::T* imported = compilenv::approx_env();
  imported_units.closures = imported->sets_of_closures.fold(
      [](set_of_closures_id::t, const simple_value_approx::FunctionDeclarations* fun_decls, variable::Set acc) {
        return fun_decls->funs.fold(
            [](variable::t var, const simple_value_approx::FunctionDeclaration*, variable::Set a) { return a.add(var); },
            acc);
      },
      variable::Set{});
  imported_units.fun_offset_table = imported->offset_fun;
  imported_units.fv_offset_table = imported->offset_fv;
  imported_units.constant_closures = imported->constant_closures;
  T t{current_unit, imported_units, &ppf_dump, {}};
  Converter cv{t};

  // to_clambda_program: each element's code before the rest's
  Env env;
  symbol::Map<const UStructuredConstant*> constants;
  std::vector<ulambda> codes;
  std::vector<PreallocatedBlock> preallocated_blocks;
  auto accumulate = [&](const Env& e, symbol::t symbol, F::constant_defining_value c) {
    using K = F::ConstantDefiningValue::Kind;
    switch (c->kind) {
      case K::Allocated_const: constants = constants.add(symbol, to_clambda_allocated_constant(c->c)); break;
      case K::Block: {
        std::vector<UConstant> fields;  // List.map: in order
        for (const F::BlockField& f : c->fields) fields.push_back(to_clambda_const(e, f));
        UStructuredConstant u{UStructuredConstant::Kind::Uconst_block};
        u.tag = c->tag;
        u.fields = slice(fields);
        constants = constants.add(symbol, make<UStructuredConstant>(u));
        break;
      }
      case K::Set_of_closures:
        constants = constants.add(symbol, cv.to_clambda_closed_set_of_closures(e, symbol, c->set));
        break;
      case K::Project_closure: break;
    }
  };
  F::program_body p = program.program_body;
  for (; p->kind != F::ProgramBody::Kind::End; p = p->body) {
    switch (p->kind) {
      case F::ProgramBody::Kind::Let_symbol:
        // Useful only for unboxing.  Since floats and boxed integers will
        // never be part of a Let_rec_symbol, handling only the Let_symbol is
        // sufficient.
        if (p->def->kind == F::ConstantDefiningValue::Kind::Allocated_const)
          env = env.add_allocated_const(p->sym, p->def->c);
        accumulate(env, p->sym, p->def);
        break;
      case F::ProgramBody::Kind::Let_rec_symbol:
        for (const F::SymbolBinding& b : p->defs) accumulate(env, b.sym, b.def);
        break;
      case F::ProgramBody::Kind::Initialize_symbol: {
        std::vector<std::pair<long, F::t>> init_fields;
        std::vector<std::optional<UConstantBlockField>> constant_fields;
        for (std::size_t i = 0; i < p->fields.size(); ++i) {
          std::optional<F::BlockField> cf = initialize_symbol_to_let_symbol::constant_field(p->fields[i]);
          if (!cf) {
            init_fields.emplace_back(static_cast<long>(i), p->fields[i]);
            constant_fields.push_back(std::nullopt);
          } else if (cf->sym) {
            constant_fields.push_back(UConstantBlockField{true, symbol::label(cf->sym), 0});
          } else {
            constant_fields.push_back(UConstantBlockField{false, {}, cf->c.n});
          }
        }
        // to_clambda_initialize_symbol
        std::vector<std::pair<long, ulambda>> fields;  // List.map: in order
        for (const auto& [index, expr] : init_fields) fields.emplace_back(index, cv.to_clambda(env, expr));
        auto build_setfield = [&](long index, ulambda field) {
          // Note that this will never cause a write barrier hit, owing to
          // the [Initialization].
          Primitive sp = prim(Primitive::K::Psetfield);
          sp.n = index;
          sp.ptr = lambda::ImmediateOrPointer::Pointer;
          sp.init = lambda::InitializationOrAssignment::Root_initialization;
          return uprim(sp, slice(std::vector<ulambda>{to_clambda_symbol(env, p->sym), field}), debuginfo::none());
        };
        ulambda e1;
        if (fields.empty()) {
          e1 = uconst(uconst_int(0));
        } else {
          e1 = build_setfield(fields[0].first, fields[0].second);
          for (std::size_t k = 1; k < fields.size(); ++k)
            e1 = usequence(build_setfield(fields[k].first, fields[k].second), e1);
        }
        codes.push_back(e1);
        PreallocatedBlock b;
        b.symbol = symbol::label(p->sym);
        b.exported = true;
        b.tag = p->tag;
        b.fields = slice(constant_fields);
        b.provenance = nullptr;
        preallocated_blocks.push_back(b);
        break;
      }
      case F::ProgramBody::Kind::Effect: codes.push_back(cv.to_clambda(env, p->expr)); break;
      case F::ProgramBody::Kind::End: break;
    }
  }
  ulambda expr = uconst(uconst_int(0));
  for (std::size_t k = codes.size(); k-- > 0;) expr = usequence(codes[k], expr);
  Result r;
  r.expr = expr;
  r.preallocated_blocks = std::move(preallocated_blocks);
  // Symbol.Map.disjoint_union structured_constants t.constants_for_instrumentation
  constants = symbol::Map<const UStructuredConstant*>::union_(
      [](symbol::t, const UStructuredConstant*, const UStructuredConstant*) -> std::optional<const UStructuredConstant*> {
        misc::fatal_error("Flambda_to_clambda: Map.disjoint_union");
      },
      constants, t.constants_for_instrumentation);
  constants.iter([&](symbol::t s, const UStructuredConstant* c) { r.structured_constants.emplace_back(s, c); });
  r.exported = export_info::t_of_transient(exported_transient, current_unit.fun_offset_table,
                                           current_unit.fv_offset_table, imported_units.fun_offset_table,
                                           imported_units.fv_offset_table, current_unit.constant_closures);
  return r;
}

}  // namespace cppcaml::typing::flambda_to_clambda
