// Port of parsing/ast_invariants.ml (TYPECHECKER.md): the checks Pparse
// runs on an AST it did not parse itself (a binary AST file, a -ppx
// rewriter's output), over Ast_iterator.default_iterator's traversal, and
// the registration of the attributes met outside attribute payloads
// (Builtin_attributes.register_attr Invariant_check, for warning 53).
#pragma once

#include <stdexcept>
#include <string>

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing {

namespace syntaxerr {
// Syntaxerr.Error (Ill_formed_ast (loc, s)): Syntaxerr.ill_formed_ast
struct IllFormedAst : std::runtime_error {
  Location loc;
  std::string msg;
  IllFormedAst(const Location& l, std::string m) : std::runtime_error("Syntaxerr.Error"), loc(l), msg(std::move(m)) {}
};
}  // namespace syntaxerr

namespace ast_invariants {
void structure(parsetree::Structure st);
void signature(parsetree::Signature sg);
}  // namespace ast_invariants

}  // namespace cppcaml::typing
