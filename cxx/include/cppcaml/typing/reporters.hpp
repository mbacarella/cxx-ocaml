// The error reports of the typing/ port's modules (TYPECHECKER.md stage 9):
// each module's `report_error` and the `Location.register_error_of_exn`
// that ocamlc runs when the module initializes.  install() registers them
// all, once.
#pragma once

#include <optional>

#include "cppcaml/typing/location.hpp"

namespace cppcaml::typing::reporters {

void install();

// per module (called by install)
void register_env();
void register_typetexp();
void register_typecore();
void register_typedecl();
void register_typemod();
void register_typeclass();
void register_includemod();
void register_misc();  // Primitive, Attr_helper, Syntaxerr, Persistent_env, Cmi_format, Translcore, ...

}  // namespace cppcaml::typing::reporters
