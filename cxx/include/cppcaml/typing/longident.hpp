// Port of parsing/longident.ml as the typer sees it (TYPECHECKER.md):
//   t = Lident of string | Ldot of t loc * string loc | Lapply of t loc * t loc
// Trunk keeps a location on each component; Env reports lookup errors at
// them.  The C++ parser's ast::Longident does not carry the inner locations
// yet, so `of_ast` gives every component the enclosing location (a known
// gap until the parser records them).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/ast.hpp"
#include "cppcaml/typing/support.hpp"

namespace cppcaml::typing {

struct Longident {
  enum class Kind : std::uint8_t { Lident, Ldot, Lapply };
  Kind kind;
  std::string_view s;               // Lident name / Ldot component
  Location s_loc;                   // Ldot: the component's location
  const Longident* l1 = nullptr;    // Ldot prefix / Lapply functor
  Location l1_loc;
  const Longident* l2 = nullptr;    // Lapply argument
  Location l2_loc;

  using t = const Longident*;
  static t lident(std::string_view s);
  static t ldot(t prefix, const Location& prefix_loc, std::string_view s, const Location& s_loc);
  static t lapply(t f, const Location& f_loc, t a, const Location& a_loc);
};

namespace longident {
using t = Longident::t;
std::string_view last(t lid);
// flatten: the components of a path without applications
std::vector<std::string_view> flatten(t lid);
// Pprintast.longident
std::string to_string(t lid);
// The parser's longident, every component located at `loc`.
t of_ast(const ast::Longident& lid, const Location& loc);
// ast::Location -> Location (file name supplied by the caller)
Location loc_of_ast(const ast::Location& l, std::string_view fname);
}  // namespace longident

}  // namespace cppcaml::typing
