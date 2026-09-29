// Port of typing/shape_reduce.ml's Make(Local_reduce params) and
// local_reduce_for_uid: see shape_reduce.hpp.  The normal forms and delayed normal forms are the
// .ml's; the two memo tables are its Hashtbl's with OCaml's structural
// equality on keys, done here by interning each term into a canonical
// number (the read-back table is what shares equal normal forms in the
// result, which the .cmt's marshaling then shows).
#include "cppcaml/typing/shape_reduce.hpp"

#include <functional>

#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include "cppcaml/typing/cmt_format.hpp"
#include "cppcaml/typing/types.hpp"

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
  // the call's { fuel; global_env } (the memo tables are the unit's)
  env::t global_env = nullptr;
  long fuel = 0;

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

  static bool is_stuck_on_comp_unit(const NF* nf) {
    using NK = NF::Kind;
    while (true) {
      switch (nf->kind) {
        case NK::NVar: return false;  // (should not happen if we only reduce closed terms)
        case NK::NApp: case NK::NProj: nf = nf->a; continue;
        case NK::NComp_unit: return true;
        default: return false;
      }
    }
  }

  const Result* reduce_aliases_for_uid(const NF* nf) {
    using RK = Result::Kind;
    auto* r = make<Result>();
    if (nf->has_uid && nf->kind == NF::Kind::NAlias && !nf->approximated) {
      const Result* result = reduce_aliases_for_uid(force(nf->dnf));
      r->kind = RK::Resolved_alias;
      r->has_uid = true;
      r->uid = nf->uid;
      r->alias = result;
    } else if (nf->has_uid && !nf->approximated) {
      r->kind = RK::Resolved;
      r->has_uid = true;
      r->uid = nf->uid;
    } else if (nf->approximated) {
      r->kind = RK::Approximated;
      r->has_uid = nf->has_uid;
      r->uid = nf->uid;
      r->uid_obj = nf->uid_obj;
    } else {
      r->kind = RK::Missing_uid;
      r->shape = read_back(nf);
    }
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
    if (fuel < 0) {  // approx_nf (return (NError "NoFuelLeft"))
      NF* n = ret(t, NK::NError);
      n->str = OCAML_LIT("NoFuelLeft");
      n->approximated = true;
      return n;
    }
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
        // find_shape global_env id (Env.shape_of_path ~namespace:Module)
        shape::t res = nullptr;
        try {
          res = env::shape_of_path(shape::SigComponentKind::Module, global_env, Path::pident(t->var));
        } catch (const env::NotFound&) {
        }
        if (!res || in_.shape(res) == in_.shape(t)) {  // exception Not_found | res when res = t
          NF* n = ret(t, NK::NVar);
          n->var = t->var;
          return n;
        }
        --fuel;
        return reduce(local_env, res);
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


// Local_store.s_table: the memo tables live for the compilation unit
static Reducer& reducer() {
  if (!g_reducer) g_reducer = new Reducer;
  return *g_reducer;
}

// Local_reduce.reduce global_env t
shape::t local_reduce(env::t global_env, shape::t t) {
  Reducer& r = reducer();
  r.global_env = global_env;
  r.fuel = 10;
  return r.read_back(r.reduce(LocalEnv{}, t));
}

shape::t local_reduce_empty(shape::t t) { return local_reduce(env::empty(), t); }

// Local_reduce.reduce_for_uid global_env t
static const Result* reduce_for_uid(env::t global_env, shape::t t) {
  Reducer& r = reducer();
  r.global_env = global_env;
  r.fuel = 10;
  const NF* nf = r.reduce(LocalEnv{}, t);
  if (Reducer::is_stuck_on_comp_unit(nf)) {
    auto* res = make<Result>();
    res->kind = Result::Kind::Unresolved;
    res->shape = r.read_back(nf);
    return res;
  }
  return r.reduce_aliases_for_uid(nf);
}

// ---- uid_memo: Ident_and_uid.Tbl (Local_store.s_table ... create 16) ----------------
// The table is Hashtbl.Make (Identifiable.Pair (Ident) (Uid)): its equality
// compares idents by NAME (Ident.equal) while its hash mixes in the stamp
// (Ident.hash) -- so two parents of one name share an entry exactly when
// their keys land in one bucket.  Hence the hash is runtime/hash.c's and the
// buckets are stdlib/hashtbl.ml's (prepend, double past 2x, order kept).
namespace ocaml_hash {
using u32 = std::uint32_t;
inline u32 rotl(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
inline u32 mix(u32 h, u32 d) {
  d *= 0xcc9e2d51u;
  d = rotl(d, 15);
  d *= 0x1b873593u;
  h ^= d;
  h = rotl(h, 13);
  return h * 5 + 0xe6546b64u;
}
inline u32 mix_intnat(u32 h, std::int64_t d) {  // an OCaml value word
  u32 n = static_cast<u32>((d >> 32) ^ (d >> 63) ^ d);
  return mix(h, n);
}
inline u32 mix_string(u32 h, std::string_view s) {
  std::size_t len = s.size(), i = 0;
  for (; i + 4 <= len; i += 4) {
    u32 w = static_cast<u32>(static_cast<unsigned char>(s[i])) |
            static_cast<u32>(static_cast<unsigned char>(s[i + 1])) << 8 |
            static_cast<u32>(static_cast<unsigned char>(s[i + 2])) << 16 |
            static_cast<u32>(static_cast<unsigned char>(s[i + 3])) << 24;
    h = mix(h, w);
  }
  u32 w = 0;
  switch (len & 3) {
    case 3: w = static_cast<u32>(static_cast<unsigned char>(s[i + 2])) << 16; [[fallthrough]];
    case 2: w |= static_cast<u32>(static_cast<unsigned char>(s[i + 1])) << 8; [[fallthrough]];
    case 1:
      w |= static_cast<u32>(static_cast<unsigned char>(s[i]));
      h = mix(h, w);
      break;
    default: break;
  }
  return h ^ static_cast<u32>(len);
}
inline long final_(u32 h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return static_cast<long>(h & 0x3FFFFFFFu);
}
inline std::int64_t val_int(long n) { return (static_cast<std::int64_t>(n) << 1) + 1; }
inline u32 header(std::size_t wosize, int tag) { return static_cast<u32>((wosize << 10) | static_cast<unsigned>(tag)); }

// Hashtbl.hash (uid : Shape.Uid.t): caml_hash 10 100 0, breadth first
long uid(const Uid& u) {
  u32 h = 0;
  switch (u.kind) {
    case Uid::Kind::Internal: return final_(mix_intnat(h, val_int(0)));
    case Uid::Kind::Compilation_unit:  // tag 0, [string]
      h = mix(h, header(1, 0));
      return final_(mix_string(h, u.comp_unit));
    case Uid::Kind::Item:  // tag 1, [comp_unit; id; from (Intf | Impl)]
      h = mix(h, header(3, 1));
      h = mix_string(h, u.comp_unit);
      h = mix_intnat(h, val_int(u.id));
      return final_(mix_intnat(h, val_int(u.from == Uid::From::Intf ? 0 : 1)));
    case Uid::Kind::Local_opaque_item:  // tag 2, [comp_unit; id]
      h = mix(h, header(2, 2));
      h = mix_string(h, u.comp_unit);
      return final_(mix_intnat(h, val_int(u.id)));
    case Uid::Kind::Predef:  // tag 3, [string]
      h = mix(h, header(1, 3));
      return final_(mix_string(h, u.comp_unit));
  }
  return 0;
}
// Hashtbl.hash (a, b) of two ints
long pair(long a, long b) {
  u32 h = mix(0, header(2, 0));
  h = mix_intnat(h, val_int(a));
  return final_(mix_intnat(h, val_int(b)));
}
}  // namespace ocaml_hash

// Ident.hash: (Char.code (name i).[0]) lxor (stamp i)
static long ident_hash(Ident::t id) {
  std::string_view n = ident::name(id);
  long st = (id->kind == Ident::Kind::Local || id->kind == Ident::Kind::Scoped || id->kind == Ident::Kind::Unscoped)
                ? ident::stamp(id)
                : 0;
  return static_cast<long>(static_cast<unsigned char>(n[0])) ^ st;
}
// Ident.equal: the names (a Predef ident by its stamp)
static bool ident_equal(Ident::t a, Ident::t b) {
  using IK = Ident::Kind;
  if (a->kind != b->kind) return false;
  if (a->kind == IK::Predef) return ident::stamp(a) == ident::stamp(b);
  return ident::name(a) == ident::name(b);
}

namespace {
struct UidMemo {
  struct Entry {
    Ident::t id;
    Uid decl_uid;
    Uid uid;
  };
  std::vector<std::vector<Entry>> data = std::vector<std::vector<Entry>>(16);  // bucket heads first
  std::size_t size = 0;

  std::size_t index(Ident::t id, const Uid& u) const {
    return static_cast<std::size_t>(ocaml_hash::pair(ident_hash(id), ocaml_hash::uid(u))) & (data.size() - 1);
  }
  const Uid* find_opt(Ident::t id, const Uid& u) const {
    for (const Entry& e : data[index(id, u)])
      if (ident_equal(id, e.id) && uid::equal(u, e.decl_uid)) return &e.uid;
    return nullptr;
  }
  void add(Ident::t id, const Uid& u, const Uid& v) {
    auto& b = data[index(id, u)];
    b.insert(b.begin(), Entry{id, u, v});
    if (++size > data.size() * 2) {  // resize: double, each bucket's order kept
      std::vector<std::vector<Entry>> odata = std::move(data);
      data = std::vector<std::vector<Entry>>(odata.size() * 2);
      for (auto& ob : odata)
        for (Entry& e : ob) data[index(e.id, e.decl_uid)].push_back(e);
    }
  }
};
}  // namespace
static UidMemo* g_uid_memo = nullptr;

static Uid make_definition_uid(Ident::t parent_id, const Uid& decl_uid) {
  if (!g_uid_memo) g_uid_memo = new UidMemo;
  if (const Uid* u = g_uid_memo->find_opt(parent_id, decl_uid)) return *u;
  Uid uid = uid::mk_local_opaque(env::get_current_unit());
  cmt_format::record_declaration_dependency(cmt_format::DepKind::Definition_to_declaration, uid, decl_uid);
  g_uid_memo->add(parent_id, decl_uid, uid);
  return uid;
}

static Ident::t stuck_on_var_or_pack(shape::t t) {
  while (true) {
    switch (t->kind) {
      case SK::Var: case SK::Pack: return t->var;
      case SK::App: case SK::Proj: t = t->t1; continue;
      default: return nullptr;
    }
  }
}

const Result* local_reduce_for_uid(env::t env, shape::SigComponentKind ns, Path::t path, shape::t shape) {
  std::function<const Result*(const Result*)> aux = [&](const Result* r) -> const Result* {
    switch (r->kind) {
      case Result::Kind::Resolved_alias: {
        auto* r2 = make<Result>(*r);
        r2->alias = aux(r->alias);
        return r2;
      }
      case Result::Kind::Missing_uid: {
        // A missing Uid after a complete reduction means either that we
        // found an occurrence of a locally-defined, opaque, item (from
        // functor arguments and first class modules) or a code error.
        auto* internal = make<Result>();
        internal->kind = Result::Kind::Internal_error_missing_uid;
        Ident::t parent_id = stuck_on_var_or_pack(r->shape);
        if (!parent_id) return internal;
        std::optional<Uid> uid = env::find_uid(ns, path, env);
        if (!uid) return internal;
        auto* r2 = make<Result>();
        r2->kind = Result::Kind::Resolved_local_use;
        r2->has_uid = true;
        r2->uid = make_definition_uid(parent_id, *uid);
        return r2;
      }
      default: return r;
    }
  };
  return aux(reduce_for_uid(env, shape));
}

void reset() {
  delete g_reducer;
  g_reducer = nullptr;
  delete g_uid_memo;
  g_uid_memo = nullptr;
}

}  // namespace cppcaml::typing::shape_reduce
