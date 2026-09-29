// Port of typing/typedecl_separability.mli: the separability signature of
// type declarations (float-array unboxing safety).
#pragma once

#include <vector>

#include "cppcaml/typing/env.hpp"

namespace cppcaml::typing::typedecl_separability {

struct Error : std::runtime_error {  // Non_separable_evar of string option
  Location loc;
  OptStr evar;
  Error(const Location& l, OptStr e) : std::runtime_error("Typedecl_separability.Error"), loc(l), evar(e) {}
};
std::vector<TypeExpr*> immediate_subtypes(TypeExpr* ty);
std::vector<Separability> compute_decl(env::t env, const TypeDeclaration* decl);
std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls);

}  // namespace cppcaml::typing::typedecl_separability
