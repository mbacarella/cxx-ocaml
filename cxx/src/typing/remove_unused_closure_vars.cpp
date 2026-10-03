// Port of middle_end/flambda/remove_unused_closure_vars.ml (see
// remove_unused_closure_vars.hpp).  The Tbl here are only queried for
// membership.
#include "cppcaml/typing/remove_unused_closure_vars.hpp"

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::remove_unused_closure_vars {

using namespace flambda;

// A variable in a closure can either be used by the closure itself or by
// an inlined version of the function.
Program remove_unused_closure_variables(bool remove_direct_call_surrogates, const Program& program) {
  variable::Set used_vars_within_closure, used_closure_ids;
  flambda_iterators::iter_named_of_program(program, [&](named n) {
    switch (n->kind) {
      case NK::Project_closure: used_closure_ids = used_closure_ids.add(as<NProject_closure>(n)->p.closure_id); break;
      case NK::Project_var: {
        const auto& p = as<NProject_var>(n)->p;
        used_vars_within_closure = used_vars_within_closure.add(p.var);
        used_closure_ids = used_closure_ids.add(p.closure_id);
        break;
      }
      case NK::Move_within_set_of_closures: {
        const auto& m = as<NMove_within_set_of_closures>(n)->m;
        used_closure_ids = used_closure_ids.add(m.start_from).add(m.move_to);
        break;
      }
      default: break;
    }
  });
  auto aux_named = [&](variable::t, named n) -> named {
    auto* s = as<NSet_of_closures>(n);
    if (!s) return n;
    const SetOfClosures* set = s->set;
    variable::Set direct_call_surrogates;
    if (!remove_direct_call_surrogates) {
      std::vector<variable::t> data;  // Variable.Map.data
      set->direct_call_surrogates.iter([&](variable::t, variable::t v) { data.push_back(v); });
      direct_call_surrogates = variable::Set::of_list(data);
    }
    using Funs = variable::Map<const FunctionDeclaration*>;
    Funs needed_funs;
    Funs remaining_funs = set->function_decls->funs;
    variable::Set free_vars_of_kept_funs;
    for (;;) {
      // Keep a function if it is used either by the rest of the code (in
      // used_closure_ids), or by any other kept function (in
      // free_vars_of_kept_funs)
      auto [new_needed_funs, rest] = remaining_funs.partition([&](variable::t fun_id, const FunctionDeclaration*) {
        return free_vars_of_kept_funs.mem(fun_id) || used_closure_ids.mem(fun_id) ||
               direct_call_surrogates.mem(fun_id);
      });
      remaining_funs = rest;
      // If no new function is needed, we reached fixpoint
      if (new_needed_funs.is_empty()) break;
      needed_funs = Funs::union_(
          [](variable::t, const FunctionDeclaration*, const FunctionDeclaration*) -> std::optional<const FunctionDeclaration*> {
            misc::fatal_error("Map.disjoint_union");
          },
          needed_funs, new_needed_funs);
      free_vars_of_kept_funs = new_needed_funs.fold(
          [](variable::t, const FunctionDeclaration* d, variable::Set acc) {
            return variable::Set::union_(d->free_variables, acc);
          },
          free_vars_of_kept_funs);
    }
    const Funs& funs = needed_funs;
    variable::Map<SpecialisedTo> free_vars = set->free_vars.filter([&](variable::t id, const SpecialisedTo&) {
      return free_vars_of_kept_funs.mem(id) || used_vars_within_closure.mem(id);
    });
    const FunctionDeclarations* function_decls = update_function_declarations(set->function_decls, funs);
    // Remove specialised args that are used by removed functions
    variable::Set all_remaining_arguments = funs.fold(
        [](variable::t, const FunctionDeclaration* d, variable::Set acc) {
          return variable::Set::union_(acc, parameter::set_vars(d->params));
        },
        variable::Set{});
    variable::Map<SpecialisedTo> specialised_args = set->specialised_args.filter(
        [&](variable::t arg, const SpecialisedTo&) { return all_remaining_arguments.mem(arg); });
    free_vars = flambda_utils::clean_projections(free_vars);
    // Remove direct call surrogates where either the existing function or
    // the surrogate has been eliminated.
    variable::Map<variable::t> dcs = set->direct_call_surrogates.fold(
        [&](variable::t existing, variable::t surrogate, variable::Map<variable::t> acc) {
          if (!funs.mem(existing) || !funs.mem(surrogate)) return acc;
          return acc.add(existing, surrogate);
        },
        variable::Map<variable::t>{});
    return n_set_of_closures(create_set_of_closures(function_decls, free_vars, specialised_args, dcs));
  };
  return flambda_iterators::map_named_of_program(program, aux_named);
}

}  // namespace cppcaml::typing::remove_unused_closure_vars
