// Port of middle_end/flambda/lift_constants.ml: constant let-bound values
// (and closed sets of closures) lifted to symbols, the program's symbol
// definitions sorted by dependency.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::lift_constants {

// lift_constants program ~backend
flambda::Program lift_constants(const flambda::Program& program);

}  // namespace cppcaml::typing::lift_constants
