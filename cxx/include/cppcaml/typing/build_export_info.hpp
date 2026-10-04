// Port of middle_end/flambda/build_export_info.ml (with
// traverse_for_exported_symbols.ml): the description of what a unit
// exports -- its symbols' approximations as export descriptions, the sets
// of closures (and function bodies) reachable from its root symbol.
#pragma once

#include "cppcaml/typing/export_info.hpp"

namespace cppcaml::typing::build_export_info {

// build_transient ~backend program
export_info::Transient build_transient(const flambda::Program& program);

}  // namespace cppcaml::typing::build_export_info
