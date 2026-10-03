// Port of middle_end/flambda/ref_to_variables.ml: local references (blocks
// of tag 0 allocated by [Pmakeblock] and only read, written and offset in
// place) turned into mutable variables, one per field.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::ref_to_variables {

flambda::Program eliminate_ref(const flambda::Program& program);

}  // namespace cppcaml::typing::ref_to_variables
