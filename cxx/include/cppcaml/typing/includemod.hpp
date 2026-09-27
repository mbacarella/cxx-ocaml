// Port of typing/includemod.ml (TYPECHECKER.md, stage 5): inclusion between
// module types, signatures and compilation units, and the coercions they
// compute.  Errors mirror includemod.mli's Error module as data; the
// Diffing-based functor diffs (Functor_inclusion_diff, Functor_app_diff),
// print_coercion and the Check module only serve error printing / merlin
// and are not ported.  Shapes are computed as includemod.ml does (they
// create identifiers).
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/diffing.hpp"
#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/shape.hpp"
#include "cppcaml/typing/subst.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::includemod {

namespace tt = typedtree;

// ---- Error ---------------------------------------------------------------------------------
namespace error {

struct FunctorArgDescr {  // Anonymous | Named of Path.t | Unit | Empty_struct
  enum class Kind { Anonymous, Named, Unit, Empty_struct };
  Kind kind;
  Path::t path = nullptr;
};

struct CoreSigitemSymptom {
  enum class Kind {
    Value_descriptions, Type_declarations, Extension_constructors, Class_type_declarations, Class_declarations
  };
  Kind kind;
  // got / expected
  const ValueDescription* vd1 = nullptr;
  const ValueDescription* vd2 = nullptr;
  const TypeDeclaration* td1 = nullptr;
  const TypeDeclaration* td2 = nullptr;
  const ExtensionConstructor* ext1 = nullptr;
  const ExtensionConstructor* ext2 = nullptr;
  const ClassTypeDeclaration* clty1 = nullptr;
  const ClassTypeDeclaration* clty2 = nullptr;
  const ClassDeclaration* cl1 = nullptr;
  const ClassDeclaration* cl2 = nullptr;
  // symptom
  std::optional<includecore::ValueMismatch> value;
  std::optional<includecore::TypeMismatch> type;
  std::optional<includecore::ExtensionConstructorMismatch> ext;
  std::vector<ctype::ClassMatchFailure> classes;
};

struct CoreModuleTypeSymptom {
  enum class Kind { Not_an_alias, Not_an_identifier, Incompatible_aliases, Abstract_module_type, Unbound_module_path };
  Kind kind;
  Path::t path = nullptr;  // Unbound_module_path
};

struct ModuleTypeSymptom;
struct SignatureSymptom;
struct ModuleTypeDiff {  // (module_type, module_type_symptom) diff
  const ModuleType* got;
  const ModuleType* expected;
  std::shared_ptr<const ModuleTypeSymptom> symptom;
};
struct FunctorParamsInfo {
  std::vector<FunctorParameter> params;
  const ModuleType* res;
};
// ('arg, 'path) functor_param_symptom
struct FunctorParamSymptom {
  enum class Kind { Incompatible_params, Mismatch };
  Kind kind;
  FunctorParameter arg{};  // Incompatible_params (arg side)
  FunctorArgDescr arg_descr{FunctorArgDescr::Kind::Anonymous};  // Functor_app_diff's Incompatible_params
  FunctorParameter param{};
  std::shared_ptr<const ModuleTypeDiff> mismatch;
};
struct ModuleTypeSymptom {
  enum class Kind { Mt_core, Signature, Functor_params, Functor_result, After_alias_expansion };
  Kind kind;
  CoreModuleTypeSymptom core{};
  std::shared_ptr<const SignatureSymptom> sig;
  FunctorParamsInfo got{}, expected{};              // Functor (Params)
  std::shared_ptr<const ModuleTypeDiff> diff;       // Functor (Result) / After_alias_expansion
};
struct ModuleTypeDeclarationSymptom {
  enum class Kind { Illegal_permutation, Not_greater_than, Not_less_than, Incomparable };
  Kind kind;
  const tt::ModuleCoercion* coercion = nullptr;
  std::shared_ptr<const ModuleTypeDiff> diff;          // Not_greater_than / Not_less_than
  std::shared_ptr<const ModuleTypeDiff> less_than, greater_than;
};
struct SigitemSymptom {
  enum class Kind { Core, Module_type_declaration, Module_type };
  Kind kind;
  CoreSigitemSymptom core{};
  const ModtypeDeclaration* mtd1 = nullptr;  // Module_type_declaration got / expected
  const ModtypeDeclaration* mtd2 = nullptr;
  std::optional<ModuleTypeDeclarationSymptom> mtd_symptom;
  std::shared_ptr<const ModuleTypeDiff> diff;  // Module_type
};
struct Untypable {
  const SignatureItem* item1;
  const SignatureItem* item2;
  long pos;
};
struct SignatureSymptom {
  env::t env;
  subst::t subst;
  Signature sig1, sig2;
  std::vector<const SignatureItem*> missings;
  std::vector<std::pair<const SignatureItem*, SigitemSymptom>> incompatibles;
  std::vector<tt::PosCoercion> oks;
  std::vector<const SignatureItem*> additions;
  std::vector<Untypable> untypables;
};
struct All {
  enum class Kind {
    In_Compilation_unit, In_Signature, In_Module_type, In_Module_type_substitution, In_Type_declaration,
    In_Expansion
  };
  Kind kind;
  std::string got_name, expected_name;           // In_Compilation_unit
  std::shared_ptr<const SignatureSymptom> sig;   // In_Compilation_unit / In_Signature
  std::shared_ptr<const ModuleTypeDiff> diff;    // In_Module_type
  Ident::t id = nullptr;                          // In_Module_type_substitution / In_Type_declaration
  const ModuleType* mty1 = nullptr;              // In_Module_type_substitution
  const ModuleType* mty2 = nullptr;
  std::optional<ModuleTypeDeclarationSymptom> mtd_symptom;
  std::optional<CoreSigitemSymptom> core;        // In_Type_declaration
  CoreModuleTypeSymptom expansion{};             // In_Expansion
};

}  // namespace error

// type explanation = Env.t * Error.all; exception Error of explanation
struct Explanation {
  env::t env;
  error::All all;
};
struct Error : std::runtime_error {
  Explanation expl;
  explicit Error(Explanation e) : std::runtime_error("Includemod.Error"), expl(std::move(e)) {}
};

enum class ApplicationNameKind { Anonymous_functor, Full_application_path, Named_leftmost_functor };
struct ApplicationName {
  ApplicationNameKind kind;
  Longident::t lid = nullptr;
};
struct ApplyError : std::runtime_error {
  Location loc;
  env::t env;
  ApplicationName app_name;
  const ModuleType* mty_f;
  std::vector<std::pair<error::FunctorArgDescr, const ModuleType*>> args;
  ApplyError() : std::runtime_error("Includemod.Apply_error") {}
};

enum class FieldKind {
  Field_value, Field_type, Field_exception, Field_typext, Field_module, Field_modtype, Field_class,
  Field_classtype
};
struct FieldDesc {
  std::string_view name;
  FieldKind kind;
};
struct FieldDescLess {  // Stdlib.compare on {name; kind}
  bool operator()(const FieldDesc& a, const FieldDesc& b) const {
    int c = a.name.compare(b.name);
    if (c != 0) return c < 0;
    return a.kind < b.kind;
  }
};
std::string_view kind_of_field_desc(const FieldDesc& fd);
FieldDesc field_desc(FieldKind kind, Ident::t id);
struct ItemIdentName {
  Ident::t id;
  Location loc;
  FieldDesc desc;
};
ItemIdentName item_ident_name(const SignatureItem* item);
subst::t item_subst(Ident::t id, const SignatureItem* item, subst::t s);
bool is_runtime_component(const SignatureItem* item);

const tt::ModuleCoercion* modtypes(const Location& loc, env::t env, bool mark, const ModuleType* mty1,
                                   const ModuleType* mty2);
void modtypes_consistency(const Location& loc, env::t env, const ModuleType* mty1, const ModuleType* mty2);
std::pair<const tt::ModuleCoercion*, shape::t> modtypes_constraint(shape::t shape, const Location& loc, env::t env,
                                                                  bool mark, const ModuleType* mty1,
                                                                  const ModuleType* mty2);
const tt::ModuleCoercion* strengthened_module_decl(const Location& loc, bool aliasable, env::t env, bool mark,
                                                   const ModuleDeclaration* md1, Path::t path1,
                                                   const ModuleDeclaration* md2);
std::optional<Explanation> check_modtype_inclusion(const Location& loc, env::t env, const ModuleType* mty1,
                                                   Path::t path1, const ModuleType* mty2);
void check_modtype_equiv(const Location& loc, env::t env, Ident::t id, const ModuleType* mty1,
                         const ModuleType* mty2);
const tt::ModuleCoercion* signatures(env::t env, subst::t subst, bool mark, Signature sig1, Signature sig2);
inline const tt::ModuleCoercion* signatures(env::t env, bool mark, Signature sig1, Signature sig2) {
  return signatures(env, subst::identity(), mark, sig1, sig2);
}
void check_implementation(env::t env, Signature impl, Signature intf);
std::pair<const tt::ModuleCoercion*, shape::t> compunit(env::t env, bool mark, std::string_view impl_name,
                                                       Signature impl_sig, std::string_view intf_name,
                                                       Signature intf_sig, shape::t unit_shape);
void type_declarations(const Location& loc, env::t env, bool mark, Ident::t id, const TypeDeclaration* decl1,
                       const TypeDeclaration* decl2);
const ModuleType* expand_module_alias(bool strengthen, env::t env, Path::t path);

// Installs Env.check_functor_application (includemod.ml's toplevel `let ()`).
void install_forward_refs();

// ---- Check: the compatibility tests of Signature_matching ----
namespace check {
bool module_types(env::t env, subst::t s, const ModtypeDeclaration* mt1, const ModtypeDeclaration* mt2);
bool modules(env::t env, subst::t s, const ModuleDeclaration* m1, const ModuleDeclaration* m2);
bool values(env::t env, subst::t s, const ValueDescription* v1, const ValueDescription* v2);
bool types(env::t env, subst::t s, const TypeDeclaration* t1, const TypeDeclaration* t2);
bool classes(env::t env, subst::t s, const ClassDeclaration* c1, const ClassDeclaration* c2);
bool class_types(env::t env, subst::t s, const ClassTypeDeclaration* c1, const ClassTypeDeclaration* c2);
bool extensions(env::t env, subst::t s, const ExtensionConstructor* e1, const ExtensionConstructor* e2);
}  // namespace check

// ---- the functor diffs (Functor_inclusion_diff / Functor_app_diff) ----
struct InclusionEnv {  // inclusion_env = { i_env; i_subst }
  env::t i_env;
  subst::t i_subst;
};
struct FunctorDiffState {
  const ModuleType* res;  // option
  env::t env;
  subst::t subst;
};
using InclusionChange =
    diffing::Change<FunctorParameter, FunctorParameter, const tt::ModuleCoercion*, error::FunctorParamSymptom>;
std::vector<InclusionChange> functor_inclusion_diff(const InclusionEnv& ie,
                                                    const std::vector<FunctorParameter>& params1,
                                                    const ModuleType* res1,
                                                    const std::vector<FunctorParameter>& params2);
using AppArg = std::pair<error::FunctorArgDescr, const ModuleType*>;
using AppChange = diffing::Change<AppArg, FunctorParameter, const tt::ModuleCoercion*, error::FunctorParamSymptom>;
std::vector<AppChange> functor_app_diff(env::t env, const ModuleType* f, const std::vector<AppArg>& args);
// retrieve_functor_params env mty
error::FunctorParamsInfo retrieve_functor_params_(env::t env, const ModuleType* mty);

}  // namespace cppcaml::typing::includemod
