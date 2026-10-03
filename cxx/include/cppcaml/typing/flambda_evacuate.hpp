// Evacuation of a flambda program: the middle end runs each pass in a
// transient zone of its own, and the program a pass leaves -- with what it
// reaches in the dying zones -- is copied into the current zone, so the
// dying ones (the pass's garbage: the approximations, the environments,
// the speculatively inlined bodies the OCaml GC would reclaim) can be
// dropped.
//
// One copy per object: sharing, and so physical equality, is kept.  What
// is outside the dying zones is left alone (it never points into them):
// the identifiers (Variable, Symbol, ... in the permanent zone), the
// debuginfo lists (whose identity is their address), the imported
// functions' bodies.  A pointer to an identifier or a debuginfo list in a
// dying zone is a fatal error -- it must not be copied.
#pragma once

#include <vector>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_evacuate {

flambda::Program evacuate(const flambda::Program& program, const std::vector<const Zone*>& dying);

}  // namespace cppcaml::typing::flambda_evacuate
