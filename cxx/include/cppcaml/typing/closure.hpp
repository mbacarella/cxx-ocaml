// Port of middle_end/closure/closure.ml and closure_middle_end.ml: the
// Closure middle end -- introduction of closures, uncurrying, recognition
// of direct calls, inlining of small functions and constant propagation --
// from Lambda to Clambda.
#pragma once

#include <vector>

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::closure {

clambda::ulambda intro(long size, lambda::lambda lam);
void reset();

}  // namespace cppcaml::typing::closure

namespace cppcaml::typing::closure_middle_end {

// Clambda.with_constants
struct WithConstants {
  clambda::ulambda code;
  std::vector<clambda::PreallocatedBlock> blocks;
  std::vector<clambda::PreallocatedConstant> constants;
};

// lambda_to_clambda: Closure.intro, the preallocated module block, the
// structured constants; -drawclambda / -dclambda on ppf_dump
WithConstants lambda_to_clambda(format::Formatter& ppf_dump, const lambda::Program& program, lambda::lambda code);

}  // namespace cppcaml::typing::closure_middle_end
