// Port of typing/typedtree.mli (cxx/PORTING.md, stage 4): the typed tree
// Typetexp / Typecore / Typemod build.  Same idiom as parsetree.hpp: records
// are structs with the OCaml field names, each variant a `*Desc` base with
// one subclass per constructor (`as<Texp_apply>(e->exp_desc)`).  The GADT
// index of patterns (value / computation) is a runtime category
// (`classify_pattern`).  Attributes are Parsetree attributes, as in OCaml.
// The class, signature and module-type parts that only Typemod and
// Typeclass build are completed with stage 5.
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/parsetree.hpp"
#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::typedtree {

using parsetree::Attribute;
using parsetree::Attributes;
using parsetree::DirectionFlag;
using parsetree::Injectivity;
using parsetree::LidLoc;
using parsetree::OptStrLoc;
using parsetree::StrLoc;
using parsetree::Variance;
using parsetree::as;

enum class Partial : std::uint8_t { Partial, Total };

// Asttypes.constant (typed constants)
struct Constant {
  enum class Kind : std::uint8_t {
    Const_int, Const_char, Const_string, Const_float, Const_int32, Const_int64, Const_nativeint
  };
  Kind kind;
  long i = 0;                  // Const_int / Const_char
  std::int64_t boxed = 0;      // Const_int32 / Const_int64 / Const_nativeint
  // the boxed integer's identity: typecore's Int64.of_string allocates one
  // box per literal, which every copy of the constant (and the Lambda
  // constants made of it -- Symtable.transl_const) shares
  const void* box = nullptr;
  std::string_view s;          // Const_string / Const_float
  Location str_loc;            // Const_string
  OptStr delim;                // Const_string
};

struct CoreType;
struct Pattern;
struct Expression;
struct ModuleExpr;
struct ModuleType;
struct ClassExpr;
struct ClassType;
struct ClassStructure;
struct ValueBinding;
struct StructureItem;
struct PackageType;

// ---- patterns ----------------------------------------------------------------------------
enum class PatternCategory : std::uint8_t { Value, Computation };

struct PatExtra {
  enum class Kind : std::uint8_t { Tpat_constraint, Tpat_type, Tpat_open, Tpat_unpack };
  Kind kind;
  const CoreType* cty = nullptr;       // Tpat_constraint
  Path::t path = nullptr;              // Tpat_type / Tpat_open
  LidLoc lid;                          // Tpat_type / Tpat_open
  env::t env = nullptr;                // Tpat_open
  const PackageType* pack = nullptr;   // Tpat_unpack: option
};
struct PatExtraItem {  // (pat_extra * Location.t * attributes)
  PatExtra extra;
  Location loc;
  Attributes attrs;
};

struct PatternDesc {
  enum class Kind : std::uint8_t {
    Tpat_any, Tpat_var, Tpat_alias, Tpat_constant, Tpat_tuple, Tpat_construct, Tpat_variant,
    Tpat_record, Tpat_array, Tpat_lazy, Tpat_value, Tpat_exception, Tpat_or
  };
  Kind kind;
};
struct Pattern {  // 'k general_pattern
  const PatternDesc* pat_desc;
  LocPtr pat_loc;
  Slice<PatExtraItem> pat_extra;
  TypeExpr* pat_type;
  env::t pat_env;
  Attributes pat_attributes;
};

#define TT_CTOR(Base_, Name) \
  struct Name : Base_ {      \
    using Base = Base_;      \
    static constexpr Kind K = Kind::Name;
#define TT_END };

struct LabeledPattern {
  OptStr label;
  const Pattern* pat;
};
struct ConstructTypeAnnot {  // (Ident.t loc list * core_type)
  Slice<std::pair<Ident::t, Location>> vars;
  const CoreType* cty;
};
struct RecordPatField {  // (Longident.t loc * label_description * pattern)
  LidLoc lid;
  const LabelDescription* label;
  const Pattern* pat;
};
struct RowDescRef {  // Types.row_desc ref
  const RowDesc* contents;
};

TT_CTOR(PatternDesc, Tpat_any) TT_END
TT_CTOR(PatternDesc, Tpat_var) Ident::t id; StrLoc name; Uid uid; TT_END
TT_CTOR(PatternDesc, Tpat_alias)
  const Pattern* pat;
  Ident::t id;
  StrLoc name;
  Uid uid;
  TypeExpr* ty;
TT_END
TT_CTOR(PatternDesc, Tpat_constant) Constant c; TT_END
TT_CTOR(PatternDesc, Tpat_tuple) Slice<LabeledPattern> pats; TT_END
TT_CTOR(PatternDesc, Tpat_construct)
  LidLoc lid;
  const ConstructorDescription* cstr;
  Slice<const Pattern*> args;
  const ConstructTypeAnnot* annot;  // option
TT_END
TT_CTOR(PatternDesc, Tpat_variant)
  std::string_view label;
  const Pattern* arg;  // option
  RowDescRef* row;
TT_END
TT_CTOR(PatternDesc, Tpat_record) Slice<RecordPatField> fields; ClosedFlag closed; TT_END
TT_CTOR(PatternDesc, Tpat_array) MutableFlag mut; Slice<const Pattern*> pats; TT_END
TT_CTOR(PatternDesc, Tpat_lazy) const Pattern* pat; TT_END
TT_CTOR(PatternDesc, Tpat_value) const Pattern* pat; TT_END
TT_CTOR(PatternDesc, Tpat_exception) const Pattern* pat; TT_END
TT_CTOR(PatternDesc, Tpat_or)
  const Pattern* p1;
  const Pattern* p2;
  const RowDesc* row;  // option
TT_END

// ---- expressions -------------------------------------------------------------------------
struct ExpExtra {
  enum class Kind : std::uint8_t { Texp_constraint, Texp_coerce, Texp_poly, Texp_newtype };
  Kind kind;
  const CoreType* cty = nullptr;   // Texp_constraint; Texp_coerce target; Texp_poly: option
  const CoreType* from = nullptr;  // Texp_coerce: option
  std::string_view name;           // Texp_newtype
};
struct ExpExtraItem {
  ExpExtra extra;
  Location loc;
  Attributes attrs;
};
struct ExpressionDesc {
  enum class Kind : std::uint8_t {
    Texp_ident, Texp_constant, Texp_let, Texp_function, Texp_apply, Texp_match, Texp_try,
    Texp_tuple, Texp_construct, Texp_variant, Texp_record, Texp_atomic_loc, Texp_field,
    Texp_setfield, Texp_array, Texp_ifthenelse, Texp_sequence, Texp_while, Texp_for,
    Texp_send, Texp_new, Texp_instvar, Texp_setinstvar, Texp_override, Texp_assert,
    Texp_lazy, Texp_object, Texp_pack, Texp_letop, Texp_unreachable,
    Texp_extension_constructor, Texp_struct_item
  };
  Kind kind;
};
struct Expression {
  const ExpressionDesc* exp_desc;
  LocPtr exp_loc;
  Slice<ExpExtraItem> exp_extra;
  TypeExpr* exp_type;
  env::t exp_env;
  Attributes exp_attributes;
};

struct Meth {  // Tmeth_name | Tmeth_val | Tmeth_ancestor
  enum class Kind : std::uint8_t { Tmeth_name, Tmeth_val, Tmeth_ancestor };
  Kind kind;
  std::string_view name;       // Tmeth_name
  Ident::t id = nullptr;       // Tmeth_val / Tmeth_ancestor
  Path::t path = nullptr;      // Tmeth_ancestor
};
struct ContDesc {  // cont_desc
  Ident::t cont_id;
  Location cont_loc;
  TypeExpr* cont_type;
  Uid cont_uid;
};
struct Case {  // 'k case
  const Pattern* c_lhs;
  const ContDesc* c_cont;       // option
  const Expression* c_guard;    // option
  const Expression* c_rhs;
};
struct FunctionParamKind {
  enum class Kind : std::uint8_t { Tparam_pat, Tparam_optional_default };
  Kind kind;
  const Pattern* pat;
  const Expression* default_ = nullptr;  // Tparam_optional_default
};
struct FunctionParam {
  ArgLabel fp_arg_label;
  Ident::t fp_param;
  Partial fp_partial;
  FunctionParamKind fp_kind;
  Slice<StrLoc> fp_newtypes;
  Location fp_loc;
};
struct FunctionBody {
  enum class Kind : std::uint8_t { Tfunction_body, Tfunction_cases };
  Kind kind;
  const Expression* body = nullptr;  // Tfunction_body
  // Tfunction_cases
  Slice<const Case*> cases;
  Partial partial = Partial::Total;
  Ident::t param = nullptr;
  Location loc;
  const ExpExtra* exp_extra = nullptr;  // option
  Attributes attributes;
};
struct RecordLabelDefinition {  // Kept of type_expr * mutable_flag | Overridden of lid * expression
  bool kept;
  TypeExpr* ty = nullptr;
  MutableFlag mut = MutableFlag::Immutable;
  LidLoc lid;
  const Expression* exp = nullptr;
};
struct RecordField {  // (label_description * record_label_definition)
  const LabelDescription* label;
  RecordLabelDefinition def;
};
struct BindingOp {
  Path::t bop_op_path;
  StrLoc bop_op_name;
  const ValueDescription* bop_op_val;
  TypeExpr* bop_op_type;
  const Expression* bop_exp;
  Location bop_loc;
};
struct ApplyArg {  // (expression, unit) arg_or_omitted
  bool omitted;
  const Expression* arg;
};
struct LabeledArg {  // arg_label * apply_arg
  ArgLabel label;
  ApplyArg arg;
};
struct LabeledExpression {
  OptStr label;
  const Expression* exp;
};

TT_CTOR(ExpressionDesc, Texp_ident) Path::t path; LidLoc lid; const ValueDescription* vd; TT_END
TT_CTOR(ExpressionDesc, Texp_constant) Constant c; TT_END
TT_CTOR(ExpressionDesc, Texp_let) RecFlag rec; Slice<const ValueBinding*> vbs; const Expression* body; TT_END
TT_CTOR(ExpressionDesc, Texp_function) Slice<const FunctionParam*> params; const FunctionBody* body; TT_END
TT_CTOR(ExpressionDesc, Texp_apply) const Expression* fn; Slice<LabeledArg> args; TT_END
TT_CTOR(ExpressionDesc, Texp_match)
  const Expression* exp;
  Slice<const Case*> comp_cases;   // computation cases
  Slice<const Case*> eff_cases;    // value cases (effects)
  Partial partial;
TT_END
TT_CTOR(ExpressionDesc, Texp_try)
  const Expression* exp;
  Slice<const Case*> exn_cases;
  Slice<const Case*> eff_cases;
TT_END
TT_CTOR(ExpressionDesc, Texp_tuple) Slice<LabeledExpression> el; TT_END
TT_CTOR(ExpressionDesc, Texp_construct)
  LidLoc lid;
  const ConstructorDescription* cstr;
  Slice<const Expression*> args;
TT_END
TT_CTOR(ExpressionDesc, Texp_variant) std::string_view label; const Expression* arg; TT_END
TT_CTOR(ExpressionDesc, Texp_record)
  Slice<RecordField> fields;
  RecordRepresentation representation;
  const Expression* extended_expression;  // option
TT_END
TT_CTOR(ExpressionDesc, Texp_atomic_loc)
  const Expression* exp;
  LidLoc lid;
  const LabelDescription* label;
TT_END
TT_CTOR(ExpressionDesc, Texp_field)
  const Expression* exp;
  LidLoc lid;
  const LabelDescription* label;
TT_END
TT_CTOR(ExpressionDesc, Texp_setfield)
  const Expression* exp;
  LidLoc lid;
  const LabelDescription* label;
  const Expression* value;
TT_END
TT_CTOR(ExpressionDesc, Texp_array) MutableFlag mut; Slice<const Expression*> el; TT_END
TT_CTOR(ExpressionDesc, Texp_ifthenelse)
  const Expression* cond;
  const Expression* then_;
  const Expression* else_;  // option
TT_END
TT_CTOR(ExpressionDesc, Texp_sequence) const Expression* e1; const Expression* e2; TT_END
TT_CTOR(ExpressionDesc, Texp_while) const Expression* cond; const Expression* body; TT_END
TT_CTOR(ExpressionDesc, Texp_for)
  Ident::t id;
  const parsetree::Pattern* pat;
  const Expression* lo;
  const Expression* hi;
  DirectionFlag dir;
  const Expression* body;
TT_END
TT_CTOR(ExpressionDesc, Texp_send) const Expression* obj; Meth meth; TT_END
TT_CTOR(ExpressionDesc, Texp_new) Path::t path; LidLoc lid; const ClassDeclaration* decl; TT_END
TT_CTOR(ExpressionDesc, Texp_instvar) Path::t self_path; Path::t path; StrLoc name; TT_END
TT_CTOR(ExpressionDesc, Texp_setinstvar)
  Path::t self_path;
  Path::t path;
  StrLoc name;
  const Expression* value;
TT_END
struct OverrideField {
  Ident::t id;
  StrLoc name;
  const Expression* exp;
};
TT_CTOR(ExpressionDesc, Texp_override) Path::t self_path; Slice<OverrideField> fields; TT_END
TT_CTOR(ExpressionDesc, Texp_assert) const Expression* exp; Location loc; TT_END
TT_CTOR(ExpressionDesc, Texp_lazy) const Expression* exp; TT_END
TT_CTOR(ExpressionDesc, Texp_object) const ClassStructure* cs; Slice<std::string_view> meths; TT_END
TT_CTOR(ExpressionDesc, Texp_pack) const ModuleExpr* me; TT_END
TT_CTOR(ExpressionDesc, Texp_letop)
  const BindingOp* let_;
  Slice<const BindingOp*> ands;
  Ident::t param;
  const Case* body;
  Partial partial;
TT_END
TT_CTOR(ExpressionDesc, Texp_unreachable) TT_END
TT_CTOR(ExpressionDesc, Texp_extension_constructor) LidLoc lid; Path::t path; TT_END
TT_CTOR(ExpressionDesc, Texp_struct_item) const StructureItem* item; const Expression* body; TT_END

// ---- core types --------------------------------------------------------------------------
struct CoreTypeDesc {
  enum class Kind : std::uint8_t {
    Ttyp_any, Ttyp_var, Ttyp_arrow, Ttyp_tuple, Ttyp_constr, Ttyp_object, Ttyp_class,
    Ttyp_alias, Ttyp_variant, Ttyp_poly, Ttyp_package, Ttyp_open, Ttyp_functor
  };
  Kind kind;
};
struct CoreType {
  const CoreTypeDesc* ctyp_desc;  // mutable (Typeclass.declare_method)
  TypeExpr* ctyp_type;            // mutable
  env::t ctyp_env;
  LocPtr ctyp_loc;
  Attributes ctyp_attributes;
};
struct PackageType {
  Path::t tpt_path;
  Slice<std::pair<LidLoc, const CoreType*>> tpt_constraints;
  const Package* tpt_type;
  LidLoc tpt_txt;
};
struct RowFieldDesc {  // Ttag of string loc * bool * core_type list | Tinherit of core_type
  bool is_tag;
  StrLoc label;
  bool constant = false;
  Slice<const CoreType*> types;
  const CoreType* inherit = nullptr;
};
struct RowField {
  RowFieldDesc rf_desc;
  Location rf_loc;
  Attributes rf_attributes;
};
struct ObjectFieldDesc {  // OTtag of string loc * core_type | OTinherit of core_type
  bool is_tag;
  StrLoc label;
  const CoreType* ty;
};
struct ObjectField {
  ObjectFieldDesc of_desc;
  Location of_loc;
  Attributes of_attributes;
};
struct LabeledCoreType {
  OptStr label;
  const CoreType* ty;
};
TT_CTOR(CoreTypeDesc, Ttyp_any) TT_END
TT_CTOR(CoreTypeDesc, Ttyp_var) std::string_view name; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_arrow) ArgLabel label; const CoreType* t1; const CoreType* t2; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_tuple) Slice<LabeledCoreType> tl; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_constr) Path::t path; LidLoc lid; Slice<const CoreType*> args; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_object) Slice<const ObjectField*> fields; ClosedFlag closed; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_class) Path::t path; LidLoc lid; Slice<const CoreType*> args; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_alias) const CoreType* ty; StrLoc name; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_variant)
  Slice<const RowField*> fields;
  ClosedFlag closed;
  bool has_labels;
  Slice<std::string_view> labels;
  // the parsetree's `Some labels` block, which Typetexp keeps: one per
  // Ptyp_variant (the parser shares a let-binding's constraint between its
  // pattern and its expression)
  const void* labels_obj = nullptr;
TT_END
TT_CTOR(CoreTypeDesc, Ttyp_poly) Slice<std::string_view> vars; const CoreType* ty; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_package) const PackageType* pack; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_open) Path::t path; LidLoc lid; const CoreType* ty; TT_END
TT_CTOR(CoreTypeDesc, Ttyp_functor)
  ArgLabel label;
  Ident::t id;
  Location id_loc;
  const PackageType* pack;
  const CoreType* ty;
TT_END

// ---- value bindings / declarations -----------------------------------------------------
enum class RecursiveBindingKind : std::uint8_t { Static, Dynamic };  // Value_rec_types
struct ValueBinding {
  const Pattern* vb_pat;
  const Expression* vb_expr;
  RecursiveBindingKind vb_rec_kind;
  Attributes vb_attributes;
  Location vb_loc;
};
struct TypeParam {  // core_type * (variance * injectivity)
  const CoreType* ty;
  Variance variance;
  Injectivity injectivity;
};
struct TValueDescription {
  Ident::t val_id;
  StrLoc val_name;
  const CoreType* val_desc;
  const ValueDescription* val_val;
  Location val_loc;
  Attributes val_attributes;
};
struct PrimitiveKind {
  enum class Kind : std::uint8_t { Tprim_decl, Tprim_alias };
  Kind kind;
  const CoreType* cty;            // Tprim_decl; Tprim_alias: option
  Slice<std::string_view> prims;  // Tprim_decl
  Path::t path = nullptr;         // Tprim_alias
  LidLoc lid;                     // Tprim_alias
};
struct TPrimitiveDescription {
  Ident::t prim_id;
  StrLoc prim_name;
  PrimitiveKind prim_kind;
  const ValueDescription* prim_val;
  Location prim_loc;
  Attributes prim_attributes;
};
struct TLabelDeclaration {
  Ident::t ld_id;
  StrLoc ld_name;
  Uid ld_uid;
  MutableFlag ld_mutable;
  AtomicFlag ld_atomic;
  const CoreType* ld_type;
  Location ld_loc;
  Attributes ld_attributes;
};
struct TConstructorArguments {  // Cstr_tuple | Cstr_record
  bool is_record;
  Slice<const CoreType*> tuple;
  Slice<const TLabelDeclaration*> record;
};
struct TConstructorDeclaration {
  Ident::t cd_id;
  StrLoc cd_name;
  Uid cd_uid;
  Slice<StrLoc> cd_vars;
  TConstructorArguments cd_args;
  const CoreType* cd_res;  // option
  Location cd_loc;
  Attributes cd_attributes;
};
struct TTypeKind {
  enum class Kind : std::uint8_t { Ttype_abstract, Ttype_variant, Ttype_record, Ttype_open, Ttype_external };
  Kind kind;
  Slice<const TConstructorDeclaration*> constructors;
  Slice<const TLabelDeclaration*> labels;
  std::string_view external;
};
struct TypeConstraintItem {
  const CoreType* t1;
  const CoreType* t2;
  Location loc;
};
struct TTypeDeclaration {
  Ident::t typ_id;
  StrLoc typ_name;
  Slice<TypeParam> typ_params;
  const TypeDeclaration* typ_type;
  Slice<TypeConstraintItem> typ_constraints;
  TTypeKind typ_kind;
  PrivateFlag typ_private;
  const CoreType* typ_manifest;  // option
  Location typ_loc;
  Attributes typ_attributes;
};
struct TExtensionConstructorKind {
  enum class Kind : std::uint8_t { Text_decl, Text_rebind };
  Kind kind;
  Slice<StrLoc> vars;
  TConstructorArguments args{};
  const CoreType* res = nullptr;
  Path::t path = nullptr;
  LidLoc lid;
};
struct TExtensionConstructor {
  Ident::t ext_id;
  StrLoc ext_name;
  const ExtensionConstructor* ext_type;
  TExtensionConstructorKind ext_kind;
  Location ext_loc;
  Attributes ext_attributes;
};
struct TTypeExtension {
  Path::t tyext_path;
  LidLoc tyext_txt;
  Slice<TypeParam> tyext_params;
  Slice<const TExtensionConstructor*> tyext_constructors;
  PrivateFlag tyext_private;
  Location tyext_loc;
  Attributes tyext_attributes;
};
struct TTypeException {
  const TExtensionConstructor* tyexn_constructor;
  Location tyexn_loc;
  Attributes tyexn_attributes;
};

// ---- class language (declarations only; Typeclass comes with stage 5) -----------------
struct ClassStructure;
struct ClassExprDesc {
  enum class Kind : std::uint8_t {
    Tcl_ident, Tcl_structure, Tcl_fun, Tcl_apply, Tcl_let, Tcl_constraint, Tcl_open
  };
  Kind kind;
};
struct ClassExpr {
  const ClassExprDesc* cl_desc;
  Location cl_loc;
  const typing::ClassType* cl_type;
  env::t cl_env;
  Attributes cl_attributes;
};
struct ClassTypeDesc {
  enum class Kind : std::uint8_t { Tcty_constr, Tcty_signature, Tcty_arrow, Tcty_open };
  Kind kind;
};
struct ClassType {
  const ClassTypeDesc* cltyp_desc;
  const typing::ClassType* cltyp_type;
  env::t cltyp_env;
  Location cltyp_loc;
  Attributes cltyp_attributes;
};
struct ClassField;
struct ClassStructure {
  const Pattern* cstr_self;
  Slice<const ClassField*> cstr_fields;
  const ClassSignature* cstr_type;
  StrMap<Ident::t> cstr_meths;
};

// ---- module language ---------------------------------------------------------------------
struct ModuleCoercion;
struct PrimitiveCoercion {
  const PrimitiveDescription* pc_desc;
  TypeExpr* pc_type;
  env::t pc_env;
  Location pc_loc;
};
struct PosCoercion {  // int * module_coercion
  long pos;
  const ModuleCoercion* cc;
};
struct IdPosCoercion {  // Ident.t * int * module_coercion
  Ident::t id;
  long pos;
  const ModuleCoercion* cc;
};
struct ModuleCoercion {
  enum class Kind : std::uint8_t {
    Tcoerce_none, Tcoerce_structure, Tcoerce_functor, Tcoerce_primitive, Tcoerce_alias
  };
  Kind kind;
  // Tcoerce_alias of Env.t * Path.t * module_coercion
  env::t alias_env = nullptr;
  Path::t alias_path = nullptr;
  const ModuleCoercion* alias_coercion = nullptr;
  // Tcoerce_structure of (int * module_coercion) list * (Ident.t * int * module_coercion) list
  Slice<PosCoercion> pos_cc;
  Slice<IdPosCoercion> id_pos_cc;
  // Tcoerce_functor of module_coercion * module_coercion
  const ModuleCoercion* arg = nullptr;
  const ModuleCoercion* res = nullptr;
  // Tcoerce_primitive of primitive_coercion
  const PrimitiveCoercion* prim = nullptr;
};
const ModuleCoercion* tcoerce_none();
struct FunctorParameter {  // Unit | Named of Ident.t option * string option loc * module_type
  bool is_unit;
  Ident::t id = nullptr;  // option
  OptStrLoc name;
  const ModuleType* mty = nullptr;
  // the `Some id` block's identity: Typemod's one option value, which the
  // Types.Named of the module type holds too (types.hpp's some_obj)
  const void* some_obj = nullptr;
};
struct ModuleExprDesc {
  enum class Kind : std::uint8_t {
    Tmod_ident, Tmod_structure, Tmod_functor, Tmod_apply, Tmod_apply_unit, Tmod_constraint,
    Tmod_unpack
  };
  Kind kind;
};
struct ModuleExpr {
  const ModuleExprDesc* mod_desc;
  Location mod_loc;
  const typing::ModuleType* mod_type;
  env::t mod_env;
  Attributes mod_attributes;
};
struct ModuleTypeDesc {
  enum class Kind : std::uint8_t {
    Tmty_ident, Tmty_signature, Tmty_functor, Tmty_with, Tmty_typeof, Tmty_alias
  };
  Kind kind;
};
struct ModuleType {
  const ModuleTypeDesc* mty_desc;
  const typing::ModuleType* mty_type;
  env::t mty_env;
  Location mty_loc;
  Attributes mty_attributes;
};
struct StructureItemDesc {
  enum class Kind : std::uint8_t {
    Tstr_eval, Tstr_value, Tstr_primitive, Tstr_type, Tstr_typext, Tstr_exception,
    Tstr_module, Tstr_recmodule, Tstr_modtype, Tstr_open, Tstr_class, Tstr_class_type,
    Tstr_include, Tstr_attribute
  };
  Kind kind;
};
struct StructureItem {
  const StructureItemDesc* str_desc;
  Location str_loc;
  env::t str_env;
};
struct Structure {
  Slice<const StructureItem*> str_items;
  Signature str_type;
  env::t str_final_env;
};
struct ModuleBinding {
  Ident::t mb_id;  // option
  OptStrLoc mb_name;
  Uid mb_uid;
  ModulePresence mb_presence;
  const ModuleExpr* mb_expr;
  Attributes mb_attributes;
  Location mb_loc;
};
template <class A>
struct OpenInfos {
  A open_expr;
  Signature open_bound_items;
  OverrideFlag open_override;
  env::t open_env;
  Location open_loc;
  Attributes open_attributes;
};
struct PathLid {
  Path::t path;
  LidLoc lid;
};
using OpenDescription = OpenInfos<PathLid>;
using OpenDeclaration = OpenInfos<const ModuleExpr*>;

template <class A>
struct IncludeInfos {
  A incl_mod;
  Signature incl_type;
  Location incl_loc;
  Attributes incl_attributes;
};
using IncludeDeclaration = IncludeInfos<const ModuleExpr*>;
using IncludeDescription = IncludeInfos<const ModuleType*>;
struct TModuleTypeDeclaration {
  Ident::t mtd_id;
  StrLoc mtd_name;
  Uid mtd_uid;
  const ModuleType* mtd_type;  // option
  Attributes mtd_attributes;
  Location mtd_loc;
};

// class fields (Texp_object / Typeclass)
struct ClassFieldKind {  // Tcfk_virtual of core_type | Tcfk_concrete of override_flag * expression
  bool is_virtual;
  const CoreType* cty = nullptr;
  OverrideFlag ovr = OverrideFlag::Fresh;
  const Expression* exp = nullptr;
};
struct ClassFieldDesc {
  enum class Kind : std::uint8_t {
    Tcf_inherit, Tcf_val, Tcf_method, Tcf_constraint, Tcf_initializer, Tcf_attribute
  };
  Kind kind;
};
struct ClassField {
  const ClassFieldDesc* cf_desc;
  Location cf_loc;
  Attributes cf_attributes;
};

TT_CTOR(StructureItemDesc, Tstr_eval) const Expression* exp; Attributes attrs; TT_END
TT_CTOR(StructureItemDesc, Tstr_value) RecFlag rec; Slice<const ValueBinding*> vbs; TT_END
TT_CTOR(StructureItemDesc, Tstr_primitive) const TPrimitiveDescription* pd; TT_END
TT_CTOR(StructureItemDesc, Tstr_type) RecFlag rec; Slice<const TTypeDeclaration*> decls; TT_END
TT_CTOR(StructureItemDesc, Tstr_typext) const TTypeExtension* ext; TT_END
TT_CTOR(StructureItemDesc, Tstr_exception) const TTypeException* exn; TT_END
TT_CTOR(StructureItemDesc, Tstr_module) const ModuleBinding* mb; TT_END
TT_CTOR(StructureItemDesc, Tstr_recmodule) Slice<const ModuleBinding*> mbs; TT_END
TT_CTOR(StructureItemDesc, Tstr_open) const OpenDeclaration* od; TT_END
TT_CTOR(StructureItemDesc, Tstr_attribute) const Attribute* attr; TT_END
TT_CTOR(StructureItemDesc, Tstr_modtype) const TModuleTypeDeclaration* mtd; TT_END
TT_CTOR(StructureItemDesc, Tstr_include) const IncludeDeclaration* incl; TT_END

TT_CTOR(ClassFieldDesc, Tcf_inherit)
  OverrideFlag ovr;
  const ClassExpr* ce;
  OptStr as;
  Slice<std::pair<std::string_view, Ident::t>> vals;
  Slice<std::pair<std::string_view, Ident::t>> meths;
TT_END
TT_CTOR(ClassFieldDesc, Tcf_val)
  StrLoc name;
  MutableFlag mut;
  Ident::t id;
  ClassFieldKind kind_;
  bool inherited;
TT_END
TT_CTOR(ClassFieldDesc, Tcf_method) StrLoc name; PrivateFlag priv; ClassFieldKind kind_; TT_END
TT_CTOR(ClassFieldDesc, Tcf_constraint) const CoreType* t1; const CoreType* t2; TT_END
TT_CTOR(ClassFieldDesc, Tcf_initializer) const Expression* exp; TT_END
TT_CTOR(ClassFieldDesc, Tcf_attribute) const Attribute* attr; TT_END

TT_CTOR(ModuleExprDesc, Tmod_ident) Path::t path; LidLoc lid; TT_END
TT_CTOR(ModuleExprDesc, Tmod_structure) const Structure* str; TT_END
TT_CTOR(ModuleExprDesc, Tmod_functor) FunctorParameter param; const ModuleExpr* body; TT_END
TT_CTOR(ModuleExprDesc, Tmod_apply)
  const ModuleExpr* fn;
  const ModuleExpr* arg;
  const ModuleCoercion* coercion;
TT_END
TT_CTOR(ModuleExprDesc, Tmod_apply_unit) const ModuleExpr* fn; TT_END
TT_CTOR(ModuleExprDesc, Tmod_constraint)
  const ModuleExpr* me;
  const typing::ModuleType* mty;
  const ModuleType* explicit_mty;  // Tmodtype_explicit (nullptr = Tmodtype_implicit)
  const ModuleCoercion* coercion;
TT_END
TT_CTOR(ModuleExprDesc, Tmod_unpack) const Expression* exp; const typing::ModuleType* mty; TT_END

TT_CTOR(ModuleTypeDesc, Tmty_ident) Path::t path; LidLoc lid; TT_END
TT_CTOR(ModuleTypeDesc, Tmty_functor) FunctorParameter param; const ModuleType* body; TT_END
TT_CTOR(ModuleTypeDesc, Tmty_typeof) const ModuleExpr* me; TT_END
TT_CTOR(ModuleTypeDesc, Tmty_alias) Path::t path; LidLoc lid; TT_END

// class expressions (class_expr_desc) and class declarations (Tstr_class)
struct IdentExpression {  // Ident.t * expression
  Ident::t id;
  const Expression* exp;
};
TT_CTOR(ClassExprDesc, Tcl_ident) Path::t path; LidLoc lid; Slice<const CoreType*> args; TT_END
TT_CTOR(ClassExprDesc, Tcl_structure) const ClassStructure* cs; TT_END
TT_CTOR(ClassExprDesc, Tcl_fun)
  ArgLabel label;
  const Pattern* pat;
  Slice<IdentExpression> args;
  const ClassExpr* ce;
  Partial partial;
TT_END
TT_CTOR(ClassExprDesc, Tcl_apply) const ClassExpr* ce; Slice<LabeledArg> args; TT_END
TT_CTOR(ClassExprDesc, Tcl_let)
  RecFlag rec;
  Slice<const ValueBinding*> vbs;
  Slice<IdentExpression> vals;
  const ClassExpr* ce;
TT_END
TT_CTOR(ClassExprDesc, Tcl_constraint)
  const ClassExpr* ce;
  const ClassType* cty;  // option
  Slice<std::string_view> vals;           // visible instance variables
  Slice<std::string_view> meths;          // methods
  Slice<std::string_view> concrete_meths;  // Types.MethSet.t
TT_END
TT_CTOR(ClassExprDesc, Tcl_open) const OpenDescription* od; const ClassExpr* ce; TT_END
template <class A>
struct ClassInfos {  // 'a class_infos
  VirtualFlag ci_virt;
  Slice<TypeParam> ci_params;
  StrLoc ci_id_name;
  Ident::t ci_id_class;
  Ident::t ci_id_class_type;
  Ident::t ci_id_object;
  A ci_expr;
  const typing::ClassDeclaration* ci_decl;
  const typing::ClassTypeDeclaration* ci_type_decl;
  Location ci_loc;
  Attributes ci_attributes;
};
using TClassDeclaration = ClassInfos<const ClassExpr*>;
struct ClassDeclarationItem {  // class_declaration * string list
  const TClassDeclaration* decl;
  Slice<std::string_view> names;
};
TT_CTOR(StructureItemDesc, Tstr_class) Slice<ClassDeclarationItem> classes; TT_END

// ---- signatures, module types and class types (Typemod / Typeclass) ----------------------
struct Signature;
struct TModuleDeclaration {
  Ident::t md_id;  // option
  OptStrLoc md_name;
  Uid md_uid;
  ModulePresence md_presence;
  const ModuleType* md_type;
  Attributes md_attributes;
  Location md_loc;
};
struct TModuleSubstitution {
  Ident::t ms_id;
  StrLoc ms_name;
  Uid ms_uid;
  Path::t ms_manifest;
  LidLoc ms_txt;
  Attributes ms_attributes;
  Location ms_loc;
};
struct WithConstraint {
  enum class Kind : std::uint8_t {
    Twith_type, Twith_module, Twith_modtype, Twith_typesubst, Twith_modsubst, Twith_modtypesubst
  };
  Kind kind;
  const TTypeDeclaration* decl = nullptr;  // Twith_type / Twith_typesubst
  Path::t path = nullptr;                  // Twith_module / Twith_modsubst
  LidLoc lid;                              // Twith_module / Twith_modsubst
  const ModuleType* mty = nullptr;         // Twith_modtype / Twith_modtypesubst
};
struct WithConstraintItem {  // Path.t * Longident.t loc * with_constraint
  Path::t path;
  LidLoc lid;
  WithConstraint cstr;
};
TT_CTOR(ModuleTypeDesc, Tmty_signature) const Signature* sig; TT_END
TT_CTOR(ModuleTypeDesc, Tmty_with) const ModuleType* mty; Slice<WithConstraintItem> cstrs; TT_END

// class types
struct TClassSignature;
TT_CTOR(ClassTypeDesc, Tcty_constr) Path::t path; LidLoc lid; Slice<const CoreType*> args; TT_END
TT_CTOR(ClassTypeDesc, Tcty_signature) const TClassSignature* sig; TT_END
TT_CTOR(ClassTypeDesc, Tcty_arrow) ArgLabel label; const CoreType* arg; const ClassType* cty; TT_END
TT_CTOR(ClassTypeDesc, Tcty_open) const OpenDescription* od; const ClassType* cty; TT_END
struct ClassTypeFieldDesc {
  enum class Kind : std::uint8_t { Tctf_inherit, Tctf_val, Tctf_method, Tctf_constraint, Tctf_attribute };
  Kind kind;
};
struct ClassTypeField {
  const ClassTypeFieldDesc* ctf_desc;
  Location ctf_loc;
  Attributes ctf_attributes;
};
struct TClassSignature {
  const CoreType* csig_self;
  Slice<const ClassTypeField*> csig_fields;
  const typing::ClassSignature* csig_type;
};
TT_CTOR(ClassTypeFieldDesc, Tctf_inherit) const ClassType* cty; TT_END
TT_CTOR(ClassTypeFieldDesc, Tctf_val)
  std::string_view name;
  MutableFlag mut;
  VirtualFlag virt;
  const CoreType* ty;
TT_END
TT_CTOR(ClassTypeFieldDesc, Tctf_method)
  std::string_view name;
  PrivateFlag priv;
  VirtualFlag virt;
  const CoreType* ty;
TT_END
TT_CTOR(ClassTypeFieldDesc, Tctf_constraint) const CoreType* t1; const CoreType* t2; TT_END
TT_CTOR(ClassTypeFieldDesc, Tctf_attribute) const Attribute* attr; TT_END
using TClassDescription = ClassInfos<const ClassType*>;
using TClassTypeDeclaration = ClassInfos<const ClassType*>;
struct ClassTypeDeclarationItem {  // Ident.t * string loc * class_type_declaration
  Ident::t id;
  StrLoc name;
  const TClassTypeDeclaration* decl;
};
TT_CTOR(StructureItemDesc, Tstr_class_type) Slice<ClassTypeDeclarationItem> classes; TT_END

// signatures
struct SignatureItemDesc {
  enum class Kind : std::uint8_t {
    Tsig_value, Tsig_primitive, Tsig_type, Tsig_typesubst, Tsig_typext, Tsig_exception, Tsig_module,
    Tsig_modsubst, Tsig_recmodule, Tsig_modtype, Tsig_modtypesubst, Tsig_open, Tsig_include,
    Tsig_class, Tsig_class_type, Tsig_attribute
  };
  Kind kind;
};
struct SignatureItem {
  const SignatureItemDesc* sig_desc;
  env::t sig_env;
  Location sig_loc;
};
struct Signature {
  Slice<const SignatureItem*> sig_items;
  typing::Signature sig_type;
  env::t sig_final_env;
};
TT_CTOR(SignatureItemDesc, Tsig_value) const TValueDescription* vd; TT_END
TT_CTOR(SignatureItemDesc, Tsig_primitive) const TPrimitiveDescription* pd; TT_END
TT_CTOR(SignatureItemDesc, Tsig_type) RecFlag rec; Slice<const TTypeDeclaration*> decls; TT_END
TT_CTOR(SignatureItemDesc, Tsig_typesubst) Slice<const TTypeDeclaration*> decls; TT_END
TT_CTOR(SignatureItemDesc, Tsig_typext) const TTypeExtension* ext; TT_END
TT_CTOR(SignatureItemDesc, Tsig_exception) const TTypeException* exn; TT_END
TT_CTOR(SignatureItemDesc, Tsig_module) const TModuleDeclaration* md; TT_END
TT_CTOR(SignatureItemDesc, Tsig_modsubst) const TModuleSubstitution* ms; TT_END
TT_CTOR(SignatureItemDesc, Tsig_recmodule) Slice<const TModuleDeclaration*> mds; TT_END
TT_CTOR(SignatureItemDesc, Tsig_modtype) const TModuleTypeDeclaration* mtd; TT_END
TT_CTOR(SignatureItemDesc, Tsig_modtypesubst) const TModuleTypeDeclaration* mtd; TT_END
TT_CTOR(SignatureItemDesc, Tsig_open) const OpenDescription* od; TT_END
TT_CTOR(SignatureItemDesc, Tsig_include) const IncludeDescription* incl; TT_END
TT_CTOR(SignatureItemDesc, Tsig_class) Slice<const TClassDescription*> classes; TT_END
TT_CTOR(SignatureItemDesc, Tsig_class_type) Slice<const TClassTypeDeclaration*> classes; TT_END
TT_CTOR(SignatureItemDesc, Tsig_attribute) const Attribute* attr; TT_END

// A typechecked implementation (the shape is not ported: it only feeds the
// cmt file)
struct Implementation {
  const Structure* structure;
  const ModuleCoercion* coercion;
  typing::Signature signature;
};

#undef TT_CTOR
#undef TT_END

// ---- auxiliary functions (typedtree.ml) ------------------------------------------------
const Pattern* as_computation_pattern(const Pattern* p);
PatternCategory classify_pattern_desc(const PatternDesc* d);
PatternCategory classify_pattern(const Pattern* p);
void shallow_iter_pattern_desc(const std::function<void(const Pattern*)>& f, const PatternDesc* d);
const PatternDesc* shallow_map_pattern_desc(const std::function<const Pattern*(const Pattern*)>& f,
                                            const PatternDesc* d);
void iter_general_pattern(const std::function<void(const Pattern*)>& f, const Pattern* p);
void iter_pattern(const std::function<void(const Pattern*)>& f, const Pattern* p);
bool exists_general_pattern(const std::function<bool(const Pattern*)>& f, const Pattern* p);
bool exists_pattern(const std::function<bool(const Pattern*)>& f, const Pattern* p);
struct BoundIdent {  // (Ident.t * string loc * type_expr * Uid.t)
  Ident::t id;
  StrLoc name;
  TypeExpr* ty;
  Uid uid;
};
std::vector<Ident::t> let_bound_idents(Slice<const ValueBinding*> vbs);
std::vector<BoundIdent> let_bound_idents_full(Slice<const ValueBinding*> vbs);
const Pattern* alpha_pat(const std::vector<std::pair<Ident::t, Ident::t>>& env, const Pattern* p);
std::vector<Ident::t> pat_bound_idents(const Pattern* p);
std::vector<BoundIdent> pat_bound_idents_full(const Pattern* p);
std::pair<const Pattern*, const Pattern*> split_pattern(const Pattern* p);
Path::t path_of_module(const ModuleExpr* me);  // nullptr = None
const ModuleExpr* remove_module_constraint(const ModuleExpr* me);

}  // namespace cppcaml::typing::typedtree
