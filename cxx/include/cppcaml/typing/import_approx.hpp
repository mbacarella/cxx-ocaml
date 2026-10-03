// Port of middle_end/flambda/import_approx.ml: the approximations of other
// units' symbols and export ids, read from their .cmx export info.
#pragma once

#include "cppcaml/typing/simple_value_approx.hpp"

namespace cppcaml::typing::import_approx {

namespace A = simple_value_approx;

A::t import_symbol(symbol::t sym);
A::Descr really_import(const A::Descr& approx);
A::t really_import_approx(A::t approx);
// Compilenv.imported_sets_of_closures_table, cleared by Compilenv.reset
void clear_imported_sets_of_closures_table();

}  // namespace cppcaml::typing::import_approx
