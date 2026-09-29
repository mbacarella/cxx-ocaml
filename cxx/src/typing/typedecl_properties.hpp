// Port of typing/typedecl_properties.ml: fixpoint computation of a
// property (variance, immediacy, separability) over a group of mutually
// recursive type declarations.
#pragma once

#include "cppcaml/typing/builtin_attributes.hpp"
#include <functional>
#include <vector>

#include "cppcaml/typing/env.hpp"

namespace cppcaml::typing::typedecl_properties {

using Decl = const TypeDeclaration*;
using Decls = std::vector<std::pair<Ident::t, Decl>>;

template <class Prop, class Req>
struct Property {
  std::function<bool(const Prop&, const Prop&)> eq;
  std::function<Prop(const Prop& prop, const Prop& new_prop)> merge;
  std::function<Prop(Decl)> default_;
  std::function<Prop(env::t, Decl, const Req&)> compute;
  std::function<Decl(Decl, const Prop&)> update_decl;
  std::function<void(env::t, Ident::t, Decl, const Req&)> check;
};

inline env::t add_type(bool check, Ident::t id, Decl decl, env::t env) {
  return builtin_attributes::warning_scope(
      builtin_attributes::ast_attributes(decl->type_attributes), [&] { return env::add_type(check, id, decl, env); },
      false);
}
inline env::t add_types_to_env(const Decls& decls, env::t env) {
  // List.fold_right
  for (std::size_t k = decls.size(); k-- > 0;)
    env = typedecl_properties::add_type(true, decls[k].first, decls[k].second, env);
  return env;
}

// [decls] and [required] must be lists of the same size, with [required]
// containing the requirement for the corresponding declaration in [decls].
template <class Prop, class Req>
Decls compute_property(const Property<Prop, Req>& property, env::t env, const Decls& decls,
                       const std::vector<Req>& required) {
  if (decls.size() != required.size()) throw std::invalid_argument("List.combine");
  std::vector<Prop> props;
  for (auto& d : decls) props.push_back(property.default_(d.second));
  for (;;) {
    Decls new_decls;
    for (std::size_t k = 0; k < decls.size(); ++k)
      new_decls.push_back({decls[k].first, property.update_decl(decls[k].second, props[k])});
    env::t new_env = add_types_to_env(new_decls, env);
    std::vector<Prop> new_props;
    for (std::size_t k = 0; k < new_decls.size(); ++k) {
      Prop new_prop = property.compute(new_env, new_decls[k].second, required[k]);
      new_props.push_back(property.merge(props[k], new_prop));
    }
    bool all_eq = true;
    for (std::size_t k = 0; k < props.size(); ++k)
      if (!property.eq(props[k], new_props[k])) {
        all_eq = false;
        break;
      }
    if (!all_eq) {
      props = new_props;
      continue;
    }
    for (std::size_t k = 0; k < new_decls.size(); ++k)
      property.check(new_env, new_decls[k].first, new_decls[k].second, required[k]);
    return new_decls;
  }
}

struct Unit {
  bool operator==(const Unit&) const = default;
};
template <class Prop>
Decls compute_property_noreq(const Property<Prop, Unit>& property, env::t env, const Decls& decls) {
  return compute_property(property, env, decls, std::vector<Unit>(decls.size()));
}

}  // namespace cppcaml::typing::typedecl_properties
