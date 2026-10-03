// Port of middle_end/flambda/flambda_middle_end.ml: Lambda through the
// flambda passes.  Ported so far: closure conversion (and -drawflambda);
// the passes after it are not.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_middle_end {

// lambda_to_flambda ~ppf_dump ~prefixname ~backend ~size ~module_ident
//   ~module_initializer, up to the passes not ported yet (the -d dumps on
//   ppf_dump)
flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, long size, Ident::t module_ident,
                                   lambda::lambda module_initializer);

}  // namespace cppcaml::typing::flambda_middle_end
