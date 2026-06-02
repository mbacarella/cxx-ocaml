// Parsetree AST (subset), faithful to parsing/parsetree.mli.
//
// Per the project data model, sum types are modelled as std::variant of node
// structs; recursive positions are boxed with std::unique_ptr. This is the first
// stage where that model is exercised in earnest. The set of constructors grows
// as the parser widens; today it covers the core expression/binding fragment.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cppcaml::ast {

// --- positions & locations (mirror Lexing.position / Location.t) ---
struct Position {
  int lnum = 1;     // 1-based line
  int bol = 0;      // byte offset of beginning of line
  int cnum = 0;     // byte offset of this position
};
struct Location {
  Position start;
  Position end;
  bool ghost = false;
};

template <class T>
using Box = std::unique_ptr<T>;

// --- Longident.t ---
// Children are boxed with shared_ptr so Longident/LongidentLoc stay *copyable*
// (they are immutable and routinely passed by value into several AST nodes).
struct Longident;
using LongidentBox = std::shared_ptr<Longident>;
struct Lident { std::string name; };
struct Ldot { LongidentBox prefix; std::string name; };
struct Lapply { LongidentBox f; LongidentBox x; };
struct Longident { std::variant<Lident, Ldot, Lapply> v; };
struct LongidentLoc { Longident txt; Location loc; };

struct StringLoc { std::string txt; Location loc; };

// --- constants ---
struct Pconst_integer { std::string value; std::optional<char> suffix; };
struct Pconst_char { int code = 0; };  // byte value 0..255
struct Pconst_string { std::string s; Location strloc; std::optional<std::string> delim; };
struct Pconst_float { std::string value; std::optional<char> suffix; };
struct Constant {
  std::variant<Pconst_integer, Pconst_char, Pconst_string, Pconst_float> desc;
  Location loc;
};

enum class RecFlag { Nonrecursive, Recursive };

// --- arg labels ---
struct Nolabel {};
struct Labelled { std::string name; };
struct Optional { std::string name; };
using ArgLabel = std::variant<Nolabel, Labelled, Optional>;

enum class ClosedFlag { Closed, Open };

// --- core types ---
struct CoreType;
using CoreTypeBox = Box<CoreType>;
struct Ptyp_any {};
struct Ptyp_var { std::string name; };
struct Ptyp_arrow { ArgLabel label; CoreTypeBox dom; CoreTypeBox cod; };
struct Ptyp_tuple { std::vector<CoreTypeBox> elems; };
struct Ptyp_constr { LongidentLoc id; std::vector<CoreTypeBox> args; };
struct CoreType {
  std::variant<Ptyp_any, Ptyp_var, Ptyp_arrow, Ptyp_tuple, Ptyp_constr> desc;
  Location loc;
};

// --- patterns ---
struct Pattern;
using PatBox = Box<Pattern>;
struct Ppat_any {};
struct Ppat_var { StringLoc name; };
struct Ppat_constant { Constant c; };
struct Ppat_tuple { std::vector<PatBox> elems; ClosedFlag closed = ClosedFlag::Closed; };
struct Ppat_construct { LongidentLoc id; std::optional<PatBox> arg; };  // vars [] in fragment
struct Ppat_or { PatBox l; PatBox r; };
struct Ppat_alias { PatBox p; StringLoc name; };
struct Ppat_constraint { PatBox p; CoreTypeBox t; };
struct Pattern {
  std::variant<Ppat_any, Ppat_var, Ppat_constant, Ppat_tuple, Ppat_construct,
               Ppat_or, Ppat_alias, Ppat_constraint>
      desc;
  Location loc;
};

// --- expressions (fragment) ---
struct Expression;
using ExprBox = Box<Expression>;
struct ValueBinding;
struct FunctionParam;
struct FunctionBody;

struct Pexp_ident { LongidentLoc id; };
struct Pexp_constant { Constant c; };
struct Pexp_apply { ExprBox fn; std::vector<std::pair<ArgLabel, ExprBox>> args; };
struct Pexp_let { RecFlag rf; std::vector<ValueBinding> bindings; ExprBox body; };
struct Pexp_function {
  std::vector<FunctionParam> params;
  // type constraint omitted in this fragment (always None)
  Box<FunctionBody> body;
};
struct Pexp_tuple { std::vector<ExprBox> elems; };  // all labels None in fragment
struct Pexp_ifthenelse { ExprBox cond; ExprBox then_; std::optional<ExprBox> else_; };
struct Case;
struct Pexp_construct { LongidentLoc id; std::optional<ExprBox> arg; };
struct Pexp_match { ExprBox e; std::vector<Case> cases; };
struct Pexp_try { ExprBox e; std::vector<Case> cases; };
struct Pexp_sequence { ExprBox e1; ExprBox e2; };
struct Pexp_constraint { ExprBox e; CoreTypeBox t; };
struct Pexp_field { ExprBox e; LongidentLoc field; };

struct Expression {
  std::variant<Pexp_ident, Pexp_constant, Pexp_apply, Pexp_let, Pexp_function,
               Pexp_tuple, Pexp_ifthenelse, Pexp_construct, Pexp_match, Pexp_try,
               Pexp_sequence, Pexp_constraint, Pexp_field>
      desc;
  Location loc;
};

struct Case { Pattern lhs; std::optional<ExprBox> guard; ExprBox rhs; };

struct ValueBinding {
  Pattern pat;
  ExprBox expr;
  // pvb_constraint omitted (None); attributes empty
};

struct Pparam_val { Location loc; ArgLabel label; Pattern pat; };  // default omitted (None)
struct FunctionParam { std::variant<Pparam_val> desc; };

struct Pfunction_body { ExprBox e; };
struct Pfunction_cases { std::vector<Case> cases; Location loc; };  // attrs empty
struct FunctionBody { std::variant<Pfunction_body, Pfunction_cases> v; };

// --- type declarations ---
enum class MutableFlag { Immutable, Mutable };
enum class PrivateFlag { Public, Private };
enum class OverrideFlag { Override, Fresh };

struct LabelDecl { StringLoc name; MutableFlag mut; CoreTypeBox type; Location loc; };
struct Pcstr_tuple { std::vector<CoreTypeBox> elems; };
struct Pcstr_record { std::vector<LabelDecl> fields; };
using ConstructorArguments = std::variant<Pcstr_tuple, Pcstr_record>;
struct ConstructorDecl {
  StringLoc name;
  ConstructorArguments args;
  std::optional<CoreTypeBox> res;
  Location loc;  // pcd_vars [] in fragment
};
struct Ptype_abstract {};
struct Ptype_variant { std::vector<ConstructorDecl> ctors; };
struct Ptype_record { std::vector<LabelDecl> fields; };
struct Ptype_open {};
using TypeKind = std::variant<Ptype_abstract, Ptype_variant, Ptype_record, Ptype_open>;
struct TypeDeclaration {
  StringLoc name;
  std::vector<CoreTypeBox> params;  // type_parameter (variance dropped)
  TypeKind kind;
  PrivateFlag priv = PrivateFlag::Public;
  std::optional<CoreTypeBox> manifest;
  Location loc;  // ptype_constraints [] in fragment
};

// --- module expressions (subset: open M) ---
struct Pmod_ident { LongidentLoc id; };
struct ModuleExpr { std::variant<Pmod_ident> desc; Location loc; };

// --- structure ---
struct Pstr_eval { ExprBox e; };       // attributes empty
struct Pstr_value { RecFlag rf; std::vector<ValueBinding> bindings; };
struct Pstr_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Pstr_open { OverrideFlag ovr; ModuleExpr expr; };
struct StructureItem {
  std::variant<Pstr_eval, Pstr_value, Pstr_type, Pstr_open> desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

}  // namespace cppcaml::ast
