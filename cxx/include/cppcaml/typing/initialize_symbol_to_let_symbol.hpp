// Port of middle_end/flambda/initialize_symbol_to_let_symbol.ml: an
// [Initialize_symbol] whose fields are all constants or symbols becomes a
// [Let_symbol] of a constant block.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::initialize_symbol_to_let_symbol {

flambda::Program run(const flambda::Program& program);

}  // namespace cppcaml::typing::initialize_symbol_to_let_symbol
