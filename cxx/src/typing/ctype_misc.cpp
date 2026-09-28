// Port of typing/ctype.ml, part 7: class type matching, subtyping
// (build_subtype / subtype), miscellaneous (unalias, nongen_vars,
// normalize_type, arrow_spine), removal of dependencies (nondep_*),
// collapse_conj_params and immediacy.
#include <algorithm>

#include "cppcaml/typing/ctype.hpp"
#include "ctype_internal.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;
using namespace internal;
using DK = DescKind;
using RK = RowFieldView::Kind;

static constexpr long subject_level = generic_level - 1;

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

// ---- class type matching ------------------------------------------------------------------
using CMK = ClassMatchFailure::Kind;

static ClassMatchFailure cm(CMK k, std::string_view lab = {}) {
  ClassMatchFailure f{};
  f.kind = k;
  f.label = lab;
  return f;
}
static ClassMatchFailure cm_class_type_mismatch(env::t env, const ClassType* c1,
                                                const ClassType* c2) {
  ClassMatchFailure f = cm(CMK::CM_Class_type_mismatch);
  f.env = env;
  f.cty1 = c1;
  f.cty2 = c2;
  return f;
}
static std::vector<ClassMatchFailure> cons_cm(ClassMatchFailure x, std::vector<ClassMatchFailure> l) {
  l.insert(l.begin(), std::move(x));
  return l;
}

static std::vector<ClassMatchFailure> match_class_sig_shape(bool strict, const ClassSignature* sign1,
                                                            const ClassSignature* sign2) {
  std::vector<ClassMatchFailure> err;  // head first
  auto push = [&](ClassMatchFailure f) { err.insert(err.begin(), std::move(f)); };
  sign2->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
    const MethEntry* e1 = sign1->csig_meths.find_opt(lab);
    if (!e1) {
      push(cm(CMK::CM_Missing_method, lab));
      return;
    }
    if (!e1->priv.is_private && e.priv.is_private) {
      push(cm(CMK::CM_Public_method, lab));
    } else if (e1->priv.is_private && !e.priv.is_private && strict) {
      push(cm(CMK::CM_Private_method, lab));
    } else if (e1->virt == VirtualFlag::Virtual && e.virt == VirtualFlag::Concrete) {
      push(cm(CMK::CM_Virtual_method, lab));
    }
  });
  sign1->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
    if (sign2->csig_meths.mem(lab)) return;
    if (!e.priv.is_private) push(cm(CMK::CM_Hide_public, lab));
    if (e.virt == VirtualFlag::Virtual) {
      ClassMatchFailure f = cm(CMK::CM_Hide_virtual, lab);
      f.kind_name = "method";
      push(f);
    }
  });
  sign2->csig_vars.iter([&](std::string_view lab, const VarEntry& e) {
    const VarEntry* e1 = sign1->csig_vars.find_opt(lab);
    if (!e1) {
      push(cm(CMK::CM_Missing_value, lab));
      return;
    }
    if (e1->mut == MutableFlag::Immutable && e.mut == MutableFlag::Mutable)
      push(cm(CMK::CM_Non_mutable_value, lab));
    else if (e1->virt == VirtualFlag::Virtual && e.virt == VirtualFlag::Concrete)
      push(cm(CMK::CM_Non_concrete_value, lab));
  });
  sign1->csig_vars.iter([&](std::string_view lab, const VarEntry& e) {
    if (e.virt == VirtualFlag::Virtual && !sign2->csig_vars.mem(lab)) {
      ClassMatchFailure f = cm(CMK::CM_Hide_virtual, lab);
      f.kind_name = "instance variable";
      push(f);
    }
  });
  return err;
}

// [arrow_index] is the number of [Cty_arrow] constructors we've seen so far.
static void moregen_clty(long arrow_index, bool trace, TypePairs& type_pairs, env::t env,
                         const ClassType* cty1, const ClassType* cty2) {
  using CK = ClassType::Kind;
  try {
    if (cty1->kind == CK::Cty_constr) {
      moregen_clty(arrow_index, true, type_pairs, env, cty1->cty, cty2);
    } else if (cty2->kind == CK::Cty_constr) {
      moregen_clty(arrow_index, true, type_pairs, env, cty1, cty2->cty);
    } else if (cty1->kind == CK::Cty_arrow && cty2->kind == CK::Cty_arrow &&
               cty1->label == cty2->label) {
      long ai = arrow_index + 1;
      try {
        moregen(type_pairs, env, cty1->arg, cty2->arg);
      } catch (MoregenTrace& e) {
        ClassMatchFailure f = cm(CMK::CM_Parameter_mismatch);
        f.index = ai;
        f.env = env;
        f.moregen = expand_to_moregen_error(env, e.trace);
        throw ClassMatchFailures{{f}};
      }
      moregen_clty(ai, false, type_pairs, env, cty1->cty, cty2->cty);
    } else if (cty1->kind == CK::Cty_signature && cty2->kind == CK::Cty_signature) {
      const ClassSignature* sign1 = cty1->sign;
      const ClassSignature* sign2 = cty2->sign;
      auto mismatch = [&](CMK k, std::string_view lab, const et::TypeTrace& tr) {
        ClassMatchFailure f = cm(k, lab);
        f.env = env;
        f.comparison.is_equality = false;
        f.comparison.moregen = expand_to_moregen_error(env, tr);
        throw ClassMatchFailures{{f}};
      };
      sign2->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
        const MethEntry* e1 = sign1->csig_meths.find_opt(lab);
        // This function is only called after checking that all methods in
        // sign2 are present in sign1.
        if (!e1) throw std::logic_error("Ctype.moregen_clty");
        try {
          moregen(type_pairs, env, e1->ty, e.ty);
        } catch (MoregenTrace& t) {
          mismatch(CMK::CM_Meth_type_mismatch, lab, t.trace);
        }
      });
      sign2->csig_vars.iter([&](std::string_view lab, const VarEntry& e) {
        const VarEntry* e1 = sign1->csig_vars.find_opt(lab);
        if (!e1) throw std::logic_error("Ctype.moregen_clty");
        try {
          moregen(type_pairs, env, e1->ty, e.ty);
        } catch (MoregenTrace& t) {
          mismatch(CMK::CM_Val_type_mismatch, lab, t.trace);
        }
      });
    } else {
      throw ClassMatchFailures{{}};
    }
  } catch (ClassMatchFailures& f) {
    if (trace || f.errors.empty())
      throw ClassMatchFailures{cons_cm(cm_class_type_mismatch(env, cty1, cty2), std::move(f.errors))};
    throw;
  }
}

std::vector<ClassMatchFailure> match_class_types(env::t env, const ClassType* pat_sch,
                                                 const ClassType* subj_sch, bool trace) {
  ClassSignature* sign1 = signature_of_class_type(pat_sch);
  ClassSignature* sign2 = signature_of_class_type(subj_sch);
  std::vector<ClassMatchFailure> errors = match_class_sig_shape(false, sign1, sign2);
  if (!errors.empty())
    return cons_cm(cm_class_type_mismatch(env, pat_sch, subj_sch), std::move(errors));
  // Moregen splits the generic level into two finer levels (see ctype.ml).
  return with_level(subject_level - 1, [&] {
    return with_local_level_generalize([&] {
      if (current_level != subject_level) throw std::logic_error("Ctype.match_class_types");
      const ClassType* subj_inst = instance_class({}, subj_sch).second;
      const ClassType* subj = duplicate_class_type(subj_inst);
      // Duplicate generic variables
      const ClassType* patt =
          with_level(generic_level, [&] { return instance_class({}, pat_sch); }).second;
      TypePairs type_pairs;
      ClassSignature* s1 = signature_of_class_type(patt);
      ClassSignature* s2 = signature_of_class_type(subj);
      type_pairs.add(s1->csig_self, s2->csig_self);
      // Always succeeds
      moregen(type_pairs, env, s1->csig_self_row, s2->csig_self_row);
      // May fail
      try {
        moregen_clty(0, trace, type_pairs, env, patt, subj);
        return std::vector<ClassMatchFailure>{};
      } catch (ClassMatchFailures& f) {
        return std::move(f.errors);
      }
    });
  });
}

using Subst = std::vector<std::pair<TypeExpr*, TypeExpr*>>;

static void equal_clsig(bool trace, TypePairs& type_pairs, Subst& subst, env::t env,
                        ClassSignature* sign1, ClassSignature* sign2) {
  try {
    auto mismatch = [&](CMK k, std::string_view lab, const et::TypeTrace& tr) {
      ClassMatchFailure f = cm(k, lab);
      f.env = env;
      f.comparison.is_equality = true;
      f.comparison.equality = expand_to_equality_error(env, tr, subst);
      throw ClassMatchFailures{{f}};
    };
    sign2->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
      const MethEntry* e1 = sign1->csig_meths.find_opt(lab);
      if (!e1) throw std::logic_error("Ctype.equal_clsig");
      try {
        eqtype(true, type_pairs, subst, env, e1->ty, e.ty);
      } catch (EqualityTrace& t) {
        mismatch(CMK::CM_Meth_type_mismatch, lab, t.trace);
      }
    });
    sign2->csig_vars.iter([&](std::string_view lab, const VarEntry& e) {
      const VarEntry* e1 = sign1->csig_vars.find_opt(lab);
      if (!e1) throw std::logic_error("Ctype.equal_clsig");
      try {
        eqtype(true, type_pairs, subst, env, e1->ty, e.ty);
      } catch (EqualityTrace& t) {
        mismatch(CMK::CM_Val_type_mismatch, lab, t.trace);
      }
    });
  } catch (ClassMatchFailures& f) {
    if (!trace) throw;
    auto mk_sig = [](ClassSignature* s) {
      return make<ClassType>(ClassType::Kind::Cty_signature, nullptr, Slice<TypeExpr*>{}, nullptr, s);
    };
    throw ClassMatchFailures{
        cons_cm(cm_class_type_mismatch(env, mk_sig(sign1), mk_sig(sign2)), std::move(f.errors))};
  }
}

std::vector<ClassMatchFailure> match_class_declarations(env::t env, Slice<TypeExpr*> patt_params,
                                                        const ClassType* patt_type,
                                                        Slice<TypeExpr*> subj_params,
                                                        const ClassType* subj_type) {
  ClassSignature* sign1 = signature_of_class_type(patt_type);
  ClassSignature* sign2 = signature_of_class_type(subj_type);
  std::vector<ClassMatchFailure> errors = match_class_sig_shape(true, sign1, sign2);
  if (!errors.empty()) return errors;
  try {
    Subst subst;
    TypePairs type_pairs;
    type_pairs.add(sign1->csig_self, sign2->csig_self);
    // Always succeeds
    eqtype(true, type_pairs, subst, env, sign1->csig_self_row, sign2->csig_self_row);
    long lp = static_cast<long>(patt_params.size());
    long ls = static_cast<long>(subj_params.size());
    if (lp != ls) {
      ClassMatchFailure f = cm(CMK::CM_Parameter_arity_mismatch);
      f.index = lp;
      f.index2 = ls;
      throw ClassMatchFailures{{f}};
    }
    for (std::size_t n = 0; n < patt_params.size(); ++n) {
      try {
        eqtype(true, type_pairs, subst, env, patt_params[n], subj_params[n]);
      } catch (EqualityTrace& t) {
        ClassMatchFailure f = cm(CMK::CM_Type_parameter_mismatch);
        f.index = static_cast<long>(n) + 1;
        f.env = env;
        f.equality = expand_to_equality_error(env, t.trace, subst);
        throw ClassMatchFailures{{f}};
      }
    }
    equal_clsig(false, type_pairs, subst, env, sign1, sign2);
    // Use moregeneral for class parameters, need to recheck everything to
    // keeps relationships (PR#4824)
    auto clty_params = [](Slice<TypeExpr*> params, const ClassType* cty) {
      for (std::size_t k = params.size(); k-- > 0;) {
        cty = make<ClassType>(ClassType::Kind::Cty_arrow, nullptr, Slice<TypeExpr*>{}, cty, nullptr,
                              ArgLabel::labelled("*"), params[k]);
      }
      return cty;
    };
    return match_class_types(env, clty_params(patt_params, patt_type),
                             clty_params(subj_params, subj_type), false);
  } catch (ClassMatchFailures& f) {
    return std::move(f.errors);
  }
}

// ---- subtyping: build a subtype of a given type ----------------------------------------------

static bool warn = false;  // whether double coercion might do better
static long pred_expand(long n) { return (n % 2 == 0 && n > 0) ? n - 1 : n; }
static long pred_enlarge(long n) { return (n % 2 == 1) ? n - 1 : n; }

enum class Change { Unchanged, Equiv, Changed };
static Change max_change(Change c1, Change c2) {
  if (c1 == Change::Changed || c2 == Change::Changed) return Change::Changed;
  if (c1 == Change::Equiv || c2 == Change::Equiv) return Change::Equiv;
  return Change::Unchanged;
}

using Visited = std::vector<TypeExpr*>;  // transient exprs, head first
using Loops = std::vector<std::pair<long, TypeExpr*>>;  // head first

static Visited filter_visited(const Visited& l) {
  for (std::size_t k = 0; k < l.size(); ++k) {
    DK d = l[k]->desc->kind;
    if (d == DK::Tobject || d == DK::Tvariant) return Visited(l.begin() + k, l.end());
  }
  return {};
}
static Visited cons_v(TypeExpr* t, const Visited& l) {
  Visited r;
  r.reserve(l.size() + 1);
  r.push_back(t);
  r.insert(r.end(), l.begin(), l.end());
  return r;
}

static bool memq_warn(TypeExpr* t, const Visited& visited) {
  if (std::find(visited.begin(), visited.end(), t) != visited.end()) {
    warn = true;
    return true;
  }
  return false;
}

std::pair<const TypeDeclaration*, TypeExpr*> find_cltype_for_path(env::t env, Path::t p) {
  const TypeDeclaration* cl_abbr = env::find_hash_type(p, env);
  TypeExpr* ty = cl_abbr->type_manifest;
  if (!ty) throw std::logic_error("Ctype.find_cltype_for_path");
  auto* o = as<Tobject>(get_desc(ty));
  if (o && o->name->contents && path::same(p, o->name->contents->path)) return {cl_abbr, ty};
  throw env::NotFound{};
}

static bool has_constr_row2(env::t env, TypeExpr* t) {
  return has_constr_row(expand_abbrev(false, env, t));
}

static std::pair<TypeExpr*, Change> build_subtype(env::t env, const Visited& visited,
                                                  const Loops& loops, bool posi, long level,
                                                  TypeExpr* t) {
  const TypeDesc* d = get_desc(t);
  using U = Change;
  switch (d->kind) {
    case DK::Tvar: {
      if (posi) {
        for (auto& [id, t2] : loops)
          if (id == get_id(t)) {
            warn = true;
            return {t2, U::Equiv};
          }
      }
      return {t, U::Unchanged};
    }
    case DK::Tarrow: {
      auto* a = as<Tarrow>(d);
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited)) return {t, U::Unchanged};
      Visited v2 = cons_v(tt, visited);
      auto [t1p, c1] = build_subtype(env, v2, loops, !posi, level, a->t1);
      auto [t2p, c2] = build_subtype(env, v2, loops, posi, level, a->t2);
      Change c = max_change(c1, c2);
      if (c > U::Unchanged) return {newty(tarrow(a->label, t1p, t2p, commu_ok())), c};
      return {t, U::Unchanged};
    }
    case DK::Tfunctor: {
      auto* fu = as<Tfunctor>(d);
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited)) return {t, U::Unchanged};
      Visited v2 = cons_v(tt, visited);
      const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
      env::t env2 = env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
      auto [ty, c] = build_subtype(env2, v2, loops, posi, level, fu->body);
      if (c > U::Unchanged) {
        ident::Unscoped* us2 = ident::Unscoped::refresh(fu->id);
        TypeExpr* ty2 = subst_unscoped(fu->id, us2, ty);
        return {newty(tfunctor(fu->label, us2, fu->pack, ty2)), c};
      }
      return {t, U::Unchanged};
    }
    case DK::Ttuple: {
      auto* tu = as<Ttuple>(d);
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited)) return {t, U::Unchanged};
      Visited v2 = cons_v(tt, visited);
      std::vector<std::pair<TypeExpr*, Change>> tl;
      for (auto& e : tu->elems) tl.push_back(build_subtype(env, v2, loops, posi, level, e.ty));
      Change c = U::Unchanged;
      for (auto& x : tl) c = max_change(c, x.second);
      if (c > U::Unchanged) {
        std::vector<LabeledTy> l;
        for (std::size_t k = 0; k < tl.size(); ++k) l.push_back({tu->elems[k].label, tl[k].first});
        return {newty(ttuple(slice(l))), c};
      }
      return {t, U::Unchanged};
    }
    case DK::Tconstr: {
      auto* co = as<Tconstr>(d);
      Path::t p = co->path;
      if (level > 0 && generic_abbrev(env, p) && safe_abbrev(env, t) && !has_constr_row2(env, t)) {
        TypeExpr* tp = expand_abbrev(false, env, t);
        long level2 = pred_expand(level);
        try {
          auto* ob = as<Tobject>(get_desc(tp));
          if (!(ob && posi && !opened_object(tp))) throw env::NotFound{};
          auto [cl_abbr, body] = find_cltype_for_path(env, p);
          TypeExpr* ty;
          try {
            ty = subst(env, current_level, PrivateFlag::Public, co->memo, nullptr,
                       cl_abbr->type_params, co->args, body);
          } catch (const CannotSubst&) {
            throw std::logic_error("Ctype.build_subtype");
          }
          auto* o2 = as<Tobject>(get_desc(ty));
          if (!(o2 && o2->name->contents && path::same(p, o2->name->contents->path)))
            throw env::NotFound{};
          TypeExpr* ty1 = o2->fields;
          Slice<TypeExpr*> tl1 = o2->name->contents->args;
          // Fix PR#4505: do not set ty to Tvar when it appears in tl1, as
          // this occurrence might break the occur check.
          if (deep_occur_list(ty, std::vector<TypeExpr*>(tl1.begin(), tl1.end())))
            throw env::NotFound{};
          set_type_desc(ty, TVAR_NONE_LIT());
          TypeExpr* t2 = newvar();
          Loops loops2 = loops;
          loops2.insert(loops2.begin(), {get_id(ty), t2});
          // May discard [visited] as level is going down
          auto [ty1p, c] = build_subtype(env, Visited{repr(tp)}, loops2, posi,
                                         pred_enlarge(level2), ty1);
          if (!is_Tvar(t2)) throw std::logic_error("Ctype.build_subtype");
          const PathArgs* nm = (c > U::Equiv || deep_occur(ty, ty1p)) ? nullptr
                                                                      : make<PathArgs>(p, tl1);
          set_type_desc(t2, tobject(ty1p, make<NameRef>(nm)));
          try {
            unify_var(env, ty, t);
          } catch (const Unify&) {
            throw std::logic_error("Ctype.build_subtype");
          }
          return {t2, U::Changed};
        } catch (const env::NotFound&) {
          auto [t2, c] = build_subtype(env, visited, loops, posi, level2, tp);
          if (c > U::Unchanged) return {t2, c};
          return {t, U::Unchanged};
        }
      }
      // Must check recursion on constructors, since we do not always
      // expand them
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited)) return {t, U::Unchanged};
      Visited v2 = cons_v(tt, visited);
      try {
        const TypeDeclaration* decl = env::find_type(p, env);
        if (level == 0 && generic_abbrev(env, p) && safe_abbrev(env, t) &&
            !has_constr_row2(env, t))
          warn = true;
        if (decl->type_variance.size() != co->args.size())
          throw std::invalid_argument("List.map2");
        std::vector<std::pair<TypeExpr*, Change>> tl;
        for (std::size_t k = 0; k < co->args.size(); ++k) {
          variance::t v = decl->type_variance[k];
          TypeExpr* a = co->args[k];
          bool vco = variance::mem(variance::F::May_pos, v);
          bool vcn = variance::mem(variance::F::May_neg, v);
          if (vcn) {
            if (vco) tl.push_back({a, U::Unchanged});
            else tl.push_back(build_subtype(env, v2, loops, !posi, level, a));
          } else {
            if (vco) tl.push_back(build_subtype(env, v2, loops, posi, level, a));
            else tl.push_back({newvar(), U::Changed});
          }
        }
        Change c = U::Unchanged;
        for (auto& x : tl) c = max_change(c, x.second);
        if (c > U::Unchanged) {
          std::vector<TypeExpr*> args;
          for (auto& x : tl) args.push_back(x.first);
          return {newconstr(p, slice(args)), c};
        }
        return {t, U::Unchanged};
      } catch (const env::NotFound&) {
        return {t, U::Unchanged};
      }
    }
    case DK::Tvariant: {
      const RowDesc* row = as<Tvariant>(d)->row;
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited) || !static_row(row)) return {t, U::Unchanged};
      long level2 = pred_enlarge(level);
      Visited v2 = cons_v(tt, level2 < level ? Visited{} : filter_visited(visited));
      std::vector<RowFieldEntry> fields = filter_row_fields(false, row_fields(row));
      std::vector<std::pair<RowFieldEntry, Change>> fs;
      for (auto& orig : fields) {
        RowFieldView fv = row_field_repr(orig.field);
        if (fv.kind != RK::Rpresent) throw std::logic_error("Ctype.build_subtype");
        if (!fv.present) {
          if (posi) fs.push_back({{orig.label, rf_either_of(nullptr)}, U::Unchanged});
          else fs.push_back({orig, U::Unchanged});
        } else {
          auto [tp, c] = build_subtype(env, v2, loops, posi, level2, fv.present);
          const RowField* f = (posi && level > 0) ? rf_either_of(tp) : rf_present(tp);
          fs.push_back({{orig.label, f}, c});
        }
      }
      Change c = U::Unchanged;
      for (auto& x : fs) c = max_change(c, x.second);
      std::vector<RowFieldEntry> nf;
      for (auto& x : fs) nf.push_back(x.first);
      const RowDesc* row2 = create_row(slice(nf), newvar(), posi, nullptr,
                                       c > U::Unchanged ? nullptr : row_name(row));
      return {newty(tvariant(row2)), U::Changed};
    }
    case DK::Tobject: {
      TypeExpr* t1 = as<Tobject>(d)->fields;
      TypeExpr* tt = repr(t);
      if (memq_warn(tt, visited) || opened_object(t1)) return {t, U::Unchanged};
      long level2 = pred_enlarge(level);
      Visited v2 = cons_v(tt, level2 < level ? Visited{} : filter_visited(visited));
      auto [t1p, c] = build_subtype(env, v2, loops, posi, level2, t1);
      if (c > U::Unchanged) return {newty(tobject(t1p, make<NameRef>(nullptr))), c};
      return {t, U::Unchanged};
    }
    case DK::Tfield: {  // Always present
      auto* f = as<Tfield>(d);
      auto [t1p, c1] = build_subtype(env, visited, loops, posi, level, f->ty);
      auto [t2p, c2] = build_subtype(env, visited, loops, posi, level, f->rest);
      Change c = max_change(c1, c2);
      if (c > U::Unchanged) return {newty(tfield(f->label, field_public(), t1p, t2p)), c};
      return {t, U::Unchanged};
    }
    case DK::Tnil:
      if (posi) return {newvar(), U::Changed};
      warn = true;
      return {t, U::Unchanged};
    case DK::Tpoly: {
      auto* p = as<Tpoly>(d);
      auto [t1p, c] = build_subtype(env, visited, loops, posi, level, p->body);
      if (c > U::Unchanged) return {newty(tpoly(t1p, p->vars)), c};
      return {t, U::Unchanged};
    }
    case DK::Tunivar:
    case DK::Tpackage:
      return {t, U::Unchanged};
    default:
      throw std::logic_error("Ctype.build_subtype");
  }
}

std::pair<TypeExpr*, bool> enlarge_type(env::t env, TypeExpr* ty) {
  warn = false;
  // [level = 4] allows 2 expansions involving objects/variants
  TypeExpr* ty2 = build_subtype(env, {}, {}, true, 4, ty).first;
  return {ty2, warn};
}

// ---- subtyping: check whether a type is a subtype of another type -----------------------------
// (see the comment in ctype.ml: constraints are accumulated, and a function
// enforcing them is returned)

using STrace = std::vector<et::Diff<TypeExpr*>>;  // back = head of the OCaml list
struct SubConstraint {
  env::t env;
  STrace trace;
  TypeExpr* t1;
  TypeExpr* t2;
  std::vector<UnivarPair> pairs;
};
using Constraints = std::vector<SubConstraint>;  // back = head of the OCaml list

static TypePairs subtypes;

[[noreturn]] static void subtype_error(env::t env, const STrace& trace,
                                       et::ErrorTrace unification_trace) {
  // expand_subtype_trace env (List.rev trace)
  et::subtype::Trace<et::ExpandedType> tr;
  for (auto& d : trace) {
    et::ExpandedType got = expand_type(env, d.got);
    et::ExpandedType expected = expand_type(env, d.expected);
    tr.push_back({got, expected});
  }
  if (tr.empty()) throw std::logic_error("Errortrace.Subtype.error");
  throw Subtype({std::move(tr), std::move(unification_trace)});
}
static STrace scons(TypeExpr* got, TypeExpr* expected, STrace t) {
  t.push_back({got, expected});
  return t;
}
static Constraints ccons(env::t env, const STrace& trace, TypeExpr* t1, TypeExpr* t2,
                         Constraints c) {
  c.push_back({env, trace, t1, t2, univar_pairs});
  return c;
}
static et::Elt<et::ExpandedType> fcm_elt(const et::FirstClassModule& e) {
  auto x = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::First_class_module);
  x.fcm = e;
  return x;
}

static Constraints subtype_rec(env::t env, const STrace& trace, TypeExpr* t1, TypeExpr* t2,
                               Constraints constraints);
static Constraints subtype_labeled_list(env::t env, const STrace& trace, Slice<LabeledTy> l1,
                                        Slice<LabeledTy> l2, Constraints constraints);
static Constraints subtype_package(env::t env, const STrace& trace, long lvl1,
                                   const Package* pack1, long lvl2, const Package* pack2,
                                   Constraints constraints);
static Constraints subtype_functor(env::t env, const STrace& trace, ident::Unscoped* id1,
                                   ident::Unscoped* id, const Package* pack, TypeExpr* u1,
                                   TypeExpr* u2, Constraints constraints);
static Constraints subtype_fields(env::t env, const STrace& trace, TypeExpr* ty1, TypeExpr* ty2,
                                  Constraints constraints);
struct SubtypeExit {};
static Constraints subtype_row(env::t env, const STrace& trace, const RowDesc* row1,
                               const RowDesc* row2, Constraints constraints);

static Constraints subtype_rec(env::t env, const STrace& trace, TypeExpr* t1, TypeExpr* t2,
                               Constraints constraints) {
  if (eq_type(t1, t2)) return constraints;
  if (subtypes.mem(t1, t2)) return constraints;
  subtypes.add(t1, t2);
  const TypeDesc* d1 = get_desc(t1);
  const TypeDesc* d2 = get_desc(t2);
  if (d1->kind == DK::Tvar || d2->kind == DK::Tvar)
    return ccons(env, trace, t1, t2, std::move(constraints));
  auto* a1 = as<Tarrow>(d1);
  auto* a2 = as<Tarrow>(d2);
  auto* f1 = as<Tfunctor>(d1);
  auto* f2 = as<Tfunctor>(d2);
  auto* c1 = as<Tconstr>(d1);
  auto* c2 = as<Tconstr>(d2);
  if (a1 && a2 && compatible_labels(false, a1->label, a2->label)) {
    // the trace will be updated at the next step due to the Tpoly wrapping
    // of parameter.
    Constraints cs = subtype_rec(env, trace, a2->t1, a1->t1, std::move(constraints));
    return subtype_rec(env, scons(a1->t2, a2->t2, trace), a1->t2, a2->t2, std::move(cs));
  }
  if (f1 && f2 && compatible_labels(false, f1->label, f2->label)) {
    TypeExpr* fcm1 = newty(tpackage(f1->pack));
    TypeExpr* fcm2 = newty(tpackage(f2->pack));
    Constraints cs = subtype_package(env, scons(fcm2, fcm1, trace), get_level(t2), f2->pack,
                                     get_level(t1), f1->pack, constraints);
    try {
      Constraints r;
      enter_functor(env, f1->id, t1, f2->id, t2, [&](IdPairList id_pairs) {
        env::t new_env = env::with_pairs(slice(id_pairs), env);
        r = subtype_functor(new_env, trace, f1->id, f2->id, f2->pack, f1->body, f2->body, cs);
      });
      return r;
    } catch (const Escape&) {
      return ccons(env, trace, t1, t2, std::move(cs));
    }
  }
  if (f1 && a2 && compatible_labels(false, f1->label, a2->label)) {
    TypeExpr* fcm1 = newmono_package(f1->pack);
    // [trace] : see [(Tarrow, Tarrow)] comment
    Constraints cs = subtype_rec(env, trace, a2->t1, fcm1, std::move(constraints));
    TypeExpr* fcm2 = tpoly_get_mono(a2->t1);
    const Package* pack2;
    try {
      pack2 = extract_package_modulo_subtype(env, fcm2);
    } catch (const env::NotFound&) {
      return ccons(env, trace, t1, t2, std::move(cs));
    }
    return subtype_functor(env, trace, nullptr, f1->id, pack2, f1->body, a2->t2, std::move(cs));
  }
  if (a1 && f2 && compatible_labels(false, a1->label, f2->label)) {
    TypeExpr* fcm2 = newmono_package(f2->pack);
    Constraints cs = subtype_rec(env, trace, fcm2, a1->t1, std::move(constraints));
    return subtype_functor(env, trace, nullptr, f2->id, f2->pack, a1->t2, f2->body, std::move(cs));
  }
  if (d1->kind == DK::Ttuple && d2->kind == DK::Ttuple)
    return subtype_labeled_list(env, trace, as<Ttuple>(d1)->elems, as<Ttuple>(d2)->elems,
                                std::move(constraints));
  if (c1 && c2 && c1->args.empty() && c2->args.empty() &&
      env::path_equiv(env, c1->path, c2->path))
    return constraints;
  if (c1 && generic_abbrev(env, c1->path) && safe_abbrev(env, t1))
    return subtype_rec(env, trace, expand_abbrev(false, env, t1), t2, std::move(constraints));
  if (c2 && generic_abbrev(env, c2->path) && safe_abbrev(env, t2))
    return subtype_rec(env, trace, t1, expand_abbrev(false, env, t2), std::move(constraints));
  if (c1 && c2 && env::path_equiv(env, c1->path, c2->path)) {
    try {
      const TypeDeclaration* decl = env::find_type(c1->path, env);
      if (c1->args.size() != c2->args.size()) throw std::invalid_argument("List.combine");
      if (decl->type_variance.size() != c1->args.size())
        throw std::invalid_argument("List.fold_left2");
      Constraints cs = std::move(constraints);
      for (std::size_t k = 0; k < c1->args.size(); ++k) {
        variance::t v = decl->type_variance[k];
        TypeExpr* u1 = c1->args[k];
        TypeExpr* u2 = c2->args[k];
        bool co = variance::mem(variance::F::May_pos, v);
        bool cn = variance::mem(variance::F::May_neg, v);
        if (co) {
          if (cn) {
            TypeExpr* w2 = newty2(get_level(u2), ttuple(slice({LabeledTy{OptStr::none(), u2}})));
            TypeExpr* w1 = newty2(get_level(u1), ttuple(slice({LabeledTy{OptStr::none(), u1}})));
            cs = ccons(env, trace, w1, w2, std::move(cs));
          } else {
            cs = subtype_rec(env, scons(u1, u2, trace), u1, u2, std::move(cs));
          }
        } else if (cn) {
          cs = subtype_rec(env, scons(u2, u1, trace), u2, u1, std::move(cs));
        }
      }
      return cs;
    } catch (const env::NotFound&) {
      return ccons(env, trace, t1, t2, std::move(constraints));
    }
  }
  if (c1 && generic_private_abbrev(env, c1->path) && safe_abbrev_opt(env, t1))
    return subtype_rec(env, trace, expand_abbrev_opt(env, t1), t2, std::move(constraints));
  auto* o1 = as<Tobject>(d1);
  auto* o2 = as<Tobject>(d2);
  if (o1 && o2 && is_Tvar(object_row(o1->fields)) && is_Tvar(object_row(o2->fields)))
    // Same row variable implies same object.
    return ccons(env, trace, t1, t2, std::move(constraints));
  if (o1 && o2) return subtype_fields(env, trace, o1->fields, o2->fields, std::move(constraints));
  if (d1->kind == DK::Tvariant && d2->kind == DK::Tvariant) {
    Constraints saved = constraints;
    try {
      return subtype_row(env, trace, as<Tvariant>(d1)->row, as<Tvariant>(d2)->row,
                         std::move(constraints));
    } catch (const SubtypeExit&) {
      return ccons(env, trace, t1, t2, std::move(saved));
    }
  }
  auto* p1 = as<Tpoly>(d1);
  auto* p2 = as<Tpoly>(d2);
  if (p1 && p2) {
    if (p1->vars.empty() && p2->vars.empty())
      return subtype_rec(env, scons(p1->body, p2->body, trace), p1->body, p2->body,
                         std::move(constraints));
    if (p2->vars.empty()) {
      STrace tr = scons(t1, p2->body, trace);
      TypeExpr* u1p = instance_poly(p1->vars, p1->body);
      return subtype_rec(env, tr, u1p, p2->body, std::move(constraints));
    }
    STrace tr = scons(t1, t2, trace);
    try {
      Constraints r;
      enter_poly(env, p1->body, p1->vars, p2->body, p2->vars, [&](TypeExpr* a, TypeExpr* b) {
        r = subtype_rec(env, tr, a, b, constraints);
      });
      return r;
    } catch (const Escape&) {
      return ccons(env, tr, t1, t2, std::move(constraints));
    }
  }
  if (d1->kind == DK::Tpackage && d2->kind == DK::Tpackage)
    return subtype_package(env, trace, get_level(t1), as<Tpackage>(d1)->pack, get_level(t2),
                           as<Tpackage>(d2)->pack, std::move(constraints));
  return ccons(env, trace, t1, t2, std::move(constraints));
}

static Constraints subtype_labeled_list(env::t env, const STrace& trace, Slice<LabeledTy> l1,
                                        Slice<LabeledTy> l2, Constraints constraints) {
  if (l1.size() != l2.size()) subtype_error(env, trace, {});
  for (std::size_t k = 0; k < l1.size(); ++k) {
    if (!(l1[k].label == l2[k].label)) subtype_error(env, trace, {});
    constraints =
        subtype_rec(env, scons(l1[k].ty, l2[k].ty, trace), l1[k].ty, l2[k].ty, std::move(constraints));
  }
  return constraints;
}

static Constraints subtype_package(env::t env, const STrace& trace, long lvl1,
                                   const Package* pack1, long lvl2, const Package* pack2,
                                   Constraints constraints) {
  // `let ntl1 = .. and ntl2 = ..`: left to right
  CompleteResult ntl1 =
      complete_type_list(et::Position::Second, env, pack2->pack_constraints, lvl1, pack1);
  CompleteResult ntl2 =
      complete_type_list(et::Position::First, env, pack1->pack_constraints, lvl2, pack2, true);
  if (!ntl1.ok) subtype_error(env, trace, {fcm_elt(ntl1.err)});
  if (!ntl2.ok) subtype_error(env, trace, {fcm_elt(ntl2.err)});
  Constraints cs2;  // constraints' (in OCaml list order: head = front)
  std::vector<SubConstraint> cprime;
  for (auto& [n2, t2] : ntl2.res) {
    TypeExpr* a = nullptr;
    for (auto& [n1, t1] : ntl1.res)
      if (std::equal(n1.begin(), n1.end(), n2.begin(), n2.end())) {
        a = t1;
        break;
      }
    if (!a) throw env::NotFound{};  // List.assoc
    cprime.push_back({env, trace, a, t2, univar_pairs});
  }
  auto append = [&](Constraints c) {  // constraints' @ constraints
    for (auto it = cprime.rbegin(); it != cprime.rend(); ++it) c.push_back(*it);
    return c;
  };
  if (eq_package_path(env, pack1->pack_path, pack2->pack_path)) return append(std::move(constraints));
  // need to check module subtyping
  Snapshot snap = btype::snapshot();
  try {
    for (auto& c : cprime) unify(c.env, c.t1, c.t2);
  } catch (const Unify& u) {
    btype::backtrack(snap);
    subtype_error(env, trace, u.err.trace);
  }
  PackageSubtypeResult r = package_subtype(env, pack1, pack2);
  btype::backtrack(snap);
  if (r.ok) return append(std::move(constraints));
  subtype_error(env, trace, {fcm_elt(r.err)});
}

static Constraints subtype_functor(env::t env, const STrace& trace, ident::Unscoped* id1,
                                   ident::Unscoped* id, const Package* pack, TypeExpr* u1,
                                   TypeExpr* u2, Constraints constraints) {
  const ModuleType* mty = modtype_of_package(env, location::none(), pack);
  env::t env2 =
      id1 ? env::add_module(Ident::of_unscoped(id1), ModulePresence::Mp_present, mty, env) : env;
  env2 = env::add_module(Ident::of_unscoped(id), ModulePresence::Mp_present, mty, env2);
  return subtype_rec(env2, scons(u1, u2, trace), u1, u2, std::move(constraints));
}

static Constraints subtype_fields(env::t env, const STrace& trace, TypeExpr* ty1, TypeExpr* ty2,
                                  Constraints constraints) {
  // Assume that either rest1 or rest2 is not Tvar
  auto [fields1, rest1] = flatten_fields(ty1);
  auto [fields2, rest2] = flatten_fields(ty2);
  AssociatedFields af = associate_fields(fields1, fields2);
  Constraints cs = std::move(constraints);
  if (get_desc(rest2)->kind != DK::Tnil) {
    if (af.miss1.empty())
      cs = subtype_rec(env, scons(rest1, rest2, trace), rest1, rest2, std::move(cs));
    else
      cs = ccons(env, trace, build_fields(get_level(ty1), af.miss1, rest1), rest2, std::move(cs));
  }
  if (!af.miss2.empty()) {
    TypeExpr* v = newvar();
    cs = ccons(env, trace, rest1, build_fields(get_level(ty2), af.miss2, v), std::move(cs));
  }
  for (auto& p : af.pairs)
    // These fields are always present
    cs = subtype_rec(env, scons(p.t1, p.t2, trace), p.t1, p.t2, std::move(cs));
  return cs;
}

static Constraints subtype_row(env::t env, const STrace& trace, const RowDesc* row1,
                               const RowDesc* row2, Constraints constraints) {
  RowDescRepr r1d = row_repr(row1);
  RowDescRepr r2d = row_repr(row2);
  TypeExpr* more1 = r1d.more;
  TypeExpr* more2 = r2d.more;
  MergedRowFields m = merge_row_fields(r1d.fields, r2d.fields);
  Slice<RowFieldEntry> r1 = r2d.closed ? slice(filter_row_fields(false, m.r1)) : m.r1;
  Slice<RowFieldEntry> r2 = r1d.closed ? slice(filter_row_fields(false, m.r2)) : m.r2;
  const TypeDesc* md1 = get_desc(more1);
  const TypeDesc* md2 = get_desc(more2);
  auto* k1 = as<Tconstr>(md1);
  auto* k2 = as<Tconstr>(md2);
  auto vcn = [](const TypeDesc* d) {
    return d->kind == DK::Tvar || d->kind == DK::Tconstr || d->kind == DK::Tnil;
  };
  if (k1 && k2 && env::path_equiv(env, k1->path, k2->path))
    return subtype_rec(env, scons(more1, more2, trace), more1, more2, std::move(constraints));
  if (vcn(md1) && vcn(md2) && r1d.closed && r1.empty()) {
    Constraints cs = std::move(constraints);
    for (auto& pr : m.pairs) {
      RowFieldView v1 = row_field_repr(pr.f1);
      RowFieldView v2 = row_field_repr(pr.f2);
      bool v1_none_or_either_true = (v1.kind == RK::Rpresent && !v1.present) ||
                                    (v1.kind == RK::Reither && v1.constant);
      if (v1_none_or_either_true && v2.kind == RK::Rpresent && !v2.present) continue;
      if (v1.kind == RK::Rpresent && v1.present && v2.kind == RK::Rpresent && v2.present) {
        cs = subtype_rec(env, scons(v1.present, v2.present, trace), v1.present, v2.present,
                         std::move(cs));
      } else if (v1.kind == RK::Reither && !v1.constant && !v1.arg_types.empty() &&
                 v2.kind == RK::Rpresent && v2.present) {
        TypeExpr* a = v1.arg_types[0];
        cs = subtype_rec(env, scons(a, v2.present, trace), a, v2.present, std::move(cs));
      } else if (v1.kind == RK::Rabsent) {
      } else if (v1.kind == RK::Rpresent && v2.kind == RK::Rpresent) {
        auto x = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::Variant);
        x.variant.kind = et::Variant::Kind::Incompatible_types_for;
        x.variant.name = pr.label;
        subtype_error(env, trace, {x});
      } else {
        throw SubtypeExit{};
      }
    }
    return cs;
  }
  if (md1->kind == DK::Tunivar && md2->kind == DK::Tunivar && r1d.closed == r2d.closed &&
      r1.empty() && r2.empty()) {
    Constraints cs = subtype_rec(env, scons(more1, more2, trace), more1, more2, std::move(constraints));
    for (auto& pr : m.pairs) {
      RowFieldView v1 = row_field_repr(pr.f1);
      RowFieldView v2 = row_field_repr(pr.f2);
      if ((v1.kind == RK::Rpresent && !v1.present && v2.kind == RK::Rpresent && !v2.present) ||
          (v1.kind == RK::Reither && v1.constant && v1.arg_types.empty() &&
           v2.kind == RK::Reither && v2.constant && v2.arg_types.empty()) ||
          (v1.kind == RK::Rabsent && v2.kind == RK::Rabsent))
        continue;
      TypeExpr* a = nullptr;
      TypeExpr* b = nullptr;
      if (v1.kind == RK::Rpresent && v1.present && v2.kind == RK::Rpresent && v2.present) {
        a = v1.present;
        b = v2.present;
      } else if (v1.kind == RK::Reither && !v1.constant && v1.arg_types.size() == 1 &&
                 v2.kind == RK::Reither && !v2.constant && v2.arg_types.size() == 1) {
        a = v1.arg_types[0];
        b = v2.arg_types[0];
      } else {
        throw SubtypeExit{};
      }
      cs = subtype_rec(env, scons(a, b, trace), a, b, std::move(cs));
    }
    return cs;
  }
  throw SubtypeExit{};
}

std::function<void()> subtype(env::t env, TypeExpr* ty1, TypeExpr* ty2) {
  subtypes.clear();
  return with_univar_pairs({}, [&]() -> std::function<void()> {
    // Build constraint set.
    STrace trace0;
    trace0.push_back({ty1, ty2});
    Constraints constraints = subtype_rec(env, trace0, ty1, ty2, {});
    subtypes.clear();
    // Enforce constraints.
    return [constraints = std::move(constraints)] {
      for (auto& c : constraints) {  // List.rev constraints
        try {
          unify_pairs(c.env, c.t1, c.t2, c.pairs);
        } catch (const Unify& u) {
          if (u.err.trace.empty()) throw std::logic_error("List.tl");
          subtype_error(c.env, c.trace, et::ErrorTrace(u.err.trace.begin() + 1, u.err.trace.end()));
        }
      }
    };
  });
}

// ---- miscellaneous ---------------------------------------------------------------------------

// Utility for printing.  The resulting type is not used in computation.
static TypeExpr* unalias_object(TypeExpr* ty) {
  long level = get_level(ty);
  const TypeDesc* d = get_desc(ty);
  switch (d->kind) {
    case DK::Tfield: {
      auto* f = as<Tfield>(d);
      return newty2(level, tfield(f->label, f->kind_, f->ty, unalias_object(f->rest)));
    }
    case DK::Tvar:
    case DK::Tnil:
    case DK::Tconstr:
      return newty2(level, d);
    case DK::Tunivar:
      return ty;
    default:
      throw std::logic_error("Ctype.unalias_object");
  }
}

TypeExpr* unalias(TypeExpr* ty) {
  long level = get_level(ty);
  const TypeDesc* d = get_desc(ty);
  switch (d->kind) {
    case DK::Tvar:
    case DK::Tunivar:
      return ty;
    case DK::Tvariant: {
      RowDescRepr r = row_repr(as<Tvariant>(d)->row);
      TypeExpr* more = newty2(get_level(r.more), get_desc(r.more));
      return newty2(level, tvariant(create_row(slice(r.fields), more, r.closed, r.fixed, r.name)));
    }
    case DK::Tobject: {
      auto* o = as<Tobject>(d);
      return newty2(level, tobject(unalias_object(o->fields), o->name));
    }
    default:
      return newty2(level, d);
  }
}

// Check for non-generalizable type variables
struct NongenAcc {
  TypeSet visited;
  TypeSet weak_set;
};
static NongenAcc nongen_loop(env::t env, NongenAcc acc, TypeExpr* ty) {
  if (acc.visited.mem(ty)) return acc;
  acc.visited.add(ty);
  const TypeDesc* d = get_desc(ty);
  auto fold = [&](NongenAcc a, TypeExpr* t) {
    iter_type_expr([&](TypeExpr* c) { a = nongen_loop(env, std::move(a), c); }, t);
    return a;
  };
  switch (d->kind) {
    case DK::Tvar:
      if (get_level(ty) != generic_level) {
        acc.weak_set.add(ty);
        return acc;
      }
      return fold(std::move(acc), ty);
    case DK::Tconstr: {
      NongenAcc unexpanded = fold(acc, ty);
      // `unexpanded_candidate == weak_set`: the sets only grow
      if (unexpanded.weak_set.s.size() == acc.weak_set.s.size()) return acc;
      TypeExpr* exp;
      try {
        exp = try_expand_head(try_expand_safe, env, ty);
      } catch (const CannotExpand&) {
        return unexpanded;
      }
      return nongen_loop(env, std::move(acc), exp);
    }
    case DK::Tfield: {
      auto* f = as<Tfield>(d);
      if (field_kind_repr(f->kind_) == FieldKindView::Fpublic)
        acc = nongen_loop(env, std::move(acc), f->ty);
      return nongen_loop(env, std::move(acc), f->rest);
    }
    case DK::Tvariant: {
      const RowDesc* row = as<Tvariant>(d)->row;
      iter_row([&](TypeExpr* c) { acc = nongen_loop(env, std::move(acc), c); }, row);
      if (!static_row(row)) return nongen_loop(env, std::move(acc), row_more(row));
      return acc;
    }
    default:
      return fold(std::move(acc), ty);
  }
}

static TypeSet add_nongen_vars_in_schema(env::t env, TypeSet acc, TypeExpr* ty) {
  return nongen_loop(env, NongenAcc{TypeSet{}, std::move(acc)}, ty).weak_set;
}

// Return all non-generic variables of [ty] (nullopt = None).
std::optional<TypeSet> nongen_vars_in_schema(env::t env, TypeExpr* ty) {
  TypeSet result = add_nongen_vars_in_schema(env, TypeSet{}, ty);
  if (result.is_empty()) return std::nullopt;
  return result;
}

// Check that all type variables are generalizable.  Use Env.empty to
// prevent expansion of recursively defined object types.
static TypeSet nongen_class_type(const ClassType* cty, TypeSet weak_set) {
  env::t e = env::empty();
  switch (cty->kind) {
    case ClassType::Kind::Cty_constr:
      for (TypeExpr* p : cty->args) weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), p);
      return weak_set;
    case ClassType::Kind::Cty_signature: {
      ClassSignature* sign = cty->sign;
      weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), sign->csig_self);
      weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), sign->csig_self_row);
      sign->csig_meths.iter([&](std::string_view, const MethEntry& m) {
        weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), m.ty);
      });
      sign->csig_vars.iter([&](std::string_view, const VarEntry& v) {
        weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), v.ty);
      });
      return weak_set;
    }
    case ClassType::Kind::Cty_arrow:
      weak_set = add_nongen_vars_in_schema(e, std::move(weak_set), cty->arg);
      return nongen_class_type(cty->cty, std::move(weak_set));
  }
  return weak_set;
}

TypeSet nongen_class_declaration(const ClassDeclaration* cty) {
  TypeSet s;
  for (TypeExpr* p : cty->cty_params) s = add_nongen_vars_in_schema(env::empty(), std::move(s), p);
  return nongen_class_type(cty->cty_type, std::move(s));
}

std::optional<TypeSet> nongen_vars_in_class_declaration(const ClassDeclaration* cty) {
  TypeSet result = nongen_class_declaration(cty);
  if (result.is_empty()) return std::nullopt;
  return result;
}

// Normalize a type before printing, saving...  (Cannot use mark_type
// because deep_occur uses it too)
static void normalize_type_rec(TypeMark& mark, TypeExpr* ty) {
  if (!try_mark_node(mark, ty)) return;
  TypeExpr* tm = row_of_type(ty);
  if (!is_Tconstr(ty) && is_constr_row(false, tm)) {
    auto* c = as<Tconstr>(get_desc(tm));  // PR#7348
    if (!c || c->path->kind != Path::Kind::Pdot) throw std::logic_error("Ctype.normalize_type_rec");
    std::string_view i = c->path->s;
    std::string_view i2 = i.substr(0, i.size() - 4);
    set_type_desc(ty, tconstr(Path::pdot(c->path->p1, i2), c->args, make<MemoRef>(mnil())));
  } else {
    const TypeDesc* d = get_desc(ty);
    if (auto* v = as<Tvariant>(d)) {
      RowDescRepr r = row_repr(v->row);
      std::vector<RowFieldEntry> fields;
      for ([[maybe_unused]] auto& [l, f, l_obj] : r.fields) {
        RowFieldView fv = row_field_repr(f);
        const RowField* nf = f;
        if (fv.kind == RK::Reither && fv.arg_types.size() >= 2) {
          std::vector<TypeExpr*> tyl2{fv.arg_types[0]};  // head first
          std::size_t ntl = fv.arg_types.size() - 1;
          for (std::size_t k = 1; k < fv.arg_types.size(); ++k) {
            TypeExpr* t = fv.arg_types[k];
            bool exists = false;
            for (TypeExpr* t2 : tyl2)
              if (is_equal(env::empty(), false, slice({t}), slice({t2}))) {
                exists = true;
                break;
              }
            if (!exists) tyl2.insert(tyl2.begin(), t);
          }
          if (tyl2.size() <= ntl) {
            std::reverse(tyl2.begin(), tyl2.end());
            nf = rf_either(f, fv.constant, slice(tyl2), fv.matched);
          }
        }
        fields.push_back({l, nf});
      }
      std::vector<RowFieldEntry> kept;
      for (auto& e : fields)
        if (row_field_repr(e.field).kind != RK::Rabsent) kept.push_back(e);
      std::stable_sort(kept.begin(), kept.end(),
                       [](const RowFieldEntry& a, const RowFieldEntry& b) { return a.label < b.label; });
      set_type_desc(ty, tvariant(create_row(slice(kept), r.more, r.closed, r.fixed, r.name)));
    } else if (auto* o = as<Tobject>(d)) {
      TypeExpr* fi = o->fields;
      NameRef* nm = o->name;
      if (const PathArgs* pa = nm->contents) {
        if (pa->args.empty()) throw std::logic_error("Ctype.normalize_type_rec");
        TypeExpr* v0 = pa->args[0];
        Slice<TypeExpr*> l = slice(std::vector<TypeExpr*>(pa->args.begin() + 1, pa->args.end()));
        if (deep_occur_list(ty, std::vector<TypeExpr*>(l.begin(), l.end()))) {
          // The abbreviation may be hiding something, so remove it
          set_name(nm, nullptr);
        } else {
          DK k = get_desc(v0)->kind;
          if (k == DK::Tvar || k == DK::Tunivar) {
          } else if (k == DK::Tnil) {
            set_type_desc(ty, tconstr(pa->path, l, make<MemoRef>(mnil())));
          } else {
            set_name(nm, nullptr);
          }
        }
      }
      long level = get_level(fi);
      if (level >= lowest_level) {
        auto [fields, row] = flatten_fields(fi);
        TypeExpr* fi2 = build_fields(level, fields, row);
        set_type_desc(fi, get_desc(fi2));
      }
    }
  }
  iter_type_expr([&](TypeExpr* t) { normalize_type_rec(mark, t); }, ty);
}

void normalize_type(TypeExpr* ty) {
  with_type_mark([&](TypeMark& mark) { normalize_type_rec(mark, ty); });
}

ArrowSpine arrow_spine(env::t env, TypeExpr* ty0) {
  Snapshot snap = btype::snapshot();
  ArrowSpine result;
  with_type_mark([&](TypeMark& mark) {
    wrap_trace_gadt_instances(env, [&] {
      TypeExpr* ty_fun = ty0;
      for (;;) {
        TypeExpr* ty = expand_head(env, ty_fun);
        if (!try_mark_node(mark, ty)) {
          result.ret_cycle = true;
          return;
        }
        const TypeDesc* d = get_desc(ty);
        if (auto* a = as<Tarrow>(d)) {
          result.args.push_back({a->label, ArrowArg{false, a->t1, nullptr, nullptr}});
          ty_fun = a->t2;
        } else if (auto* f = as<Tfunctor>(d)) {
          result.args.push_back({f->label, ArrowArg{true, nullptr, f->id, f->pack}});
          ty_fun = f->body;
        } else {
          result.ret = ty;
          return;
        }
      }
    });
  });
  btype::backtrack(snap);
  return result;
}

std::pair<std::vector<ArgLabel>, bool> arrow_labels(env::t env, TypeExpr* ty) {
  ArrowSpine s = arrow_spine(env, ty);
  bool is_ret_tvar = !s.ret_cycle && is_Tvar(s.ret);
  std::vector<ArgLabel> labels;
  for (auto& a : s.args) labels.push_back(a.first);
  return {labels, is_ret_tvar};
}

// ---- remove dependencies -----------------------------------------------------------------
// Variables are left unchanged.  Other type nodes are duplicated, with
// levels set to generic level.  We cannot use Tsubst here, because
// unification may be called by expand_abbrev.

struct NondepScope {
  TypeHash<TypeExpr*> copied_types;
  TypeHash<TypeExpr*> copied_variants;
  void reset() {
    copied_types.h.clear();
    copied_variants.h.clear();
  }
};

using IdMap = std::vector<std::pair<Ident::t, Path::t>>;

static TypeExpr* nondep_type_rec_aux(bool expand_private, env::t env, NondepScope& scope,
                                     const IdMap& id_map, const std::vector<Ident::t>& ids,
                                     TypeExpr* ty) {
  auto try_expand = [&](env::t e, TypeExpr* t) {
    return expand_private ? try_expand_safe_opt(e, t) : try_expand_safe_no_link(e, t);
  };
  const TypeDesc* desc = get_folded_desc(true, ty);
  if (desc->kind == DK::Tvar || desc->kind == DK::Tunivar) return ty;
  if (TypeExpr** f = scope.copied_types.find_opt(ty)) return *f;
  TypeExpr* ty2 = newgenstub(get_scope(ty));
  scope.copied_types.add(ty, ty2);
  auto nondep_trec = [&](TypeExpr* t, bool ep = false) {
    return nondep_type_rec_aux(ep, env, scope, id_map, ids, t);
  };
  const TypeDesc* desc2;
  try {
    switch (desc->kind) {
      case DK::Tconstr: {
        auto* c = as<Tconstr>(desc);
        try {
          // First, try keeping the same type constructor p
          if (auto id = path::find_free_opt(ids, c->path)) throw NondepCannotErase{*id};
          std::vector<TypeExpr*> tl;
          for (TypeExpr* a : c->args) tl.push_back(nondep_trec(a));
          desc2 = tconstr(path::subst(id_map, c->path), slice(tl), make<MemoRef>(mnil()));
        } catch (const NondepCannotErase& exn) {
          // If that doesn't work, try expanding abbrevs
          if (desc != get_desc(ty)) {
            scope.copied_types.remove(ty);
            desc2 = tlink(nondep_trec(ignore_abbrev(ty), expand_private));
          } else {
            TypeExpr* exp;
            try {
              exp = try_expand(env, newty2(get_level(ty), desc));
            } catch (const CannotExpand&) {
              throw exn;
            }
            // The [Tlink] is important (see ctype.ml).
            desc2 = tlink(nondep_trec(exp, expand_private));
          }
        }
        break;
      }
      case DK::Tpackage:
      case DK::Tfunctor: {
        const Package* pack =
            desc->kind == DK::Tpackage ? as<Tpackage>(desc)->pack : as<Tfunctor>(desc)->pack;
        const Package* pack2 = pack;
        std::optional<Ident::t> opt;
        if (path::exists_free(ids, pack->pack_path)) {
          Path::t p2 = env::normalize_modtype_path(env, pack->pack_path);
          pack2 = make<Package>(p2, pack->pack_constraints);
          opt = path::find_free_opt(ids, p2);
        }
        if (opt) throw NondepCannotErase{*opt};
        auto psubst = [&](Path::t p) { return path::subst(id_map, p); };
        if (desc->kind == DK::Tpackage) {
          desc2 = tpackage(map_pack(psubst, [&](TypeExpr* t) { return nondep_trec(t); }, pack2));
          break;
        }
        auto* fu = as<Tfunctor>(desc);
        const Package* pack3 = map_pack(psubst, [&](TypeExpr* t) { return nondep_trec(t); }, pack2);
        ident::Unscoped* us2 = ident::Unscoped::refresh(fu->id);
        Ident::t id_us = Ident::of_unscoped(fu->id);
        IdMap id_map2{{id_us, Path::pident(Ident::of_unscoped(us2))}};
        for (auto& e : id_map)
          if (!ident::same(e.first, id_us)) id_map2.push_back(e);
        // [ids] correspond to free variables and [id_us] is locally bound.
        for (Ident::t i : ids)
          if (ident::same(i, id_us)) throw std::logic_error("Ctype.nondep_type_rec");
        const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
        env::t env2 = env::add_module(id_us, ModulePresence::Mp_present, mty, env);
        TypeExpr* t2 = nondep_type_rec_aux(false, env2, scope, id_map2, ids, fu->body);
        desc2 = tfunctor(fu->label, us2, pack3, t2);
        break;
      }
      case DK::Tobject: {
        auto* o = as<Tobject>(desc);
        // constructor arguments: right to left
        const PathArgs* nm = nullptr;
        if (const PathArgs* pa = o->name->contents) {
          if (!path::exists_free(ids, pa->path)) {
            std::vector<TypeExpr*> tl;
            for (TypeExpr* a : pa->args) tl.push_back(nondep_trec(a));
            nm = make<PathArgs>(path::subst(id_map, pa->path), slice(tl));
          }
        }
        NameRef* name = make<NameRef>(nm);
        desc2 = tobject(nondep_trec(o->fields), name);
        break;
      }
      case DK::Tvariant: {
        const RowDesc* row = as<Tvariant>(desc)->row;
        TypeExpr* more = row_more(row);
        // We must keep sharing according to the row variable
        if (TypeExpr** f = scope.copied_variants.find_opt(more)) {
          // This variant type has been already copied
          TypeExpr* tyv = *f;
          scope.copied_types.add(ty, tyv);
          desc2 = tlink(tyv);
          break;
        }
        // Register new type first for recursion
        scope.copied_variants.add(more, ty2);
        bool is_static = static_row(row);
        TypeExpr* more2 = is_static ? newgenty(tnil()) : nondep_trec(more);
        // Return a new copy
        const RowDesc* row2 = copy_row([&](TypeExpr* t) { return nondep_trec(t); }, true, row,
                                       true, more2);
        const PathArgs* nm = row_name(row2);
        if (nm && path::exists_free(ids, nm->path))
          desc2 = tvariant(set_row_name(row2, nullptr));
        else if (nm)
          desc2 = tvariant(set_row_name(row2, make<PathArgs>(path::subst(id_map, nm->path), nm->args, nm->tail)));
        else
          desc2 = tvariant(row2);
        break;
      }
      default:
        desc2 = copy_type_desc([&](TypeExpr* t) { return nondep_trec(t); }, desc);
        break;
    }
  } catch (...) {
    scope.copied_types.remove(ty);
    throw;
  }
  transient_expr::set_stub_desc(ty2, desc2);
  return ty2;
}

static TypeExpr* nondep_type_rec(env::t env, NondepScope& scope, const std::vector<Ident::t>& ids,
                                 TypeExpr* ty, bool expand_private = false) {
  return nondep_type_rec_aux(expand_private, env, scope, {}, ids, ty);
}

TypeExpr* nondep_type(env::t env, const std::vector<Ident::t>& ids, TypeExpr* ty) {
  NondepScope scope;
  return nondep_type_rec(env, scope, ids, ty);
}

// Preserve sharing inside type declarations.
const TypeDeclaration* nondep_type_decl(env::t env, const std::vector<Ident::t>& mid,
                                        bool is_covariant, const TypeDeclaration* decl) {
  NondepScope scope;
  std::vector<TypeExpr*> params;
  for (TypeExpr* p : decl->type_params) params.push_back(nondep_type_rec(env, scope, mid, p));
  const TypeKind* tk;
  try {
    tk = map_kind([&](TypeExpr* t) { return nondep_type_rec(env, scope, mid, t); },
                  decl->type_kind);
  } catch (const NondepCannotErase&) {
    if (!is_covariant) throw;
    tk = TYPE_ABSTRACT_LIT(Definition);
  }
  TypeExpr* tm = nullptr;
  PrivateFlag priv = decl->type_private;
  if (decl->type_manifest) {
    try {
      tm = nondep_type_rec(env, scope, mid, decl->type_manifest);
    } catch (const NondepCannotErase&) {
      if (!is_covariant) throw;
      scope.reset();
      try {
        tm = nondep_type_rec(env, scope, mid, decl->type_manifest, true);
        priv = PrivateFlag::Private;
      } catch (const NondepCannotErase&) {
        tm = nullptr;
        priv = decl->type_private;
      }
    }
  }
  if (tm && has_constr_row(tm)) priv = PrivateFlag::Private;
  TypeDeclaration* r = make<TypeDeclaration>(*decl);
  r->type_params = slice(params);
  r->type_kind = tk;
  r->type_manifest = tm;
  r->manifest_obj.reset();  // a new Some block
  r->type_private = priv;
  r->type_is_newtype = false;
  r->type_expansion_scope = lowest_level;
  return r;
}

// Preserve sharing inside extension constructors.
const ExtensionConstructor* nondep_extension_constructor(env::t env,
                                                         const std::vector<Ident::t>& ids,
                                                         const ExtensionConstructor* ext) {
  NondepScope scope;
  Path::t type_path;
  Slice<TypeExpr*> type_params;
  if (auto id = path::find_free_opt(ids, ext->ext_type_path)) {
    TypeExpr* ty =
        newgenty(tconstr(ext->ext_type_path, ext->ext_type_params, make<MemoRef>(mnil())));
    TypeExpr* ty2 = nondep_type_rec(env, scope, ids, ty);
    auto* c = as<Tconstr>(get_desc(ty2));
    if (!c) throw NondepCannotErase{*id};
    type_path = c->path;
    type_params = c->args;
  } else {
    std::vector<TypeExpr*> tp;
    for (TypeExpr* p : ext->ext_type_params) tp.push_back(nondep_type_rec(env, scope, ids, p));
    type_path = ext->ext_type_path;
    type_params = slice(tp);
  }
  auto f = [&](TypeExpr* t) { return nondep_type_rec(env, scope, ids, t); };
  ConstructorArguments args = map_type_expr_cstr_args(f, ext->ext_args);
  TypeExpr* ret_type = ext->ext_ret_type ? f(ext->ext_ret_type) : nullptr;
  ExtensionConstructor* r = make<ExtensionConstructor>(*ext);
  r->ext_type_path = type_path;
  r->ext_type_params = type_params;
  r->ext_args = args;
  r->ext_ret_type = ret_type;
  return r;
}

// Preserve sharing inside class types.
static ClassSignature* nondep_class_signature(env::t env, NondepScope& scope,
                                              const std::vector<Ident::t>& ids,
                                              const ClassSignature* sign) {
  auto f = [&](TypeExpr* t) { return nondep_type_rec(env, scope, ids, t); };
  // record fields: right to left in definition order
  StrMap<MethEntry> meths =
      sign->csig_meths.map([&](const MethEntry& e) { return MethEntry{e.priv, e.virt, f(e.ty)}; });
  StrMap<VarEntry> vars =
      sign->csig_vars.map([&](const VarEntry& e) { return VarEntry{e.mut, e.virt, f(e.ty)}; });
  FieldKind* dummy = field_kind_internal_repr(sign->csig_dummy_method);
  TypeExpr* self_row = f(sign->csig_self_row);
  TypeExpr* self = f(sign->csig_self);
  return make<ClassSignature>(self, self_row, dummy, vars, meths);
}

static const ClassType* nondep_class_type(env::t env, NondepScope& scope,
                                          const std::vector<Ident::t>& ids, const ClassType* cty) {
  using CK = ClassType::Kind;
  switch (cty->kind) {
    case CK::Cty_constr: {
      if (path::exists_free(ids, cty->path)) return nondep_class_type(env, scope, ids, cty->cty);
      // constructor arguments: right to left
      const ClassType* c = nondep_class_type(env, scope, ids, cty->cty);
      std::vector<TypeExpr*> tyl;
      for (TypeExpr* t : cty->args) tyl.push_back(nondep_type_rec(env, scope, ids, t));
      return make<ClassType>(CK::Cty_constr, cty->path, slice(tyl), c);
    }
    case CK::Cty_signature:
      return make<ClassType>(CK::Cty_signature, nullptr, Slice<TypeExpr*>{}, nullptr,
                             nondep_class_signature(env, scope, ids, cty->sign));
    case CK::Cty_arrow: {
      const ClassType* c = nondep_class_type(env, scope, ids, cty->cty);
      TypeExpr* ty = nondep_type_rec(env, scope, ids, cty->arg);
      return make<ClassType>(CK::Cty_arrow, nullptr, Slice<TypeExpr*>{}, c, nullptr, cty->label, ty);
    }
  }
  return cty;
}

const ClassDeclaration* nondep_class_declaration(env::t env, const std::vector<Ident::t>& ids,
                                                 const ClassDeclaration* decl) {
  if (path::exists_free(ids, decl->cty_path))
    throw std::logic_error("Ctype.nondep_class_declaration");
  NondepScope scope;
  // record fields: right to left in definition order
  TypeExpr* cty_new = decl->cty_new ? nondep_type_rec(env, scope, ids, decl->cty_new) : nullptr;
  const ClassType* cty_type = nondep_class_type(env, scope, ids, decl->cty_type);
  std::vector<TypeExpr*> params;
  for (TypeExpr* p : decl->cty_params) params.push_back(nondep_type_rec(env, scope, ids, p));
  ClassDeclaration* r = make<ClassDeclaration>(*decl);
  r->cty_params = slice(params);
  r->cty_type = cty_type;
  r->cty_new = cty_new;
  r->new_obj.reset();  // a new Some block
  return r;
}

const ClassTypeDeclaration* nondep_cltype_declaration(env::t env, const std::vector<Ident::t>& ids,
                                                      const ClassTypeDeclaration* decl) {
  if (path::exists_free(ids, decl->clty_path))
    throw std::logic_error("Ctype.nondep_cltype_declaration");
  NondepScope scope;
  // record fields: right to left in definition order
  const TypeDeclaration* hash = nondep_type_decl(env, ids, false, decl->clty_hash_type);
  const ClassType* clty_type = nondep_class_type(env, scope, ids, decl->clty_type);
  std::vector<TypeExpr*> params;
  for (TypeExpr* p : decl->clty_params) params.push_back(nondep_type_rec(env, scope, ids, p));
  ClassTypeDeclaration* r = make<ClassTypeDeclaration>(*decl);
  r->clty_params = slice(params);
  r->clty_type = clty_type;
  r->clty_hash_type = hash;
  return r;
}

// collapse conjunctive types in class parameters
static void collapse_conj(env::t env, const std::vector<long>& visited, TypeExpr* ty) {
  long id = get_id(ty);
  if (std::find(visited.begin(), visited.end(), id) != visited.end()) return;
  std::vector<long> v2{id};
  v2.insert(v2.end(), visited.begin(), visited.end());
  if (auto* v = as<Tvariant>(get_desc(ty))) {
    const RowDesc* row = v->row;
    for (auto& e : row_fields(row)) {
      RowFieldView fv = row_field_repr(e.field);
      if (fv.kind == RK::Reither && fv.arg_types.size() >= 2)
        for (std::size_t k = 1; k < fv.arg_types.size(); ++k)
          unify(env, fv.arg_types[0], fv.arg_types[k]);
    }
    iter_row([&](TypeExpr* t) { collapse_conj(env, v2, t); }, row);
  } else {
    iter_type_expr([&](TypeExpr* t) { collapse_conj(env, v2, t); }, ty);
  }
}

void collapse_conj_params(env::t env, Slice<TypeExpr*> params) {
  for (TypeExpr* p : params) collapse_conj(env, {}, p);
}

bool same_constr(env::t env, TypeExpr* t1, TypeExpr* t2) {
  TypeExpr* a = expand_head_nolink(env, t1);
  TypeExpr* b = expand_head_nolink(env, t2);
  auto* c1 = as<Tconstr>(get_desc(a));
  auto* c2 = as<Tconstr>(get_desc(b));
  if (c1 && c2) return env::path_equiv(env, c1->path, c2->path);
  return false;
}

TypeImmediacy immediacy(env::t env, TypeExpr* typ) {
  const TypeDesc* d = get_desc(typ);
  if (auto* c = as<Tconstr>(d)) {
    try {
      return env::find_type(c->path, env)->type_immediate;
    } catch (const env::NotFound&) {
      // This can happen due to e.g. missing -I options (see ctype.ml).
      return TypeImmediacy::Unknown;
    }
  }
  if (auto* v = as<Tvariant>(d)) {
    // if all labels are devoid of arguments, not a pointer
    const RowDesc* row = v->row;
    if (!row_closed(row)) return TypeImmediacy::Unknown;
    for (auto& e : row_fields(row)) {
      RowFieldView fv = row_field_repr(e.field);
      if ((fv.kind == RK::Rpresent && fv.present) || (fv.kind == RK::Reither && !fv.constant))
        return TypeImmediacy::Unknown;
    }
    return TypeImmediacy::Always;
  }
  return TypeImmediacy::Unknown;
}

}  // namespace cppcaml::typing::ctype
