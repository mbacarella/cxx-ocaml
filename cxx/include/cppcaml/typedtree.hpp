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
struct Tpat_constant { Constant c; };
struct Tpat_construct {
  std::string name;            // fmt_longident of the constructor
  std::vector<PatBox> args;
};
struct Tpat_value { PatBox inner; };  // computation-pattern wrapper (match cases)
struct Pattern {
  std::variant<Tpat_any, Tpat_var, Tpat_constant, Tpat_construct, Tpat_value> desc;
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
struct ValueBinding;  // (defined below; used by Texp_let)
struct Case;
struct Texp_let {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
  ExprBox body;
};
struct Texp_ifthenelse { ExprBox cond; ExprBox then_; std::optional<ExprBox> else_; };
struct Texp_sequence { ExprBox e1; ExprBox e2; };
struct Texp_match { ExprBox scrut; std::vector<Case> cases; };  // cases are computation
struct Texp_try { ExprBox body; std::vector<Case> cases; };     // cases are value
struct Texp_construct { std::string name; std::vector<ExprBox> args; };
struct Texp_array { std::vector<ExprBox> elems; };
struct Texp_assert { ExprBox e; };
struct Expression {
  std::variant<Texp_constant, Texp_ident, Texp_tuple, Texp_apply, Texp_function,
               Texp_let, Texp_ifthenelse, Texp_sequence, Texp_match, Texp_try,
               Texp_construct, Texp_array, Texp_assert>
      desc;
  Location loc;
};

struct ValueBinding {
  Pattern pat;
  Expression expr;
};
struct Case {
  Pattern lhs;
  std::optional<ExprBox> guard;
  ExprBox rhs;
};

// --- structure ---
struct Tstr_value {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
};
struct Tstr_eval { ExprBox e; };
struct StructureItem {
  std::variant<Tstr_value, Tstr_eval> desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

// Render a structure in `ocamlc -dtypedtree` format.
void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});

}  // namespace cppcaml::typedtree
