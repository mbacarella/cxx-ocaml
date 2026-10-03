// Port of middle_end/flambda/share_constants.ml: equal constant
// definitions shared (one symbol), except mutable ones and the unit's own.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::share_constants {

flambda::Program share_constants(const flambda::Program& program);

}  // namespace cppcaml::typing::share_constants
