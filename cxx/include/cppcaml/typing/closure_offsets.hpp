// Port of middle_end/flambda/closure_offsets.ml: the positions of the
// functions' infix closures and of the free variables in the runtime
// closure blocks of a program's sets of closures.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::closure_offsets {

struct Result {
  variable::Map<long> function_offsets;       // Closure_id.Map
  variable::Map<long> free_variable_offsets;  // Var_within_closure.Map
};

Result compute(const flambda::Program& program);

}  // namespace cppcaml::typing::closure_offsets
