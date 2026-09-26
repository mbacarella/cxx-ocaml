// Port of parsing/builtin_attributes.ml, as far as the typer has needed it.
//
// Deviation (TYPECHECKER.md): warnings are not ported yet, so
// `warning_scope` does not process [@warning] / [@ocaml.warning]
// attributes -- it only runs its body.  This matters for diagnostics only.
#pragma once

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::builtin_attributes {

// attr_equals_builtin: `s` or `ocaml.s`
inline bool attr_equals_builtin(std::string_view txt, std::string_view s) {
  return txt == s || (txt.size() == 6 + s.size() && txt.substr(0, 6) == "ocaml." &&
                      txt.substr(6) == s);
}
inline bool has_attribute(std::string_view nm, const parsetree::Attributes& attrs) {
  for (const parsetree::Attribute* a : attrs)
    if (attr_equals_builtin(a->attr_name.txt, nm)) return true;
  return false;
}
inline bool explicit_arity(const parsetree::Attributes& attrs) {
  return has_attribute("explicit_arity", attrs);
}

template <class F>
auto warning_scope(const parsetree::Attributes&, F&& f) -> decltype(f()) {
  return f();
}

}  // namespace cppcaml::typing::builtin_attributes
