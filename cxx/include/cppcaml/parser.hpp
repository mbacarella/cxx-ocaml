// Recursive-descent + Pratt parser producing the Parsetree AST (subset).
// Validated against `ocamlc -dparsetree`. The accepted fragment grows over time.
#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/ast.hpp"

namespace cppcaml {

// Syntaxerr.Error: Other (a plain "Syntax error" at the offending token),
// Unclosed (an opening delimiter never closed) or Expecting (a nonterminal
// expected).  pos / end: the location (the offending token's span).
struct ParseError : std::runtime_error {
  enum class Kind { Other, Unclosed, Expecting, Not_expecting };
  Kind kind = Kind::Other;
  size_t pos;
  size_t end;
  std::string what_;                        // Unclosed: closing; Expecting / Not_expecting: nonterminal
  size_t open_pos = 0, open_end = 0;        // Unclosed: the opening delimiter
  std::string opening;                      // Unclosed
  // the positions, resolved by the parser (`# N "file"` directives applied);
  // file: empty for the source path
  struct Pos {
    long lnum = 0, bol = 0, cnum = 0;
    std::string file;
  };
  bool resolved = false;
  Pos p_start, p_end, p_open_start, p_open_end;
  ParseError(std::string m, size_t p) : std::runtime_error(std::move(m)), pos(p), end(p) {}
};

// Parse a compilation unit (.ml structure). `src` must outlive the call.
ast::Structure parse_structure(std::string_view src);
// Variant that also reports the filenames named by `# N "file"` directives
// (in file_id order: directive_files[k] is file_id k+1).
ast::Structure parse_structure(std::string_view src, std::vector<std::string>& directive_files);
// Parse an interface (.mli signature). `src` must outlive the call.
ast::Signature parse_signature(std::string_view src);
// ... and the filenames named by its `# N "file"` directives (as above)
ast::Signature parse_signature(std::string_view src, std::vector<std::string>& directive_files);

namespace ast {
// Lexer.comments () of the last parse (ast::Comment: text and location)
const std::vector<Comment>& last_comments();
// Render a structure in `ocamlc -dparsetree` format (see ast_print.cpp).
// `dirfiles` supplies the directive filenames (file_id>0); fname is file_id 0.
void print_dparsetree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});
// ... and an interface (Printast.interface)
void print_dparsetree(const Signature& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});
}  // namespace ast

}  // namespace cppcaml
