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
struct Papply { PathBox fn; PathBox arg; };  // F(Arg) in a path
struct Path { std::variant<Pident, Pdot, Papply> v; };

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
struct Ttyp_class { Path path; std::vector<CoreTypeBox> args; };  // [args] #class
struct Ttyp_poly { std::vector<std::string> vars; CoreTypeBox type; };
struct Ttyp_alias { std::string name; CoreTypeBox type; };  // (t as 'name)
struct OTmethod { std::string name; CoreTypeBox type; };  // name -> poly-wrapped type
struct OTinherit { CoreTypeBox type; };                   // < t ; ... >
struct ObjField { std::variant<OTmethod, OTinherit> v; };
struct Ttyp_object {  // < m : t ; t ; ... >
  std::vector<ObjField> fields;
  bool closed = true;  // < .. > vs < .. ; .. >
};
struct Ttag { std::string name; bool constant; std::vector<CoreTypeBox> types; };
struct Tinherit { CoreTypeBox type; };  // `[ t | ... ]` polyvariant inheritance row
struct RowField { std::variant<Ttag, Tinherit> v; };
struct PackageType {  // (module S with type t1 = u1 and ...)
  Path path;
  std::vector<std::pair<std::string, CoreTypeBox>> constraints;
};
struct Ttyp_package { PackageType pkg; };
struct Ttyp_variant {  // [ `A | `B of t | t | ... ]  (Ttag + Tinherit rows)
  std::vector<RowField> rows;
  bool closed = true;  // Closed vs Open
  std::optional<std::vector<std::string>> labels;  // `[< .. > l]` present tags
};
struct CoreType {
  std::variant<Ttyp_any, Ttyp_var, Ttyp_arrow, Ttyp_tuple, Ttyp_constr, Ttyp_poly,
               Ttyp_alias, Ttyp_variant, Ttyp_object, Ttyp_package, Ttyp_class>
      desc;
  Location loc;
  const ast::Attributes* attrs = nullptr;  // ctyp_attributes ([@untagged] ...)
};

struct Pattern;
struct Expression;
struct StructureItem;  // for Texp_struct_item (let module/open/exception in e)
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
struct Tpat_record {  // { l1 = p1; ... } -- only the written fields, in order
  std::vector<std::pair<std::string, PatBox>> fields;  // label (last component), sub-pattern
};
struct Tpat_array { std::vector<PatBox> elems; };  // [| ... |] (always Mutable)
struct Tpat_lazy { PatBox inner; };                // lazy p
struct Tpat_variant { std::string label; PatBox arg; };  // `Tag [p]; null = no payload
struct PatExtra {  // Tpat_extra_constraint / Tpat_extra_unpack / Tpat_extra_type
  enum class Kind { Constraint, Unpack, Type } kind = Kind::Constraint;
  CoreType ctype;                  // Constraint
  std::optional<PackageType> pkg;  // Unpack: `(module M : S)`; nullopt = untyped
  Path type_path;                  // Type: the `#t` type path
  Location loc;
};
struct Pattern {
  std::variant<Tpat_any, Tpat_var, Tpat_constant, Tpat_construct, Tpat_value,
               Tpat_tuple, Tpat_exception, Tpat_or, Tpat_alias, Tpat_record,
               Tpat_array, Tpat_lazy, Tpat_variant>
      desc;
  Location loc;
  const ast::Attributes* attrs = nullptr;
  std::vector<PatExtra> extras;
};

// Module expressions are mutually recursive with structures and expressions,
// so ModuleExpr is defined last and referenced through a box.
struct ModuleExpr;
using ModuleExprBox = Box<ModuleExpr>;

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
// A single value parameter of a Texp_function: Param_pat, or
// Param_optional_default when a `?(x = e)` default expression is present.
struct FunctionParam {
  ArgLabel label;
  PatBox pat;
  ExprBox default_;      // non-null = Param_optional_default
  bool partial = false;  // fp_partial: a refutable param pattern
};
struct Texp_function {
  std::vector<FunctionParam> params;
  // Exactly one form is used: Tfunction_body (body set) when there are params,
  // or Tfunction_cases (is_cases) for a bare `function ... | ...`.
  bool is_cases = false;
  ExprBox body;                 // Tfunction_body
  Location cases_loc;           // Tfunction_cases
  const ast::Attributes* cases_attrs = nullptr;  // `function[@attr]`
  std::vector<struct ExprExtra> cases_extras;    // `function .. : t` return constraint
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
struct Texp_match {
  ExprBox scrut;
  std::vector<Case> cases;      // value/exception (computation) cases
  std::vector<Case> eff_cases;  // `effect P, k` cases: lhs is the effect
                                // pattern; k binds invisibly (dump omits it)
  bool partial = false;
};
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
struct Texp_setfield { ExprBox record; std::string name; ExprBox value; };  // r.l <- v
struct Texp_variant { std::string label; std::optional<ExprBox> arg; };  // `A [e]
struct Texp_instvar { Ident id; };  // an instance-variable reference inside a method
// meth_id present => Tmeth_val/Tmeth_ancestor (prints "meth/stamp"); absent =>
// Tmeth_name (prints just "meth").  A self-send resolves to the method's ident.
struct Texp_send { ExprBox obj; std::string meth; std::optional<Ident> meth_id; };  // e # m
struct Texp_new { Path path; };  // new M.c
struct ClassStructure;
struct Texp_object { Box<ClassStructure> cs; };  // object … end (an expression)
// `let module/open/exception … in e` (fork): an embedded structure item + body.
struct Texp_struct_item { Box<StructureItem> item; ExprBox body; };
struct Texp_pack { ModuleExprBox me; };  // (module ME [: S])
struct BindingOp {  // one `let+`/`and+` binding: the operator path + rhs
  Path path;
  Location loc;
  ExprBox exp;
};
struct Texp_letop {
  BindingOp let_;
  std::vector<BindingOp> ands;
  Box<struct Case> body;  // single case: joined pattern -> body
  bool partial = false;
};
struct Texp_unreachable {};  // `.` refutation case
struct ExprExtra {  // Texp_constraint / Texp_coerce / Texp_poly / Texp_newtype
  enum class Kind { Constraint, Coerce, Poly, Newtype } kind = Kind::Constraint;
  CoreType ctype;                 // constraint type, or coerce TARGET type
  std::optional<CoreType> from;   // coerce SOURCE type (`(e : t1 :> t2)`), else none
  bool poly_has_type = false;     // Poly: whether a method type annotation is present
  std::string newtype;            // Newtype: the abstract type's source name
  Location loc;
};
struct Expression {
  std::variant<Texp_constant, Texp_ident, Texp_tuple, Texp_apply, Texp_function,
               Texp_let, Texp_ifthenelse, Texp_sequence, Texp_match, Texp_try,
               Texp_construct, Texp_array, Texp_assert, Texp_for, Texp_lazy,
               Texp_while, Texp_record, Texp_field, Texp_setfield, Texp_variant,
               Texp_instvar, Texp_send, Texp_object, Texp_struct_item,
               Texp_pack, Texp_letop, Texp_unreachable, Texp_new>
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
  const ast::Attributes* attrs = nullptr;
};
struct ConstructorDecl {
  Location loc;
  Ident id;
  std::vector<CoreTypeBox> args;  // Cstr_tuple argument types
  std::vector<LabelDecl> labels;  // Cstr_record (inline record); wins over args
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

struct ModuleType;
using ModuleTypeBox2 = Box<ModuleType>;

// An extension constructor (Text_decl form): shared by exceptions and typexts.
struct ExtCtor {
  Location loc;
  Ident id;
  std::vector<CoreTypeBox> args;
  std::vector<LabelDecl> labels;  // Cstr_record (inline record); wins over args
  std::optional<CoreTypeBox> res;
  std::optional<Path> rebind;  // Text_rebind (`exception E = F`); wins over args
  const ast::Attributes* attrs = nullptr;
};

// module_declaration: name + declared module type (presence per printtyped).
struct ModuleDecl {
  bool present = true;  // Absent iff the declared type is an alias
  Ident id;
  ModuleTypeBox2 type;
  const ast::Attributes* attrs = nullptr;
};
struct Tsig_value { ValueDesc vd; };
struct Tsig_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Tsig_module { ModuleDecl md; };
struct Tsig_recmodule { std::vector<ModuleDecl> decls; };
struct Tsig_modtype {  // type null = #abstract
  Ident id;
  ModuleTypeBox2 type;
  const ast::Attributes* attrs = nullptr;
};
struct Tsig_include { ModuleTypeBox2 mt; const ast::Attributes* attrs = nullptr; };
struct Tsig_exception { ExtCtor ctor; const ast::Attributes* attrs = nullptr; };
struct Tsig_typext {
  Path path;
  std::vector<CoreTypeBox> params;
  std::vector<ExtCtor> ctors;
  bool private_ = false;
  const ast::Attributes* attrs = nullptr;
};
struct Tsig_primitive {
  Ident id;
  Location loc;
  std::optional<CoreType> type;  // absent for an untyped `external f = g`
  std::vector<std::string> prims;
  std::optional<Path> alias;     // Tprim_alias (`external f [: t] = g`)
  const ast::Attributes* attrs = nullptr;
};
struct Tsig_attribute { std::string name; const ast::Structure* payload; };
struct Tsig_open { bool override_ = false; Path path; };
struct ClassTypeDeclaration;  // defined with the class-type section below
struct Tsig_class { std::vector<Box<ClassTypeDeclaration>> decls; };      // class c : ct
struct Tsig_class_type { std::vector<Box<ClassTypeDeclaration>> decls; };  // class type c = ct
struct SignatureItem {
  std::variant<Tsig_value, Tsig_type, Tsig_module, Tsig_recmodule, Tsig_modtype,
               Tsig_include, Tsig_exception, Tsig_typext, Tsig_primitive,
               Tsig_attribute, Tsig_open, Tsig_class, Tsig_class_type>
      desc;
  Location loc;
};
struct Tmty_ident { Path path; };
struct Tmty_signature { std::vector<SignatureItem> items; };
struct Tmty_alias { Path path; };
// param==nullopt with a param_type -> anonymous "(_ : S)"; param_type==null ->
// generative "()" (printed `Tmty_functor ()`, no parameter module_type).
struct Tmty_functor {
  std::optional<Ident> param;
  ModuleTypeBox2 param_type;
  ModuleTypeBox2 body;
};
struct Twith_type { TypeDeclaration td; };
struct Twith_typesubst { TypeDeclaration td; };
struct Twith_module { Path path; };
struct Twith_modsubst { Path path; };
struct Twith_modtype { ModuleTypeBox2 mt; };
using WithConstraint = std::variant<Twith_type, Twith_typesubst, Twith_module,
                                    Twith_modsubst, Twith_modtype>;
struct WithItem { Path path; WithConstraint c; };  // the constrained item + rhs
struct Tmty_with { ModuleTypeBox2 base; std::vector<WithItem> constraints; };
struct Tmty_typeof { ModuleExprBox expr; };
struct ModuleType {
  std::variant<Tmty_ident, Tmty_signature, Tmty_alias, Tmty_functor, Tmty_with,
               Tmty_typeof>
      desc;
  Location loc;
};

// --- structure ---
struct Tstr_value {
  RecFlag rf;
  std::vector<ValueBinding> bindings;
};
struct Tstr_eval { ExprBox e; };
struct Tstr_open { bool override_ = false; ModuleExprBox expr; };
struct Tstr_module {
  bool present = true;
  Ident id;
  ModuleExprBox expr;
  const ast::Attributes* attrs = nullptr;
};
struct Tstr_type { RecFlag rf; std::vector<TypeDeclaration> decls; };
struct Tstr_primitive {
  Ident id;
  Location loc;
  std::optional<CoreType> type;  // absent for an untyped `external f = g`
  std::vector<std::string> prims;  // the "external" strings
  std::optional<Path> alias;     // Tprim_alias (`external f [: t] = g`)
  const ast::Attributes* attrs = nullptr;
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
struct Tstr_modtype {
  Ident id;
  ModuleTypeBox2 type;  // null = abstract
  const ast::Attributes* attrs = nullptr;
};
struct Tstr_include {  // include M
  ModuleExprBox expr;
  const ast::Attributes* attrs = nullptr;
};
struct RecmoduleBinding {
  Ident id;
  ModuleExprBox expr;
  const ast::Attributes* attrs = nullptr;
};
struct Tstr_recmodule {  // module rec A = .. and B = ..
  std::vector<RecmoduleBinding> bindings;
};
// --- classes (Tstr_class) ---
struct ClassExpr;
// A concrete field carries expr (+ override_); a virtual field carries vtype.
struct Tcf_val {
  std::string name; bool mutable_; bool virtual_ = false; bool override_ = false;
  ExprBox expr; std::optional<CoreType> vtype;
};
struct Tcf_method {
  std::string name; bool private_; bool virtual_ = false; bool override_ = false;
  ExprBox expr; std::optional<CoreType> vtype;
};
struct Tcf_inherit {  // inherit [!] ce [as super]
  bool override_;
  Box<ClassExpr> ce;
  std::optional<std::string> super;
};
struct Tcf_constraint { CoreType t1; CoreType t2; };  // constraint t1 = t2
struct Tcf_initializer { ExprBox expr; };  // initializer e (elaborated `fun self -> e`)
struct ClassField {
  std::variant<Tcf_val, Tcf_method, Tcf_inherit, Tcf_constraint, Tcf_initializer> desc;
  Location loc;
};
struct ClassStructure {
  Box<Pattern> self;                 // synthesized self (Tpat_alias selfpat-* / Tpat_any)
  std::vector<ClassField> fields;
};
struct Tcl_structure { ClassStructure cs; };
struct Tcl_fun {  // class c <pat> = ..  (a class parameter)
  ArgLabel label;
  PatBox pat;
  Box<ClassExpr> body;
};
struct Tcl_ident { Path path; std::vector<CoreTypeBox> args; };  // a class path (e.g. inherit b)
// ct present => explicit `(ce : CT)` constraint (prints label + ce + class_type);
// null => an inferred None-constraint (prints nothing for itself, recurses).
struct Tcl_constraint { Box<ClassExpr> ce; Box<struct ClassType> ct; };
struct Tcl_let {  // let [rec] bindings in ce  (the let-vars become instance vars)
  RecFlag rf;
  std::vector<ValueBinding> bindings;           // l1
  std::vector<std::pair<Ident, ExprBox>> ivars; // l2: instvar id <- Texp_ident of let var
  Box<ClassExpr> body;
};
struct Tcl_open { bool override_ = false; Path path; Box<ClassExpr> body; };  // let open M in ce
struct Tcl_apply {  // ce arg ..  (a class applied to value arguments)
  Box<ClassExpr> fn;
  std::vector<std::pair<ArgLabel, ExprBox>> args;  // null ExprBox = an omitted arg
};
struct ClassExpr {
  std::variant<Tcl_structure, Tcl_fun, Tcl_ident, Tcl_constraint, Tcl_let, Tcl_open,
               Tcl_apply>
      desc;
  Location loc;
};
struct ClassDeclaration {
  bool virt = false;
  std::string name;
  std::vector<CoreType> params;  // pci_params: the class' type parameters
  ClassExpr expr;
  Location loc;
};
struct Tstr_class { std::vector<ClassDeclaration> decls; };

// --- class types (Tstr_class_type / Tsig_class / Tsig_class_type) ---
struct ClassType;
struct Tctf_inherit { Box<ClassType> ct; };
struct Tctf_val { std::string name; bool mutable_; bool virtual_; CoreType type; };
// method type is Ttyp_poly-wrapped
struct Tctf_method { std::string name; bool private_; bool virtual_; CoreType type; };
struct Tctf_constraint { CoreType t1; CoreType t2; };
struct ClassTypeField {
  std::variant<Tctf_inherit, Tctf_val, Tctf_method, Tctf_constraint> desc;
  Location loc;
};
struct ClassSignature { CoreType self; std::vector<ClassTypeField> fields; };
struct Tcty_constr { Path path; std::vector<CoreTypeBox> args; };
struct Tcty_signature { ClassSignature cs; };
struct Tcty_arrow { ArgLabel label; CoreTypeBox dom; Box<ClassType> cod; };
struct ClassType {
  std::variant<Tcty_constr, Tcty_signature, Tcty_arrow> desc;
  Location loc;
};
struct ClassTypeDeclaration {
  bool virt = false;
  std::string name;
  std::vector<CoreType> params;
  ClassType expr;
  Location loc;
  const ast::Attributes* attrs = nullptr;  // printed only in class_description (Tsig_class)
};
struct Tstr_class_type { std::vector<ClassTypeDeclaration> decls; };
struct StructureItem {
  std::variant<Tstr_value, Tstr_eval, Tstr_type, Tstr_primitive, Tstr_exception,
               Tstr_open, Tstr_module, Tstr_attribute, Tstr_typext, Tstr_modtype,
               Tstr_include, Tstr_recmodule, Tstr_class, Tstr_class_type>
      desc;
  Location loc;
};
using Structure = std::vector<StructureItem>;

// --- module expressions (now that Structure is complete) ---
struct Tmod_ident { Path path; };
struct Tmod_structure { Structure items; };
struct Tmod_functor {
  std::optional<Ident> param;       // Named param (nullopt for `_`/unit handled later)
  ModuleTypeBox2 param_type;
  ModuleExprBox body;
};
struct Tmod_apply { ModuleExprBox fn; ModuleExprBox arg; };
// `implicit` = OCaml's Tmodtype_implicit (e.g. the strengthening coercion on a
// cmi-loaded functor in `Set.Make(..)`): printed as a transparent extra
// module_expr layer, with no "Tmod_constraint" label and no module_type.
struct Tmod_constraint { ModuleExprBox expr; ModuleTypeBox2 type; bool implicit = false; };
struct Tmod_unpack { ExprBox e; };       // (val e [: S])
struct Tmod_apply_unit { ModuleExprBox fn; };  // F ()
struct ModuleExpr {
  std::variant<Tmod_ident, Tmod_structure, Tmod_functor, Tmod_apply,
               Tmod_constraint, Tmod_unpack, Tmod_apply_unit>
      desc;
  Location loc;
};

// Render a structure in `ocamlc -dtypedtree` format.
void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles = {});

}  // namespace cppcaml::typedtree
