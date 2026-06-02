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
struct Longident;
using LongidentBox = Box<Longident>;
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

// --- patterns (fragment) ---
struct Pattern;
using PatBox = Box<Pattern>;
struct Ppat_any {};
struct Ppat_var { StringLoc name; };
struct Ppat_constant { Constant c; };
struct Pattern {
  std::variant<Ppat_any, Ppat_var, Ppat_constant> desc;
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

struct Expression {
  std::variant<Pexp_ident, Pexp_constant, Pexp_apply, Pexp_let, Pexp_function,
               Pexp_tuple, Pexp_ifthenelse>
      desc;
  Location loc;
};

struct ValueBinding {
  Pattern pat;
  ExprBox expr;
  // pvb_constraint omitted (None); attributes empty
};

struct Pparam_val { Location loc; ArgLabel label; Pattern pat; };  // default omitted (None)
struct FunctionParam { std::variant<Pparam_val> desc; };

struct Pfunction_body { ExprBox e; };
struct FunctionBody { std::variant<Pfunction_body> v; };

// --- structure ---
struct Pstr_eval { ExprBox e; };       // attributes empty
struct Pstr_value { RecFlag rf; std::vector<ValueBinding> bindings; };
struct StructureItem {
  std::variant<Pstr_eval, Pstr_value> desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

}  // namespace cppcaml::ast
