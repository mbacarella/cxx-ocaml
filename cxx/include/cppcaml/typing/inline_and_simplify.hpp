// Port of middle_end/flambda/inline_and_simplify.ml: the main flambda
// pass -- simplification of terms from approximations, inlining and
// specialisation of direct calls, and the set-of-closures transformations.
#pragma once

#include <string>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::inline_and_simplify {

// run ~never_inline ~backend ~prefixname ~round ~ppf_dump program
flambda::Program run(bool never_inline, const std::string& prefixname, long round, format::Formatter& ppf_dump,
                     const flambda::Program& program);

}  // namespace cppcaml::typing::inline_and_simplify
