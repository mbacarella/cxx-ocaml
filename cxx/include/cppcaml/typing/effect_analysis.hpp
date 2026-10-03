// Port of middle_end/flambda/effect_analysis.ml: which Flambda terms have
// no side effects.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::effect_analysis {

bool no_effects_prim(const clambda::Primitive& prim);
bool no_effects(flambda::t flam);
bool no_effects_named(flambda::named named);

}  // namespace cppcaml::typing::effect_analysis
