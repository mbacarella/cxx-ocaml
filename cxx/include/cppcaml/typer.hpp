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
