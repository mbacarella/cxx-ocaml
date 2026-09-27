// Port of typing/ctype.ml, part 3: abbreviation expansion and the occur
// check ("Abbreviation expansion" to "Polymorphic Unification").
#include <algorithm>

#include "cppcaml/typing/ctype.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;

static et::Elt<TypeExpr*> escape_elt_(const et::Escape<TypeExpr*>& e) {
  auto x = et::Elt<TypeExpr*>::mk(et::Elt<TypeExpr*>::Kind::Escape);
  x.escape = e;
  return x;
}

// If the environment has changed, memorized expansions might not be correct
// anymore, and so we flush the cache.
static env::t g_previous_env = nullptr;
static void check_abbrev_env(env::t env) {
  if (!g_previous_env) {
    ZoneScope perm(permanent_zone());
    g_previous_env = env::empty();
  }
  if (!env::same_type_declarations(env, g_previous_env)) {
    cleanup_abbrev_memo();
    simple_abbrevs()->contents = mnil();
    g_previous_env = env;
  }
}

// How expand_abbrev_gen finds the expansion: Env.find_type_expansion,
// Env.find_type_expansion_opt, or a given function.  The first two go
// through Env.find_type_expansion_into, without the exception.
enum class FteKind { Std, Opt, Fn };

// Expand an abbreviation.  The expansion is memorized.  (See ctype.ml for
// the four failure cases.)  nullptr where ctype.ml raises Cannot_expand
// (C++ exceptions are too costly for this hot, often-failing path).
static TypeExpr* expand_abbrev_gen_(bool link, PrivateFlag kind, FteKind fk, const FindTypeExpansion* fte,
                                    env::t env, TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ty));
  if (!c) throw std::logic_error("Ctype.expand_abbrev_gen");
  Path::t path = c->path;
  Slice<TypeExpr*> args = c->args;
  MemoRef* abbrev = c->memo;
  check_abbrev_env(env);
  long level = get_level(ty);
  long scope = get_scope(ty);
  MemoRef* lookup_abbrev = proper_abbrevs(args, abbrev);
  TypeExpr* expansion = nullptr;
  // first look for an existing expansion
  if (TypeExpr* ty2 = find_expans(kind, path, lookup_abbrev->contents)) {
    try {
      if (level != generic_level) update_level(env, level, ty2);
      bool stale = false;
      if (!clflags::principal) update_scope(scope, ty2);
      // In principal mode, force re-expansion if scope increased (ctype.ml
      // raises an Escape that the handler below catches at once)
      else if (get_scope(ty2) < scope) stale = true;
      if (stale) forget_abbrev_memo(lookup_abbrev, path);
      else expansion = ty2;
    } catch (const Escape&) {
      // in case of Escape, discard the stale expansion and re-expand
      forget_abbrev_memo(lookup_abbrev, path);
    }
  }
  TypeExpr* ty2 = expansion;
  if (!ty2) {
    // attempt to (re-)expand
    env::TypeExpansion x;
    bool found = true;
    try {
      if (fk == FteKind::Fn) x = (*fte)(path, env);
      else found = env::find_type_expansion_into(path, env, fk == FteKind::Opt, x);
    } catch (const env::NotFound&) {
      found = false;
    }
    if (!found) {
      // another way to expand is to normalize the path itself
      Path::t path2 = env::try_normalize_type_path(nullptr, env, path);
      if (!path2) return nullptr;
      ty2 = newty3(level, scope, tconstr(path2, args, abbrev));
    } else {
      long scope2 = std::max(x.expansion_scope, get_scope(ty));
      try {
        // Only enforce scope recursively in principal mode as this is not
        // necessary for soundness
        std::optional<long> sc;
        if (clflags::principal) sc = scope2;
        ty2 = subst(env, level, kind, abbrev, ty, x.params, args, x.body, sc);
      } catch (const CannotSubst&) {
        et::Escape<TypeExpr*> e;
        e.kind = et::Escape<TypeExpr*>::Kind::Constraint;
        throw Escape(e);
      }
      // For gadts, remember type as non exportable
      update_scope(scope2, ty);
      update_scope(scope2, ty2);
    }
  }
  // set [ty.desc] to [Texpand (ty', path, args)]
  if (link && kind == PrivateFlag::Public) link_expand(ty, ty2);
  return ty2;
}

TypeExpr* expand_abbrev_gen(bool link, PrivateFlag kind, const FindTypeExpansion& fte, env::t env,
                            TypeExpr* ty) {
  if (TypeExpr* r = expand_abbrev_gen_(link, kind, FteKind::Fn, &fte, env, ty)) return r;
  throw CannotExpand{};
}

// Expand respecting privacy
static TypeExpr* expand_abbrev_(bool link, env::t env, TypeExpr* ty) {
  return expand_abbrev_gen_(link, PrivateFlag::Public, FteKind::Std, nullptr, env, ty);
}
TypeExpr* expand_abbrev(bool link, env::t env, TypeExpr* ty) {
  if (TypeExpr* r = expand_abbrev_(link, env, ty)) return r;
  throw CannotExpand{};
}

// Expand once the head of a type
TypeExpr* expand_head_once(env::t env, TypeExpr* ty) {
  try {
    return expand_abbrev(false, env, ty);
  } catch (const CannotExpand&) {
    throw std::logic_error("Ctype.expand_head_once");
  } catch (const Escape&) {
    throw std::logic_error("Ctype.expand_head_once");
  }
}

// Check whether a type can be expanded
bool safe_abbrev(env::t env, TypeExpr* ty) {
  Snapshot snap = btype::snapshot();
  try {
    expand_abbrev(false, env, ty);
    return true;
  } catch (const CannotExpand&) {
    btype::backtrack(snap);
    return false;
  } catch (const Escape&) {
    btype::backtrack(snap);
    cleanup_abbrev_memo();
    return false;
  }
}

// Expand the head of a type once.  Raise CannotExpand if the type cannot be
// expanded.  May raise Escape, if a recursion was hidden in the type.
// (The *_nt functions return nullptr where ctype.ml raises Cannot_expand.)
static TypeExpr* try_expand_once_nt(bool link, env::t env, TypeExpr* ty) {
  if (get_desc(ty)->kind == DescKind::Tconstr) return expand_abbrev_(link, env, ty);
  return nullptr;
}
TypeExpr* try_expand_once(bool link, env::t env, TypeExpr* ty) {
  if (TypeExpr* r = try_expand_once_nt(link, env, ty)) return r;
  throw CannotExpand{};
}

// This one only raises CannotExpand
static TypeExpr* try_expand_safe_nt(bool link, env::t env, TypeExpr* ty) {
  Snapshot snap = btype::snapshot();
  try {
    return try_expand_once_nt(link, env, ty);
  } catch (const Escape&) {
    btype::backtrack(snap);
    cleanup_abbrev_memo();
    return nullptr;
  }
}
TypeExpr* try_expand_safe(env::t env, TypeExpr* ty) {
  if (TypeExpr* r = try_expand_safe_nt(true, env, ty)) return r;
  throw CannotExpand{};
}
TypeExpr* try_expand_safe_no_link(env::t env, TypeExpr* ty) {
  if (TypeExpr* r = try_expand_safe_nt(false, env, ty)) return r;
  throw CannotExpand{};
}

// Fully expand the head of a type.
TypeExpr* try_expand_head(const std::function<TypeExpr*(env::t, TypeExpr*)>& try_once, env::t env,
                          TypeExpr* ty) {
  TypeExpr* ty2 = try_once(env, ty);
  for (;;) {
    try {
      ty2 = try_once(env, ty2);
    } catch (const CannotExpand&) {
      return ty2;
    }
  }
}

// try_expand_head with a non-raising try_once; nullptr for Cannot_expand
template <class F>
static TypeExpr* try_expand_head_nt(F&& try_once, env::t env, TypeExpr* ty) {
  TypeExpr* ty2 = try_once(env, ty);
  if (!ty2) return nullptr;
  for (;;) {
    TypeExpr* ty3 = try_once(env, ty2);
    if (!ty3) return ty2;
    ty2 = ty3;
  }
}

// Unsafe full expansion, may raise [Unify [Escape _]].
TypeExpr* expand_head_unif(env::t env, TypeExpr* ty) {
  try {
    TypeExpr* r =
        try_expand_head_nt([](env::t e, TypeExpr* t) { return try_expand_once_nt(true, e, t); }, env, ty);
    return r ? r : ty;
  } catch (const Escape& e) {
    raise_for(TraceExn::Unify, escape_elt_(e.esc));
  }
}

// Safe version of expand_head, never fails
TypeExpr* expand_head(env::t env, TypeExpr* ty) {
  TypeExpr* r =
      try_expand_head_nt([](env::t e, TypeExpr* t) { return try_expand_safe_nt(true, e, t); }, env, ty);
  return r ? r : ty;
}
TypeExpr* expand_head_nolink(env::t env, TypeExpr* ty) {
  TypeExpr* r =
      try_expand_head_nt([](env::t e, TypeExpr* t) { return try_expand_safe_nt(false, e, t); }, env, ty);
  return r ? r : ty;
}

// Expand until we find a non-abstract type declaration
TypedeclExtraction extract_concrete_typedecl(env::t env, TypeExpr* ty) {
  using K = TypedeclExtraction::Kind;
  const TypeDesc* d = get_constr_desc(ty);
  switch (d->kind) {
    case DescKind::Tconstr: {
      Path::t p = as<Tconstr>(d)->path;
      auto cannot_expand = [&]() -> TypedeclExtraction {
        if (!get_abbrev(ty)) return {K::May_have_typedecl};
        return extract_concrete_typedecl(env, newgenty(get_desc(ty)));
      };
      const TypeDeclaration* decl;
      try {
        decl = env::find_type(p, env);
      } catch (const env::NotFound&) {
        return cannot_expand();
      }
      if (!type_kind_is_abstract(decl)) return {K::Typedecl, p, p, decl};
      TypeExpr* ty2;
      try {
        ty2 = try_expand_safe_no_link(env, ty);
      } catch (const CannotExpand&) {
        return cannot_expand();
      }
      TypedeclExtraction r = extract_concrete_typedecl(env, ty2);
      if (r.kind == K::Typedecl) return {K::Typedecl, p, r.p2, r.decl};
      return r;
    }
    case DescKind::Tpoly:
      return extract_concrete_typedecl(env, as<Tpoly>(d)->body);
    case DescKind::Tarrow: case DescKind::Ttuple: case DescKind::Tobject: case DescKind::Tfield:
    case DescKind::Tnil: case DescKind::Tvariant: case DescKind::Tpackage: case DescKind::Tfunctor:
      return {K::Has_no_typedecl};
    case DescKind::Tvar: case DescKind::Tunivar:
      return {K::May_have_typedecl};
    default:
      throw std::logic_error("Ctype.extract_concrete_typedecl");
  }
}

// expand_head_opt: the compiler's own expand_head for type-based
// optimisations (sees private abbreviations' manifests)
static TypeExpr* expand_abbrev_opt_(env::t env, TypeExpr* ty) {
  return expand_abbrev_gen_(false, PrivateFlag::Private, FteKind::Opt, nullptr, env, ty);
}
TypeExpr* expand_abbrev_opt(env::t env, TypeExpr* ty) {
  if (TypeExpr* r = expand_abbrev_opt_(env, ty)) return r;
  throw CannotExpand{};
}

bool safe_abbrev_opt(env::t env, TypeExpr* ty) {
  Snapshot snap = btype::snapshot();
  try {
    expand_abbrev_opt(env, ty);
    return true;
  } catch (const CannotExpand&) {
  } catch (const Escape&) {
  }
  btype::backtrack(snap);
  return false;
}

static TypeExpr* try_expand_once_opt_nt(env::t env, TypeExpr* ty) {
  if (get_desc(ty)->kind == DescKind::Tconstr) return expand_abbrev_opt_(env, ty);
  return nullptr;
}
TypeExpr* try_expand_once_opt(env::t env, TypeExpr* ty) {
  if (TypeExpr* r = try_expand_once_opt_nt(env, ty)) return r;
  throw CannotExpand{};
}

TypeExpr* try_expand_once_gen_nolink(const FindTypeExpansion& fte, env::t env, TypeExpr* ty) {
  if (get_desc(ty)->kind == DescKind::Tconstr)
    return expand_abbrev_gen(false, PrivateFlag::Private, fte, env, ty);
  throw CannotExpand{};
}

static TypeExpr* try_expand_safe_opt_nt(env::t env, TypeExpr* ty) {
  Snapshot snap = btype::snapshot();
  try {
    return try_expand_once_opt_nt(env, ty);
  } catch (const Escape&) {
    btype::backtrack(snap);
    return nullptr;
  }
}
TypeExpr* try_expand_safe_opt(env::t env, TypeExpr* ty) {
  if (TypeExpr* r = try_expand_safe_opt_nt(env, ty)) return r;
  throw CannotExpand{};
}

TypeExpr* expand_head_opt(env::t env, TypeExpr* ty) {
  TypeExpr* r = try_expand_head_nt(try_expand_safe_opt_nt, env, ty);
  return r ? r : ty;
}

// Recursively expand the head of a type.  Also expand #-types.  Error
// printing relies on full_expand returning exactly its input when nothing
// changes.
TypeExpr* full_expand(bool may_forget_scope, env::t env, TypeExpr* ty) {
  if (may_forget_scope) {
    try {
      ty = expand_head_unif(env, ty);
    } catch (const UnifyTrace&) {
      // #10277: forget scopes when printing trace
      ty = with_level(get_level(ty), [&]() -> TypeExpr* {
        // The same as expand_head, except in the failing case we return the
        // *original* type, not [duplicate_type ty].
        TypeExpr* r = try_expand_head_nt(
            [](env::t e, TypeExpr* t) { return try_expand_safe_nt(true, e, t); }, env, duplicate_type(ty));
        return r ? r : ty;
      });
    }
  } else {
    ty = expand_head(env, ty);
  }
  ty = repr(ty);  // forget Texpand
  if (auto* o = as<Tobject>(get_desc(ty)))
    if (const PathArgs* nm = o->name->contents; nm && !nm->args.empty() && is_Tvar(nm->args[0]))
      return newty2(get_level(ty), tobject(o->fields, make<NameRef>(nullptr)));
  return ty;
}

// Check whether the abbreviation expands to a well-defined type.
bool generic_abbrev(env::t env, Path::t path) {
  try {
    return get_level(env::find_type_expansion(path, env).body) == generic_level;
  } catch (const env::NotFound&) {
    return false;
  }
}

bool generic_private_abbrev(env::t env, Path::t path) {
  try {
    const TypeDeclaration* d = env::find_type(path, env);
    if (d->type_kind->kind == TypeKind::Kind::Type_abstract &&
        d->type_private == PrivateFlag::Private && d->type_manifest)
      return get_level(d->type_manifest) == generic_level;
    return false;
  } catch (const env::NotFound&) {
    return false;
  }
}

// Auxiliary function for subtyping [(module M : S) -> t1] into [t -> t2]
// where [t = private (module S2)].
const Package* extract_package_modulo_subtype(env::t env, TypeExpr* ty) {
  for (;;) {
    const TypeDesc* d = get_desc(expand_head(env, ty));
    if (auto* p = as<Tpackage>(d)) return p->pack;
    if (auto* c = as<Tconstr>(d);
        c && generic_private_abbrev(env, c->path) && safe_abbrev_opt(env, ty)) {
      ty = expand_abbrev_opt(env, ty);
      continue;
    }
    throw env::NotFound{};
  }
}

bool is_contractive(env::t env, Path::t p) {
  try {
    const TypeDeclaration* decl = env::find_type(p, env);
    return (in_pervasives(p) && !decl->type_manifest) || is_datatype(decl);
  } catch (const env::NotFound&) {
    return false;
  }
}

// ---- occur check ---------------------------------------------------------------------
// occur_rec's `parents` (a TypeSet of the ancestors in ctype.ml): only ever
// extended by one node for the recursive calls, so a chain on the stack --
// membership by the representative's identity, as TypeSet's compare by id.
namespace {
struct Parents {
  TypeExpr* ty;  // repr'd when added (TypeSet.add)
  const Parents* next;
  bool mem(TypeExpr* t) const {
    TypeExpr* r = repr(t);
    for (const Parents* p = this; p; p = p->next)
      if (p->ty == r) return true;
    return false;
  }
};
bool parents_mem(const Parents* p, TypeExpr* t) { return p && p->mem(t); }
}  // namespace

static void occur_rec(env::t env, TypeMark& visited, bool allow_recursive, const Parents* parents,
                      TypeExpr* ty0, TypeExpr* ty) {
  if (!not_marked_node(visited, ty)) return;
  if (eq_type(ty, ty0)) throw Occur{};
  const TypeDesc* d = get_desc(ty);
  if (auto* c = as<Tconstr>(d)) {
    if (!(allow_recursive && is_contractive(env, c->path))) {
      try {
        if (parents_mem(parents, ty)) throw Occur{};
        Parents parents2{repr(ty), parents};
        iter_type_expr(
            [&](TypeExpr* t) { occur_rec(env, visited, allow_recursive, &parents2, ty0, t); }, ty);
      } catch (const Occur&) {
        TypeExpr* ty2;
        try {
          ty2 = try_expand_safe(env, ty);
        } catch (const CannotExpand&) {
          throw Occur{};
        }
        occur_rec(env, visited, allow_recursive, parents, ty0, ty2);
      }
    }
  } else if (d->kind == DescKind::Tobject || d->kind == DescKind::Tvariant) {
  } else if (!(allow_recursive || parents_mem(parents, ty))) {
    Parents parents2{repr(ty), parents};
    iter_type_expr(
        [&](TypeExpr* t) { occur_rec(env, visited, allow_recursive, &parents2, ty0, t); }, ty);
  }
  try_mark_node(visited, ty);
}

bool type_changed = false;  // trace possible changes to the studied type

bool allow_recursive_equations(const Uenv& uenv) {
  return clflags::recursive_types || (uenv.is_pattern && uenv.penv->in_counterexample);
}

void occur(const Uenv& uenv, TypeExpr* ty0, TypeExpr* ty) {
  env::t env = get_env(uenv);
  bool allow_recursive = allow_recursive_equations(uenv);
  bool old = type_changed;
  try {
    do {
      type_changed = false;
      if (!eq_type(ty0, ty))
        with_type_mark(
            [&](TypeMark& mark) { occur_rec(env, mark, allow_recursive, nullptr, ty0, ty); });
    } while (type_changed);
    if (old) type_changed = true;
  } catch (...) {
    if (old) type_changed = true;
    throw;
  }
}

void occur_for(TraceExn tr_exn, const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  try {
    occur(uenv, t1, t2);
  } catch (const Occur&) {
    auto e = et::Elt<TypeExpr*>::mk(et::Elt<TypeExpr*>::Kind::Rec_occur);
    e.rec1 = t1;
    e.rec2 = t2;
    raise_for(tr_exn, e);
  }
}

bool occur_in(env::t env, TypeExpr* ty0, TypeExpr* t) {
  try {
    occur(Uenv::expression(env), ty0, t);
    return false;
  } catch (const Occur&) {
    return true;
  }
}

// Check that a local constraint is well-founded (a simplified occur, for the
// rectypes case; PR#6992)
static void local_non_recursive_abbrev_rec(bool allow_rec, bool strict,
                                           std::vector<long>& visited, env::t env, Path::t p,
                                           TypeExpr* ty) {
  long id = get_id(ty);
  if (std::find(visited.begin(), visited.end(), id) != visited.end()) return;
  const TypeDesc* d = get_desc(ty);
  if (auto* c = as<Tconstr>(d)) {
    if (env::path_equiv(env, p, c->path)) throw Occur{};
    if (allow_rec && !strict && is_contractive(env, c->path)) return;
    visited.push_back(id);
    TypeExpr* expanded = nullptr;
    try {
      // try expanding, since [p] could be hidden
      expanded = try_expand_head(try_expand_safe_opt, env, ty);
    } catch (const CannotExpand&) {
    }
    if (expanded) {
      local_non_recursive_abbrev_rec(allow_rec, strict, visited, env, p, expanded);
    } else {
      Slice<TypeExpr*> params;
      try {
        params = env::find_type(c->path, env)->type_params;
      } catch (const env::NotFound&) {
        params = c->args;
      }
      if (params.size() != c->args.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < params.size(); ++k) {
        bool s2 = strict || !is_Tvar(params[k]);
        local_non_recursive_abbrev_rec(allow_rec, s2, visited, env, p, c->args[k]);
      }
    }
    visited.pop_back();
    return;
  }
  if ((d->kind == DescKind::Tobject || d->kind == DescKind::Tvariant) && !strict) return;
  if (auto* fu = as<Tfunctor>(d)) {
    visited.push_back(id);
    for (auto& c : fu->pack->pack_constraints)
      local_non_recursive_abbrev_rec(allow_rec, strict, visited, env, p, c.ty);
    const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
    env::t env2 = env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
    local_non_recursive_abbrev_rec(allow_rec, strict, visited, env2, p, fu->body);
    visited.pop_back();
    return;
  }
  if (strict || !allow_rec) {  // PR#7374
    visited.push_back(id);
    iter_type_expr(
        [&](TypeExpr* t) { local_non_recursive_abbrev_rec(allow_rec, true, visited, env, p, t); },
        ty);
    visited.pop_back();
  }
}

bool local_non_recursive_abbrev(const Uenv& uenv, Path::t p, TypeExpr* ty) {
  env::t env = get_env(uenv);
  bool allow_rec = allow_recursive_equations(uenv);
  try {
    // PR#7397: need to check trace_gadt_instances
    wrap_trace_gadt_instances(env, [&] {
      std::vector<long> visited;
      local_non_recursive_abbrev_rec(allow_rec, false, visited, env, p, ty);
      return 0;
    });
    return true;
  } catch (const Occur&) {
    return false;
  }
}

}  // namespace cppcaml::typing::ctype
