// Port of typing/ctype.ml, part 5: special cases of unification
// (filter_arrow, filter_method, ...) and operations on class signatures.
#include <algorithm>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/predef.hpp"
#include "ctype_internal.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;
using namespace internal;

static et::UnificationError to_unif_error(env::t env, const et::TypeTrace& trace) {
  return expand_to_unification_error(env, trace);
}

void instance_funct_nondep_inplace(env::t env, const Tfunctor_& tfun, const ModuleType* mty) {
  env::t env2 = env::add_module(Ident::of_unscoped(tfun.id_us), ModulePresence::Mp_present, mty, env);
  Snapshot snap = btype::snapshot();
  try {
    identifier_escape_for(TraceExn::Unify, env2, {tfun.id_us}, tfun.ty);
  } catch (const UnifyTrace&) {
    undo_compress(snap);
    throw;
  }
}

TypeExpr* instance_funct_nondep(env::t env, const ArgLabel& l, const Tfunctor_& tfun,
                                const ModuleType* mty) {
  ident::Unscoped* id_us2 = ident::Unscoped::refresh(tfun.id_us);
  TypeExpr* ty = subst_unscoped(tfun.id_us, id_us2, tfun.ty);
  try {
    instance_funct_nondep_inplace(env, {id_us2, tfun.pack, ty}, mty);
    return ty;
  } catch (UnifyTrace& e) {
    TypeExpr* got = newty(tfunctor(l, tfun.id_us, tfun.pack, tfun.ty));
    // Tarrow (l, newmono_package pack, newvar (), commu_ok): right to left
    TypeExpr* v = newvar();
    TypeExpr* mp = newmono_package(tfun.pack);
    TypeExpr* expected = newty(tarrow(l, mp, v, commu_ok()));
    et::TypeTrace trace = std::move(e.trace);
    trace.insert(trace.begin(), diff_elt(got, expected));
    throw Unify(to_unif_error(env, trace));
  }
}

// Unify [t] and [l:'a -> 'b].  Return ['a] and ['b].
static std::tuple<TypeExpr*, TypeExpr*, TypeExpr*> function_type(const ArgLabel& l,
                                                                  bool param_hole, long level) {
  TypeExpr* t1;
  if (param_hole) {
    if (is_optional(l)) throw std::logic_error("Ctype.function_type");
    t1 = newvar2(level);
  } else {
    TypeExpr* inner;
    if (is_optional(l)) {
      TypeExpr* v = newvar2(level);
      inner = newty2(level, tconstr(predef::paths().option, slice({v}), make<MemoRef>(mnil())));
    } else {
      inner = newvar2(level);
    }
    t1 = newty2(level, tpoly(inner, {}));
  }
  TypeExpr* t2 = newvar2(level);
  TypeExpr* t = newty2(level, tarrow(l, t1, t2, commu_ok()));
  return {t, t1, t2};
}

template <class T>
static FResult<T> arrow_unification_error(bool in_apply, env::t env, TypeExpr* t, TypeExpr* t2,
                                          et::TypeTrace trace) {
  auto d = in_apply ? diff_elt(t, t2) : diff_elt(t2, t);
  trace.insert(trace.begin(), d);
  FResult<T> r{false};
  r.error.kind = FilterArrowFailure::Kind::Unification_error;
  r.error.err = to_unif_error(env, trace);
  return r;
}

static FilteredArrow arrow_unify_var(bool param_hole, const ArgLabel& l, TypeExpr* t) {
  auto [t2, ty_param, ty_ret] = function_type(l, param_hole, get_level(t));
  link_type(t, t2);
  return {ty_param, ty_ret};
}

template <class T>
static FResult<T> label_mismatch(const ArgLabel& got, const ArgLabel& expected, TypeExpr* t) {
  FResult<T> r{false};
  r.error.kind = FilterArrowFailure::Kind::Label_mismatch;
  r.error.got = got;
  r.error.expected = expected;
  r.error.expected_type = t;
  return r;
}
template <class T>
static FResult<T> not_a_function() {
  FResult<T> r{false};
  r.error.kind = FilterArrowFailure::Kind::Not_a_function;
  return r;
}

FResult<FilteredArrow> filter_arrow(env::t env, bool in_apply, TypeExpr* t0, const ArgLabel& l,
                                    bool param_hole) {
  TypeExpr* t;
  try {
    t = expand_head_trace(env, t0);
  } catch (UnifyTrace& e) {
    auto [t2, _a, _b] = function_type(l, param_hole, get_level(t0));
    return arrow_unification_error<FilteredArrow>(in_apply, env, t0, t2, std::move(e.trace));
  }
  const TypeDesc* d = get_desc(t);
  auto ok_label = [&](const ArgLabel& l2) {
    return l == l2 || (clflags::classic && l.kind == ArgLabel::Kind::Nolabel && !is_optional(l2));
  };
  if (d->kind == DescKind::Tvar) return {true, arrow_unify_var(param_hole, l, t)};
  if (auto* a = as<Tarrow>(d)) {
    if (ok_label(a->label)) return {true, {a->t1, a->t2}};
    return label_mismatch<FilteredArrow>(l, a->label, t);
  }
  if (auto* fu = as<Tfunctor>(d)) {
    if (!ok_label(fu->label)) return label_mismatch<FilteredArrow>(l, fu->label, t);
    const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
    try {
      instance_funct_nondep_inplace(env, {fu->id, fu->pack, fu->body}, mty);
    } catch (UnifyTrace& e) {
      TypeExpr* pack = newmono_package(fu->pack);
      TypeExpr* v = newvar();
      TypeExpr* t2 = newty(tarrow(l, pack, v, commu_ok()));
      return arrow_unification_error<FilteredArrow>(in_apply, env, t, t2, std::move(e.trace));
    }
    TypeExpr* ty_param = newmono_package(fu->pack, get_level(t));
    TypeExpr* t2 = newty2(get_level(t), tarrow(l, ty_param, fu->body, commu_ok()));
    link_type(t, t2);
    return {true, {ty_param, fu->body}};
  }
  return not_a_function<FilteredArrow>();
}

FResult<std::optional<FunctorView>> filter_functor(env::t env, TypeExpr* t0, const ArgLabel& l) {
  using R = std::optional<FunctorView>;
  TypeExpr* t;
  try {
    t = expand_head_trace(env, t0);
  } catch (UnifyTrace& e) {
    auto [t2, _a, _b] = function_type(l, false, get_level(t0));
    return arrow_unification_error<R>(true, env, t0, t2, std::move(e.trace));
  }
  const TypeDesc* d = get_desc(t);
  if (auto* fu = as<Tfunctor>(d)) {
    if (compatible_labels(false, l, fu->label))
      return {true, FunctorView{fu->id, fu->pack, fu->body}};
    return label_mismatch<R>(l, fu->label, t);
  }
  if (d->kind == DescKind::Tvar) return {true, std::nullopt};
  return not_a_function<R>();
}

FResult<std::pair<env::t, TypeExpr*>> filter_arity(env::t env, TypeExpr* t0, const ArgLabel& l) {
  using R = std::pair<env::t, TypeExpr*>;
  bool param_hole = false;
  TypeExpr* t;
  try {
    t = expand_head_trace(env, t0);
  } catch (UnifyTrace& e) {
    auto [t2, _a, _b] = function_type(l, param_hole, get_level(t0));
    return arrow_unification_error<R>(false, env, t0, t2, std::move(e.trace));
  }
  const TypeDesc* d = get_desc(t);
  if (d->kind == DescKind::Tvar) {
    FilteredArrow ft = arrow_unify_var(param_hole, l, t);
    return {true, {env, ft.ty_ret}};
  }
  if (auto* a = as<Tarrow>(d)) return {true, {env, a->t2}};
  if (auto* fu = as<Tfunctor>(d)) {
    auto [env2, ret] = open_tfunctor(env, location::none(), fu->id, fu->pack, fu->body);
    return {true, {env2, ret}};
  }
  return not_a_function<R>();
}

bool is_really_poly(env::t env, TypeExpr* ty) {
  Snapshot snap = btype::snapshot();
  bool really_poly;
  try {
    unify(env, newmono(newvar()), ty);
    really_poly = false;
  } catch (const Unify&) {
    really_poly = true;
  }
  btype::backtrack(snap);
  return really_poly;
}

// Used by [filter_method].
static TypeExpr* filter_method_field(env::t env, std::string_view name, TypeExpr* ty0) {
  auto method_type = [&](long level) -> std::pair<TypeExpr*, TypeExpr*> {
    // `let ty1 = newvar2 level and ty2 = newvar2 level`: left to right
    TypeExpr* ty1 = newvar2(level);
    TypeExpr* ty2 = newvar2(level);
    return {newty2(level, tfield(name, field_public(), ty1, ty2)), ty1};
  };
  TypeExpr* ty;
  try {
    ty = expand_head_trace(env, ty0);
  } catch (UnifyTrace& e) {
    auto [ty2, _] = method_type(get_level(ty0));
    et::TypeTrace trace = std::move(e.trace);
    trace.insert(trace.begin(), diff_elt(ty0, ty2));
    FilterMethodFailed f(FilterMethodFailed::Kind::Unification_error);
    f.err = to_unif_error(env, trace);
    throw f;
  }
  const TypeDesc* d = get_desc(ty);
  if (d->kind == DescKind::Tvar) {
    auto [ty2, ty1] = method_type(get_level(ty));
    link_type(ty, ty2);
    return ty1;
  }
  if (auto* f = as<Tfield>(d)) {
    if (f->label == name) {
      unify_kind(f->kind_, field_public());
      return f->ty;
    }
    return filter_method_field(env, name, f->rest);
  }
  throw FilterMethodFailed(FilterMethodFailed::Kind::Not_a_method);
}

// Unify [ty] and [< name : 'a; .. >].  Return ['a].
TypeExpr* filter_method(env::t env, std::string_view name, TypeExpr* ty0) {
  auto object_type = [&](long level, long scope) -> std::pair<TypeExpr*, TypeExpr*> {
    TypeExpr* ty1 = newvar2(level);
    TypeExpr* ty2 = newty3(level, scope, tobject(ty1, make<NameRef>(nullptr)));
    TypeExpr* ty_meth = filter_method_field(env, name, ty1);
    return {ty2, ty_meth};
  };
  TypeExpr* ty;
  try {
    ty = expand_head_trace(env, ty0);
  } catch (UnifyTrace& e) {
    auto [ty2, _] = object_type(get_level(ty0), get_scope(ty0));
    et::TypeTrace trace = std::move(e.trace);
    trace.insert(trace.begin(), diff_elt(ty0, ty2));
    FilterMethodFailed f(FilterMethodFailed::Kind::Unification_error);
    f.err = to_unif_error(env, trace);
    throw f;
  }
  const TypeDesc* d = get_desc(ty);
  if (d->kind == DescKind::Tvar) {
    auto [ty2, ty_meth] = object_type(get_level(ty), get_scope(ty));
    link_type(ty, ty2);
    return ty_meth;
  }
  if (auto* o = as<Tobject>(d)) return filter_method_field(env, name, o->fields);
  FilterMethodFailed f(FilterMethodFailed::Kind::Not_an_object);
  f.ty = ty;
  throw f;
}

struct MethodRow {
  FieldKind* kind;
  TypeExpr* field;
  TypeExpr* row;
};

static MethodRow filter_method_row(env::t env, std::string_view name, PrivateFlag priv,
                                   TypeExpr* ty0) {
  TypeExpr* ty = expand_head_unif(env, ty0);
  const TypeDesc* d = get_desc(ty);
  if (d->kind == DescKind::Tvar) {
    long level = get_level(ty);
    TypeExpr* field = newvar2(level);
    TypeExpr* row = newvar2(level);
    FieldKind* kind = priv == PrivateFlag::Private ? field_private() : field_public();
    TypeExpr* ty2 = newty2(level, tfield(name, kind, field, row));
    link_type(ty, ty2);
    return {kind, field, row};
  }
  if (auto* f = as<Tfield>(d)) {
    if (f->label == name) {
      if (priv == PrivateFlag::Public) unify_kind(f->kind_, field_public());
      return {f->kind_, f->ty, f->rest};
    }
    long level = get_level(ty);
    MethodRow r = filter_method_row(env, name, priv, f->rest);
    TypeExpr* row = newty2(level, tfield(f->label, f->kind_, f->ty, r.row));
    return {r.kind, r.field, row};
  }
  if (d->kind == DescKind::Tnil) {
    if (name == dummy_method) throw FilterMethodRowFailed{};
    if (priv == PrivateFlag::Public) throw FilterMethodRowFailed{};
    long level = get_level(ty);
    return {field_absent(), newvar2(level), ty};
  }
  throw FilterMethodRowFailed{};
}

// ---- operations on class signatures -------------------------------------------------
ClassSignature* new_class_signature() {
  TypeExpr* row = newvar();
  TypeExpr* self = newobj(row);
  return make<ClassSignature>(self, row, field_absent(), StrMap<VarEntry>{}, StrMap<MethEntry>{});
}

void add_dummy_method(env::t env, long scope, ClassSignature* sign) {
  if (field_kind_repr(sign->csig_dummy_method) != FieldKindView::Fabsent)
    throw std::logic_error("Ctype.add_dummy_method");
  MethodRow r = filter_method_row(env, dummy_method, PrivateFlag::Private, sign->csig_self_row);
  unify(env, r.field, new_scoped_ty(scope, ttuple({})));
  sign->csig_dummy_method = r.kind;
  sign->csig_self_row = r.row;
}

void remove_dummy_method(ClassSignature* sign) {
  if (field_kind_repr(sign->csig_dummy_method) != FieldKindView::Fprivate)
    throw std::logic_error("Ctype.remove_dummy_method");
  link_kind(sign->csig_dummy_method, field_absent());
}

void add_method(env::t env, std::string_view label, PrivateFlag priv, VirtualFlag virt,
                TypeExpr* ty, ClassSignature* sign) {
  StrMap<MethEntry> meths = sign->csig_meths;
  MethodPrivacy priv2;
  VirtualFlag virt2;
  if (const MethEntry* e = meths.find_opt(label)) {
    if (!e->priv.is_private) {
      priv2 = MethodPrivacy{};
    } else if (priv == PrivateFlag::Public) {
      switch (field_kind_repr(e->priv.kind)) {
        case FieldKindView::Fpublic: break;
        case FieldKindView::Fprivate: link_kind(e->priv.kind, field_public()); break;
        case FieldKindView::Fabsent: throw AddMethodFailed(true);
      }
      priv2 = MethodPrivacy{};
    } else {
      priv2 = e->priv;
    }
    virt2 = e->virt == VirtualFlag::Concrete ? VirtualFlag::Concrete : virt;
    try {
      unify(env, ty, e->ty);
    } catch (const Unify& u) {
      AddMethodFailed f(false);
      f.err = u.err;
      throw f;
    }
  } else {
    MethodRow r;
    try {
      r = filter_method_row(env, label, priv, sign->csig_self_row);
    } catch (const FilterMethodRowFailed&) {
      throw AddMethodFailed(true);
    }
    priv2 = priv == PrivateFlag::Public ? MethodPrivacy{} : MethodPrivacy{true, r.kind};
    try {
      unify(env, ty, r.field);
    } catch (const Unify& u) {
      AddMethodFailed f(false);
      f.err = u.err;
      throw f;
    }
    sign->csig_self_row = r.row;
    virt2 = virt;
  }
  sign->csig_meths = meths.add(zborrow(label), MethEntry{priv2, virt2, ty});
}

void add_instance_variable(bool strict, env::t env, std::string_view label, MutableFlag mut,
                           VirtualFlag virt, TypeExpr* ty, ClassSignature* sign) {
  StrMap<VarEntry> vars = sign->csig_vars;
  VirtualFlag virt2 = virt;
  if (const VarEntry* e = vars.find_opt(label)) {
    virt2 = e->virt == VirtualFlag::Concrete ? VirtualFlag::Concrete : virt;
    if (strict) {
      if (mut != e->mut) {
        AddInstanceVariableFailed f(true);
        f.mut = mut;
        throw f;
      }
      try {
        unify(env, ty, e->ty);
      } catch (const Unify& u) {
        AddInstanceVariableFailed f(false);
        f.err = u.err;
        throw f;
      }
    }
  }
  sign->csig_vars = vars.add(zborrow(label), VarEntry{mut, virt2, ty});
}

static void unify_self_types(env::t env, const ClassSignature* sign1, const ClassSignature* sign2) {
  try {
    unify(env, sign1->csig_self, sign2->csig_self);
  } catch (const Unify& u) {
    const auto& tr = u.err.trace;
    using K2 = et::Elt<et::ExpandedType>::Kind;
    if (tr.size() >= 2 && tr[0].kind == K2::Diff && tr[1].kind == K2::Incompatible_fields) {
      InheritClassSignatureFailed f(InheritClassSignatureFailed::Kind::Method);
      f.label = tr[1].field_name;
      AddMethodFailed am(false);
      am.err.trace.assign(tr.begin() + 2, tr.end());
      if (am.err.trace.empty()) throw std::logic_error("Errortrace.unification_error");
      f.method = am;
      throw f;
    }
    InheritClassSignatureFailed f(InheritClassSignatureFailed::Kind::Self_type_mismatch);
    f.err = u.err;
    throw f;
  }
}

// Unify components of sign2 into sign1
void inherit_class_signature(bool strict, env::t env, ClassSignature* sign1,
                             const ClassSignature* sign2) {
  unify_self_types(env, sign1, sign2);
  sign2->csig_meths.iter([&](std::string_view label, const MethEntry& e) {
    PrivateFlag priv = e.priv.is_private ? PrivateFlag::Private : PrivateFlag::Public;
    try {
      add_method(env, label, priv, e.virt, e.ty, sign1);
    } catch (const AddMethodFailed& am) {
      InheritClassSignatureFailed f(InheritClassSignatureFailed::Kind::Method);
      f.label = label;
      f.method = am;
      throw f;
    }
  });
  sign2->csig_vars.iter([&](std::string_view label, const VarEntry& e) {
    try {
      add_instance_variable(strict, env, label, e.mut, e.virt, e.ty, sign1);
    } catch (const AddInstanceVariableFailed& iv) {
      InheritClassSignatureFailed f(InheritClassSignatureFailed::Kind::Instance_variable);
      f.label = label;
      f.ivar = iv;
      throw f;
    }
  });
}

std::vector<std::string_view> update_implicitly_public_methods(ClassSignature* sign) {
  StrMap<MethEntry> meths = sign->csig_meths;
  std::vector<std::string_view> implicitly_public;  // consed
  sign->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
    if (!e.priv.is_private) return;
    if (field_kind_repr(e.priv.kind) == FieldKindView::Fpublic) {
      meths = meths.add(lab, MethEntry{MethodPrivacy{}, e.virt, e.ty});
      implicitly_public.insert(implicitly_public.begin(), lab);
    }
  });
  sign->csig_meths = meths;
  return implicitly_public;
}

std::vector<std::string_view> update_implicitly_declared_methods(env::t env, ClassSignature* sign) {
  TypeExpr* row0 = expand_head(env, sign->csig_self_row);
  auto [fields, row] = flatten_fields(row0);
  long row_level = get_level(row);
  StrMap<MethEntry> meths = sign->csig_meths;
  std::vector<std::string_view> implicitly_declared;
  for (auto& f : fields) {
    switch (field_kind_repr(f.kind)) {
      case FieldKindView::Fabsent: throw std::logic_error("Ctype.update_implicitly_declared_methods");
      case FieldKindView::Fprivate:
        row = newty2(row_level, tfield(f.name, f.kind, f.ty, row));
        break;
      case FieldKindView::Fpublic:
        meths = meths.add(f.name, MethEntry{MethodPrivacy{}, VirtualFlag::Virtual, f.ty});
        implicitly_declared.insert(implicitly_declared.begin(), f.name);
        break;
    }
  }
  sign->csig_meths = meths;
  sign->csig_self_row = row;
  return implicitly_declared;
}

void hide_private_methods(const ClassSignature* sign) {
  sign->csig_meths.iter([&](std::string_view, const MethEntry& e) {
    if (!e.priv.is_private) return;
    switch (field_kind_repr(e.priv.kind)) {
      case FieldKindView::Fpublic: throw std::logic_error("Ctype.hide_private_methods");
      case FieldKindView::Fabsent: return;
      case FieldKindView::Fprivate: link_kind(e.priv.kind, field_absent()); return;
    }
  });
}

void reveal_private_methods(env::t env, ClassSignature* sign) {
  StrMap<MethEntry> meths = sign->csig_meths;
  TypeExpr* row = sign->csig_self_row;
  sign->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
    if (!e.priv.is_private) return;
    if (field_kind_repr(e.priv.kind) != FieldKindView::Fabsent)
      throw std::logic_error("Ctype.reveal_private_methods");
    MethodRow r = filter_method_row(env, lab, PrivateFlag::Private, row);
    unify(env, e.ty, r.field);
    meths = meths.add(lab, MethEntry{MethodPrivacy{true, r.kind}, e.virt, e.ty});
    row = r.row;
  });
  sign->csig_meths = meths;
  sign->csig_self_row = row;
}

bool close_class_signature(env::t env, ClassSignature* sign) {
  std::function<bool(TypeExpr*)> close = [&](TypeExpr* ty0) -> bool {
    TypeExpr* ty = expand_head(env, ty0);
    const TypeDesc* d = get_desc(ty);
    if (d->kind == DescKind::Tvar) {
      link_type(ty, newty2(get_level(ty), tnil()));
      return true;
    }
    if (auto* f = as<Tfield>(d)) {
      if (f->label == dummy_method) return false;
      switch (field_kind_repr(f->kind_)) {
        case FieldKindView::Fabsent: throw std::logic_error("Ctype.close_class_signature");
        case FieldKindView::Fpublic: return false;
        case FieldKindView::Fprivate:
          link_kind(f->kind_, field_absent());
          return close(f->rest);
      }
    }
    if (d->kind == DescKind::Tnil) return true;
    throw std::logic_error("Ctype.close_class_signature");
  };
  return close(expand_head(env, sign->csig_self_row));
}

// Build a copy of a type in which nodes reachable through a path composed
// only of Tarrow, Tpoly, Ttuple, Tpackage and Tconstr, and whose level was no
// lower than current_level, are at generic_level (see ctype.ml).
static TypeExpr* copy_spine_rec(const UnscopedMapping& unscoped, CopyScope& copy_scope,
                                TypeExpr* ty) {
  const TypeDesc* desc = get_desc(ty);
  switch (desc->kind) {
    case DescKind::Tsubst: return as<Tsubst>(desc)->ty;
    case DescKind::Tvar: case DescKind::Tnil: case DescKind::Tlink: case DescKind::Tunivar:
    case DescKind::Texpand:
      return ty;
    case DescKind::Tfield: case DescKind::Tvariant: case DescKind::Tobject:
      // We left the spine but still need to apply id_map.
      if (unscoped.closed(ty)) return ty;
      return copy(copy_scope, ty, nullptr, false, std::nullopt, &unscoped);
    default:
      break;
  }
  long level = get_level(ty);
  if (unscoped.closed(ty) && (level < current_level || level == generic_level)) return ty;
  TypeExpr* t = newgenstub(get_scope(ty));
  redirect_desc(copy_scope, ty, btype::scoped_tsubst(t, nullptr));
  auto copy_rec = [&](TypeExpr* x) { return copy_spine_rec(unscoped, copy_scope, x); };
  auto psubst = [&](Path::t p) { return path::subst(unscoped.map, p); };
  const TypeDesc* desc2;
  switch (desc->kind) {
    case DescKind::Tarrow: {
      auto* a = as<Tarrow>(desc);
      TypeExpr* t2 = copy_rec(a->t2);  // right to left
      TypeExpr* t1 = copy_rec(a->t1);
      desc2 = tarrow(a->label, t1, t2, commu_ok());
      break;
    }
    case DescKind::Tpoly: {
      auto* p = as<Tpoly>(desc);
      desc2 = tpoly(copy_rec(p->body), p->vars);
      break;
    }
    case DescKind::Ttuple: {
      std::vector<LabeledTy> l;
      for (auto& e : as<Ttuple>(desc)->elems) l.push_back({e.label, copy_rec(e.ty)});
      desc2 = ttuple(slice(l));
      break;
    }
    case DescKind::Tpackage:
      desc2 = tpackage(map_pack(psubst, copy_rec, as<Tpackage>(desc)->pack));
      break;
    case DescKind::Tconstr: {
      auto* c = as<Tconstr>(desc);
      std::vector<TypeExpr*> tl;
      for (TypeExpr* a : c->args) tl.push_back(copy_rec(a));
      desc2 = tconstr(psubst(c->path), slice(tl), make<MemoRef>(mnil()));
      break;
    }
    case DescKind::Tfunctor: {
      auto* fu = as<Tfunctor>(desc);
      const Package* pack2 = map_pack(psubst, copy_rec, fu->pack);
      ident::Unscoped* us2 = ident::Unscoped::refresh(fu->id);
      UnscopedMapping nm = compute_new_closed(fu->id, us2, unscoped.map, fu->body);
      TypeExpr* ty2 = copy_spine_rec(nm, copy_scope, fu->body);
      desc2 = tfunctor(fu->label, us2, pack2, ty2);
      break;
    }
    default:
      throw std::logic_error("Ctype.copy_spine");
  }
  transient_expr::set_stub_desc(t, desc2);
  return t;
}

TypeExpr* copy_spine(TypeExpr* ty) {
  TypeExpr* r = nullptr;
  UnscopedMapping u = empty_unscoped_mapping();
  with_copy_scope([&](CopyScope& cs) { r = copy_spine_rec(u, cs, ty); });
  return r;
}

void generalize_class_signature_spine(ClassSignature* sign) {
  // Generalize the spine of methods
  sign->csig_meths =
      sign->csig_meths.map([](const MethEntry& e) { return MethEntry{e.priv, e.virt, copy_spine(e.ty)}; });
}

}  // namespace cppcaml::typing::ctype
