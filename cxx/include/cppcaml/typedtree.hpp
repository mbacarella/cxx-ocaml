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
struct Tpat_exception { PatBox inner; };  // computation: `exception P`
struct Tpat_tuple {
  std::vector<std::pair<std::optional<std::string>, PatBox>> elems;
};
struct Tpat_or { PatBox left; PatBox right; };
struct Tpat_alias { Ident id; PatBox inner; };  // (p as id)
struct PatExtra { CoreType ctype; Location loc; };  // Tpat_extra_constraint
struct Pattern {
  std::variant<Tpat_any, Tpat_var, Tpat_constant, Tpat_construct, Tpat_value,
               Tpat_tuple, Tpat_exception, Tpat_or, Tpat_alias>
      desc;
  Location loc;
  const ast::Attributes* attrs = nullptr;
  std::vector<PatExtra> extras;
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
  // Exactly one form is used: Tfunction_body (body set) when there are params,
  // or Tfunction_cases (is_cases) for a bare `function ... | ...`.
  bool is_cases = false;
  ExprBox body;                 // Tfunction_body
  Location cases_loc;           // Tfunction_cases
  std::vector<struct Case> cases;
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
struct Texp_while { ExprBox cond; ExprBox body; };
// A record field in Texp_record: Overridden (name + value) or Kept (`with`).
struct RecordField { bool kept = false; std::string name; ExprBox value; };
struct Texp_record {
  std::vector<RecordField> fields;          // in declaration order
  std::string representation = "Record_regular";
  std::optional<ExprBox> extended;          // `{ e with ... }`
};
struct Texp_field { ExprBox record; std::string name; };
struct ExprExtra { CoreType ctype; Location loc; };  // Texp_constraint
struct Expression {
  std::variant<Texp_constant, Texp_ident, Texp_tuple, Texp_apply, Texp_function,
               Texp_let, Texp_ifthenelse, Texp_sequence, Texp_match, Texp_try,
               Texp_construct, Texp_array, Texp_assert, Texp_for, Texp_lazy,
               Texp_while, Texp_record, Texp_field>
      desc;
  Location loc;
  const ast::Attributes* attrs = nullptr;
  std::vector<ExprExtra> extras;
};

struct ValueBinding {
  Pattern pat;
  Expression expr;
  const ast::Attributes* attrs = nullptr;
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
  const ast::Attributes* attrs = nullptr;
};

// --- module types / signatures (fragment) ---
struct ValueDesc {
  Ident id;
  CoreType type;
  Location loc;
  const ast::Attributes* attrs = nullptr;
};
struct Tsig_value { ValueDesc vd; };
struct Tsig_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct SignatureItem {
  std::variant<Tsig_value, Tsig_type> desc;
  Location loc;
};
struct Tmty_ident { Path path; };
struct Tmty_signature { std::vector<SignatureItem> items; };
struct ModuleType {
  std::variant<Tmty_ident, Tmty_signature> desc;
  Location loc;
};
using ModuleTypeBox2 = Box<ModuleType>;

// Module expressions are mutually recursive with structures, so ModuleExpr is
// defined after StructureItem and referenced here through a box.
struct ModuleExpr;
using ModuleExprBox = Box<ModuleExpr>;

// --- structure ---
struct Tstr_value {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
};
struct Tstr_eval { ExprBox e; };
struct Tstr_open { bool override_ = false; ModuleExprBox expr; };
struct Tstr_module { bool present = true; Ident id; ModuleExprBox expr; };
struct Tstr_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Tstr_primitive {
  Ident id;
  Location loc;
  CoreType type;
  std::vector<std::string> prims;  // the "external" strings
};
// An extension constructor (Text_decl form): shared by exceptions and typexts.
struct ExtCtor {
  Location loc;
  Ident id;
  std::vector<CoreTypeBox> args;
  std::optional<CoreTypeBox> res;
};
struct Tstr_exception {
  ExtCtor ctor;
  const ast::Attributes* attrs = nullptr;  // ptyexn_attributes
};
struct Tstr_typext {
  Path path;
  std::vector<CoreTypeBox> params;
  std::vector<ExtCtor> ctors;
  bool private_ = false;
  const ast::Attributes* attrs = nullptr;
};
struct Tstr_attribute { std::string name; const ast::Structure* payload; };
struct Tstr_modtype { Ident id; ModuleTypeBox2 type; };  // type null = abstract
struct StructureItem {
  std::variant<Tstr_value, Tstr_eval, Tstr_type, Tstr_primitive, Tstr_exception,
               Tstr_open, Tstr_module, Tstr_attribute, Tstr_typext, Tstr_modtype>
      desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

// --- module expressions (now that Structure is complete) ---
struct Tmod_ident { Path path; };
struct Tmod_structure { Structure items; };
struct ModuleExpr {
  std::variant<Tmod_ident, Tmod_structure> desc;
  Location loc;
};

// Render a structure in `ocamlc -dtypedtree` format.
void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});

}  // namespace cppcaml::typedtree
