// Port of typing/ctype.ml, part 4: polymorphic unification, mcomp,
// first-class module comparison and unification ("Polymorphic Unification"
// to "Special cases of unification").
#include <algorithm>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/predef.hpp"
#include "ctype_internal.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;
using namespace internal;
using EscK = et::Escape<TypeExpr*>::Kind;

[[noreturn]] static void raise_escape(EscK k, TypeExpr* univ = nullptr, Ident::t module = nullptr,
                                      Path::t path = nullptr) {
  et::Escape<TypeExpr*> e;
  e.kind = k;
  e.univ = univ;
  e.module = module;
  e.path = path;
  throw Escape(e);
}
// ---- polymorphic unification (univar_pairs) -------------------------------------------
std::vector<UnivarPair> univar_pairs;

void unify_univar(TypeExpr* t1, TypeExpr* t2, const std::vector<UnivarPair>& pairs) {
  auto find_univ = [](TypeExpr* t, const std::vector<UnivarCell>& cl) -> TyOptRef* {
    for (auto& c : cl)
      if (eq_type(t, c.univ)) return c.ref;
    return nullptr;
  };
  for (auto& pr : pairs) {
    TyOptRef* r1 = find_univ(t1, pr.cl1);
    TyOptRef* r2 = find_univ(t2, pr.cl2);
    if (r1 && r2) {
      if (r1->contents) {
        if (!eq_type(t2, r1->contents))
          throw CannotUnifyUniversalVariables{et::Order::Equal, {t1, t2}};
        return;
      }
      if (!r2->contents) {
        set_univar(r1, t2);
        set_univar(r2, t1);
        return;
      }
      throw CannotUnifyUniversalVariables{et::Order::Equal, {t1, t2}};
    }
    if (r1) throw CannotUnifyUniversalVariables{et::Order::More, {t1, t2}};
    if (r2) throw CannotUnifyUniversalVariables{et::Order::Less, {t1, t2}};
  }
  throw OutOfScopeUniversalVariable{};
}

// The same as unify_univar, but raises the appropriate exception
void internal::unify_univar_for(TraceExn tr_exn, TypeExpr* t1, TypeExpr* t2,
                                const std::vector<UnivarPair>& pairs) {
  try {
    unify_univar(t1, t2, pairs);
  } catch (const CannotUnifyUniversalVariables& c) {
    auto x = elt(EK::Univar);
    x.univar.is_var_mismatch = true;
    x.univar.order = c.order;
    x.univar.diff = c.diff;
    raise_for(tr_exn, x);
  } catch (const OutOfScopeUniversalVariable&) {
    // Allow unscoped univars when checking for equality (#13514)
    if (tr_exn == TraceExn::Equality) raise_unexplained_for(tr_exn);
    throw std::logic_error("Ctype.unify_univar_for: univar not in scope");
  }
}

// Test the occurrence of free univars and unscoped identifiers in a type.
// If inj_only, only check injective positions.
void occur_univar_or_unscoped(env::t env0, TypeExpr* root, bool inj_only) {
  std::map<TypeExpr*, std::pair<TypeSet, path::UnscopedSet>, ById> visited;
  with_type_mark([&](TypeMark& mark) {
    std::function<void(env::t, const TypeSet&, const path::UnscopedSet&, TypeExpr*)> occur_rec,
        occur_desc;
    auto set_subset = [](const path::UnscopedSet& a, const path::UnscopedSet& b) {
      for (auto* x : a)
        if (!b.count(x)) return false;
      return true;
    };
    occur_rec = [&](env::t env, const TypeSet& bound_uv, const path::UnscopedSet& bound_id,
                    TypeExpr* ty) {
      if (!not_marked_node(mark, ty)) return;
      if (bound_uv.is_empty() && bound_id.empty()) {
        try_mark_node(mark, ty);
        occur_desc(env, bound_uv, bound_id, ty);
        return;
      }
      TypeExpr* key = repr(ty);
      auto it = visited.find(key);
      if (it != visited.end()) {
        auto& [uv2, id2] = it->second;
        if (!(uv2.subset(bound_uv) && set_subset(id2, bound_id))) {
          path::UnscopedSet inter;
          for (auto* x : bound_id)
            if (id2.count(x)) inter.insert(x);
          it->second = {bound_uv.inter(uv2), inter};
          occur_desc(env, bound_uv, bound_id, ty);
        }
        return;
      }
      visited[key] = {bound_uv, bound_id};
      occur_desc(env, bound_uv, bound_id, ty);
    };
    auto occur_expand_safe = [&](env::t env, const TypeSet& bound_uv,
                                 const path::UnscopedSet& bound_id, ident::Unscoped* us,
                                 TypeExpr* ty) {
      TypeExpr* ty2;
      try {
        ty2 = try_expand_safe(env, ty);
      } catch (const CannotExpand&) {
        raise_escape(EscK::Module, nullptr, Ident::of_unscoped(us));
      }
      link_type(ty, ty2);
      occur_desc(env, bound_uv, bound_id, ty);
    };
    auto occur_set_name = [&](env::t env, const TypeSet& bound_uv,
                              const path::UnscopedSet& bound_id, TypeExpr* ty, Path::t p,
                              const std::function<void()>& set_name_None) {
      if (path::check_for_unbound_unscoped_idents(bound_id, p)) {
        set_name_None();
        occur_desc(env, bound_uv, bound_id, ty);
      } else {
        iter_type_expr([&](TypeExpr* t) { occur_rec(env, bound_uv, bound_id, t); }, ty);
      }
    };
    // (as in ctype.ml, this sets and re-examines the ROOT type [ty] of
    // occur_univar_or_unscoped, the only [ty] in scope there)
    auto occur_normalize_modtype_path = [&](env::t env, const TypeSet& bound_uv,
                                            const path::UnscopedSet& bound_id, ident::Unscoped* us,
                                            Path::t p,
                                            const std::function<const TypeDesc*(Path::t)>& f) {
      Path::t p2 = env::try_normalize_modtype_path(env, p);
      if (!p2) raise_escape(EscK::Module, nullptr, Ident::of_unscoped(us));
      set_type_desc(root, f(p2));
      occur_desc(env, bound_uv, bound_id, root);
    };
    occur_desc = [&](env::t env, const TypeSet& bound_uv, const path::UnscopedSet& bound_id,
                     TypeExpr* ty) {
      const TypeDesc* d = get_desc(ty);
      switch (d->kind) {
        case DescKind::Tunivar:
          if (!bound_uv.mem(ty)) raise_escape(EscK::Univ, ty);
          return;
        case DescKind::Tpoly: {
          auto* p = as<Tpoly>(d);
          TypeSet uv = bound_uv;
          for (TypeExpr* t : p->vars) uv.add(t);
          occur_rec(env, uv, bound_id, p->body);
          return;
        }
        case DescKind::Tconstr: {
          auto* c = as<Tconstr>(d);
          if (ident::Unscoped* i = path::check_for_unbound_unscoped_idents(bound_id, c->path)) {
            occur_expand_safe(env, bound_uv, bound_id, i, ty);
            return;
          }
          if (c->args.empty()) return;
          try {
            const TypeDeclaration* td = env::find_type(c->path, env);
            if (td->type_variance.size() != c->args.size()) throw std::invalid_argument("List.iter2");
            for (std::size_t k = 0; k < c->args.size(); ++k) {
              variance::t v = td->type_variance[k];
              // The null variance only occurs in type abbreviations (see
              // ctype.ml)
              if (inj_only ? variance::mem(variance::F::Inj, v) : !variance::eq(v, variance::null))
                occur_rec(env, bound_uv, bound_id, c->args[k]);
            }
          } catch (const env::NotFound&) {
            if (!inj_only)
              for (TypeExpr* t : c->args) occur_rec(env, bound_uv, bound_id, t);
          }
          return;
        }
        case DescKind::Tobject: {
          auto* o = as<Tobject>(d);
          if (o->name->contents) {
            NameRef* nm = o->name;
            occur_set_name(env, bound_uv, bound_id, ty, nm->contents->path,
                           [nm] { set_name(nm, nullptr); });
            return;
          }
          break;
        }
        case DescKind::Tvariant: {
          const RowDesc* row = as<Tvariant>(d)->row;
          if (const PathArgs* nm = row_name(row)) {
            occur_set_name(env, bound_uv, bound_id, ty, nm->path, [ty, row] {
              set_type_desc(ty, tvariant(set_row_name(row, nullptr)));
            });
            return;
          }
          break;
        }
        case DescKind::Tpackage: {
          const Package* pk = as<Tpackage>(d)->pack;
          if (ident::Unscoped* i = path::check_for_unbound_unscoped_idents(bound_id, pk->pack_path)) {
            occur_normalize_modtype_path(env, bound_uv, bound_id, i, pk->pack_path,
                                         [pk](Path::t pp) {
                                           return tpackage(make<Package>(pp, pk->pack_constraints));
                                         });
            return;
          }
          for (auto& c : pk->pack_constraints) occur_rec(env, bound_uv, bound_id, c.ty);
          return;
        }
        case DescKind::Tfunctor: {
          auto* fu = as<Tfunctor>(d);
          if (ident::Unscoped* i =
                  path::check_for_unbound_unscoped_idents(bound_id, fu->pack->pack_path)) {
            occur_normalize_modtype_path(
                env, bound_uv, bound_id, i, fu->pack->pack_path, [fu](Path::t pp) {
                  return tfunctor(fu->label, fu->id,
                                  make<Package>(pp, fu->pack->pack_constraints), fu->body);
                });
            return;
          }
          for (auto& c : fu->pack->pack_constraints) occur_rec(env, bound_uv, bound_id, c.ty);
          const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
          env::t env2 =
              env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
          path::UnscopedSet ids = bound_id;
          ids.insert(fu->id);
          occur_rec(env2, bound_uv, ids, fu->body);
          return;
        }
        default:
          break;
      }
      iter_type_expr([&](TypeExpr* t) { occur_rec(env, bound_uv, bound_id, t); }, ty);
    };
    occur_rec(env0, TypeSet{}, path::UnscopedSet{}, root);
  });
}

bool has_free_univars(env::t env, TypeExpr* ty) {
  try {
    occur_univar_or_unscoped(env, ty, false);
    return false;
  } catch (const Escape&) {
    return true;
  }
}
bool has_injective_univars(env::t env, TypeExpr* ty) {
  try {
    occur_univar_or_unscoped(env, ty, true);
    return false;
  } catch (const Escape&) {
    return true;
  }
}
void internal::occur_univar_or_unscoped_for(TraceExn tr_exn, env::t env, TypeExpr* ty) {
  try {
    occur_univar_or_unscoped(env, ty);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

// Grouping univars by families according to their binders
static TypeSet get_univar_family(const std::vector<UnivarPair>& pairs, Slice<TypeExpr*> univars) {
  TypeSet s;
  if (univars.empty()) return s;
  for (TypeExpr* u : univars) s.add(u);  // List.fold_right TypeSet.add
  for (auto& pr : pairs) {
    if (pr.cl2.empty()) continue;
    bool hit = std::any_of(pr.cl1.begin(), pr.cl1.end(), [&](const UnivarCell& c) { return s.mem(c.univ); });
    if (hit)
      for (auto& c : pr.cl2) s.add(c.univ);
  }
  return s;
}

// Whether a family of univars escapes from a type
static void univars_escape(env::t env, const std::vector<UnivarPair>& pairs, Slice<TypeExpr*> vl,
                           TypeExpr* ty) {
  TypeSet family = get_univar_family(pairs, vl);
  with_type_mark([&](TypeMark& mark) {
    std::function<void(TypeExpr*)> occ = [&](TypeExpr* t) {
      if (!try_mark_node(mark, t)) return;
      const TypeDesc* d = get_desc(t);
      switch (d->kind) {
        case DescKind::Tpoly: {
          auto* p = as<Tpoly>(d);
          if (std::any_of(p->vars.begin(), p->vars.end(), [&](TypeExpr* v) { return family.mem(v); }))
            return;
          occ(p->body);
          return;
        }
        case DescKind::Tunivar:
          if (family.mem(t)) raise_escape(EscK::Univ, t);
          return;
        case DescKind::Tconstr: {
          auto* c = as<Tconstr>(d);
          if (c->args.empty()) return;
          try {
            const TypeDeclaration* td = env::find_type(c->path, env);
            if (td->type_variance.size() != c->args.size()) throw std::invalid_argument("List.iter2");
            for (std::size_t k = 0; k < c->args.size(); ++k)
              if (!variance::eq(td->type_variance[k], variance::null)) occ(c->args[k]);
          } catch (const env::NotFound&) {
            for (TypeExpr* a : c->args) occ(a);
          }
          return;
        }
        default:
          iter_type_expr(occ, t);
      }
    };
    occ(ty);
  });
}

// Wrapper checking that no variable escapes and updating univar_pairs
void enter_poly(env::t env, TypeExpr* t1, Slice<TypeExpr*> tl1, TypeExpr* t2,
                Slice<TypeExpr*> tl2, const std::function<void(TypeExpr*, TypeExpr*)>& f) {
  std::vector<UnivarPair> old_univars = univar_pairs;
  TypeSet known_univars;
  for (auto& pr : old_univars)
    for (auto& c : pr.cl1) known_univars.add(c.univ);
  if (std::any_of(tl1.begin(), tl1.end(), [&](TypeExpr* t) { return known_univars.mem(t); }))
    univars_escape(env, old_univars, tl1, newty(tpoly(t2, tl2)));
  if (std::any_of(tl2.begin(), tl2.end(), [&](TypeExpr* t) { return known_univars.mem(t); }))
    univars_escape(env, old_univars, tl2, newty(tpoly(t1, tl1)));
  std::vector<UnivarCell> cl1, cl2;
  for (TypeExpr* t : tl1) cl1.push_back({t, make<TyOptRef>(nullptr)});
  for (TypeExpr* t : tl2) cl2.push_back({t, make<TyOptRef>(nullptr)});
  std::vector<UnivarPair> pairs{{cl1, cl2}, {cl2, cl1}};
  pairs.insert(pairs.end(), old_univars.begin(), old_univars.end());
  with_univar_pairs(std::move(pairs), [&] {
    f(t1, t2);
    return 0;
  });
}

void enter_poly_for(TraceExn tr_exn, env::t env, TypeExpr* t1, Slice<TypeExpr*> tl1, TypeExpr* t2,
                    Slice<TypeExpr*> tl2, const std::function<void(TypeExpr*, TypeExpr*)>& f) {
  try {
    enter_poly(env, t1, tl1, t2, tl2, f);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

// Similar to nondep_type, but removes the dependency in place.
void identifier_escape(env::t env, const std::vector<ident::Unscoped*>& idl0, TypeExpr* root) {
  with_type_mark([&](TypeMark& mark) {
    std::function<void(const std::vector<Ident::t>&, TypeExpr*, bool)> occ;
    auto occur_expand_safe = [&](const std::vector<Ident::t>& idl, TypeExpr* ty, Ident::t id) {
      TypeExpr* ty2;
      try {
        ty2 = try_expand_safe(env, ty);
      } catch (const CannotExpand&) {
        raise_escape(EscK::Module, nullptr, id);
      }
      link_type(ty, ty2);
      occ(idl, ty2, true);
    };
    auto occur_set_name = [&](const std::vector<Ident::t>& idl, TypeExpr* ty, Path::t p,
                              const std::function<void()>& set_name_None) {
      if (path::exists_free(idl, p)) {
        set_name_None();
        occ(idl, ty, true);
      } else {
        iter_type_expr([&](TypeExpr* t) { occ(idl, t, false); }, ty);
      }
    };
    // (as in ctype.ml: sets and re-examines the ROOT type of
    // identifier_escape)
    auto occur_normalize_modtype_path = [&](const std::vector<Ident::t>& idl, Path::t p,
                                            Ident::t id,
                                            const std::function<const TypeDesc*(Path::t)>& f) {
      Path::t p2 = env::try_normalize_modtype_path(env, p);
      if (!p2) raise_escape(EscK::Module, nullptr, id);
      set_type_desc(root, f(p2));
      occ(idl, root, true);
    };
    occ = [&](const std::vector<Ident::t>& idl, TypeExpr* ty, bool ignore_mark) {
      if (!(try_mark_node(mark, ty) || ignore_mark)) return;
      const TypeDesc* d = get_desc(ty);
      switch (d->kind) {
        case DescKind::Tconstr: {
          auto* c = as<Tconstr>(d);
          if (auto i = path::find_free_opt(idl, c->path)) occur_expand_safe(idl, ty, *i);
          else iter_type_expr([&](TypeExpr* t) { occ(idl, t, false); }, ty);
          return;
        }
        case DescKind::Tpackage: {
          const Package* pk = as<Tpackage>(d)->pack;
          if (auto i = path::find_free_opt(idl, pk->pack_path))
            occur_normalize_modtype_path(idl, pk->pack_path, *i, [pk](Path::t pp) {
              return tpackage(make<Package>(pp, pk->pack_constraints));
            });
          else
            iter_type_expr([&](TypeExpr* t) { occ(idl, t, false); }, ty);
          return;
        }
        case DescKind::Tobject: {
          auto* o = as<Tobject>(d);
          if (o->name->contents) {
            NameRef* nm = o->name;
            occur_set_name(idl, ty, nm->contents->path, [nm] { set_name(nm, nullptr); });
            return;
          }
          break;
        }
        case DescKind::Tvariant: {
          const RowDesc* row = as<Tvariant>(d)->row;
          if (const PathArgs* nm = row_name(row)) {
            occur_set_name(idl, ty, nm->path, [ty, row] {
              set_type_desc(ty, tvariant(set_row_name(row, nullptr)));
            });
            return;
          }
          break;
        }
        case DescKind::Tfunctor: {
          auto* fu = as<Tfunctor>(d);
          if (auto i = path::find_free_opt(idl, fu->pack->pack_path)) {
            // (as in ctype.ml, the rebuilt functor's body is [ty], the node)
            occur_normalize_modtype_path(idl, fu->pack->pack_path, *i, [fu, ty](Path::t pp) {
              return tfunctor(fu->label, fu->id, make<Package>(pp, fu->pack->pack_constraints), ty);
            });
            return;
          }
          for (auto& c : fu->pack->pack_constraints) occ(idl, c.ty, false);
          std::vector<Ident::t> idl2;
          for (Ident::t i : idl)
            if (!ident::same(i, Ident::of_unscoped(fu->id))) idl2.push_back(i);
          if (!idl2.empty()) occ(idl2, fu->body, false);
          return;
        }
        default:
          break;
      }
      iter_type_expr([&](TypeExpr* t) { occ(idl, t, false); }, ty);
    };
    std::vector<Ident::t> idl;
    for (auto* u : idl0) idl.push_back(Ident::of_unscoped(u));
    occ(idl, root, false);
  });
}

void identifier_escape_for(TraceExn tr_exn, env::t env,
                           const std::vector<ident::Unscoped*>& idl, TypeExpr* t) {
  try {
    identifier_escape(env, idl, t);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

// An identifier for a Tfunctor is bound at a single type node, but recursive
// types can match it several times: an identifier cannot be equal to
// several ones at once (see ctype.ml).
void internal::enter_functor(env::t env, ident::Unscoped* id1, TypeExpr* t1,
                             ident::Unscoped* id2, TypeExpr* t2,
                             const std::function<void(std::vector<std::pair<ident::Unscoped*, ident::Unscoped*>>)>& f) {
  using U = ident::Unscoped;
  std::vector<std::pair<U*, U*>> filtered;
  for (auto& pr : env::get_pairs(env)) {
    U* i1 = pr.first;
    U* i2 = pr.second;
    if (U::same(id1, i1) || U::same(id1, i2) || U::same(id2, i1) || U::same(id2, i2)) {
      identifier_escape(env, {i1, i2}, t1);
      identifier_escape(env, {i1, i2}, t2);
    } else {
      filtered.push_back(pr);
    }
  }
  filtered.insert(filtered.begin(), {id1, id2});
  f(filtered);
}

static void enter_functor_for(TraceExn tr_exn, env::t env, ident::Unscoped* id1, TypeExpr* t1,
                              ident::Unscoped* id2, TypeExpr* t2,
                              const std::function<void(std::vector<std::pair<ident::Unscoped*, ident::Unscoped*>>)>& f) {
  try {
    enter_functor(env, id1, t1, id2, t2, f);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

static void enter_functor_for_unify(const Uenv& uenv, ident::Unscoped* id1, TypeExpr* t1,
                                    ident::Unscoped* id2, TypeExpr* t2, const ModuleType* mty,
                                    const std::function<void(const Uenv&)>& f) {
  enter_functor_for(TraceExn::Unify, get_env(uenv), id1, t1, id2, t2, [&](auto id_pairs) {
    ident::Unscoped::link(id1, id2);
    if (!uenv.is_pattern) {
      env::t env = env::add_module(Ident::of_unscoped(id1), ModulePresence::Mp_present, mty,
                                   uenv.expr_env);
      env = env::with_pairs(slice(id_pairs), env);
      Uenv u2 = uenv;
      u2.expr_env = env;
      f(u2);
    } else {
      uenv.penv->with_mty(slice(id_pairs), id1, mty, [&] { f(uenv); });
    }
  });
}

void enter_functor_with_mtys_for(TraceExn tr_exn, env::t env, ident::Unscoped* id1,
                                 const ModuleType* mty1, TypeExpr* t1, ident::Unscoped* id2,
                                 const ModuleType* mty2, TypeExpr* t2,
                                 const std::function<void(env::t)>& f) {
  enter_functor_for(tr_exn, env, id1, t1, id2, t2, [&](auto id_pairs) {
    env::t e = env::add_module(Ident::of_unscoped(id1), ModulePresence::Mp_present, mty1, env);
    e = env::add_module(Ident::of_unscoped(id2), ModulePresence::Mp_present, mty2, e);
    e = env::with_pairs(slice(id_pairs), e);
    f(e);
  });
}

// ---- instantiate a generic type into a poly type -------------------------------------
std::pair<TypeExpr*, std::vector<TypeExpr*>> polyfy(env::t env, TypeExpr* ty,
                                                    const std::vector<TypeExpr*>& vars0) {
  // need to expand twice? cf. Ctype.unify2
  std::vector<TypeExpr*> vars;
  for (TypeExpr* v : vars0) vars.push_back(expand_head(env, v));
  for (TypeExpr*& v : vars) v = expand_head(env, v);
  std::pair<TypeExpr*, std::vector<TypeExpr*>> r;
  with_copy_scope([&](CopyScope& copy_scope) {
    std::vector<TypeExpr*> vars2, err;
    for (TypeExpr* t : vars) {
      const TypeDesc* d = get_desc(t);
      if (auto* v = as<Tvar>(d); v && get_level(t) == generic_level) {
        TypeExpr* u = newty(tunivar(v->name));
        redirect_desc(copy_scope, t, tsubst(u, nullptr));
        vars2.push_back(u);
      } else if (auto* s = as<Tsubst>(d)) {
        err.push_back(s->ty);
      } else {
        err.push_back(t);
      }
    }
    TypeExpr* ty2 = copy(copy_scope, ty);
    r.first = newty2(get_level(ty2), tpoly(ty2, slice(vars2)));
    r.second = err;
  });
  return r;
}

// assumption: [ty] is fully generalized.
TypeExpr* reify_univars(env::t env, TypeExpr* ty) {
  return polyfy(env, ty, free_variables(ty)).first;
}

// ---- unification ---------------------------------------------------------------------
static bool has_cached_expansion(Path::t p, const AbbrevMemo* m) {
  for (;;) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return false;
      case AbbrevMemo::Kind::Mcons:
        if (path::same(p, m->path)) return true;
        m = m->rem;
        continue;
      case AbbrevMemo::Kind::Mlink:
        m = m->link->contents;
        continue;
    }
  }
}

// ---- transform error trace -----------------------------------------------------------
et::ExpandedType expand_type(env::t env, TypeExpr* ty) { return {ty, full_expand(true, env, ty)}; }

et::ErrorTrace expand_trace(env::t env, const et::TypeTrace& trace) {
  et::ErrorTrace out;
  for (auto& e : trace)
    out.push_back(et::map_elt<TypeExpr*, et::ExpandedType>(
        [&](TypeExpr* t) { return expand_type(env, t); }, e));
  return out;
}

et::UnificationError internal::expand_to_unification_error(env::t env, const et::TypeTrace& trace) {
  if (trace.empty()) throw std::logic_error("Errortrace.unification_error");
  return {expand_trace(env, trace)};
}

et::Elt<et::ExpandedType> expanded_diff(env::t env, TypeExpr* got, TypeExpr* expected) {
  auto x = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::Diff);
  et::ExpandedType g = expand_type(env, got);
  et::ExpandedType e = expand_type(env, expected);
  x.diff = {g, e};
  return x;
}

et::Elt<et::ExpandedType> unexpanded_diff(TypeExpr* got, TypeExpr* expected) {
  auto x = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::Diff);
  x.diff = {et::trivial_expansion(got), et::trivial_expansion(expected)};
  return x;
}

// ---- unification helpers ----------------------------------------------------------------
static void add_type_equality(const Uenv& u, TypeExpr* t1, TypeExpr* t2) {
  if (!u.is_pattern) throw std::invalid_argument("Ctype.add_type_equality");
  if (get_id(t1) <= get_id(t2)) u.unify_eq_set->add(t1, t2);
  else u.unify_eq_set->add(t2, t1);
}
static bool unify_eq(const Uenv& u, TypeExpr* t1, TypeExpr* t2) {
  if (eq_type(t1, t2)) return true;
  if (!u.is_pattern) return false;
  return get_id(t1) <= get_id(t2) ? u.unify_eq_set->mem(t1, t2) : u.unify_eq_set->mem(t2, t1);
}
static bool in_subst_mode(const Uenv& u) { return !u.is_pattern && u.in_subst; }
static void record_equation(const Uenv& u, TypeExpr* t1, TypeExpr* t2) {
  if (!u.is_pattern) throw std::invalid_argument("Ctype.record_equation");
  u.equated_types->add(t1, t2);
}
static bool can_assume_injective(const Uenv& u) { return u.is_pattern && u.assume_injective; }
static bool in_counterexample(const Uenv& u) { return u.is_pattern && u.penv->in_counterexample; }
static long get_equations_scope(const Uenv& u) {
  if (!u.is_pattern) throw std::invalid_argument("Ctype.get_equations_scope");
  return u.penv->equations_scope;
}

// A local constraint can be added only if the rhs of the constraint does not
// contain any Tvars; they are removed with this (Pattern mode only).
static void reify(const Uenv& uenv, TypeExpr* t, TypeExpr* eqn_lhs = nullptr,
                  TypeExpr* eqn_rhs = nullptr) {
  long fresh_constr_scope = get_equations_scope(uenv);
  TypeOrigin origin;
  if (eqn_lhs) {
    origin.kind = TypeOrigin::Kind::Equation;
    origin.eq1 = eqn_lhs;
    origin.eq2 = eqn_rhs;
  }
  auto create_fresh_constr = [&](long lev, OptStr name) -> std::pair<Path::t, TypeExpr*> {
    std::string nm = name.some ? "$'" + std::string(name.v) : "$";
    const TypeDeclaration* decl = new_local_type(origin);
    env::t env = get_env(uenv);
    // unique names are needed only for error messages
    std::string new_name = in_counterexample(uenv) ? nm : get_new_abstract_name(env, nm);
    Ident::t id = uenv.penv->enter_type(fresh_constr_scope, new_name, decl);
    Path::t path = Path::pident(id);
    TypeExpr* t2 = newty2(lev, tconstr(path, {}, make<MemoRef>(mnil())));
    return {path, t2};
  };
  TypeSet visited;
  std::function<void(TypeExpr*)> iterator = [&](TypeExpr* ty) {
    if (visited.mem(ty)) return;
    visited.add(ty);
    const TypeDesc* d = get_desc(ty);
    if (auto* v = as<Tvar>(d)) {
      long level = get_level(ty);
      auto [path, t2] = create_fresh_constr(level, v->name);
      link_type(ty, t2);
      if (level < fresh_constr_scope) {
        et::Escape<TypeExpr*> e;
        e.kind = EscK::Constructor;
        e.path = path;
        raise_for(TraceExn::Unify, escape_elt(e));
      }
      return;
    }
    if (auto* vr = as<Tvariant>(d)) {
      const RowDesc* r = vr->row;
      if (!static_row(r)) {
        if (is_fixed(r)) {
          iterator(row_more(r));
        } else {
          TypeExpr* m = row_more(r);
          auto* mv = as<Tvar>(get_desc(m));
          if (!mv) throw std::logic_error("Ctype.reify");
          long level = get_level(m);
          auto [path, t2] = create_fresh_constr(level, mv->name);
          const RowDesc* row = create_row(
              {}, t2, row_closed(r),
              make<FixedExplanation>(FixedExplanation::Kind::Reified, nullptr, path), row_name(r));
          link_type(m, newty2(level, tvariant(row)));
          if (level < fresh_constr_scope) {
            et::Escape<TypeExpr*> e;
            e.kind = EscK::Constructor;
            e.path = path;
            raise_for(TraceExn::Unify, escape_elt(e));
          }
        }
      }
      iter_row(iterator, r);
      return;
    }
    iter_type_expr(iterator, ty);
  };
  iterator(t);
}

static long find_expansion_scope(env::t env, Path::t path) {
  try {
    const TypeDeclaration* d = env::find_type(path, env);
    if (!d->type_manifest) return generic_level;
    return d->type_expansion_scope;
  } catch (const env::NotFound&) {
    return generic_level;
  }
}

static bool is_instantiable(env::t env, Path::t p) {
  try {
    const TypeDeclaration* d = env::find_type(p, env);
    return type_kind_is_abstract(d) && d->type_private == PrivateFlag::Public &&
           d->type_arity == 0 && !d->type_manifest;
  } catch (const env::NotFound&) {
    return false;
  }
}

// Two labels are compatible if equal, or (in classic or pattern mode) when
// neither is optional.
bool compatible_labels(bool in_pattern_mode, const ArgLabel& l1, const ArgLabel& l2) {
  return l1 == l2 ||
         ((clflags::classic || in_pattern_mode) && !(is_optional(l1) || is_optional(l2)));
}

void internal::eq_labels(TraceExn error_mode, bool in_pattern_mode, const ArgLabel& l1,
                         const ArgLabel& l2) {
  if (!compatible_labels(in_pattern_mode, l1, l2)) {
    auto x = elt(EK::Function_label_mismatch);
    x.label_diff = {l1, l2};
    raise_for(error_mode, x);
  }
}

// Check for datatypes carefully; see PR#6348
static bool expands_to_datatype(env::t env, Path::t p) {
  try {
    return is_datatype(env::find_type(p, env));
  } catch (const env::NotFound&) {
    return false;
  }
}

// ---- mcomp (see ctype.ml: overapproximates compatibility) -------------------------------
static void mcomp_rec(TypePairs& type_pairs, env::t env, TypeExpr* t1, TypeExpr* t2);

static void mcomp_list(TypePairs& tp, env::t env, Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  if (tl1.size() != tl2.size()) throw Incompatible{};
  for (std::size_t k = 0; k < tl1.size(); ++k) mcomp_rec(tp, env, tl1[k], tl2[k]);
}

static void mcomp_record_description(TypePairs& tp, env::t env, Slice<const LabelDeclaration*> x,
                                     Slice<const LabelDeclaration*> y) {
  std::size_t k = 0;
  for (; k < x.size() && k < y.size(); ++k) {
    mcomp_rec(tp, env, x[k]->ld_type, y[k]->ld_type);
    if (!(ident::name(x[k]->ld_id) == ident::name(y[k]->ld_id) &&
          x[k]->ld_mutable == y[k]->ld_mutable && x[k]->ld_atomic == y[k]->ld_atomic))
      throw Incompatible{};
  }
  if (k != x.size() || k != y.size()) throw Incompatible{};
}

static void mcomp_variant_description(TypePairs& tp, env::t env,
                                      Slice<const ConstructorDeclaration*> xs,
                                      Slice<const ConstructorDeclaration*> ys) {
  std::size_t k = 0;
  for (; k < xs.size() && k < ys.size(); ++k) {
    const ConstructorDeclaration* c1 = xs[k];
    const ConstructorDeclaration* c2 = ys[k];
    // mcomp_type_option
    if (c1->cd_res && c2->cd_res) mcomp_rec(tp, env, c1->cd_res, c2->cd_res);
    else if (c1->cd_res || c2->cd_res) throw Incompatible{};
    if (c1->cd_args.kind != c2->cd_args.kind) throw Incompatible{};
    if (c1->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple)
      mcomp_list(tp, env, c1->cd_args.tuple, c2->cd_args.tuple);
    else
      mcomp_record_description(tp, env, c1->cd_args.record, c2->cd_args.record);
    if (ident::name(c1->cd_id) != ident::name(c2->cd_id)) throw Incompatible{};
  }
  if (k != xs.size() || k != ys.size()) throw Incompatible{};
}

static void mcomp_type_decl(TypePairs& tp, env::t env, Path::t p1, Path::t p2,
                            Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  try {
    const TypeDeclaration* decl = env::find_type(p1, env);
    const TypeDeclaration* decl2 = env::find_type(p2, env);
    if (env::path_equiv(env, p1, p2)) {
      std::vector<bool> inj;
      try {
        for (variance::t v : env::find_type(p1, env)->type_variance)
          inj.push_back(variance::mem(variance::F::Inj, v));
      } catch (const env::NotFound&) {
        inj.assign(tl1.size(), false);
      }
      // List.iter2 over inj and List.combine tl1 tl2
      if (tl1.size() != tl2.size() || inj.size() != tl1.size())
        throw std::invalid_argument("List.combine/iter2");
      for (std::size_t k = 0; k < inj.size(); ++k)
        if (inj[k]) mcomp_rec(tp, env, tl1[k], tl2[k]);
      return;
    }
    const TypeKind* k1 = decl->type_kind;
    const TypeKind* k2 = decl2->type_kind;
    using KK = TypeKind::Kind;
    auto same_rrepr = [](const RecordRepresentation& a, const RecordRepresentation& b) {
      if (a.kind != b.kind) return false;
      switch (a.kind) {
        case RecordRepresentation::Kind::Record_unboxed: return a.unboxed_inlined == b.unboxed_inlined;
        case RecordRepresentation::Kind::Record_inlined: return a.inlined_tag == b.inlined_tag;
        case RecordRepresentation::Kind::Record_extension: return path::compare(a.extension, b.extension) == 0;
        default: return true;
      }
    };
    if (k1->kind == KK::Type_record && k2->kind == KK::Type_record &&
        same_rrepr(k1->record_repr, k2->record_repr)) {
      mcomp_list(tp, env, tl1, tl2);
      mcomp_record_description(tp, env, k1->labels, k2->labels);
    } else if (k1->kind == KK::Type_variant && k2->kind == KK::Type_variant &&
               k1->variant_repr == k2->variant_repr) {
      mcomp_list(tp, env, tl1, tl2);
      mcomp_variant_description(tp, env, k1->constructors, k2->constructors);
    } else if (k1->kind == KK::Type_open && k2->kind == KK::Type_open) {
      mcomp_list(tp, env, tl1, tl2);  // thus, exn and eff are incompatible
    } else if (k1->kind == KK::Type_external && k2->kind == KK::Type_external &&
               k1->external == k2->external) {
      mcomp_list(tp, env, tl1, tl2);
    } else if (k1->kind == KK::Type_abstract || k2->kind == KK::Type_abstract) {
    } else {
      throw Incompatible{};
    }
  } catch (const env::NotFound&) {
  }
}

static void mcomp_kind(FieldKind* k1, FieldKind* k2) {
  auto a = field_kind_repr(k1), b = field_kind_repr(k2);
  if ((a == FieldKindView::Fpublic && b == FieldKindView::Fabsent) ||
      (a == FieldKindView::Fabsent && b == FieldKindView::Fpublic))
    throw Incompatible{};
}

static void mcomp_fields(TypePairs& tp, env::t env, TypeExpr* ty1, TypeExpr* ty2) {
  if (!(concrete_object(ty1) && concrete_object(ty2))) throw std::logic_error("Ctype.mcomp_fields");
  auto [fields2, rest2] = flatten_fields(ty2);
  auto [fields1, rest1] = flatten_fields(ty1);
  AssociatedFields af = associate_fields(fields1, fields2);
  auto has_present = [](const std::vector<FieldEntry>& l) {
    return std::any_of(l.begin(), l.end(), [](const FieldEntry& e) {
      return field_kind_repr(e.kind) == FieldKindView::Fpublic;
    });
  };
  mcomp_rec(tp, env, rest1, rest2);
  if ((has_present(af.miss1) && get_desc(object_row(ty2))->kind == DescKind::Tnil) ||
      (has_present(af.miss2) && get_desc(object_row(ty1))->kind == DescKind::Tnil))
    throw Incompatible{};
  for (auto& p : af.pairs) {
    mcomp_kind(p.k1, p.k2);
    mcomp_rec(tp, env, p.t1, p.t2);
  }
}

static void mcomp_row(TypePairs& tp, env::t env, const RowDesc* row1, const RowDesc* row2) {
  MergedRowFields m = merge_row_fields(row_fields(row1), row_fields(row2));
  auto cannot_erase = [](const RowFieldEntry& e) {
    return row_field_repr(e.field).kind == RowFieldView::Kind::Rpresent;
  };
  if ((row_closed(row1) && std::any_of(m.r2.begin(), m.r2.end(), cannot_erase)) ||
      (row_closed(row2) && std::any_of(m.r1.begin(), m.r1.end(), cannot_erase)))
    throw Incompatible{};
  using V = RowFieldView::Kind;
  for (auto& pr : m.pairs) {
    RowFieldView a = row_field_repr(pr.f1), b = row_field_repr(pr.f2);
    bool a_pn = a.kind == V::Rpresent && !a.present, a_ps = a.kind == V::Rpresent && a.present;
    bool b_pn = b.kind == V::Rpresent && !b.present, b_ps = b.kind == V::Rpresent && b.present;
    bool a_either_args = a.kind == V::Reither && !a.arg_types.empty();
    bool b_either_args = b.kind == V::Reither && !b.arg_types.empty();
    bool a_either_const = a.kind == V::Reither && a.constant;
    bool b_either_const = b.kind == V::Reither && b.constant;
    bool a_abs = a.kind == V::Rabsent, b_abs = b.kind == V::Rabsent;
    if ((a_pn && (b_ps || b_either_args || b_abs)) || (a_ps && (b_pn || b_either_const || b_abs)) ||
        ((a_either_args || a_abs) && b_pn) || ((a_either_const || a_abs) && b_ps))
      throw Incompatible{};
    if (a_ps && b_ps) mcomp_rec(tp, env, a.present, b.present);
    else if (a_ps && b.kind == V::Reither && !b.constant)
      for (TypeExpr* t : b.arg_types) mcomp_rec(tp, env, a.present, t);
    else if (a.kind == V::Reither && !a.constant && b_ps)
      for (TypeExpr* t : a.arg_types) mcomp_rec(tp, env, b.present, t);
  }
}

static void mcomp_rec(TypePairs& type_pairs, env::t env, TypeExpr* t1, TypeExpr* t2) {
  if (eq_type(t1, t2)) return;
  const TypeDesc* d1 = get_desc(t1);
  const TypeDesc* d2 = get_desc(t2);
  if (d1->kind == DescKind::Tvar || d2->kind == DescKind::Tvar) return;
  if (auto* c1 = as<Tconstr>(d1))
    if (auto* c2 = as<Tconstr>(d2))
      if (c1->args.empty() && c2->args.empty() && env::path_equiv(env, c1->path, c2->path)) return;
  TypeExpr* t1e = expand_head_opt(env, t1);
  TypeExpr* t2e = expand_head_opt(env, t2);
  // Expansion may have changed the representative of the types...
  if (eq_type(t1e, t2e)) return;
  if (type_pairs.mem(t1e, t2e)) return;
  type_pairs.add(t1e, t2e);
  const TypeDesc* e1 = get_desc(t1e);
  const TypeDesc* e2 = get_desc(t2e);
  auto K1 = e1->kind, K2 = e2->kind;
  using DK = DescKind;
  if (K1 == DK::Tvar || K2 == DK::Tvar) return;
  if (K1 == DK::Tarrow && K2 == DK::Tarrow) {
    auto* a1 = as<Tarrow>(e1);
    auto* a2 = as<Tarrow>(e2);
    if (compatible_labels(true, a1->label, a2->label)) {
      mcomp_rec(type_pairs, env, a1->t1, a2->t1);
      mcomp_rec(type_pairs, env, a1->t2, a2->t2);
      return;
    }
  }
  if (K1 == DK::Ttuple && K2 == DK::Ttuple) {
    auto& l1 = as<Ttuple>(e1)->elems;
    auto& l2 = as<Ttuple>(e2)->elems;
    if (l1.size() != l2.size()) throw Incompatible{};
    for (std::size_t k = 0; k < l1.size(); ++k) {
      if (!(l1[k].label == l2[k].label)) throw Incompatible{};
      mcomp_rec(type_pairs, env, l1[k].ty, l2[k].ty);
    }
    return;
  }
  if (K1 == DK::Tconstr && K2 == DK::Tconstr) {
    auto* c1 = as<Tconstr>(e1);
    auto* c2 = as<Tconstr>(e2);
    mcomp_type_decl(type_pairs, env, c1->path, c2->path, c1->args, c2->args);
    return;
  }
  if (K1 == DK::Tconstr && as<Tconstr>(e1)->args.empty() && has_injective_univars(env, t2e))
    throw Incompatible{};
  if (K2 == DK::Tconstr && as<Tconstr>(e2)->args.empty() && has_injective_univars(env, t1e))
    throw Incompatible{};
  if (K1 == DK::Tconstr || K2 == DK::Tconstr) {
    Path::t p = K1 == DK::Tconstr ? as<Tconstr>(e1)->path : as<Tconstr>(e2)->path;
    try {
      if (is_datatype(env::find_type(p, env))) throw Incompatible{};
    } catch (const env::NotFound&) {
    }
    return;
  }
  if (K1 == DK::Tfunctor && K2 == DK::Tfunctor) {
    auto* f1 = as<Tfunctor>(e1);
    auto* f2 = as<Tfunctor>(e2);
    if (compatible_labels(true, f1->label, f2->label)) {
      mcomp_rec(type_pairs, env, f1->body, f2->body);
      return;
    }
  }
  if (K1 == DK::Tfunctor && K2 == DK::Tarrow) {
    auto* f1 = as<Tfunctor>(e1);
    auto* a2 = as<Tarrow>(e2);
    if (compatible_labels(true, f1->label, a2->label)) {
      mcomp_rec(type_pairs, env, newmono_package(f1->pack), a2->t1);
      mcomp_rec(type_pairs, env, f1->body, a2->t2);
      return;
    }
  }
  if (K1 == DK::Tarrow && K2 == DK::Tfunctor) {
    auto* a1 = as<Tarrow>(e1);
    auto* f2 = as<Tfunctor>(e2);
    if (compatible_labels(true, a1->label, f2->label)) {
      mcomp_rec(type_pairs, env, a1->t1, newmono_package(f2->pack));
      mcomp_rec(type_pairs, env, a1->t2, f2->body);
      return;
    }
  }
  if (K1 == DK::Tpackage && K2 == DK::Tpackage) return;
  if (K1 == DK::Tvariant && K2 == DK::Tvariant) {
    mcomp_row(type_pairs, env, as<Tvariant>(e1)->row, as<Tvariant>(e2)->row);
    return;
  }
  if (K1 == DK::Tobject && K2 == DK::Tobject) {
    mcomp_fields(type_pairs, env, as<Tobject>(e1)->fields, as<Tobject>(e2)->fields);
    return;
  }
  if (K1 == DK::Tfield && K2 == DK::Tfield) {  // Actually unused
    mcomp_fields(type_pairs, env, t1e, t2e);
    return;
  }
  if (K1 == DK::Tnil && K2 == DK::Tnil) return;
  if (K1 == DK::Tpoly && K2 == DK::Tpoly) {
    auto* p1 = as<Tpoly>(e1);
    auto* p2 = as<Tpoly>(e2);
    if (p1->vars.empty() && p2->vars.empty()) {
      mcomp_rec(type_pairs, env, p1->body, p2->body);
      return;
    }
    try {
      enter_poly(env, p1->body, p1->vars, p2->body, p2->vars,
                 [&](TypeExpr* a, TypeExpr* b) { mcomp_rec(type_pairs, env, a, b); });
    } catch (const Escape&) {
      throw Incompatible{};
    }
    return;
  }
  if (K1 == DK::Tunivar && K2 == DK::Tunivar) {
    try {
      unify_univar(t1e, t2e, univar_pairs);
    } catch (const CannotUnifyUniversalVariables&) {
      throw Incompatible{};
    } catch (const OutOfScopeUniversalVariable&) {
    }
    return;
  }
  throw Incompatible{};
}

void mcomp(env::t env, TypeExpr* t1, TypeExpr* t2) {
  TypePairs tp;
  mcomp_rec(tp, env, t1, t2);
}

static void mcomp_for(TraceExn tr_exn, env::t env, TypeExpr* t1, TypeExpr* t2) {
  try {
    mcomp(env, t1, t2);
  } catch (const Incompatible&) {
    raise_unexplained_for(tr_exn);
  }
}

// ---- real unification -----------------------------------------------------------------
// This function can be called only in [Pattern] mode.
static void add_gadt_equation(const Uenv& uenv, Path::t source, TypeExpr* destination) {
  env::t env = get_env(uenv);
  if (has_free_univars(env, destination)) {
    occur_univar_or_unscoped(env, destination, true);
  } else if (local_non_recursive_abbrev(uenv, source, destination)) {
    TypeExpr* dest = duplicate_type(destination);
    long expansion_scope = std::max<long>(path::scope(source), get_equations_scope(uenv));
    TypeOrigin origin;
    try {
      origin = type_origin(env::find_type(source, env));
    } catch (const env::NotFound&) {
      throw std::logic_error("Ctype.add_gadt_equation");
    }
    const TypeDeclaration* decl = new_local_type(origin, location::none(), dest, expansion_scope);
    uenv.penv->add_local_constraint(source, decl);
    cleanup_abbrev_memo();
  }
}

bool eq_package_path(env::t env, Path::t p1, Path::t p2) {
  return env::path_equiv(env, p1, p2) ||
         env::path_equiv(env, env::normalize_modtype_path(env, p1),
                         env::normalize_modtype_path(env, p2));
}

std::function<TypeExpr*(env::t, const std::vector<Ident::t>&, TypeExpr*)> nondep_type_ref;
std::function<PackageSubtypeResult(env::t, const Package*, const Package*)> package_subtype;

static TypeExpr* nondep_instance(env::t env, long level, Ident::t id, TypeExpr* ty) {
  TypeExpr* t = nondep_type_ref(env, {id}, ty);
  if (level == generic_level) return duplicate_type(t);
  return with_level(level, [&] { return instance(t); });
}


// Find the type paths nl1 in the module type pack2, and add them to the list
// (nl2, tl2).
CompleteResult internal::complete_type_list(et::Position pos, env::t env,
                                            Slice<PackConstraint> fl1, long lv2,
                                            const Package* pack2, bool allow_absent) {
  struct ExitEx {
    et::FirstClassModule e;
  };
  auto mismatch = [&](Slice<std::string_view> lhs, const TypeDeclaration* decl) {
    et::FirstClassModule f;
    f.kind = et::FirstClassModule::Kind::Constraint_on_mismatched_type;
    f.pos = pos;
    f.decl = decl;
    f.lhs.assign(lhs.begin(), lhs.end());
    return ExitEx{f};
  };
  Ident::t id2 = Ident::create_local("Pkg");
  auto* mt = make<ModuleType>(ModuleType::Kind::Mty_ident);
  mt->path = pack2->pack_path;
  env::t env2 = env::add_module(id2, ModulePresence::Mp_present, mt, env);
  auto lex_less = [](Slice<std::string_view> a, Slice<std::string_view> b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  };
  auto lex_eq = [](Slice<std::string_view> a, Slice<std::string_view> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
  };
  std::function<std::vector<PackConstraint>(std::size_t, std::size_t)> complete =
      [&](std::size_t i1, std::size_t i2) -> std::vector<PackConstraint> {
    Slice<PackConstraint> fl2 = pack2->pack_constraints;
    if (i1 == fl1.size()) return {fl2.begin() + i2, fl2.end()};
    Slice<std::string_view> n = fl1[i1].path;
    if (i2 < fl2.size() && !lex_less(n, fl2[i2].path)) {  // n >= n2
      PackConstraint nt2 = fl2[i2];
      auto rest = complete(lex_eq(n, fl2[i2].path) ? i1 + 1 : i1, i2 + 1);
      rest.insert(rest.begin(), nt2);
      return rest;
    }
    // lid = "Pkg" :: n
    Longident::t lid = Longident::lident("Pkg");
    for (std::string_view c : n) lid = Longident::ldot(lid, location::none(), c, location::none());
    std::pair<Path::t, const TypeDeclaration*> found;
    try {
      found = env::find_type_by_name(lid, env2);
    } catch (const env::NotFound&) {
      if (allow_absent) return complete(i1 + 1, i2);
      et::FirstClassModule f;
      f.kind = et::FirstClassModule::Kind::Constraint_on_missing_type;
      f.pos = pos;
      f.lhs.assign(n.begin(), n.end());
      throw ExitEx{f};
    }
    const TypeDeclaration* decl = found.second;
    bool abstract_public = decl->type_arity == 0 &&
                           decl->type_kind->kind == TypeKind::Kind::Type_abstract &&
                           decl->type_private == PrivateFlag::Public;
    if (abstract_public && decl->type_manifest) {
      TypeExpr* t;
      try {
        t = nondep_instance(env2, lv2, id2, decl->type_manifest);
      } catch (const NondepCannotErase&) {
        if (allow_absent) return complete(i1 + 1, i2);
        et::FirstClassModule f;
        f.kind = et::FirstClassModule::Kind::Constraint_with_deps;
        f.pos = pos;
        f.lhs.assign(n.begin(), n.end());
        throw ExitEx{f};
      }
      auto rest = complete(i1 + 1, i2);
      rest.insert(rest.begin(), PackConstraint{n, t});
      return rest;
    }
    if (abstract_public && !decl->type_manifest) {
      if (allow_absent) return complete(i1 + 1, i2);
      throw mismatch(n, decl);
    }
    throw mismatch(n, decl);
  };
  try {
    return {true, complete(0, 0)};
  } catch (const ExitEx& e) {
    return {false, {}, e.e};
  }
}

PackageSubtypeResult internal::compare_package(env::t env,
                                            const std::function<void(TypeExpr*, TypeExpr*)>& unify_f,
                                            long lv1, const Package* pack1, long lv2,
                                            const Package* pack2) {
  auto check = [](const CompleteResult& r) -> const std::vector<PackConstraint>& {
    if (!r.ok) {
      auto x = elt(EK::First_class_module);
      x.fcm = r.err;
      raise_for(TraceExn::Unify, x);
    }
    return r.res;
  };
  // `let ntl2 = .. and ntl1 = ..`: left to right
  CompleteResult ntl2r = complete_type_list(et::Position::Second, env, pack1->pack_constraints, lv2, pack2);
  CompleteResult ntl1r = complete_type_list(et::Position::First, env, pack2->pack_constraints, lv1, pack1);
  const auto& ntl2 = check(ntl2r);
  const auto& ntl1 = check(ntl1r);
  // ntl1 and ntl2 have the same length by construction
  for (std::size_t k = 0; k < ntl1.size() && k < ntl2.size(); ++k) unify_f(ntl1[k].ty, ntl2[k].ty);
  if (eq_package_path(env, pack1->pack_path, pack2->pack_path)) return {true};
  PackageSubtypeResult r = package_subtype(env, pack1, pack2);
  if (!r.ok) return r;
  return package_subtype(env, pack2, pack1);
}

// force unification in Reither when one side has a non-conjunctive type
bool rigid_variants = false;

static void unify_rec(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
static void unify2(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
static void unify2_rec(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
static void unify2_expand(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
static void unify3(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
static void unify_fields(const Uenv& uenv, TypeExpr* ty1, TypeExpr* ty2);
static void unify_row(const Uenv& uenv, const RowDesc* row1, const RowDesc* row2);
static void unify_package(const Uenv& uenv, long lvl1, const Package* pack1, long lvl2,
                          const Package* pack2);

static bool unify1_var(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  if (!is_Tvar(t1)) throw std::logic_error("Ctype.unify1_var");
  occur_for(TraceExn::Unify, uenv, t1, t2);
  env::t env = get_env(uenv);
  try {
    occur_univar_or_unscoped_for(TraceExn::Unify, env, t2);
  } catch (const UnifyTrace&) {
    if (in_pattern_mode(uenv)) return false;
    throw;
  }
  try {
    update_level(env, get_level(t1), t2);
    update_level(env, get_level(t2), t1);  // for Texpand
    update_scope(get_scope(t1), t2);
  } catch (const Escape& e) {
    raise_for(TraceExn::Unify, escape_elt(e.esc));
  }
  link_type(t1, t2);
  return true;
}

// Called from unify3
static void unify3_var(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  occur_for(TraceExn::Unify, uenv, t1, t2);
  try {
    occur_univar_or_unscoped_for(TraceExn::Unify, get_env(uenv), t2);
  } catch (const UnifyTrace&) {
    if (!in_pattern_mode(uenv)) throw;
    reify(uenv, t1, t1, t2);
    reify(uenv, t2, t1, t2);
    occur_univar_or_unscoped(get_env(uenv), t2, true);
    record_equation(uenv, t1, t2);
    return;
  }
  link_type(t1, t2);
}

// (see the long comment in ctype.ml on abbreviations and unification)
static void unify_rec(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  // First step: special cases (optimizations)
  if (unify_eq(uenv, t1, t2)) return;
  bool reset_tracing = check_trace_gadt_instances(get_env(uenv));
  try {
    type_changed = true;
    const TypeDesc* d1 = get_desc(t1);
    const TypeDesc* d2 = get_desc(t2);
    auto c1 = as<Tconstr>(d1);
    auto c2 = as<Tconstr>(d2);
    if (d1->kind == DescKind::Tvar && c2 && !c2->args.empty()) {
      unify2(uenv, t1, t2);
    } else if (c1 && !c1->args.empty() && d2->kind == DescKind::Tvar) {
      unify2(uenv, t1, t2);
    } else if (d1->kind == DescKind::Tvar) {
      if (!unify1_var(uenv, t1, t2)) unify2(uenv, t1, t2);
    } else if (d2->kind == DescKind::Tvar) {
      if (!unify1_var(uenv, t2, t1)) unify2(uenv, t1, t2);
    } else if (d1->kind == DescKind::Tunivar && d2->kind == DescKind::Tunivar) {
      unify_univar_for(TraceExn::Unify, t1, t2, univar_pairs);
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(t1), t2);
      update_scope_for(TraceExn::Unify, get_scope(t1), t2);
      link_type(t1, t2);
    } else {
      const TypeDesc* d3 = get_constr_desc(t1);
      const TypeDesc* d4 = get_constr_desc(t2);
      auto c3 = as<Tconstr>(d3);
      auto c4 = as<Tconstr>(d4);
      if (c3 && c4 && c3->args.empty() && c4->args.empty() &&
          env::path_equiv(get_env(uenv), c3->path, c4->path) &&
          // This optimization assumes that t1 does not expand to t2 (and
          // conversely), so we fall back to the general case when any of
          // the types has a cached expansion.
          !(has_cached_expansion(c3->path, c3->memo->contents) ||
            has_cached_expansion(c4->path, c4->memo->contents)) &&
          (d1 == d3 || d2 == d4)) {
        auto unify1_constr = [&](TypeExpr* a, TypeExpr* b) {
          update_level_for(TraceExn::Unify, get_env(uenv), get_level(a), b);
          update_scope_for(TraceExn::Unify, get_scope(a), b);
          link_type(a, b);
        };
        if (d1 == d3) unify1_constr(t1, t2);
        else unify1_constr(t2, t1);
      } else if (c1 && c2 && env::has_local_constraints(get_env(uenv))) {
        unify2_rec(uenv, t1, t2);
      } else {
        unify2(uenv, t1, t2);
      }
    }
    reset_trace_gadt_instances(reset_tracing);
  } catch (UnifyTrace& e) {
    reset_trace_gadt_instances(reset_tracing);
    raise_trace_for(TraceExn::Unify, cons(diff_elt(t1, t2), std::move(e.trace)));
  }
}

static void unify2(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) { unify2_expand(uenv, t1, t2); }

static void unify2_rec(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  if (unify_eq(uenv, t1, t2)) return;
  try {
    auto* c1 = as<Tconstr>(get_desc(t1));
    auto* c2 = as<Tconstr>(get_desc(t2));
    if (!(c1 && c2)) throw CannotExpand{};
    if (env::path_equiv(get_env(uenv), c1->path, c2->path) && c1->args.empty() &&
        c2->args.empty() &&
        !(has_cached_expansion(c1->path, c1->memo->contents) ||
          has_cached_expansion(c2->path, c2->memo->contents))) {
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(t1), t2);
      update_scope_for(TraceExn::Unify, get_scope(t1), t2);
      link_type(t1, t2);
      return;
    }
    env::t env = get_env(uenv);
    if (find_expansion_scope(env, c1->path) > find_expansion_scope(env, c2->path))
      unify2_rec(uenv, t1, try_expand_safe(env, t2));
    else
      unify2_rec(uenv, try_expand_safe(env, t1), t2);
  } catch (const CannotExpand&) {
    unify2_expand(uenv, t1, t2);
  }
}

static void unify2_expand(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  // Second step: expansion of abbreviations
  env::t env = get_env(uenv);
  expand_head_unif(env, t1);
  expand_head_unif(env, t2);
  // vouillon: expanding a type can perform some unification; because of
  // caching, a second expansion gives the right result.
  expand_head_unif(env, t1);
  expand_head_unif(env, t2);
  long lv = std::min(get_level(t1), get_level(t2));
  long scope = std::max(get_scope(t1), get_scope(t2));
  update_level_for(TraceExn::Unify, env, lv, t2);
  update_level_for(TraceExn::Unify, env, lv, t1);
  update_scope_for(TraceExn::Unify, scope, t2);
  update_scope_for(TraceExn::Unify, scope, t1);
  if (unify_eq(uenv, t1, t2)) return;
  if (!get_abbrev(t1) || get_abbrev(t2)) {
    unify3(uenv, t1, t2);
  } else {
    try {
      unify3(uenv, t2, t1);
    } catch (UnifyTrace& e) {
      raise_trace_for(TraceExn::Unify, et::swap_trace(e.trace));
    }
  }
}

static void unify_list(const Uenv& uenv, Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  if (tl1.size() != tl2.size()) raise_unexplained_for(TraceExn::Unify);
  for (std::size_t k = 0; k < tl1.size(); ++k) unify_rec(uenv, tl1[k], tl2[k]);
}

static void unify_labeled_list(const Uenv& uenv, Slice<LabeledTy> l1, Slice<LabeledTy> l2) {
  if (l1.size() != l2.size()) raise_unexplained_for(TraceExn::Unify);
  for (std::size_t k = 0; k < l1.size(); ++k) {
    if (!(l1[k].label == l2[k].label)) {
      auto x = elt(EK::Tuple_label_mismatch);
      x.tuple_label_diff = {l1[k].label, l2[k].label};
      raise_for(TraceExn::Unify, x);
    }
    unify_rec(uenv, l1[k].ty, l2[k].ty);
  }
}

static void unify3(const Uenv& uenv, TypeExpr* t1p, TypeExpr* t2p) {
  // Third step: truly unification
  TypeExpr* tt1 = repr(t1p);
  const TypeDesc* d1 = tt1->desc;
  const TypeDesc* d2 = get_desc(t2p);
  using DK = DescKind;
  // handle vars and univars specially
  if (d1->kind == DK::Tunivar && d2->kind == DK::Tunivar) {
    unify_univar_for(TraceExn::Unify, t1p, t2p, univar_pairs);
    link_type(t1p, t2p);
    return;
  }
  if (d1->kind == DK::Tvar) {
    unify3_var(uenv, t1p, t2p);
    return;
  }
  if (d2->kind == DK::Tvar) {
    unify3_var(uenv, t2p, t1p);
    return;
  }
  if (d1->kind == DK::Tfield && d2->kind == DK::Tfield) {  // special case for GADTs
    unify_fields(uenv, t1p, t2p);
    return;
  }
  if (in_pattern_mode(uenv)) {
    add_type_equality(uenv, t1p, t2p);
  } else {
    occur_for(TraceExn::Unify, uenv, t1p, t2p);
    link_type(t1p, t2p);
  }
  try {
    bool pm = in_pattern_mode(uenv);
    auto* c1 = as<Tconstr>(d1);
    auto* c2 = as<Tconstr>(d2);
    if (d1->kind == DK::Tarrow && d2->kind == DK::Tarrow) {
      auto* a1 = as<Tarrow>(d1);
      auto* a2 = as<Tarrow>(d2);
      eq_labels(TraceExn::Unify, pm, a1->label, a2->label);
      unify_rec(uenv, a1->t1, a2->t1);
      unify_rec(uenv, a1->t2, a2->t2);
      bool o1 = is_commu_ok(a1->commu), o2 = is_commu_ok(a2->commu);
      if (!o1 && o2) set_commu_ok(a1->commu);
      else if (o1 && !o2) set_commu_ok(a2->commu);
      else if (!o1 && !o2) link_commu(a1->commu, a2->commu);
    } else if (d1->kind == DK::Tfunctor && d2->kind == DK::Tfunctor) {
      auto* f1 = as<Tfunctor>(d1);
      auto* f2 = as<Tfunctor>(d2);
      eq_labels(TraceExn::Unify, pm, f1->label, f2->label);
      try {
        unify_package(uenv, get_level(t1p), f1->pack, get_level(t2p), f2->pack);
      } catch (UnifyTrace& e) {
        TypeExpr* got = newty(tpackage(f1->pack));
        TypeExpr* expected = newty(tpackage(f2->pack));
        raise_trace_for(TraceExn::Unify, cons(diff_elt(got, expected), std::move(e.trace)));
      }
      env::t env = get_env(uenv);
      const ModuleType* mty2 = modtype_of_package(env, location::none(), f2->pack);
      enter_functor_for_unify(uenv, f1->id, newty(d1), f2->id, t2p, mty2,
                              [&](const Uenv& u) { unify_rec(u, f1->body, f2->body); });
    } else if (d1->kind == DK::Tfunctor && d2->kind == DK::Tarrow) {
      auto* f1 = as<Tfunctor>(d1);
      auto* a2 = as<Tarrow>(d2);
      eq_labels(TraceExn::Unify, pm, f1->label, a2->label);
      unify_rec(uenv, newmono_package(f1->pack), a2->t1);
      env::t env = get_env(uenv);
      const ModuleType* mty1 = modtype_of_package(env, location::none(), f1->pack);
      identifier_escape_for(
          TraceExn::Unify,
          env::add_module(Ident::of_unscoped(f1->id), ModulePresence::Mp_present, mty1, env),
          {f1->id}, f1->body);
      unify_rec(uenv, f1->body, a2->t2);
      if (!is_commu_ok(a2->commu)) set_commu_ok(a2->commu);
    } else if (d1->kind == DK::Tarrow && d2->kind == DK::Tfunctor) {
      auto* a1 = as<Tarrow>(d1);
      auto* f2 = as<Tfunctor>(d2);
      eq_labels(TraceExn::Unify, pm, a1->label, f2->label);
      unify_rec(uenv, a1->t1, newmono_package(f2->pack));
      env::t env = get_env(uenv);
      const ModuleType* mty2 = modtype_of_package(env, location::none(), f2->pack);
      identifier_escape_for(
          TraceExn::Unify,
          env::add_module(Ident::of_unscoped(f2->id), ModulePresence::Mp_present, mty2, env),
          {f2->id}, f2->body);
      unify_rec(uenv, a1->t2, f2->body);
      if (!is_commu_ok(a1->commu)) set_commu_ok(a1->commu);
    } else if (d1->kind == DK::Ttuple && d2->kind == DK::Ttuple) {
      unify_labeled_list(uenv, as<Ttuple>(d1)->elems, as<Ttuple>(d2)->elems);
    } else if (c1 && c2 && env::path_equiv(get_env(uenv), c1->path, c2->path)) {
      Slice<TypeExpr*> tl1 = c1->args, tl2 = c2->args;
      if (!pm) {
        unify_list(uenv, tl1, tl2);
      } else if (can_assume_injective(uenv)) {
        Uenv u2 = uenv;
        u2.assume_injective = false;  // without_assume_injective
        unify_list(u2, tl1, tl2);
      } else {
        bool datatype = in_current_module(c1->path);  // || in_pervasives p1
        if (!datatype) {
          std::vector<Path::t> ps;
          if (const PathArgs* a = get_abbrev(t1p)) ps.push_back(a->path);
          if (const PathArgs* a = get_abbrev(t2p)) ps.push_back(a->path);
          ps.push_back(c1->path);
          for (Path::t p : ps)
            if (expands_to_datatype(get_env(uenv), p)) {
              datatype = true;
              break;
            }
        }
        if (datatype) {
          unify_list(uenv, tl1, tl2);
        } else {
          std::vector<bool> inj;
          try {
            for (variance::t v : env::find_type(c1->path, get_env(uenv))->type_variance)
              inj.push_back(variance::mem(variance::F::Inj, v));
          } catch (const env::NotFound&) {
            inj.assign(tl1.size(), false);
          }
          if (tl1.size() != tl2.size() || inj.size() != tl1.size())
            throw std::invalid_argument("List.combine/iter2");
          for (std::size_t k = 0; k < inj.size(); ++k) {
            if (inj[k]) {
              unify_rec(uenv, tl1[k], tl2[k]);
            } else {
              reify(uenv, tl1[k]);
              reify(uenv, tl2[k]);
            }
          }
        }
      }
    } else if (c1 && c2 && c1->args.empty() && c2->args.empty() && pm &&
               is_instantiable(get_env(uenv), c1->path) &&
               is_instantiable(get_env(uenv), c2->path)) {
      Path::t source;
      TypeExpr* destination;
      if (path::scope(c1->path) > path::scope(c2->path)) {
        source = c1->path;
        destination = t2p;
      } else {
        source = c2->path;
        destination = t1p;
      }
      record_equation(uenv, t1p, t2p);
      add_gadt_equation(uenv, source, destination);
    } else if (c1 && c1->args.empty() && pm && is_instantiable(get_env(uenv), c1->path)) {
      reify(uenv, t2p, t1p, t2p);
      record_equation(uenv, t1p, t2p);
      add_gadt_equation(uenv, c1->path, t2p);
    } else if (c2 && c2->args.empty() && pm && is_instantiable(get_env(uenv), c2->path)) {
      reify(uenv, t1p, t1p, t2p);
      record_equation(uenv, t1p, t2p);
      add_gadt_equation(uenv, c2->path, t1p);
    } else if ((c1 || c2) && pm) {
      reify(uenv, t1p, t1p, t2p);
      reify(uenv, t2p, t1p, t2p);
      mcomp_for(TraceExn::Unify, get_env(uenv), t1p, t2p);
      record_equation(uenv, t1p, t2p);
    } else if (d1->kind == DK::Tobject && d2->kind == DK::Tobject) {
      auto* o1 = as<Tobject>(d1);
      unify_fields(uenv, o1->fields, as<Tobject>(d2)->fields);
      // Type [t2'] may have been instantiated by [unify_fields]
      // XXX One should do some kind of unification...
      if (auto* o2 = as<Tobject>(get_desc(t2p))) {
        const PathArgs* nm2 = o2->name->contents;
        bool skip = false;
        if (nm2 && !nm2->args.empty()) {
          auto k = get_desc(nm2->args[0])->kind;
          skip = k == DK::Tvar || k == DK::Tunivar || k == DK::Tnil;
        }
        if (!skip) set_name(o2->name, o1->name->contents);
      }
    } else if (d1->kind == DK::Tvariant && d2->kind == DK::Tvariant) {
      const RowDesc* row1 = as<Tvariant>(d1)->row;
      const RowDesc* row2 = as<Tvariant>(d2)->row;
      if (!pm) {
        unify_row(uenv, row1, row2);
      } else {
        Snapshot snap = btype::snapshot();
        try {
          unify_row(uenv, row1, row2);
        } catch (const UnifyTrace&) {
          btype::backtrack(snap);
          reify(uenv, t1p, t1p, t2p);
          reify(uenv, t2p, t1p, t2p);
          mcomp_for(TraceExn::Unify, get_env(uenv), t1p, t2p);
          record_equation(uenv, t1p, t2p);
        }
      }
    } else if ((d1->kind == DK::Tfield && d2->kind == DK::Tnil) ||
               (d1->kind == DK::Tnil && d2->kind == DK::Tfield)) {
      auto* f = as<Tfield>(d1->kind == DK::Tfield ? d1 : d2);
      if (field_kind_repr(f->kind_) == FieldKindView::Fprivate && f->label != dummy_method) {
        link_kind(f->kind_, field_absent());
        if (d2->kind == DK::Tnil) unify_rec(uenv, f->rest, t2p);
        else unify_rec(uenv, newgenty(tnil()), f->rest);
      } else if (f->label == dummy_method) {
        raise_for(TraceExn::Unify, obj_elt({et::Obj::Kind::Self_cannot_be_closed}));
      } else if (d1->kind == DK::Tnil) {
        raise_for(TraceExn::Unify,
                  obj_elt({et::Obj::Kind::Missing_field, et::Position::First, f->label}));
      } else {
        raise_for(TraceExn::Unify,
                  obj_elt({et::Obj::Kind::Missing_field, et::Position::Second, f->label}));
      }
    } else if (d1->kind == DK::Tnil && d2->kind == DK::Tnil) {
    } else if (d1->kind == DK::Tpoly && d2->kind == DK::Tpoly) {
      auto* p1 = as<Tpoly>(d1);
      auto* p2 = as<Tpoly>(d2);
      if (p1->vars.empty() && p2->vars.empty()) unify_rec(uenv, p1->body, p2->body);
      else
        enter_poly_for(TraceExn::Unify, get_env(uenv), p1->body, p1->vars, p2->body, p2->vars,
                       [&](TypeExpr* a, TypeExpr* b) { unify_rec(uenv, a, b); });
    } else if (d1->kind == DK::Tpackage && d2->kind == DK::Tpackage) {
      unify_package(uenv, get_level(t1p), as<Tpackage>(d1)->pack, get_level(t2p),
                    as<Tpackage>(d2)->pack);
    } else if (d1->kind == DK::Tnil && c2) {
      raise_for(TraceExn::Unify, obj_elt({et::Obj::Kind::Abstract_row, et::Position::Second}));
    } else if (c1 && d2->kind == DK::Tnil) {
      raise_for(TraceExn::Unify, obj_elt({et::Obj::Kind::Abstract_row, et::Position::First}));
    } else {
      raise_unexplained_for(TraceExn::Unify);
    }
  } catch (UnifyTrace& e) {
    transient_expr::set_desc(tt1, d1);
    throw;
  }
}

static void unify_package(const Uenv& uenv, long lvl1, const Package* pack1, long lvl2,
                          const Package* pack2) {
  PackageSubtypeResult r;
  try {
    r = compare_package(get_env(uenv), [&](TypeExpr* a, TypeExpr* b) { unify_rec(uenv, a, b); },
                        lvl1, pack1, lvl2, pack2);
  } catch (const env::NotFound&) {
    if (!in_pattern_mode(uenv)) raise_unexplained_for(TraceExn::Unify);
    for (auto& c : pack1->pack_constraints) reify(uenv, c.ty);
    for (auto& c : pack2->pack_constraints) reify(uenv, c.ty);
    return;
  }
  if (r.ok) return;
  if (!in_pattern_mode(uenv)) {
    auto x = elt(EK::First_class_module);
    x.fcm = r.err;
    raise_for(TraceExn::Unify, x);
  }
  for (auto& c : pack1->pack_constraints) reify(uenv, c.ty);
  for (auto& c : pack2->pack_constraints) reify(uenv, c.ty);
}

// Build a fresh row variable for unification
static TypeExpr* make_rowvar(long level, bool use1, TypeExpr* rest1, bool use2, TypeExpr* rest2) {
  auto set_name_ = [](TypeExpr* ty, OptStr name) {
    if (auto* v = as<Tvar>(get_desc(ty)); v && !v->name.some) set_type_desc(ty, tvar(name));
  };
  OptStr name = OptStr::none();
  auto* v1 = as<Tvar>(get_desc(rest1));
  auto* v2 = as<Tvar>(get_desc(rest2));
  if (v1 && v1->name.some && v2 && v2->name.some) {
    name = get_level(rest1) <= get_level(rest2) ? v1->name : v2->name;
  } else if (v1 && v1->name.some) {
    name = v1->name;
    if (use2) set_name_(rest2, name);
  } else if (v2 && v2->name.some) {
    name = v2->name;
    if (use1) set_name_(rest2, name);  // (sic: rest2, as in ctype.ml)
  }
  if (use1) return rest1;
  if (use2) return rest2;
  return newty2(level, tvar(name));
}

void internal::unify_kind(FieldKind* k1, FieldKind* k2) {
  auto a = field_kind_repr(k1), b = field_kind_repr(k2);
  if (a == FieldKindView::Fprivate &&
      (b == FieldKindView::Fprivate || b == FieldKindView::Fpublic))
    link_kind(k1, k2);
  else if (a == FieldKindView::Fpublic && b == FieldKindView::Fprivate)
    link_kind(k2, k1);
  else if (a == FieldKindView::Fpublic && b == FieldKindView::Fpublic)
    ;
  else
    throw std::logic_error("Ctype.unify_kind");
}

static void unify_fields(const Uenv& uenv, TypeExpr* ty1, TypeExpr* ty2) {  // Optimization
  // `let (fields1, rest1) = .. and (fields2, rest2) = ..`: left to right
  auto [fields1, rest1] = flatten_fields(ty1);
  auto [fields2, rest2] = flatten_fields(ty2);
  AssociatedFields af = associate_fields(fields1, fields2);
  long l1 = get_level(ty1), l2 = get_level(ty2);
  TypeExpr* va = make_rowvar(std::min(l1, l2), af.miss2.empty(), rest1, af.miss1.empty(), rest2);
  TypeExpr* tr1 = repr(rest1);
  TypeExpr* tr2 = repr(rest2);
  const TypeDesc* d1 = tr1->desc;
  const TypeDesc* d2 = tr2->desc;
  try {
    unify_rec(uenv, build_fields(l1, af.miss1, va), rest2);
    unify_rec(uenv, rest1, build_fields(l2, af.miss2, va));
    for (auto& p : af.pairs) {
      unify_kind(p.k1, p.k2);
      try {
        if (trace_gadt_instances && !in_subst_mode(uenv)) {
          // in_subst_mode: see PR#11771
          update_level_for(TraceExn::Unify, get_env(uenv), get_level(va), p.t1);
          update_scope_for(TraceExn::Unify, get_scope(va), p.t1);
        }
        unify_rec(uenv, p.t1, p.t2);
      } catch (UnifyTrace& e) {
        auto x = elt(EK::Incompatible_fields);
        x.field_name = p.name;
        x.field_diff = {p.t1, p.t2};
        raise_trace_for(TraceExn::Unify, cons(x, std::move(e.trace)));
      }
    }
  } catch (...) {
    transient_expr::set_desc(tr1, d1);
    transient_expr::set_desc(tr2, d2);
    throw;
  }
}

static void unify_row_field(const Uenv& uenv, const FixedExplanation* fixed1,
                            const FixedExplanation* fixed2, TypeExpr* rm1, TypeExpr* rm2,
                            std::string_view l, const RowField* f1, const RowField* f2);

static void unify_row(const Uenv& uenv, const RowDesc* row1, const RowDesc* row2) {
  RowDescRepr r1d = row_repr(row1);
  RowDescRepr r2d = row_repr(row2);
  TypeExpr* rm1 = r1d.more;
  TypeExpr* rm2 = r2d.more;
  if (unify_eq(uenv, rm1, rm2)) return;
  MergedRowFields m = merge_row_fields(r1d.fields, r2d.fields);
  auto& r1 = m.r1;
  auto& r2 = m.r2;
  auto& pairs = m.pairs;
  if (!r1.empty() && !r2.empty()) {
    std::unordered_map<long, std::string_view> ht;
    for (auto& e : r1) ht[hash_variant(e.label)] = e.label;  // Hashtbl.add: latest wins find
    for (auto& e : r2)
      if (auto it = ht.find(hash_variant(e.label)); it != ht.end()) throw Tags(e.label, it->second);
  }
  const FixedExplanation* fixed1 = fixed_explanation(row1);
  const FixedExplanation* fixed2 = fixed_explanation(row2);
  TypeExpr* more;
  if (fixed1 && fixed2) more = get_level(rm2) < get_level(rm1) ? rm2 : rm1;
  else if (fixed1) more = rm1;
  else if (fixed2) more = rm2;
  else more = newty2(std::min(get_level(rm1), get_level(rm2)), tvar(OptStr::none()));
  // `let fixed = .. and closed = ..`
  const FixedExplanation* fixed = merge_fixed_explanation(fixed1, fixed2);
  bool closed = r1d.closed || r2d.closed;
  using V = RowFieldView::Kind;
  auto keep = [&](bool swapped) {
    for (auto& p : pairs) {
      const RowField* f1 = swapped ? p.f2 : p.f1;
      const RowField* f2 = swapped ? p.f1 : p.f2;
      if (!(row_field_repr(f1).kind == V::Rabsent || row_field_repr(f2).kind != V::Rabsent))
        return false;
    }
    return true;
  };
  auto empty = [](const std::vector<RowFieldEntry>& fields) {
    return std::all_of(fields.begin(), fields.end(), [](const RowFieldEntry& e) {
      return row_field_repr(e.field).kind == V::Rabsent;
    });
  };
  // Check whether we are going to build an empty type
  if (closed && (empty(r1) || r2d.closed) && (empty(r2) || r1d.closed) &&
      std::all_of(pairs.begin(), pairs.end(), [](const MergedRowFields::Pair& p) {
        return row_field_repr(p.f1).kind == V::Rabsent || row_field_repr(p.f2).kind == V::Rabsent;
      })) {
    et::Variant v;
    v.kind = et::Variant::Kind::No_intersection;
    raise_for(TraceExn::Unify, variant_elt(v));
  }
  const PathArgs* name;
  if (r1d.name && (r1d.closed || empty(r2)) && (!r2d.closed || (keep(false) && empty(r1))))
    name = r1d.name;
  else if (r2d.name && (r2d.closed || empty(r1)) && (!r1d.closed || (keep(true) && empty(r2))))
    name = r2d.name;
  else
    name = nullptr;
  auto set_more = [&](et::Position pos, const RowDesc* row, std::vector<RowFieldEntry> rest) {
    if (closed) rest = filter_row_fields(row_closed(row), rest);
    const FixedExplanation* fx = fixed_explanation(row);
    if (!fx) {
      if (!rest.empty() && row_closed(row)) {
        et::Variant v;
        v.kind = et::Variant::Kind::No_tags;
        v.pos = pos;
        v.tags = rest;
        raise_for(TraceExn::Unify, variant_elt(v));
      }
    } else {
      if (closed && !row_closed(row)) {
        et::Variant v;
        v.kind = et::Variant::Kind::Fixed_row;
        v.pos = pos;
        v.fixed_case.kind = et::FixedRowCaseKind::Cannot_be_closed;
        v.fixed = fx;
        raise_for(TraceExn::Unify, variant_elt(v));
      } else if (!rest.empty()) {
        et::Variant v;
        v.kind = et::Variant::Kind::Fixed_row;
        v.pos = pos;
        v.fixed_case.kind = et::FixedRowCaseKind::Cannot_add_tags;
        for (auto& e : rest) v.fixed_case.tags.push_back(e.label);
        v.fixed = fx;
        raise_for(TraceExn::Unify, variant_elt(v));
      }
    }
    // The following test is not principal... should rather use Tnil
    TypeExpr* rm = row_more(row);
    if (trace_gadt_instances && !in_subst_mode(uenv))  // in_subst_mode: see PR#11771
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(rm), newgenty(tvariant(row)));
    if (has_fixed_explanation(row)) {
      if (eq_type(more, rm)) return;
      if (is_Tvar(rm)) link_type(rm, more);
      else unify_rec(uenv, rm, more);
    } else {
      TypeExpr* ty = newgenty(tvariant(create_row(slice(rest), more, closed, fixed, name)));
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(rm), ty);
      update_scope_for(TraceExn::Unify, get_scope(rm), ty);
      link_type(rm, ty);
    }
  };
  TypeExpr* tm1 = repr(rm1);
  TypeExpr* tm2 = repr(rm2);
  const TypeDesc* md1 = tm1->desc;
  const TypeDesc* md2 = tm2->desc;
  try {
    set_more(et::Position::Second, row2, r1);
    set_more(et::Position::First, row1, r2);
    for (auto& p : pairs) {
      try {
        unify_row_field(uenv, fixed1, fixed2, rm1, rm2, p.label, p.f1, p.f2);
      } catch (UnifyTrace& e) {
        et::Variant v;
        v.kind = et::Variant::Kind::Incompatible_types_for;
        v.name = p.label;
        raise_trace_for(TraceExn::Unify, cons(variant_elt(v), std::move(e.trace)));
      }
    }
    if (static_row(row1)) {
      TypeExpr* rm = row_more(row1);
      if (is_Tvar(rm)) link_type(rm, newty2(get_level(rm), tnil()));
    }
  } catch (...) {
    transient_expr::set_desc(tm1, md1);
    transient_expr::set_desc(tm2, md2);
    throw;
  }
}

static void unify_row_field(const Uenv& uenv, const FixedExplanation* fixed1,
                            const FixedExplanation* fixed2, TypeExpr* rm1, TypeExpr* rm2,
                            std::string_view l, const RowField* f1, const RowField* f2) {
  auto if_not_fixed = [&](et::Position pos, const FixedExplanation* fixed,
                          const std::function<void()>& f) {
    if (!fixed) {
      f();
      return;
    }
    et::Variant v;
    v.kind = et::Variant::Kind::Fixed_row;
    v.pos = pos;
    v.fixed_case.kind = et::FixedRowCaseKind::Cannot_add_tags;
    v.fixed_case.tags = {l};
    v.fixed = fixed;
    raise_trace_for(TraceExn::Unify, {variant_elt(v)});
  };
  bool either_fixed = fixed1 || fixed2;
  if (f1 == f2) return;
  RowFieldView a = row_field_repr(f1), b = row_field_repr(f2);
  using V = RowFieldView::Kind;
  auto no_tags = [&](et::Position pos, const RowField* f) {
    et::Variant v;
    v.kind = et::Variant::Kind::No_tags;
    v.pos = pos;
    v.tags = {{l, f}};
    raise_trace_for(TraceExn::Unify, {variant_elt(v)});
  };
  if (a.kind == V::Rpresent && a.present && b.kind == V::Rpresent && b.present) {
    unify_rec(uenv, a.present, b.present);
  } else if (a.kind == V::Rpresent && !a.present && b.kind == V::Rpresent && !b.present) {
  } else if (a.kind == V::Reither && b.kind == V::Reither) {
    if (eq_row_field_ext(f1, f2)) return;
    bool no_arg = a.constant || b.constant;
    bool matched = a.matched || b.matched;
    auto& tl1 = a.arg_types;
    auto& tl2 = b.arg_types;
    if (either_fixed && !no_arg && tl1.size() == tl2.size()) {
      // PR#7496
      const RowField* f = rf_either(nullptr, no_arg, {}, matched);
      link_row_field_ext(f1, f);
      link_row_field_ext(f2, f);
      for (std::size_t k = 0; k < tl1.size(); ++k) unify_rec(uenv, tl1[k], tl2[k]);
      return;
    }
    bool redo = false;
    if (a.matched || b.matched || either_fixed ||
        (rigid_variants && (tl1.size() == 1 || tl2.size() == 1))) {
      std::vector<TypeExpr*> all = tl1;
      all.insert(all.end(), tl2.begin(), tl2.end());
      if (!all.empty()) {
        if (no_arg) raise_unexplained_for(TraceExn::Unify);
        TypeExpr* t1 = all[0];
        redo = changed_row_field_exts({f1, f2}, [&] {
          for (std::size_t k = 1; k < all.size(); ++k) unify_rec(uenv, t1, all[k]);
        });
      }
    }
    if (redo) {
      unify_row_field(uenv, fixed1, fixed2, rm1, rm2, l, f1, f2);
      return;
    }
    auto remq = [](const std::vector<TypeExpr*>& tl, const std::vector<TypeExpr*>& from) {
      std::vector<TypeExpr*> r;
      for (TypeExpr* ty : from)
        if (std::none_of(tl.begin(), tl.end(), [&](TypeExpr* x) { return eq_type(ty, x); }))
          r.push_back(ty);
      return r;
    };
    std::vector<TypeExpr*> tl1p = remq(tl2, tl1), tl2p = remq(tl1, tl2);
    // PR#6744
    env::t env = get_env(uenv);
    std::vector<TypeExpr*> tlu1, tlu2, rest1, rest2;
    for (TypeExpr* t : tl1p) (has_free_univars(env, t) ? tlu1 : rest1).push_back(t);
    for (TypeExpr* t : tl2p) (has_free_univars(env, t) ? tlu2 : rest2).push_back(t);
    tl1p = rest1;
    tl2p = rest2;
    if (!tlu1.empty() && !tlu2.empty()) {
      // Attempt to merge all the types containing univars
      TypeExpr* tu1 = tlu1[0];
      std::vector<TypeExpr*> others(tlu1.begin() + 1, tlu1.end());
      others.insert(others.end(), tlu2.begin(), tlu2.end());
      for (TypeExpr* t : others) unify_rec(uenv, tu1, t);
    } else if (!tlu1.empty() || !tlu2.empty()) {
      occur_univar_or_unscoped_for(TraceExn::Unify, env, !tlu1.empty() ? tlu1[0] : tlu2[0]);
    }
    // Is this handling of levels really principal?
    auto update_levels = [&](TypeExpr* rm, const std::vector<TypeExpr*>& tys) {
      env::t e = get_env(uenv);
      for (TypeExpr* ty : tys) {
        update_level_for(TraceExn::Unify, e, get_level(rm), ty);
        update_scope_for(TraceExn::Unify, get_scope(rm), ty);
      }
    };
    update_levels(rm2, tl1p);
    update_levels(rm1, tl2p);
    const RowField* f1n = rf_either(nullptr, no_arg, slice(tl2p), matched);
    const RowField* f2n = rf_either(f1n, no_arg, slice(tl1p), matched);
    link_row_field_ext(f1, f1n);
    link_row_field_ext(f2, f2n);
  } else if (a.kind == V::Reither && !a.matched && b.kind == V::Rabsent) {
    if_not_fixed(et::Position::First, fixed1, [&] { link_row_field_ext(f1, f2); });
  } else if (a.kind == V::Rabsent && b.kind == V::Reither && !b.matched) {
    if_not_fixed(et::Position::Second, fixed2, [&] { link_row_field_ext(f2, f1); });
  } else if (a.kind == V::Rabsent && b.kind == V::Rabsent) {
  } else if (a.kind == V::Reither && !a.constant && b.kind == V::Rpresent && b.present) {
    if_not_fixed(et::Position::First, fixed1, [&] {
      Snapshot s = btype::snapshot();
      link_row_field_ext(f1, f2);
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(rm1), b.present);
      update_scope_for(TraceExn::Unify, get_scope(rm1), b.present);
      try {
        for (TypeExpr* t1 : a.arg_types) unify_rec(uenv, t1, b.present);
      } catch (...) {
        undo_first_change_after(s);
        throw;
      }
    });
  } else if (a.kind == V::Rpresent && a.present && b.kind == V::Reither && !b.constant) {
    if_not_fixed(et::Position::Second, fixed2, [&] {
      Snapshot s = btype::snapshot();
      link_row_field_ext(f2, f1);
      update_level_for(TraceExn::Unify, get_env(uenv), get_level(rm2), a.present);
      update_scope_for(TraceExn::Unify, get_scope(rm2), a.present);
      try {
        for (TypeExpr* t : b.arg_types) unify_rec(uenv, a.present, t);
      } catch (...) {
        undo_first_change_after(s);
        throw;
      }
    });
  } else if (a.kind == V::Reither && a.constant && a.arg_types.empty() && b.kind == V::Rpresent &&
             !b.present) {
    if_not_fixed(et::Position::First, fixed1, [&] { link_row_field_ext(f1, f2); });
  } else if (a.kind == V::Rpresent && !a.present && b.kind == V::Reither && b.constant &&
             b.arg_types.empty()) {
    if_not_fixed(et::Position::Second, fixed2, [&] { link_row_field_ext(f2, f1); });
  } else if (a.kind == V::Rabsent && (b.kind == V::Rpresent || (b.kind == V::Reither && b.matched))) {
    no_tags(et::Position::First, f1);
  } else if ((a.kind == V::Rpresent || (a.kind == V::Reither && a.matched)) && b.kind == V::Rabsent) {
    no_tags(et::Position::Second, f2);
  } else {
    // constructor arity mismatch, or inconsistent conjunction on a
    // non-absent field
    raise_unexplained_for(TraceExn::Unify);
  }
}

void unify_uenv(const Uenv& uenv, TypeExpr* ty1, TypeExpr* ty2) {
  Snapshot snap = btype::snapshot();
  try {
    unify_rec(uenv, ty1, ty2);
  } catch (const UnifyTrace& e) {
    undo_compress(snap);
    throw Unify(expand_to_unification_error(get_env(uenv), e.trace));
  }
}

btype::TypePairs* unify_gadt(PatternEnv* penv, TypeExpr* ty1, TypeExpr* ty2) {
  auto* equated_types = new TypePairs();  // lives as long as the result is used
  static std::vector<std::unique_ptr<TypePairs>> keep;
  keep.emplace_back(equated_types);
  auto do_unify_gadt = [&]() {
    auto* eq_set = new TypePairs();
    keep.emplace_back(eq_set);
    Uenv uenv{true};
    uenv.penv = penv;
    uenv.equated_types = equated_types;
    uenv.assume_injective = true;
    uenv.unify_eq_set = eq_set;
    unify_uenv(uenv, ty1, ty2);
    return equated_types;
  };
  bool no_leak = penv->in_counterexample || closed_type_expr(ty2);
  if (no_leak) return with_univar_pairs({}, do_unify_gadt);
  Snapshot snap = btype::snapshot();
  try {
    // If there are free variables, first try normal unification
    Uenv uenv = Uenv::expression(penv->env);
    with_univar_pairs({}, [&] {
      unify_uenv(uenv, ty1, ty2);
      return 0;
    });
    return equated_types;
  } catch (const Unify&) {
    // If it fails, retry in pattern mode
    btype::backtrack(snap);
    return with_univar_pairs({}, do_unify_gadt);
  }
}

void unify_var_uenv(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2) {
  if (eq_type(t1, t2)) return;
  const TypeDesc* d1 = get_desc(t1);
  const TypeDesc* d2 = get_desc(t2);
  if (d1->kind == DescKind::Tvar && d2->kind == DescKind::Tconstr && deep_occur(t1, t2)) {
    unify_uenv(uenv, t1, t2);
    return;
  }
  if (d1->kind == DescKind::Tvar) {
    env::t env = get_env(uenv);
    bool reset_tracing = check_trace_gadt_instances(env);
    try {
      occur_for(TraceExn::Unify, uenv, t1, t2);
      update_level_for(TraceExn::Unify, env, get_level(t1), t2);
      update_scope_for(TraceExn::Unify, get_scope(t1), t2);
      link_type(t1, t2);
      reset_trace_gadt_instances(reset_tracing);
    } catch (UnifyTrace& e) {
      reset_trace_gadt_instances(reset_tracing);
      throw Unify(expand_to_unification_error(env, cons(diff_elt(t1, t2), std::move(e.trace))));
    }
    return;
  }
  unify_uenv(uenv, t1, t2);
}

// the final versions of unification functions
void unify_var(env::t env, TypeExpr* t1, TypeExpr* t2) {
  unify_var_uenv(Uenv::expression(env), t1, t2);
}

void unify_pairs(env::t env, TypeExpr* t1, TypeExpr* t2, std::vector<UnivarPair> pairs) {
  with_univar_pairs(std::move(pairs), [&] {
    unify_uenv(Uenv::expression(env), t1, t2);
    return 0;
  });
}

void unify(env::t env, TypeExpr* t1, TypeExpr* t2) { unify_pairs(env, t1, t2, {}); }

// Lower the level of a type to the current level
void enforce_current_level(env::t env, TypeExpr* ty) { unify_var(env, newvar(), ty); }

TypeExpr* expand_head_trace(env::t env, TypeExpr* t) {
  bool reset_tracing = check_trace_gadt_instances(env);
  TypeExpr* r = expand_head_unif(env, t);
  reset_trace_gadt_instances(reset_tracing);
  return r;
}

}  // namespace cppcaml::typing::ctype
