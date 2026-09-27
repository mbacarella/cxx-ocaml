// Port of typing/env.ml (TYPECHECKER.md): typing environments.
//
// Deviations, each marked at its site in env.cpp:
//  - no shapes (Shape.t only feeds .cmt / project-index output);
//  - warnings, alerts and usage tracking (Builtin_attributes, the
//    *_declarations usage tables, delayed checks) are not ported yet: they
//    never change whether a program type-checks;
//  - save_signature and the short-path iterators (iter_env, run_iter_cont)
//    come with the cmi-writing and printing stages.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/longident.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/subst.hpp"

namespace cppcaml::typing::env {

struct EnvT;
using t = const EnvT*;

// ---- reasons, summaries, addresses ------------------------------------------
struct ValueUnboundReason {
  enum class Kind : std::uint8_t {
    Val_unbound_instance_variable, Val_unbound_self, Val_unbound_ancestor,
    Val_unbound_ghost_recursive
  };
  Kind kind;
  Location ghost_loc;  // Val_unbound_ghost_recursive
};

struct ModuleUnboundReason {  // Mod_unbound_illegal_recursion {container; unbound}
  OptStr container;
  std::string_view unbound;
};

struct Summary {
  enum class Kind : std::uint8_t {
    Env_empty, Env_value, Env_type, Env_extension, Env_module, Env_modtype, Env_class,
    Env_cltype, Env_open, Env_not_aliasable, Env_constraints, Env_copy_types, Env_persistent,
    Env_value_unbound, Env_module_unbound
  };
  Kind kind;
  const Summary* next = nullptr;
  Ident::t id = nullptr;
  const ValueDescription* value = nullptr;
  const TypeDeclaration* type = nullptr;
  const ExtensionConstructor* ext = nullptr;
  ModulePresence presence = ModulePresence::Mp_present;
  const ModuleDeclaration* md = nullptr;
  const ModtypeDeclaration* mtd = nullptr;
  const ClassDeclaration* cls = nullptr;
  const ClassTypeDeclaration* clty = nullptr;
  Path::t path = nullptr;                                 // Env_open
  PathMap<const TypeDeclaration*> constraints;            // Env_constraints
  std::string_view name;                                  // *_unbound
  ValueUnboundReason value_reason{};                      // Env_value_unbound
  ModuleUnboundReason module_reason{};                    // Env_module_unbound
};

struct Address {  // Aident of Ident.t | Adot of address * int
  bool is_ident;
  Ident::t id = nullptr;
  const Address* parent = nullptr;
  long pos = 0;
};
struct AddressUnforced {  // Projection {parent; pos} | ModAlias {env; path}
  bool is_projection;
  LazyBacktrack<AddressUnforced, const Address*>* parent = nullptr;
  long pos = 0;
  t env = nullptr;
  Path::t path = nullptr;
};
using AddressLazy = LazyBacktrack<AddressUnforced, const Address*>;

// ---- the data stored in environments ---------------------------------------
struct ValueData {
  const ValueDescription* vda_description;
  AddressLazy* vda_address;
};
struct ValueEntry {  // Val_bound of value_data | Val_unbound of reason
  bool bound;
  const ValueData* data = nullptr;
  ValueUnboundReason reason{};
};

struct ConstructorData {
  const ConstructorDescription* cda_description;
  AddressLazy* cda_address;  // nullptr = None
};
using LabelData = const LabelDescription*;

// type_descr_kind = (label_description, constructor_description) type_kind
struct TypeDescriptions {
  TypeKind::Kind kind = TypeKind::Kind::Type_abstract;
  TypeOrigin origin;
  Slice<const LabelDescription*> labels;
  RecordRepresentation record_repr;
  Slice<const ConstructorDescription*> constructors;
  VariantRepresentation variant_repr = VariantRepresentation::Variant_regular;
  std::string_view external;
};

struct TypeData {
  const TypeDeclaration* tda_declaration;
  const TypeDescriptions* tda_descriptions;
};

struct ModuleComponents;
struct ModuleData {
  const subst::lazy::ModuleDecl* mda_declaration;
  ModuleComponents* mda_components;
  AddressLazy* mda_address;
};
struct ModuleEntry {  // Mod_local | Mod_persistent | Mod_unbound
  enum class Kind : std::uint8_t { Mod_local, Mod_persistent, Mod_unbound };
  Kind kind;
  const ModuleData* data = nullptr;
  ModuleUnboundReason reason{};
};
struct ModtypeData {
  const subst::lazy::ModtypeDecl* mtda_declaration;
};
struct ClassData {
  const ClassDeclaration* clda_declaration;
  AddressLazy* clda_address;
};
struct CltypeData {
  const ClassTypeDeclaration* cltda_declaration;
};

// ---- IdTbl / TycompTbl -------------------------------------------------------
// `using`: the open's callback (name, (shadowed, from-open) option).
template <class A>
using UsingFn = std::function<void(std::string_view, const std::pair<A, A>*)>;

template <class A, class B>
struct IdTbl {
  struct Layer;
  ident::Tbl<A> current;
  const Layer* layer = nullptr;  // nullptr = Nothing

  struct Layer {
    bool is_open;  // Open or Map
    Path::t root = nullptr;
    StrMap<B> components;
    const UsingFn<A>* using_ = nullptr;  // nullptr = None
    std::function<A(A)> f;               // Map
    IdTbl next;
  };
};

template <class A>
struct TycompTbl {
  struct Opened;
  ident::Tbl<A> current;
  const Opened* opened = nullptr;  // nullptr = None

  struct Opened {
    StrMap<Slice<A>> components;
    Path::t root;
    const UsingFn<A>* using_ = nullptr;
    TycompTbl next;
  };
};

// ---- module components ---------------------------------------------------
struct StructureComponents {
  StrMap<const ValueData*> comp_values;
  StrMap<Slice<const ConstructorData*>> comp_constrs;
  StrMap<Slice<LabelData>> comp_labels;
  StrMap<const TypeData*> comp_types;
  StrMap<const ModuleData*> comp_modules;
  StrMap<const ModtypeData*> comp_modtypes;
  StrMap<const ClassData*> comp_classes;
  StrMap<const CltypeData*> comp_cltypes;
};

struct PathLess {
  bool operator()(Path::t a, Path::t b) const { return path::compare(a, b) < 0; }
};

struct FunctorComponents {
  FunctorParameter fcomp_arg;
  const ModuleType* fcomp_res;
  std::map<Path::t, ModuleComponents*, PathLess> fcomp_cache;         // memoization
  std::map<Path::t, const ModuleType*, PathLess> fcomp_subst_cache;
};

struct ModuleComponentsRepr {  // Structure_comps | Functor_comps
  bool is_structure;
  StructureComponents* structure = nullptr;
  FunctorComponents* functor = nullptr;
};

struct ComponentsResult {  // (repr, No_components_abstract | No_components_alias p) result
  bool ok;
  const ModuleComponentsRepr* repr = nullptr;
  Path::t alias = nullptr;  // Error (No_components_alias p); nullptr = No_components_abstract
};

struct ComponentsMaker {
  t cm_env;
  subst::t cm_prefixing_subst;
  Path::t cm_path;
  AddressLazy* cm_addr;
  const subst::lazy::Modtype* cm_mty;
};

struct ModuleComponents {
  StrMap<std::string_view> alerts;
  Uid uid;
  LazyBacktrack<ComponentsMaker, ComponentsResult>* comps;
};

// ---- the environment ---------------------------------------------------------
inline constexpr int in_signature_flag = 0x01;

struct EnvT {
  IdTbl<const ValueEntry*, const ValueData*> values;
  TycompTbl<const ConstructorData*> constrs;
  TycompTbl<LabelData> labels;
  IdTbl<const TypeData*, const TypeData*> types;
  IdTbl<const ModuleEntry*, const ModuleData*> modules;
  IdTbl<const ModtypeData*, const ModtypeData*> modtypes;
  IdTbl<const ClassData*, const ClassData*> classes;
  IdTbl<const CltypeData*, const CltypeData*> cltypes;
  ident::Tbl<bool> not_aliasable;
  const Summary* summary;
  PathMap<const TypeDeclaration*> local_constraints;
  Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> id_pairs;
  int flags = 0;
};

// ---- errors -------------------------------------------------------------------
struct LookupError {
  enum class Kind : std::uint8_t {
    Unbound_value, Unbound_type, Unbound_constructor, Unbound_label, Unbound_module,
    Unbound_class, Unbound_modtype, Unbound_cltype, Unbound_instance_variable,
    Not_an_instance_variable, Masked_instance_variable, Masked_self_variable,
    Masked_ancestor_variable, Structure_used_as_functor, Abstract_used_as_functor,
    Functor_used_as_structure, Abstract_used_as_structure, Generative_used_as_applicative,
    Illegal_reference_to_recursive_module, Illegal_reference_to_recursive_class_type,
    Cannot_scrape_alias
  };
  Kind kind;
  Longident::t lid = nullptr;
  bool missing_rec = false;  // Unbound_value's hint
  Location hint_loc;
  std::string_view name;     // instance variables
  OptStr container;          // Illegal_reference_*
  std::string_view unbound;
  Longident::t unbound_class_type = nullptr;
  std::string_view container_class_type;
  Path::t alias = nullptr;   // Cannot_scrape_alias
};

// exception Error.In_context of error
struct Error : std::runtime_error {
  enum class Kind : std::uint8_t { Missing_module, Illegal_value_name, Lookup_error };
  Kind kind;
  Location loc;
  Path::t path1 = nullptr, path2 = nullptr;  // Missing_module
  std::string name;                          // Illegal_value_name
  t env = nullptr;                           // Lookup_error
  LookupError err{};
  explicit Error(Kind k) : std::runtime_error("Env.Error"), kind(k) {}
};

// `raise Not_found` of the find_* / non-error lookups
struct NotFound {};

// ---- forward references (set by later modules) --------------------------------
// Includemod
extern std::function<void(bool errors, const Location& loc, Longident::t lid_whole_app,
                          Path::t f0_path,
                          const std::vector<std::pair<Path::t, const ModuleType*>>& args,
                          Path::t arg_path, const ModuleType* arg_mty,
                          const ModuleType* param_mty, t env)>
    check_functor_application;
// Mtype.strengthen
extern std::function<const subst::lazy::Modtype*(bool aliasable, t env,
                                                  const subst::lazy::Modtype* mty, Path::t p)>
    strengthen;
// Ctype (checks whether two constructor result types are the same)
extern std::function<bool(t env, TypeExpr* a, TypeExpr* b)> same_constr;
// Typemod
extern std::function<void(t env, const Location& loc, const std::string& context,
                          const ModuleType* mty)>
    check_well_formed_module;

// ---- current unit ---------------------------------------------------------------
void set_current_unit(const UnitInfo& u);
const UnitInfo* get_current_unit();
std::string get_current_unit_name();

// ---- construction ---------------------------------------------------------------
t empty();
// env = Env.empty (structural equality, as `env <> Env.empty` tests it)
bool is_empty(t env);
t initial();  // Predef.build_initial_env (add_type ~check:false) (add_extension ..) empty
t in_signature(bool b, t env);
bool is_in_signature(t env);
bool has_local_constraints(t env);
void reset_cache();

// ---- lookups by path -------------------------------------------------------------
// (raise NotFound)
const ValueDescription* find_value(Path::t p, t env);
const TypeDeclaration* find_type(Path::t p, t env);
const TypeDescriptions* find_type_descrs(Path::t p, t env);
const ModuleDeclaration* find_module(Path::t p, t env);
const subst::lazy::ModuleDecl* find_module_lazy(Path::t p, t env);
const ModuleType* find_strengthened_module(bool aliasable, Path::t p, t env);
const ModtypeDeclaration* find_modtype(Path::t p, t env);
const subst::lazy::ModtypeDecl* find_modtype_lazy(Path::t p, t env);
const ClassDeclaration* find_class(Path::t p, t env);
const ClassTypeDeclaration* find_cltype(Path::t p, t env);
const ConstructorDescription* find_ident_constructor(Ident::t id, t env);
const LabelDescription* find_ident_label(Ident::t id, t env);
const ConstructorDescription* find_cstr(Path::t p, std::string_view name, t env);
const LabelDescription* find_label(Path::t p, std::string_view name, t env);
const TypeDeclaration* find_hash_type(Path::t p, t env);
const ConstructorData* find_extension_full(Path::t p, t env);

struct TypeExpansion {
  Slice<TypeExpr*> params;
  TypeExpr* body;
  long expansion_scope;
};
TypeExpansion find_type_expansion(Path::t p, t env);
TypeExpansion find_type_expansion_opt(Path::t p, t env);
const ModuleType* find_modtype_expansion(Path::t p, t env);
const subst::lazy::Modtype* find_modtype_expansion_lazy(Path::t p, t env);

const Address* find_value_address(Path::t p, t env);
const Address* find_module_address(Path::t p, t env);
const Address* find_class_address(Path::t p, t env);
const Address* find_constructor_address(Path::t p, t env);

bool is_aliasable(Path::t p, t env);

// normalization (raise Error Missing_module when a location is given)
Path::t normalize_module_path(const Location* oloc, t env, Path::t p);
Path::t normalize_type_path(const Location* oloc, t env, Path::t p);
Path::t normalize_value_path(const Location* oloc, t env, Path::t p);
Path::t normalize_modtype_path(t env, Path::t p);
Path::t try_normalize_type_path(const Location* oloc, t env, Path::t p);  // nullptr = None
Path::t try_normalize_modtype_path(t env, Path::t p);                     // nullptr = None

const ModuleType* scrape_alias(t env, const ModuleType* mty);
const subst::lazy::Modtype* scrape_alias_lazy(t env, const subst::lazy::Modtype* mty);

// required globals
void reset_required_globals();
std::vector<Ident::t> get_required_globals();
void add_required_global(Ident::t id);

// ---- insertion -------------------------------------------------------------------
t add_value(Ident::t id, const ValueDescription* desc, t env);
t add_type(bool check, Ident::t id, const TypeDeclaration* info, t env);
t add_extension(bool check, bool rebind, Ident::t id, const ExtensionConstructor* ext, t env);
t add_module_declaration(bool check, Ident::t id, ModulePresence presence,
                         const ModuleDeclaration* md, t env, bool noalias = false);
t add_module_declaration_lazy(bool update_summary, Ident::t id, ModulePresence presence,
                              const subst::lazy::ModuleDecl* md, t env);
t add_modtype(Ident::t id, const ModtypeDeclaration* info, t env);
t add_modtype_lazy(bool update_summary, Ident::t id, const subst::lazy::ModtypeDecl* info, t env);
t add_class(Ident::t id, const ClassDeclaration* ty, t env);
t add_cltype(Ident::t id, const ClassTypeDeclaration* ty, t env);
t add_module(Ident::t id, ModulePresence presence, const ModuleType* mty, t env,
             bool noalias = false);
t add_module_lazy(bool update_summary, Ident::t id, ModulePresence presence,
                  const subst::lazy::Modtype* mty, t env);
t add_local_constraint(Path::t path, const TypeDeclaration* info, t env);
t add_persistent_structure(Ident::t id, t env);
t add_signature(Signature sg, t env);
t mark_not_aliasable(Ident::t id, t env);

std::pair<Ident::t, t> enter_value(std::string_view name, const ValueDescription* desc, t env);
std::pair<Ident::t, t> enter_type(int scope, std::string_view name, const TypeDeclaration* info,
                                  t env);
t reenter_type(Ident::t id, const TypeDeclaration* info, t env);
std::pair<Ident::t, t> enter_extension(int scope, bool rebind, std::string_view name,
                                       const ExtensionConstructor* ext, t env);
std::pair<Ident::t, t> enter_module_declaration(int scope, std::string_view name,
                                                ModulePresence presence,
                                                const ModuleDeclaration* md, t env,
                                                bool noalias = false);
std::pair<Ident::t, t> enter_modtype(int scope, std::string_view name,
                                     const ModtypeDeclaration* mtd, t env);
std::pair<Ident::t, t> enter_class(int scope, std::string_view name,
                                   const ClassDeclaration* desc, t env);
std::pair<Ident::t, t> enter_cltype(int scope, std::string_view name,
                                    const ClassTypeDeclaration* desc, t env);
std::pair<Ident::t, t> enter_module(int scope, std::string_view name, ModulePresence presence,
                                    const ModuleType* mty, t env, bool noalias = false);
std::pair<Signature, t> enter_signature(int scope, Signature sg, t env);
t enter_unbound_value(std::string_view name, ValueUnboundReason reason, t env);
t enter_unbound_module(std::string_view name, ModuleUnboundReason reason, t env);

// open_signature: nullopt = `Not_found`, and throws on `Functor`... returned
// as the variant below
struct OpenResult {
  enum class Kind { Ok, Not_found, Functor };
  Kind kind;
  t env = nullptr;
};
OpenResult open_signature(OverrideFlag ovf, Path::t root, t env,
                          const Location& loc = location::none(), bool toplevel = false);
OpenResult open_pers_signature(std::string_view name, t env);
t remove_last_open(Path::t root, t env);  // nullptr = None

// ---- lookups by name (errors reported as Error) --------------------------------
Path::t lookup_module_path(bool use, const Location& loc, bool load, Longident::t lid, t env);
std::pair<Path::t, const ModuleDeclaration*> lookup_module(bool use, const Location& loc,
                                                           Longident::t lid, t env);
std::pair<Path::t, const ValueDescription*> lookup_value(bool use, const Location& loc,
                                                         Longident::t lid, t env);
std::pair<Path::t, const TypeDeclaration*> lookup_type(bool use, const Location& loc,
                                                       Longident::t lid, t env);
std::pair<Path::t, const ModtypeDeclaration*> lookup_modtype(bool use, const Location& loc,
                                                             Longident::t lid, t env);
Path::t lookup_modtype_path(bool use, const Location& loc, Longident::t lid, t env);
std::pair<Path::t, const ClassDeclaration*> lookup_class(bool use, const Location& loc,
                                                         Longident::t lid, t env);
std::pair<Path::t, const ClassTypeDeclaration*> lookup_cltype(bool use, const Location& loc,
                                                              Longident::t lid, t env);

enum class ConstructorUsage { Positive, Pattern, Exported_private, Exported };
enum class LabelUsage { Projection, Mutation, Construct, Exported_private, Exported };

// lookup_all_constructors: Ok list | Error (loc, env, lookup_error)
struct LookupAllCstrs {
  bool ok;
  std::vector<std::pair<const ConstructorDescription*, std::function<void()>>> cstrs;
  Location err_loc;
  t err_env = nullptr;
  LookupError err{};
};
LookupAllCstrs lookup_all_constructors(bool use, const Location& loc, ConstructorUsage usage,
                                       Longident::t lid, t env);
const ConstructorDescription* lookup_constructor(bool use, const Location& loc,
                                                 ConstructorUsage usage, Longident::t lid,
                                                 t env);
std::vector<std::pair<const ConstructorDescription*, std::function<void()>>>
lookup_all_constructors_from_type(bool use, const Location& loc, ConstructorUsage usage,
                                  Path::t ty_path, t env);

struct LookupAllLabels {
  bool ok;
  std::vector<std::pair<const LabelDescription*, std::function<void()>>> lbls;
  Location err_loc;
  t err_env = nullptr;
  LookupError err{};
};
LookupAllLabels lookup_all_labels(bool use, const Location& loc, LabelUsage usage,
                                  Longident::t lid, t env);
const LabelDescription* lookup_label(bool use, const Location& loc, LabelUsage usage,
                                     Longident::t lid, t env);
std::vector<std::pair<const LabelDescription*, std::function<void()>>>
lookup_all_labels_from_type(bool use, const Location& loc, LabelUsage usage, Path::t ty_path,
                            t env);

struct InstanceVariable {
  Path::t path;
  MutableFlag mut;
  std::string_view cl_num;
  TypeExpr* ty;
};
InstanceVariable lookup_instance_variable(bool use, const Location& loc, std::string_view name,
                                          t env);

// the find_*_by_name family: no marking, raise NotFound
std::pair<Path::t, const ModuleDeclaration*> find_module_by_name(Longident::t lid, t env);
std::pair<Path::t, const ValueDescription*> find_value_by_name(Longident::t lid, t env);
std::pair<Path::t, const TypeDeclaration*> find_type_by_name(Longident::t lid, t env);
std::pair<Path::t, const ModtypeDeclaration*> find_modtype_by_name(Longident::t lid, t env);
std::pair<Path::t, const ClassDeclaration*> find_class_by_name(Longident::t lid, t env);
std::pair<Path::t, const ClassTypeDeclaration*> find_cltype_by_name(Longident::t lid, t env);
const ConstructorDescription* find_constructor_by_name(Longident::t lid, t env);
const LabelDescription* find_label_by_name(Longident::t lid, t env);

bool bound_module(std::string_view name, t env);
bool bound_value(std::string_view name, t env);
bool bound_type(std::string_view name, t env);
bool bound_modtype(std::string_view name, t env);
bool bound_class(std::string_view name, t env);
bool bound_cltype(std::string_view name, t env);

// ---- folds (spellchecking) ------------------------------------------------------
void fold_values(const std::function<void(std::string_view, Path::t, const ValueDescription*)>& f,
                 Longident::t lid, t env);
void fold_types(const std::function<void(std::string_view, Path::t, const TypeDeclaration*)>& f,
                Longident::t lid, t env);
void fold_modules(const std::function<void(std::string_view, Path::t, const ModuleDeclaration*)>& f,
                  Longident::t lid, t env);
void fold_constructors(const std::function<void(const ConstructorDescription*)>& f,
                       Longident::t lid, t env);
void fold_labels(const std::function<void(const LabelDescription*)>& f, Longident::t lid, t env);
void fold_modtypes(const std::function<void(std::string_view, Path::t, const ModtypeDeclaration*)>& f,
                   Longident::t lid, t env);
void fold_classes(const std::function<void(std::string_view, Path::t, const ClassDeclaration*)>& f,
                  Longident::t lid, t env);
void fold_cltypes(const std::function<void(std::string_view, Path::t, const ClassTypeDeclaration*)>& f,
                  Longident::t lid, t env);

// ---- misc -------------------------------------------------------------------------
const Summary* summary(t env);
std::vector<Ident::t> diff(t env1, t env2);
// find_*_index id env: the ident's position among the bindings of its name
// (0 = the most recent), None when it is not bound
std::optional<long> find_value_index(Ident::t id, t env);
std::optional<long> find_type_index(Ident::t id, t env);
std::optional<long> find_module_index(Ident::t id, t env);
std::optional<long> find_modtype_index(Ident::t id, t env);
std::optional<long> find_class_index(Ident::t id, t env);
std::optional<long> find_cltype_index(Ident::t id, t env);
bool same_types(t env1, t env2);
bool same_type_declarations(t env1, t env2);
std::function<t(t)> make_copy_of_types(t env0);
t with_pairs(Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> id_pairs, t env);
Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> get_pairs(t env);
bool path_equiv(t env, Path::t p1, Path::t p2);

// persistent structures
Signature read_signature(const std::string& modname, const std::string& filename);
// save_signature ~alerts sg modname filename (Env.save_signature): substitute
// the signature for saving, write the .cmi and enter it in the persistent
// table; returns the cmi written.
cmi_format::CmiInfos save_signature(StrMap<std::string_view> alerts, Signature sg, const std::string& modname,
                                    const std::string& filename);
// save_signature_with_imports ~alerts sg cmi imports: the cmi's crcs are
// [imports] (-pack's packed interface)
cmi_format::CmiInfos save_signature_with_imports(
    StrMap<std::string_view> alerts, Signature sg, const std::string& modname, const std::string& filename,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& imports);
const ModuleData* find_pers_mod(bool allow_hidden, std::string_view name);
std::vector<std::pair<std::string, std::optional<std::string>>> imports();
// the string object Env.imports carries for an imported unit's name
std::string_view import_name(std::string_view name);
std::string crc_of_unit(const std::string& name);
bool is_imported_opaque(const std::string& modname);
void register_import_as_opaque(const std::string& modname);
// without_cmis f x: run f with cmi loading disabled (results are captured
// by the callback)
void without_cmis(const std::function<void()>& f);

// get_components on module components (for Mtype / Includemod)
const ModuleComponentsRepr* get_components(const ModuleComponents* c);
ComponentsResult get_components_res(const ModuleComponents* c);

}  // namespace cppcaml::typing::env
