// Port of parsing/builtin_attributes.ml, as far as the typer has needed it.
//
// Deviation (TYPECHECKER.md): warnings are not ported yet, so
// `warning_scope` does not process [@warning] / [@ocaml.warning]
// attributes -- it only runs its body.  This matters for diagnostics only.
#pragma once

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::builtin_attributes {

template <class F>
auto warning_scope(const parsetree::Attributes&, F&& f) -> decltype(f()) {
  return f();
}

}  // namespace cppcaml::typing::builtin_attributes
