// Port of typing/ctype.ml, part 6: matching between type schemes (moregen,
// rigidify, matches) and equivalence between parameterized types (eqtype,
// equal).
#include "cppcaml/typing/ctype.hpp"
#include "ctype_internal.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;
using namespace internal;
using DK = DescKind;
using RK = RowFieldView::Kind;
using VK = et::Variant::Kind;

static et::Variant variant(VK k) {
  et::Variant v{};
  v.kind = k;
  return v;
}
static et::Elt<TypeExpr*> incompatible_types_for(std::string_view l) {
  et::Variant v = variant(VK::Incompatible_types_for);
  v.name = l;
  return variant_elt(v);
}
static et::Elt<TypeExpr*> no_tags(et::Position pos, std::vector<RowFieldEntry> tags) {
  et::Variant v = variant(VK::No_tags);
  v.pos = pos;
  v.tags = std::move(tags);
  return variant_elt(v);
}
static et::Elt<TypeExpr*> openness(et::Position pos) {
  et::Variant v = variant(VK::Openness);
  v.pos = pos;
  return variant_elt(v);
}
static et::Elt<TypeExpr*> presence_not_guaranteed_for(et::Position pos, std::string_view l) {
  et::Variant v = variant(VK::Presence_not_guaranteed_for);
  v.pos = pos;
  v.name = l;
  return variant_elt(v);
}
static et::Elt<TypeExpr*> abstract_row(et::Position pos) {
  et::Obj o{};
  o.kind = et::Obj::Kind::Abstract_row;
  o.pos = pos;
  return obj_elt(o);
}
static et::Elt<TypeExpr*> missing_field(et::Position pos, std::string_view n) {
  et::Obj o{};
  o.kind = et::Obj::Kind::Missing_field;
  o.pos = pos;
  o.name = n;
  return obj_elt(o);
}
static et::Elt<TypeExpr*> kind_differ(std::string_view n, FieldKindView k1, FieldKindView k2) {
  et::Obj o{};
  o.kind = et::Obj::Kind::Kind_differ;
  o.name = n;
  o.k1 = k1;
  o.k2 = k2;
  return obj_elt(o);
}
static et::Elt<TypeExpr*> first_class_module(const et::FirstClassModule& fcm) {
  auto x = elt(EK::First_class_module);
  x.fcm = fcm;
  return x;
}

static et::ErrorTrace nonempty(et::ErrorTrace t) {
  if (t.empty()) throw std::logic_error("Errortrace: empty trace");
  return t;
}
static et::MoregenError expand_to_moregen_error(env::t env, const et::TypeTrace& trace) {
  return {nonempty(expand_trace(env, trace))};
}
static et::EqualityError expand_to_equality_error(
    env::t env, const et::TypeTrace& trace,
    const std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst) {
  return {nonempty(expand_trace(env, trace)), subst};
}

// ---- matching between type schemes ------------------------------------------------------

// Level of the subject, should be just below generic_level
static constexpr long subject_level = generic_level - 1;

// Update the level of [ty].  First check that the levels of generic
// variables from the subject are not lowered.
static void moregen_occur(env::t env, long level, TypeExpr* ty) {
  with_type_mark([&](TypeMark& mark) {
    std::function<void(TypeExpr*)> occ = [&](TypeExpr* t) {
      long lv = get_level(t);
      if (lv <= level) return;
      if (is_Tvar(t) && lv >= subject_level) throw Occur{};
      if (try_mark_node(mark, t)) iter_type_expr(occ, t);
    };
    try {
      occ(ty);
    } catch (const Occur&) {
      raise_unexplained_for(TraceExn::Moregen);
    }
  });
  // also check for free univars
  occur_univar_or_unscoped_for(TraceExn::Moregen, env, ty);
  update_level_for(TraceExn::Moregen, env, level, ty);
}

static bool may_instantiate(TypeExpr* t1) { return get_level(t1) != subject_level; }

static void moregen_rec(TypePairs& type_pairs, env::t env, TypeExpr* t1, TypeExpr* t2);
static void moregen_list(TypePairs& tp, env::t env, Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2);
static void moregen_labeled_list(TypePairs& tp, env::t env, Slice<LabeledTy> l1,
                                 Slice<LabeledTy> l2);
static void moregen_package(TypePairs& tp, env::t env, long lvl1, const Package* pack1, long lvl2,
                            const Package* pack2);
static void moregen_fields(TypePairs& tp, env::t env, TypeExpr* ty1, TypeExpr* ty2);
static void moregen_row(TypePairs& tp, env::t env, const RowDesc* row1, const RowDesc* row2);

static void moregen_rec(TypePairs& type_pairs, env::t env, TypeExpr* t1, TypeExpr* t2) {
  if (eq_type(t1, t2)) return;
  try {
    const TypeDesc* d1 = get_desc(t1);
    const TypeDesc* d2 = get_desc(t2);
    auto* c1 = as<Tconstr>(d1);
    auto* c2 = as<Tconstr>(d2);
    if (d1->kind == DK::Tvar && may_instantiate(t1)) {
      moregen_occur(env, get_level(t1), t2);
      update_scope_for(TraceExn::Moregen, get_scope(t1), t2);
      occur_for(TraceExn::Moregen, Uenv::expression(env), t1, t2);
      link_type(t1, t2);
      return;
    }
    if (c1 && c2 && c1->args.empty() && c2->args.empty() &&
        env::path_equiv(env, c1->path, c2->path))
      return;
    TypeExpr* t1p = expand_head(env, t1);
    TypeExpr* t2p = expand_head(env, t2);
    // Expansion may have changed the representative of the types...
    if (eq_type(t1p, t2p)) return;
    if (type_pairs.mem(t1p, t2p)) return;
    type_pairs.add(t1p, t2p);
    const TypeDesc* e1 = get_desc(t1p);
    const TypeDesc* e2 = get_desc(t2p);
    auto* a1 = as<Tarrow>(e1);
    auto* a2 = as<Tarrow>(e2);
    auto* f1 = as<Tfunctor>(e1);
    auto* f2 = as<Tfunctor>(e2);
    auto* k1 = as<Tconstr>(e1);
    auto* k2 = as<Tconstr>(e2);
    if (e1->kind == DK::Tvar && may_instantiate(t1p)) {
      moregen_occur(env, get_level(t1p), t2);
      update_scope_for(TraceExn::Moregen, get_scope(t1p), t2);
      link_type(t1p, t2);
    } else if (a1 && a2) {
      eq_labels(TraceExn::Moregen, false, a1->label, a2->label);
      moregen_rec(type_pairs, env, a1->t1, a2->t1);
      moregen_rec(type_pairs, env, a1->t2, a2->t2);
    } else if (f1 && f2) {
      eq_labels(TraceExn::Moregen, false, f1->label, f2->label);
      moregen_package(type_pairs, env, get_level(t1p), f1->pack, get_level(t2p), f2->pack);
      const ModuleType* mty1 = modtype_of_package(env, location::none(), f1->pack);
      const ModuleType* mty2 = modtype_of_package(env, location::none(), f2->pack);
      enter_functor_with_mtys_for(TraceExn::Moregen, env, f1->id, mty1, t1p, f2->id, mty2, t2p,
                                  [&](env::t new_env) {
                                    moregen_rec(type_pairs, new_env, f1->body, f2->body);
                                  });
    } else if (a1 && f2) {
      eq_labels(TraceExn::Moregen, false, a1->label, f2->label);
      TypeExpr* tt2 = newmono_package(f2->pack);
      moregen_rec(type_pairs, env, a1->t1, tt2);
      const ModuleType* mty = modtype_of_package(env, location::none(), f2->pack);
      env::t env2 =
          env::add_module(Ident::of_unscoped(f2->id), ModulePresence::Mp_present, mty, env);
      identifier_escape_for(TraceExn::Moregen, env2, {f2->id}, f2->body);
      moregen_rec(type_pairs, env, a1->t2, f2->body);
    } else if (f1 && a2) {
      eq_labels(TraceExn::Moregen, false, f1->label, a2->label);
      TypeExpr* tt1 = newmono_package(f1->pack);
      moregen_rec(type_pairs, env, tt1, a2->t1);
      const ModuleType* mty = modtype_of_package(env, location::none(), f1->pack);
      env::t env2 =
          env::add_module(Ident::of_unscoped(f1->id), ModulePresence::Mp_present, mty, env);
      identifier_escape_for(TraceExn::Moregen, env2, {f1->id}, f1->body);
      moregen_rec(type_pairs, env, f1->body, a2->t2);
    } else if (e1->kind == DK::Ttuple && e2->kind == DK::Ttuple) {
      moregen_labeled_list(type_pairs, env, as<Ttuple>(e1)->elems, as<Ttuple>(e2)->elems);
    } else if (k1 && k2 && env::path_equiv(env, k1->path, k2->path)) {
      moregen_list(type_pairs, env, k1->args, k2->args);
    } else if (e1->kind == DK::Tpackage && e2->kind == DK::Tpackage) {
      moregen_package(type_pairs, env, get_level(t1p), as<Tpackage>(e1)->pack, get_level(t2p),
                      as<Tpackage>(e2)->pack);
    } else if (e1->kind == DK::Tnil && k2) {
      raise_for(TraceExn::Moregen, abstract_row(et::Position::Second));
    } else if (k1 && e2->kind == DK::Tnil) {
      raise_for(TraceExn::Moregen, abstract_row(et::Position::First));
    } else if (e1->kind == DK::Tvariant && e2->kind == DK::Tvariant) {
      moregen_row(type_pairs, env, as<Tvariant>(e1)->row, as<Tvariant>(e2)->row);
    } else if (e1->kind == DK::Tobject && e2->kind == DK::Tobject) {
      moregen_fields(type_pairs, env, as<Tobject>(e1)->fields, as<Tobject>(e2)->fields);
    } else if (e1->kind == DK::Tfield && e2->kind == DK::Tfield) {  // Actually unused
      moregen_fields(type_pairs, env, t1p, t2p);
    } else if (e1->kind == DK::Tnil && e2->kind == DK::Tnil) {
    } else if (e1->kind == DK::Tpoly && e2->kind == DK::Tpoly) {
      auto* p1 = as<Tpoly>(e1);
      auto* p2 = as<Tpoly>(e2);
      if (p1->vars.empty() && p2->vars.empty())
        moregen_rec(type_pairs, env, p1->body, p2->body);
      else
        enter_poly_for(TraceExn::Moregen, env, p1->body, p1->vars, p2->body, p2->vars,
                       [&](TypeExpr* a, TypeExpr* b) { moregen_rec(type_pairs, env, a, b); });
    } else if (e1->kind == DK::Tunivar && e2->kind == DK::Tunivar) {
      unify_univar_for(TraceExn::Moregen, t1p, t2p, univar_pairs);
    } else {
      raise_unexplained_for(TraceExn::Moregen);
    }
  } catch (MoregenTrace& e) {
    raise_trace_for(TraceExn::Moregen, cons(diff_elt(t1, t2), std::move(e.trace)));
  }
}

static void moregen_list(TypePairs& tp, env::t env, Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  if (tl1.size() != tl2.size()) raise_unexplained_for(TraceExn::Moregen);
  for (std::size_t k = 0; k < tl1.size(); ++k) moregen_rec(tp, env, tl1[k], tl2[k]);
}

static void moregen_labeled_list(TypePairs& tp, env::t env, Slice<LabeledTy> l1,
                                 Slice<LabeledTy> l2) {
  if (l1.size() != l2.size()) raise_unexplained_for(TraceExn::Moregen);
  for (std::size_t k = 0; k < l1.size(); ++k) {
    if (!(l1[k].label == l2[k].label)) raise_unexplained_for(TraceExn::Moregen);
    moregen_rec(tp, env, l1[k].ty, l2[k].ty);
  }
}

static void moregen_package(TypePairs& tp, env::t env, long lvl1, const Package* pack1, long lvl2,
                            const Package* pack2) {
  PackageSubtypeResult r = compare_package(
      env, [&](TypeExpr* a, TypeExpr* b) { moregen_rec(tp, env, a, b); }, lvl1, pack1, lvl2,
      pack2);
  if (!r.ok) raise_for(TraceExn::Moregen, first_class_module(r.err));
}

static void moregen_kind(std::string_view name, FieldKind* k1, FieldKind* k2) {
  FieldKindView a = field_kind_repr(k1), b = field_kind_repr(k2);
  if ((a == FieldKindView::Fpublic && b == FieldKindView::Fpublic) ||
      (a == FieldKindView::Fprivate && b == FieldKindView::Fprivate))
    return;
  raise_for(TraceExn::Moregen, kind_differ(name, a, b));
}

static void moregen_fields(TypePairs& tp, env::t env, TypeExpr* ty1, TypeExpr* ty2) {
  auto [fields1, rest1] = flatten_fields(ty1);
  auto [fields2, rest2] = flatten_fields(ty2);
  AssociatedFields af = associate_fields(fields1, fields2);
  if (!af.miss1.empty())
    raise_for(TraceExn::Moregen, missing_field(et::Position::Second, af.miss1[0].name));
  moregen_rec(tp, env, rest1, build_fields(get_level(ty2), af.miss2, rest2));
  for (auto& p : af.pairs) {
    moregen_kind(p.name, p.k1, p.k2);
    try {
      moregen_rec(tp, env, p.t1, p.t2);
    } catch (MoregenTrace& e) {
      raise_trace_for(TraceExn::Moregen,
                      cons(incompatible_fields(p.name, p.t1, p.t2), std::move(e.trace)));
    }
  }
}

static void moregen_row(TypePairs& tp, env::t env, const RowDesc* row1, const RowDesc* row2) {
  RowDescRepr r1d = row_repr(row1);
  RowDescRepr r2d = row_repr(row2);
  TypeExpr* rm1 = r1d.more;
  TypeExpr* rm2 = r2d.more;
  if (eq_type(rm1, rm2)) return;
  bool may_inst = (is_Tvar(rm1) && may_instantiate(rm1)) || get_desc(rm1)->kind == DK::Tnil;
  MergedRowFields m = merge_row_fields(r1d.fields, r2d.fields);
  std::vector<RowFieldEntry> r1 = m.r1, r2 = m.r2;
  if (r2d.closed) {
    // a tuple: right to left
    std::vector<RowFieldEntry> nr2 = filter_row_fields(false, r2);
    std::vector<RowFieldEntry> nr1 = filter_row_fields(may_inst, r1);
    r1 = std::move(nr1);
    r2 = std::move(nr2);
  }
  if (!r1.empty()) raise_for(TraceExn::Moregen, no_tags(et::Position::Second, r1));
  if (r1d.closed) {
    if (!r2d.closed) raise_for(TraceExn::Moregen, openness(et::Position::Second));
    if (!r2.empty()) raise_for(TraceExn::Moregen, no_tags(et::Position::First, r2));
  }
  const TypeDesc* md1 = get_desc(rm1);  // This lets us undo a following [link_type]
  const TypeDesc* md2 = get_desc(rm2);
  if (md1->kind == DK::Tunivar && md2->kind == DK::Tunivar) {
    unify_univar_for(TraceExn::Moregen, rm1, rm2, univar_pairs);
  } else if (md1->kind == DK::Tunivar || md2->kind == DK::Tunivar) {
    raise_unexplained_for(TraceExn::Moregen);
  } else if (static_row(row1)) {
  } else if (may_inst) {
    TypeExpr* ext = newgenty(tvariant(create_row(slice(r2), rm2, r2d.closed, r2d.fixed, nullptr)));
    moregen_occur(env, get_level(rm1), ext);
    update_scope_for(TraceExn::Moregen, get_scope(rm1), ext);
    // This [link_type] has to be undone if the rest of the function fails
    link_type(rm1, ext);
  } else if (md1->kind == DK::Tconstr && md2->kind == DK::Tconstr) {
    moregen_rec(tp, env, rm1, rm2);
  } else {
    raise_unexplained_for(TraceExn::Moregen);
  }
  try {
    for (auto& pr : m.pairs) {
      std::string_view l = pr.label;
      const RowField* f1 = pr.f1;
      const RowField* f2 = pr.f2;
      if (f1 == f2) continue;
      RowFieldView v1 = row_field_repr(f1);
      RowFieldView v2 = row_field_repr(f2);
      auto with_tag = [&](auto&& body) {
        try {
          body();
        } catch (MoregenTrace& e) {
          raise_trace_for(TraceExn::Moregen, cons(incompatible_types_for(l), std::move(e.trace)));
        }
      };
      if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent && v1.present && v2.present) {
        // Both matching [Rpresent]s
        with_tag([&] { moregen_rec(tp, env, v1.present, v2.present); });
      } else if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent && !v1.present &&
                 !v2.present) {
      } else if (v1.kind == RK::Reither && v2.kind == RK::Reither) {
        // Both [Reither]
        with_tag([&] {
          if (!eq_row_field_ext(f1, f2)) {
            if (v1.constant && !v2.constant) raise_unexplained_for(TraceExn::Moregen);
            const RowField* f2p = rf_either(f2, v2.constant, {}, v2.matched);
            link_row_field_ext(f1, f2p);
            const auto& tl1 = v1.arg_types;
            const auto& tl2 = v2.arg_types;
            if (tl1.size() == tl2.size()) {
              for (std::size_t k = 0; k < tl1.size(); ++k) moregen_rec(tp, env, tl1[k], tl2[k]);
            } else if (!tl2.empty()) {
              for (TypeExpr* t1 : tl1) moregen_rec(tp, env, t1, tl2[0]);
            } else if (!tl1.empty()) {
              raise_unexplained_for(TraceExn::Moregen);
            }
          }
        });
      } else if (v1.kind == RK::Reither && !v1.constant && v2.kind == RK::Rpresent &&
                 v2.present && may_inst) {
        // Generalizing [Reither]
        with_tag([&] {
          link_row_field_ext(f1, f2);
          for (TypeExpr* t1 : v1.arg_types) moregen_rec(tp, env, t1, v2.present);
        });
      } else if (v1.kind == RK::Reither && v1.constant && v1.arg_types.empty() &&
                 v2.kind == RK::Rpresent && !v2.present && may_inst) {
        link_row_field_ext(f1, f2);
      } else if (v1.kind == RK::Reither && v2.kind == RK::Rabsent && may_inst) {
        link_row_field_ext(f1, f2);
      } else if (v1.kind == RK::Rabsent && v2.kind == RK::Rabsent) {
      } else if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent) {
        // Mismatched constructor arguments
        raise_for(TraceExn::Moregen, incompatible_types_for(l));
      } else if (v1.kind == RK::Reither && v2.kind == RK::Rpresent) {
        // Mismatched presence
        raise_for(TraceExn::Moregen, presence_not_guaranteed_for(et::Position::First, l));
      } else if (v1.kind == RK::Rpresent && v2.kind == RK::Reither) {
        raise_for(TraceExn::Moregen, presence_not_guaranteed_for(et::Position::Second, l));
      } else if (v1.kind == RK::Rabsent) {
        // Missing tags
        raise_for(TraceExn::Moregen, no_tags(et::Position::First, {{l, f2}}));
      } else {
        raise_for(TraceExn::Moregen, no_tags(et::Position::Second, {{l, f1}}));
      }
    }
  } catch (...) {
    // Undo [link_type] if we failed
    set_type_desc(rm1, md1);
    throw;
  }
}

// Must empty univar_pairs first
void moregen(TypePairs& type_pairs, env::t env, TypeExpr* patt, TypeExpr* subj) {
  with_univar_pairs({}, [&] {
    wrap_trace_gadt_instances(env, [&] { moregen_rec(type_pairs, env, patt, subj); });
  });
}

// Non-generic variable can be instantiated only if [inst_nongen] is true
// (see ctype.ml).
void moregeneral(env::t env, TypeExpr* pat_sch, TypeExpr* subj_sch) {
  // Moregen splits the generic level into two finer levels: [generic_level]
  // and [subject_level = generic_level - 1] (see ctype.ml).
  with_level(subject_level - 1, [&] {
    std::optional<et::TypeTrace> err = with_local_level_generalize([&] {
      if (current_level != subject_level) throw std::logic_error("Ctype.moregeneral");
      // Generic variables are first duplicated with [instance]...
      TypeExpr* subj_inst = instance(subj_sch);
      TypeExpr* subj = duplicate_type(subj_inst);
      // Duplicate generic variables
      TypeExpr* patt = generic_instance(pat_sch);
      TypePairs tp;
      try {
        moregen(tp, env, patt, subj);
        return std::optional<et::TypeTrace>{};
      } catch (MoregenTrace& e) {
        return std::optional<et::TypeTrace>{std::move(e.trace)};
      }
    });
    if (err) throw Moregen(expand_to_moregen_error(env, *err));
    return 0;
  });
}

bool is_moregeneral(env::t env, TypeExpr* pat_sch, TypeExpr* subj_sch) {
  try {
    moregeneral(env, pat_sch, subj_sch);
    return true;
  } catch (const Moregen&) {
    return false;
  }
}

// Alternative approach: "rigidify" a type scheme, and check validity after
// unification
static void rigidify_rec(TypeMark& mark, TypeSet& vars, TypeExpr* ty) {
  if (!try_mark_node(mark, ty)) return;
  const TypeDesc* d = get_desc(ty);
  if (d->kind == DK::Tvar) {
    if (!vars.mem(ty)) vars.add(ty);
  } else if (auto* v = as<Tvariant>(d)) {
    const RowDesc* row = v->row;
    RowDescRepr r = row_repr(row);
    TypeExpr* more = r.more;
    if (is_Tvar(more) && !has_fixed_explanation(row)) {
      TypeExpr* more2 = newty2(get_level(more), get_desc(more));
      const RowDesc* row2 = create_row({}, more2, r.closed,
                                       make<FixedExplanation>(FixedExplanation::Kind::Rigid),
                                       r.name);
      link_type(more, newty2(get_level(ty), tvariant(row2)));
    }
    iter_row([&](TypeExpr* t) { rigidify_rec(mark, vars, t); }, row);
    // only consider the row variable if the variant is not static
    if (!static_row(row)) rigidify_rec(mark, vars, row_more(row));
  } else {
    iter_type_expr([&](TypeExpr* t) { rigidify_rec(mark, vars, t); }, ty);
  }
}

std::vector<TypeExpr*> rigidify(TypeExpr* ty) {
  TypeSet vars;
  with_type_mark([&](TypeMark& mark) { rigidify_rec(mark, vars, ty); });
  return vars.elements();
}

bool all_distinct_vars(env::t env, const std::vector<TypeExpr*>& vars) {
  TypeSet tys;
  for (TypeExpr* ty0 : vars) {
    TypeExpr* ty = expand_head(env, ty0);
    if (tys.mem(ty)) return false;
    tys.add(ty);
    if (!is_Tvar(ty)) return false;
  }
  return true;
}

void matches(bool expand_error_trace, env::t env, TypeExpr* ty, TypeExpr* ty2) {
  Snapshot snap = btype::snapshot();
  std::vector<TypeExpr*> vars = rigidify(ty);
  cleanup_abbrev_memo();
  try {
    unify(env, ty, ty2);
  } catch (const Unify& u) {
    btype::backtrack(snap);
    throw MatchesFailure{env, u.err};
  }
  if (!all_distinct_vars(env, vars)) {
    btype::backtrack(snap);
    auto diff = expand_error_trace ? expanded_diff(env, ty, ty2) : unexpanded_diff(ty, ty2);
    throw MatchesFailure{env, {{diff}}};
  }
  btype::backtrack(snap);
}

bool does_match(env::t env, TypeExpr* ty, TypeExpr* ty2) {
  try {
    matches(false, env, ty, ty2);
    return true;
  } catch (const MatchesFailure&) {
    return false;
  }
}

// ---- equivalence between parameterized types ---------------------------------------------

TypeExpr* expand_head_rigid(env::t env, TypeExpr* ty) {
  bool old = rigid_variants;
  rigid_variants = true;
  TypeExpr* ty2 = expand_head_nolink(env, ty);
  rigid_variants = old;
  return ty2;
}

using Subst = std::vector<std::pair<TypeExpr*, TypeExpr*>>;  // head first

static void eqtype_subst(TypePairs& type_pairs, Subst& subst, TypeExpr* t1, TypeExpr* t2) {
  for (auto& [t, tp] : subst) {
    bool found1 = eq_type(t1, t);
    bool found2 = eq_type(t2, tp);
    if (found1 && found2) return;
    if (found1 || found2) raise_unexplained_for(TraceExn::Equality);
  }
  subst.insert(subst.begin(), {t1, t2});
  type_pairs.add(t1, t2);
}

static void eqtype_rec(bool rename, TypePairs& tp, Subst& subst, env::t env, TypeExpr* t1,
                       TypeExpr* t2);
static void eqtype_list_same_length_rec(bool rename, TypePairs& tp, Subst& subst, env::t env,
                                        Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  if (tl1.size() != tl2.size()) throw std::invalid_argument("List.iter2");
  for (std::size_t k = 0; k < tl1.size(); ++k) eqtype_rec(rename, tp, subst, env, tl1[k], tl2[k]);
}
static void eqtype_labeled_list(bool rename, TypePairs& tp, Subst& subst, env::t env,
                                Slice<LabeledTy> l1, Slice<LabeledTy> l2);
static void eqtype_package(bool rename, TypePairs& tp, Subst& subst, env::t env, long lvl1,
                           const Package* pack1, long lvl2, const Package* pack2);
static void eqtype_fields(bool rename, TypePairs& tp, Subst& subst, env::t env, TypeExpr* ty1,
                          TypeExpr* ty2);
static void eqtype_row(bool rename, TypePairs& tp, Subst& subst, env::t env, const RowDesc* row1,
                       const RowDesc* row2);

static void eqtype_rec(bool rename, TypePairs& type_pairs, Subst& subst, env::t env, TypeExpr* t1,
                       TypeExpr* t2) {
  // Checking for physical equality of type representatives when [rename]
  // is true would be incorrect (see ctype.ml).
  auto check_phys_eq = [&](TypeExpr* a, TypeExpr* b) { return !rename && eq_type(a, b); };
  if (check_phys_eq(t1, t2)) return;
  auto rec = [&](env::t e, TypeExpr* a, TypeExpr* b) {
    eqtype_rec(rename, type_pairs, subst, e, a, b);
  };
  try {
    const TypeDesc* d1 = get_desc(t1);
    const TypeDesc* d2 = get_desc(t2);
    auto* c1 = as<Tconstr>(d1);
    auto* c2 = as<Tconstr>(d2);
    if (d1->kind == DK::Tvar && d2->kind == DK::Tvar && rename) {
      eqtype_subst(type_pairs, subst, t1, t2);
      return;
    }
    if (c1 && c2 && c1->args.empty() && c2->args.empty() &&
        env::path_equiv(env, c1->path, c2->path))
      return;
    TypeExpr* t1p = expand_head_rigid(env, t1);
    TypeExpr* t2p = expand_head_rigid(env, t2);
    // Expansion may have changed the representative of the types...
    if (check_phys_eq(t1p, t2p)) return;
    if (type_pairs.mem(t1p, t2p)) return;
    type_pairs.add(t1p, t2p);
    const TypeDesc* e1 = get_desc(t1p);
    const TypeDesc* e2 = get_desc(t2p);
    auto* a1 = as<Tarrow>(e1);
    auto* a2 = as<Tarrow>(e2);
    auto* f1 = as<Tfunctor>(e1);
    auto* f2 = as<Tfunctor>(e2);
    auto* k1 = as<Tconstr>(e1);
    auto* k2 = as<Tconstr>(e2);
    if (e1->kind == DK::Tvar && e2->kind == DK::Tvar && rename) {
      eqtype_subst(type_pairs, subst, t1p, t2p);
    } else if (a1 && a2) {
      eq_labels(TraceExn::Equality, false, a1->label, a2->label);
      rec(env, a1->t1, a2->t1);
      rec(env, a1->t2, a2->t2);
    } else if (f1 && f2) {
      eq_labels(TraceExn::Equality, false, f1->label, f2->label);
      eqtype_package(rename, type_pairs, subst, env, get_level(t1p), f1->pack, get_level(t2p),
                     f2->pack);
      const ModuleType* mty1 = modtype_of_package(env, location::none(), f1->pack);
      const ModuleType* mty2 = modtype_of_package(env, location::none(), f2->pack);
      enter_functor_with_mtys_for(TraceExn::Equality, env, f1->id, mty1, t1p, f2->id, mty2, t2p,
                                  [&](env::t new_env) { rec(new_env, f1->body, f2->body); });
    } else if (f1 && a2) {
      eq_labels(TraceExn::Equality, false, f1->label, a2->label);
      TypeExpr* tt1 = newmono_package(f1->pack);
      rec(env, tt1, a2->t1);
      const ModuleType* mty = modtype_of_package(env, location::none(), f1->pack);
      env::t env2 =
          env::add_module(Ident::of_unscoped(f1->id), ModulePresence::Mp_present, mty, env);
      identifier_escape_for(TraceExn::Equality, env2, {f1->id}, f1->body);
      rec(env, f1->body, a2->t2);
    } else if (a1 && f2) {
      eq_labels(TraceExn::Equality, false, a1->label, f2->label);
      TypeExpr* tt2 = newmono_package(f2->pack);
      rec(env, a1->t1, tt2);
      const ModuleType* mty = modtype_of_package(env, location::none(), f2->pack);
      env::t env2 =
          env::add_module(Ident::of_unscoped(f2->id), ModulePresence::Mp_present, mty, env);
      identifier_escape_for(TraceExn::Equality, env2, {f2->id}, f2->body);
      rec(env, a1->t2, f2->body);
    } else if (e1->kind == DK::Ttuple && e2->kind == DK::Ttuple) {
      eqtype_labeled_list(rename, type_pairs, subst, env, as<Ttuple>(e1)->elems,
                          as<Ttuple>(e2)->elems);
    } else if (k1 && k2 && env::path_equiv(env, k1->path, k2->path)) {
      eqtype_list_same_length_rec(rename, type_pairs, subst, env, k1->args, k2->args);
    } else if (e1->kind == DK::Tpackage && e2->kind == DK::Tpackage) {
      eqtype_package(rename, type_pairs, subst, env, get_level(t1p), as<Tpackage>(e1)->pack,
                     get_level(t2p), as<Tpackage>(e2)->pack);
    } else if (e1->kind == DK::Tnil && k2) {
      raise_for(TraceExn::Equality, abstract_row(et::Position::Second));
    } else if (k1 && e2->kind == DK::Tnil) {
      raise_for(TraceExn::Equality, abstract_row(et::Position::First));
    } else if (e1->kind == DK::Tvariant && e2->kind == DK::Tvariant) {
      eqtype_row(rename, type_pairs, subst, env, as<Tvariant>(e1)->row, as<Tvariant>(e2)->row);
    } else if (e1->kind == DK::Tobject && e2->kind == DK::Tobject) {
      eqtype_fields(rename, type_pairs, subst, env, as<Tobject>(e1)->fields,
                    as<Tobject>(e2)->fields);
    } else if (e1->kind == DK::Tfield && e2->kind == DK::Tfield) {  // Actually unused
      eqtype_fields(rename, type_pairs, subst, env, t1p, t2p);
    } else if (e1->kind == DK::Tnil && e2->kind == DK::Tnil) {
    } else if (e1->kind == DK::Tpoly && e2->kind == DK::Tpoly) {
      auto* p1 = as<Tpoly>(e1);
      auto* p2 = as<Tpoly>(e2);
      if (p1->vars.empty() && p2->vars.empty())
        rec(env, p1->body, p2->body);
      else
        enter_poly_for(TraceExn::Equality, env, p1->body, p1->vars, p2->body, p2->vars,
                       [&](TypeExpr* a, TypeExpr* b) { rec(env, a, b); });
    } else if (e1->kind == DK::Tunivar && e2->kind == DK::Tunivar) {
      unify_univar_for(TraceExn::Equality, t1p, t2p, univar_pairs);
    } else {
      raise_unexplained_for(TraceExn::Equality);
    }
  } catch (EqualityTrace& e) {
    raise_trace_for(TraceExn::Equality, cons(diff_elt(t1, t2), std::move(e.trace)));
  }
}

static void eqtype_labeled_list(bool rename, TypePairs& tp, Subst& subst, env::t env,
                                Slice<LabeledTy> l1, Slice<LabeledTy> l2) {
  if (l1.size() != l2.size()) raise_unexplained_for(TraceExn::Equality);
  for (std::size_t k = 0; k < l1.size(); ++k) {
    if (!(l1[k].label == l2[k].label)) raise_unexplained_for(TraceExn::Equality);
    eqtype_rec(rename, tp, subst, env, l1[k].ty, l2[k].ty);
  }
}

static void eqtype_package(bool rename, TypePairs& tp, Subst& subst, env::t env, long lvl1,
                           const Package* pack1, long lvl2, const Package* pack2) {
  PackageSubtypeResult r = compare_package(
      env, [&](TypeExpr* a, TypeExpr* b) { eqtype_rec(rename, tp, subst, env, a, b); }, lvl1,
      pack1, lvl2, pack2);
  if (!r.ok) raise_for(TraceExn::Equality, first_class_module(r.err));
}

static void eqtype_kind(std::string_view name, FieldKind* k1, FieldKind* k2) {
  FieldKindView a = field_kind_repr(k1), b = field_kind_repr(k2);
  if ((a == FieldKindView::Fprivate && b == FieldKindView::Fprivate) ||
      (a == FieldKindView::Fpublic && b == FieldKindView::Fpublic))
    return;
  raise_for(TraceExn::Equality, kind_differ(name, a, b));
}

static void eqtype_fields(bool rename, TypePairs& tp, Subst& subst, env::t env, TypeExpr* ty1,
                          TypeExpr* ty2) {
  auto [fields1, rest1] = flatten_fields(ty1);
  auto [fields2, rest2] = flatten_fields(ty2);
  // First check if same row => already equal
  bool same_row = (!rename && eq_type(rest1, rest2)) || tp.mem(rest1, rest2);
  if (same_row) return;
  // Try expansion, needed when called from Includecore.type_manifest
  if (auto* o = as<Tobject>(get_desc(expand_head_rigid(env, rest2)))) {
    eqtype_fields(rename, tp, subst, env, ty1, o->fields);
    return;
  }
  AssociatedFields af = associate_fields(fields1, fields2);
  if (!af.miss1.empty())
    raise_for(TraceExn::Equality, missing_field(et::Position::Second, af.miss1[0].name));
  if (!af.miss2.empty())
    raise_for(TraceExn::Equality, missing_field(et::Position::First, af.miss2[0].name));
  eqtype_rec(rename, tp, subst, env, rest1, rest2);
  for (auto& p : af.pairs) {
    eqtype_kind(p.name, p.k1, p.k2);
    try {
      eqtype_rec(rename, tp, subst, env, p.t1, p.t2);
    } catch (EqualityTrace& e) {
      raise_trace_for(TraceExn::Equality,
                      cons(incompatible_fields(p.name, p.t1, p.t2), std::move(e.trace)));
    }
  }
}

static void eqtype_row(bool rename, TypePairs& tp, Subst& subst, env::t env, const RowDesc* row1,
                       const RowDesc* row2) {
  // Try expansion, needed when called from Includecore.type_manifest
  if (auto* v = as<Tvariant>(get_desc(expand_head_rigid(env, row_more(row2))))) {
    eqtype_row(rename, tp, subst, env, row1, v->row);
    return;
  }
  MergedRowFields m = merge_row_fields(row_fields(row1), row_fields(row2));
  bool closed1 = row_closed(row1), closed2 = row_closed(row2);
  if (closed1 != closed2)
    raise_for(TraceExn::Equality,
              openness(closed2 ? et::Position::First : et::Position::Second));
  if (!closed1) {
    if (!m.r1.empty()) raise_for(TraceExn::Equality, no_tags(et::Position::Second, m.r1));
    if (!m.r2.empty()) raise_for(TraceExn::Equality, no_tags(et::Position::First, m.r2));
  }
  if (auto r1 = filter_row_fields(false, m.r1); !r1.empty())
    raise_for(TraceExn::Equality, no_tags(et::Position::Second, r1));
  if (auto r2 = filter_row_fields(false, m.r2); !r2.empty())
    raise_for(TraceExn::Equality, no_tags(et::Position::First, r2));
  if (!static_row(row1)) eqtype_rec(rename, tp, subst, env, row_more(row1), row_more(row2));
  for (auto& pr : m.pairs) {
    std::string_view l = pr.label;
    const RowField* f1 = pr.f1;
    const RowField* f2 = pr.f2;
    if (f1 == f2) continue;
    RowFieldView v1 = row_field_repr(f1);
    RowFieldView v2 = row_field_repr(f2);
    auto with_tag = [&](auto&& body) {
      try {
        body();
      } catch (EqualityTrace& e) {
        raise_trace_for(TraceExn::Equality, cons(incompatible_types_for(l), std::move(e.trace)));
      }
    };
    bool both_either = v1.kind == RK::Reither && v2.kind == RK::Reither;
    if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent && v1.present && v2.present) {
      // Both matching [Rpresent]s
      with_tag([&] { eqtype_rec(rename, tp, subst, env, v1.present, v2.present); });
    } else if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent && !v1.present &&
               !v2.present) {
    } else if (both_either && v1.arg_types.empty() && v2.arg_types.empty() &&
               v1.constant == v2.constant) {
      // Both matching [Reither]s
    } else if (both_either && !v1.arg_types.empty() && !v2.arg_types.empty() &&
               v1.constant == v2.constant) {
      with_tag([&] {
        TypeExpr* t1 = v1.arg_types[0];
        TypeExpr* t2 = v2.arg_types[0];
        eqtype_rec(rename, tp, subst, env, t1, t2);
        std::size_t n1 = v1.arg_types.size() - 1, n2 = v2.arg_types.size() - 1;
        if (n1 == n2) {
          // if same length allow different types (meaning?)
          for (std::size_t k = 0; k < n1; ++k)
            eqtype_rec(rename, tp, subst, env, v1.arg_types[k + 1], v2.arg_types[k + 1]);
        } else {
          // otherwise everything must be equal
          for (std::size_t k = 1; k < v2.arg_types.size(); ++k)
            eqtype_rec(rename, tp, subst, env, t1, v2.arg_types[k]);
          for (std::size_t k = 1; k < v1.arg_types.size(); ++k)
            eqtype_rec(rename, tp, subst, env, v1.arg_types[k], t2);
        }
      });
    } else if (v1.kind == RK::Rabsent && v2.kind == RK::Rabsent) {
    } else if ((v1.kind == RK::Rpresent && v2.kind == RK::Rpresent) || both_either) {
      // Mismatched constructor arguments
      raise_for(TraceExn::Equality, incompatible_types_for(l));
    } else if (v1.kind == RK::Reither && v2.kind == RK::Rpresent) {
      // Mismatched presence
      raise_for(TraceExn::Equality, presence_not_guaranteed_for(et::Position::First, l));
    } else if (v1.kind == RK::Rpresent && v2.kind == RK::Reither) {
      raise_for(TraceExn::Equality, presence_not_guaranteed_for(et::Position::Second, l));
    } else if (v1.kind == RK::Rabsent) {
      // Missing tags
      raise_for(TraceExn::Equality, no_tags(et::Position::First, {{l, f2}}));
    } else {
      raise_for(TraceExn::Equality, no_tags(et::Position::Second, {{l, f1}}));
    }
  }
}

// Must empty univar_pairs first
static void eqtype_list_same_length(bool rename, TypePairs& tp, Subst& subst, env::t env,
                                    Slice<TypeExpr*> tl1, Slice<TypeExpr*> tl2) {
  with_univar_pairs({}, [&] {
    Snapshot snap = btype::snapshot();
    struct Always {
      Snapshot s;
      ~Always() { btype::backtrack(s); }
    } always{snap};
    eqtype_list_same_length_rec(rename, tp, subst, env, tl1, tl2);
  });
}

void eqtype(bool rename, TypePairs& type_pairs, Subst& subst, env::t env, TypeExpr* t1,
            TypeExpr* t2) {
  eqtype_list_same_length(rename, type_pairs, subst, env, slice({t1}), slice({t2}));
}

// Two modes: with or without renaming of variables
void equal(env::t env, bool rename, Slice<TypeExpr*> tyl1, Slice<TypeExpr*> tyl2) {
  if (tyl1.size() != tyl2.size()) raise_unexplained_for(TraceExn::Equality);
  bool all_eq = true;
  for (std::size_t k = 0; k < tyl1.size(); ++k)
    if (!eq_type(tyl1[k], tyl2[k])) {
      all_eq = false;
      break;
    }
  if (all_eq) return;
  Subst subst;
  TypePairs tp;
  try {
    eqtype_list_same_length(rename, tp, subst, env, tyl1, tyl2);
  } catch (EqualityTrace& e) {
    throw Equality(expand_to_equality_error(env, e.trace, subst));
  }
}

bool is_equal(env::t env, bool rename, Slice<TypeExpr*> tyl1, Slice<TypeExpr*> tyl2) {
  try {
    equal(env, rename, tyl1, tyl2);
    return true;
  } catch (const Equality&) {
    return false;
  }
}

void equal_private(env::t env, Slice<TypeExpr*> params1, TypeExpr* ty1, Slice<TypeExpr*> params2,
                   TypeExpr* ty2) {
  std::vector<TypeExpr*> l1(params1.begin(), params1.end()), l2(params2.begin(), params2.end());
  l1.push_back(ty1);
  l2.push_back(ty2);
  TypeExpr* ty1p;
  try {
    equal(env, true, slice(l1), slice(l2));
    return;
  } catch (const Equality& err) {
    try {
      ty1p = try_expand_safe_opt(env, expand_head_nolink(env, ty1));
    } catch (const CannotExpand&) {
      throw err;
    }
  }
  equal_private(env, params1, ty1p, params2, ty2);
}

}  // namespace cppcaml::typing::ctype
