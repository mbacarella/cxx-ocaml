// Port of middle_end/flambda/lift_let_to_initialize_symbol.ml: the
// toplevel lets of Initialize_symbol and Effect expressions turned into
// their own symbols.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::lift_let_to_initialize_symbol {

// lift ~backend program
flambda::Program lift(const flambda::Program& program);

}  // namespace cppcaml::typing::lift_let_to_initialize_symbol
