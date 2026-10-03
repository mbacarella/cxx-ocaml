// Port of middle_end/flambda/inlining_transforms.ml: inlining a function by
// copying its body to the call site, and specialising it by copying its
// declaration into a new set of closures.
#pragma once

#include "cppcaml/typing/inlining_decision.hpp"

namespace cppcaml::typing::inlining_transforms {

namespace A = simple_value_approx;
using inline_and_simplify_aux::Env;
using inline_and_simplify_aux::Result;
using inlining_decision::Simplify;

std::pair<flambda::t, Result> inline_by_copying_function_body(
    const Env& env, const Result& r, variable::t lhs_of_application, lambda::InlineAttribute inline_requested,
    lambda::SpecialiseAttribute specialise_requested, variable::t closure_id_being_applied,
    const A::FunctionDeclaration* function_decl, const A::FunctionBody* function_body, const variable::Set& fun_vars,
    Slice<variable::t> args, const debuginfo::t& dbg, const Simplify& simplify);

// (nullopt: None)
std::optional<std::pair<flambda::t, Result>> inline_by_copying_function_declaration(
    const Env& env, const Result& r, const A::FunctionDeclarations* function_decls, variable::t lhs_of_application,
    lambda::InlineAttribute inline_requested, variable::t closure_id_being_applied,
    const A::FunctionDeclaration* function_decl, Slice<variable::t> args, const std::vector<A::t>& args_approxs,
    const A::Lazy<variable::Map<variable::Set>>& invariant_params,
    const variable::Map<flambda::SpecialisedTo>& specialised_args,
    const variable::Map<flambda::SpecialisedTo>& free_vars, const variable::Map<variable::t>& direct_call_surrogates,
    const debuginfo::t& dbg, const Simplify& simplify);

}  // namespace cppcaml::typing::inlining_transforms
