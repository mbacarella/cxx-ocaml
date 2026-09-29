// Port of typing/out_type.ml (cxx/PORTING.md stage 9).
#include "cppcaml/typing/utf8_lexeme.hpp"
#include "cppcaml/typing/out_type.hpp"

#include "cppcaml/typing/location.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/ident.hpp"
#include "cppcaml/typing/longident.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/signature_group.hpp"
#include "cppcaml/typing/typedecl_unboxed.hpp"

namespace cppcaml::typing::out_type {

using namespace format_doc;
using OT = ot::OutType::K;
namespace bt = btype;
using types::FieldKindView;
using types::RowDescRepr;
using types::RowFieldView;

std::string_view namespace_to_string(Namespace n) {
  switch (n) {
    case Namespace::Value: return "value";
    case Namespace::Type: return "type";
    case Namespace::Constructor: return "constructor";
    case Namespace::Label: return "label";
    case Namespace::Module: return "module";
    case Namespace::Module_type: return "module type";
    case Namespace::Extension_constructor: return "extension constructor";
    case Namespace::Class: return "class";
    case Namespace::Class_type: return "class type";
  }
  return "";
}

namespace {

// ---- the printing environment ----
env::t g_printing_env = nullptr;  // Env.empty until first set

env::t cur_env() {
  if (!g_printing_env) g_printing_env = env::empty();
  return g_printing_env;
}

// in_printing_env f: Env.without_cmis f !printing_env
template <class F>
auto in_printing_env(F&& f) -> decltype(f(cur_env())) {
  using R = decltype(f(cur_env()));
  std::optional<R> r;
  env::t e = cur_env();
  env::without_cmis([&] { r.emplace(f(e)); });
  return std::move(*r);
}

// ---- Namespace ----
int namespace_id(Namespace n) {
  switch (n) {
    case Namespace::Type: return 0;
    case Namespace::Module: return 1;
    case Namespace::Module_type: return 2;
    case Namespace::Class: return 3;
    case Namespace::Class_type: return 4;
    default: return 5;
  }
}

// Namespace.lookup ns lid: the path the name resolves to (throws env::NotFound)
Path::t namespace_lookup(std::optional<Namespace> ns, std::string_view name) {
  Longident::t lid = Longident::lident(name);
  if (!ns) throw env::NotFound{};
  switch (*ns) {
    case Namespace::Type: return in_printing_env([&](env::t e) { return env::find_type_by_name(lid, e).first; });
    case Namespace::Module: return in_printing_env([&](env::t e) { return env::find_module_by_name(lid, e).first; });
    case Namespace::Module_type:
      return in_printing_env([&](env::t e) { return env::find_modtype_by_name(lid, e).first; });
    case Namespace::Class: return in_printing_env([&](env::t e) { return env::find_class_by_name(lid, e).first; });
    case Namespace::Class_type:
      return in_printing_env([&](env::t e) { return env::find_cltype_by_name(lid, e).first; });
    default: throw env::NotFound{};
  }
}

// Namespace.location ns id
std::optional<Location> namespace_location(std::optional<Namespace> ns, Ident::t id) {
  Path::t path = Path::pident(id);
  try {
    if (!ns) return location::none();
    switch (*ns) {
      case Namespace::Type: return in_printing_env([&](env::t e) { return env::find_type(path, e)->type_loc; });
      case Namespace::Module: return in_printing_env([&](env::t e) { return env::find_module(path, e)->md_loc; });
      case Namespace::Module_type:
        return in_printing_env([&](env::t e) { return env::find_modtype(path, e)->mtd_loc; });
      case Namespace::Class: return in_printing_env([&](env::t e) { return env::find_class(path, e)->cty_loc; });
      case Namespace::Class_type:
        return in_printing_env([&](env::t e) { return env::find_cltype(path, e)->clty_loc; });
      default: return location::none();
    }
  } catch (const env::NotFound&) {
    return std::nullopt;
  }
}

std::optional<Namespace> best_class_namespace(Path::t p) {
  switch (p->kind) {
    case Path::Kind::Papply:
    case Path::Kind::Pdot: return Namespace::Module;
    case Path::Kind::Pextra_ty: throw std::logic_error("Out_type.best_class_namespace");
    case Path::Kind::Pident:
      if (namespace_location(Namespace::Class, p->id)) return Namespace::Class;
      return Namespace::Class_type;
  }
  return std::nullopt;
}

}  // namespace

env::t printing_env() { return cur_env(); }

// ---- Ident_conflicts ----
namespace ident_conflicts {
namespace {
struct Explanation {
  Namespace kind;
  std::string name;
  std::string root_name;
  Location location;
};
std::map<std::string, Explanation> explanations;

void add(Namespace ns, const std::string& name, Ident::t id) {
  auto loc = namespace_location(ns, id);
  if (!loc) return;
  explanations[name] = Explanation{ns, name, std::string(ident::name(id)), *loc};
}
}  // namespace

void collect_explanation(Namespace ns, Ident::t id, const std::string& name) {
  std::string root_name(ident::name(id));
  if (root_name != name && !explanations.count(name)) {
    add(ns, name, id);
    if (!explanations.count(root_name)) {
      try {
        Path::t p = namespace_lookup(ns, root_name);
        if (p->kind == Path::Kind::Pident) add(ns, root_name, p->id);
      } catch (const env::NotFound&) {
      }
    }
  }
}

void reset() { explanations.clear(); }
bool exists() { return !explanations.empty(); }

std::optional<Doc> err_msg() {
  // list_explanations: the bindings, sorted with Stdlib.compare
  std::vector<Explanation> l;
  for (auto& [_, e] : explanations) l.push_back(e);
  explanations.clear();
  auto cmp_pos = [](const Position& a, const Position& b) {
    if (a.pos_fname != b.pos_fname) return a.pos_fname < b.pos_fname ? -1 : 1;
    if (a.pos_lnum != b.pos_lnum) return a.pos_lnum < b.pos_lnum ? -1 : 1;
    if (a.pos_bol != b.pos_bol) return a.pos_bol < b.pos_bol ? -1 : 1;
    if (a.pos_cnum != b.pos_cnum) return a.pos_cnum < b.pos_cnum ? -1 : 1;
    return 0;
  };
  std::stable_sort(l.begin(), l.end(), [&](const Explanation& a, const Explanation& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.name != b.name) return a.name < b.name;
    if (a.root_name != b.root_name) return a.root_name < b.root_name;
    if (int c = cmp_pos(a.location.loc_start, b.location.loc_start)) return c < 0;
    if (int c = cmp_pos(a.location.loc_end, b.location.loc_end)) return c < 0;
    return a.location.loc_ghost < b.location.loc_ghost;
  });
  // (the toplevel's explanations, isolated in ocamlc, do not arise here)
  if (l.empty()) return std::nullopt;
  Formatter f;
  fprintf(f, "@[<v>");
  bool first = true;
  for (auto& r : l) {
    if (!first) pp_print_cut(f);
    first = false;
    fprintf(f, "@[<v 2>%a:@,Definition of %s %a@]", [&](Formatter& ff) { location::doc::loc(ff, r.location); },
            namespace_to_string(r.kind), [&](Formatter& ff) {
              pp_open_stag(ff, "inline_code");
              pp_print_string(ff, r.name);
              pp_close_stag(ff);
            });
  }
  fprintf(f, "@]");
  return f.doc;
}

void err_print(Formatter& ppf) {
  if (std::optional<Doc> d = err_msg()) fprintf(ppf, "@,%a", [&](Formatter& ff) { pp_doc(ff, *d); });
}
}  // namespace ident_conflicts

// ---- Ident_names ----
namespace ident_names {
namespace {
bool enabled = true;
std::map<std::string, Ident::t, std::less<>> bound_in_recursion;
std::set<std::string, std::less<>> fuzzy;

bool fuzzy_id(Namespace ns, Ident::t id) { return ns == Namespace::Module && fuzzy.count(ident::name(id)); }

std::string human_id(Ident::t id, long index) {
  if (index == 0) return std::string(ident::name(id));
  return std::string(ident::name(id)) + "/" + std::to_string(index + 1);
}

std::optional<long> find_index(Namespace ns, Ident::t id, env::t e) {
  switch (ns) {
    case Namespace::Type: return env::find_type_index(id, e);
    case Namespace::Module: return env::find_module_index(id, e);
    case Namespace::Module_type: return env::find_modtype_index(id, e);
    case Namespace::Class: return env::find_class_index(id, e);
    case Namespace::Class_type: return env::find_cltype_index(id, e);
    default: return std::nullopt;
  }
}

std::string indexed_name(Namespace ns, Ident::t id) {
  std::optional<long> index;
  auto it = bound_in_recursion.find(ident::name(id));
  if (it != bound_in_recursion.end()) {
    if (ident::same(it->second, id)) {
      index = 0;
    } else {
      auto i = in_printing_env([&](env::t e) { return find_index(ns, id, e); });
      if (i) index = *i + 1;
    }
  } else {
    index = in_printing_env([&](env::t e) { return find_index(ns, id, e); });
  }
  return human_id(id, index.value_or(0));
}
}  // namespace

void enable(bool b) { enabled = b; }

void with_fuzzy(Ident::t id, const std::function<void()>& f) {
  auto saved = fuzzy;
  fuzzy.insert(std::string(ident::name(id)));
  try {
    f();
  } catch (...) {
    fuzzy = saved;
    throw;
  }
  fuzzy = saved;
}

void with_hidden(const std::vector<Ident::t>& ids, const std::function<void()>& f) {
  auto saved = bound_in_recursion;
  for (Ident::t id : ids) bound_in_recursion[std::string(ident::name(id))] = id;
  try {
    f();
  } catch (...) {
    bound_in_recursion = saved;
    throw;
  }
  bound_in_recursion = saved;
}

ot::OutName* ident_name(std::optional<Namespace> ns, Ident::t id) {
  if (!ns || !enabled) return ot::out_name_create(std::string(ident::name(id)));
  if (fuzzy_id(*ns, id)) return ot::out_name_create(std::string(ident::name(id)));
  std::string name = indexed_name(*ns, id);
  ident_conflicts::collect_explanation(*ns, id, name);
  return ot::out_name_create(name);
}
}  // namespace ident_names

ot::OutName* ident_name(std::optional<Namespace> ns, Ident::t id) { return ident_names::ident_name(ns, id); }

// ---- paths ----
namespace {

bool is_ident_stdlib(Ident::t id) { return ident::global(id) && ident::name(id) == "Stdlib"; }

bool non_shadowed_stdlib(std::optional<Namespace> ns, Path::t path) {
  if (path->kind != Path::Kind::Pdot || path->p1->kind != Path::Kind::Pident) return false;
  if (!is_ident_stdlib(path->p1->id)) return false;
  try {
    Path::t p2 = namespace_lookup(ns, path->s);
    return path::same(path, p2);
  } catch (const env::NotFound&) {
    return true;
  }
}

std::optional<std::size_t> find_double_underscore(std::string_view s) {
  for (std::size_t i = 0; i + 1 < s.size(); ++i)
    if (s[i] == '_' && s[i + 1] == '_') return i;
  return std::nullopt;
}

bool module_path_is_an_alias_of(env::t e, Path::t p, Path::t alias_of) {
  try {
    const ModuleDeclaration* md = env::find_module(p, e);
    if (md->md_type->kind != ModuleType::Kind::Mty_alias) return false;
    Path::t p2 = md->md_type->path;
    return path::same(p2, alias_of) || module_path_is_an_alias_of(e, p2, alias_of);
  } catch (const env::NotFound&) {
    return false;
  }
}

std::string modulize(std::string_view s) {  // Unit_info.modulize: Ok x | Error x -> x
  return utf8_lexeme::capitalize(s).s;
}

Path::t rewrite_double_underscore_paths_rec(env::t e, Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pdot: return Path::pdot(rewrite_double_underscore_paths_rec(e, p->p1), p->s);
    case Path::Kind::Papply: {
      // (the functor's rewrite is evaluated after the argument's)
      Path::t b = rewrite_double_underscore_paths_rec(e, p->p2);
      Path::t a = rewrite_double_underscore_paths_rec(e, p->p1);
      return Path::papply(a, b);
    }
    case Path::Kind::Pextra_ty:
      return Path::pextra_ty(rewrite_double_underscore_paths_rec(e, p->p1), p->extra, p->s);
    case Path::Kind::Pident: {
      std::string_view name = ident::name(p->id);
      auto i = find_double_underscore(name);
      if (!i) return p;
      Longident::t better_lid =
          Longident::ldot(Longident::lident(zborrow(name.substr(0, *i))), location::none(),
                          zborrow(modulize(name.substr(*i + 2))), location::none());
      Path::t p2;
      try {
        p2 = env::find_module_by_name(better_lid, e).first;
      } catch (const env::NotFound&) {
        return p;
      }
      return module_path_is_an_alias_of(e, p2, p) ? p2 : p;
    }
  }
  return p;
}

Path::t rewrite_double_underscore_paths(env::t e, Path::t p) {
  if (e == env::empty()) return p;
  return rewrite_double_underscore_paths_rec(e, p);
}

const ot::OutIdent* tree_of_path_rec(bool disambiguation, std::optional<Namespace> ns, Path::t p) {
  if (!disambiguation) ns = std::nullopt;
  switch (p->kind) {
    case Path::Kind::Pident: return ot::oide_ident(ident_name(ns, p->id));
    case Path::Kind::Pdot:
      if (non_shadowed_stdlib(ns, p)) return ot::oide_ident(ot::out_name_create(std::string(p->s)));
      return ot::oide_dot(tree_of_path_rec(disambiguation, Namespace::Module, p->p1), std::string(p->s));
    case Path::Kind::Papply: {
      const ot::OutIdent* t1 = tree_of_path_rec(disambiguation, Namespace::Module, p->p1);
      const ot::OutIdent* t2 = tree_of_path_rec(disambiguation, Namespace::Module, p->p2);
      return ot::oide_apply(t1, t2);
    }
    case Path::Kind::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty)
        return ot::oide_dot(tree_of_path_rec(disambiguation, Namespace::Type, p->p1), std::string(p->s));
      return tree_of_path_rec(disambiguation, std::nullopt, p->p1);
  }
  return nullptr;
}

const ot::OutIdent* tree_of_path_ns(std::optional<Namespace> ns, Path::t p, bool disambiguation = true) {
  return tree_of_path_rec(disambiguation, ns, rewrite_double_underscore_paths(cur_env(), p));
}

}  // namespace

const ot::OutIdent* tree_of_path(Path::t p, bool disambiguation) {
  return tree_of_path_ns(std::nullopt, p, disambiguation);
}
const ot::OutIdent* namespaced_tree_of_path(Namespace n, Path::t p) { return tree_of_path_ns(n, p); }

ot::OutRecStatus tree_of_rec(RecStatus rs) {
  switch (rs) {
    case RecStatus::Trec_not: return ot::OutRecStatus::Orec_not;
    case RecStatus::Trec_first: return ot::OutRecStatus::Orec_first;
    case RecStatus::Trec_next: return ot::OutRecStatus::Orec_next;
  }
  return ot::OutRecStatus::Orec_not;
}

// ---- normalized paths ----
namespace {

struct ParamSubst {  // Id | Nth of int | Map of int list
  enum class K : std::uint8_t { Id, Nth, Map } k = K::Id;
  long n = 0;
  std::vector<long> map;
};

bool is_nth(const ParamSubst& s) { return s.k == ParamSubst::K::Nth; }

std::vector<TypeExpr*> apply_subst(const ParamSubst& s1, const std::vector<TypeExpr*>& tyl) {
  if (tyl.empty()) return {};
  switch (s1.k) {
    case ParamSubst::K::Nth: return {tyl.at(static_cast<std::size_t>(s1.n))};
    case ParamSubst::K::Map: {
      std::vector<TypeExpr*> r;
      for (long i : s1.map) r.push_back(tyl.at(static_cast<std::size_t>(i)));
      return r;
    }
    case ParamSubst::K::Id: return tyl;
  }
  return tyl;
}


ParamSubst compose(const std::vector<long>& l1, const ParamSubst& s) {
  ParamSubst r;
  switch (s.k) {
    case ParamSubst::K::Id:
      r.k = ParamSubst::K::Map;
      r.map = l1;
      return r;
    case ParamSubst::K::Map:
      r.k = ParamSubst::K::Map;
      for (long i : s.map) r.map.push_back(l1.at(static_cast<std::size_t>(i)));
      return r;
    case ParamSubst::K::Nth:
      r.k = ParamSubst::K::Nth;
      r.n = l1.at(static_cast<std::size_t>(s.n));
      return r;
  }
  return r;
}

// ---- the short-paths cache (-short-paths: Clflags.real_paths off) ----
// A one-slot cache keyed by printing_old / printing_pers; printing_map is
// evaluated lazily, one module depth at a time (printing_depth), through
// Env's iteration continuations (printing_cont).
struct BestPath {  // Paths of Path.t list | Best of Path.t
  bool best = false;
  Path::t p = nullptr;
  std::vector<Path::t> paths;  // in list order
};
env::t g_printing_old = nullptr;  // Env.empty until first set
std::set<std::string> g_printing_pers;
long g_printing_depth = 0;
std::vector<env::IterCont> g_printing_cont;
PathMap<BestPath*> g_printing_map;

env::t printing_old() {
  if (!g_printing_old) g_printing_old = env::empty();
  return g_printing_old;
}

struct NotFoundIndex {};
long index_of(const Slice<TypeExpr*>& l, TypeExpr* x) {
  long i = 0;
  for (TypeExpr* a : l) {
    if (types::eq_type(x, a)) return i;
    ++i;
  }
  throw env::NotFound{};
}

bool uniq(const std::vector<long>& l) {
  for (std::size_t i = 0; i < l.size(); ++i)
    for (std::size_t j = i + 1; j < l.size(); ++j)
      if (l[i] == l[j]) return false;
  return true;
}

std::pair<Path::t, ParamSubst> normalize_type_path(bool cache, env::t e, Path::t p) {
  try {
    env::TypeExpansion x = env::find_type_expansion(p, e);
    const TypeDesc* d = types::get_desc(x.body);
    if (auto* c = as<Tconstr>(d)) {
      bool same_params = x.params.size() == c->args.size();
      if (same_params)
        for (std::size_t i = 0; i < x.params.size(); ++i)
          if (!types::eq_type(x.params[i], c->args[i])) {
            same_params = false;
            break;
          }
      if (same_params) return normalize_type_path(cache, e, c->path);
      std::vector<long> ids;
      for (TypeExpr* t : c->args) ids.push_back(types::get_id(t));
      if (cache || x.params.size() <= c->args.size() || !uniq(ids)) return {p, ParamSubst{}};
      std::vector<long> l1;
      for (TypeExpr* t : c->args) l1.push_back(index_of(x.params, t));
      auto [p2, s2] = normalize_type_path(cache, e, c->path);
      return {p2, compose(l1, s2)};
    }
    ParamSubst s;
    s.k = ParamSubst::K::Nth;
    s.n = index_of(x.params, x.body);
    return {p, s};
  } catch (const env::NotFound&) {
    return {env::normalize_type_path(nullptr, e, p), ParamSubst{}};
  }
}

long penalty(std::string_view s) {
  if (!s.empty() && s[0] == '_') return 10;
  return find_double_underscore(s) ? 10 : 1;
}

std::pair<long, long> path_size(Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident: return {penalty(ident::name(p->id)), -static_cast<long>(ident::scope(p->id))};
    case Path::Kind::Pdot: {
      auto [l, b] = path_size(p->p1);
      return {1 + l, b};
    }
    case Path::Kind::Papply: {
      auto [l, b] = path_size(p->p1);
      return {l + path_size(p->p2).first, b};
    }
    case Path::Kind::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty) {
        auto [l, b] = path_size(p->p1);
        return {1 + l, b};
      }
      return path_size(p->p1);
  }
  return {0, 0};
}

bool is_id(const ParamSubst& s) { return s.k == ParamSubst::K::Id; }

Longident::t lid_of_path(Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident: return Longident::lident(ident::name(p->id));
    case Path::Kind::Pdot: return Longident::ldot(lid_of_path(p->p1), location::none(), p->s, location::none());
    case Path::Kind::Papply:
      return Longident::lapply(lid_of_path(p->p1), location::none(), lid_of_path(p->p2), location::none());
    case Path::Kind::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty)
        return Longident::ldot(lid_of_path(p->p1), location::none(), p->s, location::none());
      return lid_of_path(p->p1);
  }
  return nullptr;
}

bool is_unambiguous(Path::t path, env::t e) {
  std::vector<Path::t> l = env::find_shadowed_types(path, e);
  for (Path::t q : l)
    if (path::same(path, q)) return true;  // concrete paths are ok
  if (l.empty()) return true;
  // allow also coherent paths:
  auto normalize = [&](Path::t q) { return normalize_type_path(true, e, q).first; };
  Path::t p0 = l[0];
  Path::t p1 = normalize(p0);
  bool coherent = true;
  for (std::size_t i = 1; i < l.size(); ++i)
    if (!path::same(normalize(l[i]), p1)) {
      coherent = false;
      break;
    }
  if (coherent) return true;
  // also allow repeatedly defining and opening (for toplevel)
  Longident::t id = lid_of_path(p0);
  for (std::size_t i = 1; i < l.size(); ++i)
    if (!longident::same(lid_of_path(l[i]), id)) return false;
  return path::same(p0, env::find_type_by_name(id, e).first);
}

Path::t get_best_path(BestPath* r) {
  for (;;) {
    if (r->best) return r->p;
    if (r->paths.empty()) throw env::NotFound{};
    std::vector<Path::t> l = std::move(r->paths);
    r->paths.clear();  // r := Paths []
    for (Path::t p : l) {
      if (r->best && path_size(p) >= path_size(r->p)) continue;
      if (is_unambiguous(p, cur_env())) {
        r->best = true;
        r->p = p;
      }
    }
  }
}

// best_type_path p
std::pair<Path::t, ParamSubst> best_type_path(Path::t p) {
  if (cur_env() == env::empty()) return {p, ParamSubst{}};
  if (clflags::real_paths) return {p, ParamSubst{}};
  auto [p1, s] = normalize_type_path(false, cur_env(), p);
  auto get_path = [&, p1 = p1]() -> Path::t {
    BestPath* const* r = g_printing_map.find_opt(p1);
    if (!r) throw env::NotFound{};
    return get_best_path(*r);
  };
  for (;;) {
    if (g_printing_cont.empty()) break;
    bool deeper;
    try {
      deeper = path_size(get_path()).first > g_printing_depth;
    } catch (const env::NotFound&) {
      deeper = true;
    }
    if (!deeper) break;
    std::vector<env::IterCont> next;
    for (auto& [q, c] : env::run_iter_cont(g_printing_cont)) next.push_back(std::move(c));
    g_printing_cont = std::move(next);
    ++g_printing_depth;
  }
  Path::t p2;
  try {
    p2 = get_path();
  } catch (const env::NotFound&) {
    p2 = p1;
  }
  return {p2, s};
}

const ot::OutIdent* tree_of_best_type_path(Path::t p, Path::t p2) {
  if (path::same(p, p2)) return tree_of_path_ns(Namespace::Type, p2);
  return tree_of_path_ns(std::nullopt, p2, false);
}


// proxy ty = Transient_expr.repr (proxy ty)
TypeExpr* proxy(TypeExpr* ty) { return types::repr(bt::proxy(ty)); }

bool is_non_gen(Mode mode, TypeExpr* ty) {
  return mode == Mode::Type_scheme && bt::is_Tvar(ty) && types::get_level(ty) != bt::generic_level;
}

bool nameable_row(const RowDesc* row) {
  if (!types::row_name(row)) return false;
  for (const auto& e : types::row_fields(row)) {
    auto f = types::row_field_repr(e.field);
    if (f.kind == RowFieldView::Kind::Reither) {
      bool ok = types::row_closed(row) && (f.constant ? f.arg_types.empty() : f.arg_types.size() == 1);
      if (!ok) return false;
    }
  }
  return true;
}

std::vector<TypeExpr*> slice_vec(Slice<TypeExpr*> s) { return std::vector<TypeExpr*>(s.begin(), s.end()); }

// printer_iter_type_expr: the subterms the printer prints
void printer_iter_type_expr(const std::function<void(TypeExpr*)>& f, TypeExpr* ty) {
  const TypeDesc* d = types::get_desc(ty);
  if (auto* c = as<Tconstr>(d)) {
    auto [p2, s] = best_type_path(c->path);
    (void)p2;
    for (TypeExpr* t : apply_subst(s, slice_vec(c->args))) f(t);
    return;
  }
  if (auto* v = as<Tvariant>(d)) {
    const PathArgs* nm = types::row_name(v->row);
    if (nm && nameable_row(v->row)) {
      for (TypeExpr* t : nm->args) f(t);
    } else {
      bt::iter_row(f, v->row);
    }
    return;
  }
  if (auto* o = as<Tobject>(d)) {
    if (!o->name->contents) {
      auto [fields, rest] = ctype::flatten_fields(o->fields);
      (void)rest;
      for (const auto& fe : fields)
        if (types::field_kind_repr(fe.kind) == FieldKindView::Fpublic) f(fe.ty);
    } else {
      const auto& l = o->name->contents->args;
      for (std::size_t i = 1; i < l.size(); ++i) f(l[i]);
    }
    return;
  }
  if (auto* fl = as<Tfield>(d)) {
    if (types::field_kind_repr(fl->kind_) == FieldKindView::Fpublic) f(fl->ty);
    f(fl->rest);
    return;
  }
  bt::iter_type_expr(f, ty);
}

}  // namespace

const ot::OutIdent* tree_of_type_path(Path::t p) {
  auto [p2, s] = best_type_path(p);
  Path::t p3 = s.k == ParamSubst::K::Id ? p2 : p;
  return tree_of_best_type_path(p, p3);
}

// ---- Internal_names ----
namespace internal_names {
namespace {
struct IdentLess {
  bool operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b) < 0; }
};
std::set<Ident::t, IdentLess> names;
}
void reset() { names.clear(); }
void add(Path::t p) {
  if (p->kind == Path::Kind::Pident) {
    std::string_view name = ident::name(p->id);
    if (!name.empty() && name[0] == '$') names.insert(p->id);
  }
}
void print_explanations(env::t e, Formatter& ppf) {
  // constrs: String.Map of the constructor to its idents' trees, the most
  // recent first (the trees made in Ident.Set order)
  std::map<std::string, std::vector<const ot::OutIdent*>> constrs;
  for (Ident::t id : names) {
    Path::t p = Path::pident(id);
    const TypeDeclaration* decl;
    try {
      decl = env::find_type(p, e);
    } catch (const env::NotFound&) {
      continue;
    }
    TypeOrigin o = bt::type_origin(decl);
    if (o.kind != TypeOrigin::Kind::Existential) continue;
    auto& l = constrs[std::string(o.existential)];
    l.insert(l.begin(), tree_of_path(p));
  }
  // Style.as_inline_code !Oprint.out_ident / Style.inline_code
  auto quoted_ident = [](Formatter& f, const ot::OutIdent* x) {
    pp_open_stag(f, "inline_code");
    oprint::out_ident(f, x);
    pp_close_stag(f);
  };
  auto inline_code = [](const std::string& str) {
    return [str](Formatter& f) {
      pp_open_stag(f, "inline_code");
      pp_print_string(f, str);
      pp_close_stag(f);
    };
  };
  for (auto& [constr, out_idents] : constrs) {
    if (out_idents.empty()) continue;
    if (out_idents.size() == 1) {
      fprintf(ppf, "@ @[<2>@{<hint>Hint@}:@ %a@ is an existential type@ bound by the constructor@ %a.@]",
              [&](Formatter& f) { quoted_ident(f, out_idents[0]); }, inline_code(constr));
    } else {
      std::vector<const ot::OutIdent*> rest(out_idents.rbegin(), out_idents.rend() - 1);
      fprintf(ppf,
              "@ @[<2>@{<hint>Hint@}:@ %a@ and %a@ are existential types@ bound by the constructor@ %a.@]",
              [&](Formatter& f) {
                pp_print_list(f, quoted_ident, rest, comma);
              },
              [&](Formatter& f) { quoted_ident(f, out_idents[0]); }, inline_code(constr));
    }
  }
}
}  // namespace internal_names

// ---- Variable_names ----
namespace {
namespace variable_names {

std::vector<std::pair<TypeExpr*, std::string>> names;  // head first
std::vector<std::pair<TypeExpr*, TypeExpr*>> name_subst;
long name_counter = 0;
std::vector<std::string> named_vars;
std::vector<TypeExpr*> visited_for_named_vars;
long weak_counter = 1;
std::map<long, std::pair<TypeExpr*, std::string>> weak_var_map;  // TypeMap (by id)
std::set<std::string> named_weak_vars;

void reset_names() {
  names.clear();
  name_subst.clear();
  name_counter = 0;
  named_vars.clear();
  visited_for_named_vars.clear();
}

bool memq(const std::vector<TypeExpr*>& l, TypeExpr* x) { return std::find(l.begin(), l.end(), x) != l.end(); }

void add_named_var(TypeExpr* tty) {
  const TypeDesc* d = tty->desc;
  OptStr name;
  if (auto* v = as<Tvar>(d)) name = v->name;
  else if (auto* u = as<Tunivar>(d)) name = u->name;
  else return;
  if (!name.some) return;
  std::string n(name.v);
  if (std::find(named_vars.begin(), named_vars.end(), n) != named_vars.end()) return;
  named_vars.insert(named_vars.begin(), n);
}

void add_named_vars(TypeExpr* ty) {
  TypeExpr* tty = types::repr(ty);
  TypeExpr* px = proxy(ty);
  if (memq(visited_for_named_vars, px)) return;
  visited_for_named_vars.insert(visited_for_named_vars.begin(), px);
  if (as<Tvar>(tty->desc) || as<Tunivar>(tty->desc))
    add_named_var(tty);
  else
    printer_iter_type_expr(add_named_vars, ty);
}

TypeExpr* substitute(TypeExpr* ty) {
  for (auto& [a, b] : name_subst)
    if (a == ty) return b;
  return ty;
}

bool name_is_already_used(const std::string& name) {
  if (std::find(named_vars.begin(), named_vars.end(), name) != named_vars.end()) return true;
  for (auto& [_, n] : names)
    if (n == name) return true;
  return named_weak_vars.count(name) != 0;
}

std::string letter_of_int(long n) {
  std::string letter(1, static_cast<char>('a' + n % 26));
  long num = n / 26;
  if (num == 0) return letter;
  return letter + std::to_string(num);
}

std::string new_name() {
  while (true) {
    std::string name = letter_of_int(name_counter);
    ++name_counter;
    if (!name_is_already_used(name)) return name;
  }
}

std::string new_weak_name(TypeExpr* ty) {
  while (true) {
    std::string name = "weak" + std::to_string(weak_counter);
    ++weak_counter;
    if (name_is_already_used(name)) continue;
    named_weak_vars.insert(name);
    weak_var_map[types::get_id(ty)] = {ty, name};
    return name;
  }
}

std::function<std::string()> new_var_name(bool non_gen, TypeExpr* ty) {
  if (non_gen) return [ty] { return new_weak_name(ty); };
  return [] { return new_name(); };
}

// Misc.find_first_mono
long find_first_mono(const std::function<bool(long)>& p) {
  if (p(0)) return 0;
  long low = 0, jump = 1, high = std::numeric_limits<long>::max() / 2;  // max_int
  high = 0x3fffffffffffffffL;
  while (true) {
    if (low + 1 == high) return high;
    if (jump < 1) {
      jump = 1;
      continue;
    }
    if (jump >= high - low) {
      jump = (high - low) / 2;
      continue;
    }
    if (p(low + jump)) {
      high = low + jump;
      jump = jump / 2;
    } else {
      low = low + jump;
      jump = std::max(jump, 2 * jump);
    }
  }
}

std::string name_of_type(const std::function<std::string()>& name_generator, TypeExpr* t0) {
  TypeExpr* t = substitute(t0);
  for (auto& [a, n] : names)
    if (a == t) return n;
  if (auto it = weak_var_map.find(types::get_id(t)); it != weak_var_map.end()) return it->second.second;
  std::string name;
  OptStr vname;
  if (auto* v = as<Tvar>(t->desc)) vname = v->name;
  else if (auto* u = as<Tunivar>(t->desc)) vname = u->name;
  if (vname.some) {
    std::string base(vname.v);
    auto available = [&](const std::string& n) {
      for (auto& [_, n2] : names)
        if (n == n2) return false;
      return true;
    };
    if (available(base)) {
      name = base;
    } else {
      long i = find_first_mono([&](long i) { return available(base + std::to_string(i)); });
      name = base + std::to_string(i);
    }
  } else {
    name = name_generator();
  }
  if (name != "_") names.insert(names.begin(), {t, name});
  return name;
}

void check_name_of_type(bool non_gen, TypeExpr* px) { (void)name_of_type(new_var_name(non_gen, px), px); }

void remove_names(const std::vector<TypeExpr*>& tyl0) {
  std::vector<TypeExpr*> tyl;
  for (TypeExpr* t : tyl0) tyl.push_back(substitute(t));
  std::vector<std::pair<TypeExpr*, std::string>> kept;
  for (auto& e : names)
    if (!memq(tyl, e.first)) kept.push_back(e);
  names = std::move(kept);
}

void with_local_names(const std::function<void()>& f) {
  auto old_names = names;
  auto old_subst = name_subst;
  names.clear();
  name_subst.clear();
  try {
    f();
  } catch (...) {
    names = old_names;
    name_subst = old_subst;
    throw;
  }
  names = old_names;
  name_subst = old_subst;
}

void reserve(TypeExpr* ty) {
  ctype::normalize_type(ty);
  add_named_vars(ty);
}

}  // namespace variable_names

// ---- Aliases ----
namespace aliases {

std::vector<TypeExpr*> visited_objects, aliased, delayed, printed_aliases;

bool memq(const std::vector<TypeExpr*>& l, TypeExpr* x) { return std::find(l.begin(), l.end(), x) != l.end(); }
bool is_delayed(TypeExpr* t) { return memq(delayed, t); }
void remove_delay(TypeExpr* t) {
  if (is_delayed(t)) delayed.erase(std::remove(delayed.begin(), delayed.end(), t), delayed.end());
}
void add_delayed(TypeExpr* t) {
  if (!is_delayed(t)) delayed.insert(delayed.begin(), t);
}
bool is_aliased_proxy(TypeExpr* px) { return memq(aliased, px); }
bool is_printed_proxy(TypeExpr* px) { return memq(printed_aliases, px); }
void add_proxy(TypeExpr* px) {
  if (!is_aliased_proxy(px)) aliased.insert(aliased.begin(), px);
}
void add(TypeExpr* ty) { add_proxy(proxy(ty)); }
void add_printed_proxy(bool non_gen, TypeExpr* px) {
  variable_names::check_name_of_type(non_gen, px);
  printed_aliases.insert(printed_aliases.begin(), px);
}
void mark_as_printed(TypeExpr* px) {
  if (is_aliased_proxy(px)) add_printed_proxy(false, px);
}
void add_printed(bool non_gen, TypeExpr* ty) { add_printed_proxy(non_gen, proxy(ty)); }

bool aliasable(TypeExpr* ty) {
  const TypeDesc* d = types::get_desc(ty);
  switch (d->kind) {
    case DescKind::Tvar:
    case DescKind::Tunivar:
    case DescKind::Tpoly: return false;
    case DescKind::Tconstr: return !is_nth(best_type_path(static_cast<const Tconstr*>(d)->path).second);
    default: return true;
  }
}

bool should_visit_object(TypeExpr* ty) {
  const TypeDesc* d = types::get_desc(ty);
  if (auto* v = as<Tvariant>(d)) return !bt::static_row(v->row);
  if (as<Tobject>(d)) return ctype::opened_object(ty);
  return false;
}

void mark_loops_rec(const std::vector<TypeExpr*>& visited0, TypeExpr* ty) {
  TypeExpr* px = proxy(ty);
  if (memq(visited0, px) && aliasable(ty)) {
    add_proxy(px);
    return;
  }
  std::vector<TypeExpr*> visited = visited0;
  visited.insert(visited.begin(), px);
  const TypeDesc* d = types::get_desc(ty);
  auto rec = [&](TypeExpr* t) { mark_loops_rec(visited, t); };
  switch (d->kind) {
    case DescKind::Tvariant:
    case DescKind::Tobject:
      if (memq(visited_objects, px)) {
        add_proxy(px);
      } else {
        if (should_visit_object(ty)) visited_objects.insert(visited_objects.begin(), px);
        printer_iter_type_expr(rec, ty);
      }
      return;
    case DescKind::Tpoly: {
      auto* p = static_cast<const Tpoly*>(d);
      for (TypeExpr* t : p->vars) add(t);
      rec(p->body);
      return;
    }
    case DescKind::Tarrow: {
      auto* a = static_cast<const Tarrow*>(d);
      if (a->label.kind == ArgLabel::Kind::Optional) {
        const TypeDesc* d1 = types::get_desc(a->t1);
        bool done = false;
        if (auto* pl = as<Tpoly>(d1); pl && pl->vars.empty()) {
          if (auto* c = as<Tconstr>(types::get_desc(pl->body));
              c && c->args.size() == 1 && path::same(c->path, predef::paths().option)) {
            rec(c->args[0]);
            done = true;
          }
        }
        if (!done) rec(a->t1);
        rec(a->t2);
        return;
      }
      printer_iter_type_expr(rec, ty);
      return;
    }
    default: printer_iter_type_expr(rec, ty); return;
  }
}

void mark_loops(TypeExpr* ty) { mark_loops_rec({}, ty); }

void reset() {
  visited_objects.clear();
  aliased.clear();
  delayed.clear();
  printed_aliases.clear();
}

}  // namespace aliases
}  // namespace

void prepare_type(TypeExpr* ty) {
  variable_names::reserve(ty);
  aliases::mark_loops(ty);
}

void reset_except_conflicts() {
  variable_names::reset_names();
  aliases::reset();
  internal_names::reset();
}

void reset() {
  ident_conflicts::reset();
  reset_except_conflicts();
}

void prepare_for_printing(const std::vector<TypeExpr*>& tyl) {
  reset_except_conflicts();
  for (TypeExpr* t : tyl) prepare_type(t);
}

void add_type_to_preparation(TypeExpr* ty) { prepare_type(ty); }
void reserve_names(TypeExpr* ty) { variable_names::reserve(ty); }
void mark_loops(TypeExpr* ty) { aliases::mark_loops(ty); }
void reset_aliases() { aliases::reset(); }
void add_delayed(TypeExpr* ty) { aliases::add_delayed(proxy(ty)); }
void with_local_names(const std::function<void()>& f) { variable_names::with_local_names(f); }
void add_subst(const std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst) {
  std::vector<std::pair<TypeExpr*, TypeExpr*>> l;
  for (auto& [a, b] : subst) l.emplace_back(types::repr(a), types::repr(b));
  variable_names::name_subst.insert(variable_names::name_subst.begin(), l.begin(), l.end());
}
std::string new_name() { return variable_names::new_name(); }
std::string name_of_type_named(TypeExpr* t) {
  return variable_names::name_of_type([] { return variable_names::new_name(); }, t);
}

namespace {
bool same_printing_env(env::t e) {
  std::set<std::string> used_pers = env::used_persistent();
  return env::same_types(printing_old(), e) && g_printing_pers == used_pers;
}
}  // namespace

void set_printing_env(env::t e) {
  g_printing_env = e;
  if (clflags::real_paths || cur_env() == env::empty() || same_printing_env(e)) return;
  g_printing_old = e;
  g_printing_pers = env::used_persistent();
  g_printing_map = PathMap<BestPath*>{};
  g_printing_depth = 0;
  env::IterCont cont = env::iter_types(
      [e](Path::t p, Path::t p2, const TypeDeclaration*) {
        auto [p1, s1] = normalize_type_path(true, e, p2);
        if (!is_id(s1)) return;
        if (BestPath* const* r = g_printing_map.find_opt(p1)) {
          if ((*r)->best) {  // Best p' -> r := Paths [p; p']
            Path::t p3 = (*r)->p;
            (*r)->best = false;
            (*r)->p = nullptr;
            (*r)->paths = {p, p3};
          } else {
            (*r)->paths.insert((*r)->paths.begin(), p);
          }
          return;
        }
        BestPath* r = make<BestPath>();
        r->paths = {p};
        g_printing_map = g_printing_map.add(p1, r);
      },
      e);
  g_printing_cont = {cont};
}

void reset_short_paths_cache() {
  g_printing_env = nullptr;
  g_printing_old = nullptr;
  g_printing_pers.clear();
  g_printing_depth = 0;
  g_printing_cont.clear();
  g_printing_map = PathMap<BestPath*>{};
}

void wrap_printing_env(bool error, env::t e, const std::function<void()>& f) {
  auto run = [&] {
    set_printing_env(e);
    try {
      f();
    } catch (...) {
      set_printing_env(env::empty());
      throw;
    }
    set_printing_env(env::empty());
  };
  if (error)
    env::without_cmis(run);
  else
    run();
}

namespace {

// wrap_env ?keep_short_paths fenv ftree arg
template <class R>
R wrap_env(bool keep_short_paths, const std::function<env::t(env::t)>& fenv, const std::function<R()>& ftree) {
  // We save the current value of the short-path cache: from keys ...
  env::t e = cur_env();
  std::set<std::string> old_pers = g_printing_pers;
  // ... to data
  PathMap<BestPath*> old_map = g_printing_map;
  long old_depth = g_printing_depth;
  std::vector<env::IterCont> old_cont = g_printing_cont;
  if (keep_short_paths)
    g_printing_env = fenv(e);
  else
    set_printing_env(fenv(e));
  R tree = ftree();
  if (clflags::real_paths || same_printing_env(e)) {
    // our cached key is still live in the cache, and we want to keep all
    // progress made on the computation of the printing_map
  } else {
    // we restore the snapshotted cache before calling set_printing_env
    g_printing_old = e;
    g_printing_pers = std::move(old_pers);
    g_printing_depth = old_depth;
    g_printing_cont = std::move(old_cont);
    g_printing_map = old_map;
  }
  set_printing_env(e);
  return tree;
}

// we hide items being defined from short-path to avoid shortening
// [type t = Path.To.t] into [type t = t].
struct BoundIdent {
  bool hide;
  Ident::t ident;
};

const TypeDeclaration* dummy_decl() {
  auto* d = make<TypeDeclaration>();
  d->type_params = {};
  d->type_arity = 0;
  d->type_kind = TYPE_ABSTRACT_LIT(Definition);
  d->type_private = PrivateFlag::Public;
  d->type_manifest = nullptr;
  d->type_is_newtype = false;
  d->type_expansion_scope = bt::lowest_level;
  d->type_loc = location::none();
  d->type_immediate = TypeImmediacy::Unknown;
  d->type_unboxed_default = false;
  d->type_uid = uid::internal_not_actually_unique();
  return d;
}

// hide ids env: List.fold_right hide_id ids env
env::t hide(const std::vector<BoundIdent>& ids, env::t e) {
  for (auto it = ids.rbegin(); it != ids.rend(); ++it)
    // Global idents cannot be renamed
    if (it->hide && !ident::global(it->ident)) e = env::add_type(false, ident::rename(it->ident), dummy_decl(), e);
  return e;
}

template <class R>
R with_hidden_items(const std::vector<BoundIdent>& ids, const std::function<R()>& f) {
  std::vector<Ident::t> l;
  for (auto& b : ids) l.push_back(b.ident);
  auto hidden = [&]() -> R {  // Ident_names.with_hidden ids f
    std::optional<R> r;
    ident_names::with_hidden(l, [&] { r.emplace(f()); });
    return std::move(*r);
  };
  if (!clflags::real_paths)
    return wrap_env<R>(false, [&](env::t e) { return hide(ids, e); }, hidden);
  return hidden();
}

}  // namespace

bool print_labels = true;
void with_labels(bool b, const std::function<void()>& f) {
  bool saved = print_labels;
  print_labels = b;
  try {
    f();
  } catch (...) {
    print_labels = saved;
    throw;
  }
  print_labels = saved;
}

// ---- tree_of_typexp ----
namespace {

void alias_nongen_row(Mode mode, TypeExpr* px, TypeExpr* ty) {
  const TypeDesc* d = types::get_desc(ty);
  if (d->kind == DescKind::Tvariant || d->kind == DescKind::Tobject)
    if (is_non_gen(mode, px)) aliases::add_proxy(px);
}

ot::ArgLabel out_label(const ArgLabel& l) {
  ot::ArgLabel r;
  switch (l.kind) {
    case ArgLabel::Kind::Nolabel: r.k = ot::ArgLabel::K::Nolabel; break;
    case ArgLabel::Kind::Labelled: r.k = ot::ArgLabel::K::Labelled; break;
    case ArgLabel::Kind::Optional: r.k = ot::ArgLabel::K::Optional; break;
  }
  r.s = std::string(l.name);
  return r;
}

const ot::OutType* stuff(std::string s) {
  auto* t = ot::otyp(OT::Otyp_stuff);
  t->s = std::move(s);
  return t;
}

ot::OutVariant::Field tree_of_row_field(Mode mode, const RowFieldEntry& e);
const ot::OutType* tree_of_typobject(Mode mode, TypeExpr* fi, const PathArgs* nm);
std::pair<std::vector<std::pair<std::string, const ot::OutType*>>, ot::OutRow> tree_of_typfields(
    Mode mode, TypeExpr* rest, const std::vector<std::pair<std::string_view, TypeExpr*>>& l, std::size_t from);
const ot::OutPackage* tree_of_package(Mode mode, const Package* pack);

}  // namespace

std::vector<const ot::OutType*> tree_of_typlist(Mode mode, const std::vector<TypeExpr*>& tyl) {
  std::vector<const ot::OutType*> r;
  for (TypeExpr* t : tyl) r.push_back(tree_of_typexp(mode, t));
  return r;
}

const ot::OutType* tree_of_typexp(Mode mode, TypeExpr* ty) {
  TypeExpr* px = proxy(ty);
  if (aliases::is_printed_proxy(px) && !aliases::is_delayed(px)) {
    bool non_gen = is_non_gen(mode, px);
    std::string name = variable_names::name_of_type(variable_names::new_var_name(non_gen, ty), px);
    auto* t = ot::otyp(OT::Otyp_var);
    t->non_gen = non_gen;
    t->s = name;
    return t;
  }
  auto pr_typ = [&]() -> const ot::OutType* {
    TypeExpr* tty = types::repr(ty);
    const TypeDesc* d = types::get_desc(ty);
    switch (d->kind) {
      case DescKind::Tvar: {
        bool non_gen = is_non_gen(mode, ty);
        auto name_gen = variable_names::new_var_name(non_gen, ty);
        auto* t = ot::otyp(OT::Otyp_var);
        t->non_gen = non_gen;
        t->s = variable_names::name_of_type(name_gen, tty);
        return t;
      }
      case DescKind::Tarrow: {
        auto* a = static_cast<const Tarrow*>(d);
        ArgLabel lab = print_labels || bt::is_optional(a->label) ? a->label : ArgLabel::nolabel();
        const ot::OutType* t1;
        if (bt::is_optional(a->label)) {
          if (bt::tpoly_is_mono(a->t1)) {
            TypeExpr* mono = bt::tpoly_get_mono(a->t1);
            auto* c = as<Tconstr>(types::get_desc(mono));
            if (c && c->args.size() == 1 && path::same(c->path, predef::paths().option)) {
              if (aliases::is_aliased_proxy(proxy(mono)))
                t1 = stuff("<hidden>");
              else
                t1 = tree_of_typexp(mode, c->args[0]);
            } else {
              t1 = stuff("<hidden>");
            }
          } else {
            t1 = stuff("<hidden>");
          }
        } else {
          t1 = tree_of_typexp(mode, a->t1);
        }
        auto* t = ot::otyp(OT::Otyp_arrow);
        t->label = out_label(lab);
        t->t1 = t1;
        t->t2 = tree_of_typexp(mode, a->t2);
        return t;
      }
      case DescKind::Tfunctor: {
        auto* fn = static_cast<const Tfunctor*>(d);
        ArgLabel lab = print_labels || bt::is_optional(fn->label) ? fn->label : ArgLabel::nolabel();
        auto fenv = [&](env::t e) {
          // We compute an approximation of the signature.
          auto* mty = make<ModuleType>();
          mty->kind = ModuleType::Kind::Mty_ident;
          mty->path = fn->pack->pack_path;
          return env::add_module(Ident::of_unscoped(fn->id), ModulePresence::Mp_present, mty, e, true);
        };
        const ot::OutType* body =
            wrap_env<const ot::OutType*>(true, fenv, [&] { return tree_of_typexp(mode, fn->body); });
        // (constructor args right to left: the package first)
        const ot::OutPackage* pk = tree_of_package(mode, fn->pack);
        auto* t = ot::otyp(OT::Otyp_functor);
        t->label = out_label(lab);
        t->id = ot::oide_ident(ot::out_name_create(std::string(ident::Unscoped::name_of(fn->id))));
        t->pack = pk;
        t->t1 = body;
        return t;
      }
      case DescKind::Ttuple: {
        auto* tu = static_cast<const Ttuple*>(d);
        auto* t = ot::otyp(OT::Otyp_tuple);
        for (const LabeledTy& e : tu->elems)
          t->tuple.emplace_back(e.label.some ? std::optional<std::string>(std::string(e.label.v)) : std::nullopt,
                                tree_of_typexp(mode, e.ty));
        return t;
      }
      case DescKind::Tconstr: {
        auto* c = static_cast<const Tconstr*>(d);
        auto [p2, s] = best_type_path(c->path);
        std::vector<TypeExpr*> tyl2 = apply_subst(s, slice_vec(c->args));
        if (is_nth(s) && !tyl2.empty()) return tree_of_typexp(mode, tyl2[0]);
        internal_names::add(p2);
        auto args = tree_of_typlist(mode, tyl2);
        auto* t = ot::otyp(OT::Otyp_constr);
        t->id = tree_of_best_type_path(c->path, p2);
        t->args = std::move(args);
        return t;
      }
      case DescKind::Tvariant: {
        auto* v = static_cast<const Tvariant*>(d);
        RowDescRepr row = types::row_repr(v->row);
        std::vector<RowFieldEntry> fields;
        if (row.closed) {
          for (auto& e : row.fields)
            if (types::row_field_repr(e.field).kind != RowFieldView::Kind::Rabsent) fields.push_back(e);
        } else {
          fields.assign(row.fields.begin(), row.fields.end());
        }
        std::vector<std::string> present;
        for (auto& e : fields)
          if (types::row_field_repr(e.field).kind == RowFieldView::Kind::Rpresent)
            present.emplace_back(e.label);
        bool all_present = present.size() == fields.size();
        auto* t = ot::otyp(OT::Otyp_variant);
        t->closed = row.closed;
        if (row.name && nameable_row(v->row)) {
          auto [p2, s] = best_type_path(row.name->path);
          const ot::OutIdent* id = tree_of_best_type_path(row.name->path, p2);
          auto args = tree_of_typlist(mode, apply_subst(s, slice_vec(row.name->args)));
          const ot::OutType* out_variant;
          if (is_nth(s)) {
            out_variant = args.at(0);
          } else {
            auto* c = ot::otyp(OT::Otyp_constr);
            c->id = id;
            c->args = args;
            out_variant = c;
          }
          if (row.closed && all_present) return out_variant;
          if (!all_present) t->tags = present;
          t->variant.is_typ = true;
          t->variant.typ = out_variant;
          return t;
        }
        for (auto& e : fields) t->variant.fields.push_back(tree_of_row_field(mode, e));
        if (!all_present) t->tags = present;
        return t;
      }
      case DescKind::Tobject: {
        auto* o = static_cast<const Tobject*>(d);
        return tree_of_typobject(mode, o->fields, o->name->contents);
      }
      case DescKind::Tnil:
      case DescKind::Tfield: return tree_of_typobject(mode, ty, nullptr);
      case DescKind::Tsubst: return stuff("<Tsubst>");
      case DescKind::Tlink: throw std::logic_error("Out_type.tree_of_typexp");
      case DescKind::Tpoly: {
        auto* p = static_cast<const Tpoly*>(d);
        if (p->vars.empty()) return tree_of_typexp(mode, p->body);
        std::vector<TypeExpr*> tyl;
        for (TypeExpr* t : p->vars) tyl.push_back(types::repr(t));
        auto old_delayed = aliases::delayed;
        // Make the names delayed, so that the real type is
        // printed once when used as proxy
        for (TypeExpr* t : tyl) aliases::add_delayed(t);
        std::vector<std::string> tl;
        for (TypeExpr* t : tyl) tl.push_back(variable_names::name_of_type([] { return variable_names::new_name(); }, t));
        auto* tr = ot::otyp(OT::Otyp_poly);
        tr->vars = tl;
        tr->t1 = tree_of_typexp(mode, p->body);
        // Forget names when we leave scope
        variable_names::remove_names(tyl);
        aliases::delayed = old_delayed;
        return tr;
      }
      case DescKind::Tunivar: {
        auto* t = ot::otyp(OT::Otyp_var);
        t->non_gen = false;
        t->s = variable_names::name_of_type([] { return variable_names::new_name(); }, tty);
        return t;
      }
      case DescKind::Tpackage: {
        auto* t = ot::otyp(OT::Otyp_module);
        t->pack = tree_of_package(mode, static_cast<const Tpackage*>(d)->pack);
        return t;
      }
    }
    throw std::logic_error("Out_type.tree_of_typexp");
  };
  aliases::remove_delay(px);
  alias_nongen_row(mode, px, ty);
  if (aliases::is_aliased_proxy(px) && aliases::aliasable(ty)) {
    bool non_gen = is_non_gen(mode, px);
    aliases::add_printed_proxy(non_gen, px);
    // add_printed_proxy chose a name, thus the name generator doesn't matter.
    std::string alias = variable_names::name_of_type(variable_names::new_var_name(non_gen, ty), px);
    auto* t = ot::otyp(OT::Otyp_alias);
    t->non_gen = non_gen;
    t->s = alias;
    t->t1 = pr_typ();
    return t;
  }
  return pr_typ();
}

namespace {

ot::OutVariant::Field tree_of_row_field(Mode mode, const RowFieldEntry& e) {
  RowFieldView f = types::row_field_repr(e.field);
  std::string l(e.label);
  switch (f.kind) {
    case RowFieldView::Kind::Rpresent:
      if (!f.present) return {l, false, {}};
      return {l, false, {tree_of_typexp(mode, f.present)}};
    case RowFieldView::Kind::Reither:
      if (f.constant && f.arg_types.empty()) return {l, false, {}};
      // c: contradiction, a constant constructor with an argument
      return {l, f.constant, tree_of_typlist(mode, f.arg_types)};
    case RowFieldView::Kind::Rabsent: return {l, false, {}};
  }
  return {l, false, {}};
}

const ot::OutType* tree_of_typobject(Mode mode, TypeExpr* fi, const PathArgs* nm) {
  if (!nm) {
    auto [fields, rest] = ctype::flatten_fields(fi);
    std::vector<std::pair<std::string_view, TypeExpr*>> present_fields;
    for (const auto& fe : fields)
      if (types::field_kind_repr(fe.kind) == FieldKindView::Fpublic) present_fields.emplace_back(fe.name, fe.ty);
    std::stable_sort(present_fields.begin(), present_fields.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    auto [fs, row] = tree_of_typfields(mode, rest, present_fields, 0);
    auto* t = ot::otyp(OT::Otyp_object);
    t->fields = std::move(fs);
    t->row = row;
    return t;
  }
  if (nm->args.empty()) throw std::logic_error("Out_type.tree_of_typobject");
  std::vector<TypeExpr*> tyl(nm->args.begin() + 1, nm->args.end());
  auto args = tree_of_typlist(mode, tyl);
  auto [p2, s] = best_type_path(nm->path);
  (void)s;
  auto* t = ot::otyp(OT::Otyp_class);
  t->id = tree_of_best_type_path(nm->path, p2);
  t->args = std::move(args);
  return t;
}

std::pair<std::vector<std::pair<std::string, const ot::OutType*>>, ot::OutRow> tree_of_typfields(
    Mode mode, TypeExpr* rest, const std::vector<std::pair<std::string_view, TypeExpr*>>& l, std::size_t from) {
  if (from >= l.size()) {
    ot::OutRow row;
    const TypeDesc* d = types::get_desc(rest);
    if (auto* c = as<Tconstr>(d)) {
      if (c->path->kind == Path::Kind::Pident && bt::is_row_name(ident::name(c->path->id)))
        row.k = ot::OutRowK::Orow_open_anonymous;
      else {
        row.k = ot::OutRowK::Orow_open;
        row.ty = tree_of_typexp(mode, rest);
      }
    } else if (d->kind == DescKind::Tvar || d->kind == DescKind::Tunivar) {
      row.k = ot::OutRowK::Orow_open_anonymous;
    } else if (d->kind == DescKind::Tnil) {
      row.k = ot::OutRowK::Orow_closed;
    } else {
      throw std::logic_error("typfields (1)");
    }
    return {{}, row};
  }
  std::pair<std::string, const ot::OutType*> field{std::string(l[from].first), tree_of_typexp(mode, l[from].second)};
  auto [fields, row] = tree_of_typfields(mode, rest, l, from + 1);
  fields.insert(fields.begin(), field);
  return {fields, row};
}

const ot::OutPackage* tree_of_package(Mode mode, const Package* pack) {
  auto* p = make<ot::OutPackage>();
  // (record fields: the constraints are evaluated before the path)
  for (const PackConstraint& c : pack->pack_constraints) {
    std::string li;
    for (std::size_t i = 0; i < c.path.size(); ++i) {
      if (i) li += ".";
      li += c.path[i];
    }
    p->opack_constraints.emplace_back(li, tree_of_typexp(mode, c.ty));
  }
  p->opack_path = tree_of_path_ns(Namespace::Module_type, pack->pack_path);
  return p;
}

}  // namespace

void typexp(Mode mode, Formatter& ppf, TypeExpr* ty) { oprint::out_type(ppf, tree_of_typexp(mode, ty)); }
void prepared_type_expr(Formatter& ppf, TypeExpr* ty) { typexp(Mode::Type, ppf, ty); }
void type_expr_with_reserved_names(Formatter& ppf, TypeExpr* ty) {
  aliases::reset();
  aliases::mark_loops(ty);
  prepared_type_expr(ppf, ty);
}
void prepared_type_scheme(Formatter& ppf, TypeExpr* ty) { typexp(Mode::Type_scheme, ppf, ty); }

// ---- type declarations ----
namespace {

std::vector<std::pair<const ot::OutType*, const ot::OutType*>> tree_of_constraints(const std::vector<TypeExpr*>& params) {
  // List.fold_right: the last parameter first
  std::vector<std::pair<const ot::OutType*, const ot::OutType*>> list;
  for (std::size_t k = params.size(); k-- > 0;) {
    TypeExpr* ty = params[k];
    TypeExpr* ty2 = ctype::unalias(ty);
    if (proxy(ty) != proxy(ty2)) {
      const ot::OutType* tr = tree_of_typexp(Mode::Type_scheme, ty);
      list.insert(list.begin(), {tr, tree_of_typexp(Mode::Type_scheme, ty2)});
    }
  }
  return list;
}

std::vector<TypeExpr*> filter_params(Slice<TypeExpr*> tyl) {
  std::vector<TypeExpr*> params;  // reversed
  for (TypeExpr* ty : tyl) {
    bool dup = false;
    for (TypeExpr* t : params)
      if (types::eq_type(ty, t)) dup = true;
    if (dup)
      params.insert(params.begin(),
                    bt::newty2(bt::generic_level, types::ttuple(slice(std::vector<LabeledTy>{{OptStr::none(), ty}}))));
    else
      params.insert(params.begin(), ty);
  }
  std::reverse(params.begin(), params.end());
  return params;
}

std::pair<ot::Variance, ot::Injectivity> syntactic_variance(bool with_variance, bool with_injectivity, variance::t vi) {
  using variance::F;
  ot::Variance v = ot::Variance::NoVariance;
  if (with_variance) {
    bool p = variance::mem(F::May_pos, vi), n = variance::mem(F::May_neg, vi);
    if (!p && !n) v = ot::Variance::Bivariant;
    else if (p && !n) v = ot::Variance::Covariant;
    else if (!p && n) v = ot::Variance::Contravariant;
    else v = ot::Variance::NoVariance;
  }
  ot::Injectivity i =
      with_injectivity && variance::mem(F::Inj, vi) ? ot::Injectivity::Injective : ot::Injectivity::NoInjectivity;
  return {v, i};
}

std::pair<ot::Variance, ot::Injectivity> syntactic_variance(variance::t vi) {
  return syntactic_variance(clflags::print_variance, clflags::print_variance, vi);
}

// prepare_decl id decl: (ty_manifest, params)
std::pair<TypeExpr*, std::vector<TypeExpr*>> prepare_decl(Ident::t id, const TypeDeclaration* decl) {
  std::vector<TypeExpr*> params = filter_params(decl->type_params);
  if (decl->type_manifest) {
    std::vector<TypeExpr*> vars = ctype::free_variables(decl->type_manifest);
    for (TypeExpr* ty : params) {
      auto* v = as<Tvar>(types::get_desc(ty));
      if (v && v->name.some && v->name.v == "_") {
        bool in = false;
        for (TypeExpr* x : vars)
          if (types::eq_type(ty, x)) in = true;
        if (in) types::set_type_desc(ty, TVAR_NONE_LIT());
      }
    }
  }
  for (TypeExpr* t : params) aliases::add(t);
  for (TypeExpr* t : params) prepare_type(t);
  for (TypeExpr* t : params) aliases::add_printed(false, t);
  TypeExpr* ty_manifest = nullptr;
  if (decl->type_manifest) {
    TypeExpr* ty = decl->type_manifest;
    // Special hack to hide row name
    const TypeDesc* d = types::get_desc(ty);
    if (auto* v = as<Tvariant>(d)) {
      const PathArgs* nm = types::row_name(v->row);
      if (nm && nm->path->kind == Path::Kind::Pident && ident::same(id, nm->path->id))
        ty = bt::newgenty(types::tvariant(types::set_row_name(v->row, nullptr)));
    } else if (as<Tobject>(d)) {
      TypeExpr* object_row = bt::row_of_type(ty);
      if (auto* c = as<Tconstr>(types::get_desc(object_row));
          c && c->path->kind == Path::Kind::Pident && ident::same(id, c->path->id))
        types::set_type_desc(object_row, TVAR_NONE_LIT());
    }
    prepare_type(ty);
    ty_manifest = ty;
  }
  const TypeKind* k = decl->type_kind;
  switch (k->kind) {
    case TypeKind::Kind::Type_abstract: break;
    case TypeKind::Kind::Type_variant:
      for (const ConstructorDeclaration* c : k->constructors) {
        prepare_type_constructor_arguments(c->cd_args);
        if (c->cd_res) prepare_type(c->cd_res);
      }
      break;
    case TypeKind::Kind::Type_record:
      for (const LabelDeclaration* l : k->labels) prepare_type(l->ld_type);
      break;
    case TypeKind::Kind::Type_open:
    case TypeKind::Kind::Type_external: break;
  }
  return {ty_manifest, params};
}

ot::OutTypeParam type_param_of(const std::pair<ot::Variance, ot::Injectivity>& v, const ot::OutType* t) {
  if (t->k == OT::Otyp_var) return {t->non_gen, t->s, v};
  return {false, "?", v};
}

ot::OutConstructor tree_of_constructor_in_decl(const ConstructorDeclaration* cd) {
  if (!cd->cd_res) return tree_of_single_constructor(cd);
  ot::OutConstructor r;
  variable_names::with_local_names([&] { r = tree_of_single_constructor(cd); });
  return r;
}

ot::TypeImmediacyO immediacy_of(TypeImmediacy t) {
  switch (t) {
    case TypeImmediacy::Unknown: return ot::TypeImmediacyO::Unknown;
    case TypeImmediacy::Always: return ot::TypeImmediacyO::Always;
    case TypeImmediacy::Always_on_64bits: return ot::TypeImmediacyO::Always_on_64bits;
  }
  return ot::TypeImmediacyO::Unknown;
}

const ot::OutTypeDecl* tree_of_type_decl_(Ident::t id, const TypeDeclaration* decl) {
  auto [ty_manifest, params] = prepare_decl(id, decl);
  // type_defined decl
  const TypeKind* k = decl->type_kind;
  bool abstr;
  switch (k->kind) {
    case TypeKind::Kind::Type_abstract:
      abstr = decl->type_manifest == nullptr || decl->type_private == PrivateFlag::Private;
      break;
    case TypeKind::Kind::Type_record: abstr = decl->type_private == PrivateFlag::Private; break;
    case TypeKind::Kind::Type_variant: {
      abstr = decl->type_private == PrivateFlag::Private;
      for (const ConstructorDeclaration* cd : k->constructors)
        if (cd->cd_res) abstr = true;
      break;
    }
    case TypeKind::Kind::Type_open: abstr = decl->type_manifest == nullptr; break;
    case TypeKind::Kind::Type_external: abstr = true; break;
  }
  std::vector<std::pair<ot::Variance, ot::Injectivity>> vari;
  for (std::size_t i = 0; i < decl->type_params.size(); ++i) {
    TypeExpr* ty = decl->type_params[i];
    variance::t v = decl->type_variance[i];
    bool is_var = bt::is_Tvar(ty);
    bool with_variance = clflags::print_variance || abstr || !is_var;
    bool with_injectivity = clflags::print_variance;
    if (!with_injectivity && (abstr || !is_var) && bt::type_kind_is_abstract(decl)) {
      if (!decl->type_manifest)
        with_injectivity = true;
      else
        with_injectivity = decl->type_private == PrivateFlag::Private &&
                           bt::is_constr_row(true, bt::row_of_type(decl->type_manifest));
    }
    vari.push_back(syntactic_variance(with_variance, with_injectivity, v));
  }
  std::vector<ot::OutTypeParam> args;
  for (std::size_t i = 0; i < params.size(); ++i) args.push_back(type_param_of(vari.at(i), tree_of_typexp(Mode::Type, params[i])));
  std::string name(ident::name(id));
  auto tree_of_manifest = [&](const ot::OutType* ty1) -> const ot::OutType* {
    if (!ty_manifest) return ty1;
    auto* m = ot::otyp(OT::Otyp_manifest);
    m->t1 = tree_of_typexp(Mode::Type, ty_manifest);
    m->t2 = ty1;
    return m;
  };
  auto constraints = tree_of_constraints(params);
  const ot::OutType* ty;
  bool priv;
  bool unboxed;
  switch (k->kind) {
    case TypeKind::Kind::Type_abstract:
      if (!ty_manifest) {
        ty = ot::otyp(OT::Otyp_abstract);
        priv = false;
        unboxed = false;
      } else {
        ty = tree_of_typexp(Mode::Type, ty_manifest);
        priv = decl->type_private == PrivateFlag::Private;
        unboxed = false;
      }
      break;
    case TypeKind::Kind::Type_variant: {
      auto* sum = ot::otyp(OT::Otyp_sum);
      for (const ConstructorDeclaration* cd : k->constructors) sum->constrs.push_back(tree_of_constructor_in_decl(cd));
      unboxed = k->variant_repr == VariantRepresentation::Variant_unboxed;
      priv = decl->type_private == PrivateFlag::Private;
      ty = tree_of_manifest(sum);
      break;
    }
    case TypeKind::Kind::Type_record: {
      auto* rec = ot::otyp(OT::Otyp_record);
      for (const LabelDeclaration* l : k->labels) rec->labels.push_back(tree_of_label(l));
      unboxed = k->record_repr.kind == RecordRepresentation::Kind::Record_unboxed;
      priv = decl->type_private == PrivateFlag::Private;
      ty = tree_of_manifest(rec);
      break;
    }
    case TypeKind::Kind::Type_open:
      unboxed = false;
      priv = decl->type_private == PrivateFlag::Private;
      ty = tree_of_manifest(ot::otyp(OT::Otyp_open));
      break;
    case TypeKind::Kind::Type_external: {
      auto* e = ot::otyp(OT::Otyp_external);
      e->s = std::string(k->external);
      ty = e;
      priv = false;
      unboxed = false;
      break;
    }
  }
  auto* td = make<ot::OutTypeDecl>();
  td->otype_name = name;
  td->otype_params = args;
  td->otype_type = ty;
  td->otype_private = priv;
  td->otype_immediate = immediacy_of(type_immediacy::of_attributes(decl->type_attributes));
  td->otype_unboxed = unboxed;
  td->otype_constraints = constraints;
  return td;
}

const ot::OutSigItem* osig_type(const ot::OutTypeDecl* td, RecStatus rs) {
  auto* it = make<ot::OutSigItem>();
  it->k = ot::OutSigItem::K::Osig_type;
  it->td = td;
  it->rs = tree_of_rec(rs);
  return it;
}

const ot::OutSigItem* tree_of_type_declaration_(Ident::t id, const TypeDeclaration* decl, RecStatus rs) {
  reset_except_conflicts();
  return osig_type(tree_of_type_decl_(id, decl), rs);
}

}  // namespace

ot::OutLabel tree_of_label(const LabelDeclaration* l) {
  return ot::OutLabel{std::string(ident::name(l->ld_id)), l->ld_mutable == MutableFlag::Mutable,
                      l->ld_atomic == AtomicFlag::Atomic, tree_of_typexp(Mode::Type, l->ld_type)};
}

std::vector<const ot::OutType*> tree_of_constructor_arguments(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple) return tree_of_typlist(Mode::Type, slice_vec(a.tuple));
  auto* r = ot::otyp(OT::Otyp_record);
  for (const LabelDeclaration* l : a.record) r->labels.push_back(tree_of_label(l));
  return {r};
}

void prepare_type_constructor_arguments(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple)
    for (TypeExpr* t : a.tuple) prepare_type(t);
  else
    for (const LabelDeclaration* l : a.record) prepare_type(l->ld_type);
}

ot::OutConstructor tree_of_single_constructor(const ConstructorDeclaration* cd) {
  std::string name(ident::name(cd->cd_id));
  const ot::OutType* ret = cd->cd_res ? tree_of_typexp(Mode::Type, cd->cd_res) : nullptr;
  auto args = tree_of_constructor_arguments(cd->cd_args);
  return ot::OutConstructor{name, args, ret};
}

void add_constructor_to_preparation(const ConstructorDeclaration* c) {
  prepare_type_constructor_arguments(c->cd_args);
  if (c->cd_res) prepare_type(c->cd_res);
}

void prepared_constructor(Formatter& ppf, const ConstructorDeclaration* c) {
  oprint::out_constr(ppf, tree_of_single_constructor(c));
}

const ot::OutSigItem* tree_of_type_declaration(Ident::t id, const TypeDeclaration* decl, RecStatus rs) {
  return with_hidden_items<const ot::OutSigItem*>(
      {BoundIdent{true, id}}, [&] { return tree_of_type_declaration_(id, decl, rs); });
}

const ot::OutSigItem* tree_of_prepared_type_declaration(Ident::t id, const TypeDeclaration* decl, RecStatus rs) {
  return osig_type(tree_of_type_decl_(id, decl), rs);
}

void add_type_declaration_to_preparation(Ident::t id, const TypeDeclaration* decl) { (void)prepare_decl(id, decl); }

void prepared_type_declaration(Ident::t id, Formatter& ppf, const TypeDeclaration* decl) {
  oprint::out_sig_item(ppf, tree_of_prepared_type_declaration(id, decl, RecStatus::Trec_first));
}

// ---- extension constructors ----

void add_extension_constructor_to_preparation(const ExtensionConstructor* ext) {
  std::vector<TypeExpr*> ty_params = filter_params(ext->ext_type_params);
  for (TypeExpr* t : ty_params) aliases::add(t);
  for (TypeExpr* t : ty_params) prepare_type(t);
  prepare_type_constructor_arguments(ext->ext_args);
  if (ext->ext_ret_type) prepare_type(ext->ext_ret_type);
}

std::pair<std::vector<const ot::OutType*>, const ot::OutType*> extension_constructor_args_and_ret_type_subtree(
    const ConstructorArguments& ext_args, TypeExpr* ext_ret_type) {
  const ot::OutType* ret = ext_ret_type ? tree_of_typexp(Mode::Type, ext_ret_type) : nullptr;
  auto args = tree_of_constructor_arguments(ext_args);
  return {args, ret};
}

namespace {

const ot::OutSigItem* prepared_tree_of_extension_constructor(Ident::t id, const ExtensionConstructor* ext,
                                                            ExtStatus es) {
  std::string ty_name = path::name(ext->ext_type_path);
  std::vector<TypeExpr*> ty_params = filter_params(ext->ext_type_params);
  std::vector<std::pair<ot::Variance, ot::Injectivity>> ty_variances;
  try {
    const TypeDeclaration* d =
        in_printing_env([&](env::t e) { return env::find_type(ext->ext_type_path, e); });
    for (variance::t v : d->type_variance) ty_variances.push_back(syntactic_variance(v));
  } catch (const env::NotFound&) {
    for (std::size_t i = 0; i < ty_params.size(); ++i)
      ty_variances.emplace_back(ot::Variance::NoVariance, ot::Injectivity::NoInjectivity);
  }
  auto type_param = [](const std::pair<ot::Variance, ot::Injectivity>& v, const ot::OutType* t) -> ot::OutTypeParam {
    if (t->k == OT::Otyp_var) return {false, t->s, v};
    return {false, "?", {ot::Variance::NoVariance, ot::Injectivity::NoInjectivity}};
  };
  std::vector<ot::OutTypeParam> params;
  auto body = [&] {
    for (TypeExpr* t : ty_params) aliases::add_printed(false, t);
    for (std::size_t i = 0; i < ty_params.size(); ++i)
      params.push_back(type_param(ty_variances.at(i), tree_of_typexp(Mode::Type, ty_params[i])));
  };
  if (!ext->ext_ret_type)
    body();  // normal constructor: same scope for parameters and the constructor
  else
    variable_names::with_local_names(body);  // gadt constructor: isolated scope for the type parameters
  std::string name(ident::name(id));
  auto [args, ret] = extension_constructor_args_and_ret_type_subtree(ext->ext_args, ext->ext_ret_type);
  auto* oe = make<ot::OutExtensionConstructor>();
  oe->oext_name = name;
  oe->oext_type_name = ty_name;
  oe->oext_type_params = params;
  oe->oext_args = args;
  oe->oext_ret_type = ret;
  oe->oext_private = ext->ext_private == PrivateFlag::Private;
  auto* it = make<ot::OutSigItem>();
  it->k = ot::OutSigItem::K::Osig_typext;
  it->ext = oe;
  switch (es) {
    case ExtStatus::Text_first: it->es = ot::OutExtStatus::Oext_first; break;
    case ExtStatus::Text_next: it->es = ot::OutExtStatus::Oext_next; break;
    case ExtStatus::Text_exception: it->es = ot::OutExtStatus::Oext_exception; break;
  }
  return it;
}

}  // namespace

const ot::OutSigItem* tree_of_extension_constructor(Ident::t id, const ExtensionConstructor* ext, ExtStatus es) {
  reset_except_conflicts();
  add_extension_constructor_to_preparation(ext);
  return prepared_tree_of_extension_constructor(id, ext, es);
}

void prepared_extension_constructor(Ident::t id, Formatter& ppf, const ExtensionConstructor* ext) {
  oprint::out_sig_item(ppf, prepared_tree_of_extension_constructor(id, ext, ExtStatus::Text_first));
}

// ---- value descriptions ----
namespace {

// Primitive.add_native_repr_attributes
const ot::OutType* add_native_repr_attributes(const ot::OutType* ty, const std::vector<const char*>& attrs,
                                              std::size_t from) {
  std::size_t n = attrs.size() - from;
  if (ty->k == OT::Otyp_arrow && n > 0) {
    const ot::OutType* b = add_native_repr_attributes(ty->t2, attrs, from + 1);
    const ot::OutType* a = ty->t1;
    if (attrs[from]) {
      auto* at = ot::otyp(OT::Otyp_attribute);
      at->t1 = a;
      at->attr.oattr_name = attrs[from];
      a = at;
    }
    auto* r = ot::otyp(OT::Otyp_arrow);
    r->label = ty->label;
    r->t1 = a;
    r->t2 = b;
    return r;
  }
  if (n == 1 && attrs[from]) {
    auto* at = ot::otyp(OT::Otyp_attribute);
    at->t1 = ty;
    at->attr.oattr_name = attrs[from];
    return at;
  }
  return ty;
}

bool is_unboxed(const NativeRepr& r) {
  return r.kind == NativeRepr::Kind::Unboxed_float || r.kind == NativeRepr::Kind::Unboxed_integer;
}
bool is_untagged(const NativeRepr& r) { return r.kind == NativeRepr::Kind::Untagged_immediate; }

// Primitive.print p osig_val_decl
void primitive_print(const PrimitiveDescription* p, ot::OutValDecl* vd) {
  std::vector<std::string> prims;
  prims.emplace_back(p->prim_name);
  if (!p->prim_native_name.empty()) prims.emplace_back(p->prim_native_name);
  auto for_all = [&](bool (*f)(const NativeRepr&)) {
    for (const NativeRepr& r : p->prim_native_repr_args)
      if (!f(r)) return false;
    return f(p->prim_native_repr_res);
  };
  bool all_unboxed = for_all(is_unboxed);
  bool all_untagged = for_all(is_untagged);
  std::vector<ot::OutAttribute> attrs;
  if (!p->prim_alloc) attrs.push_back({"noalloc"});
  if (all_unboxed)
    attrs.insert(attrs.begin(), {"unboxed"});
  else if (all_untagged)
    attrs.insert(attrs.begin(), {"untagged"});
  auto attr_of_native_repr = [&](const NativeRepr& r) -> const char* {
    switch (r.kind) {
      case NativeRepr::Kind::Same_as_ocaml_repr: return nullptr;
      case NativeRepr::Kind::Unboxed_float:
      case NativeRepr::Kind::Unboxed_integer: return all_unboxed ? nullptr : "unboxed";
      case NativeRepr::Kind::Untagged_immediate: return all_untagged ? nullptr : "untagged";
    }
    return nullptr;
  };
  std::vector<const char*> type_attrs;
  for (const NativeRepr& r : p->prim_native_repr_args) type_attrs.push_back(attr_of_native_repr(r));
  type_attrs.push_back(attr_of_native_repr(p->prim_native_repr_res));
  vd->oval_prims = prims;
  vd->oval_type = add_native_repr_attributes(vd->oval_type, type_attrs, 0);
  vd->oval_attributes = attrs;
}

}  // namespace

const ot::OutSigItem* tree_of_value_description(Ident::t id, const ValueDescription* decl) {
  std::string name(ident::name(id));
  prepare_for_printing({decl->val_type});
  const ot::OutType* ty = tree_of_typexp(Mode::Type_scheme, decl->val_type);
  auto* vd = make<ot::OutValDecl>();
  vd->oval_name = name;
  vd->oval_type = ty;
  if (decl->val_kind.kind == ValueKind::Kind::Val_prim) primitive_print(decl->val_kind.prim, vd);
  auto* it = make<ot::OutSigItem>();
  it->k = ot::OutSigItem::K::Osig_value;
  it->vd = vd;
  return it;
}

// ---- class types ----
namespace {

std::pair<TypeExpr*, std::vector<TypeExpr*>> method_type(const MethodPrivacy& priv, TypeExpr* ty) {
  if (!priv.is_private)
    if (auto* p = as<Tpoly>(types::get_desc(ty))) return {p->body, slice_vec(p->vars)};
  return {ty, {}};
}

void prepare_class_type_(const std::vector<TypeExpr*>& params, const ClassType* cty) {
  switch (cty->kind) {
    case ClassType::Kind::Cty_constr: {
      TypeExpr* row = bt::self_type_row(cty->cty);
      bool all_tvar = std::all_of(params.begin(), params.end(), [](TypeExpr* t) { return bt::is_Tvar(t); });
      if (aliases::memq(aliases::visited_objects, proxy(row)) || !all_tvar ||
          bt::deep_occur_list(row, slice_vec(cty->args)))
        prepare_class_type_(params, cty->cty);
      else
        for (TypeExpr* t : cty->args) prepare_type(t);
      break;
    }
    case ClassType::Kind::Cty_signature: {
      const ClassSignature* sign = cty->sign;
      // Self may have a name
      TypeExpr* px = proxy(sign->csig_self_row);
      if (aliases::memq(aliases::visited_objects, px))
        aliases::add_proxy(px);
      else
        aliases::visited_objects.insert(aliases::visited_objects.begin(), px);
      sign->csig_vars.iter([](std::string_view, const VarEntry& v) { prepare_type(v.ty); });
      sign->csig_meths.iter([](std::string_view, const MethEntry& m) { prepare_type(method_type(m.priv, m.ty).first); });
      break;
    }
    case ClassType::Kind::Cty_arrow:
      prepare_type(cty->arg);
      prepare_class_type_(params, cty->cty);
      break;
  }
}

const ot::OutClassSigItem* tree_of_method(Mode mode, std::string_view lab, const MethEntry& m) {
  auto [ty, tyl] = method_type(m.priv, m.ty);
  const ot::OutType* tty = tree_of_typexp(mode, ty);
  std::vector<TypeExpr*> reprs;
  for (TypeExpr* t : tyl) reprs.push_back(types::repr(t));
  variable_names::remove_names(reprs);
  auto* it = make<ot::OutClassSigItem>();
  it->k = ot::OutClassSigItem::K::Ocsg_method;
  it->name = std::string(lab);
  it->b1 = m.priv.is_private;
  it->b2 = m.virt == VirtualFlag::Virtual;
  it->t1 = tty;
  return it;
}

const ot::OutClassType* tree_of_class_type_(Mode mode, const std::vector<TypeExpr*>& params, const ClassType* cty) {
  auto* r = make<ot::OutClassType>();
  switch (cty->kind) {
    case ClassType::Kind::Cty_constr: {
      TypeExpr* row = bt::self_type_row(cty->cty);
      bool all_tvar = std::all_of(params.begin(), params.end(), [](TypeExpr* t) { return bt::is_Tvar(t); });
      if (aliases::memq(aliases::visited_objects, proxy(row)) || !all_tvar)
        return tree_of_class_type_(mode, params, cty->cty);
      std::optional<Namespace> ns = best_class_namespace(cty->path);
      auto tys = tree_of_typlist(Mode::Type_scheme, slice_vec(cty->args));
      r->k = ot::OutClassType::K::Octy_constr;
      r->id = tree_of_path_ns(ns, cty->path);
      r->tys = tys;
      return r;
    }
    case ClassType::Kind::Cty_signature: {
      const ClassSignature* sign = cty->sign;
      TypeExpr* px = proxy(sign->csig_self_row);
      const ot::OutType* self_ty = nullptr;
      if (aliases::is_aliased_proxy(px)) {
        auto* v = ot::otyp(OT::Otyp_var);
        v->non_gen = false;
        v->s = variable_names::name_of_type([] { return variable_names::new_name(); }, px);
        self_ty = v;
      }
      std::vector<const ot::OutClassSigItem*> csil;  // reversed
      for (auto& [t1, t2] : tree_of_constraints(params)) {
        auto* c = make<ot::OutClassSigItem>();
        c->k = ot::OutClassSigItem::K::Ocsg_constraint;
        c->t1 = t1;
        c->t2 = t2;
        csil.insert(csil.begin(), c);
      }
      // Vars.fold then List.rev: the variables in increasing order
      std::vector<std::pair<std::string_view, VarEntry>> all_vars;
      sign->csig_vars.iter([&](std::string_view l, const VarEntry& v) { all_vars.emplace_back(l, v); });
      for (auto& [l, v] : all_vars) {
        auto* c = make<ot::OutClassSigItem>();
        c->k = ot::OutClassSigItem::K::Ocsg_value;
        c->name = std::string(l);
        c->b1 = v.mut == MutableFlag::Mutable;
        c->b2 = v.virt == VirtualFlag::Virtual;
        c->t1 = tree_of_typexp(mode, v.ty);
        csil.insert(csil.begin(), c);
      }
      std::vector<std::pair<std::string_view, MethEntry>> all_meths;
      sign->csig_meths.iter([&](std::string_view l, const MethEntry& m) { all_meths.emplace_back(l, m); });
      for (auto& [l, m] : all_meths) csil.insert(csil.begin(), tree_of_method(mode, l, m));
      std::reverse(csil.begin(), csil.end());
      r->k = ot::OutClassType::K::Octy_signature;
      r->ty = self_ty;
      r->csil = csil;
      return r;
    }
    case ClassType::Kind::Cty_arrow: {
      ArgLabel lab = print_labels || bt::is_optional(cty->label) ? cty->label : ArgLabel::nolabel();
      const ot::OutType* tr;
      if (bt::is_optional(cty->label)) {
        auto* c = as<Tconstr>(types::get_desc(cty->arg));
        if (c && c->args.size() == 1 && path::same(c->path, predef::paths().option))
          tr = tree_of_typexp(mode, c->args[0]);
        else
          tr = stuff("<hidden>");
      } else {
        tr = tree_of_typexp(mode, cty->arg);
      }
      r->k = ot::OutClassType::K::Octy_arrow;
      r->label = out_label(lab);
      r->ty = tr;
      r->cty = tree_of_class_type_(mode, params, cty->cty);
      return r;
    }
  }
  return r;
}

ot::OutTypeParam tree_of_class_param(TypeExpr* param, variance::t v) {
  auto var = syntactic_variance(clflags::print_variance || !bt::is_Tvar(param), clflags::print_variance, v);
  const ot::OutType* t = tree_of_typexp(Mode::Type_scheme, param);
  if (t->k == OT::Otyp_var) return {t->non_gen, t->s, var};
  return {false, "?", var};
}

}  // namespace

const ot::OutClassType* tree_of_class_type(Mode mode, const ClassType* cty) { return tree_of_class_type_(mode, {}, cty); }
void prepare_class_type(const ClassType* cty) { prepare_class_type_({}, cty); }

const ot::OutSigItem* tree_of_class_declaration(Ident::t id, const ClassDeclaration* cl, RecStatus rs) {
  std::vector<TypeExpr*> params = filter_params(cl->cty_params);
  reset_except_conflicts();
  for (TypeExpr* t : params) aliases::add(t);
  prepare_class_type_(params, cl->cty_type);
  TypeExpr* px = proxy(bt::self_type_row(cl->cty_type));
  for (TypeExpr* t : params) prepare_type(t);
  for (TypeExpr* t : params) aliases::add_printed(false, t);
  if (aliases::is_aliased_proxy(px)) aliases::add_printed_proxy(false, px);
  bool vir_flag = cl->cty_new == nullptr;
  // (the tuple: the rec status, the class type, then the parameters)
  const ot::OutClassType* clt = tree_of_class_type_(Mode::Type_scheme, params, cl->cty_type);
  std::vector<ot::OutTypeParam> ps;
  for (std::size_t i = 0; i < params.size(); ++i) ps.push_back(tree_of_class_param(params[i], cl->cty_variance[i]));
  auto* it = make<ot::OutSigItem>();
  it->k = ot::OutSigItem::K::Osig_class;
  it->virt = vir_flag;
  it->name = std::string(ident::name(id));
  it->params = ps;
  it->clt = clt;
  it->rs = tree_of_rec(rs);
  return it;
}

const ot::OutSigItem* tree_of_cltype_declaration(Ident::t id, const ClassTypeDeclaration* cl, RecStatus rs) {
  std::vector<TypeExpr*> params = slice_vec(cl->clty_params);
  reset_except_conflicts();
  for (TypeExpr* t : params) aliases::add(t);
  prepare_class_type_(params, cl->clty_type);
  TypeExpr* px = proxy(bt::self_type_row(cl->clty_type));
  for (TypeExpr* t : params) prepare_type(t);
  for (TypeExpr* t : params) aliases::add_printed(false, t);
  aliases::mark_as_printed(px);
  const ClassSignature* sign = bt::signature_of_class_type(cl->clty_type);
  bool has_virtual_vars = false, has_virtual_meths = false;
  sign->csig_vars.iter([&](std::string_view, const VarEntry& v) {
    if (v.virt == VirtualFlag::Virtual) has_virtual_vars = true;
  });
  sign->csig_meths.iter([&](std::string_view, const MethEntry& m) {
    if (m.virt == VirtualFlag::Virtual) has_virtual_meths = true;
  });
  const ot::OutClassType* clt = tree_of_class_type_(Mode::Type_scheme, params, cl->clty_type);
  std::vector<ot::OutTypeParam> ps;
  for (std::size_t i = 0; i < params.size(); ++i) ps.push_back(tree_of_class_param(params[i], cl->clty_variance[i]));
  auto* it = make<ot::OutSigItem>();
  it->k = ot::OutSigItem::K::Osig_class_type;
  it->virt = has_virtual_vars || has_virtual_meths;
  it->name = std::string(ident::name(id));
  it->params = ps;
  it->clt = clt;
  it->rs = tree_of_rec(rs);
  return it;
}

// ---- module types and signatures ----
namespace {

BoundIdent ident_sigitem(const SignatureItem* it) {
  return {it->kind == SignatureItem::Kind::Sig_type, it->id};
}


env::t add_sigitem(env::t e, const signature_group::SigItem& x) {
  auto items = signature_group::flatten(x);
  return env::add_signature(slice(items), e);
}

const ot::OutModuleType* tree_of_modtype_(bool ellipsis, const ModuleType* mty);
std::vector<std::pair<env::t, std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>>>>
tree_of_signature_rec(env::t env2, Signature sg);

// tree_of_functor_parameter: the parameter's tree and the environment extension
std::pair<ot::OutFunctorParam, std::function<env::t(env::t)>> tree_of_functor_parameter(const FunctorParameter& p) {
  if (p.is_unit) return {ot::OutFunctorParam{false, std::nullopt, nullptr}, [](env::t e) { return e; }};
  std::optional<std::string> name;
  std::function<env::t(env::t)> fenv = [](env::t e) { return e; };
  if (p.id) {
    name = std::string(ident::name(p.id));
    Ident::t id = p.id;
    const ModuleType* mty = p.mty;
    fenv = [id, mty](env::t e) { return env::add_module(id, ModulePresence::Mp_present, mty, e, true); };
  }
  return {ot::OutFunctorParam{true, name, tree_of_modtype_(false, p.mty)}, fenv};
}

const ot::OutModuleType* tree_of_modtype_(bool ellipsis, const ModuleType* mty) {
  auto* r = make<ot::OutModuleType>();
  switch (mty->kind) {
    case ModuleType::Kind::Mty_ident:
      r->k = ot::OutModuleType::K::Omty_ident;
      r->id = tree_of_path_ns(Namespace::Module_type, mty->path);
      return r;
    case ModuleType::Kind::Mty_signature: {
      r->k = ot::OutModuleType::K::Omty_signature;
      if (ellipsis) {
        auto* e = make<ot::OutSigItem>();
        e->k = ot::OutSigItem::K::Osig_ellipsis;
        r->sg = {e};
      } else {
        r->sg = tree_of_signature(mty->sign);
      }
      return r;
    }
    case ModuleType::Kind::Mty_functor: {
      auto [param, fenv] = tree_of_functor_parameter(mty->param);
      const ot::OutModuleType* res =
          wrap_env<const ot::OutModuleType*>(false, fenv, [&] { return tree_of_modtype_(ellipsis, mty->res); });
      r->k = ot::OutModuleType::K::Omty_functor;
      r->param = param;
      r->res = res;
      return r;
    }
    case ModuleType::Kind::Mty_alias:
      r->k = ot::OutModuleType::K::Omty_alias;
      r->id = tree_of_path_ns(Namespace::Module, mty->path);
      return r;
  }
  return r;
}

std::pair<env::t, std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>>> trees_of_recursive_sigitem_group(
    env::t e, const signature_group::RecGroup& g) {
  auto display = [](const signature_group::SigItem& x) {
    return std::pair<const SignatureItem*, const ot::OutSigItem*>{x.src, tree_of_sigitem(x.src)};
  };
  env::t e2 = env::add_signature(slice(g.pre_ghosts), e);
  if (!g.group.is_rec) {
    const auto& x = g.group.items[0];
    return {add_sigitem(e2, x), {display(x)}};
  }
  std::vector<BoundIdent> ids;
  for (const auto& x : g.group.items) ids.push_back(ident_sigitem(x.src));
  // (the tuple: the trees first, then the environment)
  auto trees = with_hidden_items<std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>>>(ids, [&] {
    std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>> l;
    for (const auto& x : g.group.items) l.push_back(display(x));
    return l;
  });
  env::t e3 = e2;
  for (const auto& x : g.group.items) e3 = add_sigitem(e3, x);
  return {e3, trees};
}

std::vector<std::pair<env::t, std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>>>>
tree_of_signature_rec(env::t env2, Signature sg) {
  std::vector<signature_group::RecGroup> structured = signature_group::seq(sg);
  set_printing_env(env2);
  std::vector<std::pair<env::t, std::vector<std::pair<const SignatureItem*, const ot::OutSigItem*>>>> out;
  for (const auto& group : structured) {
    env::t e = cur_env();
    auto [e2, group_trees] = trees_of_recursive_sigitem_group(e, group);
    set_printing_env(e2);
    out.emplace_back(e, group_trees);
  }
  return out;
}

// the payload is `PStr []`
bool empty_pstr(const Attribute* a) {
  if (a->ast) return a->ast->attr_payload.kind == parsetree::Payload::Kind::PStr && a->ast->attr_payload.str.empty();
  const OValue* v = a->attr_payload;
  return v && v->kind == OValue::Kind::Block && v->tag == 0 && v->fields.size() == 1 &&
         v->fields[0]->kind == OValue::Kind::Int && v->fields[0]->i == 0;
}

bool has_ellipsis_attribute(const Attributes& attrs) {
  for (const Attribute* a : attrs)
    if (a->attr_name == "..." && empty_pstr(a)) return true;
  return false;
}

}  // namespace

std::vector<const ot::OutSigItem*> tree_of_signature(Signature sg) {
  return wrap_env<std::vector<const ot::OutSigItem*>>(false, [](env::t e) { return e; }, [&] {
    auto tree_groups = tree_of_signature_rec(cur_env(), sg);
    std::vector<const ot::OutSigItem*> l;
    for (auto& [_, g] : tree_groups)
      for (auto& [__, t] : g) l.push_back(t);
    return l;
  });
}

const ot::OutSigItem* tree_of_sigitem(const SignatureItem* it) {
  switch (it->kind) {
    case SignatureItem::Kind::Sig_value: return tree_of_value_description(it->id, it->value);
    // (out_type.ml's own tree_of_type_declaration, defined before the
    // exported one that hides the declared name)
    case SignatureItem::Kind::Sig_type: return tree_of_type_declaration_(it->id, it->type, it->rec);
    case SignatureItem::Kind::Sig_typext: return tree_of_extension_constructor(it->id, it->ext, it->ext_status);
    case SignatureItem::Kind::Sig_module: {
      bool ellipsis = has_ellipsis_attribute(it->md->md_attributes);
      auto* r = make<ot::OutSigItem>();
      r->k = ot::OutSigItem::K::Osig_module;
      r->name = std::string(ident::name(it->id));
      r->mty = tree_of_modtype_(ellipsis, it->md->md_type);
      r->rs = tree_of_rec(it->rec);
      return r;
    }
    case SignatureItem::Kind::Sig_modtype: return tree_of_modtype_declaration(it->id, it->mtd);
    case SignatureItem::Kind::Sig_class: return tree_of_class_declaration(it->id, it->cls, it->rec);
    case SignatureItem::Kind::Sig_class_type: return tree_of_cltype_declaration(it->id, it->clty, it->rec);
  }
  return nullptr;
}

const ot::OutModuleType* tree_of_modtype(const ModuleType* mty) { return tree_of_modtype_(false, mty); }

ot::OutFunctorParam tree_of_functor_parameter_tree(const FunctorParameter& p) {
  return tree_of_functor_parameter(p).first;
}

const ot::OutSigItem* tree_of_modtype_declaration(Ident::t id, const ModtypeDeclaration* decl) {
  const ot::OutModuleType* mty;
  if (!decl->mtd_type) {
    auto* a = make<ot::OutModuleType>();
    a->k = ot::OutModuleType::K::Omty_abstract;
    mty = a;
  } else {
    mty = tree_of_modtype(decl->mtd_type);
  }
  auto* r = make<ot::OutSigItem>();
  r->k = ot::OutSigItem::K::Osig_modtype;
  r->name = std::string(ident::name(id));
  r->mty = mty;
  return r;
}

const ot::OutSigItem* tree_of_module(Ident::t id, const ModuleType* mty, RecStatus rs) {
  auto* r = make<ot::OutSigItem>();
  r->k = ot::OutSigItem::K::Osig_module;
  r->name = std::string(ident::name(id));
  r->mty = tree_of_modtype_(false, mty);
  r->rs = tree_of_rec(rs);
  return r;
}

// ---- type expansions ----

bool same_path(TypeExpr* t, TypeExpr* t2) {
  if (types::eq_type(t, t2)) return true;
  auto* c1 = as<Tconstr>(types::get_desc(t));
  auto* c2 = as<Tconstr>(types::get_desc(t2));
  if (!c1 || !c2) return false;
  auto [p1, s1] = best_type_path(c1->path);
  auto [p2, s2] = best_type_path(c2->path);
  if (s1.k == ParamSubst::K::Nth && s2.k == ParamSubst::K::Nth) return s1.n == s2.n;
  if (s1.k != ParamSubst::K::Nth && s2.k != ParamSubst::K::Nth && path::same(p1, p2)) {
    auto tl = apply_subst(s1, slice_vec(c1->args));
    auto tl2 = apply_subst(s2, slice_vec(c2->args));
    if (tl.size() != tl2.size()) return false;
    for (std::size_t i = 0; i < tl.size(); ++i)
      if (!types::eq_type(tl[i], tl2[i])) return false;
    return true;
  }
  return false;
}

// Same t (expanded == nullptr) | Diff (t, expanded)
ExpansionDiff trees_of_type_expansion(Mode mode, const ExpansionPair& e) {
  TypeExpr* t = e.ty;
  TypeExpr* t2 = e.expanded;
  aliases::reset();
  aliases::mark_loops(t);
  if (same_path(t, t2)) {
    aliases::add_delayed(proxy(t));
    return {tree_of_typexp(mode, t), nullptr};
  }
  aliases::mark_loops(t2);
  TypeExpr* t3 = proxy(t) == proxy(t2) ? ctype::unalias(t2) : t2;
  // beware order matter due to side effect, e.g. when printing object types
  const ot::OutType* first = tree_of_typexp(mode, t);
  const ot::OutType* second = tree_of_typexp(mode, t3);
  if (ot::equal(first, second)) return {first, nullptr};
  return {first, second};
}

void pp_type(Formatter& ppf, const ot::OutType* t) {
  pp_open_stag(ppf, "inline_code");
  oprint::out_type(ppf, t);
  pp_close_stag(ppf);
}

void pp_type_expansion(Formatter& ppf, const ExpansionDiff& d) {
  if (!d.expanded) pp_type(ppf, d.ty);
  else fprintf(ppf, "@[<2>%a@ =@ %a@]", pr(pp_type, d.ty), pr(pp_type, d.expanded));
}

// Hide variant name and var, to force printing the expanded type
TypeExpr* hide_variant_name(TypeExpr* t) {
  auto* v = as<Tvariant>(types::get_desc(t));
  if (!v) return t;
  RowDescRepr r = types::row_repr(v->row);
  if (!r.name) return t;
  return bt::newty2(types::get_level(t),
                    types::tvariant(types::create_row(slice(r.fields), ctype::newvar2(types::get_level(r.more)), r.closed,
                                                   r.fixed, nullptr)));
}

ExpansionPair prepare_expansion(const ExpansionPair& e) {
  TypeExpr* expanded = hide_variant_name(e.expanded);
  variable_names::reserve(e.ty);
  if (!same_path(e.ty, expanded)) variable_names::reserve(expanded);
  return {e.ty, expanded};
}

}  // namespace cppcaml::typing::out_type
