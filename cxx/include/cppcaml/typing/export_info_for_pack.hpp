// Port of middle_end/flambda/export_info_for_pack.ml: the export
// information of a packed unit's members, renamed to live within the pack
// (their export IDs, symbols, sets of closures).
#pragma once

#include "cppcaml/typing/export_info.hpp"

namespace cppcaml::typing::export_info_for_pack {

// import_for_pack ~pack_units ~pack exp
const export_info::T* import_for_pack(const OSet<compilation_unit::t, compilation_unit::Cmp>& pack_units,
                                      compilation_unit::t pack, const export_info::T* exp);
void clear_import_state();

}  // namespace cppcaml::typing::export_info_for_pack
