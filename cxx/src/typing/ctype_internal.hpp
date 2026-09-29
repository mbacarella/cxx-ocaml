// Helpers shared by the ctype_*.cpp parts of the typing/ctype.ml port that
// ctype.mli does not export.
#pragma once

#include "cppcaml/typing/ctype.hpp"

namespace cppcaml::typing::ctype::internal {

using EK = et::Elt<TypeExpr*>::Kind;

inline et::Elt<TypeExpr*> elt(EK k) { return et::Elt<TypeExpr*>::mk(k); }
inline et::Elt<TypeExpr*> escape_elt(const et::Escape<TypeExpr*>& e) {
  auto x = elt(EK::Escape);
  x.escape = e;
  return x;
}
inline et::Elt<TypeExpr*> diff_elt(TypeExpr* got, TypeExpr* expected) {
  auto x = elt(EK::Diff);
  x.diff = {got, expected};
  return x;
}
inline et::Elt<TypeExpr*> variant_elt(et::Variant v) {
  auto x = elt(EK::Variant);
  x.variant = std::move(v);
  return x;
}
inline et::Elt<TypeExpr*> obj_elt(et::Obj o) {
  auto x = elt(EK::Obj);
  x.obj = o;
  return x;
}
// Errortrace.incompatible_fields
inline et::Elt<TypeExpr*> incompatible_fields(std::string_view name, TypeExpr* got,
                                              TypeExpr* expected) {
  auto x = elt(EK::Incompatible_fields);
  x.field_name = name;
  x.field_diff = {got, expected};
  return x;
}
inline et::TypeTrace cons(et::Elt<TypeExpr*> e, et::TypeTrace t) {
  t.insert(t.begin(), std::move(e));
  return t;
}

void unify_univar_for(TraceExn tr_exn, TypeExpr* t1, TypeExpr* t2,
                      const std::vector<UnivarPair>& pairs);
void occur_univar_or_unscoped_for(TraceExn tr_exn, env::t env, TypeExpr* ty);
void eq_labels(TraceExn error_mode, bool in_pattern_mode, const ArgLabel& l1, const ArgLabel& l2);
// raises env::NotFound (Not_found) rather than Unify if the module types
// are incompatible
PackageSubtypeResult compare_package(env::t env,
                                     const std::function<void(Slice<TypeExpr*>, Slice<TypeExpr*>)>& unify_list_f,
                                     long lv1, const Package* pack1, long lv2,
                                     const Package* pack2);
et::UnificationError expand_to_unification_error(env::t env, const et::TypeTrace& trace);
void unify_kind(FieldKind* k1, FieldKind* k2);

// raises env::NotFound if impossible
std::vector<PackConstraint> complete_type_list(env::t env, Slice<PackConstraint> fl1, long lv2,
                                               const Package* pack2, bool allow_absent = false);
using IdPairList = std::vector<std::pair<ident::Unscoped*, ident::Unscoped*>>;
void enter_functor(env::t env, ident::Unscoped* id1, TypeExpr* t1, ident::Unscoped* id2,
                   TypeExpr* t2, const std::function<void(IdPairList)>& f);
const TypeKind* map_kind(const std::function<TypeExpr*(TypeExpr*)>& f, const TypeKind* k);

}  // namespace cppcaml::typing::ctype::internal
