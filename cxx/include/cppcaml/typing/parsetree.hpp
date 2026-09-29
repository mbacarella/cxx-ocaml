// Port of parsing/parsetree.mli (cxx/PORTING.md, stage 4): the abstract
// syntax tree the typer consumes, field for field.  Records are structs with
// the OCaml field names; each variant is a `*Desc` base with one subclass
// per constructor (`as<Pexp_apply>(e->pexp_desc)`), as in types.hpp.  Lists
// are Slices, options nullable pointers (or OptStr), everything lives in
// the current zone.  The tree is built from the C++ parser's ast:: by
// `parsetree::of_ast` (parsetree_of_ast.cpp).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/longident.hpp"
#include "cppcaml/typing/support.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::parsetree {

// ---- Asttypes pieces not in support.hpp ----------------------------------------------
enum class DirectionFlag : std::uint8_t { Upto, Downto };
enum class Variance : std::uint8_t { Covariant, Contravariant, NoVariance, Bivariant };
enum class Injectivity : std::uint8_t { Injective, NoInjectivity };

struct StrLoc {  // string loc
  std::string_view txt;
  Location loc;
};
struct OptStrLoc {  // string option loc
  OptStr txt;
  Location loc;
};
struct LidLoc {  // Longident.t loc
  Longident::t txt;
  Location loc;
  // the record's identity: the parser's record, which the typed tree keeps
  // (Typetexp's Ttyp_constr lid, ...) -- a let constraint's type is typed
  // twice, the two core types sharing the parsetree's lid (the occurrence
  // index shows it); nullptr for a record built after parsing
  const void* obj = nullptr;
};

template <class D>
const D* as(const typename D::Base* d) {
  return d->kind == D::K ? static_cast<const D*>(d) : nullptr;
}

struct CoreType;
struct Pattern;
struct Expression;
struct StructureItem;
struct SignatureItem;
struct ClassStructure;
struct ModuleExpr;
struct ModuleType;
struct ValueBinding;
struct Case;
using Structure = Slice<const StructureItem*>;
using Signature = Slice<const SignatureItem*>;

// ---- constants ---------------------------------------------------------------------
struct ConstantDesc {
  enum class Kind : std::uint8_t { Pconst_integer, Pconst_char, Pconst_string, Pconst_float };
  Kind kind;
  std::string_view s;           // integer / float literal text; string contents
  char suffix = 0;              // integer / float: char option ('\0' = None)
  bool has_suffix = false;
  char c = 0;                   // Pconst_char
  Location str_loc;             // Pconst_string
  OptStr delim;                 // Pconst_string
};
struct Constant {
  ConstantDesc pconst_desc;
  Location pconst_loc;
};

using LocationStack = Slice<Location>;

// ---- extension points ----------------------------------------------------------------
struct Payload {
  enum class Kind : std::uint8_t { PStr, PSig, PTyp, PPat };
  Kind kind;
  Structure str;                     // PStr
  Signature sig;                     // PSig
  const CoreType* typ = nullptr;     // PTyp
  const Pattern* pat = nullptr;      // PPat
  const Expression* guard = nullptr; // PPat: expression option
};
struct Attribute {
  StrLoc attr_name;
  Payload attr_payload;
  Location attr_loc;
};
using Attributes = Slice<const Attribute*>;
struct Extension {  // string loc * payload
  StrLoc name;
  Payload payload;
};

// ---- type expressions --------------------------------------------------------------------
struct PackageType {
  LidLoc ppt_path;
  Slice<std::pair<LidLoc, const CoreType*>> ppt_constraints;
  Location ppt_loc;
  Attributes ppt_attrs;
};

struct CoreTypeDesc {
  enum class Kind : std::uint8_t {
    Ptyp_any, Ptyp_var, Ptyp_arrow, Ptyp_tuple, Ptyp_constr, Ptyp_object, Ptyp_class,
    Ptyp_alias, Ptyp_variant, Ptyp_poly, Ptyp_package, Ptyp_open, Ptyp_extension, Ptyp_functor
  };
  Kind kind;
};
struct CoreType {
  const CoreTypeDesc* ptyp_desc;
  LocPtr ptyp_loc;
  LocationStack ptyp_loc_stack;
  Attributes ptyp_attributes;
};
struct LabeledCoreType {
  OptStr label;
  const CoreType* ty;
};
struct RowFieldDesc {
  enum class Kind : std::uint8_t { Rtag, Rinherit };
  Kind kind;
};
struct RowField {
  const RowFieldDesc* prf_desc;
  Location prf_loc;
  Attributes prf_attributes;
};
struct Rtag : RowFieldDesc {
  using Base = RowFieldDesc;
  static constexpr Kind K = Kind::Rtag;
  StrLoc label;
  bool constant;
  Slice<const CoreType*> types;
};
struct Rinherit : RowFieldDesc {
  using Base = RowFieldDesc;
  static constexpr Kind K = Kind::Rinherit;
  const CoreType* ty;
};
struct ObjectFieldDesc {
  enum class Kind : std::uint8_t { Otag, Oinherit };
  Kind kind;
};
struct ObjectField {
  const ObjectFieldDesc* pof_desc;
  Location pof_loc;
  Attributes pof_attributes;
};
struct Otag : ObjectFieldDesc {
  using Base = ObjectFieldDesc;
  static constexpr Kind K = Kind::Otag;
  StrLoc label;
  const CoreType* ty;
};
struct Oinherit : ObjectFieldDesc {
  using Base = ObjectFieldDesc;
  static constexpr Kind K = Kind::Oinherit;
  const CoreType* ty;
};

#define PT_CTOR(Base_, Name)  \
  struct Name : Base_ {       \
    using Base = Base_;       \
    static constexpr Kind K = Kind::Name;
#define PT_END };

PT_CTOR(CoreTypeDesc, Ptyp_any) PT_END
PT_CTOR(CoreTypeDesc, Ptyp_var) std::string_view name; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_arrow) ArgLabel label; const CoreType* t1; const CoreType* t2; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_tuple) Slice<LabeledCoreType> tl; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_constr) LidLoc lid; Slice<const CoreType*> args; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_object) Slice<const ObjectField*> fields; ClosedFlag closed; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_class) LidLoc lid; Slice<const CoreType*> args; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_alias) const CoreType* ty; StrLoc name; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_variant)
  Slice<const RowField*> fields;
  ClosedFlag closed;
  bool has_labels;                  // label list option
  Slice<std::string_view> labels;
PT_END
PT_CTOR(CoreTypeDesc, Ptyp_poly) Slice<StrLoc> vars; const CoreType* ty; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_package) const PackageType* pack; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_open) LidLoc lid; const CoreType* ty; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_extension) const Extension* ext; PT_END
PT_CTOR(CoreTypeDesc, Ptyp_functor)
  ArgLabel label;
  StrLoc name;
  const PackageType* pack;
  const CoreType* ty;
PT_END

// ---- patterns ---------------------------------------------------------------------------
struct PatternDesc {
  enum class Kind : std::uint8_t {
    Ppat_any, Ppat_var, Ppat_alias, Ppat_constant, Ppat_interval, Ppat_tuple,
    Ppat_construct, Ppat_variant, Ppat_record, Ppat_array, Ppat_or, Ppat_constraint,
    Ppat_type, Ppat_lazy, Ppat_unpack, Ppat_exception, Ppat_effect, Ppat_extension, Ppat_open
  };
  Kind kind;
};
struct Pattern {
  const PatternDesc* ppat_desc;
  LocPtr ppat_loc;
  LocationStack ppat_loc_stack;
  Attributes ppat_attributes;
};
struct LabeledPattern {
  OptStr label;
  const Pattern* pat;
};
struct ConstructArg {  // (string loc list * pattern)
  Slice<StrLoc> vars;
  const Pattern* pat;
};
PT_CTOR(PatternDesc, Ppat_any) PT_END
PT_CTOR(PatternDesc, Ppat_var) StrLoc name; PT_END
PT_CTOR(PatternDesc, Ppat_alias) const Pattern* pat; StrLoc name; PT_END
PT_CTOR(PatternDesc, Ppat_constant) Constant c; PT_END
PT_CTOR(PatternDesc, Ppat_interval) Constant c1; Constant c2; PT_END
PT_CTOR(PatternDesc, Ppat_tuple) Slice<LabeledPattern> pl; ClosedFlag closed; PT_END
PT_CTOR(PatternDesc, Ppat_construct) LidLoc lid; const ConstructArg* arg; PT_END
PT_CTOR(PatternDesc, Ppat_variant) std::string_view label; const Pattern* arg; PT_END
PT_CTOR(PatternDesc, Ppat_record)
  Slice<std::pair<LidLoc, const Pattern*>> fields;
  ClosedFlag closed;
PT_END
PT_CTOR(PatternDesc, Ppat_array) Slice<const Pattern*> pats; PT_END
PT_CTOR(PatternDesc, Ppat_or) const Pattern* p1; const Pattern* p2; PT_END
PT_CTOR(PatternDesc, Ppat_constraint) const Pattern* pat; const CoreType* ty; PT_END
PT_CTOR(PatternDesc, Ppat_type) LidLoc lid; PT_END
PT_CTOR(PatternDesc, Ppat_lazy) const Pattern* pat; PT_END
PT_CTOR(PatternDesc, Ppat_unpack) OptStrLoc name; const PackageType* pack; PT_END
PT_CTOR(PatternDesc, Ppat_exception) const Pattern* pat; PT_END
PT_CTOR(PatternDesc, Ppat_effect) const Pattern* eff; const Pattern* cont; PT_END
PT_CTOR(PatternDesc, Ppat_extension) const Extension* ext; PT_END
PT_CTOR(PatternDesc, Ppat_open) LidLoc lid; const Pattern* pat; PT_END

// ---- expressions ------------------------------------------------------------------------
struct ExpressionDesc {
  enum class Kind : std::uint8_t {
    Pexp_ident, Pexp_constant, Pexp_let, Pexp_function, Pexp_apply, Pexp_match, Pexp_try,
    Pexp_tuple, Pexp_construct, Pexp_variant, Pexp_record, Pexp_field, Pexp_setfield,
    Pexp_array, Pexp_ifthenelse, Pexp_sequence, Pexp_while, Pexp_for, Pexp_constraint,
    Pexp_coerce, Pexp_send, Pexp_new, Pexp_setinstvar, Pexp_override, Pexp_struct_item,
    Pexp_assert, Pexp_lazy, Pexp_poly, Pexp_object, Pexp_newtype, Pexp_pack, Pexp_letop,
    Pexp_extension, Pexp_unreachable, Pexp_hole
  };
  Kind kind;
};
struct Expression {
  const ExpressionDesc* pexp_desc;
  LocPtr pexp_loc;
  LocationStack pexp_loc_stack;
  Attributes pexp_attributes;
};
struct Case {
  const Pattern* pc_lhs;
  const Expression* pc_guard;  // option
  const Expression* pc_rhs;
};
struct BindingOp {
  StrLoc pbop_op;
  const Pattern* pbop_pat;
  const Expression* pbop_exp;
  Location pbop_loc;
};
struct Letop {
  const BindingOp* let_;
  Slice<const BindingOp*> ands;
  const Expression* body;
};
struct FunctionParamDesc {
  enum class Kind : std::uint8_t { Pparam_val, Pparam_newtype };
  Kind kind;
  ArgLabel label;                     // Pparam_val
  const Expression* default_ = nullptr;  // Pparam_val: expression option
  const Pattern* pat = nullptr;       // Pparam_val
  StrLoc newtype;                     // Pparam_newtype
};
struct FunctionParam {
  Location pparam_loc;
  FunctionParamDesc pparam_desc;
};
struct FunctionBody {
  enum class Kind : std::uint8_t { Pfunction_body, Pfunction_cases };
  Kind kind;
  const Expression* body = nullptr;  // Pfunction_body
  Slice<const Case*> cases;          // Pfunction_cases
  Location loc;
  Attributes attrs;
};
struct TypeConstraint {
  enum class Kind : std::uint8_t { Pconstraint, Pcoerce };
  Kind kind;
  const CoreType* ty = nullptr;      // Pconstraint / Pcoerce target
  const CoreType* from = nullptr;    // Pcoerce: core_type option
};
struct LabeledExpression {
  OptStr label;
  const Expression* exp;
};
struct ArgExpression {  // arg_label * expression
  ArgLabel label;
  const Expression* exp;
};

PT_CTOR(ExpressionDesc, Pexp_ident) LidLoc lid; PT_END
PT_CTOR(ExpressionDesc, Pexp_constant) Constant c; PT_END
PT_CTOR(ExpressionDesc, Pexp_let)
  RecFlag rec;
  Slice<const ValueBinding*> vbs;
  const Expression* body;
PT_END
PT_CTOR(ExpressionDesc, Pexp_function)
  Slice<const FunctionParam*> params;
  const TypeConstraint* constraint;  // option
  const FunctionBody* body;
PT_END
PT_CTOR(ExpressionDesc, Pexp_apply) const Expression* fn; Slice<ArgExpression> args; PT_END
PT_CTOR(ExpressionDesc, Pexp_match) const Expression* exp; Slice<const Case*> cases; PT_END
PT_CTOR(ExpressionDesc, Pexp_try) const Expression* exp; Slice<const Case*> cases; PT_END
PT_CTOR(ExpressionDesc, Pexp_tuple) Slice<LabeledExpression> el; PT_END
PT_CTOR(ExpressionDesc, Pexp_construct) LidLoc lid; const Expression* arg; PT_END
PT_CTOR(ExpressionDesc, Pexp_variant) std::string_view label; const Expression* arg; PT_END
PT_CTOR(ExpressionDesc, Pexp_record)
  Slice<std::pair<LidLoc, const Expression*>> fields;
  const Expression* base;  // option
PT_END
PT_CTOR(ExpressionDesc, Pexp_field) const Expression* exp; LidLoc lid; PT_END
PT_CTOR(ExpressionDesc, Pexp_setfield)
  const Expression* exp;
  LidLoc lid;
  const Expression* value;
PT_END
PT_CTOR(ExpressionDesc, Pexp_array) Slice<const Expression*> el; PT_END
PT_CTOR(ExpressionDesc, Pexp_ifthenelse)
  const Expression* cond;
  const Expression* then_;
  const Expression* else_;  // option
PT_END
PT_CTOR(ExpressionDesc, Pexp_sequence) const Expression* e1; const Expression* e2; PT_END
PT_CTOR(ExpressionDesc, Pexp_while) const Expression* cond; const Expression* body; PT_END
PT_CTOR(ExpressionDesc, Pexp_for)
  const Pattern* pat;
  const Expression* lo;
  const Expression* hi;
  DirectionFlag dir;
  const Expression* body;
PT_END
PT_CTOR(ExpressionDesc, Pexp_constraint) const Expression* exp; const CoreType* ty; PT_END
PT_CTOR(ExpressionDesc, Pexp_coerce)
  const Expression* exp;
  const CoreType* from;  // option
  const CoreType* to;
PT_END
PT_CTOR(ExpressionDesc, Pexp_send) const Expression* exp; StrLoc meth; PT_END
PT_CTOR(ExpressionDesc, Pexp_new) LidLoc lid; PT_END
PT_CTOR(ExpressionDesc, Pexp_setinstvar) StrLoc name; const Expression* value; PT_END
PT_CTOR(ExpressionDesc, Pexp_override) Slice<std::pair<StrLoc, const Expression*>> fields; PT_END
PT_CTOR(ExpressionDesc, Pexp_struct_item) const StructureItem* item; const Expression* body; PT_END
PT_CTOR(ExpressionDesc, Pexp_assert) const Expression* exp; PT_END
PT_CTOR(ExpressionDesc, Pexp_lazy) const Expression* exp; PT_END
PT_CTOR(ExpressionDesc, Pexp_poly) const Expression* exp; const CoreType* ty; PT_END
PT_CTOR(ExpressionDesc, Pexp_object) const ClassStructure* cs; PT_END
PT_CTOR(ExpressionDesc, Pexp_newtype) StrLoc name; const Expression* body; PT_END
PT_CTOR(ExpressionDesc, Pexp_pack) const ModuleExpr* me; const PackageType* pack; PT_END
PT_CTOR(ExpressionDesc, Pexp_letop) const Letop* letop; PT_END
PT_CTOR(ExpressionDesc, Pexp_extension) const Extension* ext; PT_END
PT_CTOR(ExpressionDesc, Pexp_unreachable) PT_END
PT_CTOR(ExpressionDesc, Pexp_hole) PT_END

// ---- value / primitive descriptions -------------------------------------------------------
struct ValueDescription {
  StrLoc pval_name;
  const CoreType* pval_type;
  Attributes pval_attributes;
  Location pval_loc;
};
struct PrimitiveKind {
  enum class Kind : std::uint8_t { Pprim_decl, Pprim_alias };
  Kind kind;
  const CoreType* ty;               // Pprim_decl; Pprim_alias: option
  Slice<std::string_view> prims;    // Pprim_decl
  LidLoc alias;                     // Pprim_alias
};
struct PrimitiveDescription {
  StrLoc pprim_name;
  PrimitiveKind pprim_kind;
  Attributes pprim_attributes;
  Location pprim_loc;
};

// ---- type declarations -------------------------------------------------------------------
struct TypeParam {  // core_type * (variance * injectivity)
  const CoreType* ty;
  Variance variance;
  Injectivity injectivity;
};
struct TypeConstraintDecl {  // core_type * core_type * Location.t
  const CoreType* t1;
  const CoreType* t2;
  Location loc;
};
struct LabelDeclaration {
  StrLoc pld_name;
  MutableFlag pld_mutable;
  const CoreType* pld_type;
  Location pld_loc;
  Attributes pld_attributes;
};
struct ConstructorArguments {
  enum class Kind : std::uint8_t { Pcstr_tuple, Pcstr_record };
  Kind kind;
  Slice<const CoreType*> tuple;
  Slice<const LabelDeclaration*> record;
};
struct ConstructorDeclaration {
  StrLoc pcd_name;
  Slice<StrLoc> pcd_vars;
  ConstructorArguments pcd_args;
  const CoreType* pcd_res;  // option
  Location pcd_loc;
  Attributes pcd_attributes;
};
struct TypeKind {
  enum class Kind : std::uint8_t {
    Ptype_abstract, Ptype_variant, Ptype_record, Ptype_open, Ptype_external
  };
  Kind kind;
  Slice<const ConstructorDeclaration*> constructors;  // Ptype_variant
  Slice<const LabelDeclaration*> labels;              // Ptype_record
  std::string_view external;                          // Ptype_external
};
struct TypeDeclaration {
  StrLoc ptype_name;
  Slice<TypeParam> ptype_params;
  Slice<TypeConstraintDecl> ptype_constraints;
  TypeKind ptype_kind;
  PrivateFlag ptype_private;
  const CoreType* ptype_manifest;  // option
  Attributes ptype_attributes;
  Location ptype_loc;
};
struct ExtensionConstructorKind {
  enum class Kind : std::uint8_t { Pext_decl, Pext_rebind };
  Kind kind;
  Slice<StrLoc> vars;             // Pext_decl
  ConstructorArguments args;      // Pext_decl
  const CoreType* res = nullptr;  // Pext_decl: option
  LidLoc rebind;                  // Pext_rebind
};
struct ExtensionConstructor {
  StrLoc pext_name;
  ExtensionConstructorKind pext_kind;
  Location pext_loc;
  Attributes pext_attributes;
};
struct TypeExtension {
  LidLoc ptyext_path;
  Slice<TypeParam> ptyext_params;
  Slice<const ExtensionConstructor*> ptyext_constructors;
  PrivateFlag ptyext_private;
  Location ptyext_loc;
  Attributes ptyext_attributes;
};
struct TypeException {
  const ExtensionConstructor* ptyexn_constructor;
  Location ptyexn_loc;
  Attributes ptyexn_attributes;
};

// ---- class language ------------------------------------------------------------------------
struct ClassType;
struct ClassExpr;
template <class A>
struct OpenInfos {
  A popen_expr;
  OverrideFlag popen_override;
  Location popen_loc;
  Attributes popen_attributes;
};
using OpenDescription = OpenInfos<LidLoc>;
using OpenDeclaration = OpenInfos<const ModuleExpr*>;
template <class A>
struct IncludeInfos {
  A pincl_mod;
  Location pincl_loc;
  Attributes pincl_attributes;
};
using IncludeDescription = IncludeInfos<const ModuleType*>;
using IncludeDeclaration = IncludeInfos<const ModuleExpr*>;

struct ClassTypeDesc {
  enum class Kind : std::uint8_t {
    Pcty_constr, Pcty_signature, Pcty_arrow, Pcty_extension, Pcty_open
  };
  Kind kind;
};
struct ClassType {
  const ClassTypeDesc* pcty_desc;
  Location pcty_loc;
  Attributes pcty_attributes;
};
struct ClassTypeField;
struct ClassSignature {
  const CoreType* pcsig_self;
  Slice<const ClassTypeField*> pcsig_fields;
};
PT_CTOR(ClassTypeDesc, Pcty_constr) LidLoc lid; Slice<const CoreType*> args; PT_END
PT_CTOR(ClassTypeDesc, Pcty_signature) const ClassSignature* sign; PT_END
PT_CTOR(ClassTypeDesc, Pcty_arrow) ArgLabel label; const CoreType* ty; const ClassType* cty; PT_END
PT_CTOR(ClassTypeDesc, Pcty_extension) const Extension* ext; PT_END
PT_CTOR(ClassTypeDesc, Pcty_open) const OpenDescription* od; const ClassType* cty; PT_END

struct ClassTypeFieldDesc {
  enum class Kind : std::uint8_t {
    Pctf_inherit, Pctf_val, Pctf_method, Pctf_constraint, Pctf_attribute, Pctf_extension
  };
  Kind kind;
};
struct ClassTypeField {
  const ClassTypeFieldDesc* pctf_desc;
  Location pctf_loc;
  Attributes pctf_attributes;
};
PT_CTOR(ClassTypeFieldDesc, Pctf_inherit) const ClassType* cty; PT_END
PT_CTOR(ClassTypeFieldDesc, Pctf_val)
  StrLoc label;
  MutableFlag mut;
  VirtualFlag virt;
  const CoreType* ty;
PT_END
PT_CTOR(ClassTypeFieldDesc, Pctf_method)
  StrLoc label;
  PrivateFlag priv;
  VirtualFlag virt;
  const CoreType* ty;
PT_END
PT_CTOR(ClassTypeFieldDesc, Pctf_constraint) const CoreType* t1; const CoreType* t2; PT_END
PT_CTOR(ClassTypeFieldDesc, Pctf_attribute) const Attribute* attr; PT_END
PT_CTOR(ClassTypeFieldDesc, Pctf_extension) const Extension* ext; PT_END

template <class A>
struct ClassInfos {
  VirtualFlag pci_virt;
  Slice<TypeParam> pci_params;
  StrLoc pci_name;
  A pci_expr;
  Location pci_loc;
  Attributes pci_attributes;
};
using ClassDescription = ClassInfos<const ClassType*>;
using ClassTypeDeclaration = ClassInfos<const ClassType*>;
using ClassDeclaration = ClassInfos<const ClassExpr*>;

struct ClassExprDesc {
  enum class Kind : std::uint8_t {
    Pcl_constr, Pcl_structure, Pcl_fun, Pcl_apply, Pcl_let, Pcl_constraint, Pcl_extension,
    Pcl_open
  };
  Kind kind;
};
struct ClassExpr {
  const ClassExprDesc* pcl_desc;
  Location pcl_loc;
  Attributes pcl_attributes;
};
struct ClassField;
struct ClassStructure {
  const Pattern* pcstr_self;
  Slice<const ClassField*> pcstr_fields;
};
PT_CTOR(ClassExprDesc, Pcl_constr) LidLoc lid; Slice<const CoreType*> args; PT_END
PT_CTOR(ClassExprDesc, Pcl_structure) const ClassStructure* cs; PT_END
PT_CTOR(ClassExprDesc, Pcl_fun)
  ArgLabel label;
  const Expression* default_;  // option
  const Pattern* pat;
  const ClassExpr* body;
PT_END
PT_CTOR(ClassExprDesc, Pcl_apply) const ClassExpr* ce; Slice<ArgExpression> args; PT_END
PT_CTOR(ClassExprDesc, Pcl_let)
  RecFlag rec;
  Slice<const ValueBinding*> vbs;
  const ClassExpr* body;
PT_END
PT_CTOR(ClassExprDesc, Pcl_constraint) const ClassExpr* ce; const ClassType* cty; PT_END
PT_CTOR(ClassExprDesc, Pcl_extension) const Extension* ext; PT_END
PT_CTOR(ClassExprDesc, Pcl_open) const OpenDescription* od; const ClassExpr* ce; PT_END

struct ClassFieldKind {
  enum class Kind : std::uint8_t { Cfk_virtual, Cfk_concrete };
  Kind kind;
  const CoreType* ty = nullptr;          // Cfk_virtual
  OverrideFlag ovr = OverrideFlag::Fresh; // Cfk_concrete
  const Expression* exp = nullptr;       // Cfk_concrete
};
struct ClassFieldDesc {
  enum class Kind : std::uint8_t {
    Pcf_inherit, Pcf_val, Pcf_method, Pcf_constraint, Pcf_initializer, Pcf_attribute,
    Pcf_extension
  };
  Kind kind;
};
struct ClassField {
  const ClassFieldDesc* pcf_desc;
  Location pcf_loc;
  Attributes pcf_attributes;
};
PT_CTOR(ClassFieldDesc, Pcf_inherit)
  OverrideFlag ovr;
  const ClassExpr* ce;
  const StrLoc* as;  // option
PT_END
PT_CTOR(ClassFieldDesc, Pcf_val) StrLoc label; MutableFlag mut; ClassFieldKind kind_; PT_END
PT_CTOR(ClassFieldDesc, Pcf_method) StrLoc label; PrivateFlag priv; ClassFieldKind kind_; PT_END
PT_CTOR(ClassFieldDesc, Pcf_constraint) const CoreType* t1; const CoreType* t2; PT_END
PT_CTOR(ClassFieldDesc, Pcf_initializer) const Expression* exp; PT_END
PT_CTOR(ClassFieldDesc, Pcf_attribute) const Attribute* attr; PT_END
PT_CTOR(ClassFieldDesc, Pcf_extension) const Extension* ext; PT_END

// ---- module language ----------------------------------------------------------------------
struct FunctorParameter {  // Unit | Named of string option loc * module_type
  bool is_unit;
  OptStrLoc name;
  const ModuleType* mty = nullptr;
};
struct ModuleTypeDesc {
  enum class Kind : std::uint8_t {
    Pmty_ident, Pmty_signature, Pmty_functor, Pmty_with, Pmty_typeof, Pmty_extension,
    Pmty_alias
  };
  Kind kind;
};
struct ModuleType {
  const ModuleTypeDesc* pmty_desc;
  Location pmty_loc;
  Attributes pmty_attributes;
};
struct WithConstraint {
  enum class Kind : std::uint8_t {
    Pwith_type, Pwith_module, Pwith_modtype, Pwith_modtypesubst, Pwith_typesubst,
    Pwith_modsubst
  };
  Kind kind;
  LidLoc lid;
  const TypeDeclaration* decl = nullptr;  // Pwith_type / Pwith_typesubst
  LidLoc lid2;                            // Pwith_module / Pwith_modsubst
  const ModuleType* mty = nullptr;        // Pwith_modtype / Pwith_modtypesubst
};
PT_CTOR(ModuleTypeDesc, Pmty_ident) LidLoc lid; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_signature) Signature sg; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_functor) FunctorParameter param; const ModuleType* body; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_with) const ModuleType* mty; Slice<const WithConstraint*> cstrs; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_typeof) const ModuleExpr* me; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_extension) const Extension* ext; PT_END
PT_CTOR(ModuleTypeDesc, Pmty_alias) LidLoc lid; PT_END

struct ModuleDeclaration {
  OptStrLoc pmd_name;
  const ModuleType* pmd_type;
  Attributes pmd_attributes;
  Location pmd_loc;
};
struct ModuleSubstitution {
  StrLoc pms_name;
  LidLoc pms_manifest;
  Attributes pms_attributes;
  Location pms_loc;
};
struct ModuleTypeDeclaration {
  StrLoc pmtd_name;
  const ModuleType* pmtd_type;  // option
  Attributes pmtd_attributes;
  Location pmtd_loc;
};

struct SignatureItemDesc {
  enum class Kind : std::uint8_t {
    Psig_value, Psig_primitive, Psig_type, Psig_typesubst, Psig_typext, Psig_exception,
    Psig_module, Psig_modsubst, Psig_recmodule, Psig_modtype, Psig_modtypesubst, Psig_open,
    Psig_include, Psig_class, Psig_class_type, Psig_attribute, Psig_extension
  };
  Kind kind;
};
struct SignatureItem {
  const SignatureItemDesc* psig_desc;
  Location psig_loc;
};
PT_CTOR(SignatureItemDesc, Psig_value) const ValueDescription* vd; PT_END
PT_CTOR(SignatureItemDesc, Psig_primitive) const PrimitiveDescription* pd; PT_END
PT_CTOR(SignatureItemDesc, Psig_type) RecFlag rec; Slice<const TypeDeclaration*> decls; PT_END
PT_CTOR(SignatureItemDesc, Psig_typesubst) Slice<const TypeDeclaration*> decls; PT_END
PT_CTOR(SignatureItemDesc, Psig_typext) const TypeExtension* ext; PT_END
PT_CTOR(SignatureItemDesc, Psig_exception) const TypeException* exn; PT_END
PT_CTOR(SignatureItemDesc, Psig_module) const ModuleDeclaration* md; PT_END
PT_CTOR(SignatureItemDesc, Psig_modsubst) const ModuleSubstitution* ms; PT_END
PT_CTOR(SignatureItemDesc, Psig_recmodule) Slice<const ModuleDeclaration*> mds; PT_END
PT_CTOR(SignatureItemDesc, Psig_modtype) const ModuleTypeDeclaration* mtd; PT_END
PT_CTOR(SignatureItemDesc, Psig_modtypesubst) const ModuleTypeDeclaration* mtd; PT_END
PT_CTOR(SignatureItemDesc, Psig_open) const OpenDescription* od; PT_END
PT_CTOR(SignatureItemDesc, Psig_include) const IncludeDescription* incl; PT_END
PT_CTOR(SignatureItemDesc, Psig_class) Slice<const ClassDescription*> decls; PT_END
PT_CTOR(SignatureItemDesc, Psig_class_type) Slice<const ClassTypeDeclaration*> decls; PT_END
PT_CTOR(SignatureItemDesc, Psig_attribute) const Attribute* attr; PT_END
PT_CTOR(SignatureItemDesc, Psig_extension) const Extension* ext; Attributes attrs; PT_END

struct ModuleExprDesc {
  enum class Kind : std::uint8_t {
    Pmod_ident, Pmod_structure, Pmod_functor, Pmod_apply, Pmod_apply_unit, Pmod_constraint,
    Pmod_unpack, Pmod_extension, Pmod_hole
  };
  Kind kind;
};
struct ModuleExpr {
  const ModuleExprDesc* pmod_desc;
  Location pmod_loc;
  Attributes pmod_attributes;
};
PT_CTOR(ModuleExprDesc, Pmod_ident) LidLoc lid; PT_END
PT_CTOR(ModuleExprDesc, Pmod_structure) Structure str; PT_END
PT_CTOR(ModuleExprDesc, Pmod_functor) FunctorParameter param; const ModuleExpr* body; PT_END
PT_CTOR(ModuleExprDesc, Pmod_apply) const ModuleExpr* fn; const ModuleExpr* arg; PT_END
PT_CTOR(ModuleExprDesc, Pmod_apply_unit) const ModuleExpr* fn; PT_END
PT_CTOR(ModuleExprDesc, Pmod_constraint) const ModuleExpr* me; const ModuleType* mty; PT_END
PT_CTOR(ModuleExprDesc, Pmod_unpack) const Expression* exp; PT_END
PT_CTOR(ModuleExprDesc, Pmod_extension) const Extension* ext; PT_END
PT_CTOR(ModuleExprDesc, Pmod_hole) PT_END

struct ValueConstraint {
  enum class Kind : std::uint8_t { Pvc_constraint, Pvc_coercion };
  Kind kind;
  Slice<StrLoc> locally_abstract_univars;  // Pvc_constraint
  const CoreType* typ = nullptr;           // Pvc_constraint
  const CoreType* ground = nullptr;        // Pvc_coercion: option
  const CoreType* coercion = nullptr;      // Pvc_coercion
};
struct ValueBinding {
  const Pattern* pvb_pat;
  const Expression* pvb_expr;
  const ValueConstraint* pvb_constraint;  // option
  Attributes pvb_attributes;
  Location pvb_loc;
};
struct ModuleBinding {
  OptStrLoc pmb_name;
  const ModuleExpr* pmb_expr;
  Attributes pmb_attributes;
  Location pmb_loc;
};

struct StructureItemDesc {
  enum class Kind : std::uint8_t {
    Pstr_eval, Pstr_value, Pstr_val, Pstr_primitive, Pstr_type, Pstr_typext, Pstr_exception,
    Pstr_module, Pstr_recmodule, Pstr_modtype, Pstr_open, Pstr_class, Pstr_class_type,
    Pstr_include, Pstr_attribute, Pstr_extension
  };
  Kind kind;
};
struct StructureItem {
  const StructureItemDesc* pstr_desc;
  Location pstr_loc;
};
PT_CTOR(StructureItemDesc, Pstr_eval) const Expression* exp; Attributes attrs; PT_END
PT_CTOR(StructureItemDesc, Pstr_value) RecFlag rec; Slice<const ValueBinding*> vbs; PT_END
PT_CTOR(StructureItemDesc, Pstr_val) const ValueDescription* vd; PT_END
PT_CTOR(StructureItemDesc, Pstr_primitive) const PrimitiveDescription* pd; PT_END
PT_CTOR(StructureItemDesc, Pstr_type) RecFlag rec; Slice<const TypeDeclaration*> decls; PT_END
PT_CTOR(StructureItemDesc, Pstr_typext) const TypeExtension* ext; PT_END
PT_CTOR(StructureItemDesc, Pstr_exception) const TypeException* exn; PT_END
PT_CTOR(StructureItemDesc, Pstr_module) const ModuleBinding* mb; PT_END
PT_CTOR(StructureItemDesc, Pstr_recmodule) Slice<const ModuleBinding*> mbs; PT_END
PT_CTOR(StructureItemDesc, Pstr_modtype) const ModuleTypeDeclaration* mtd; PT_END
PT_CTOR(StructureItemDesc, Pstr_open) const OpenDeclaration* od; PT_END
PT_CTOR(StructureItemDesc, Pstr_class) Slice<const ClassDeclaration*> decls; PT_END
PT_CTOR(StructureItemDesc, Pstr_class_type) Slice<const ClassTypeDeclaration*> decls; PT_END
PT_CTOR(StructureItemDesc, Pstr_include) const IncludeDeclaration* incl; PT_END
PT_CTOR(StructureItemDesc, Pstr_attribute) const Attribute* attr; PT_END
PT_CTOR(StructureItemDesc, Pstr_extension) const Extension* ext; Attributes attrs; PT_END

#undef PT_CTOR
#undef PT_END

// Parsetree attributes as Types records hold them (support.hpp)
// Types' attributes are the parsetree's list itself: one Slice per
// parsetree list (memoized per unit -- reset_types_attributes at its start)
typing::Attributes types_attributes(const Attributes& l);
void reset_types_attributes();

// ---- construction from the C++ parser (parsetree_of_ast.cpp) --------------------------------
// Locations the parser does not record yet are gap_loc(): Location.none with
// pos_cnum = -2 (cxx/PORTING.md lists them).
Location gap_loc();
bool is_gap_loc(const Location& l);
Structure of_ast(const ast::Structure& s, std::string_view fname,
                 const std::vector<std::string>& dirfiles);
Signature of_ast_signature(const ast::Signature& s, std::string_view fname,
                           const std::vector<std::string>& dirfiles);
// Lexer.comments (), with the locations of_ast gives
std::vector<std::pair<std::string_view, Location>> comments_of_ast(const std::vector<ast::Comment>& cs,
                                                                   std::string_view fname,
                                                                   const std::vector<std::string>& dirfiles);

}  // namespace cppcaml::typing::parsetree
