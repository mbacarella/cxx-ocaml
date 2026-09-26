// Port of typing/typedecl_unboxed.ml and typing/typedecl_immediacy.ml, and
// Type_immediacy's coercion.
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/env.hpp"

namespace cppcaml::typing {

namespace typedecl_unboxed {
// nullptr = None
TypeExpr* get_unboxed_type_representation(env::t env, TypeExpr* ty);
}  // namespace typedecl_unboxed

namespace type_immediacy {
enum class Violation { Not_always_immediate, Not_always_immediate_on_64bits };
std::optional<Violation> coerce(TypeImmediacy t, TypeImmediacy as_);  // nullopt = Ok ()
TypeImmediacy of_attributes(const Attributes& attrs);
}  // namespace type_immediacy

namespace typedecl_immediacy {
struct Error : std::runtime_error {  // Bad_immediacy_attribute of Type_immediacy.Violation.t
  Location loc;
  type_immediacy::Violation violation;
  Error(const Location& l, type_immediacy::Violation v)
      : std::runtime_error("Typedecl_immediacy.Error"), loc(l), violation(v) {}
};
TypeImmediacy compute_decl(env::t env, const TypeDeclaration* tdecl);
std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls);
}  // namespace typedecl_immediacy

}  // namespace cppcaml::typing
