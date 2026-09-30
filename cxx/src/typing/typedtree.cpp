// Port of typing/typedtree.ml: auxiliary functions over the typed tree.
#include "cppcaml/typing/typedtree.hpp"

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::typedtree {

using PK = PatternDesc::Kind;

const Pattern* as_computation_pattern(const Pattern* p) {
  return make<Pattern>(make<Tpat_value>(Tpat_value{{PK::Tpat_value}, p}), p->pat_loc,
                       Slice<PatExtraItem>{}, p->pat_type, p->pat_env, Attributes{});
}

PatternCategory classify_pattern_desc(const PatternDesc* d) {
  switch (d->kind) {
    case PK::Tpat_value:
    case PK::Tpat_exception: return PatternCategory::Computation;
    case PK::Tpat_or: {
      auto* o = as<Tpat_or>(d);
      PatternCategory c1 = classify_pattern(o->p1), c2 = classify_pattern(o->p2);
      if (c1 != c2) throw std::logic_error("Typedtree.classify_pattern_desc");
      return c1;
    }
    default: return PatternCategory::Value;
  }
}
PatternCategory classify_pattern(const Pattern* p) { return classify_pattern_desc(p->pat_desc); }

void shallow_iter_pattern_desc(const std::function<void(const Pattern*)>& f, const PatternDesc* d) {
  switch (d->kind) {
    case PK::Tpat_alias: f(as<Tpat_alias>(d)->pat); break;
    case PK::Tpat_tuple: for (auto& x : as<Tpat_tuple>(d)->pats) f(x.pat); break;
    case PK::Tpat_construct: for (auto* p : as<Tpat_construct>(d)->args) f(p); break;
    case PK::Tpat_variant: if (auto* p = as<Tpat_variant>(d)->arg) f(p); break;
    case PK::Tpat_record: for (auto& x : as<Tpat_record>(d)->fields) f(x.pat); break;
    case PK::Tpat_array: for (auto* p : as<Tpat_array>(d)->pats) f(p); break;
    case PK::Tpat_lazy: f(as<Tpat_lazy>(d)->pat); break;
    case PK::Tpat_any:
    case PK::Tpat_var:
    case PK::Tpat_constant: break;
    case PK::Tpat_value: f(as<Tpat_value>(d)->pat); break;
    case PK::Tpat_exception: f(as<Tpat_exception>(d)->pat); break;
    case PK::Tpat_or: {
      auto* o = as<Tpat_or>(d);
      f(o->p1);
      f(o->p2);
      break;
    }
  }
}

const PatternDesc* shallow_map_pattern_desc(const std::function<const Pattern*(const Pattern*)>& f,
                                            const PatternDesc* d) {
  switch (d->kind) {
    case PK::Tpat_alias: {
      auto* a = as<Tpat_alias>(d);
      return make<Tpat_alias>(Tpat_alias{{PK::Tpat_alias}, f(a->pat), a->id, a->name, a->uid, a->ty});
    }
    case PK::Tpat_tuple: {
      std::vector<LabeledPattern> l;
      for (auto& x : as<Tpat_tuple>(d)->pats) l.push_back({x.label, f(x.pat)});
      return make<Tpat_tuple>(Tpat_tuple{{PK::Tpat_tuple}, slice(l)});
    }
    case PK::Tpat_record: {
      auto* r = as<Tpat_record>(d);
      std::vector<RecordPatField> l;
      for (auto& x : r->fields) l.push_back({x.lid, x.label, f(x.pat)});
      return make<Tpat_record>(Tpat_record{{PK::Tpat_record}, slice(l), r->closed});
    }
    case PK::Tpat_construct: {
      auto* c = as<Tpat_construct>(d);
      std::vector<const Pattern*> l;
      for (auto* p : c->args) l.push_back(f(p));
      return make<Tpat_construct>(Tpat_construct{{PK::Tpat_construct}, c->lid, c->cstr, slice(l), c->annot});
    }
    case PK::Tpat_array: {
      auto* a = as<Tpat_array>(d);
      std::vector<const Pattern*> l;
      for (auto* p : a->pats) l.push_back(f(p));
      return make<Tpat_array>(Tpat_array{{PK::Tpat_array}, a->mut, slice(l)});
    }
    case PK::Tpat_lazy: return make<Tpat_lazy>(Tpat_lazy{{PK::Tpat_lazy}, f(as<Tpat_lazy>(d)->pat)});
    case PK::Tpat_variant: {
      auto* v = as<Tpat_variant>(d);
      if (!v->arg) return d;
      return make<Tpat_variant>(Tpat_variant{{PK::Tpat_variant}, v->label, f(v->arg), v->row});
    }
    case PK::Tpat_var:
    case PK::Tpat_constant:
    case PK::Tpat_any: return d;
    case PK::Tpat_value: return make<Tpat_value>(Tpat_value{{PK::Tpat_value}, f(as<Tpat_value>(d)->pat)});
    case PK::Tpat_exception:
      return make<Tpat_exception>(Tpat_exception{{PK::Tpat_exception}, f(as<Tpat_exception>(d)->pat)});
    case PK::Tpat_or: {
      auto* o = as<Tpat_or>(d);
      // (f p1, f p2, path): right to left
      const Pattern* p2 = f(o->p2);
      const Pattern* p1 = f(o->p1);
      return make<Tpat_or>(Tpat_or{{PK::Tpat_or}, p1, p2, o->row});
    }
  }
  return d;
}

void iter_general_pattern(const std::function<void(const Pattern*)>& f, const Pattern* p) {
  f(p);
  shallow_iter_pattern_desc([&](const Pattern* q) { iter_general_pattern(f, q); }, p->pat_desc);
}
void iter_pattern(const std::function<void(const Pattern*)>& f, const Pattern* p) {
  iter_general_pattern(
      [&](const Pattern* q) {
        if (classify_pattern(q) == PatternCategory::Value) f(q);
      },
      p);
}
// (the `raise Found` is a flag: [f] is not called once it is set)
bool exists_general_pattern(const std::function<bool(const Pattern*)>& f, const Pattern* p) {
  bool found = false;
  iter_general_pattern(
      [&](const Pattern* q) {
        if (!found && f(q)) found = true;
      },
      p);
  return found;
}
bool exists_pattern(const std::function<bool(const Pattern*)>& f, const Pattern* p) {
  return exists_general_pattern(
      [&](const Pattern* q) { return classify_pattern(q) == PatternCategory::Value && f(q); }, p);
}

// List the identifiers bound by a pattern or a let
static void iter_bound_idents(const std::function<void(const BoundIdent&)>& f, const Pattern* pat) {
  const PatternDesc* d = pat->pat_desc;
  if (auto* v = as<Tpat_var>(d)) {
    f({v->id, v->name, pat->pat_type, v->uid});
  } else if (auto* a = as<Tpat_alias>(d)) {
    iter_bound_idents(f, a->pat);
    f({a->id, a->name, a->ty, a->uid});
  } else if (auto* o = as<Tpat_or>(d)) {
    // Invariant : both arguments bind the same variables
    iter_bound_idents(f, o->p1);
  } else {
    shallow_iter_pattern_desc([&](const Pattern* q) { iter_bound_idents(f, q); }, d);
  }
}

std::vector<BoundIdent> pat_bound_idents_full(const Pattern* p) {
  std::vector<BoundIdent> r;
  iter_bound_idents([&](const BoundIdent& b) { r.push_back(b); }, p);
  return r;
}
std::vector<Ident::t> pat_bound_idents(const Pattern* p) {
  std::vector<Ident::t> r;
  for (auto& b : pat_bound_idents_full(p)) r.push_back(b.id);
  return r;
}
std::vector<BoundIdent> let_bound_idents_full(Slice<const ValueBinding*> vbs) {
  std::vector<BoundIdent> r;
  for (auto* vb : vbs) iter_bound_idents([&](const BoundIdent& b) { r.push_back(b); }, vb->vb_pat);
  return r;
}
std::vector<Ident::t> let_bound_idents(Slice<const ValueBinding*> vbs) {
  std::vector<Ident::t> r;
  for (auto& b : let_bound_idents_full(vbs)) r.push_back(b.id);
  return r;
}

const Pattern* alpha_pat(const std::vector<std::pair<Ident::t, Ident::t>>& env, const Pattern* p) {
  auto alpha_var = [&](Ident::t id) -> Ident::t {
    for (auto& [a, b] : env)
      if (a == id || ident::same(a, id)) return b;  // List.assoc (structural on idents)
    return nullptr;
  };
  auto with_desc = [&](const PatternDesc* d) {
    Pattern* q = make<Pattern>(*p);
    q->pat_desc = d;
    return q;
  };
  const PatternDesc* d = p->pat_desc;
  if (auto* v = as<Tpat_var>(d)) {  // note the "Not_found" case
    Ident::t id2 = alpha_var(v->id);
    if (!id2) return with_desc(make<Tpat_any>(PK::Tpat_any));
    return with_desc(make<Tpat_var>(Tpat_var{{PK::Tpat_var}, id2, v->name, v->uid}));
  }
  if (auto* a = as<Tpat_alias>(d)) {
    const Pattern* new_p = alpha_pat(env, a->pat);
    Ident::t id2 = alpha_var(a->id);
    if (!id2) return new_p;
    return with_desc(make<Tpat_alias>(Tpat_alias{{PK::Tpat_alias}, new_p, id2, a->name, a->uid, a->ty}));
  }
  return with_desc(shallow_map_pattern_desc([&](const Pattern* q) { return alpha_pat(env, q); }, d));
}

std::pair<const Pattern*, const Pattern*> split_pattern(const Pattern* pat) {
  auto into = [](const Pattern* cpat, const Pattern* p1, const Pattern* p2) {
    // The third parameter of [Tpat_or] is [Some _] only for "#typ" patterns,
    // which we do *not* expand.  Hence we can put [None] here.
    Pattern* q = make<Pattern>(*cpat);
    q->pat_desc = make<Tpat_or>(Tpat_or{{PK::Tpat_or}, p1, p2, nullptr});
    return static_cast<const Pattern*>(q);
  };
  auto combine = [&](const Pattern* cpat, const Pattern* p1, const Pattern* p2) -> const Pattern* {
    if (!p1) return p2;
    if (!p2) return p1;
    return into(cpat, p1, p2);
  };
  std::function<std::pair<const Pattern*, const Pattern*>(const Pattern*)> split =
      [&](const Pattern* cpat) -> std::pair<const Pattern*, const Pattern*> {
    const PatternDesc* d = cpat->pat_desc;
    if (auto* v = as<Tpat_value>(d)) return {v->pat, nullptr};
    if (auto* e = as<Tpat_exception>(d)) return {nullptr, e->pat};
    if (auto* o = as<Tpat_or>(d)) {
      auto [vals1, exns1] = split(o->p1);
      auto [vals2, exns2] = split(o->p2);
      // a tuple: right to left
      const Pattern* exns = combine(cpat, exns1, exns2);
      const Pattern* vals = combine(cpat, vals1, vals2);
      return {vals, exns};
    }
    throw std::logic_error("Typedtree.split_pattern");
  };
  return split(pat);
}

// Try to convert a module expression to a module path.
static Path::t path_of_module_rec(const ModuleExpr* me) {
  const ModuleExprDesc* d = me->mod_desc;
  if (auto* i = as<Tmod_ident>(d)) return i->path;
  if (auto* a = as<Tmod_apply>(d); a && clflags::applicative_functors) {
    // Path.Papply (path_of_module funct, path_of_module arg): right to left
    Path::t pa = path_of_module_rec(a->arg);
    if (!pa) return nullptr;
    Path::t pf = path_of_module_rec(a->fn);
    if (!pf) return nullptr;
    return Path::papply(pf, pa);
  }
  if (auto* c = as<Tmod_constraint>(d)) return path_of_module_rec(c->me);
  return nullptr;  // Not_a_path
}
Path::t path_of_module(const ModuleExpr* me) { return path_of_module_rec(me); }

const ModuleExpr* remove_module_constraint(const ModuleExpr* me) {
  if (auto* c = as<Tmod_constraint>(me->mod_desc)) return c->me;
  return me;
}

}  // namespace cppcaml::typing::typedtree

namespace cppcaml::typing::typedtree {
const ModuleCoercion* tcoerce_none() {
  static const ModuleCoercion* c = [] {
    ZoneScope perm(permanent_zone());
    return make<ModuleCoercion>(ModuleCoercion{ModuleCoercion::Kind::Tcoerce_none});
  }();
  return c;
}
}  // namespace cppcaml::typing::typedtree
