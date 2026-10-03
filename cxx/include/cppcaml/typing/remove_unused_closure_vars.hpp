// Port of middle_end/flambda/remove_unused_closure_vars.ml: closure
// variables and functions of sets of closures nothing uses, removed.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::remove_unused_closure_vars {

flambda::Program remove_unused_closure_variables(bool remove_direct_call_surrogates, const flambda::Program& program);

}  // namespace cppcaml::typing::remove_unused_closure_vars
