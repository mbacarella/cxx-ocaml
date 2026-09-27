// Port of typing/path.ml (TYPECHECKER.md).  Path.t is an immutable value; here
// a zone-allocated `Path` reached through `Path::t` (a const pointer).
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/ident.hpp"

namespace cppcaml::typing {

struct Path {
  enum class Kind : std::uint8_t { Pident, Pdot, Papply, Pextra_ty };
  // extra_ty = Pcstr_ty of string | Pext_ty
  enum class Extra : std::uint8_t { Pcstr_ty, Pext_ty };
  Kind kind;
  Ident::t id = nullptr;       // Pident
  const Path* p1 = nullptr;    // Pdot / Pextra_ty prefix, Papply functor
  const Path* p2 = nullptr;    // Papply argument
  std::string_view s;          // Pdot component, Pcstr_ty name
  Extra extra = Extra::Pext_ty;

  using t = const Path*;
  static t pident(Ident::t id);
  static t pdot(t p, std::string_view s);
  static t papply(t f, t a);
  static t pextra_ty(t p, Extra e, std::string_view s = {});
};

namespace path {

using t = Path::t;

bool same(t p1, t p2);
bool equiv(const std::vector<std::pair<Ident::t, Ident::t>>& id_pairs, t p1, t p2);
int compare(t p1, t p2);
std::optional<Ident::t> find_free_opt(const std::vector<Ident::t>& ids, t p);
bool exists_free(const std::vector<Ident::t>& ids, t p);
int scope(t p);
t subst(const std::vector<std::pair<Ident::t, t>>& id_map, t p);
bool contains_unscoped_ident(t p);
// Lexer.is_keyword (the default keyword set)
bool is_keyword(std::string_view s);
// path.ml `name` without ~paren (Lexer.is_keyword escaping included).
std::string name(t p);
Ident::t head(t p);
std::vector<Ident::t> heads(t p);
std::string last(t p);
t scrape_extra_ty(t p);
bool is_constructor_typath(t p);
// Ident.Unscoped.Set (ordered by stamp)
struct UnscopedLess {
  bool operator()(const ident::Unscoped* a, const ident::Unscoped* b) const {
    return ident::Unscoped::stamp_of(a) < ident::Unscoped::stamp_of(b);
  }
};
using UnscopedSet = std::set<ident::Unscoped*, UnscopedLess>;
// check_for_unbound_unscoped_idents idl p: the first unscoped ident of p not
// in idl (nullptr = None)
ident::Unscoped* check_for_unbound_unscoped_idents(const UnscopedSet& idl, t p);
// `flatten`: `Ok (id, components) or `Contains_apply (nullopt).
std::optional<std::pair<Ident::t, std::vector<std::string_view>>> flatten(t p);

}  // namespace path

// Path.Map (keys ordered by Path.compare)
struct PathCmp {
  int operator()(Path::t a, Path::t b) const { return path::compare(a, b); }
};
template <class V>
using PathMap = PMap<Path::t, V, PathCmp>;

}  // namespace cppcaml::typing
