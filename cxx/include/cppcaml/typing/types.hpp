// Port of typing/types.ml (TYPECHECKER.md): the representation of types and
// declarations, and the trail used for backtracking.
//
// Mapping from OCaml:
//  - `transient_expr` (= type_expr) is `TypeExpr`, mutable, zone-allocated;
//    `type_expr` values are `TypeExpr*`.
//  - `type_desc` is an immutable `TypeDesc` node, one struct per constructor
//    (switch on `kind`, then `as<Tarrow>(d)`).  Constant constructors (Tnil)
//    are singletons, so pointer equality is OCaml's physical equality.
//  - The mutable cells keep their identity: `Commutable` (Cvar cells),
//    `FieldKind` (FKvar cells), `RowFieldCell` (row-field ext), `MemoRef`
//    (abbrev_memo ref), `NameRef` (Tobject's name ref), `TyOptRef` (univar
//    pairs).  Constant constructors of those types are singletons.
//  - OCaml `option` of a pointer is a nullable pointer; lists are Slices.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/typing/ident.hpp"
#include "cppcaml/typing/path.hpp"
#include "cppcaml/typing/support.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

struct TypeExpr;
struct TypeDesc;
struct RowDesc;
struct RowField;
struct Package;

// ---- commutable ----------------------------------------------------------
// Cok | Cunknown (only under a Cvar) | Cvar {mutable commu}
struct Commutable {
  enum class Kind : std::uint8_t { Cok, Cunknown, Cvar };
  Kind kind;
  Commutable* commu = nullptr;  // Cvar
};

// ---- field_kind ------------------------------------------------------------
// FKvar {mutable field_kind} | FKprivate (only under FKvar) | FKpublic | FKabsent
struct FieldKind {
  enum class Kind : std::uint8_t { FKvar, FKprivate, FKpublic, FKabsent };
  Kind kind;
  FieldKind* field_kind = nullptr;  // FKvar
};

// ---- row_field ---------------------------------------------------------------
struct RowFieldCell {  // [`some | `none] row_field_gen ref
  const RowField* contents;
};
struct RowField {
  enum class Kind : std::uint8_t { RFpresent, RFeither, RFabsent, RFnone };
  Kind kind;
  TypeExpr* present = nullptr;  // RFpresent (None = nullptr)
  bool no_arg = false;          // RFeither
  Slice<TypeExpr*> arg_type;
  bool matched = false;
  RowFieldCell* ext = nullptr;
};

// ---- abbrev_memo ---------------------------------------------------------
struct MemoRef;
struct AbbrevMemo {
  enum class Kind : std::uint8_t { Mnil, Mcons, Mlink };
  Kind kind;
  PrivateFlag privacy = PrivateFlag::Public;  // Mcons
  Path::t path = nullptr;
  TypeExpr* abbreviation = nullptr;
  TypeExpr* expansion = nullptr;
  const AbbrevMemo* rem = nullptr;
  MemoRef* link = nullptr;  // Mlink
};
struct MemoRef {  // abbrev_memo ref
  const AbbrevMemo* contents;
};

// (Path.t * type_expr list), and its option ref (Tobject's name, row_name).
struct PathArgs {
  Path::t path;
  Slice<TypeExpr*> args;
};
struct NameRef {  // (Path.t * type_expr list) option ref
  const PathArgs* contents;  // nullptr = None
};
struct TyOptRef {  // type_expr option ref (Cuniv)
  TypeExpr* contents;
};

// ---- type_desc ---------------------------------------------------------------
enum class DescKind : std::uint8_t {
  Tvar, Tarrow, Ttuple, Tconstr, Tobject, Tfield, Tnil, Tvariant, Tunivar,
  Tpoly, Tpackage, Tfunctor, Texpand, Tlink, Tsubst
};

struct TypeDesc {
  DescKind kind;
};

struct Tvar : TypeDesc {
  static constexpr DescKind K = DescKind::Tvar;
  OptStr name;
};
struct Tarrow : TypeDesc {
  static constexpr DescKind K = DescKind::Tarrow;
  ArgLabel label;
  TypeExpr* t1;
  TypeExpr* t2;
  Commutable* commu;
};
struct LabeledTy {
  OptStr label;
  TypeExpr* ty;
};
struct Ttuple : TypeDesc {
  static constexpr DescKind K = DescKind::Ttuple;
  Slice<LabeledTy> elems;
};
struct Tconstr : TypeDesc {
  static constexpr DescKind K = DescKind::Tconstr;
  Path::t path;
  Slice<TypeExpr*> args;
  MemoRef* memo;
};
struct Tobject : TypeDesc {
  static constexpr DescKind K = DescKind::Tobject;
  TypeExpr* fields;
  NameRef* name;
};
struct Tfield : TypeDesc {
  static constexpr DescKind K = DescKind::Tfield;
  std::string_view label;
  FieldKind* kind_;
  TypeExpr* ty;
  TypeExpr* rest;
};
struct Tnil : TypeDesc {
  static constexpr DescKind K = DescKind::Tnil;
};
struct Tvariant : TypeDesc {
  static constexpr DescKind K = DescKind::Tvariant;
  const RowDesc* row;
};
struct Tunivar : TypeDesc {
  static constexpr DescKind K = DescKind::Tunivar;
  OptStr name;
};
struct Tpoly : TypeDesc {
  static constexpr DescKind K = DescKind::Tpoly;
  TypeExpr* body;
  Slice<TypeExpr*> vars;
};
struct Tpackage : TypeDesc {
  static constexpr DescKind K = DescKind::Tpackage;
  const Package* pack;
};
struct Tfunctor : TypeDesc {
  static constexpr DescKind K = DescKind::Tfunctor;
  ArgLabel label;
  ident::Unscoped* id;
  const Package* pack;
  TypeExpr* body;
};
struct Texpand : TypeDesc {
  static constexpr DescKind K = DescKind::Texpand;
  TypeExpr* ty;
  Path::t path;
  Slice<TypeExpr*> args;
};
struct Tlink : TypeDesc {
  static constexpr DescKind K = DescKind::Tlink;
  TypeExpr* ty;
};
struct Tsubst : TypeDesc {
  static constexpr DescKind K = DescKind::Tsubst;
  TypeExpr* ty;
  TypeExpr* row;  // nullptr = None
};

template <class T>
const T* as(const TypeDesc* d) {
  return d->kind == T::K ? static_cast<const T*>(d) : nullptr;
}

struct PackConstraint {
  Slice<std::string_view> path;
  TypeExpr* ty;
};
struct Package {
  Path::t pack_path;
  Slice<PackConstraint> pack_constraints;
};

struct FixedExplanation {
  enum class Kind : std::uint8_t { Univar, Fixed_private, Reified, Rigid };
  Kind kind;
  TypeExpr* univar = nullptr;  // Univar
  Path::t reified = nullptr;   // Reified
};

struct RowFieldEntry {
  std::string_view label;
  const RowField* field;
};
struct RowDesc {
  Slice<RowFieldEntry> row_fields;
  TypeExpr* row_more;
  bool row_closed;
  const FixedExplanation* row_fixed;  // nullptr = None
  const PathArgs* row_name;           // nullptr = None
};

// ---- transient_expr / type_expr ----------------------------------------
struct TypeExpr {
  const TypeDesc* desc;
  long level;
  long scope;  // scope_field: 27 bits of scope, marks above
  long id;
};
using type_expr = TypeExpr*;

// ---- Variance / Separability / immediacy ---------------------------------
namespace variance {
using t = long;
enum class F { May_pos, May_neg, May_weak, Inj, Pos, Neg, Inv };
long single(F f);
inline t union_(t a, t b) { return a | b; }
inline t inter(t a, t b) { return a & b; }
inline bool subset(t a, t b) { return (a & b) == a; }
inline bool eq(t a, t b) { return a == b; }
inline t set(F x, t v) { return union_(v, single(x)); }
inline t set_if(bool b, F x, t v) { return b ? set(x, v) : v; }
inline bool mem(F x, t v) { return subset(single(x), v); }
inline constexpr t null = 0;
inline constexpr t unknown = 7;
t full();
t covariant();
t contravariant();
t conjugate(t v);
t compose(t v1, t v2);
t strengthen(t v);
std::vector<t> unknown_signature(bool injective, long arity);
}  // namespace variance

enum class Separability : std::uint8_t { Ind, Sep, Deepsep };
enum class TypeImmediacy : std::uint8_t { Unknown, Always, Always_on_64bits };

// ---- declarations -------------------------------------------------------
struct LabelDeclaration {
  Ident::t ld_id;
  MutableFlag ld_mutable;
  AtomicFlag ld_atomic;
  TypeExpr* ld_type;
  Location ld_loc;
  Attributes ld_attributes;
  Uid ld_uid;
};

struct ConstructorArguments {
  enum class Kind : std::uint8_t { Cstr_tuple, Cstr_record };
  Kind kind = Kind::Cstr_tuple;
  Slice<TypeExpr*> tuple;
  Slice<const LabelDeclaration*> record;
};

struct ConstructorDeclaration {
  Ident::t cd_id;
  ConstructorArguments cd_args;
  TypeExpr* cd_res;  // nullptr = None
  Location cd_loc;
  Attributes cd_attributes;
  Uid cd_uid;
};

struct TypeOrigin {
  enum class Kind : std::uint8_t {
    Definition, Rec_check_regularity, Approx_recmod, Existential, Equation
  };
  Kind kind = Kind::Definition;
  std::string_view existential;
  TypeExpr* eq1 = nullptr;
  TypeExpr* eq2 = nullptr;
};

struct RecordRepresentation {
  enum class Kind : std::uint8_t {
    Record_regular, Record_float, Record_unboxed, Record_inlined, Record_extension
  };
  Kind kind = Kind::Record_regular;
  bool unboxed_inlined = false;  // Record_unboxed
  long inlined_tag = 0;          // Record_inlined
  Path::t extension = nullptr;   // Record_extension
};

enum class VariantRepresentation : std::uint8_t { Variant_regular, Variant_unboxed };

struct TypeKind {
  enum class Kind : std::uint8_t {
    Type_abstract, Type_record, Type_variant, Type_open, Type_external
  };
  Kind kind = Kind::Type_abstract;
  TypeOrigin origin;                                 // Type_abstract
  Slice<const LabelDeclaration*> labels;             // Type_record
  RecordRepresentation record_repr;
  Slice<const ConstructorDeclaration*> constructors; // Type_variant
  VariantRepresentation variant_repr = VariantRepresentation::Variant_regular;
  std::string_view external;                         // Type_external
};

struct TypeDeclaration {
  Slice<TypeExpr*> type_params;
  long type_arity;
  const TypeKind* type_kind;
  PrivateFlag type_private;
  TypeExpr* type_manifest;  // nullptr = None
  Slice<variance::t> type_variance;
  Slice<Separability> type_separability;
  bool type_is_newtype;
  long type_expansion_scope;
  Location type_loc;
  Attributes type_attributes;
  TypeImmediacy type_immediate;
  bool type_unboxed_default;
  Uid type_uid;
};

struct ExtensionConstructor {
  Path::t ext_type_path;
  Slice<TypeExpr*> ext_type_params;
  ConstructorArguments ext_args;
  TypeExpr* ext_ret_type;  // nullptr = None
  PrivateFlag ext_private;
  Location ext_loc;
  Attributes ext_attributes;
  Uid ext_uid;
};

enum class TypeTransparence : std::uint8_t { Type_public, Type_new, Type_private };

// ---- classes ------------------------------------------------------------
struct VarEntry {  // (mutable_flag * virtual_flag * type_expr)
  MutableFlag mut;
  VirtualFlag virt;
  TypeExpr* ty;
};
struct MethodPrivacy {  // Mpublic | Mprivate of field_kind
  bool is_private = false;
  FieldKind* kind = nullptr;
};
struct MethEntry {  // (method_privacy * virtual_flag * type_expr)
  MethodPrivacy priv;
  VirtualFlag virt;
  TypeExpr* ty;
};

struct ClassSignature {  // mutable fields as in types.ml
  TypeExpr* csig_self;
  TypeExpr* csig_self_row;
  FieldKind* csig_dummy_method;
  StrMap<VarEntry> csig_vars;
  StrMap<MethEntry> csig_meths;
};

struct ClassType {
  enum class Kind : std::uint8_t { Cty_constr, Cty_signature, Cty_arrow };
  Kind kind;
  Path::t path = nullptr;              // Cty_constr
  Slice<TypeExpr*> args;               // Cty_constr
  const ClassType* cty = nullptr;      // Cty_constr / Cty_arrow
  ClassSignature* sign = nullptr;      // Cty_signature
  ArgLabel label;                      // Cty_arrow
  TypeExpr* arg = nullptr;             // Cty_arrow
};

struct ClassDeclaration {
  Slice<TypeExpr*> cty_params;
  const ClassType* cty_type;  // mutable in types.ml
  Path::t cty_path;
  TypeExpr* cty_new;  // nullptr = None
  Slice<variance::t> cty_variance;
  Location cty_loc;
  Attributes cty_attributes;
  Uid cty_uid;
};

struct ClassTypeDeclaration {
  Slice<TypeExpr*> clty_params;
  const ClassType* clty_type;
  Path::t clty_path;
  const TypeDeclaration* clty_hash_type;
  Slice<variance::t> clty_variance;
  Location clty_loc;
  Attributes clty_attributes;
  Uid clty_uid;
};

// ---- values -------------------------------------------------------------
struct ValueKind {
  enum class Kind : std::uint8_t { Val_reg, Val_prim, Val_ivar, Val_self, Val_anc };
  Kind kind = Kind::Val_reg;
  const PrimitiveDescription* prim = nullptr;  // Val_prim
  MutableFlag ivar_mut = MutableFlag::Immutable;  // Val_ivar
  std::string_view ivar_name;                     // Val_ivar
  // Val_self / Val_anc never occur in a cmi; their payload is ported with
  // Typeclass.
};

struct ValueDescription {
  TypeExpr* val_type;
  ValueKind val_kind;
  Location val_loc;
  Attributes val_attributes;
  Uid val_uid;
};

// ---- modules ---------------------------------------------------------------
enum class Visibility : std::uint8_t { Exported, Hidden };
enum class ModulePresence : std::uint8_t { Mp_present, Mp_absent };
enum class RecStatus : std::uint8_t { Trec_not, Trec_first, Trec_next };
enum class ExtStatus : std::uint8_t { Text_first, Text_next, Text_exception };

struct SignatureItem;
using Signature = Slice<const SignatureItem*>;

struct ModuleType;
struct FunctorParameter {  // Unit | Named of Ident.t option * module_type
  bool is_unit = true;
  Ident::t id = nullptr;  // Named: nullptr = None
  const ModuleType* mty = nullptr;
};

struct ModuleType {
  enum class Kind : std::uint8_t { Mty_ident, Mty_signature, Mty_functor, Mty_alias };
  Kind kind;
  Path::t path = nullptr;          // Mty_ident / Mty_alias
  Signature sign;                  // Mty_signature
  FunctorParameter param;          // Mty_functor
  const ModuleType* res = nullptr; // Mty_functor
};

struct ModuleDeclaration {
  const ModuleType* md_type;
  Attributes md_attributes;
  Location md_loc;
  Uid md_uid;
};

struct ModtypeDeclaration {
  const ModuleType* mtd_type;  // nullptr = None (abstract)
  Attributes mtd_attributes;
  Location mtd_loc;
  Uid mtd_uid;
};

struct SignatureItem {
  enum class Kind : std::uint8_t {
    Sig_value, Sig_type, Sig_typext, Sig_module, Sig_modtype, Sig_class, Sig_class_type
  };
  Kind kind;
  Ident::t id;
  Visibility vis = Visibility::Exported;
  RecStatus rec = RecStatus::Trec_not;           // type/module/class/class_type
  const ValueDescription* value = nullptr;       // Sig_value
  const TypeDeclaration* type = nullptr;         // Sig_type
  const ExtensionConstructor* ext = nullptr;     // Sig_typext
  ExtStatus ext_status = ExtStatus::Text_first;  // Sig_typext
  ModulePresence presence = ModulePresence::Mp_present;  // Sig_module
  const ModuleDeclaration* md = nullptr;         // Sig_module
  const ModtypeDeclaration* mtd = nullptr;       // Sig_modtype
  const ClassDeclaration* cls = nullptr;         // Sig_class
  const ClassTypeDeclaration* clty = nullptr;    // Sig_class_type
};

// ===========================================================================
// Functions of types.ml
namespace types {

// Singletons for the constant constructors.
const TypeDesc* tnil();
Commutable* cok();
Commutable* cunknown();
FieldKind* fkprivate();
FieldKind* fkpublic();
FieldKind* fkabsent();
const RowField* rfabsent();
const RowField* rfnone();
const AbbrevMemo* mnil();

// Desc constructors (fresh immutable nodes).
const TypeDesc* tvar(OptStr name);
const TypeDesc* tarrow(ArgLabel l, TypeExpr* a, TypeExpr* b, Commutable* c);
const TypeDesc* ttuple(Slice<LabeledTy> l);
const TypeDesc* tconstr(Path::t p, Slice<TypeExpr*> args, MemoRef* memo);
const TypeDesc* tobject(TypeExpr* f, NameRef* nm);
const TypeDesc* tfield(std::string_view l, FieldKind* k, TypeExpr* a, TypeExpr* b);
const TypeDesc* tvariant(const RowDesc* row);
const TypeDesc* tunivar(OptStr name);
const TypeDesc* tpoly(TypeExpr* a, Slice<TypeExpr*> vars);
const TypeDesc* tpackage(const Package* p);
const TypeDesc* tfunctor(ArgLabel l, ident::Unscoped* id, const Package* p, TypeExpr* a);
const TypeDesc* texpand(TypeExpr* a, Path::t p, Slice<TypeExpr*> args);
const TypeDesc* tlink(TypeExpr* a);
const TypeDesc* tsubst(TypeExpr* a, TypeExpr* row);

// field_kind
enum class FieldKindView : std::uint8_t { Fprivate, Fpublic, Fabsent };
FieldKind* field_kind_internal_repr(FieldKind* fk);
FieldKindView field_kind_repr(FieldKind* fk);
FieldKind* field_public();
FieldKind* field_absent();
FieldKind* field_private();

// commutable
bool is_commu_ok(const Commutable* c);
Commutable* commu_ok();
Commutable* commu_var();

// representative
TypeExpr* repr(TypeExpr* t);
const TypeDesc* get_desc(TypeExpr* t);
long get_level(TypeExpr* t);
long get_scope(TypeExpr* t);
long get_id(TypeExpr* t);

// marks
inline constexpr long scope_mask = (1L << 27) - 1;
inline constexpr long marks_mask = ~scope_mask;
struct TypeMark;
void with_type_mark(const std::function<void(TypeMark&)>& f);
bool not_marked_node(TypeMark& mark, TypeExpr* t);
bool try_mark_node(TypeMark& mark, TypeExpr* t);

// kept abbreviations
const PathArgs* get_abbrev(TypeExpr* t);  // (path, args) option
void iter_abbrev(const std::function<void(Path::t, Slice<TypeExpr*>)>& f, TypeExpr* t);
TypeExpr* ignore_abbrev(TypeExpr* t);
void forget_abbrev(TypeExpr* t);

// Transient_expr
namespace transient_expr {
TypeExpr* create(const TypeDesc* desc, long level, long scope, long id);
void set_desc(TypeExpr* ty, const TypeDesc* d);
void set_stub_desc(TypeExpr* ty, const TypeDesc* d);
void set_level(TypeExpr* ty, long lv);
long get_scope(TypeExpr* ty);
long get_marks(TypeExpr* ty);
void set_scope(TypeExpr* ty, long sc);
inline TypeExpr* coerce(TypeExpr* ty) { return ty; }
}  // namespace transient_expr

TypeExpr* create_expr(const TypeDesc* desc, long level, long scope, long id);
TypeExpr* proto_newty3(long level, long scope, const TypeDesc* desc);

bool eq_type(TypeExpr* a, TypeExpr* b);
int compare_type(TypeExpr* a, TypeExpr* b);

// rows
const RowDesc* create_row(Slice<RowFieldEntry> fields, TypeExpr* more, bool closed,
                          const FixedExplanation* fixed, const PathArgs* name);
std::vector<RowFieldEntry> row_fields(const RowDesc* row);
TypeExpr* row_more(const RowDesc* row);
bool row_closed(const RowDesc* row);
const FixedExplanation* row_fixed(const RowDesc* row);
const PathArgs* row_name(const RowDesc* row);
const RowDesc* set_row_name(const RowDesc* row, const PathArgs* name);
const RowDesc* subst_row_name_path(const std::vector<std::pair<Ident::t, Path::t>>& id_map,
                                   const RowDesc* row);
const RowField* get_row_field(std::string_view tag, const RowDesc* row);
struct RowDescRepr {
  std::vector<RowFieldEntry> fields;
  TypeExpr* more;
  bool closed;
  const FixedExplanation* fixed;
  const PathArgs* name;
};
RowDescRepr row_repr(const RowDesc* row);

struct RowFieldView {  // Rpresent of type_expr option | Reither of .. | Rabsent
  enum class Kind : std::uint8_t { Rpresent, Reither, Rabsent };
  Kind kind;
  TypeExpr* present = nullptr;
  bool constant = false;
  std::vector<TypeExpr*> arg_types;
  bool matched = false;
};
RowFieldView row_field_repr(const RowField* fi);
const RowField* rf_present(TypeExpr* oty);
const RowField* rf_absent();
const RowField* rf_either(const RowField* use_ext_of, bool no_arg,
                          Slice<TypeExpr*> arg_type, bool matched);
const RowField* rf_either_of(TypeExpr* oty);
bool eq_row_field_ext(const RowField* a, const RowField* b);
bool changed_row_field_exts(const std::vector<const RowField*>& l,
                            const std::function<void()>& f);

// signature helpers
Visibility item_visibility(const SignatureItem* it);
std::vector<Ident::t> bound_value_identifiers(Signature sg);
Ident::t signature_item_id(const SignatureItem* it);

// ids
long& new_id();
void reset();

// ---- backtracking ---------------------------------------------------------
struct ChangesRef;
struct Snapshot {
  ChangesRef* changes;
  long old;
};
Snapshot snapshot();
void backtrack(const std::function<void()>& cleanup, Snapshot s);
void undo_first_change_after(Snapshot s);
void undo_compress(Snapshot s);

void link_expand(TypeExpr* ty, TypeExpr* ty2);
void link_type(TypeExpr* ty, TypeExpr* ty2);
void set_type_desc(TypeExpr* ty, const TypeDesc* td);
void set_level(TypeExpr* ty, long level);
void set_scope(TypeExpr* ty, long scope);
void set_name(NameRef* nm, const PathArgs* v);
void link_row_field_ext(const RowField* inside, const RowField* v);
void set_univar(TyOptRef* rty, TypeExpr* ty);
void link_kind(FieldKind* inside, FieldKind* k);
void link_commu(Commutable* inside, Commutable* c);
void set_commu_ok(Commutable* c);

}  // namespace types

}  // namespace cppcaml::typing
