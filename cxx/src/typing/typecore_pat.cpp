// Port of typing/typecore.ml, part 2: typing of patterns (type_pat and the
// helpers that enter pattern variables), counter-example checking for
// Parmatch, and delayed checks ("Typing of patterns" to
// "final_subexpression").
#include <algorithm>

#include "cppcaml/typing/parmatch.hpp"
#include "typecore_internal.hpp"
#include "typecore_pat.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using PK = tt::PatternDesc::Kind;
using SK = pt::PatternDesc::Kind;
using PC = tt::PatternCategory;

bool has_literal_pattern(const pt::Pattern* p) {
  switch (p->ppat_desc->kind) {
    case SK::Ppat_constant:
    case SK::Ppat_interval: return true;
    case SK::Ppat_any:
    case SK::Ppat_type:
    case SK::Ppat_var:
    case SK::Ppat_unpack:
    case SK::Ppat_extension: return false;
    case SK::Ppat_variant: {
      auto* v = as<pt::Ppat_variant>(p->ppat_desc);
      return v->arg && has_literal_pattern(v->arg);
    }
    case SK::Ppat_construct: {
      auto* c = as<pt::Ppat_construct>(p->ppat_desc);
      return c->arg && has_literal_pattern(c->arg->pat);
    }
    case SK::Ppat_exception: return has_literal_pattern(as<pt::Ppat_exception>(p->ppat_desc)->pat);
    case SK::Ppat_constraint: return has_literal_pattern(as<pt::Ppat_constraint>(p->ppat_desc)->pat);
    case SK::Ppat_alias: return has_literal_pattern(as<pt::Ppat_alias>(p->ppat_desc)->pat);
    case SK::Ppat_lazy: return has_literal_pattern(as<pt::Ppat_lazy>(p->ppat_desc)->pat);
    case SK::Ppat_open: return has_literal_pattern(as<pt::Ppat_open>(p->ppat_desc)->pat);
    case SK::Ppat_array:
      for (const pt::Pattern* q : as<pt::Ppat_array>(p->ppat_desc)->pats)
        if (has_literal_pattern(q)) return true;
      return false;
    case SK::Ppat_tuple:
      for (const pt::LabeledPattern& q : as<pt::Ppat_tuple>(p->ppat_desc)->pl)
        if (has_literal_pattern(q.pat)) return true;
      return false;
    case SK::Ppat_record:
      for (auto& [l, q] : as<pt::Ppat_record>(p->ppat_desc)->fields)
        if (has_literal_pattern(q)) return true;
      return false;
    case SK::Ppat_effect: {
      auto* e = as<pt::Ppat_effect>(p->ppat_desc);
      return has_literal_pattern(e->eff) || has_literal_pattern(e->cont);
    }
    case SK::Ppat_or: {
      auto* o = as<pt::Ppat_or>(p->ppat_desc);
      return has_literal_pattern(o->p1) || has_literal_pattern(o->p2);
    }
  }
  return false;
}

// pure / only_impure / as_comp_pattern (the value / computation tags)
static const tt::Pattern* pure(PC category, const tt::Pattern* pat) {
  return category == PC::Value ? pat : tt::as_computation_pattern(pat);
}
static const tt::Pattern* only_impure(PC category, const tt::Pattern* pat) {
  if (category == PC::Value)
    // LATER: this exception could be renamed/generalized
    raise_error(err(pat->pat_loc, pat->pat_env, EK::Exception_pattern_disallowed));
  return pat;
}
const tt::Pattern* as_comp_pattern(PC category, const tt::Pattern* pat) {
  return category == PC::Value ? tt::as_computation_pattern(pat) : pat;
}

static void forbid_atomic_field_patterns(const Location& loc, ctype::PatternEnv* penv,
                                         const tt::RecordPatField& f) {
  // Pattern-matching under atomic record fields is not allowed.  We still
  // allow wildcard patterns, so that it is valid to list all record fields
  // exhaustively.
  if (f.label->lbl_atomic == AtomicFlag::Atomic && f.pat->pat_desc->kind != PK::Tpat_any) {
    Error e = err(loc, penv->env, EK::Atomic_in_pattern);
    e.lid = f.lid.txt;
    raise_error(e);  // log_or_raise
  }
}

static tt::Pattern* mkpat(const tt::PatternDesc* d, const Location& loc, TypeExpr* ty, env::t env,
                          const pt::Attributes& attrs, Slice<tt::PatExtraItem> extra = {}) {
  return make<tt::Pattern>(d, loc, extra, ty, env, attrs);
}
static Slice<tt::PatExtraItem> cons_extra(const tt::PatExtraItem& x, Slice<tt::PatExtraItem> l) {
  std::vector<tt::PatExtraItem> v{x};
  v.insert(v.end(), l.begin(), l.end());
  return slice(v);
}

static const tt::Pattern* type_pat_aux(TypePatState& tps, PC category,
                                       std::optional<ExistentialRestriction> no_existentials,
                                       ctype::PatternEnv* penv, const pt::Pattern* sp,
                                       TypeExpr* expected_ty);

// [type_pat] propagates the expected type, and unification may update the
// typing environment.  (-typing-recovery is never on in batch ocamlc.)
const tt::Pattern* type_pat(TypePatState& tps, PC category,
                            std::optional<ExistentialRestriction> no_existentials,
                            ctype::PatternEnv* penv, const pt::Pattern* sp, TypeExpr* expected_ty) {
  return builtin_attributes::warning_scope(sp->ppat_attributes, [&] {
    return type_pat_aux(tps, category, no_existentials, penv, sp, expected_ty);
  });
}

// Ast_helper constructors used by type_pat
static const pt::Pattern* mk_ppat(const pt::PatternDesc* d, const Location& loc) {
  return make<pt::Pattern>(d, loc, pt::LocationStack{}, pt::Attributes{});
}
static const pt::Pattern* ppat_char(char c, const Location& gloc) {
  pt::ConstantDesc cd{};
  cd.kind = pt::ConstantDesc::Kind::Pconst_char;
  cd.c = c;
  return mk_ppat(make<pt::Ppat_constant>(pt::Ppat_constant{{SK::Ppat_constant}, pt::Constant{cd, gloc}}), gloc);
}

static const tt::Pattern* type_pat_aux(TypePatState& tps, PC category,
                                       std::optional<ExistentialRestriction> no_existentials,
                                       ctype::PatternEnv* penv, const pt::Pattern* sp,
                                       TypeExpr* expected_ty) {
  if (penv->in_counterexample) throw std::logic_error("type_pat_aux");
  auto type_pat_ = [&](TypePatState& t, PC c, const pt::Pattern* p, TypeExpr* ty,
                       ctype::PatternEnv* pe = nullptr) {
    return type_pat(t, c, no_existentials, pe ? pe : penv, p, ty);
  };
  const Location& loc = sp->ppat_loc;
  auto solve_expected = [&](const tt::Pattern* x) {
    unify_pat(penv->env, x, ctype::instance(expected_ty), sp->ppat_desc);
    return x;
  };
  // record {general,value,computation} pattern
  auto crp = [&](const tt::Pattern* x) {
    return category == PC::Value ? typecore::rp(x) : typecore::rcp(x);
  };
  auto rvp = [&](const tt::Pattern* x) { return crp(pure(category, x)); };
  auto rcp = [&](const tt::Pattern* x) { return crp(only_impure(category, x)); };
  const pt::PatternDesc* d = sp->ppat_desc;
  switch (d->kind) {
    case SK::Ppat_any:
      return rvp(mkpat(make<tt::Tpat_any>(PK::Tpat_any), loc, ctype::instance(expected_ty), penv->env,
                       sp->ppat_attributes));
    case SK::Ppat_var: {
      const pt::StrLoc& name = as<pt::Ppat_var>(d)->name;
      TypeExpr* ty = ctype::instance(expected_ty);
      auto [id, uid] = enter_variable(tps, loc, name, ty, sp->ppat_attributes);
      return rvp(mkpat(make<tt::Tpat_var>(tt::Tpat_var{{PK::Tpat_var}, id, name, uid}), loc, ty,
                       penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_unpack: {
      auto* u = as<pt::Ppat_unpack>(d);
      const tt::PackageType* optyp = nullptr;
      TypeExpr* wrap_ty = nullptr;
      TypeExpr* e_ty = expected_ty;
      if (u->pack) {
        // Ast_helper.Typ.package ~loc ptyp
        auto* sty = make<pt::CoreType>(
            make<pt::Ptyp_package>(pt::Ptyp_package{{pt::CoreTypeDesc::Kind::Ptyp_package}, u->pack}),
            loc, pt::LocationStack{}, pt::Attributes{});
        SolvedConstraint sc = solve_Ppat_constraint(tps, loc, penv->env, sty, expected_ty);
        auto* tp = as<tt::Ttyp_package>(sc.cty->ctyp_desc);
        if (!tp) throw std::logic_error("type_pat: Ppat_unpack");  // Should not happen
        optyp = tp->pack;
        wrap_ty = sc.ty;
        e_ty = sc.expected_ty;
      }
      TypeExpr* t = ctype::instance(e_ty);
      tt::PatExtra xu{};
      xu.kind = tt::PatExtra::Kind::Tpat_unpack;
      xu.pack = optyp;
      const tt::Pattern* r;
      if (!u->name.txt.some) {
        r = rvp(mkpat(make<tt::Tpat_any>(PK::Tpat_any), sp->ppat_loc, t, penv->env, pt::Attributes{},
                      slice({tt::PatExtraItem{xu, u->name.loc, sp->ppat_attributes}})));
      } else {
        pt::StrLoc v{u->name.txt.v, u->name.loc};
        // We're able to pass ~is_module:true here without an error because
        // [Ppat_unpack] is a case identified by [may_contain_modules].
        auto [id, uid] = enter_variable(tps, loc, v, t, sp->ppat_attributes, true);
        r = rvp(mkpat(make<tt::Tpat_var>(tt::Tpat_var{{PK::Tpat_var}, id, v, uid}), sp->ppat_loc, t,
                      penv->env, pt::Attributes{},
                      slice({tt::PatExtraItem{xu, loc, sp->ppat_attributes}})));
      }
      if (!wrap_ty) return r;
      tt::Pattern* w = make<tt::Pattern>(*r);
      w->pat_type = wrap_ty;
      return w;
    }
    case SK::Ppat_alias: {
      auto* a = as<pt::Ppat_alias>(d);
      const tt::Pattern* q = type_pat_(tps, PC::Value, a->pat, expected_ty);
      TypeExpr* ty_var = solve_Ppat_alias(penv->env, q);
      auto [id, uid] = enter_variable(tps, a->name.loc, a->name, ty_var, sp->ppat_attributes, false, true);
      return rvp(mkpat(make<tt::Tpat_alias>(tt::Tpat_alias{{PK::Tpat_alias}, q, id, a->name, uid, ty_var}),
                       loc, q->pat_type, penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_constant: {
      tt::Constant cst = constant_or_raise(penv->env, loc, as<pt::Ppat_constant>(d)->c);
      return rvp(solve_expected(mkpat(make<tt::Tpat_constant>(tt::Tpat_constant{{PK::Tpat_constant}, cst}),
                                      loc, type_constant(cst), penv->env, sp->ppat_attributes)));
    }
    case SK::Ppat_interval: {
      auto* iv = as<pt::Ppat_interval>(d);
      auto get_bound = [&](const pt::Constant& c) -> char {
        if (c.pconst_desc.kind == pt::ConstantDesc::Kind::Pconst_char) return c.pconst_desc.c;
        raise_error(err(c.pconst_loc, penv->env, EK::Invalid_interval));
      };
      char c1 = get_bound(iv->c1);
      char c2 = get_bound(iv->c2);
      Location gloc = loc;
      gloc.loc_ghost = true;
      gloc = location::distinct_record(gloc);  // {loc with loc_ghost = true}
      std::function<const pt::Pattern*(unsigned char, unsigned char)> loop =
          [&](unsigned char a, unsigned char b) -> const pt::Pattern* {
        if (a == b) return ppat_char(static_cast<char>(a), gloc);
        // Pat.or_ ~loc:gloc (Pat.constant ..c1) (loop (c1+1) c2): right to left
        const pt::Pattern* rest = loop(static_cast<unsigned char>(a + 1), b);
        const pt::Pattern* first = ppat_char(static_cast<char>(a), gloc);
        return mk_ppat(make<pt::Ppat_or>(pt::Ppat_or{{SK::Ppat_or}, first, rest}), gloc);
      };
      unsigned char u1 = static_cast<unsigned char>(c1), u2 = static_cast<unsigned char>(c2);
      const pt::Pattern* p = u1 <= u2 ? loop(u1, u2) : loop(u2, u1);
      pt::Pattern* p2 = make<pt::Pattern>(*p);
      p2->ppat_loc = loc;
      return type_pat_(tps, category, p2, expected_ty);
      // TODO: record 'extra' to remember about interval
    }
    case SK::Ppat_tuple: {
      auto* t = as<pt::Ppat_tuple>(d);
      if (!(t->closed == ClosedFlag::Open || t->pl.size() >= 2))
        throw std::logic_error("type_pat: tuple arity");
      {  // Misc.repeated_label
        std::vector<std::string_view> seen;
        for (auto& e : t->pl) {
          if (!e.label.some) continue;
          if (std::find(seen.begin(), seen.end(), e.label.v) != seen.end()) {
            Error er = err(loc, penv->env, EK::Repeated_tuple_pat_label);
            er.name = std::string(e.label.v);
            raise_error(er);  // log_or_raise
          }
          seen.push_back(e.label.v);
        }
      }
      std::vector<pt::LabeledPattern> spl(t->pl.begin(), t->pl.end());
      std::vector<pt::LabeledPattern> args;
      auto* tu = as<Ttuple>(get_desc(ctype::expand_head(penv->env, expected_ty)));
      if (tu && is_principal(expected_ty)) {
        // If it's a principally-known tuple pattern, try to reorder
        args = reorder_pat(loc, penv, spl, t->closed, tu->elems, expected_ty);
      } else {
        // If not, it's not allowed to be open (partial)
        if (t->closed == ClosedFlag::Open) raise_error(err(loc, penv->env, EK::Partial_tuple_pattern_bad_type));
        args = spl;
      }
      std::vector<LabeledTy> expected_tys = solve_Ppat_tuple(loc, penv, args, expected_ty);
      std::vector<tt::LabeledPattern> pl;
      for (std::size_t k = 0; k < args.size(); ++k)
        pl.push_back({expected_tys[k].label, type_pat_(tps, PC::Value, args[k].pat, expected_tys[k].ty)});
      std::vector<LabeledTy> tys;
      for (auto& x : pl) tys.push_back({x.label, x.pat->pat_type});
      return rvp(mkpat(make<tt::Tpat_tuple>(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(pl)}), loc,
                       ctype::newty(ttuple(slice(tys))), penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_construct: {
      auto* c = as<pt::Ppat_construct>(d);
      const pt::LidLoc& lid = c->lid;
      std::optional<ExpectedTypePath> expected_type;
      VariantExtraction ve = extract_concrete_variant(penv->env, expected_ty);
      switch (ve.kind) {
        case VariantExtraction::Kind::Variant_type:
          expected_type = ExpectedTypePath{ve.p0, ve.p, is_principal(expected_ty)};
          break;
        case VariantExtraction::Kind::Maybe_a_variant_type: break;
        case VariantExtraction::Kind::Not_a_variant_type: {
          Error e = err(loc, penv->env, EK::Wrong_expected_kind);
          e.sort = wrong_kind_sort_of_constructor(lid.txt);
          e.ctx = WrongKindContext{true, std::nullopt};
          e.ty = expected_ty;
          raise_error(e);
        }
      }
      const ConstructorDescription* constr;
      {
        env::LookupAllCstrs candidates =
            env::lookup_all_constructors(true, lid.loc, env::ConstructorUsage::Pattern, lid.txt, penv->env);
        constr = wrap_disambiguate("This variant pattern is expected to have", mk_expected(expected_ty), [&] {
          return disambiguate_constructor(env::ConstructorUsage::Pattern, lid, penv->env, expected_type,
                                          candidates);
        });
      }
      if (no_existentials && !constr->cstr_existentials.empty()) {
        Error e = err(loc, penv->env, EK::Unexpected_existential);
        e.restriction = *no_existentials;
        e.name = std::string(constr->cstr_name);
        raise_error(e);
      }
      const pt::Pattern* sarg2 = nullptr;
      std::optional<ExistentialStyp> existential_styp;
      if (c->arg) {
        const pt::Pattern* ap = c->arg->pat;
        auto* pc = as<pt::Ppat_constraint>(ap->ppat_desc);
        if (pc && (!c->arg->vars.empty() || constr->cstr_arity > 1)) {
          sarg2 = pc->pat;
          existential_styp = ExistentialStyp{c->arg->vars, pc->ty};
        } else if (c->arg->vars.empty()) {
          sarg2 = ap;
        } else {
          raise_error(err(ap->ppat_loc, penv->env, EK::Missing_type_constraint));
        }
      }
      std::vector<const pt::Pattern*> sargs;
      if (sarg2) {
        auto* st = as<pt::Ppat_tuple>(sarg2->ppat_desc);
        if (st && (constr->cstr_arity > 1 || builtin_attributes::explicit_arity(sp->ppat_attributes))) {
          for (auto& e : st->pl) {
            if (e.label.some) raise_error(err(loc, penv->env, EK::Constructor_labeled_arg));
            sargs.push_back(e.pat);
          }
        } else if (sarg2->ppat_desc->kind == SK::Ppat_any && constr->cstr_arity == 0 &&
                   !existential_styp) {
          prerr_warning(sarg2->ppat_loc, WK::Wildcard_arg_to_constant_constr);
        } else if (sarg2->ppat_desc->kind == SK::Ppat_any && constr->cstr_arity > 1) {
          sargs.assign(static_cast<std::size_t>(constr->cstr_arity), sarg2);
        } else {
          sargs.push_back(sarg2);
        }
      }
      if (builtin_attributes::warn_on_literal_pattern(constr->cstr_attributes))
        for (const pt::Pattern* sp1 : sargs)
          if (has_literal_pattern(sp1)) {
            prerr_warning(sp1->ppat_loc, WK::Fragile_literal_pattern);
            break;
          }
      if (static_cast<long>(sargs.size()) != constr->cstr_arity) {
        Error e = err(loc, penv->env, EK::Constructor_arity_mismatch);
        e.lid = lid.txt;
        e.n1 = constr->cstr_arity;
        e.n2 = static_cast<long>(sargs.size());
        raise_error(e);
      }
      SolvedConstruct sc = solve_Ppat_construct(tps, penv, loc, constr, no_existentials,
                                                existential_styp ? &*existential_styp : nullptr,
                                                expected_ty);
      std::function<void(const pt::Pattern*)> check_non_escaping = [&](const pt::Pattern* p) {
        if (auto* o = as<pt::Ppat_or>(p->ppat_desc)) {
          check_non_escaping(o->p1);
          check_non_escaping(o->p2);
        } else if (auto* a = as<pt::Ppat_alias>(p->ppat_desc)) {
          check_non_escaping(a->pat);
        } else if (p->ppat_desc->kind == SK::Ppat_constraint) {
          raise_error(err(p->ppat_loc, penv->env, EK::Inlined_record_escape));  // log_or_raise
        }
      };
      if (constr->cstr_inlined) {
        for (auto* p : sargs) check_non_escaping(p);
        if (c->arg) check_non_escaping(c->arg->pat);
      }
      if (sargs.size() != sc.ty_args.size()) throw std::invalid_argument("List.map2");
      std::vector<const tt::Pattern*> args;
      for (std::size_t k = 0; k < sargs.size(); ++k)
        args.push_back(type_pat_(tps, PC::Value, sargs[k], sc.ty_args[k]));
      return rvp(mkpat(make<tt::Tpat_construct>(
                           tt::Tpat_construct{{PK::Tpat_construct}, lid, constr, slice(args), sc.existential_ctyp}),
                       loc, ctype::instance(expected_ty), penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_variant: {
      auto* v = as<pt::Ppat_variant>(d);
      if (v->label == parmatch::some_private_tag) throw std::logic_error("type_pat: private tag");
      bool constant = v->arg == nullptr;
      SolvedVariant sv = solve_Ppat_variant(loc, penv, v->label, constant, expected_ty);
      const tt::Pattern* arg = nullptr;
      // PR#6235: propagate type information
      if (v->arg && sv.arg_type.size() == 1) arg = type_pat_(tps, PC::Value, v->arg, sv.arg_type[0]);
      return rvp(mkpat(make<tt::Tpat_variant>(tt::Tpat_variant{{PK::Tpat_variant}, zborrow(v->label), arg,
                                                               make<tt::RowDescRef>(tt::RowDescRef{sv.row})}),
                       loc, sv.expected_ty, penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_record: {
      auto* r = as<pt::Ppat_record>(d);
      if (r->fields.empty()) throw std::logic_error("type_pat: empty record");
      std::optional<ExpectedTypePath> expected_type;
      TypeExpr* record_ty;
      RecordExtraction re = extract_concrete_record(penv->env, expected_ty);
      switch (re.kind) {
        case RecordExtraction::Kind::Record_type: {
          TypeExpr* ty = ctype::generic_instance(expected_ty);
          expected_type = ExpectedTypePath{re.p0, re.p, is_principal(expected_ty)};
          record_ty = ty;
          break;
        }
        case RecordExtraction::Kind::Maybe_a_record_type: record_ty = ctype::newvar(); break;
        case RecordExtraction::Kind::Not_a_record_type: {
          Error e = err(loc, penv->env, EK::Wrong_expected_kind);
          e.sort = WrongKindSort::Record;
          e.ctx = WrongKindContext{true, std::nullopt};
          e.ty = expected_ty;
          raise_error(e);
        }
      }
      std::vector<tt::RecordPatField> lbl_pat_list =
          wrap_disambiguate("This record pattern is expected to have", mk_expected(expected_ty), [&] {
            // type_label_a_list loc false env Projection type_label_pat expected_type
            std::vector<pt::LidLoc> lids;
            for (auto& f : r->fields) lids.push_back(f.first);
            std::vector<const LabelDescription*> lbls =
                disambiguate_lid_list(loc, false, penv->env, env::LabelUsage::Projection, expected_type, lids);
            struct Item {
              pt::LidLoc lid;
              const LabelDescription* label;
              const pt::Pattern* sarg;
            };
            std::vector<Item> items;
            for (std::size_t k = 0; k < lids.size(); ++k) items.push_back({lids[k], lbls[k], r->fields[k].second});
            // Invariant: records are sorted in the typed tree
            items = ocaml_list::stable_sort(
                [](const Item& a, const Item& b) {
                  return a.label->lbl_pos < b.label->lbl_pos ? -1 : a.label->lbl_pos > b.label->lbl_pos ? 1 : 0;
                },
                items);
            std::vector<tt::RecordPatField> out;
            for (auto& it : items) {
              TypeExpr* ty_arg = solve_Ppat_record_field(loc, penv, it.label, it.lid, record_ty);
              out.push_back({it.lid, it.label, type_pat_(tps, PC::Value, it.sarg, ty_arg)});
            }
            return out;
          });
      check_recordpat_labels(loc, lbl_pat_list, r->closed);
      for (auto& f : lbl_pat_list) forbid_atomic_field_patterns(loc, penv, f);
      const tt::Pattern* rp = mkpat(make<tt::Tpat_record>(tt::Tpat_record{{PK::Tpat_record}, slice(lbl_pat_list), r->closed}),
                                    loc, ctype::instance(record_ty), penv->env, sp->ppat_attributes);
      return rvp(solve_expected(rp));
    }
    case SK::Ppat_array: {
      auto [ty_elt, mut] = solve_Ppat_array(loc, penv, expected_ty);
      std::vector<const tt::Pattern*> pl;
      for (auto* p : as<pt::Ppat_array>(d)->pats) pl.push_back(type_pat_(tps, PC::Value, p, ty_elt));
      return rvp(mkpat(make<tt::Tpat_array>(tt::Tpat_array{{PK::Tpat_array}, mut, slice(pl)}), loc,
                       ctype::instance(expected_ty), penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_or: {
      auto* o = as<pt::Ppat_or>(d);
      // Reset pattern forces for just [tps2] because later we append [tps1]
      // and [tps2]'s pattern forces, and we don't want to duplicate [tps]'s
      // pattern forces.
      TypePatState tps1 = copy_type_pat_state(tps);
      TypePatState tps2 = copy_type_pat_state(tps);
      tps2.tps_pattern_force.clear();
      // Introduce a new level to avoid keeping nodes at intermediate levels
      const tt::PatternDesc* pat_desc = ctype::with_local_level_generalize([&]() -> const tt::PatternDesc* {
        // Introduce a new scope using with_local_level without generalizations
        struct R {
          env::t env1;
          const tt::Pattern* p1;
          env::t env2;
          const tt::Pattern* p2;
        };
        R r = ctype::with_local_level([&] {
          ctype::PatternEnv* penv1 = penv->copy(ctype::get_current_level());
          ctype::PatternEnv* penv2 = penv1->copy();
          const tt::Pattern* p1 = type_pat(tps1, category, no_existentials, penv1, o->p1, expected_ty);
          const tt::Pattern* p2 = type_pat(tps2, category, no_existentials, penv2, o->p2, expected_ty);
          return R{penv1->env, p1, penv2->env, p2};
        });
        const std::vector<PatternVariable>& p1_variables = tps1.tps_pattern_variables;
        const std::vector<PatternVariable>& p2_variables = tps2.tps_pattern_variables;
        // Make sure no variable with an ambiguous type gets added to the
        // environment.
        long outer_lev = ctype::get_current_level();
        for (auto& v : p1_variables) check_scope_escape(v.pv_loc, r.env1, outer_lev, v.pv_type);
        for (auto& v : p2_variables) check_scope_escape(v.pv_loc, r.env2, outer_lev, v.pv_type);
        std::vector<std::pair<Ident::t, Ident::t>> alpha_env =
            enter_orpat_variables(loc, penv->env, p1_variables, p2_variables);
        // Propagate the outcome of checking the or-pattern back to the
        // type_pat_state that the caller passed in.
        TypePatState src;
        src.tps_pattern_variables = tps1.tps_pattern_variables;
        // We want to propagate all pattern forces, regardless of which
        // branch they were found in.
        src.tps_pattern_force = tps2.tps_pattern_force;
        src.tps_pattern_force.insert(src.tps_pattern_force.end(), tps1.tps_pattern_force.begin(),
                                     tps1.tps_pattern_force.end());
        src.tps_module_variables = tps1.tps_module_variables;
        blit_type_pat_state(src, tps);
        const tt::Pattern* p2 = tt::alpha_pat(alpha_env, r.p2);
        return make<tt::Tpat_or>(tt::Tpat_or{{PK::Tpat_or}, r.p1, p2, nullptr});
      });
      return crp(mkpat(pat_desc, loc, ctype::instance(expected_ty), penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_lazy: {
      TypeExpr* nv = solve_Ppat_lazy(loc, penv, expected_ty);
      const tt::Pattern* p1 = type_pat_(tps, PC::Value, as<pt::Ppat_lazy>(d)->pat, nv);
      return rvp(mkpat(make<tt::Tpat_lazy>(tt::Tpat_lazy{{PK::Tpat_lazy}, p1}), loc,
                       ctype::instance(expected_ty), penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_constraint: {
      auto* c = as<pt::Ppat_constraint>(d);
      SolvedConstraint sc = solve_Ppat_constraint(tps, loc, penv->env, c->ty, expected_ty);
      const tt::Pattern* p = type_pat_(tps, category, c->pat, sc.expected_ty);
      tt::PatExtra x{};
      x.kind = tt::PatExtra::Kind::Tpat_constraint;
      x.cty = sc.cty;
      tt::Pattern* q = make<tt::Pattern>(*p);
      q->pat_type = sc.ty;
      q->pat_extra = cons_extra(tt::PatExtraItem{x, loc, sp->ppat_attributes}, p->pat_extra);
      return q;
    }
    case SK::Ppat_type: {
      const pt::LidLoc& lid = as<pt::Ppat_type>(d)->lid;
      auto [path, p] = build_or_pat(penv->env, loc, lid);
      tt::PatExtra x{};
      x.kind = tt::PatExtra::Kind::Tpat_type;
      x.path = path;
      x.lid = lid;
      tt::Pattern* q = make<tt::Pattern>(*p);
      q->pat_extra = cons_extra(tt::PatExtraItem{x, loc, sp->ppat_attributes}, p->pat_extra);
      return pure(category, solve_expected(q));
    }
    case SK::Ppat_open: {
      auto* o = as<pt::Ppat_open>(d);
      auto [path, new_env] = type_open(nullptr, OverrideFlag::Fresh, penv->env, sp->ppat_loc, o->lid);
      penv->set_env(new_env);
      const tt::Pattern* p = type_pat(tps, category, no_existentials, penv, o->pat, expected_ty);
      env::t new_env2 = penv->env;
      env::t closed_env = env::remove_last_open(path, new_env2);
      if (!closed_env) throw std::logic_error("type_pat: Ppat_open");
      penv->set_env(closed_env);
      tt::PatExtra x{};
      x.kind = tt::PatExtra::Kind::Tpat_open;
      x.path = path;
      x.lid = o->lid;
      x.env = new_env2;
      tt::Pattern* q = make<tt::Pattern>(*p);
      q->pat_extra = cons_extra(tt::PatExtraItem{x, loc, sp->ppat_attributes}, p->pat_extra);
      return q;
    }
    case SK::Ppat_exception: {
      const tt::Pattern* p_exn = type_pat_(tps, PC::Value, as<pt::Ppat_exception>(d)->pat, predef::type_exn());
      return rcp(mkpat(make<tt::Tpat_exception>(tt::Tpat_exception{{PK::Tpat_exception}, p_exn}),
                       sp->ppat_loc, expected_ty, penv->env, sp->ppat_attributes));
    }
    case SK::Ppat_effect:
      raise_error(err(loc, penv->env, EK::Effect_pattern_below_toplevel));
    case SK::Ppat_extension:
      throw ErrorForward(as<pt::Ppat_extension>(d)->ext);
  }
  throw std::logic_error("type_pat_aux");
}

// ---- entering pattern variables -----------------------------------------------------------
env::t add_pattern_variables(env::t env, const std::vector<PatternVariable>& pv, const env::CheckFn& check,
                             const env::CheckFn& check_as) {
  // List.fold_right
  for (auto it = pv.rbegin(); it != pv.rend(); ++it)
    env = env::add_value(it->pv_id,
                         make<ValueDescription>(ValueDescription{it->pv_type, ValueKind{}, it->pv_loc,
                                                                 pt::types_attributes(it->pv_attributes),
                                                                 it->pv_uid}),
                         env, it->pv_kind == PatternVariableKind::As_var ? check_as : check);
  return env;
}

// [add_let_pattern_vars] adds the pattern variables [pvs] to [env] for a let
// bindings.  Additionally binds any type vars used in the patterns.
env::t add_let_pattern_vars(env::t env, const std::vector<PatternVariable>& pvs,
                            const std::vector<std::function<void()>>& bind_type_vars_delayed) {
  env::t new_env = add_pattern_variables(env, pvs);
  for (auto& f : bind_type_vars_delayed) f();
  return new_env;
}

env::t add_module_variables(env::t env, const ModuleVariables& module_variables) {
  if (module_variables.kind != ModuleVariables::Kind::Modvars_allowed) return env;
  // List.fold_left over the list (head first)
  for (const ModuleVariable& mv : module_variables.module_variables) {
    env = typetexp::ty_var_env::with_local_scope([&] {
      // Ast_helper.(Mod.unpack ~loc:mv_loc (Exp.ident ~loc:mv_name.loc (mkloc (Lident name) mv_name.loc)))
      auto* eid = make<pt::Expression>(
          make<pt::Pexp_ident>(pt::Pexp_ident{{pt::ExpressionDesc::Kind::Pexp_ident},
                                              pt::LidLoc{Longident::lident(mv.mv_name.txt), mv.mv_name.loc}}),
          mv.mv_name.loc, pt::LocationStack{}, pt::Attributes{});
      auto* me = make<pt::ModuleExpr>(
          make<pt::Pmod_unpack>(pt::Pmod_unpack{{pt::ModuleExprDesc::Kind::Pmod_unpack}, eid}), mv.mv_loc,
          pt::Attributes{});
      auto [modl, md_shape] = type_module(env, me);
      (void)md_shape;
      ModulePresence pres = modl->mod_type->kind == ModuleType::Kind::Mty_alias ? ModulePresence::Mp_absent
                                                                               : ModulePresence::Mp_present;
      auto* md = make<ModuleDeclaration>(ModuleDeclaration{modl->mod_type, Attributes{}, mv.mv_name.loc, mv.mv_uid});
      return env::add_module_declaration(true, mv.mv_id, pres, md, env);
    });
  }
  return env;
}

TypePatternResult type_pattern(PC category, long lev, env::t env, const pt::Pattern* spat,
                               TypeExpr* expected_ty, const std::optional<ContinuationVar>& cont,
                               const ModulePatternsRestriction& allow_modules) {
  std::shared_ptr<TypePatState> tps = create_type_pat_state(cont, allow_modules);
  ctype::PatternEnv* new_penv = ctype::PatternEnv::make_(env, lev, false);
  const tt::Pattern* pat = type_pat(*tps, category, std::nullopt, new_penv, spat, expected_ty);
  return {pat, new_penv->env, tps->tps_pattern_force, tps->tps_pattern_variables, tps->tps_module_variables};
}

TypePatternListResult type_pattern_list(PC category, std::optional<ExistentialRestriction> no_existentials,
                                        env::t env,
                                        const std::vector<std::pair<pt::Attributes, const pt::Pattern*>>& spatl,
                                        const std::vector<TypeExpr*>& expected_tys,
                                        const ModulePatternsRestriction& allow_modules) {
  std::shared_ptr<TypePatState> tps = create_type_pat_state(std::nullopt, allow_modules);
  long equations_scope = ctype::get_current_level();
  ctype::PatternEnv* new_penv = ctype::PatternEnv::make_(env, equations_scope, false);
  if (spatl.size() != expected_tys.size()) throw std::invalid_argument("List.map2");
  std::vector<const tt::Pattern*> patl;
  for (std::size_t k = 0; k < spatl.size(); ++k)
    patl.push_back(builtin_attributes::warning_scope(
        spatl[k].first,
        [&] { return type_pat(*tps, category, no_existentials, new_penv, spatl[k].second, expected_tys[k]); },
        false));
  return {patl, new_penv->env, tps->tps_pattern_force, tps->tps_pattern_variables, tps->tps_module_variables};
}

// ---- counter-example checking (see typecore.ml) ------------------------------------------
struct NeedBacktrack {};
struct EmptyBranch {};
enum class AbortReason { Adds_constraints, Empty };

// Remember current typing state for backtracking
struct UnificationState {
  Snapshot snapshot;
  ctype::PatternEnv::State pattern_env;
};
static UnificationState save_state(ctype::PatternEnv* penv) {
  return {btype::snapshot(), penv->save()};
}
static void set_state(const UnificationState& s, ctype::PatternEnv* penv) {
  btype::backtrack(s.snapshot);
  penv->reset(s.pattern_env);
}

// Type variables allocated when searching for counter-examples should be
// discarded at the end of the search
template <class F>
static auto with_counterexample_pool(F&& f) -> decltype(f()) {
  std::optional<decltype(f())> r;
  btype::with_new_pool(ctype::get_current_level(), [&] { r.emplace(f()); });
  return *r;
}

using K = std::function<const tt::Pattern*(const tt::Pattern*)>;

// Find the first alternative in the tree of or-patterns for which [f] does
// not raise an error.  If all fail, the last error is propagated
static const tt::Pattern* find_valid_alternative(const K& f, const tt::Pattern* pat) {
  if (auto* o = as<tt::Tpat_or>(pat->pat_desc)) {
    try {
      return find_valid_alternative(f, o->p1);
    } catch (const EmptyBranch&) {
    } catch (const Error&) {
    }
    return find_valid_alternative(f, o->p2);
  }
  return f(pat);
}

static CounterExampleInfo no_explosion(CounterExampleInfo info) {
  info.explosion_fuel = 0;
  return info;
}
static CounterExampleInfo enter_nonsplit_or(CounterExampleInfo info) {
  // in Backtrack_or mode, or-patterns are always split
  if (info.mode == SplittingMode::Backtrack_or) throw std::logic_error("enter_nonsplit_or");
  info.inside_nonsplit_or = true;
  return info;
}

// map_fold_cont f xs k (left to right, in continuation-passing style)
template <class X, class Y>
static const tt::Pattern* map_fold_cont(
    const std::function<const tt::Pattern*(const X&, const std::function<const tt::Pattern*(Y)>&)>& f,
    const std::vector<X>& xs, const std::function<const tt::Pattern*(std::vector<Y>)>& k) {
  std::function<const tt::Pattern*(std::size_t, std::vector<Y>)> go =
      [&](std::size_t i, std::vector<Y> ys) -> const tt::Pattern* {
    if (i == xs.size()) return k(ys);
    return f(xs[i], [&, i, ys](Y y) {
      std::vector<Y> ys2 = ys;
      ys2.push_back(y);
      return go(i + 1, ys2);
    });
  };
  return go(0, {});
}

static tt::Constant untype_constant(const tt::Constant& c) { return c; }

static const tt::Pattern* check_counter_example_pat(const CounterExampleInfo& info,
                                                    ctype::PatternEnv* penv, TypePatState& tps,
                                                    const tt::Pattern* tp, TypeExpr* expected_ty,
                                                    const K& k) {
  if (!penv->in_counterexample) throw std::logic_error("check_counter_example_pat");
  if (is_Tpoly(expected_ty)) throw std::logic_error("check_counter_example_pat: poly");
  auto check_rec = [&](const tt::Pattern* p, TypeExpr* ty, const K& kk, const CounterExampleInfo* i = nullptr,
                       ctype::PatternEnv* pe = nullptr) {
    return check_counter_example_pat(i ? *i : info, pe ? pe : penv, tps, p, ty, kk);
  };
  const Location& loc = tp->pat_loc;
  auto solve_expected = [&](const tt::Pattern* x) {
    unify_pat_types_penv(x->pat_loc, penv, x->pat_type, ctype::instance(expected_ty));
    return x;
  };
  // "make pattern" and "make pattern then continue"
  auto mp = [&](const tt::PatternDesc* d, TypeExpr* pat_type = nullptr) {
    return mkpat(d, loc, ctype::instance(pat_type ? pat_type : expected_ty), penv->env, pt::Attributes{});
  };
  auto mkp = [&](const K& kk, const tt::PatternDesc* d, TypeExpr* pat_type = nullptr) {
    return kk(mp(d, pat_type));
  };
  bool must_backtrack_on_gadt = info.mode == SplittingMode::Refine_or && info.inside_nonsplit_or;
  const tt::PatternDesc* d = tp->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_var: {
      auto k2 = [&] { return mkp(k, d); };
      if (info.explosion_fuel <= 0) return k2();
      auto decrease = [&](int n) {
        CounterExampleInfo i = info;
        i.explosion_fuel -= n;
        return i;
      };
      std::vector<const tt::Pattern*> pats = parmatch::pats_of_type(penv->env, expected_ty);
      if (pats.empty()) throw EmptyBranch{};
      if (pats.size() == 1 && pats[0]->pat_desc->kind == PK::Tpat_any) return k2();
      if (pats.size() == 1) {
        CounterExampleInfo i = decrease(1);
        return check_rec(pats[0], expected_ty, k, &i);
      }
      if (must_backtrack_on_gadt) throw NeedBacktrack{};
      const tt::Pattern* t = pats[0];
      for (std::size_t n = 1; n < pats.size(); ++n) {
        tt::Pattern* q = make<tt::Pattern>(*t);
        q->pat_desc = make<tt::Tpat_or>(tt::Tpat_or{{PK::Tpat_or}, t, pats[n], nullptr});
        t = q;
      }
      CounterExampleInfo i = decrease(5);
      return check_rec(t, expected_ty, k, &i);
    }
    case PK::Tpat_alias: return check_rec(as<tt::Tpat_alias>(d)->pat, expected_ty, k);
    case PK::Tpat_constant: {
      // constant_or_raise env loc (Untypeast.constant cst): a typed
      // constant round-trips unchanged
      tt::Constant cst = untype_constant(as<tt::Tpat_constant>(d)->c);
      return k(solve_expected(mp(make<tt::Tpat_constant>(tt::Tpat_constant{{PK::Tpat_constant}, cst}),
                                 type_constant(cst))));
    }
    case PK::Tpat_tuple: {
      auto* t = as<tt::Tpat_tuple>(d);
      if (t->pats.size() < 2) throw std::logic_error("check_counter_example_pat: tuple");
      std::vector<pt::LabeledPattern> dummy;
      for (auto& x : t->pats) dummy.push_back({x.label, nullptr});
      std::vector<LabeledTy> expected_tys = solve_Ppat_tuple(loc, penv, dummy, expected_ty);
      struct Ann {
        OptStr label;
        const tt::Pattern* p;
        TypeExpr* t;
      };
      std::vector<Ann> ann;
      for (std::size_t n = 0; n < t->pats.size(); ++n) ann.push_back({t->pats[n].label, t->pats[n].pat, expected_tys[n].ty});
      return map_fold_cont<Ann, tt::LabeledPattern>(
          [&](const Ann& a, const std::function<const tt::Pattern*(tt::LabeledPattern)>& kk) {
            return check_rec(a.p, a.t, [&, a](const tt::Pattern* p) { return kk(tt::LabeledPattern{a.label, p}); });
          },
          ann,
          [&](std::vector<tt::LabeledPattern> pl) {
            std::vector<LabeledTy> tys;
            for (auto& x : pl) tys.push_back({x.label, x.pat->pat_type});
            return mkp(k, make<tt::Tpat_tuple>(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(pl)}),
                       ctype::newty(ttuple(slice(tys))));
          });
    }
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      if (c->cstr->cstr_generalized && must_backtrack_on_gadt) throw NeedBacktrack{};
      SolvedConstruct sc = solve_Ppat_construct(tps, penv, loc, c->cstr, std::nullopt, nullptr, expected_ty);
      if (c->args.size() != sc.ty_args.size()) throw std::invalid_argument("List.combine");
      std::vector<std::pair<const tt::Pattern*, TypeExpr*>> pts;
      for (std::size_t n = 0; n < c->args.size(); ++n) pts.push_back({c->args[n], sc.ty_args[n]});
      return map_fold_cont<std::pair<const tt::Pattern*, TypeExpr*>, const tt::Pattern*>(
          [&](const std::pair<const tt::Pattern*, TypeExpr*>& pt_,
              const std::function<const tt::Pattern*(const tt::Pattern*)>& kk) {
            return check_rec(pt_.first, pt_.second, kk);
          },
          pts,
          [&](std::vector<const tt::Pattern*> args) {
            return mkp(k, make<tt::Tpat_construct>(
                              tt::Tpat_construct{{PK::Tpat_construct}, c->lid, c->cstr, slice(args), sc.existential_ctyp}));
          });
    }
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      bool constant = v->arg == nullptr;
      SolvedVariant sv = solve_Ppat_variant(loc, penv, v->label, constant, expected_ty);
      auto k2 = [&](const tt::Pattern* arg) {
        return mkp(k, make<tt::Tpat_variant>(tt::Tpat_variant{{PK::Tpat_variant}, v->label, arg,
                                                              make<tt::RowDescRef>(tt::RowDescRef{sv.row})}),
                   sv.expected_ty);
      };
      // PR#6235: propagate type information
      if (v->arg && sv.arg_type.size() == 1) return check_rec(v->arg, sv.arg_type[0], k2);
      return k2(nullptr);
    }
    case PK::Tpat_record: {
      auto* r = as<tt::Tpat_record>(d);
      TypeExpr* record_ty = ctype::generic_instance(expected_ty);
      std::vector<tt::RecordPatField> fields(r->fields.begin(), r->fields.end());
      return map_fold_cont<tt::RecordPatField, tt::RecordPatField>(
          [&](const tt::RecordPatField& f, const std::function<const tt::Pattern*(tt::RecordPatField)>& kk) {
            TypeExpr* ty_arg = solve_Ppat_record_field(loc, penv, f.label, f.lid, record_ty);
            return check_rec(f.pat, ty_arg, [&, f](const tt::Pattern* arg) {
              return kk(tt::RecordPatField{f.lid, f.label, arg});
            });
          },
          fields,
          [&](std::vector<tt::RecordPatField> fs) {
            return mkp(k, make<tt::Tpat_record>(tt::Tpat_record{{PK::Tpat_record}, slice(fs), r->closed}));
          });
    }
    case PK::Tpat_array: {
      auto* a = as<tt::Tpat_array>(d);
      auto [ty_elt, _] = solve_Ppat_array(loc, penv, expected_ty);
      std::vector<const tt::Pattern*> tpl(a->pats.begin(), a->pats.end());
      return map_fold_cont<const tt::Pattern*, const tt::Pattern*>(
          [&](const tt::Pattern* const& p, const std::function<const tt::Pattern*(const tt::Pattern*)>& kk) {
            return check_rec(p, ty_elt, kk);
          },
          tpl,
          [&](std::vector<const tt::Pattern*> pl) {
            return mkp(k, make<tt::Tpat_array>(tt::Tpat_array{{PK::Tpat_array}, a->mut, slice(pl)}));
          });
    }
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      // We are in counter-example mode, but try to avoid backtracking
      bool must_split = info.mode == SplittingMode::Backtrack_or;
      UnificationState state = save_state(penv);
      auto split_or = [&](const tt::Pattern* t) {
        auto type_alternative = [&](const tt::Pattern* pat) {
          set_state(state, penv);
          // Type nodes should be discarded as soon as possible
          return with_counterexample_pool([&] { return check_rec(pat, expected_ty, k); });
        };
        return find_valid_alternative(type_alternative, t);
      };
      if (must_split) return split_or(tp);
      struct Res {
        bool ok;
        const tt::Pattern* p;
        AbortReason reason;
      };
      auto check_rec_result = [&](ctype::PatternEnv* pe, const tt::Pattern* t) -> Res {
        CounterExampleInfo i = enter_nonsplit_or(info);
        try {
          return {true, check_rec(t, expected_ty, [](const tt::Pattern* x) { return x; }, &i, pe), {}};
        } catch (const NeedBacktrack&) {
          return {false, nullptr, AbortReason::Adds_constraints};
        } catch (const EmptyBranch&) {
          return {false, nullptr, AbortReason::Empty};
        }
      };
      Res p1 = check_rec_result(penv->copy(), o->p1);
      Res p2 = check_rec_result(penv->copy(), o->p2);
      if (!p1.ok && !p2.ok) {
        if (p1.reason == AbortReason::Empty && p2.reason == AbortReason::Empty) throw EmptyBranch{};
        bool inside_nonsplit_or = info.mode == SplittingMode::Refine_or && info.inside_nonsplit_or;
        if (inside_nonsplit_or) throw NeedBacktrack{};
        return split_or(tp);
      }
      if (p1.ok && !p2.ok) return k(p1.p);
      if (!p1.ok && p2.ok) return k(p2.p);
      return mkp(k, make<tt::Tpat_or>(tt::Tpat_or{{PK::Tpat_or}, p1.p, p2.p, nullptr}));
    }
    case PK::Tpat_lazy: {
      TypeExpr* nv = solve_Ppat_lazy(loc, penv, expected_ty);
      // do not explode under lazy: PR#7421
      CounterExampleInfo i = no_explosion(info);
      return check_rec(as<tt::Tpat_lazy>(d)->pat, nv, [&](const tt::Pattern* p1) {
        return mkp(k, make<tt::Tpat_lazy>(tt::Tpat_lazy{{PK::Tpat_lazy}, p1}));
      }, &i);
    }
    default: throw std::logic_error("check_counter_example_pat: computation pattern");
  }
}

static const tt::Pattern* check_counter_example_pat_top(const CounterExampleInfo& counter_example_args,
                                                        ctype::PatternEnv* penv, const tt::Pattern* tp,
                                                        TypeExpr* expected_ty0) {
  // [check_counter_example_pat] doesn't use [type_pat_state] in an
  // interesting way, so we can just ignore module patterns.
  std::shared_ptr<TypePatState> tps = create_type_pat_state(
      std::nullopt, ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_ignored});
  TypeExpr* expected_ty = ctype::with_level(generic_level, [&] { return ctype::maybe_instance_poly(expected_ty0); });
  return ctype::wrap_trace_gadt_instances(
      penv->env,
      [&] {
        return check_counter_example_pat(counter_example_args, penv, *tps, tp, expected_ty,
                                         [](const tt::Pattern* x) { return x; });
      },
      true);
}

// this function is passed to Partial.parmatch to type check gadt
// nonexhaustiveness
const tt::Pattern* partial_pred(long lev, SplittingMode splitting_mode, int explode, env::t env,
                                TypeExpr* expected_ty, const tt::Pattern* p) {
  ctype::PatternEnv* penv = ctype::PatternEnv::make_(env, lev, true);
  UnificationState state = save_state(penv);
  CounterExampleInfo counter_example_args{explode, splitting_mode, false};
  try {
    // Here we disable recovery (see typecore.ml)
    const tt::Pattern* typed_p = with_counterexample_pool(
        [&] { return check_counter_example_pat_top(counter_example_args, penv, p, expected_ty); });
    set_state(state, penv);
    // types are invalidated but we don't need them here
    return typed_p;
  } catch (const Error&) {
    set_state(state, penv);
    return nullptr;
  } catch (const EmptyBranch&) {
    set_state(state, penv);
    return nullptr;
  }
}

tt::Partial check_partial(long lev, env::t env, TypeExpr* expected_ty, const Location& loc,
                          const std::vector<parmatch::TypedCase>& cases) {
  int explode = cases.size() == 1 ? 5 : 0;
  return parmatch::check_partial(
      [=](const tt::Pattern* p) {
        return partial_pred(lev, SplittingMode::Refine_or, explode, env, expected_ty, p);
      },
      loc, cases);
}

void check_unused(long lev, env::t env, TypeExpr* expected_ty,
                  const std::vector<parmatch::TypedCase>& cases) {
  parmatch::check_unused(
      [=](bool refute, const tt::Pattern* pat) -> const tt::Pattern* {
        const tt::Pattern* r = partial_pred(lev, SplittingMode::Backtrack_or, 5, env, expected_ty, pat);
        if (r && refute) {
          Error e = err(pat->pat_loc, env, EK::Unrefuted_pattern);
          e.pat = r;
          raise_error(e);  // log_or_raise
        }
        return r;
      },
      cases);
}

// ---- delayed checks, executed after typing the whole compilation unit ----------------------
// (f, Warnings.backup ()) pairs, oldest first (typecore.ml conses them
// and runs List.rev of the list)
static std::vector<std::pair<std::function<void()>, warnings::State>> delayed_checks;
void reset_delayed_checks() { delayed_checks.clear(); }
void add_delayed_check(std::function<void()> f) {
  delayed_checks.emplace_back(std::move(f), warnings::backup());
}
void force_delayed_checks() {
  // checks may change type levels
  Snapshot snap = btype::snapshot();
  warnings::State w_old = warnings::backup();
  std::vector<std::pair<std::function<void()>, warnings::State>> l = std::move(delayed_checks);
  delayed_checks.clear();
  for (auto& [f, w] : l) {
    warnings::restore(w);
    f();
  }
  warnings::restore(w_old);
  reset_delayed_checks();
  btype::backtrack(snap);
}

}  // namespace cppcaml::typing::typecore
