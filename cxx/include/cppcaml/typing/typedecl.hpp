// Port of typing/typedecl.mli (TYPECHECKER.md, stage 5): typing of type
// declarations, type extensions, exceptions, value and primitive
// descriptions, and `with type` constraints.  Shapes (cmt-only) are not
// returned.  Errors are `typedecl::Error` (the OCaml Error.In_context).
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/typedecl_variance.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typedecl {

namespace tt = typedtree;
namespace pt = parsetree;
namespace et = errortrace;

enum class NativeReprKind { Unboxed, Untagged };
struct ReachingTypeStep {  // Expands_to | Contains | Parameter
  enum class Kind { Expands_to, Contains, Parameter, Considered_abstract };
  Kind kind;
  TypeExpr* t1 = nullptr;
  TypeExpr* t2 = nullptr;
  Path::t path = nullptr;  // Parameter / Considered_abstract
  long n = 0;
};
using ReachingTypePath = std::vector<ReachingTypeStep>;

struct Error : std::runtime_error {
  enum class Kind {
    Repeated_parameter, Duplicate_constructor, Too_many_constructors, Duplicate_label, Recursive_abbrev,
    Cycle_in_def, Definition_mismatch, Constraint_failed, Inconsistent_constraint, Type_clash, Non_regular,
    Null_arity_external, Missing_native_external, Unbound_type_var, Cannot_extend_private_type,
    Not_extensible_type, Extension_mismatch, Rebind_wrong_type, Rebind_mismatch, Rebind_private, Variance,
    Unavailable_type_constructor, Multiple_native_repr_attributes, Cannot_unbox_or_untag_type,
    Deep_unbox_or_untag_attribute, Type_cannot_be_external, Immediacy, Separability, Bad_unboxed_attribute,
    Boxed_and_unboxed, Nonrec_gadt, Invalid_private_row_declaration, Atomic_field_must_be_mutable,
    External_with_non_syntactic_arity, Primitive_alias_does_not_refer_to_primitive, Primitive_type_mismatch
  };
  Location loc;
  Kind kind;
  // payloads (which ones are set depends on the kind)
  std::string name;
  env::t env = nullptr;
  ReachingTypePath reaching_path;
  TypeExpr* ty = nullptr;
  TypeExpr* ty2 = nullptr;
  std::optional<includecore::TypeMismatch> mismatch;
  et::UnificationError trace;
  Path::t path = nullptr;
  Path::t path2 = nullptr;
  Longident::t lid = nullptr;
  std::vector<TypeExpr*> params;        // Unbound_type_var
  const TypeDeclaration* decl = nullptr;  // Unbound_type_var
  std::optional<typedecl_variance::Error> variance;
  NativeReprKind repr = NativeReprKind::Unboxed;
  ValueKind value_kind{};
  // Immediacy / Separability errors are recorded by kind only
  int sub = 0;
  Error(const Location& l, Kind k) : std::runtime_error("Typedecl.Error"), loc(l), kind(k) {}
};

// The declarations' shapes (transl_declaration's typ_shape, an extension
// constructor's): given to Env with the declarations, returned to Typemod
// (the same objects)
shape::ItemMap shape_map_labels(Slice<const tt::TLabelDeclaration*> lds);
shape::ItemMap shape_map_cstrs(Slice<const tt::TConstructorDeclaration*> cds);

struct TranslTypeDeclResult {
  std::vector<const tt::TTypeDeclaration*> decls;
  env::t env;
  std::vector<shape::t> shapes;
};
TranslTypeDeclResult transl_type_decl(env::t env, RecFlag rec_flag, Slice<const pt::TypeDeclaration*> sdecl_list);
struct TranslException {
  const tt::TExtensionConstructor* ext;
  env::t env;
  shape::t shape;
};
TranslException transl_exception(env::t env, const pt::ExtensionConstructor* sext);
struct TranslTypeException {
  const tt::TTypeException* tyexn;
  env::t env;
  shape::t shape;
};
TranslTypeException transl_type_exception(env::t env, const pt::TypeException* t);
struct TranslTypeExtension {
  const tt::TTypeExtension* tyext;
  env::t env;
  std::vector<shape::t> shapes;
};
TranslTypeExtension transl_type_extension(bool extend, env::t env, const Location& loc,
                                          const pt::TypeExtension* styext);
std::pair<const tt::TValueDescription*, env::t> transl_value_decl(env::t env, const Location& loc,
                                                                  const pt::ValueDescription* valdecl);
std::pair<const tt::TPrimitiveDescription*, env::t> transl_prim_desc(env::t env, const Location& loc,
                                                                     const pt::PrimitiveDescription* primdecl);
// the parsetree type_declaration should satisfy [is_fixed_type] when
// fixed_row_path is set
const tt::TTypeDeclaration* transl_with_constraint(Ident::t id, Path::t fixed_row_path, env::t sig_env,
                                                   const TypeDeclaration* sig_decl, env::t outer_env,
                                                   const pt::TypeDeclaration* sdecl);
const TypeDeclaration* transl_package_constraint(const Location& loc, env::t env, TypeExpr* ty);
const TypeDeclaration* abstract_type_decl(bool injective, TypeOrigin explanation, long arity);
std::vector<std::pair<Ident::t, const TypeDeclaration*>> approx_type_decl(TypeOrigin explanation,
                                                                          Slice<const pt::TypeDeclaration*> sdecls);
void check_well_founded_decl(env::t abs_env, env::t final_env, const std::function<bool(Path::t)>& is_decl_path,
                             const Location& loc, Path::t path, const TypeDeclaration* decl);
void check_recmod_typedecl(env::t abs_env, env::t env, const Location& loc, const std::vector<Ident::t>& recmod_ids,
                           Path::t path, const TypeDeclaration* decl);
void check_coherence(env::t env, const Location& loc, Path::t path, const TypeDeclaration* decl);
bool is_fixed_type(const pt::TypeDeclaration* sd);

}  // namespace cppcaml::typing::typedecl
