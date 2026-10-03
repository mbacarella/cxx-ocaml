// Port of middle_end/flambda/flambda_utils.ml (the functions ported so far:
// closure conversion's).
#pragma once

#include "cppcaml/typing/flambda.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"

namespace cppcaml::typing::flambda_utils {

// name_expr ~name named: let var = named in var, var fresh
flambda::t name_expr(flambda::named named, internal_variable_names::t name);
// name_expr_from_var ~var named: the same, var renamed
flambda::t name_expr_from_var(flambda::named named, variable::t var);

}  // namespace cppcaml::typing::flambda_utils
