// Port of asmcomp/cmmgen.ml: translation from Clambda to C--.
#pragma once

#include <vector>

#include "cppcaml/typing/closure.hpp"
#include "cppcaml/typing/cmm.hpp"

namespace cppcaml::typing::cmmgen {

// compunit (ulam, preallocated_blocks, constants): the unit's phrases
std::vector<cmm::Phrase> compunit(const closure_middle_end::WithConstants& c);

}  // namespace cppcaml::typing::cmmgen
