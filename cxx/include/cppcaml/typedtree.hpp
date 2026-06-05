// Typedtree IR — the type-checker's output, consumed later by the lambda stage.
// Validated against `ocamlc -dtypedtree`.  Like ast.hpp it starts as a small
// fragment and grows one construct at a time.  Note: printtyped emits no
// inferred type_expr, so the dump is tree shape + resolved idents/paths +
// stamps; we reuse ast::{Location,Constant,ArgLabel,RecFlag} verbatim.
#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cppcaml/ast.hpp"

namespace cppcaml::typedtree {

using ast::ArgLabel;
using ast::Constant;
using ast::Location;
using ast::RecFlag;

template <class T>
using Box = std::unique_ptr<T>;

// A resolved identifier (Ident.t).  `global` marks a persistent/global ident,
// which Ident.print renders with a trailing '!' (e.g. "Stdlib!").
struct Ident {
  std::string name;
  long long stamp = 0;
  bool global = false;
};

// Resolved Path.t.
struct Path;
using PathBox = std::shared_ptr<Path>;
struct Pident { Ident id; };
struct Pdot { PathBox prefix; std::string name; };
struct Path { std::variant<Pident, Pdot> v; };

struct Pattern;
struct Expression;
using PatBox = Box<Pattern>;
using ExprBox = Box<Expression>;

// --- patterns ---
struct Tpat_any {};
struct Tpat_var { Ident id; };
struct Pattern {
  std::variant<Tpat_any, Tpat_var> desc;
  Location loc;
};

// --- expressions ---
struct Texp_constant { Constant c; };
struct Texp_ident { Path path; };
struct Texp_tuple {
  std::vector<std::pair<std::optional<std::string>, ExprBox>> elems;  // label, value
};
struct Texp_apply {
  ExprBox fn;
  std::vector<std::pair<ArgLabel, ExprBox>> args;
};
// A single value parameter of a Texp_function (Param_pat form).
struct FunctionParam {
  ArgLabel label;
  PatBox pat;
};
struct Texp_function {
  std::vector<FunctionParam> params;
  ExprBox body;  // Tfunction_body
};
struct Expression {
  std::variant<Texp_constant, Texp_ident, Texp_tuple, Texp_apply, Texp_function>
      desc;
  Location loc;
};

// --- structure ---
struct ValueBinding {
  Pattern pat;
  Expression expr;
};
struct Tstr_value {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
};
struct StructureItem {
  std::variant<Tstr_value> desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

// Render a structure in `ocamlc -dtypedtree` format.
void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});

}  // namespace cppcaml::typedtree
