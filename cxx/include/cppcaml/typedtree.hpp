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

// A resolved identifier (Ident.t).  Ident.print renders:
//   Local  -> name/stamp        Global -> name!        Predef -> name/stamp!
struct Ident {
  enum Kind { Local, Global, Predef };
  std::string name;
  long long stamp = 0;
  Kind kind = Local;
};

// Resolved Path.t.
struct Path;
using PathBox = std::shared_ptr<Path>;
struct Pident { Ident id; };
struct Pdot { PathBox prefix; std::string name; };
struct Path { std::variant<Pident, Pdot> v; };

// --- core types (Ttyp_*) ---
struct CoreType;
using CoreTypeBox = Box<CoreType>;
struct Ttyp_any {};
struct Ttyp_var { std::string name; };
struct Ttyp_arrow { ArgLabel label; CoreTypeBox dom; CoreTypeBox cod; };
struct Ttyp_tuple {
  std::vector<std::pair<std::optional<std::string>, CoreTypeBox>> elems;
};
struct Ttyp_constr { Path path; std::vector<CoreTypeBox> args; };
struct Ttyp_poly { std::vector<std::string> vars; CoreTypeBox type; };
struct CoreType {
  std::variant<Ttyp_any, Ttyp_var, Ttyp_arrow, Ttyp_tuple, Ttyp_constr, Ttyp_poly>
      desc;
  Location loc;
};

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
enum class Direction { Up, Down };
struct Texp_for { Ident var; Direction dir; ExprBox lo; ExprBox hi; ExprBox body; };
struct Texp_lazy { ExprBox e; };
struct Expression {
  std::variant<Texp_constant, Texp_ident, Texp_tuple, Texp_apply, Texp_function,
               Texp_let, Texp_ifthenelse, Texp_sequence, Texp_match, Texp_try,
               Texp_construct, Texp_array, Texp_assert, Texp_for, Texp_lazy>
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

// --- type declarations ---
struct LabelDecl {
  Location loc;
  bool mutable_ = false;
  bool atomic = false;
  Ident id;
  CoreType type;  // already Ttyp_poly-wrapped (record fields always are)
};
struct ConstructorDecl {
  Location loc;
  Ident id;
  std::vector<CoreTypeBox> args;  // Cstr_tuple argument types
  std::optional<CoreTypeBox> res; // GADT return type
};
struct Ttype_abstract {};
struct Ttype_variant { std::vector<ConstructorDecl> ctors; };
struct Ttype_record { std::vector<LabelDecl> labels; };
struct Ttype_open {};
struct TypeKind {
  std::variant<Ttype_abstract, Ttype_variant, Ttype_record, Ttype_open> v;
};
struct TypeDeclaration {
  Ident id;
  Location loc;
  std::vector<CoreTypeBox> params;
  TypeKind kind;
  bool private_ = false;
  std::optional<CoreTypeBox> manifest;
};

// --- structure ---
struct Tstr_value {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
};
struct Tstr_eval { ExprBox e; };
struct Tstr_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Tstr_primitive {
  Ident id;
  Location loc;
  CoreType type;
  std::vector<std::string> prims;  // the "external" strings
};
struct Tstr_exception {
  Location loc;
  Ident id;
  std::vector<CoreTypeBox> args;
  std::optional<CoreTypeBox> res;
};
struct StructureItem {
  std::variant<Tstr_value, Tstr_eval, Tstr_type, Tstr_primitive, Tstr_exception>
      desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

// Render a structure in `ocamlc -dtypedtree` format.
void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});

}  // namespace cppcaml::typedtree
