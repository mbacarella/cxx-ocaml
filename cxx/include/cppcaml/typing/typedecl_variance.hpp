// Port of typing/typedecl_variance.mli (cxx/PORTING.md, stage 5): variance
// inference and checking for type declarations, extensions and classes.
#pragma once

#include <tuple>
#include <vector>

#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typedecl_variance {

namespace tt = typedtree;

struct SurfaceVariance {  // bool * bool * bool
  bool co, cn, inj;
  bool operator==(const SurfaceVariance&) const = default;
};
using Req = std::vector<SurfaceVariance>;
using Prop = std::vector<variance::t>;

std::vector<SurfaceVariance> variance_of_params(Slice<parsetree::TypeParam> params);
std::vector<SurfaceVariance> variance_of_sdecl(const parsetree::TypeDeclaration* sdecl);

struct VarianceVariableContext {  // Type_declaration | Gadt_constructor | Extension_constructor
  enum class Kind { Type_declaration, Gadt_constructor, Extension_constructor };
  Kind kind;
  Ident::t id = nullptr;
  const TypeDeclaration* decl = nullptr;
  const ConstructorDeclaration* cd = nullptr;
  const ExtensionConstructor* ext = nullptr;
};
enum class VarianceVariableError { No_variable, Variance_not_reflected, Variance_not_deducible };
struct VarianceError {  // Variance_not_satisfied of int | Variance_variable_error of {..}
  bool not_satisfied;
  long n = 0;
  VarianceVariableError error = VarianceVariableError::No_variable;
  VarianceVariableContext context{};
  TypeExpr* variable = nullptr;
};
struct AnonymousVarianceError {  // Variable_constrained | Variable_instantiated of type_expr
  bool constrained;
  TypeExpr* ty;
};
struct Error : std::runtime_error {  // Bad_variance | Varying_anonymous
  enum class Kind { Bad_variance, Varying_anonymous };
  Location loc;
  Kind kind;
  VarianceError variance{};
  SurfaceVariance s1{}, s2{};
  long n = 0;
  AnonymousVarianceError anon{};
  Error(const Location& l, Kind k) : std::runtime_error("Typedecl_variance.Error"), loc(l), kind(k) {}
};

void check_variance_extension(env::t env, const TypeDeclaration* decl, const tt::TExtensionConstructor* ext,
                              const Req& req, const Location& loc);
Prop compute_decl(env::t env, Ident::t check, const TypeDeclaration* decl, const Req& req);
std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, Slice<const parsetree::TypeDeclaration*> sdecls,
    const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls);
struct ClassDeclInput {  // Ident.t * decl * class_declaration * class_type_declaration * 'a class_infos
  Ident::t id;
  const TypeDeclaration* decl;
  const ClassDeclaration* cl;
  const ClassTypeDeclaration* cltype;
  Slice<tt::TypeParam> ci_params;  // (the only field of the class_infos read)
  Location ci_loc;
};
struct ClassDeclOutput {
  const TypeDeclaration* decl;
  const ClassDeclaration* cl;
  const ClassTypeDeclaration* cltype;
};
std::vector<ClassDeclOutput> update_class_decls(env::t env, const std::vector<ClassDeclInput>& decls);

}  // namespace cppcaml::typing::typedecl_variance
