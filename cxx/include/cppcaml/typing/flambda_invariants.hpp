// Port of middle_end/flambda/flambda_invariants.ml: the checks a flambda
// program must pass after closure conversion and every pass
// (-dflambda-invariants, the default where the compiler is configured with
// codegen invariants).
#pragma once

#include <string>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_invariants {

// check_exn program: a violated invariant is printed on stderr (">> ...")
// and thrown as Failed; an assertion of the checker as AssertFailure.
// [what] is the exception as Printexc.to_string shows it.
struct Failed {
  std::string what;
};
void check_exn(const flambda::Program& program);

}  // namespace cppcaml::typing::flambda_invariants
