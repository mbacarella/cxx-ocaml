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

// Forward-declared early so attributes (whose payload is a structure) can be
// threaded through expression/pattern/type/binding nodes.
struct StructureItem;
using Structure = std::vector<StructureItem>;
struct ModuleExpr;  // (used by Pexp_pack, Pstr_include, …)
struct Attribute { std::string name; Structure payload; };  // [@name payload] (PStr)
using Attributes = std::vector<Attribute>;

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
struct StrOptLoc { std::optional<std::string> txt; Location loc; };  // names that may be `_`

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
struct Rtag { std::string name; bool constant; std::vector<CoreTypeBox> types; };
struct Rinherit { CoreTypeBox ct; };
using RowField = std::variant<Rtag, Rinherit>;
struct Ptyp_variant {
  std::vector<RowField> rows;
  ClosedFlag closed = ClosedFlag::Closed;
  std::optional<std::vector<std::string>> labels;  // `[< … > l]` present tags
};
struct CoreType {
  std::variant<Ptyp_any, Ptyp_var, Ptyp_arrow, Ptyp_tuple, Ptyp_constr, Ptyp_variant> desc;
  Location loc;
  Attributes attrs;
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
struct Ppat_record {
  std::vector<std::pair<LongidentLoc, PatBox>> fields;
  ClosedFlag closed = ClosedFlag::Closed;
};
struct Ppat_lazy { PatBox p; };
struct Ppat_interval { Constant c1; Constant c2; };
struct Ppat_variant { std::string label; std::optional<PatBox> arg; };
struct Ppat_exception { PatBox p; };
struct Ppat_array { std::vector<PatBox> elems; };
struct Ppat_type { LongidentLoc id; };                  // #tconst
struct Ppat_unpack { StrOptLoc name; };                 // (module M)  (package type deferred)
struct Ppat_extension { std::string name; Structure payload; };  // [%id]
struct Ppat_open { LongidentLoc mod_; PatBox p; };      // M.(P)
struct Pattern {
  std::variant<Ppat_any, Ppat_var, Ppat_constant, Ppat_tuple, Ppat_construct,
               Ppat_or, Ppat_alias, Ppat_constraint, Ppat_record, Ppat_lazy,
               Ppat_interval, Ppat_variant, Ppat_exception, Ppat_array,
               Ppat_type, Ppat_unpack, Ppat_extension, Ppat_open>
      desc;
  Location loc;
  Attributes attrs;
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
struct Pexp_record {
  std::vector<std::pair<LongidentLoc, ExprBox>> fields;
  std::optional<ExprBox> base;  // `{ e with ... }`
};
enum class DirectionFlag { Upto, Downto };
struct Pexp_assert { ExprBox e; };
struct Pexp_lazy { ExprBox e; };
struct Pexp_variant { std::string label; std::optional<ExprBox> arg; };
struct Pexp_newtype { StringLoc name; ExprBox body; };  // fun (type a) -> e
// fork-specific: `let open … in e`, `let module … in e`, `M.(e)` all lower to a
// structure item scoped over an expression.
struct Pexp_struct_item { Box<StructureItem> item; ExprBox body; };
struct Pexp_setfield { ExprBox obj; LongidentLoc field; ExprBox value; };  // e.l <- e2
struct Pexp_setinstvar { StringLoc name; ExprBox value; };  // x <- e  (in objects)
struct Pexp_coerce { ExprBox e; std::optional<CoreTypeBox> from; CoreTypeBox to_; };  // (e :> t)
struct Pexp_send { ExprBox obj; StringLoc meth; };  // e # m
struct Pexp_pack { Box<ModuleExpr> me; };           // (module ME)
struct Pexp_extension { std::string name; Structure payload; };  // [%id …]
struct BindingOp { StringLoc op; Pattern pat; ExprBox exp; Location loc; };
struct Pexp_letop { BindingOp let_; std::vector<BindingOp> ands; ExprBox body; };  // let* … in …
struct Pexp_while { ExprBox cond; ExprBox body; };
struct Pexp_for { Pattern var; ExprBox lo; ExprBox hi; DirectionFlag dir; ExprBox body; };
struct Pexp_array { std::vector<ExprBox> elems; };

struct Expression {
  std::variant<Pexp_ident, Pexp_constant, Pexp_apply, Pexp_let, Pexp_function,
               Pexp_tuple, Pexp_ifthenelse, Pexp_construct, Pexp_match, Pexp_try,
               Pexp_sequence, Pexp_constraint, Pexp_field, Pexp_record,
               Pexp_assert, Pexp_lazy, Pexp_while, Pexp_for, Pexp_array,
               Pexp_variant, Pexp_newtype, Pexp_struct_item, Pexp_setfield,
               Pexp_setinstvar, Pexp_coerce, Pexp_send, Pexp_pack,
               Pexp_extension, Pexp_letop>
      desc;
  Location loc;
  Attributes attrs;
};

struct Case { Pattern lhs; std::optional<ExprBox> guard; ExprBox rhs; };

struct ValueBinding {
  Pattern pat;
  ExprBox expr;
  std::optional<CoreTypeBox> constraint_;  // Pvc_constraint (simple `: t`)
  Attributes attrs;                        // pvb_attributes
};

struct Pparam_val { Location loc; ArgLabel label; std::optional<ExprBox> default_; Pattern pat; };
struct Pparam_newtype { StringLoc name; Location loc; };  // (type a)
struct FunctionParam { std::variant<Pparam_val, Pparam_newtype> desc; };

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

// --- extension constructors / exceptions ---
struct Pext_decl { ConstructorArguments args; std::optional<CoreTypeBox> res; };  // vars []
struct Pext_rebind { LongidentLoc id; };
struct ExtensionConstructor {
  StringLoc name;
  std::variant<Pext_decl, Pext_rebind> kind;
  Location loc;
};
struct TypeException { ExtensionConstructor ctor; };  // attributes empty
struct TypeExtension {
  LongidentLoc path;
  std::vector<CoreTypeBox> params;
  std::vector<ExtensionConstructor> ctors;
  PrivateFlag priv = PrivateFlag::Public;
};

// --- primitives (external) ---
struct PrimitiveDescription {
  StringLoc name;
  CoreTypeBox type;
  std::vector<std::string> prims;
  Location loc;
};

// --- module types / signatures ---
struct ValueDescription { StringLoc name; CoreTypeBox type; Location loc; };
struct SignatureItem;
using Signature = std::vector<SignatureItem>;
struct ModuleType;
using ModuleTypeBox = Box<ModuleType>;
struct Functor_unit {};
struct Functor_named { StrOptLoc name; ModuleTypeBox type; };
using FunctorParam = std::variant<Functor_unit, Functor_named>;
struct Pmty_ident { LongidentLoc id; };
struct Pmty_signature { Signature items; };
struct Pmty_functor { FunctorParam param; ModuleTypeBox body; };
struct ModuleType {
  std::variant<Pmty_ident, Pmty_signature, Pmty_functor> desc;
  Location loc;
  Attributes attrs;
};
struct Psig_value { ValueDescription vd; };
struct Psig_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct ModuleDeclaration { StrOptLoc name; ModuleTypeBox type; };
struct Psig_module { ModuleDeclaration md; };
struct SignatureItem {
  std::variant<Psig_value, Psig_type, Psig_module> desc;
  Location loc;
};

// --- module expressions ---
struct Pmod_ident { LongidentLoc id; };
struct Pmod_structure { Structure items; };
struct ModuleExpr;
struct Pmod_functor { FunctorParam param; Box<ModuleExpr> body; };
struct Pmod_constraint { Box<ModuleExpr> me; ModuleTypeBox mt; };
struct ModuleExpr {
  std::variant<Pmod_ident, Pmod_structure, Pmod_functor, Pmod_constraint> desc;
  Location loc;
};
struct ModuleBinding { StrOptLoc name; ModuleExpr expr; };

// --- structure items ---
struct Pstr_eval { ExprBox e; };       // attributes empty
struct Pstr_value { RecFlag rf; std::vector<ValueBinding> bindings; };
struct Pstr_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Pstr_open { OverrideFlag ovr; ModuleExpr expr; };
struct Pstr_exception { TypeException exn; };
struct Pstr_typext { TypeExtension ext; };
struct Pstr_primitive { PrimitiveDescription prim; };
struct Pstr_module { ModuleBinding binding; };
struct Pstr_attribute { std::string name; Structure payload; };  // [@@@attr …]
struct Pstr_extension { std::string name; Structure payload; };  // [%%ext …]
struct Pstr_include { ModuleExpr expr; };
struct Pstr_modtype { StringLoc name; std::optional<ModuleType> type; };  // module type S = mty
struct StructureItem {
  std::variant<Pstr_eval, Pstr_value, Pstr_type, Pstr_open, Pstr_exception,
               Pstr_typext, Pstr_primitive, Pstr_module, Pstr_attribute,
               Pstr_extension, Pstr_include, Pstr_modtype>
      desc;
  Location loc;
};

}  // namespace cppcaml::ast
