// Port of middle_end/flambda/flambda_middle_end.ml: Lambda through the
// flambda passes.  Ported so far: closure conversion and some passes; the
// first pass not ported stops the middle end (NotPorted).
#pragma once

#include <stdexcept>
#include <string>

#include "cppcaml/typing/closure.hpp"
#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_middle_end {

// a pass of the pipeline that is not ported yet (its -dflambda-verbose
// header and input printed first)
struct NotPorted : std::runtime_error {
  explicit NotPorted(const std::string& pass) : std::runtime_error(pass) {}
};

// lambda_to_flambda ~ppf_dump ~prefixname ~backend ~size ~module_ident
//   ~module_initializer (the -d dumps on ppf_dump)
flambda::Program lambda_to_flambda(format::Formatter& ppf_dump, const std::string& prefixname, long size,
                                   Ident::t module_ident,
                                   lambda::lambda module_initializer);

// lambda_to_clambda ~backend ~prefixname ~ppf_dump program: the middle end,
// Build_export_info, Flambda_to_clambda (-drawclambda), the export info
// recorded (Compilenv.set_export_info), Un_anf (-dclambda)
closure_middle_end::WithConstants lambda_to_clambda(format::Formatter& ppf_dump, const std::string& prefixname,
                                                    const lambda::Program& program,
                                                    lambda::lambda module_initializer);

}  // namespace cppcaml::typing::flambda_middle_end
