// Port of middle_end/flambda/closure_conversion.ml and
// closure_conversion_aux.ml: Lambda to Flambda (the first pass of the
// flambda middle end).
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::closure_conversion {

// lambda_to_flambda ~backend ~module_ident ~size lam (the backend is
// ocamlopt's: Compilenv.symbol_for_global', amd64's size_int/big_endian)
flambda::Program lambda_to_flambda(Ident::t module_ident, long size, lambda::lambda lam);

}  // namespace cppcaml::typing::closure_conversion
