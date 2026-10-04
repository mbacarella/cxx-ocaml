// Port of middle_end/flambda/flambda_to_clambda.ml: a flambda program
// (with its export information) to Clambda -- the runtime closures laid out,
// the constants and preallocated blocks of the symbols' definitions.
#pragma once

#include <vector>

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/export_info.hpp"

namespace cppcaml::typing::flambda_to_clambda {

struct Result {
  clambda::ulambda expr;
  std::vector<clambda::PreallocatedBlock> preallocated_blocks;
  // Symbol.Map: in the symbols' order
  std::vector<std::pair<symbol::t, const clambda::UStructuredConstant*>> structured_constants;
  const export_info::T* exported;
};

// convert ~ppf_dump (program, exported_transient)
Result convert(format::Formatter& ppf_dump, const flambda::Program& program,
               const export_info::Transient& exported_transient);

}  // namespace cppcaml::typing::flambda_to_clambda
