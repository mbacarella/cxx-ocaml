// Port of typing/typedecl_variance.ml: variance inference and checking.
#include "cppcaml/typing/typedecl_variance.hpp"

#include <map>
#include <set>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "typedecl_properties.hpp"

namespace cppcaml::typing::typedecl_variance {

using namespace types;
namespace V = variance;
using VF = V::F;

namespace {

// Btype.TypeMap / TypeSet: keyed by the id of the representative (the map
// keeps the key's type expression, as OCaml's Map.add does)
struct TMEntry {
  TypeExpr* ty;
  V::t v;
};
using TypeMap = std::map<long, TMEntry>;
struct VisitedSet {
  std::set<long> s;
  bool mem(TypeExpr* t) const { return s.count(get_id(t)) != 0; }
  void add(TypeExpr* t) { s.insert(get_id(t)); }
};

std::pair<bool, bool> get_upper(V::t v) { return {V::mem(VF::May_pos, v), V::mem(VF::May_neg, v)}; }
std::tuple<bool, bool, bool> get_lower(V::t v) {
  return {V::mem(VF::Pos, v), V::mem(VF::Neg, v), V::mem(VF::Inj, v)};
}

// Compute variance
V::t get_variance(TypeExpr* ty, const TypeMap& visited) {
  auto it = visited.find(get_id(ty));
  return it == visited.end() ? V::null : it->second.v;
}

void compute_variance(env::t env, TypeMap& visited, V::t vari, TypeExpr* ty) {
  std::function<void(env::t, V::t, TypeExpr*)> rec = [&](env::t env, V::t vari, TypeExpr* ty) {
    V::t vari2 = get_variance(ty, visited);
    if (V::subset(vari, vari2)) return;
    vari = V::union_(vari, vari2);
    visited[get_id(ty)] = TMEntry{ty, vari};
    auto compute_same = [&](TypeExpr* t) { rec(env, vari, t); };
    const TypeDesc* d = get_desc(ty);
    switch (d->kind) {
      case DescKind::Tarrow: {
        auto* a = as<Tarrow>(d);
        rec(env, V::conjugate(vari), a->t1);
        compute_same(a->t2);
        return;
      }
      case DescKind::Tfunctor: {
        auto* f = as<Tfunctor>(d);
        auto* mty = make<ModuleType>(ModuleType{ModuleType::Kind::Mty_ident, f->pack->pack_path});
        env::t env2 = env::add_module(Ident::of_unscoped(f->id), ModulePresence::Mp_present, mty, env);
        rec(env, V::conjugate(vari), ctype::newty(tpackage(f->pack)));
        rec(env2, vari, f->body);
        return;
      }
      case DescKind::Ttuple:
        for (auto& x : as<Ttuple>(d)->elems) compute_same(x.ty);
        return;
      case DescKind::Tconstr: {
        auto* c = as<Tconstr>(d);
        if (c->args.empty()) return;
        const TypeDeclaration* decl;
        try {
          decl = env::find_type(c->path, env);
        } catch (const env::NotFound&) {
          for (auto* t : c->args) rec(env, V::unknown, t);
          return;
        }
        if (c->args.size() != decl->type_variance.size()) throw std::invalid_argument("List.iter2");
        for (std::size_t k = 0; k < c->args.size(); ++k) rec(env, V::compose(vari, decl->type_variance[k]), c->args[k]);
        return;
      }
      case DescKind::Tobject: compute_same(as<Tobject>(d)->fields); return;
      case DescKind::Tfield: {
        auto* f = as<Tfield>(d);
        compute_same(f->ty);
        compute_same(f->rest);
        return;
      }
      case DescKind::Tvariant: {
        const RowDesc* row = as<Tvariant>(d)->row;
        for (auto& e : row_fields(row)) {
          RowFieldView f = row_field_repr(e.field);
          if (f.kind == RowFieldView::Kind::Rpresent && f.present) {
            compute_same(f.present);
          } else if (f.kind == RowFieldView::Kind::Reither) {
            V::t v = V::inter(vari, V::unknown);  // cf PR#7269
            for (auto* t : f.arg_types) rec(env, v, t);
          }
        }
        compute_same(row_more(row));
        return;
      }
      case DescKind::Tpoly: compute_same(as<Tpoly>(d)->body); return;
      case DescKind::Tvar:
      case DescKind::Tnil:
      case DescKind::Tunivar: return;
      case DescKind::Tpackage: {
        V::t v = V::compose(vari, V::full());
        for (auto& c : as<Tpackage>(d)->pack->pack_constraints) rec(env, v, c.ty);
        return;
      }
      default: throw std::logic_error("compute_variance");
    }
  };
  rec(env, vari, ty);
}

V::t make(bool p, bool n, bool i) { return V::set_if(p, VF::May_pos, V::set_if(n, VF::May_neg, V::set_if(i, VF::Inj, V::null))); }

const V::t injective = V::single(VF::Inj);

struct Tyl {  // (bool * type_expr) list
  bool cn;
  TypeExpr* ty;
};

Prop compute_variance_type(env::t env, const std::optional<VarianceVariableContext>& check, const Req& required0,
                           const Location& loc, const TypeDeclaration* decl, const std::vector<Tyl>& tyl) {
  // Requirements
  bool check_injectivity = btype::type_kind_is_abstract(decl);
  // c and n reflects respectively + and - in the syntax, and maps
  // respectively to `not May_neg` and `not May_pos` in the Variance.f fields
  Req required;
  for (auto& r : required0) required.push_back({!r.cn, !r.co, check_injectivity ? r.inj : false});
  // Prepare
  Slice<TypeExpr*> params = decl->type_params;
  TypeMap tvl;
  // Compute occurrences in the body
  for (auto& x : tyl) compute_variance(env, tvl, x.cn ? V::full() : V::covariant(), x.ty);
  // Infer injectivity of constrained parameters
  if (check_injectivity) {
    for (TypeExpr* ty : params) {
      if (btype::is_Tvar(ty) || V::mem(VF::Inj, get_variance(ty, tvl))) continue;
      struct Exit {};
      VisitedSet visited;
      std::function<void(TypeExpr*)> chk = [&](TypeExpr* t) {
        if (visited.mem(t)) return;
        visited.add(t);
        if (V::mem(VF::Inj, get_variance(t, tvl))) return;
        const TypeDesc* d = get_desc(t);
        if (d->kind == DescKind::Tvar) throw Exit{};
        if (d->kind == DescKind::Tconstr) {
          VisitedSet old = visited;
          try {
            btype::iter_type_expr(chk, t);
          } catch (const Exit&) {
            visited = old;
            TypeExpr* t2 = ctype::expand_head_opt(env, t);
            if (eq_type(t, t2)) throw Exit{};
            chk(t2);
          }
          return;
        }
        btype::iter_type_expr(chk, t);
      };
      try {
        chk(ty);
        compute_variance(env, tvl, injective, ty);
      } catch (const Exit&) {
      }
    }
  }
  if (check) {
    const VarianceVariableContext& context = *check;
    // Check variance of parameters
    if (params.size() != required.size()) throw std::invalid_argument("List.iter2");
    long pos = 0;
    for (std::size_t k = 0; k < params.size(); ++k) {
      TypeExpr* ty = params[k];
      auto [c, n, i] = required[k];
      ++pos;
      V::t var = get_variance(ty, tvl);
      auto [co, cn] = get_upper(var);
      bool ij = V::mem(VF::Inj, var);
      if ((btype::is_Tvar(ty) && ((co && !c) || (cn && !n))) || (!ij && i)) {
        Error e(loc, Error::Kind::Bad_variance);
        e.variance = VarianceError{true, pos};
        e.s1 = {co, cn, ij};
        e.s2 = {c, n, i};
        throw e;
      }
    }
    // Check propagation from constrained parameters
    std::vector<TypeExpr*> fvl0 =
        ctype::free_variables_list(std::vector<TypeExpr*>(params.begin(), params.end()));
    std::vector<TypeExpr*> fvl;
    for (TypeExpr* v : fvl0) {
      bool in_params = false;
      for (TypeExpr* p : params) in_params = in_params || eq_type(v, p);
      if (!in_params) fvl.push_back(v);
    }
    // If there are no extra variables there is nothing to do
    if (!fvl.empty()) {
      TypeMap tvl2;
      for (std::size_t k = 0; k < params.size(); ++k) {
        TypeExpr* ty = params[k];
        auto [p, n, i] = required[k];
        (void)i;
        if (btype::is_Tvar(ty)) continue;
        V::t v = p ? (n ? V::full() : V::covariant()) : V::conjugate(V::covariant());
        compute_variance(env, tvl2, v, ty);
      }
      VisitedSet visited;
      std::function<void(TypeExpr*)> chk = [&](TypeExpr* ty) {
        if (visited.mem(ty)) return;
        visited.add(ty);
        V::t v1 = get_variance(ty, tvl);
        Snapshot snap = btype::snapshot();
        V::t v2 = V::null;
        for (auto& [id, e] : tvl2) {
          std::vector<TypeExpr*> a{ty}, b{e.ty};
          if (ctype::is_equal(env, false, slice(a), slice(b))) v2 = V::union_(e.v, v2);
        }
        btype::backtrack(snap);
        auto [c1, n1] = get_upper(v1);
        auto [c2, n2, i2] = get_lower(v2);
        if ((c1 && !c2) || (n1 && !n2)) {
          TypeExpr* variable = nullptr;
          for (TypeExpr* f : fvl)
            if (eq_type(ty, f)) {
              variable = f;
              break;
            }
          if (variable) {
            VarianceVariableError error = !i2              ? VarianceVariableError::No_variable
                                          : (c2 || n2)     ? VarianceVariableError::Variance_not_reflected
                                                           : VarianceVariableError::Variance_not_deducible;
            Error e(loc, Error::Kind::Bad_variance);
            e.variance = VarianceError{false, 0, error, context, variable};
            e.s1 = {c1, n1, false};
            e.s2 = {c2, n2, false};
            throw e;
          }
          btype::iter_type_expr(chk, ty);
        }
      };
      for (auto& x : tyl) chk(x.ty);
    }
  }
  if (params.size() != required.size()) throw std::invalid_argument("List.map2");
  Prop out;
  for (std::size_t k = 0; k < params.size(); ++k) {
    TypeExpr* ty = params[k];
    auto [p, n, i0] = required[k];
    (void)i0;
    V::t v = get_variance(ty, tvl);
    PrivateFlag tr = decl->type_private;
    // Use required variance where relevant
    bool concr = !btype::type_kind_is_abstract(decl);
    bool p2 = p, n2 = n;
    if (!(tr == PrivateFlag::Private || !btype::is_Tvar(ty))) {  // only check
      p2 = false;
      n2 = false;
    }
    v = V::union_(v, make(p2, n2, concr));
    if (!concr || btype::is_Tvar(ty)) out.push_back(v);
    else out.push_back(V::union_(v, p ? (n ? V::full() : V::covariant()) : V::conjugate(V::covariant())));
  }
  return out;
}

std::vector<Tyl> add_false(const std::vector<TypeExpr*>& l) {
  std::vector<Tyl> out;
  for (auto* t : l) out.push_back({false, t});
  return out;
}

// A parameter is constrained if it is either instantiated, or it is a
// variable appearing in another parameter
std::optional<AnonymousVarianceError> constrained(const std::vector<std::vector<TypeExpr*>>& vars, TypeExpr* ty) {
  if (get_desc(ty)->kind == DescKind::Tvar) {
    for (auto& l : vars)
      for (TypeExpr* v : l)
        if (eq_type(ty, v)) return AnonymousVarianceError{true, v};
    return std::nullopt;
  }
  return AnonymousVarianceError{false, ty};
}

std::vector<Tyl> for_constr(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple)
    return add_false(std::vector<TypeExpr*>(a.tuple.begin(), a.tuple.end()));
  std::vector<Tyl> out;
  for (auto* ld : a.record) out.push_back({ld->ld_mutable == MutableFlag::Mutable, ld->ld_type});
  return out;
}

const TypeDeclaration* with_private(const TypeDeclaration* decl) {
  TypeDeclaration* d = make<TypeDeclaration>(*decl);
  d->type_private = PrivateFlag::Private;
  return d;
}

Prop compute_variance_gadt(env::t env, const std::optional<VarianceVariableContext>& check, const Req& required,
                           const Location& rloc, const TypeDeclaration* decl, const Location& cloc,
                           const ConstructorArguments& tl, TypeExpr* ret_type_opt) {
  if (!ret_type_opt) return compute_variance_type(env, check, required, rloc, with_private(decl), for_constr(tl));
  auto* tc = as<Tconstr>(get_desc(ret_type_opt));
  if (!tc) throw std::logic_error("compute_variance_gadt");
  // let tyl = List.map (Ctype.expand_head env) tyl in
  std::vector<std::vector<TypeExpr*>> fvl;
  for (TypeExpr* t : tc->args) fvl.push_back(ctype::free_variables(t));
  if (tc->args.size() != required.size()) throw std::invalid_argument("List.fold_left2");
  long index = 1;
  std::vector<std::vector<TypeExpr*>> fv1;  // head first
  std::size_t fv2 = 0;                       // fvl from index fv2
  for (std::size_t k = 0; k < tc->args.size(); ++k) {
    if (fv2 >= fvl.size()) throw std::logic_error("compute_variance_gadt: fv2");
    const std::vector<TypeExpr*>& fv = fvl[fv2];
    // fv1 @ fv2 = free_variables of other parameters
    if (required[k].co || required[k].cn) {
      std::vector<std::vector<TypeExpr*>> others = fv1;
      for (std::size_t j = fv2 + 1; j < fvl.size(); ++j) others.push_back(fvl[j]);
      if (auto reason = constrained(others, tc->args[k])) {
        Error e(cloc, Error::Kind::Varying_anonymous);
        e.n = index;
        e.anon = *reason;
        throw e;
      }
    }
    ++index;
    fv1.insert(fv1.begin(), fv);
    ++fv2;
  }
  TypeDeclaration* d = make<TypeDeclaration>(*decl);
  d->type_params = tc->args;
  d->type_private = PrivateFlag::Private;
  return compute_variance_type(env, check, required, rloc, d, for_constr(tl));
}

Prop compute_variance_decl(env::t env, Ident::t check_id, const TypeDeclaration* decl, const Req& required,
                           const Location& rloc) {
  std::optional<VarianceVariableContext> check;
  if (check_id) check = VarianceVariableContext{VarianceVariableContext::Kind::Type_declaration, check_id, decl};
  bool abstract = btype::type_kind_is_abstract(decl);
  TypeKind::Kind k = decl->type_kind->kind;
  bool no_body = k == TypeKind::Kind::Type_abstract || k == TypeKind::Kind::Type_open ||
                 k == TypeKind::Kind::Type_external;
  if (no_body && !decl->type_manifest) {
    Prop out;
    for (auto& r : required) out.push_back(make(!r.cn, !r.co, !abstract || r.inj));
    return out;
  }
  std::vector<Tyl> mn;
  if (decl->type_manifest) mn.push_back({false, decl->type_manifest});
  Prop vari;
  if (no_body) {
    vari = compute_variance_type(env, check, required, rloc, decl, mn);
  } else if (k == TypeKind::Kind::Type_variant) {
    Slice<const ConstructorDeclaration*> tll = decl->type_kind->constructors;
    bool no_res = true;
    for (auto* c : tll) no_res = no_res && c->cd_res == nullptr;
    if (no_res) {
      std::vector<Tyl> all = mn;
      for (auto* c : tll) {
        std::vector<Tyl> f = for_constr(c->cd_args);
        all.insert(all.end(), f.begin(), f.end());
      }
      vari = compute_variance_type(env, check, required, rloc, decl, all);
    } else {
      std::vector<Prop> varis;
      if (decl->type_manifest)
        varis.push_back(compute_variance_type(env, check, required, rloc, with_private(decl),
                                              add_false({decl->type_manifest})));
      for (auto* tl : tll) {
        // compute_variance_gadt_constructor
        std::optional<VarianceVariableContext> check2;
        if (check) {
          VarianceVariableContext c{VarianceVariableContext::Kind::Gadt_constructor};
          c.cd = tl;
          check2 = c;
        }
        varis.push_back(compute_variance_gadt(env, check2, required, rloc, decl, tl->cd_loc, tl->cd_args, tl->cd_res));
      }
      if (varis.empty()) throw std::logic_error("compute_variance_decl");
      vari = varis[0];
      for (std::size_t j = 1; j < varis.size(); ++j) {
        if (varis[j].size() != vari.size()) throw std::invalid_argument("List.map2");
        for (std::size_t i = 0; i < vari.size(); ++i) vari[i] = V::union_(vari[i], varis[j][i]);
      }
    }
  } else {  // Type_record
    std::vector<Tyl> all = mn;
    for (auto* ld : decl->type_kind->labels) all.push_back({ld->ld_mutable == MutableFlag::Mutable, ld->ld_type});
    vari = compute_variance_type(env, check, required, rloc, decl, all);
  }
  if (mn.empty() || !abstract) {
    for (auto& v : vari) v = V::strengthen(v);
  }
  return vari;
}

bool is_hash(Ident::t id) {
  std::string_view s = ident::name(id);
  return !s.empty() && s[0] == '#';
}

SurfaceVariance transl_variance(parsetree::Variance v, parsetree::Injectivity i) {
  bool co = false, cn = false;
  switch (v) {
    case parsetree::Variance::Covariant: co = true; break;
    case parsetree::Variance::Contravariant: cn = true; break;
    case parsetree::Variance::NoVariance: break;
    case parsetree::Variance::Bivariant: co = cn = true; break;
  }
  return {co, cn, i == parsetree::Injectivity::Injective};
}

typedecl_properties::Property<Prop, Req> property() {
  typedecl_properties::Property<Prop, Req> p;
  p.eq = [](const Prop& a, const Prop& b) {
    // (List.for_all2 raising Invalid_argument is caught)
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k)
      if (!V::eq(a[k], b[k])) return false;
    return true;
  };
  p.merge = [](const Prop& prop, const Prop& new_prop) {
    if (prop.size() != new_prop.size()) throw std::invalid_argument("List.map2");
    Prop out;
    for (std::size_t k = 0; k < prop.size(); ++k) out.push_back(V::union_(prop[k], new_prop[k]));
    return out;
  };
  p.default_ = [](const TypeDeclaration* decl) { return Prop(decl->type_params.size(), V::null); };
  p.compute = [](env::t env, const TypeDeclaration* decl, const Req& req) {
    return compute_variance_decl(env, nullptr, decl, req, decl->type_loc);
  };
  p.update_decl = [](const TypeDeclaration* decl, const Prop& variance) {
    TypeDeclaration* d = make<TypeDeclaration>(*decl);
    d->type_variance = slice(variance);
    return static_cast<const TypeDeclaration*>(d);
  };
  p.check = [](env::t env, Ident::t id, const TypeDeclaration* decl, const Req& req) {
    if (is_hash(id)) return;
    compute_variance_decl(env, id, decl, req, decl->type_loc);  // check_decl
  };
  return p;
}

}  // namespace

std::vector<SurfaceVariance> variance_of_params(Slice<parsetree::TypeParam> params) {
  std::vector<SurfaceVariance> out;
  for (auto& p : params) out.push_back(transl_variance(p.variance, p.injectivity));
  return out;
}
std::vector<SurfaceVariance> variance_of_sdecl(const parsetree::TypeDeclaration* sdecl) {
  return variance_of_params(sdecl->ptype_params);
}

void check_variance_extension(env::t env, const TypeDeclaration* decl, const tt::TExtensionConstructor* ext,
                              const Req& req, const Location& loc) {
  // TODO: refactorize compute_variance_extension (typedecl_variance.ml)
  VarianceVariableContext check{VarianceVariableContext::Kind::Extension_constructor};
  check.id = ext->ext_id;
  check.ext = ext->ext_type;
  const ExtensionConstructor* e = ext->ext_type;
  TypeDeclaration* d = make<TypeDeclaration>(*decl);
  d->type_params = e->ext_type_params;
  compute_variance_gadt(env, check, req, loc, d, e->ext_loc, e->ext_args, e->ext_ret_type);
}

Prop compute_decl(env::t env, Ident::t check, const TypeDeclaration* decl, const Req& req) {
  return compute_variance_decl(env, check, decl, req, decl->type_loc);
}

std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, Slice<const parsetree::TypeDeclaration*> sdecls,
    const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls) {
  std::vector<Req> required;
  for (auto* s : sdecls) required.push_back(variance_of_sdecl(s));
  return typedecl_properties::compute_property(property(), env, decls, required);
}

std::vector<ClassDeclOutput> update_class_decls(env::t env, const std::vector<ClassDeclInput>& cldecls) {
  typedecl_properties::Decls decls;
  std::vector<Req> required;
  for (auto& c : cldecls) {
    decls.push_back({c.id, c.decl});
    std::vector<SurfaceVariance> r;
    for (auto& p : c.ci_params) r.push_back(transl_variance(p.variance, p.injectivity));
    required.push_back(r);
  }
  decls = typedecl_properties::compute_property(property(), env, decls, required);
  std::vector<ClassDeclOutput> out;
  for (std::size_t k = 0; k < decls.size(); ++k) {
    const TypeDeclaration* decl = decls[k].second;
    Slice<V::t> variance = decl->type_variance;
    ClassDeclaration* clty = make<ClassDeclaration>(*cldecls[k].cl);
    clty->cty_variance = variance;
    ClassTypeDeclaration* cltydef = make<ClassTypeDeclaration>(*cldecls[k].cltype);
    cltydef->clty_variance = variance;
    TypeDeclaration* hash = make<TypeDeclaration>(*cltydef->clty_hash_type);
    hash->type_variance = variance;
    cltydef->clty_hash_type = hash;
    out.push_back({decl, clty, cltydef});
  }
  return out;
}

}  // namespace cppcaml::typing::typedecl_variance
