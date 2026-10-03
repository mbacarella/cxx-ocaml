// Port of middle_end/flambda/unbox_closures.ml, unbox_specialised_args.ml
// and unbox_free_vars_of_closures.ml (see unbox_closures.hpp).
#include "cppcaml/typing/unbox_closures.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/extract_projections.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/invariant_params.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pass_wrapper.hpp"

namespace cppcaml::typing {

using namespace flambda;
using E = inline_and_simplify_aux::Env;
using W = augment_specialised_args::WhatToSpecialise;
using augment_specialised_args::Definition;
namespace B = inlining_cost::benefit;

namespace unbox_closures {
namespace {
bool precondition(const E& env, const SetOfClosures* set_of_closures) {
  return clflags::unbox_closures && !env.at_toplevel() && !set_of_closures->free_vars.is_empty();
}

W what_to_specialise(const E& env, const SetOfClosures* set_of_closures) {
  W what = W::create(set_of_closures);
  if (!precondition(env, set_of_closures)) return what;
  long round = env.round();
  long num_closure_vars = set_of_closures->free_vars.cardinal();
  // For the moment assume that we're going to cause all functions in the
  // set to become closed.
  inlining_cost::Benefit saved_by_not_building_closure = B::remove_prims(B::remove_call(B::zero()), num_closure_vars);
  // Flambda_iterators.fold_function_decls_ignoring_stubs
  set_of_closures->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration* function_decl) {
    long body_size = inlining_cost::lambda_size(function_decl->body);
    // If the function is small enough, make a direct call surrogate for it,
    // so that indirect calls are not penalised by having to bounce through
    // the stub.  (Making such a surrogate involves duplicating the
    // function.)
    inlining_cost::WhetherSufficientBenefit wsb = inlining_cost::whether_sufficient_benefit::create_estimate(
        0, false, 0, body_size / clflags::unbox_closures_factor + 1, saved_by_not_building_closure, false, round);
    bool small_enough_to_duplicate = inlining_cost::whether_sufficient_benefit::evaluate(wsb);
    if (small_enough_to_duplicate) what = what.make_direct_call_surrogate_for(fun_var);
    variable::Set bound_by_the_closure =
        flambda_utils::variables_bound_by_the_closure(fun_var, set_of_closures->function_decls);
    bound_by_the_closure.iter([&](variable::t inner_free_var) {
      Definition d;
      d.existing_inner_free_var = inner_free_var;
      what = what.new_specialised_arg(fun_var, inner_free_var, d);
    });
  });
  return what;
}
const bool registered = pass_wrapper::register_pass("unbox-closures");
}  // namespace

const augment_specialised_args::Make& pass() {
  static const augment_specialised_args::Make m{"unbox-closures", what_to_specialise};
  return m;
}
}  // namespace unbox_closures

namespace unbox_specialised_args {
namespace {
W what_to_specialise(const E& env, const SetOfClosures* set_of_closures) {
  W what = W::create(set_of_closures);
  if (!(clflags::unbox_specialised_args && !set_of_closures->specialised_args.is_empty())) return what;
  variable::Map<extract_projections::ProjSet> projections_by_function =
      set_of_closures->function_decls->funs.filter_map(
          [&](variable::t, const FunctionDeclaration* function_decl) -> std::optional<extract_projections::ProjSet> {
            if (function_decl->stub) return std::nullopt;
            return extract_projections::from_function_decl(env, set_of_closures->specialised_args, function_decl);
          });
  // CR-soon mshinwell: consider caching the Invariant_params *relation* as
  // well as the "_in_recursion" map
  variable::Map<invariant_params::PairSet> invariant_params_flow =
      invariant_params::invariant_param_sources(set_of_closures->function_decls);
  projections_by_function.iter([&](variable::t fun_var, const extract_projections::ProjSet& extractions) {
    extractions.iter([&](projection::t p) {
      variable::t group = projection::projecting_from(p);
      if (!set_of_closures->specialised_args.mem(group))
        misc::fatal_error("Unbox_specialised_args.what_to_specialise");  // (assert)
      Definition d;
      d.projection = p;
      what = what.new_specialised_arg(fun_var, group, d);
      const invariant_params::PairSet* flow = invariant_params_flow.find_opt(group);
      if (!flow) return;
      // If for function [f] we would extract a projection expression [e]
      // from some specialised argument [x] of [f], and we know from
      // [Invariant_params] that a specialised argument [y] of another
      // function [g] flows to [x], we will add [e] with [y] substituted for
      // [x] throughout as a newly-specialised argument for [g].
      flow->iter([&](const invariant_params::Pair& target) {
        variable::t target_fun_var = target.first, target_spec_arg = target.second;
        if (variable::equal(fun_var, target_fun_var) || !set_of_closures->specialised_args.mem(target_spec_arg)) return;
        // Rewrite the projection (that was in terms of an inner specialised
        // arg of [fun_var]) to be in terms of the corresponding inner
        // specialised arg of [target_fun_var].
        projection::t p2 = projection::map_projecting_from(p, [&](variable::t v) {
          if (!variable::equal(v, group)) misc::fatal_error("Unbox_specialised_args: projecting_from");  // (assert)
          return target_spec_arg;
        });
        Definition d2;
        d2.projection = p2;
        what = what.new_specialised_arg(target_fun_var, group, d2);
      });
    });
  });
  return what;
}
const bool registered = pass_wrapper::register_pass("unbox-specialised-args");
}  // namespace

const augment_specialised_args::Make& pass() {
  static const augment_specialised_args::Make m{"unbox-specialised-args", what_to_specialise};
  return m;
}
}  // namespace unbox_specialised_args

namespace unbox_free_vars_of_closures {
namespace {
const char* const pass_name = "unbox-free-vars-of-closures";
const bool registered = pass_wrapper::register_pass(pass_name);

// CR-someday mshinwell: Nearly but not quite the same as something that
// Augment_specialised_args uses.
std::pair<t, inlining_cost::Benefit> add_lifted_projections_around_set_of_closures(
    const SetOfClosures* set_of_closures, const variable::Map<SpecialisedTo>& existing_inner_to_outer_vars,
    inlining_cost::Benefit benefit, const variable::Map<projection::t>& definitions_indexed_by_new_inner_vars) {
  t body = flambda_utils::name_expr(n_set_of_closures(set_of_closures),
                                    internal_variable_names::unbox_free_vars_of_closures);
  definitions_indexed_by_new_inner_vars.iter([&](variable::t new_inner_var, projection::t p) {
    auto find_outer_var = [&](variable::t inner_var) -> variable::t {
      const SpecialisedTo* outer_var = existing_inner_to_outer_vars.find_opt(inner_var);
      if (!outer_var) {
        format::Formatter f;
        format::fprintf(f,
                        "(UFV) find_outer_var: expected %a to be in [existing_inner_to_outer_vars], but it is not.  (The "
                        "projection was: %a)",
                        pr(variable::print, inner_var), pr(projection::print, p));
        misc::fatal_error(f.contents());
      }
      return outer_var->var;
    };
    benefit = B::add_projection(p, benefit);
    // The lifted projection must be in terms of outer variables, not inner
    // variables.
    named n = flambda_utils::projection_to_named(projection::map_projecting_from(p, find_outer_var));
    body = create_let(find_outer_var(new_inner_var), n, body);
  });
  return {body, benefit};
}

std::optional<std::pair<t, inlining_cost::Benefit>> run_core(const E& env, const SetOfClosures* set_of_closures) {
  if (!clflags::unbox_free_vars_of_closures) return std::nullopt;
  extract_projections::ProjSet all_existing_definitions = set_of_closures->free_vars.fold(
      [](variable::t, const SpecialisedTo& outer_var, extract_projections::ProjSet acc) {
        return outer_var.projection ? acc.add(outer_var.projection) : acc;
      },
      extract_projections::ProjSet{});
  variable::Map<projection::t> definitions_indexed_by_new_inner_vars;
  extract_projections::ProjSet all_including_added = all_existing_definitions;
  variable::Map<SpecialisedTo> free_vars = set_of_closures->free_vars;
  bool done_something = false;
  set_of_closures->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* function_decl) {
    extract_projections::ProjSet extracted =
        extract_projections::from_function_decl(env, set_of_closures->free_vars, function_decl);
    extracted.iter([&](projection::t p) {
      // Don't add a new free variable if there already exists a free
      // variable with the desired projection.  (unbox_free_vars_of_closures.ml)
      if (all_including_added.mem(p)) return;
      // Add a new free variable.  This needs both a fresh "new inner" and a
      // fresh "new outer" var, since we know the definition is not a
      // duplicate.
      variable::t projecting_from = projection::projecting_from(p);
      variable::t new_inner_var = variable::rename(projecting_from);
      variable::t new_outer_var = variable::rename(projecting_from);
      definitions_indexed_by_new_inner_vars = definitions_indexed_by_new_inner_vars.add(new_inner_var, p);
      all_including_added = all_including_added.add(p);
      free_vars = free_vars.add(new_inner_var, SpecialisedTo{new_outer_var, p});
      done_something = true;
    });
  });
  if (!done_something) return std::nullopt;
  // CR-someday mshinwell: could consider doing the grouping thing similar
  // to Augment_specialised_args
  long num_free_vars_before = set_of_closures->free_vars.cardinal();
  long num_free_vars_after = free_vars.cardinal();
  if (!(num_free_vars_after > num_free_vars_before))
    misc::fatal_error("Unbox_free_vars_of_closures.run");  // (assert)
  // Don't let the closure grow too large.
  if (num_free_vars_after > 2 * num_free_vars_before) return std::nullopt;
  const SetOfClosures* soc = create_set_of_closures(set_of_closures->function_decls, free_vars,
                                                    set_of_closures->specialised_args,
                                                    set_of_closures->direct_call_surrogates);
  return add_lifted_projections_around_set_of_closures(soc, soc->free_vars, B::zero(),
                                                       definitions_indexed_by_new_inner_vars);
}
}  // namespace

std::optional<std::pair<t, inlining_cost::Benefit>> run(const E& env, const SetOfClosures* set_of_closures) {
  return pass_wrapper::with_dump<std::pair<t, inlining_cost::Benefit>>(
      *env.ppf_dump(), pass_name, [&] { return run_core(env, set_of_closures); },
      [&](format::Formatter& f) { print_set_of_closures(f, set_of_closures); },
      [](format::Formatter& f, const std::pair<t, inlining_cost::Benefit>& r) { print(f, r.first); });
}
}  // namespace unbox_free_vars_of_closures

}  // namespace cppcaml::typing
