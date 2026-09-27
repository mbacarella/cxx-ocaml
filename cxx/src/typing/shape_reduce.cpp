// Port of typing/shape_reduce.ml's Make(Local_reduce params): see
// shape_reduce.hpp.  The normal forms and delayed normal forms are the
// .ml's; the two memo tables are its Hashtbl's with OCaml's structural
// equality on keys, done here by interning each term into a canonical
// number (the read-back table is what shares equal normal forms in the
// result, which the .cmt's marshaling then shows).
#include "cppcaml/typing/shape_reduce.hpp"

#include <map>
#include <tuple>
#include <vector>

namespace cppcaml::typing::shape_reduce {
namespace {

using shape::Item;
using shape::Shape;
using SK = Shape::Kind;

struct Thunk;  // delayed_nf = Thunk of local_env * t
struct IdentCmp {
  int operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b); }
};
using LocalEnv = PMap<Ident::t, const Thunk*, IdentCmp>;  // delayed_nf option (nullptr = None)
using ThunkMap = PMap<Item, const Thunk*, shape::ItemCmp>;

struct Thunk {
  LocalEnv env;
  shape::t t;
};

// Map.map into another value type: the same tree, node for node (l, d, r)
template <class V2, class V1, class F>
const PMapNode<Item, V2>* map_nodes(const PMapNode<Item, V1>* n, F&& f) {
  if (!n) return nullptr;
  const PMapNode<Item, V2>* l = map_nodes<V2>(n->l, f);
  V2 d = f(n->d);
  const PMapNode<Item, V2>* r = map_nodes<V2>(n->r, f);
  return make<PMapNode<Item, V2>>(PMapNode<Item, V2>{l, n->v, d, r, n->h});
}

struct NF {
  enum class Kind : std::uint8_t {
    NVar, NApp, NAbs, NStruct, NAlias, NProj, NLeaf, NPack, NComp_unit, NError
  };
  // uid : Uid.t option -- the option block of the shape it came from
  bool has_uid = false;
  Uid uid{};
  const void* uid_obj = nullptr;
  Kind kind = Kind::NLeaf;
  Ident::t var = nullptr;          // NVar / NAbs / NPack
  const NF* a = nullptr;           // NApp fn / NProj
  const NF* b = nullptr;           // NApp arg
  LocalEnv env;                    // NAbs
  shape::t body = nullptr;         // NAbs
  const Thunk* dnf = nullptr;      // NAbs / NAlias
  ThunkMap items;                  // NStruct
  Item item{};                     // NProj
  std::string_view str;            // NComp_unit / NError
  bool approximated = false;
};

// ---- structural interning (OCaml's compare = 0) ----------------------------
class Interner {
 public:
  long ident(Ident::t id) {
    if (!id) return 0;
    if (auto it = ids_.find(id); it != ids_.end()) return it->second;
    std::vector<long> k{1, static_cast<long>(id->kind)};
    if (id->kind == Ident::Kind::Unscoped) {
      k.push_back(reinterpret_cast<long>(id->us));
    } else {
      k.push_back(strid(id->name_));
      k.push_back(id->stamp_);
      k.push_back(id->scope_);
    }
    return ids_[id] = canon(k);
  }
  long uid(bool has, const Uid& u) {
    if (!has) return 0;
    return canon({2, static_cast<long>(u.kind), strid(u.comp_unit), u.id, static_cast<long>(u.from)});
  }
  long item(const Item& it) { return canon({3, strid(it.name), static_cast<long>(it.kind)}); }
  long shape(shape::t t) {
    if (auto it = shapes_.find(t); it != shapes_.end()) return it->second;
    std::vector<long> k{4, uid(t->has_uid, t->uid), static_cast<long>(t->kind), t->approximated ? 1 : 0};
    switch (t->kind) {
      case SK::Var: case SK::Pack: k.push_back(ident(t->var)); break;
      case SK::Abs: k.push_back(ident(t->var)); k.push_back(shape(t->t1)); break;
      case SK::App: k.push_back(shape(t->t1)); k.push_back(shape(t->t2)); break;
      case SK::Struct: k.push_back(shape_map(t->map.root())); break;
      case SK::Alias: k.push_back(shape(t->t1)); break;
      case SK::Proj: k.push_back(shape(t->t1)); k.push_back(item(t->item)); break;
      case SK::Comp_unit: case SK::Error: k.push_back(strid(t->str)); break;
      case SK::Leaf: break;
    }
    return shapes_[t] = canon(k);
  }
  long shape_map(const PMapNode<Item, shape::t>* n) {
    if (!n) return 0;
    if (auto it = smaps_.find(n); it != smaps_.end()) return it->second;
    return smaps_[n] = canon({5, shape_map(n->l), item(n->v), shape(n->d), shape_map(n->r), n->h});
  }
  long env(const PMapNode<Ident::t, const Thunk*>* n) {
    if (!n) return 0;
    if (auto it = envs_.find(n); it != envs_.end()) return it->second;
    return envs_[n] = canon({6, env(n->l), ident(n->v), n->d ? thunk(n->d) : 0, env(n->r), n->h});
  }
  long thunk(const Thunk* th) {
    if (auto it = thunks_.find(th); it != thunks_.end()) return it->second;
    return thunks_[th] = canon({7, env(th->env.root()), shape(th->t)});
  }
  long thunk_map(const PMapNode<Item, const Thunk*>* n) {
    if (!n) return 0;
    if (auto it = tmaps_.find(n); it != tmaps_.end()) return it->second;
    return tmaps_[n] = canon({8, thunk_map(n->l), item(n->v), thunk(n->d), thunk_map(n->r), n->h});
  }
  long nf(const NF* x) {
    if (auto it = nfs_.find(x); it != nfs_.end()) return it->second;
    std::vector<long> k{9, uid(x->has_uid, x->uid), static_cast<long>(x->kind), x->approximated ? 1 : 0};
    using NK = NF::Kind;
    switch (x->kind) {
      case NK::NVar: case NK::NPack: k.push_back(ident(x->var)); break;
      case NK::NApp: k.push_back(nf(x->a)); k.push_back(nf(x->b)); break;
      case NK::NAbs:
        k.push_back(env(x->env.root()));
        k.push_back(ident(x->var));
        k.push_back(shape(x->body));
        k.push_back(thunk(x->dnf));
        break;
      case NK::NStruct: k.push_back(thunk_map(x->items.root())); break;
      case NK::NAlias: k.push_back(thunk(x->dnf)); break;
      case NK::NProj: k.push_back(nf(x->a)); k.push_back(item(x->item)); break;
      case NK::NComp_unit: case NK::NError: k.push_back(strid(x->str)); break;
      case NK::NLeaf: break;
    }
    return nfs_[x] = canon(k);
  }

 private:
  long strid(std::string_view s) {
    auto [it, fresh] = strs_.try_emplace(std::string(s), static_cast<long>(strs_.size()) + 1);
    return it->second;
  }
  long canon(const std::vector<long>& k) {
    auto [it, fresh] = canon_.try_emplace(k, static_cast<long>(canon_.size()) + 1);
    return it->second;
  }
  std::map<std::string, long> strs_;
  std::map<std::vector<long>, long> canon_;
  std::map<Ident::t, long> ids_;
  std::map<shape::t, long> shapes_;
  std::map<const void*, long> smaps_, envs_, tmaps_;
  std::map<const Thunk*, long> thunks_;
  std::map<const NF*, long> nfs_;
};

class Reducer {
 public:
  // reduce_ env t, memoized on (local_env, t)
  const NF* reduce(const LocalEnv& local_env, shape::t t) {
    auto key = std::make_pair(in_.env(local_env.root()), in_.shape(t));
    if (auto it = reduce_memo_.find(key); it != reduce_memo_.end()) return it->second;
    const NF* r = reduce__(local_env, t);
    reduce_memo_[key] = r;
    return r;
  }
  const NF* force(const Thunk* th) { return reduce(th->env, th->t); }

  // read_back env nf, memoized on the normal form
  shape::t read_back(const NF* nf) {
    long key = in_.nf(nf);
    if (auto it = read_back_memo_.find(key); it != read_back_memo_.end()) return it->second;
    shape::t r = read_back_(nf);
    read_back_memo_[key] = r;
    return r;
  }

 private:
  const Thunk* delay(const LocalEnv& env, shape::t t) { return make<Thunk>(Thunk{env, t}); }
  NF* ret(shape::t t, NF::Kind k) {  // { uid = t.uid; desc; approximated = t.approximated }
    NF* n = make<NF>();
    n->has_uid = t->has_uid;
    n->uid = t->uid;
    n->uid_obj = t->uid_obj;
    n->kind = k;
    n->approximated = t->approximated;
    return n;
  }
  const NF* force_aliases(const NF* nf) {
    while (nf->kind == NF::Kind::NAlias) nf = force(nf->dnf);
    return nf;
  }
  const NF* reset_uid_if_new_binding(shape::t t, const NF* t2) {
    if (!t->has_uid) return t2;
    NF* n = make<NF>(*t2);
    n->has_uid = true;
    n->uid = t->uid;
    n->uid_obj = t->uid_obj;
    return n;
  }
  const NF* reduce__(const LocalEnv& local_env, shape::t t) {
    using NK = NF::Kind;
    switch (t->kind) {
      case SK::Comp_unit: {  // Params.read_unit_shape = None
        NF* n = ret(t, NK::NComp_unit);
        n->str = t->str;
        return n;
      }
      case SK::App: {
        const NF* f = force_aliases(reduce(local_env, t->t1));
        if (f->kind == NK::NAbs) {
          const Thunk* arg = delay(local_env, t->t2);
          LocalEnv env = f->env.add(f->var, arg);
          return reset_uid_if_new_binding(t, reduce(env, f->body));
        }
        const NF* arg = reduce(local_env, t->t2);
        NF* n = ret(t, NK::NApp);
        n->a = f;
        n->b = arg;
        return n;
      }
      case SK::Proj: {
        const NF* str = force_aliases(reduce(local_env, t->t1));
        if (str->kind == NK::NStruct)
          if (const Thunk* const* th = str->items.find_opt(t->item))
            return reset_uid_if_new_binding(t, force(*th));
        NF* n = ret(t, NK::NProj);
        n->a = str;
        n->item = t->item;
        return n;
      }
      case SK::Abs: {
        const Thunk* body_nf = delay(local_env.add(t->var, nullptr), t->t1);
        NF* n = ret(t, NK::NAbs);
        n->env = local_env;
        n->var = t->var;
        n->body = t->t1;
        n->dnf = body_nf;
        return n;
      }
      case SK::Var: {
        if (const Thunk* const* def = local_env.find_opt(t->var)) {
          if (!*def) {
            NF* n = ret(t, NK::NVar);
            n->var = t->var;
            return n;
          }
          const NF* nf = force(*def);
          if (nf->has_uid) return nf;
          NF* n = make<NF>(*nf);  // { nf with uid = t.uid }
          n->has_uid = t->has_uid;
          n->uid = t->uid;
          n->uid_obj = t->uid_obj;
          return n;
        }
        // find_shape Env.empty: Not_found
        NF* n = ret(t, NK::NVar);
        n->var = t->var;
        return n;
      }
      case SK::Leaf: return ret(t, NK::NLeaf);
      case SK::Pack: {
        NF* n = ret(t, NK::NPack);
        n->var = t->var;
        return n;
      }
      case SK::Struct: {
        NF* n = ret(t, NK::NStruct);
        n->items = ThunkMap(map_nodes<const Thunk*>(t->map.root(), [&](shape::t s) { return delay(local_env, s); }));
        return n;
      }
      case SK::Alias: {
        NF* n = ret(t, NK::NAlias);
        n->dnf = delay(local_env, t->t1);
        return n;
      }
      case SK::Error: {  // approx_nf (return (NError s))
        NF* n = ret(t, NK::NError);
        n->str = t->str;
        n->approximated = true;
        return n;
      }
    }
    throw std::logic_error("Shape_reduce.reduce__");
  }
  shape::t read_back_(const NF* nf) {
    using NK = NF::Kind;
    Shape* s = make<Shape>();
    s->has_uid = nf->has_uid;
    s->uid = nf->uid;
    s->uid_obj = nf->uid_obj;
    s->approximated = nf->approximated;
    switch (nf->kind) {
      case NK::NVar: s->kind = SK::Var; s->var = nf->var; break;
      case NK::NApp: {  // App (read_back nft, read_back nfu): right to left
        shape::t u = read_back(nf->b);
        shape::t f = read_back(nf->a);
        s->kind = SK::App;
        s->t1 = f;
        s->t2 = u;
        break;
      }
      case NK::NAbs:
        s->kind = SK::Abs;
        s->var = nf->var;
        s->t1 = read_back(force(nf->dnf));
        break;
      case NK::NStruct:
        s->kind = SK::Struct;
        s->map = shape::ItemMap(map_nodes<shape::t>(nf->items.root(), [&](const Thunk* th) { return read_back(force(th)); }));
        break;
      case NK::NAlias: s->kind = SK::Alias; s->t1 = read_back(force(nf->dnf)); break;
      case NK::NProj:
        s->kind = SK::Proj;
        s->t1 = read_back(nf->a);
        s->item = nf->item;
        break;
      case NK::NLeaf: s->kind = SK::Leaf; break;
      case NK::NPack: s->kind = SK::Pack; s->var = nf->var; break;
      case NK::NComp_unit: s->kind = SK::Comp_unit; s->str = nf->str; break;
      case NK::NError: s->kind = SK::Error; s->str = nf->str; break;
    }
    return s;
  }

  Interner in_;
  std::map<std::pair<long, long>, const NF*> reduce_memo_;
  std::map<long, shape::t> read_back_memo_;
};

Reducer* g_reducer = nullptr;

}  // namespace

void reset() {
  delete g_reducer;
  g_reducer = nullptr;
}

shape::t local_reduce_empty(shape::t t) {
  // Local_store.s_table: the memo tables live for the compilation unit
  if (!g_reducer) g_reducer = new Reducer;
  return g_reducer->read_back(g_reducer->reduce(LocalEnv{}, t));
}

}  // namespace cppcaml::typing::shape_reduce
