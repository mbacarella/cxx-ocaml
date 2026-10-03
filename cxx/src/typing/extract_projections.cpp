// Port of middle_end/flambda/extract_projections.ml (see
// extract_projections.hpp).
#include "cppcaml/typing/extract_projections.hpp"

#include <utility>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::extract_projections {

using namespace flambda;
namespace A = simple_value_approx;
using E = inline_and_simplify_aux::Env;

namespace {
projection::t mk(projection::T p) { return make<projection::T>(p); }

ProjSet known_valid_projections(const E& env, const ProjSet& projections,
                                const variable::Map<SpecialisedTo>& which_variables) {
  return projections.filter([&](projection::t p) {
    variable::t from = projection::projecting_from(p);
    const SpecialisedTo* outer = which_variables.find_opt(from);
    if (!outer) misc::fatal_error("Extract_projections.known_valid_projections");  // (assert false)
    variable::t outer_var = freshening::apply_variable(env.freshening(), outer->var);
    A::t approx = env.find_exn(outer_var);
    switch (p->kind) {
      case projection::T::Kind::Project_var: {
        A::CheckedClosure c = A::check_approx_for_closure(approx);
        if (c.kind != A::CheckedClosure::Kind::Ok) return false;
        return c.set->bound_vars.mem(p->project_var.var);
      }
      case projection::T::Kind::Project_closure: {
        A::CheckedSetOfClosures c = A::strict_check_approx_for_set_of_closures(approx);
        if (c.kind != A::CheckedSetOfClosures::Kind::Ok) return false;
        return c.set->function_decls->funs.keys().mem(p->project_closure.closure_id);
      }
      case projection::T::Kind::Move_within_set_of_closures: {
        A::CheckedClosure c = A::check_approx_for_closure(approx);
        if (c.kind != A::CheckedClosure::Kind::Ok) return false;
        // We could check that [move.move_to] is in
        // [value_set_of_closures], but this is unnecessary, since
        // [Closure_id]s are unique.
        return variable::equal(c.closure->closure_id, p->move.start_from);
      }
      case projection::T::Kind::Field: {
        std::optional<A::BlockApprox> b = A::check_approx_for_block(approx);
        if (!b) return false;
        return p->field_index >= 0 && p->field_index < static_cast<long>(b->fields.size());
      }
    }
    return false;
  });
}

std::pair<ProjSet, variable::Set> analyse_expr(const variable::Map<SpecialisedTo>& which_variables, t expr) {
  ProjSet projections;
  variable::Set used_which_variables;
  auto check_free_variable = [&](variable::t var) {
    if (which_variables.mem(var)) used_which_variables = used_which_variables.add(var);
  };
  auto for_expr = [&](t e) {
    switch (e->kind) {
      case EK::Var: check_free_variable(static_cast<const Var*>(e)->var); break;
      case EK::Let_mutable: check_free_variable(static_cast<const Let_mutable*>(e)->initial_value); break;
      // CR-soon mshinwell: We don't handle [Apply] for the moment to avoid
      // disabling unboxing optimizations whenever we see a recursive call.
      case EK::Apply: break;
      case EK::Send: {
        auto* s = static_cast<const Send*>(e);
        check_free_variable(s->meth);
        check_free_variable(s->obj);
        for (variable::t a : s->args) check_free_variable(a);
        break;
      }
      case EK::Assign: check_free_variable(static_cast<const Assign*>(e)->new_value); break;
      case EK::If_then_else: check_free_variable(static_cast<const If_then_else*>(e)->cond); break;
      case EK::Switch: check_free_variable(static_cast<const Switch*>(e)->scrutinee); break;
      case EK::String_switch: check_free_variable(static_cast<const String_switch*>(e)->scrutinee); break;
      case EK::Static_raise:
        for (variable::t a : static_cast<const Static_raise*>(e)->args) check_free_variable(a);
        break;
      case EK::For: {
        auto* f = static_cast<const For*>(e);
        check_free_variable(f->from_value);
        check_free_variable(f->to_value);
        break;
      }
      case EK::Let: case EK::Static_catch: case EK::While: case EK::Try_with: case EK::Proved_unreachable: break;
    }
  };
  auto for_named = [&](named n) {
    switch (n->kind) {
      case NK::Project_var: {
        const auto& pv = static_cast<const NProject_var*>(n)->p;
        if (which_variables.mem(pv.closure)) {
          projection::T p{projection::T::Kind::Project_var};
          p.project_var = pv;
          projections = projections.add(mk(p));
        }
        break;
      }
      case NK::Project_closure: {
        const auto& pc = static_cast<const NProject_closure*>(n)->p;
        if (which_variables.mem(pc.set_of_closures)) {
          projection::T p{projection::T::Kind::Project_closure};
          p.project_closure = pc;
          projections = projections.add(mk(p));
        }
        break;
      }
      case NK::Move_within_set_of_closures: {
        const auto& m = static_cast<const NMove_within_set_of_closures*>(n)->m;
        if (which_variables.mem(m.closure)) {
          projection::T p{projection::T::Kind::Move_within_set_of_closures};
          p.move = m;
          projections = projections.add(mk(p));
        }
        break;
      }
      case NK::Prim: {
        auto* pr_ = static_cast<const NPrim*>(n);
        if (pr_->prim->kind == clambda::Primitive::K::Pfield && pr_->args.size() == 1 &&
            which_variables.mem(pr_->args[0])) {
          projection::T p{projection::T::Kind::Field};
          p.field_index = pr_->prim->n;
          p.field_var = pr_->args[0];
          projections = projections.add(mk(p));
        } else {
          for (variable::t v : pr_->args) check_free_variable(v);
        }
        break;
      }
      case NK::Set_of_closures: {
        const SetOfClosures* set = static_cast<const NSet_of_closures*>(n)->set;
        auto aliasing = [&](const SpecialisedTo& s) { return which_variables.mem(s.var); };
        variable::Map<SpecialisedTo> aliasing_free_vars =
            set->free_vars.filter([&](variable::t, const SpecialisedTo& s) { return aliasing(s); });
        variable::Map<SpecialisedTo> aliasing_specialised_args =
            set->specialised_args.filter([&](variable::t, const SpecialisedTo& s) { return aliasing(s); });
        variable::Map<SpecialisedTo> aliasing_vars = variable::Map<SpecialisedTo>::union_(
            [](variable::t, const SpecialisedTo&, const SpecialisedTo&) -> std::optional<SpecialisedTo> {
              misc::fatal_error("Map.disjoint_union");
            },
            aliasing_free_vars, aliasing_specialised_args);
        if (!aliasing_vars.is_empty())
          set->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* fun_decl) {
            // We ignore projections from within nested sets of closures.
            auto [_, used] = analyse_expr(aliasing_vars, fun_decl->body);
            used.iter([&](variable::t var) {
              const SpecialisedTo* s = aliasing_vars.find_opt(var);
              if (!s) misc::fatal_error("Extract_projections.analyse_expr");  // (assert false)
              check_free_variable(s->var);
            });
          });
        break;
      }
      default: break;
    }
  };
  flambda_iterators::iter_toplevel(for_expr, for_named, expr);
  return {projections, used_which_variables};
}
}  // namespace

ProjSet from_function_decl(const E& env, const variable::Map<SpecialisedTo>& which_variables,
                           const FunctionDeclaration* function_decl) {
  auto [projections, used_which_variables] = analyse_expr(which_variables, function_decl->body);
  // We must use approximation information to determine which projections
  // are actually valid in the current environment, other we might lift
  // expressions too far.
  projections = known_valid_projections(env, projections, which_variables);
  // Don't extract projections whose [projecting_from] variable is also used
  // boxed.  (extract_projections.ml)
  return projections.filter(
      [&](projection::t p) { return !used_which_variables.mem(projection::projecting_from(p)); });
}

}  // namespace cppcaml::typing::extract_projections
