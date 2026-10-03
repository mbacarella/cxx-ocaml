// Port of middle_end/flambda/flambda_utils.ml (see flambda_utils.hpp).
#include "cppcaml/typing/flambda_utils.hpp"

namespace cppcaml::typing::flambda_utils {

flambda::t name_expr(flambda::named named, internal_variable_names::t name) {
  variable::t var = variable::create(name, compilation_unit::get_current_exn());
  return flambda::create_let(var, named, flambda::var(var));
}

flambda::t name_expr_from_var(flambda::named named, variable::t v) {
  variable::t var = variable::rename(v, compilation_unit::get_current_exn());
  return flambda::create_let(var, named, flambda::var(var));
}

}  // namespace cppcaml::typing::flambda_utils
