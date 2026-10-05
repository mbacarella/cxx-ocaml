// Port of middle_end/flambda/flambda_invariants.ml (see
// flambda_invariants.hpp).
#include "cppcaml/typing/flambda_invariants.hpp"

#include <optional>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/printclambda.hpp"

namespace cppcaml::typing::flambda_invariants {

using namespace flambda;
using format::Formatter;
using format::fprintf;

namespace {

// a violated invariant: its message (Format.eprintf's), printed by check_exn
struct Violation {
  std::function<void(Formatter&)> describe;
};
// Assert_failure ("middle_end/flambda/flambda_invariants.ml", line, col)
[[noreturn]] void assert_failure(long line, long col) {
  throw Failed{"File \"middle_end/flambda/flambda_invariants.ml\", line " + std::to_string(line) + ", characters " +
               std::to_string(col) + "-" + std::to_string(col + 6) + ": Assertion failed"};
}
template <class F>
[[noreturn]] void violation(F&& describe) {
  throw Violation{std::forward<F>(describe)};
}
auto pv(variable::t v) {
  return [v](Formatter& f) { variable::print(f, v); };
}
auto pvs(const variable::Set& s) {
  return [s](Formatter& f) { variable::print_set(f, s); };
}
auto psym(symbol::t s) {
  return [s](Formatter& f) { symbol::print(f, s); };
}

// (var_env, mut_var_env, sym_env)
struct Env {
  variable::Set var;
  variable::Set mut_var;  // Mutable_variable.Set
  symbol::Set sym;
};

void variable_and_symbol_invariants(const Program& program) {
  variable::Set all_declared_variables;
  auto declare_variable = [&](variable::t var) {
    if (all_declared_variables.mem(var))
      violation([var](Formatter& f) {
        fprintf(f, ">> Binding occurrence of variable that was already bound: %a", pv(var));
      });
    all_declared_variables = all_declared_variables.add(var);
  };
  auto declare_variables = [&](const variable::Set& vars) { vars.iter(declare_variable); };
  variable::Set all_declared_mutable_variables;
  auto declare_mutable_variable = [&](variable::t mut_var) {
    if (all_declared_mutable_variables.mem(mut_var))
      violation([mut_var](Formatter& f) {
        fprintf(f, ">> Binding occurrence of mutable variable that was already bound: %a", pv(mut_var));
      });
    all_declared_mutable_variables = all_declared_mutable_variables.add(mut_var);
  };
  auto in_current_unit = [](variable::t v) {
    return compilation_unit::equal(variable::get_compilation_unit(v), compilation_unit::get_current_exn());
  };
  auto add_binding_occurrence = [&](Env env, variable::t var) {
    if (!in_current_unit(var))
      violation([var](Formatter& f) {
        fprintf(f, ">> Binding occurrence of variable marked as not being from the current compilation unit: %a",
                pv(var));
      });
    declare_variable(var);
    env.var = env.var.add(var);
    return env;
  };
  auto add_mutable_binding_occurrence = [&](Env env, variable::t mut_var) {
    if (!in_current_unit(mut_var))
      violation([mut_var](Formatter& f) {
        fprintf(f,
                ">> Binding occurrence of mutable variable marked as not being from the current compilation unit: "
                "%a",
                pv(mut_var));
      });
    declare_mutable_variable(mut_var);
    env.mut_var = env.mut_var.add(mut_var);
    return env;
  };
  auto add_binding_occurrence_of_symbol = [&](Env env, symbol::t sym) {
    if (env.sym.mem(sym))
      violation([sym](Formatter& f) {
        fprintf(f, ">> Binding occurrence of symbol that was already bound: %a", psym(sym));
      });
    env.sym = env.sym.add(sym);
    return env;
  };
  auto check_variable_is_bound = [&](const Env& env, variable::t var) {
    if (!env.var.mem(var)) violation([var](Formatter& f) { fprintf(f, ">> Unbound variable: %a", pv(var)); });
  };
  auto check_symbol_is_bound = [&](const Env& env, symbol::t sym) {
    // (the message's backtrace, Printexc.get_callstack's, is not reproduced)
    if (!env.sym.mem(sym)) violation([sym](Formatter& f) { fprintf(f, ">> Unbound symbol: %a %s", psym(sym), ""); });
  };
  auto check_variables_are_bound = [&](const Env& env, Slice<variable::t> vars) {
    for (variable::t v : vars) check_variable_is_bound(env, v);
  };
  auto check_mutable_variable_is_bound = [&](const Env& env, variable::t mut_var) {
    if (!env.mut_var.mem(mut_var))
      violation([mut_var](Formatter& f) { fprintf(f, ">> Unbound mutable variable: %a", pv(mut_var)); });
  };
  std::function<void(Env, t)> loop;
  std::function<void(const Env&, named)> loop_named;
  std::function<void(const Env&, const SetOfClosures*)> loop_set_of_closures;
  loop = [&](Env env, t flam) {
    for (;;) {
      switch (flam->kind) {
        // Expressions that can bind [Variable.t]s:
        case EK::Let: {
          auto* l = as<Let>(flam);
          loop_named(env, l->defining_expr);
          env = add_binding_occurrence(env, l->var);
          flam = l->body;
          continue;
        }
        case EK::Let_mutable: {
          auto* l = as<Let_mutable>(flam);
          check_variable_is_bound(env, l->initial_value);
          env = add_mutable_binding_occurrence(env, l->var);
          flam = l->body;
          continue;
        }
        case EK::For: {
          auto* f = as<For>(flam);
          check_variable_is_bound(env, f->from_value);
          check_variable_is_bound(env, f->to_value);
          env = add_binding_occurrence(env, f->bound_var);
          flam = f->body;
          continue;
        }
        case EK::Static_catch: {
          auto* c = as<Static_catch>(flam);
          loop(env, c->body);
          for (const CatchVar& v : c->vars) env = add_binding_occurrence(env, v.var);  // List.fold_left
          flam = c->handler;
          continue;
        }
        case EK::Try_with: {
          auto* tw = as<Try_with>(flam);
          loop(env, tw->body);
          env = add_binding_occurrence(env, tw->var);
          flam = tw->handler;
          continue;
        }
        // Everything else:
        case EK::Var: check_variable_is_bound(env, as<Var>(flam)->var); return;
        case EK::Apply: {
          auto* a = as<Apply>(flam);
          check_variable_is_bound(env, a->func);
          check_variables_are_bound(env, a->args);
          return;
        }
        case EK::Assign: {
          auto* a = as<Assign>(flam);
          check_mutable_variable_is_bound(env, a->being_assigned);
          check_variable_is_bound(env, a->new_value);
          return;
        }
        case EK::Send: {
          auto* s = as<Send>(flam);
          check_variable_is_bound(env, s->meth);
          check_variable_is_bound(env, s->obj);
          check_variables_are_bound(env, s->args);
          return;
        }
        case EK::If_then_else: {
          auto* i = as<If_then_else>(flam);
          check_variable_is_bound(env, i->cond);
          loop(env, i->ifso);
          flam = i->ifnot;
          continue;
        }
        case EK::Switch: {
          auto* sw = as<Switch>(flam);
          check_variable_is_bound(env, sw->scrutinee);
          for (const SwitchCase& c : sw->consts) loop(env, c.action);
          for (const SwitchCase& c : sw->blocks) loop(env, c.action);
          if (sw->failaction) loop(env, sw->failaction);
          return;
        }
        case EK::String_switch: {
          auto* ss = as<String_switch>(flam);
          check_variable_is_bound(env, ss->scrutinee);
          for (const StringCase& c : ss->cases) loop(env, c.action);
          if (ss->def) loop(env, ss->def);
          return;
        }
        case EK::Static_raise:
          for (variable::t v : as<Static_raise>(flam)->args) check_variable_is_bound(env, v);
          return;
        case EK::While: {
          auto* w = as<While>(flam);
          loop(env, w->cond);
          flam = w->body;
          continue;
        }
        case EK::Proved_unreachable: return;
      }
      return;
    }
  };
  loop_named = [&](const Env& env, named n) {
    switch (n->kind) {
      case NK::Symbol: check_symbol_is_bound(env, as<NSymbol>(n)->sym); break;
      case NK::Const: case NK::Allocated_const: break;
      case NK::Read_mutable: check_mutable_variable_is_bound(env, as<NRead_mutable>(n)->var); break;
      case NK::Read_symbol_field: {
        auto* r = as<NRead_symbol_field>(n);
        check_symbol_is_bound(env, r->sym);
        if (!(r->field >= 0)) assert_failure(227, 6);
        break;
      }
      case NK::Set_of_closures: loop_set_of_closures(env, as<NSet_of_closures>(n)->set); break;
      case NK::Project_closure: check_variable_is_bound(env, as<NProject_closure>(n)->p.set_of_closures); break;
      case NK::Move_within_set_of_closures:
        check_variable_is_bound(env, as<NMove_within_set_of_closures>(n)->m.closure);
        break;
      case NK::Project_var: check_variable_is_bound(env, as<NProject_var>(n)->p.closure); break;
      case NK::Prim: check_variables_are_bound(env, as<NPrim>(n)->args); break;
      case NK::Expr: loop(env, as<NExpr>(n)->expr); break;
    }
  };
  loop_set_of_closures = [&](const Env& env, const SetOfClosures* set_of_closures) {
    const variable::Map<const FunctionDeclaration*>& funs = set_of_closures->function_decls->funs;
    variable::Set functions_in_closure = funs.keys();
    variable::Set variables_in_closure;
    set_of_closures->free_vars.iter([&](variable::t var, const SpecialisedTo& var_in_closure) {
      // [var] may occur in the body, but will effectively be renamed to
      // [var_in_closure], so the latter is what we check to make sure it's
      // bound.
      check_variable_is_bound(env, var_in_closure.var);
      variables_in_closure = variables_in_closure.add(var);
    });
    variable::Set all_params, all_free_vars;
    funs.iter([&](variable::t fun_var, const FunctionDeclaration* function_decl) {
      if (!functions_in_closure.mem(fun_var)) assert_failure(277, 12);
      // Check that [free_variables], which is only present as an
      // optimization, is not lying.
      variable::Set free_variables2 = free_variables(function_decl->body);
      const variable::Set& claimed = function_decl->free_variables;
      if (!variable::Set::subset(free_variables2, claimed))
        violation([=](Formatter& f) {
          fprintf(f,
                  ">> Function declaration whose [free_variables] set (%a) is not a superset of the result of "
                  "[Flambda.free_variables] applied to the body of the function (%a).  Declaration: %a",
                  pvs(claimed), pvs(free_variables2),
                  [=](Formatter& g) { print_function_declaration_pub(g, fun_var, function_decl); });
        });
      // Check that every variable free in the body of the function is
      // bound by either the set of closures or the parameter list.
      variable::Set params = parameter::set_vars(function_decl->params);
      variable::Set acceptable_free_variables = variable::Set::union_(
          variable::Set::union_(variables_in_closure, functions_in_closure), params);
      variable::Set bad = variable::Set::diff(claimed, acceptable_free_variables);
      if (!bad.is_empty())
        violation([=](Formatter& f) {
          fprintf(f,
                  ">> Variable(s) (%a) in the body of a function declaration (fun_var = %a) that is not bound by "
                  "either the closure or the function's parameter list.  Set of closures: %a",
                  pvs(bad), pv(fun_var), [=](Formatter& g) { print_set_of_closures(g, set_of_closures); });
        });
      // Check that parameters are unique across all functions in the
      // declaration.
      long old_all_params_size = all_params.cardinal();
      long params_size = params.cardinal();
      all_params = variable::Set::union_(all_params, params);
      if (all_params.cardinal() != old_all_params_size + params_size) {
        variable::Set ap = all_params;
        violation([ap](Formatter& f) { fprintf(f, ">> Function declarations whose parameters overlap: %a", pvs(ap)); });
      }
      // Check that parameters and function variables are not bound
      // somewhere else in the program
      declare_variables(params);
      declare_variable(fun_var);
      // Check that the body of the functions is correctly structured
      // (Mutable variables cannot be captured by closures)
      Env body_env{env.var, {}, env.sym};
      claimed.iter([&](variable::t v) { body_env.var = body_env.var.add(v); });  // Set.fold add
      loop(body_env, function_decl->body);
      all_free_vars = variable::Set::union_(claimed, all_free_vars);
    });
    // Check that free variables are not bound somewhere else in the program
    declare_variables(set_of_closures->free_vars.keys());
    // Check that every "specialised arg" is a parameter of one of the
    // functions being declared, and that the variable to which the
    // parameter is being specialised is bound.
    set_of_closures->free_vars.iter([&](variable::t, const SpecialisedTo& s) {
      check_variable_is_bound(env, s.var);
      if (s.projection && !set_of_closures->free_vars.mem(projection::projecting_from(s.projection))) {
        projection::t p = s.projection;
        violation([p](Formatter& f) {
          fprintf(f,
                  ">> Projection %a in [free_vars] from a variable that is not a (inner) free variable of the set of "
                  "closures",
                  [p](Formatter& g) { projection::print(g, p); });
        });
      }
    });
    set_of_closures->specialised_args.iter([&](variable::t being_specialised, const SpecialisedTo& s) {
      if (!all_params.mem(being_specialised))
        violation([being_specialised](Formatter& f) {
          fprintf(f,
                  ">> Variable in [specialised_args] that is not a parameter of any of the function(s) in the "
                  "corresponding declaration(s): %a",
                  pv(being_specialised));
        });
      check_variable_is_bound(env, s.var);
      if (s.projection && !set_of_closures->specialised_args.mem(projection::projecting_from(s.projection))) {
        projection::t p = s.projection;
        violation([p](Formatter& f) {
          fprintf(f,
                  ">> Projection %a in [specialised_args] from a variable that is not a (inner) specialised "
                  "argument variable of the set of closures",
                  [p](Formatter& g) { projection::print(g, p); });
        });
      }
    });
  };
  auto loop_constant_defining_value = [&](const Env& env, constant_defining_value c) {
    using K = ConstantDefiningValue::Kind;
    switch (c->kind) {
      case K::Allocated_const: break;
      case K::Block:
        for (const BlockField& f : c->fields)
          if (f.sym) check_symbol_is_bound(env, f.sym);
        break;
      case K::Set_of_closures:
        loop_set_of_closures(env, c->set);
        // Constant set of closures must not have free variables
        if (!c->set->free_vars.is_empty()) assert_failure(401, 8);
        if (!c->set->specialised_args.is_empty()) assert_failure(403, 8);
        break;
      case K::Project_closure: check_symbol_is_bound(env, c->sym); break;
    }
  };
  Env env;
  program.imported_symbols.iter([&](symbol::t s) { env = add_binding_occurrence_of_symbol(env, s); });
  program_body p = program.program_body;
  for (;; p = p->body) {
    switch (p->kind) {
      case ProgramBody::Kind::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) env = add_binding_occurrence_of_symbol(env, b.sym);
        for (const SymbolBinding& b : p->defs) loop_constant_defining_value(env, b.def);
        continue;
      case ProgramBody::Kind::Let_symbol:
        loop_constant_defining_value(env, p->def);
        env = add_binding_occurrence_of_symbol(env, p->sym);
        continue;
      case ProgramBody::Kind::Initialize_symbol:
        for (t f : p->fields) loop(env, f);
        env = add_binding_occurrence_of_symbol(env, p->sym);
        continue;
      case ProgramBody::Kind::Effect: loop(env, p->expr); continue;
      case ProgramBody::Kind::End: check_symbol_is_bound(env, p->sym); return;
    }
  }
}

void primitive_invariants(t flam) {
  flambda_iterators::iter_named(
      [](named n) {
        if (auto* p = as<NPrim>(n);
            p && (p->prim->kind == clambda::Primitive::K::Psequand || p->prim->kind == clambda::Primitive::K::Psequor)) {
          const clambda::Primitive* prim = p->prim;
          violation([prim](Formatter& f) {
            fprintf(f, ">> Sequential logical operator primitives must be expanded (see closure_conversion.ml): %a",
                    [prim](Formatter& g) { printclambda::primitive(g, *prim); });
          });
        }
      },
      flam);
}

// (declared Var_within_closures and Closure_ids: those bound twice, the
// last such)
std::pair<variable::Set, variable::t> declared(const Program& program, bool closure_ids) {
  variable::Set bound;
  variable::t bound_multiple_times = nullptr;
  flambda_iterators::iter_on_set_of_closures_of_program(program, [&](bool, const SetOfClosures* s) {
    auto add = [&](variable::t v) {
      if (bound.mem(v)) bound_multiple_times = v;
      bound = bound.add(v);
    };
    if (closure_ids) s->function_decls->funs.iter([&](variable::t id, const FunctionDeclaration*) { add(id); });
    else s->free_vars.iter([&](variable::t id, const SpecialisedTo&) { add(id); });
  });
  return {bound, bound_multiple_times};
}

void every_declared_closure_is_from_current_compilation_unit(t flam) {
  compilation_unit::t current = compilation_unit::get_current_exn();
  flambda_iterators::iter_on_sets_of_closures(
      [&](const SetOfClosures* s) {
        compilation_unit::t cu = s->function_decls->set_of_closures_id->unit;
        if (!compilation_unit::equal(cu, current))
          violation([cu](Formatter& f) {
            fprintf(f, ">> Closure declared as being from another compilation unit: %a",
                    [cu](Formatter& g) { compilation_unit::print(g, cu); });
          });
      },
      flam);
}

void every_static_exception_is_caught(t flam) {
  std::function<void(const static_exception::Set&, t)> loop = [&](const static_exception::Set& env, t e) {
    if (auto* c = as<Static_catch>(e)) {
      static_exception::Set env2 = env.add(c->exn);
      loop(env2, c->handler);
      loop(env2, c->body);
      return;
    }
    if (auto* r = as<Static_raise>(e); r && !env.mem(r->exn)) {
      long exn = r->exn;
      violation([exn](Formatter& f) { fprintf(f, ">> Uncaught static exception: %d", exn); });
    }
    flambda_iterators::apply_on_subexpressions([&](t sub) { loop(env, sub); }, [](named) {}, e);
  };
  loop({}, flam);
}

void every_static_exception_is_caught_at_a_single_position(t flam) {
  static_exception::Set caught;
  flambda_iterators::iter(
      [&](t e) {
        if (auto* c = as<Static_catch>(e)) {
          if (caught.mem(c->exn)) {
            long exn = c->exn;
            violation([exn](Formatter& f) { fprintf(f, ">> Static exception caught in multiple places: %d", exn); });
          }
          caught = caught.add(c->exn);
        }
      },
      [](named) {}, flam);
}

void check(const Program& flam) {
  variable_and_symbol_invariants(flam);
  // no_closure_id_is_bound_multiple_times
  if (variable::t id = declared(flam, true).second)
    violation([id](Formatter& f) { fprintf(f, ">> Closure ID is bound multiple times: %a", pv(id)); });
  // no_set_of_closures_id_is_bound_multiple_times
  {
    set_of_closures_id::Set bound;
    set_of_closures_id::t twice = nullptr;
    flambda_iterators::iter_on_set_of_closures_of_program(flam, [&](bool, const SetOfClosures* s) {
      set_of_closures_id::t id = s->function_decls->set_of_closures_id;
      if (bound.mem(id)) twice = id;
      bound = bound.add(id);
    });
    if (twice)
      violation([twice](Formatter& f) {
        fprintf(f, ">> Set of closures ID is bound multiple times: %a", [twice](Formatter& g) { unit_id::print(g, twice); });
      });
  }
  compilation_unit::t current = compilation_unit::get_current_exn();
  auto in_current = [&](variable::t v) { return compilation_unit::equal(variable::get_compilation_unit(v), current); };
  // every_used_function_from_current_compilation_unit_is_declared
  {
    variable::Set declared_ids = declared(flam, true).first;
    variable::Set used;
    flambda_iterators::iter_named_of_program(flam, [&](named n) {
      if (auto* pc = as<NProject_closure>(n)) used = used.add(pc->p.closure_id);
      else if (auto* m = as<NMove_within_set_of_closures>(n)) used = used.add(m->m.start_from).add(m->m.move_to);
      else if (auto* pv2 = as<NProject_var>(n)) used = used.add(pv2->p.closure_id);
    });
    variable::Set counter_examples = variable::Set::diff(used.filter(in_current), declared_ids);
    if (!counter_examples.is_empty())
      violation([counter_examples](Formatter& f) {
        fprintf(f, ">> Unbound closure ID(s) from the current compilation unit: %a", pvs(counter_examples));
      });
  }
  // no_var_within_closure_is_bound_multiple_times
  auto [declared_vars, twice] = declared(flam, false);
  if (twice)
    violation([twice](Formatter& f) { fprintf(f, ">> Variable within a closure is bound multiple times: %a", pv(twice)); });
  // every_used_var_within_closure_from_current_compilation_unit_is_declared
  {
    variable::Set used;
    flambda_iterators::iter_named_of_program(flam, [&](named n) {
      if (auto* pv2 = as<NProject_var>(n)) used = used.add(pv2->p.var);
    });
    variable::Set counter_examples = variable::Set::diff(used.filter(in_current), declared_vars);
    if (!counter_examples.is_empty())
      violation([counter_examples](Formatter& f) {
        fprintf(f, ">> Unbound variable(s) within closure(s) from the current compilation_unit: %a",
                pvs(counter_examples));
      });
  }
  flambda_iterators::iter_exprs_at_toplevel_of_program(flam, [](t e) {
    primitive_invariants(e);
    every_static_exception_is_caught(e);
    every_static_exception_is_caught_at_a_single_position(e);
    every_declared_closure_is_from_current_compilation_unit(e);
  });
}

}  // namespace

void check_exn(const Program& program) {
  try {
    check(program);
  } catch (const Violation& v) {
    Formatter& ppf = location::err_formatter();
    v.describe(ppf);
    fprintf(ppf, "\n@?");
    throw Failed{"Flambda_invariants.Flambda_invariants_failed"};
  }
}

}  // namespace cppcaml::typing::flambda_invariants
