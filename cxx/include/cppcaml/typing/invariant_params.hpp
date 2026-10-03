// Port of middle_end/flambda/invariant_params.ml and
// find_recursive_functions.ml: the dataflow of parameters through the
// recursive calls of a set of closures.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::invariant_params {

using Pair = std::pair<variable::t, variable::t>;  // Variable.Pair.t
struct PairCmp {
  int operator()(const Pair& a, const Pair& b) const {
    int c = variable::compare(a.first, b.first);
    return c != 0 ? c : variable::compare(a.second, b.second);
  }
};
using PairSet = OSet<Pair, PairCmp>;

// invariant_params_in_recursion decls ~backend
variable::Map<variable::Set> invariant_params_in_recursion(const flambda::FunctionDeclarations* decls);
// invariant_param_sources decls ~backend
variable::Map<PairSet> invariant_param_sources(const flambda::FunctionDeclarations* decls);
// unused_arguments decls ~backend
variable::Set unused_arguments(const flambda::FunctionDeclarations* decls);

}  // namespace cppcaml::typing::invariant_params

namespace cppcaml::typing::find_recursive_functions {
// in_function_declarations function_decls ~backend
variable::Set in_function_declarations(const flambda::FunctionDeclarations* function_decls);
}  // namespace cppcaml::typing::find_recursive_functions
