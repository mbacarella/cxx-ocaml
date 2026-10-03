// Port of middle_end/flambda/remove_unused_program_constructs.ml: symbol
// definitions nothing depends on, and effect-free effects, removed.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::remove_unused_program_constructs {

flambda::Program remove_unused_program_constructs(const flambda::Program& program);

}  // namespace cppcaml::typing::remove_unused_program_constructs
