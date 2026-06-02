// Recursive-descent + Pratt parser producing the Parsetree AST (subset).
// Validated against `ocamlc -dparsetree`. The accepted fragment grows over time.
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

#include "cppcaml/ast.hpp"

namespace cppcaml {

struct ParseError : std::runtime_error {
  size_t pos;
  ParseError(std::string m, size_t p) : std::runtime_error(std::move(m)), pos(p) {}
};

// Parse a compilation unit (.ml structure). `src` must outlive the call.
ast::Structure parse_structure(std::string_view src);

namespace ast {
// Render a structure in `ocamlc -dparsetree` format (see ast_print.cpp).
void print_dparsetree(const Structure& s, std::string_view fname, std::ostream& os);
}  // namespace ast

}  // namespace cppcaml
