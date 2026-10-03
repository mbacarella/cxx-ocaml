// Port of middle_end/flambda/inlining_transforms.ml (see
// inlining_transforms.hpp).
#include "cppcaml/typing/inlining_transforms.hpp"

#include <vector>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inlining_transforms {

using namespace flambda;
namespace B = inlining_cost::benefit;
namespace Names = internal_variable_names;

namespace {
variable::t new_var(Names::t name) { return variable::create(name, compilation_unit::get_current_exn()); }

t set_inline_attribute_on_all_apply(t body, lambda::InlineAttribute inline_, lambda::SpecialiseAttribute specialise) {
  return flambda_iterators::map_toplevel_expr(
      [&](t e) -> t {
        if (auto* a = as<Apply>(e)) return apply(a->func, a->args, a->call_kind, a->dbg, inline_, specialise);
        return e;
      },
      body);
}

// Assign fresh names for a function's parameters and rewrite the body to
// use these new names.
std::pair<std::vector<Parameter>, t> copy_of_functions_body_with_freshened_params(
    const Env& env, const A::FunctionDeclaration* function_decl, const A::FunctionBody* function_body) {
  Slice<Parameter> params = function_decl->params;
  std::vector<variable::t> param_vars;
  for (const Parameter& p : params) param_vars.push_back(p.var);
  // We cannot avoid the substitution in the case where we are inlining
  // inside the function itself.  (inlining_transforms.ml)
  if (env.does_not_bind(slice(param_vars)) && env.does_not_freshen(slice(param_vars)))
    return {std::vector<Parameter>(params.begin(), params.end()), function_body->body};
  std::vector<Parameter> freshened_params;  // List.map: in order
  for (const Parameter& p : params) freshened_params.push_back(parameter::wrap(variable::rename(p.var)));
  variable::Map<variable::t> subst;  // Variable.Map.of_list (List.combine ...)
  for (std::size_t k = 0; k < param_vars.size(); ++k) subst = subst.add(param_vars[k], freshened_params[k].var);
  t body = flambda_utils::toplevel_substitution(subst, function_body->body);
  return {freshened_params, body};
}
}  // namespace

// Inline a function by copying its body into a context where it becomes
// closed.  That is to say, we bind the free variables of the body (=
// "variables bound by the closure"), and any function identifiers
// introduced by the corresponding set of closures.
std::pair<t, Result> inline_by_copying_function_body(const Env& env0, const Result& r0, variable::t lhs_of_application,
                                                     lambda::InlineAttribute inline_requested,
                                                     lambda::SpecialiseAttribute specialise_requested,
                                                     variable::t closure_id_being_applied,
                                                     const A::FunctionDeclaration* function_decl,
                                                     const A::FunctionBody* function_body,
                                                     const variable::Set& fun_vars, Slice<variable::t> args,
                                                     const debuginfo::t& dbg, const Simplify& simplify) {
  if (!env0.mem(lhs_of_application)) misc::fatal_error("Inlining_transforms: lhs_of_application");  // (assert)
  for (variable::t a : args)
    if (!env0.mem(a)) misc::fatal_error("Inlining_transforms: args");  // (assert)
  Result r = function_body->stub ? r0 : r0.map_benefit(B::remove_call);
  auto [freshened_params, body] = copy_of_functions_body_with_freshened_params(env0, function_decl, function_body);
  bool default_inline = inline_requested.kind == lambda::InlineAttribute::Kind::Default_inline;
  bool default_specialise = specialise_requested == lambda::SpecialiseAttribute::Default_specialise;
  // When the function inlined function is a stub, the annotation is
  // reported to the function applications inside the stub.
  // (inlining_transforms.ml)
  if (function_body->stub && (!default_inline || !default_specialise))
    body = set_inline_attribute_on_all_apply(body, inline_requested, specialise_requested);
  // Bind the function's parameters to the arguments from the call site.
  std::vector<std::pair<variable::t, named>> bindings;
  std::vector<named> arg_named;  // List.map: in order
  for (variable::t a : args) arg_named.push_back(n_expr(var(a)));
  if (freshened_params.size() != args.size()) misc::fatal_error("Invalid_argument(\"List.combine\")");
  for (std::size_t k = 0; k < args.size(); ++k) bindings.emplace_back(freshened_params[k].var, arg_named[k]);
  t expr = flambda_utils::bind(bindings, body);
  // Add bindings for the variables bound by the closure.
  variable::Set params = parameter::set_vars(function_decl->params);
  variable::Set bound_variables =
      variable::Set::diff(variable::Set::diff(function_body->free_variables, params), fun_vars);
  bound_variables.iter([&](variable::t v) {
    expr = create_let(v, n_project_var(projection::ProjectVar{lhs_of_application, closure_id_being_applied, v}), expr);
  });
  // Add bindings for variables corresponding to the functions introduced by
  // the whole set of closures.  (inlining_transforms.ml)
  fun_vars.iter([&](variable::t another_closure_in_the_same_set) {
    if (function_body->free_variables.mem(another_closure_in_the_same_set))
      expr = create_let(another_closure_in_the_same_set,
                        n_move_within_set_of_closures(projection::MoveWithinSetOfClosures{
                            lhs_of_application, closure_id_being_applied, another_closure_in_the_same_set}),
                        expr);
  });
  Env env = env0.set_never_inline();
  env = env.activate_freshening();
  env = env.set_inline_debuginfo(dbg);
  return simplify(env, r, expr);
}

namespace {
struct State {
  variable::Map<variable::t> old_inside_to_new_inside;    // Map from old inner vars to new inner vars
  variable::Map<variable::t> old_outside_to_new_outside;  // Map from old outer vars to new outer vars
  // Map from old parameters to new outer vars.  These are params that
  // should be specialised if they are copied to the new set of closures.
  variable::Map<variable::t> old_params_to_new_outside;
  // Map from old fun vars to new fun vars.  These are the functions that
  // will be copied into the new set of closures
  variable::Map<variable::t> old_fun_var_to_new_fun_var;
  // Let bindings that will surround the definition of the new set of
  // closures (newest first: the vector's back is the list's head)
  std::vector<std::pair<variable::t, named>> let_bindings;
  // List of functions that still need to be copied to the new set of
  // closures (head: the vector's back)
  std::vector<variable::t> to_copy;
  variable::Map<const FunctionDeclaration*> new_funs;  // The function declarations for the new set of closures
  // The free variables for the new set of closures, but the projection
  // fields still point to old free variables.
  variable::Map<SpecialisedTo> new_free_vars_with_old_projections;
  // The specialised parameters for the new set of closures, but the
  // projection fields still point to old specialised parameters.
  variable::Map<SpecialisedTo> new_specialised_args_with_old_projections;
};

// Add let bindings for the free vars in the set_of_closures and add them to
// [old_outside_to_new_outside]
State bind_free_vars(variable::t lhs_of_application, variable::t closure_id_being_applied, State state,
                     const variable::Map<SpecialisedTo>& free_vars) {
  free_vars.iter([&](variable::t free_var, const SpecialisedTo& spec) {
    variable::t var_clos = new_var(Names::from_closure);
    named e = n_project_var(projection::ProjectVar{lhs_of_application, closure_id_being_applied, free_var});
    state.let_bindings.emplace_back(var_clos, e);
    state.old_outside_to_new_outside = state.old_outside_to_new_outside.add(spec.var, var_clos);
  });
  return state;
}

// For arguments of specialised parameters ... (inlining_transforms.ml)
State register_arguments(const variable::Map<SpecialisedTo>& specialised_args,
                         const A::Lazy<variable::Map<variable::Set>>& invariant_params, State state,
                         Slice<Parameter> params, Slice<variable::t> args, const std::vector<A::t>& args_approxs) {
  if (params.size() != args.size() || params.size() != args_approxs.size())
    misc::fatal_error("Inlining_transforms.register_arguments");  // (assert false)
  for (std::size_t k = 0; k < params.size(); ++k) {
    variable::t param = params[k].var;
    variable::t arg = args[k];
    bool worth_specialising;
    variable::Map<variable::t> old_outside_to_new_outside = state.old_outside_to_new_outside;
    if (const SpecialisedTo* spec = specialised_args.find_opt(param)) {
      old_outside_to_new_outside = old_outside_to_new_outside.add(spec->var, arg);
      worth_specialising = true;
    } else {
      worth_specialising = A::useful(args_approxs[k]) && invariant_params.force().mem(param);
    }
    variable::Map<variable::t> old_params_to_new_outside = state.old_params_to_new_outside;
    if (worth_specialising) {
      old_params_to_new_outside = old_params_to_new_outside.add(param, arg);
      if (const variable::Set* set = invariant_params.force().find_opt(param))
        set->iter([&](variable::t elem) { old_params_to_new_outside = old_params_to_new_outside.add(elem, arg); });
    }
    state.old_outside_to_new_outside = old_outside_to_new_outside;
    state.old_params_to_new_outside = old_params_to_new_outside;
  }
  return state;
}

// Add an old parameter to [old_inside_to_new_inside].  If it appears in
// [old_params_to_new_outside] then also add it to the new specialised args.
Parameter add_param(const variable::Map<SpecialisedTo>& specialised_args, State& state, const Parameter& p) {
  variable::t param = p.var;
  variable::t new_param = variable::rename(param);
  state.old_inside_to_new_inside = state.old_inside_to_new_inside.add(param, new_param);
  if (const SpecialisedTo* spec = specialised_args.find_opt(param)) {
    const variable::t* new_outside_var = state.old_outside_to_new_outside.find_opt(spec->var);
    if (!new_outside_var) misc::fatal_error("Inlining_transforms.add_param");  // (Not_found)
    state.new_specialised_args_with_old_projections =
        state.new_specialised_args_with_old_projections.add(new_param, SpecialisedTo{*new_outside_var, spec->projection});
  } else if (const variable::t* new_outside_var = state.old_params_to_new_outside.find_opt(param)) {
    state.new_specialised_args_with_old_projections =
        state.new_specialised_args_with_old_projections.add(new_param, SpecialisedTo{*new_outside_var, nullptr});
  }
  return parameter::wrap(new_param);
}

// Add a let binding for an old fun_var, add it to the new free variables,
// and add it to [old_inside_to_new_inside]
void add_fun_var(variable::t lhs_of_application, variable::t closure_id_being_applied, State& state,
                 variable::t fun_var) {
  if (state.old_inside_to_new_inside.mem(fun_var)) return;
  variable::t inside_var = variable::rename(fun_var);
  variable::t outside_var = variable::create(Names::closure);
  named e = n_move_within_set_of_closures(
      projection::MoveWithinSetOfClosures{lhs_of_application, closure_id_being_applied, fun_var});
  state.let_bindings.emplace_back(outside_var, e);
  state.new_free_vars_with_old_projections =
      state.new_free_vars_with_old_projections.add(inside_var, SpecialisedTo{outside_var, nullptr});
  state.old_inside_to_new_inside = state.old_inside_to_new_inside.add(fun_var, inside_var);
}

// Add an old free_var to the new free variables and add it to
// [old_inside_to_new_inside].
void add_free_var(const variable::Map<SpecialisedTo>& free_vars, State& state, variable::t free_var) {
  if (state.old_inside_to_new_inside.mem(free_var)) return;
  const SpecialisedTo* spec = free_vars.find_opt(free_var);
  if (!spec) misc::fatal_error("Inlining_transforms.add_free_var");  // (Not_found)
  const variable::t* new_outside_var = state.old_outside_to_new_outside.find_opt(spec->var);
  if (!new_outside_var) misc::fatal_error("Inlining_transforms.add_free_var");  // (Not_found)
  SpecialisedTo new_spec{*new_outside_var, spec->projection};
  variable::t new_inside_var = variable::rename(free_var);
  state.new_free_vars_with_old_projections = state.new_free_vars_with_old_projections.add(new_inside_var, new_spec);
  state.old_inside_to_new_inside = state.old_inside_to_new_inside.add(free_var, new_inside_var);
}

// Add a function to the new set of closures iff:
// 1) All its specialised parameters are available in
//    [old_outside_to_new_outside]
// 2) At least one more parameter will become specialised
std::optional<variable::t> add_function(const variable::Map<SpecialisedTo>& specialised_args, State& state,
                                        variable::t fun_var, const A::FunctionDeclaration* function_decl) {
  if (!function_decl->function_body) return std::nullopt;
  bool worth_specialising = false;
  for (const Parameter& p : function_decl->params) {
    if (const SpecialisedTo* spec = specialised_args.find_opt(p.var)) {
      if (!state.old_outside_to_new_outside.mem(spec->var)) return std::nullopt;
    } else {
      worth_specialising = worth_specialising || state.old_params_to_new_outside.mem(p.var);
    }
  }
  if (!worth_specialising) return std::nullopt;
  variable::t new_fun_var = variable::rename(fun_var);
  state.old_fun_var_to_new_fun_var = state.old_fun_var_to_new_fun_var.add(fun_var, new_fun_var);
  state.to_copy.push_back(fun_var);
  return new_fun_var;
}

// Lookup a function in the new set of closures, trying to add it if
// necessary.
std::optional<variable::t> lookup_function(const variable::Map<SpecialisedTo>& specialised_args, State& state,
                                           variable::t fun_var, const A::FunctionDeclaration* function_decl) {
  if (const variable::t* v = state.old_fun_var_to_new_fun_var.find_opt(fun_var)) return *v;
  return add_function(specialised_args, state, fun_var, function_decl);
}

// A direct call to a function in the new set of closures can be
// specialised if all the function's newly specialised parameters are
// passed arguments that are specialised to the same outside variable
bool specialisable_call(const variable::Map<SpecialisedTo>& specialised_args, const State& state,
                        Slice<variable::t> args, Slice<Parameter> params) {
  if (args.size() != params.size()) misc::fatal_error("Invalid_argument(\"List.for_all2\")");
  for (std::size_t k = 0; k < args.size(); ++k) {
    variable::t param = params[k].var;
    if (specialised_args.mem(param)) continue;
    const variable::t* outside_var = state.old_params_to_new_outside.find_opt(param);
    if (!outside_var) continue;
    const variable::t* outside_var2 = state.old_params_to_new_outside.find_opt(args[k]);
    if (!outside_var2 || !variable::equal(*outside_var, *outside_var2)) return false;
  }
  return true;
}

// Rewrite a call iff ... (inlining_transforms.ml)
std::optional<t> rewrite_direct_call(const variable::Map<SpecialisedTo>& specialised_args,
                                     const variable::Map<const A::FunctionDeclaration*>& funs,
                                     const variable::Map<variable::t>& direct_call_surrogates, State& state,
                                     variable::t closure_id, const Apply* a) {
  for (;;) {
    const variable::t* surrogate = direct_call_surrogates.find_opt(closure_id);
    if (!surrogate) break;
    closure_id = *surrogate;
  }
  const A::FunctionDeclaration* const* fd = funs.find_opt(closure_id);
  if (!fd) return std::nullopt;
  State st = state;
  std::optional<variable::t> new_fun_var = lookup_function(specialised_args, st, closure_id, *fd);
  if (!new_fun_var) return std::nullopt;
  if (!specialisable_call(specialised_args, st, a->args, (*fd)->params)) return std::nullopt;
  state = st;
  return apply(*new_fun_var, a->args, CallKind{*new_fun_var}, a->dbg, a->inline_, a->specialise);
}

// Rewrite the body a function declaration for use in the new set of
// closures.
State rewrite_function(variable::t lhs_of_application, variable::t closure_id_being_applied,
                       const variable::Map<variable::t>& direct_call_surrogates,
                       const variable::Map<SpecialisedTo>& specialised_args,
                       const variable::Map<SpecialisedTo>& free_vars,
                       const variable::Map<const A::FunctionDeclaration*>& funs, State state, variable::t fun_var) {
  const A::FunctionDeclaration* function_decl = *funs.find_opt(fun_var);
  const A::FunctionBody* function_body = function_decl->function_body;
  if (!function_body) misc::fatal_error("Inlining_transforms.rewrite_function");  // (assert false)
  variable::t new_fun_var = *state.old_fun_var_to_new_fun_var.find_opt(fun_var);
  // List.fold_right: the last parameter first
  std::vector<Parameter> params(function_decl->params.size());
  for (std::size_t k = function_decl->params.size(); k-- > 0;)
    params[k] = add_param(specialised_args, state, function_decl->params[k]);
  function_body->free_variables.iter([&](variable::t v) {
    if (funs.mem(v)) add_fun_var(lhs_of_application, closure_id_being_applied, state, v);
    else if (free_vars.mem(v)) add_free_var(free_vars, state, v);
  });
  State state_ref = state;
  t body = flambda_iterators::map_toplevel_expr(
      [&](t e) -> t {
        auto* a = as<Apply>(e);
        if (!a || !a->call_kind.direct) return e;
        std::optional<t> r =
            rewrite_direct_call(specialised_args, funs, direct_call_surrogates, state_ref, a->call_kind.direct, a);
        return r ? *r : e;
      },
      function_body->body);
  body = flambda_utils::toplevel_substitution(state.old_inside_to_new_inside, body);
  const FunctionDeclaration* new_function_decl = create_function_declaration(
      slice(params), body, function_body->stub, function_body->dbg, function_body->inline_, function_body->specialise,
      function_body->is_a_functor, new_fun_var, function_body->poll);
  variable::Map<const FunctionDeclaration*> new_funs = state.new_funs.add(new_fun_var, new_function_decl);
  state_ref.new_funs = new_funs;
  return state_ref;
}

variable::Map<SpecialisedTo> update_projections(const State& state, const variable::Map<SpecialisedTo>& projections) {
  const variable::Map<variable::t>& old_to_new = state.old_inside_to_new_inside;
  return projections.map([&](const SpecialisedTo& spec_to) -> SpecialisedTo {
    projection::t p = spec_to.projection;
    projection::t np = nullptr;
    if (p) {
      projection::T r = *p;
      const variable::t* v = nullptr;
      switch (p->kind) {
        case projection::T::Kind::Project_var:
          if ((v = old_to_new.find_opt(p->project_var.closure))) r.project_var.closure = *v;
          break;
        case projection::T::Kind::Project_closure:
          if ((v = old_to_new.find_opt(p->project_closure.set_of_closures))) r.project_closure.set_of_closures = *v;
          break;
        case projection::T::Kind::Move_within_set_of_closures:
          if ((v = old_to_new.find_opt(p->move.closure))) r.move.closure = *v;
          break;
        case projection::T::Kind::Field:
          if ((v = old_to_new.find_opt(p->field_var))) r.field_var = *v;
          break;
      }
      if (v) np = make<projection::T>(r);
    }
    return SpecialisedTo{spec_to.var, np};
  });
}
}  // namespace

std::optional<std::pair<t, Result>> inline_by_copying_function_declaration(
    const Env& env, const Result& r, const A::FunctionDeclarations* function_decls, variable::t lhs_of_application,
    lambda::InlineAttribute inline_requested, variable::t closure_id_being_applied,
    const A::FunctionDeclaration* function_decl, Slice<variable::t> args, const std::vector<A::t>& args_approxs,
    const A::Lazy<variable::Map<variable::Set>>& invariant_params,
    const variable::Map<SpecialisedTo>& specialised_args, const variable::Map<SpecialisedTo>& free_vars0,
    const variable::Map<variable::t>& direct_call_surrogates, const debuginfo::t& dbg, const Simplify& simplify) {
  State state = bind_free_vars(lhs_of_application, closure_id_being_applied, State{}, free_vars0);
  state = register_arguments(specialised_args, invariant_params, state, function_decl->params, args, args_approxs);
  variable::t fun_var = closure_id_being_applied;
  std::optional<variable::t> new_fun_var = add_function(specialised_args, state, fun_var, function_decl);
  if (!new_fun_var) return std::nullopt;
  const variable::Map<const A::FunctionDeclaration*>& funs = function_decls->funs;
  while (!state.to_copy.empty()) {
    variable::t next = state.to_copy.back();
    state.to_copy.pop_back();
    state = rewrite_function(lhs_of_application, closure_id_being_applied, direct_call_surrogates, specialised_args,
                             free_vars0, funs, state, next);
  }
  variable::t closure_id = *new_fun_var;
  const FunctionDeclarations* new_function_decls = create_function_declarations_with_origin(
      function_decls->is_classic_mode, state.new_funs, function_decls->set_of_closures_origin);
  // (the arguments right to left: pure)
  variable::Map<SpecialisedTo> free_vars = update_projections(state, state.new_free_vars_with_old_projections);
  variable::Map<SpecialisedTo> sargs = update_projections(state, state.new_specialised_args_with_old_projections);
  const SetOfClosures* set_of_closures = create_set_of_closures(new_function_decls, free_vars, sargs, {});
  variable::t closure_var = new_var(Names::dup_func);
  variable::t set_of_closures_var = new_var(Names::dup_set_of_closures);
  t body = create_let(set_of_closures_var, n_set_of_closures(set_of_closures),
                      create_let(closure_var, n_project_closure(projection::ProjectClosure{set_of_closures_var, closure_id}),
                                 apply(closure_var, args, CallKind{closure_id}, dbg, inline_requested,
                                       lambda::SpecialiseAttribute::Default_specialise)));
  // Flambda_utils.bind ~bindings:state.let_bindings (the list's head first)
  std::vector<std::pair<variable::t, named>> bindings(state.let_bindings.rbegin(), state.let_bindings.rend());
  t expr = flambda_utils::bind(bindings, body);
  Env env2 = env.set_never_inline().activate_freshening();
  return simplify(env2, r, expr);
}

}  // namespace cppcaml::typing::inlining_transforms
