// Port of middle_end/flambda/flambda_middle_end.ml: Lambda through the
// flambda passes.  Ported so far: closure conversion and some passes; the
// first pass not ported stops the middle end (NotPorted).
#pragma once

#include <stdexcept>
#include <string>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_middle_end {

// a pass of the pipeline that is not ported yet (its -dflambda-verbose
// header and input printed first)
struct NotPorted : std::runtime_error {
  explicit NotPorted(const std::string& pass) : std::runtime_error(pass) {}
};

// lambda_to_flambda ~ppf_dump ~prefixname ~backend ~size ~module_ident
//   ~module_initializer (the -d dumps on ppf_dump)
flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, long size, Ident::t module_ident,
                                   lambda::lambda module_initializer);

}  // namespace cppcaml::typing::flambda_middle_end
