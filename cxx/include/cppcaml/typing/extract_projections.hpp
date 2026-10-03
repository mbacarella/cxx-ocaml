// Port of middle_end/flambda/extract_projections.ml: the projections from
// specialised arguments or closure variables a function body makes that
// are valid in the current environment.
#pragma once

#include "cppcaml/typing/inline_and_simplify_aux.hpp"

namespace cppcaml::typing::extract_projections {

using ProjSet = OSet<projection::t, inline_and_simplify_aux::ProjCmp>;

// from_function_decl ~env ~which_variables ~function_decl
ProjSet from_function_decl(const inline_and_simplify_aux::Env& env,
                           const variable::Map<flambda::SpecialisedTo>& which_variables,
                           const flambda::FunctionDeclaration* function_decl);

}  // namespace cppcaml::typing::extract_projections
