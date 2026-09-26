// DEPRECATED AS A TYPE CHECKER (2026-09-26) -- see TYPECHECKER.md.
// A typedtree TRANSCRIBER for already-valid programs: it does not unify and
// rejects nothing.
// The goal is a faithful port of ocamlc's typing/; do not grow this into a
// checker.
// The type-checker: parsetree -> typedtree.  Currently a transcriber over the
// no-external-reference fragment (let bindings, var/any patterns, constants,
// tuples); name resolution and inference grow it from here.  Validated against
// `ocamlc -dtypedtree`.
#pragma once

#include <stdexcept>

#include "cppcaml/ast.hpp"
#include "cppcaml/typedtree.hpp"

namespace cppcaml {

struct TypeError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

typedtree::Structure type_structure(const ast::Structure& s);

}  // namespace cppcaml
