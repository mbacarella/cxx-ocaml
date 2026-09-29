// Port of typing/typecore.ml, part 4: type_exp / type_expect / type_expect_
// (every Pexp_* case), and the constraint, coercion, newtype, identifier and
// function-splitting helpers that follow it ("type_exp" to
// "split_function_mty").
#include "cppcaml/typing/cmt_format.hpp"
#include <unordered_set>

#include "ast_helper.hpp"
#include "cppcaml/typing/subst.hpp"
#include "typecore_exp.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using XK = tt::ExpressionDesc::Kind;
using SXK = pt::ExpressionDesc::Kind;
using SPK = pt::PatternDesc::Kind;
namespace ah = ast_helper;

static tt::Expression* mk(const tt::ExpressionDesc* d, const Location& loc, TypeExpr* ty, env::t env,
                          pt::Attributes attrs) {
  return make<tt::Expression>(d, loc, Slice<tt::ExpExtraItem>{}, ty, env, attrs);
}
template <class D>
static const D* mkd(D d) {
  return make<D>(std::move(d));
}
static Slice<tt::ExpExtraItem> cons_extra(const tt::ExpExtra& e, const Location& loc, pt::Attributes attrs,
                                          Slice<tt::ExpExtraItem> rest) {
  std::vector<tt::ExpExtraItem> v{{e, loc, attrs}};
  v.insert(v.end(), rest.begin(), rest.end());
  return slice(v);
}
static Longident::t self_lid(std::string_view cl_num) {
  return Longident::lident(zborrow(std::string("self-") + std::string(cl_num)));
}

const tt::Expression* type_exp_r(Recarg recarg, env::t env, const pt::Expression* sexp) {
  // We now delegate everything to type_expect
  return type_expect_r(recarg, env, sexp, mk_expected(ctype::newvar()));
}
const tt::Expression* type_exp(env::t env, const pt::Expression* sexp) {
  return type_exp_r(Recarg::Rejected, env, sexp);
}
const tt::Expression* type_expect(env::t env, const pt::Expression* sexp, const TypeExpected& ty) {
  return type_expect_r(Recarg::Rejected, env, sexp, ty);
}

static const tt::Expression* type_expect_(Recarg recarg, env::t env, const pt::Expression* sexp,
                                          const TypeExpected& ty_expected_explained);
static const tt::Expression* type_expect_w(Recarg recarg, env::t env, const pt::Expression* sexp,
                                           const TypeExpected& ty_expected_explained);

// Typing of an expression with an expected type.  (Typing_recovery_state
// saves the partial typedtree for the cmt file; typing recovery is off.)
const tt::Expression* type_expect_r(Recarg recarg, env::t env, const pt::Expression* sexp,
                                    const TypeExpected& ty_expected_explained) {
  return cmt_format::with_saved_types([&] { return type_expect_w(recarg, env, sexp, ty_expected_explained); },
                                      [](const tt::Expression* e) {
                                        return cmt_format::BinaryPart{
                                            cmt_format::BinaryPart::Kind::Partial_expression, false, e};
                                      });
}
static const tt::Expression* type_expect_w(Recarg recarg, env::t env, const pt::Expression* sexp,
                                           const TypeExpected& ty_expected_explained) {
  return builtin_attributes::warning_scope(sexp->pexp_attributes, [&] {
    return type_expect_(recarg, env, sexp, ty_expected_explained);
  });
}

static pt::Expression* with_desc(const pt::Expression* sexp, const pt::ExpressionDesc* d) {
  pt::Expression* e = make<pt::Expression>(*sexp);
  e->pexp_desc = d;
  return e;
}

// the payload `PStr [{pstr_desc = Pstr_eval (e, _)}]`
static const pt::Expression* single_eval_payload(const pt::Payload& p) {
  if (p.kind != pt::Payload::Kind::PStr || p.str.size() != 1) return nullptr;
  auto* ev = as<pt::Pstr_eval>(p.str[0]->pstr_desc);
  return ev ? ev->exp : nullptr;
}

// split_cases: the effect cases `effect p1, p2` apart from the others
static void split_effect_cases(Slice<const pt::Case*> caselist, std::vector<const pt::Case*>& other,
                               std::vector<const pt::Case*>& effc, std::vector<const pt::Pattern*>& conts) {
  for (auto* c : caselist) {
    if (auto* e = as<pt::Ppat_effect>(c->pc_lhs->ppat_desc)) {
      pt::Case* c2 = make<pt::Case>(*c);
      c2->pc_lhs = e->eff;
      effc.push_back(c2);
      conts.push_back(e->cont);
    } else {
      other.push_back(c);
    }
  }
}

static const tt::Expression* type_expect_(Recarg recarg, env::t env, const pt::Expression* sexp,
                                          const TypeExpected& ty_expected_explained) {
  TypeExpr* ty_expected = ty_expected_explained.ty;
  Explanation explanation = ty_expected_explained.explanation;
  const Location& loc = sexp->pexp_loc;
  // Unify the result with [ty_expected], enforcing the current level
  auto rue = [&](tt::Expression* exp) -> const tt::Expression* {
    with_explanation(explanation, [&] {
      TypeExpr* ity = ctype::instance(ty_expected);
      unify_exp(sexp, env, re(exp), ity);
    });
    return exp;
  };
  const pt::ExpressionDesc* d = sexp->pexp_desc;
  switch (d->kind) {
    case SXK::Pexp_ident: {
      const pt::LidLoc& lid = as<pt::Pexp_ident>(d)->lid;
      auto [path, desc] = type_ident(env, recarg, lid);
      const tt::ExpressionDesc* exp_desc;
      switch (desc->val_kind.kind) {
        case ValueKind::Kind::Val_ivar: {
          Path::t self_path = env::find_value_by_name(self_lid(desc->val_kind.ivar_name), env).first;
          if (lid.txt->kind != Longident::Kind::Lident) throw std::logic_error("type_expect_: ivar");
          exp_desc = mkd(tt::Texp_instvar{{XK::Texp_instvar}, self_path, path, pt::StrLoc{lid.txt->s, lid.loc}});
          break;
        }
        case ValueKind::Kind::Val_self: {
          Path::t p = env::find_value_by_name(self_lid(desc->val_kind.cl_num), env).first;
          exp_desc = mkd(tt::Texp_ident{{XK::Texp_ident}, p, lid, desc});
          break;
        }
        default: exp_desc = mkd(tt::Texp_ident{{XK::Texp_ident}, path, lid, desc}); break;
      }
      return rue(mk(exp_desc, loc, ctype::instance(desc->val_type), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_constant: {
      const pt::Constant& c = as<pt::Pexp_constant>(d)->c;
      if (c.pconst_desc.kind == pt::ConstantDesc::Kind::Pconst_string) {
        tt::Constant cst = constant_or_raise(env, loc, c);
        // Terrible hack for format strings
        TypeExpr* ty_exp = ctype::expand_head(env, protect_expansion(env, ty_expected));
        Path::t fmt6_path =
            Path::pdot(Path::pident(Ident::create_persistent(OCAML_LIT("CamlinternalFormatBasics"))), "format6");
        bool is_format = false;
        if (auto* tc = as<Tconstr>(get_desc(ty_exp)); tc && path::same(tc->path, fmt6_path)) {
          if (clflags::principal && get_level(ty_exp) != generic_level)
            prerr_warning(loc, not_principal("this coercion to format6"));
          is_format = true;
        }
        if (is_format) {
          const pt::Expression* fp = type_format(loc, c.pconst_desc.s, env);
          pt::Expression* format_parsetree = make<pt::Expression>(*fp);
          format_parsetree->pexp_loc = sexp->pexp_loc;
          return type_expect(env, format_parsetree, ty_expected_explained);
        }
        return rue(mk(mkd(tt::Texp_constant{{XK::Texp_constant}, cst}), loc, ctype::instance(predef::type_string()),
                      env, sexp->pexp_attributes));
      }
      tt::Constant cst = constant_or_raise(env, loc, c);
      return rue(mk(mkd(tt::Texp_constant{{XK::Texp_constant}, cst}), loc, type_constant(cst), env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_let: {
      auto* l = as<pt::Pexp_let>(d);
      if (l->rec == RecFlag::Nonrecursive && l->vbs.size() == 1 && l->vbs[0]->pvb_attributes.empty() &&
          turn_let_into_match(l->vbs[0]->pvb_pat)) {
        // TODO: allow non-empty attributes? (typecore.ml)
        const pt::ValueBinding* vb = l->vbs[0];
        const pt::Expression* sval = vb_exp_constraint(vb);
        std::vector<const pt::Case*> cases{make<pt::Case>(vb->pvb_pat, nullptr, l->body)};
        return type_expect(env, with_desc(sexp, mkd(pt::Pexp_match{{SXK::Pexp_match}, sval, slice(cases)})),
                           ty_expected_explained);
      }
      ExistentialRestriction existential_context = l->rec == RecFlag::Recursive ? ExistentialRestriction::In_rec
                                                   : l->vbs.size() > 1            ? ExistentialRestriction::In_group
                                                                                  : ExistentialRestriction::With_attributes;
      bool may_contain_modules_ = false;
      for (auto* pvb : l->vbs)
        if (may_contain_modules(pvb->pvb_pat)) may_contain_modules_ = true;
      long outer_level = ctype::get_current_level();
      struct R {
        Slice<const tt::ValueBinding*> pel;
        const tt::Expression* body;
        env::t new_env;
      };
      // If the patterns contain module unpacks, the types of the let body or
      // bound expressions may mention types introduced by those unpacks: check
      // for scope escape via both pathways (body, bound expressions).
      R r = ctype::with_local_level_generalize_if(
          may_contain_modules_,
          [&] {
            ModulePatternsRestriction allow_modules{ModulePatternsRestriction::Kind::Modules_rejected};
            if (may_contain_modules_)
              allow_modules = ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_allowed,
                                                        static_cast<int>(ctype::create_scope())};
            auto [pel, new_env] = type_let(existential_context, env, l->rec, l->vbs, allow_modules);
            const tt::Expression* b = type_expect(new_env, l->body, ty_expected_explained);
            if (l->rec == RecFlag::Recursive) pel = annotate_recursive_bindings(env, pel);
            // The "bound expressions" component of the scope escape check,
            // relevant only for recursive module definitions.
            if (l->rec == RecFlag::Recursive && may_contain_modules_) {
              for (auto* vb : pel) {
                // [type_let] already generalized bound expressions' types
                // in-place: take an instance before checking scope escape at
                // the outer level.
                const tt::Expression* bound_exp = vb->vb_expr;
                TypeExpr* bound_exp_type = ctype::instance(bound_exp->exp_type);
                Location loc2 = proper_exp_loc(bound_exp);
                TypeExpr* outer_var = ctype::newvar2(outer_level);
                unify_exp_types(loc2, new_env, bound_exp_type, outer_var);
              }
            }
            return R{pel, b, new_env};
          },
          [&](const R& r) {
            // The "body" component of the scope escape check.
            TypeExpr* v = ctype::newvar();
            unify_exp(sexp, r.new_env, r.body, v);
          });
      Slice<const tt::ValueBinding*> pat_exp_list = r.pel;
      const tt::Expression* body = r.body;
      return re(mk(mkd(tt::Texp_let{{XK::Texp_let}, l->rec, pat_exp_list, body}), loc, body->exp_type, env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_function: {
      auto* f = as<pt::Pexp_function>(d);
      InFunction in_function{ty_expected_explained, loc};
      TypeFunctionResult r = type_function(env, f->params, f->constraint, f->body, ty_expected, true, in_function);
      if (r.contains_gadt) enforce_syntactic_arity(loc, env, r.exp_type, r.params, r.body);
      std::vector<const tt::FunctionParam*> params;
      for (auto& p : r.params) params.push_back(p.param);
      std::vector<tt::ExpExtraItem> extra;
      for (auto& nt : r.newtypes) {
        tt::ExpExtra x{tt::ExpExtra::Kind::Texp_newtype};
        x.name = nt.txt;
        extra.push_back({x, nt.loc, {}});
      }
      tt::Expression* e =
          mk(mkd(tt::Texp_function{{XK::Texp_function}, slice(params), r.body}), loc, r.exp_type, env,
             sexp->pexp_attributes);
      e->exp_extra = slice(extra);
      return re(e);
    }
    case SXK::Pexp_apply: {
      auto* a = as<pt::Pexp_apply>(d);
      if (a->args.empty()) throw std::logic_error("type_expect_: Pexp_apply []");
      long outer_level = ctype::get_current_level();
      // one more level for warning on non-returning functions
      return ctype::with_local_level_generalize([&]() -> const tt::Expression* {
        auto type_sfunct = [&](const pt::Expression* sfunct) {
          const tt::Expression* funct =
              ctype::with_local_level_generalize_structure_if_principal([&] { return type_exp(env, sfunct); });
          lower_args(outer_level, env, funct->exp_type);
          return funct;
        };
        std::vector<std::pair<ArgLabel, const pt::Expression*>> sargs;
        for (auto& x : a->args) sargs.push_back({x.label, x.exp});
        const tt::Expression* funct = type_sfunct(a->fn);
        auto nolabel = [](const ArgLabel& l) { return l.kind == ArgLabel::Kind::Nolabel; };
        if (auto* i = as<tt::Texp_ident>(funct->exp_desc);
            i && i->vd->val_kind.kind == ValueKind::Kind::Val_prim && sargs.size() == 2 &&
            nolabel(sargs[0].first) && nolabel(sargs[1].first)) {
          std::string_view name = i->vd->val_kind.prim->prim_name;
          if (name == "%revapply" && is_inferred(sargs[1].second) &&
              check_apply_prim_type(ApplyPrim::Revapply, i->vd->val_type)) {
            funct = type_sfunct(sargs[1].second);
            sargs = {sargs[0]};
          } else if (name == "%apply" && check_apply_prim_type(ApplyPrim::Apply, i->vd->val_type)) {
            funct = type_sfunct(sargs[0].second);
            sargs = {sargs[1]};
          }
        }
        auto [args, ty_res] = type_application(env, loc, funct, sargs);
        return rue(mk(mkd(tt::Texp_apply{{XK::Texp_apply}, funct, args}), loc, ty_res, env, sexp->pexp_attributes));
      });
    }
    case SXK::Pexp_match: {
      auto* m = as<pt::Pexp_match>(d);
      const tt::Expression* arg = ctype::with_local_level_generalize(
          [&] { return type_exp(env, m->exp); }, [&](const tt::Expression* e) { may_lower_contravariant(env, e); });
      std::vector<const pt::Case*> val_caselist, eff_caselist;
      std::vector<const pt::Pattern*> eff_conts;
      split_effect_cases(m->cases, val_caselist, eff_caselist, eff_conts);
      if (val_caselist.empty() && !eff_caselist.empty()) raise_error(err(loc, env, EK::No_value_clauses));
      auto [val_cases, partial] = type_cases(tt::PatternCategory::Computation, env, arg->exp_type,
                                             ty_expected_explained, nullptr, true, loc, slice(val_caselist));
      Slice<const tt::Case*> eff_cases;
      if (!eff_caselist.empty())
        eff_cases = type_effect_cases(tt::PatternCategory::Value, env, ty_expected_explained, loc,
                                      slice(eff_caselist), eff_conts);
      bool all = true;
      for (auto* c : val_cases)
        if (!pattern_needs_partial_application_check(c->c_lhs)) all = false;
      if (all) check_partial_application(false, arg);
      return re(mk(mkd(tt::Texp_match{{XK::Texp_match}, arg, val_cases, eff_cases, partial}), loc,
                ctype::instance(ty_expected), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_try: {
      auto* t = as<pt::Pexp_try>(d);
      const tt::Expression* body = type_expect(env, t->exp, ty_expected_explained);
      std::vector<const pt::Case*> exn_caselist, eff_caselist;
      std::vector<const pt::Pattern*> eff_conts;
      split_effect_cases(t->cases, exn_caselist, eff_caselist, eff_conts);
      auto exn_cases = type_cases(tt::PatternCategory::Value, env, predef::type_exn(), ty_expected_explained,
                                  nullptr, false, loc, slice(exn_caselist))
                           .first;
      Slice<const tt::Case*> eff_cases;
      if (!eff_caselist.empty())
        eff_cases = type_effect_cases(tt::PatternCategory::Value, env, ty_expected_explained, loc,
                                      slice(eff_caselist), eff_conts);
      return re(mk(mkd(tt::Texp_try{{XK::Texp_try}, body, exn_cases, eff_cases}), loc, body->exp_type, env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_tuple: {
      Slice<pt::LabeledExpression> sexpl = as<pt::Pexp_tuple>(d)->el;
      if (sexpl.size() < 2) throw std::logic_error("type_expect_: Pexp_tuple");
      {  // Misc.repeated_label
        std::vector<std::string_view> seen;
        for (auto& x : sexpl) {
          if (!x.label.some) continue;
          if (std::find(seen.begin(), seen.end(), x.label.v) != seen.end()) {
            Error e = err(loc, env, EK::Repeated_tuple_exp_label);
            e.name = std::string(x.label.v);
            raise_error(e);
          }
          seen.push_back(x.label.v);
        }
      }
      std::vector<LabeledTy> labeled_subtypes;
      for (auto& x : sexpl) labeled_subtypes.push_back({x.label, newgenvar()});
      TypeExpr* to_unify = newgenty(ttuple(slice(labeled_subtypes)));
      with_explanation(explanation, [&] {
        TypeExpr* gi = ctype::generic_instance(ty_expected);
        unify_exp_types(loc, env, to_unify, gi);
      });
      std::vector<tt::LabeledExpression> expl;
      std::vector<LabeledTy> tys;
      for (std::size_t k = 0; k < sexpl.size(); ++k) {
        const tt::Expression* e = type_expect(env, sexpl[k].exp, mk_expected(labeled_subtypes[k].ty));
        expl.push_back({sexpl[k].label, e});
      }
      for (auto& x : expl) tys.push_back({x.label, x.exp->exp_type});
      // Keep sharing
      return re(mk(mkd(tt::Texp_tuple{{XK::Texp_tuple}, slice(expl)}), loc, ctype::newty(ttuple(slice(tys))), env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_construct: {
      auto* c = as<pt::Pexp_construct>(d);
      return type_construct(env, sexp, c->lid, c->arg, ty_expected_explained);
    }
    case SXK::Pexp_variant: {
      auto* v = as<pt::Pexp_variant>(d);
      std::string_view l = v->label;
      // Keep sharing
      TypeExpr* ty_expected1 = protect_expansion(env, ty_expected);
      TypeExpr* ty_expected0 = ctype::instance(ty_expected);
      if (v->arg) {
        // (sarg, get_desc (expand_head env ty_expected1), get_desc (expand_head env ty_expected0)):
        // a `match` scrutinee tuple, evaluated left to right (Translcore binds its components in order)
        TypeExpr* e1 = ctype::expand_head(env, ty_expected1);
        TypeExpr* e0 = ctype::expand_head(env, ty_expected0);
        auto* tv = as<Tvariant>(get_desc(e1));
        auto* tv0 = as<Tvariant>(get_desc(e0));
        if (tv && tv0) {
          RowFieldView f = row_field_repr(get_row_field(l, tv->row));
          RowFieldView f0 = row_field_repr(get_row_field(l, tv0->row));
          if (f.kind == RowFieldView::Kind::Rpresent && f.present && f0.kind == RowFieldView::Kind::Rpresent &&
              f0.present) {
            const tt::Expression* arg = type_argument(env, v->arg, f.present, f0.present);
            return re(mk(mkd(tt::Texp_variant{{XK::Texp_variant}, l, arg}), loc, ty_expected0, env,
                      sexp->pexp_attributes));
          }
        }
      } else {
        ctype::expand_head(env, ty_expected1);
        ctype::expand_head(env, ty_expected0);
      }
      // with Exit
      const tt::Expression* arg = v->arg ? type_exp(env, v->arg) : nullptr;
      TypeExpr* arg_type = arg ? arg->exp_type : nullptr;
      std::vector<RowFieldEntry> fields{{l, rf_present(arg_type)}};
      const RowDesc* row = create_row(slice(fields), ctype::newvar(), false, nullptr, nullptr);
      return rue(mk(mkd(tt::Texp_variant{{XK::Texp_variant}, l, arg}), loc, ctype::newty(tvariant(row)), env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_record: {
      auto* r = as<pt::Pexp_record>(d);
      Slice<std::pair<pt::LidLoc, const pt::Expression*>> lid_sexp_list = r->fields;
      const pt::Expression* opt_sexp = r->base;
      if (lid_sexp_list.empty()) throw std::logic_error("type_expect_: empty record");
      const tt::Expression* opt_exp = nullptr;
      if (opt_sexp)
        opt_exp = ctype::with_local_level_generalize_structure_if_principal(
            [&] { return type_exp_r(recarg, env, opt_sexp); });
      std::optional<ExpectedTypePath> expected_opath;
      {
        RecordExtraction re = extract_concrete_record(env, ty_expected);
        switch (re.kind) {
          case RecordExtraction::Kind::Record_type:
            expected_opath = ExpectedTypePath{re.p0, re.p, is_principal(ty_expected)};
            break;
          case RecordExtraction::Kind::Maybe_a_record_type: break;
          case RecordExtraction::Kind::Not_a_record_type: {
            Error e = err(loc, env, EK::Wrong_expected_kind);
            e.sort = WrongKindSort::Record;
            e.ctx = WrongKindContext{false, explanation};
            e.ty = ty_expected;
            raise_error(e);
          }
        }
      }
      std::optional<ExpectedTypePath> opt_exp_opath;
      if (opt_exp) {
        RecordExtraction re = extract_concrete_record(env, opt_exp->exp_type);
        switch (re.kind) {
          case RecordExtraction::Kind::Record_type:
            opt_exp_opath = ExpectedTypePath{re.p0, re.p, is_principal(opt_exp->exp_type)};
            break;
          case RecordExtraction::Kind::Maybe_a_record_type: break;
          case RecordExtraction::Kind::Not_a_record_type: {
            Error e = err(opt_exp->exp_loc, env, EK::Expr_not_a_record_type);
            e.ty = opt_exp->exp_type;
            raise_error(e);
          }
        }
      }
      TypeExpr* ty_record;
      std::optional<ExpectedTypePath> expected_type;
      if (!expected_opath && !opt_exp_opath) {
        ty_record = ctype::newvar();
      } else if (expected_opath && !opt_exp_opath) {
        ty_record = ty_expected;
        expected_type = expected_opath;
      } else if (expected_opath && expected_opath->principal) {
        ty_record = ty_expected;
        expected_type = expected_opath;
      } else {
        Path::t p2 = opt_exp_opath->tpath;
        const TypeDeclaration* decl = env::find_type(p2, env);
        ty_record = ctype::with_local_level_generalize_structure([&] {
          std::vector<TypeExpr*> ps(decl->type_params.begin(), decl->type_params.end());
          return ctype::newconstr(p2, slice(ctype::instance_list(ps)));
        });
        expected_type = opt_exp_opath;
      }
      bool closed = opt_sexp == nullptr;
      std::vector<LabelExp> lbl_exp_list =
          wrap_disambiguate("This record expression is expected to have", mk_expected(ty_record), [&] {
            // type_label_a_list loc closed env Construct (type_label_exp true env loc ty_record)
            //   expected_type lid_sexp_list
            std::vector<pt::LidLoc> lids;
            for (auto& f : lid_sexp_list) lids.push_back(f.first);
            std::vector<const LabelDescription*> lbls =
                disambiguate_lid_list(loc, closed, env, env::LabelUsage::Construct, expected_type, lids);
            struct Item {
              pt::LidLoc lid;
              const LabelDescription* label;
              const pt::Expression* sarg;
            };
            std::vector<Item> items;
            for (std::size_t k = 0; k < lids.size(); ++k)
              items.push_back({lids[k], lbls[k], lid_sexp_list[k].second});
            // Invariant: records are sorted in the typed tree
            items = ocaml_list::stable_sort(
                [](const Item& a, const Item& b) {
                  return a.label->lbl_pos < b.label->lbl_pos ? -1 : a.label->lbl_pos > b.label->lbl_pos ? 1 : 0;
                },
                items);
            std::vector<LabelExp> out;
            for (auto& it : items) out.push_back(type_label_exp(true, env, loc, ty_record, it.lid, it.label, it.sarg));
            return out;
          });
      with_explanation(explanation, [&] {
        // unify_exp_types loc env (instance ty_record) (instance ty_expected): right to left
        TypeExpr* b = ctype::instance(ty_expected);
        TypeExpr* a2 = ctype::instance(ty_record);
        unify_exp_types(loc, env, a2, b);
      });
      // type_label_a_list returns a list of labels sorted by lbl_pos
      for (std::size_t k = 0; k + 1 < lbl_exp_list.size(); ++k)
        if (lbl_exp_list[k].label->lbl_pos == lbl_exp_list[k + 1].label->lbl_pos) {
          Error e = err(loc, env, EK::Label_multiply_defined);
          e.name = std::string(lbl_exp_list[k].label->lbl_name);
          raise_error(e);
        }
      const LabelDescription* lbl = lbl_exp_list[0].label;
      auto matching_label = [&](const LabelDescription* l) -> const LabelExp* {
        for (auto& x : lbl_exp_list)
          if (x.label->lbl_pos == l->lbl_pos) return &x;
        return nullptr;
      };
      std::vector<tt::RecordLabelDefinition> label_definitions;
      const tt::Expression* ext = nullptr;
      if (!opt_exp) {
        for (const LabelDescription* l : lbl->lbl_all) {
          if (const LabelExp* m = matching_label(l)) {
            tt::RecordLabelDefinition def{false};
            def.lid = m->lid;
            def.exp = m->exp;
            label_definitions.push_back(def);
            continue;
          }
          std::vector<long> present_indices;
          for (auto& x : lbl_exp_list) present_indices.push_back(x.label->lbl_pos);
          std::vector<Ident::t> label_names = extract_label_names(env, ty_expected);
          std::vector<Ident::t> missing;
          for (std::size_t n = 0; n < label_names.size(); ++n)
            if (std::find(present_indices.begin(), present_indices.end(), static_cast<long>(n)) ==
                present_indices.end())
              missing.push_back(label_names[n]);
          Error e = err(loc, env, EK::Label_missing);
          e.ids = missing;
          raise_error(e);
        }
      } else {
        TypeExpr* ty_exp = ctype::instance(opt_exp->exp_type);
        for (const LabelDescription* l : lbl->lbl_all) {
          ctype::InstancedLabel il1 = ctype::instance_label(false, l);
          unify_exp_types(opt_exp->exp_loc, env, ty_exp, il1.res);
          if (const LabelExp* m = matching_label(l)) {
            // do not connect result types for overridden labels
            tt::RecordLabelDefinition def{false};
            def.lid = m->lid;
            def.exp = m->exp;
            label_definitions.push_back(def);
            continue;
          }
          ctype::InstancedLabel il2 = ctype::instance_label(false, l);
          unify_exp_types(loc, env, il1.arg, il2.arg);
          with_explanation(explanation, [&] {
            TypeExpr* ie = ctype::instance(ty_expected);
            unify_exp_types(loc, env, ie, il2.res);
          });
          tt::RecordLabelDefinition def{true};
          def.ty = il1.arg;
          def.mut = l->lbl_mut;
          label_definitions.push_back(def);
        }
        tt::Expression* e2 = make<tt::Expression>(*opt_exp);
        e2->exp_type = ty_exp;
        ext = e2;
      }
      std::size_t num_fields = lbl_exp_list.at(0).label->lbl_all.size();
      if (opt_sexp && lid_sexp_list.size() == num_fields) prerr_warning(loc, WK::Useless_record_with);
      std::vector<tt::RecordField> fields;
      for (std::size_t k = 0; k < lbl->lbl_all.size(); ++k) fields.push_back({lbl->lbl_all[k], label_definitions[k]});
      return re(mk(mkd(tt::Texp_record{{XK::Texp_record}, slice(fields), lbl->lbl_repres, ext}), loc,
                ctype::instance(ty_expected), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_field: {
      auto* f = as<pt::Pexp_field>(d);
      SolvedField sf = solve_Pexp_field(env::LabelUsage::Projection, env, sexp, f->exp, f->lid);
      return rue(mk(mkd(tt::Texp_field{{XK::Texp_field}, sf.record, f->lid, sf.label}), loc, sf.ty_arg, env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_setfield: {
      auto* f = as<pt::Pexp_setfield>(d);
      LabelAccess la = type_label_access(env, f->exp, env::LabelUsage::Mutation, f->lid);
      TypeExpr* ty_record = la.expected_type ? la.record->exp_type : ctype::newvar();
      LabelExp le = type_label_exp(false, env, loc, ty_record, f->lid, la.label, f->value);
      unify_exp(sexp, env, la.record, ty_record);
      if (le.label->lbl_mut == MutableFlag::Immutable) {
        Error e = err(loc, env, EK::Label_not_mutable);
        e.lid = f->lid.txt;
        raise_error(e);
      }
      return rue(mk(mkd(tt::Texp_setfield{{XK::Texp_setfield}, la.record, le.lid, le.label, le.exp}), loc,
                    ctype::instance(predef::type_unit()), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_array: {
      TypeExpr* ty_elt;
      MutableFlag mut;
      {
        TypeExpr* ty_exp = ctype::generic_instance(ty_expected);
        ArrayInfo ai = disambiguate_array_literal(loc, env, ty_exp);
        mut = ai.mut;
        if (ai.ty_elt) {
          ty_elt = ai.ty_elt;
        } else {
          ty_elt = newgenvar();
          TypeExpr* to_unify = mut == MutableFlag::Mutable ? predef::type_array(ty_elt) : predef::type_iarray(ty_elt);
          with_explanation(explanation, [&] { unify_exp_types(loc, env, to_unify, ty_exp); });
        }
      }
      std::vector<const tt::Expression*> argl;
      for (auto* sarg : as<pt::Pexp_array>(d)->el) argl.push_back(type_expect(env, sarg, mk_expected(ty_elt)));
      return re(mk(mkd(tt::Texp_array{{XK::Texp_array}, mut, slice(argl)}), loc, ctype::instance(ty_expected), env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_ifthenelse: {
      auto* i = as<pt::Pexp_ifthenelse>(d);
      const tt::Expression* cond =
          type_expect(env, i->cond, mk_expected(predef::type_bool(), TypeForcingContext::If_conditional));
      if (!i->else_) {
        const tt::Expression* ifso =
            type_expect(env, i->then_, mk_expected(predef::type_unit(), TypeForcingContext::If_no_else_branch));
        return rue(mk(mkd(tt::Texp_ifthenelse{{XK::Texp_ifthenelse}, cond, ifso, nullptr}), loc, ifso->exp_type,
                      env, sexp->pexp_attributes));
      }
      const tt::Expression* ifso = type_expect(env, i->then_, ty_expected_explained);
      const tt::Expression* ifnot = type_expect(env, i->else_, ty_expected_explained);
      // Keep sharing
      unify_exp(sexp, env, ifnot, ifso->exp_type);
      return re(mk(mkd(tt::Texp_ifthenelse{{XK::Texp_ifthenelse}, cond, ifso, ifnot}), loc, ifso->exp_type, env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_sequence: {
      auto* s = as<pt::Pexp_sequence>(d);
      const tt::Expression* exp1 = type_statement(TypeForcingContext::Sequence_left_hand_side, env, s->e1);
      const tt::Expression* exp2 = type_expect(env, s->e2, ty_expected_explained);
      return re(mk(mkd(tt::Texp_sequence{{XK::Texp_sequence}, exp1, exp2}), loc, exp2->exp_type, env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_while: {
      auto* w = as<pt::Pexp_while>(d);
      const tt::Expression* cond =
          type_expect(env, w->cond, mk_expected(predef::type_bool(), TypeForcingContext::While_loop_conditional));
      TypeExpr* exp_type;
      auto* c = as<tt::Texp_construct>(cond->exp_desc);
      if (c && c->cstr->cstr_name == "true") exp_type = ctype::instance(ty_expected);
      else exp_type = ctype::instance(predef::type_unit());
      const tt::Expression* body = type_statement(TypeForcingContext::While_loop_body, env, w->body);
      return rue(mk(mkd(tt::Texp_while{{XK::Texp_while}, cond, body}), loc, exp_type, env, sexp->pexp_attributes));
    }
    case SXK::Pexp_for: {
      auto* f = as<pt::Pexp_for>(d);
      const tt::Expression* low =
          type_expect(env, f->lo, mk_expected(predef::type_int(), TypeForcingContext::For_loop_start_index));
      const tt::Expression* high =
          type_expect(env, f->hi, mk_expected(predef::type_int(), TypeForcingContext::For_loop_stop_index));
      Ident::t id;
      env::t new_env;
      const pt::Pattern* param = f->pat;
      if (param->ppat_desc->kind == SPK::Ppat_any) {
        id = Ident::create_local(OCAML_LIT("_for"));
        new_env = env;
      } else if (auto* v = as<pt::Ppat_var>(param->ppat_desc)) {
        // record fields right to left: val_uid first
        Uid uid = uid::mk(env::get_current_unit());
        TypeExpr* ty = ctype::instance(predef::type_int());
        auto* vd = make<ValueDescription>(ty, ValueKind{}, loc, Attributes{}, uid);
        std::tie(id, new_env) = env::enter_value(v->name.txt, vd, env, [](std::string s) {
          return warnings::Warning::with_s(WK::Unused_for_index, s);
        });
      } else {
        raise_error(err(param->ppat_loc, env, EK::Invalid_for_loop_index));
      }
      const tt::Expression* body = type_statement(TypeForcingContext::For_loop_body, new_env, f->body);
      return rue(mk(mkd(tt::Texp_for{{XK::Texp_for}, id, param, low, high, f->dir, body}), loc,
                    ctype::instance(predef::type_unit()), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_constraint: {
      auto* c = as<pt::Pexp_constraint>(d);
      auto [ty, exp_extra] = type_constraint(env, c->ty);
      TypeExpr* ity = ctype::instance(ty);
      const tt::Expression* arg = type_argument(env, c->exp, ty, ity);
      tt::Expression* e = make<tt::Expression>(*arg);
      e->exp_type = ctype::instance(ty);
      e->exp_env = env;
      e->exp_extra = cons_extra(exp_extra, loc, sexp->pexp_attributes, arg->exp_extra);
      return rue(e);
    }
    case SXK::Pexp_coerce: {
      auto* c = as<pt::Pexp_coerce>(d);
      Constrained<const tt::Expression*> r =
          type_coerce(expression_constraint(c->exp), env, loc, c->from, c->to, c->exp->pexp_loc);
      tt::Expression* e = make<tt::Expression>(*r.ret);
      e->exp_type = r.ty;
      e->exp_env = env;
      e->exp_extra = cons_extra(r.extra, loc, sexp->pexp_attributes, r.ret->exp_extra);
      return rue(e);
    }
    case SXK::Pexp_send: {
      auto* s = as<pt::Pexp_send>(d);
      SendResult sr = ctype::with_local_level_generalize_structure_if_principal(
          [&] { return type_send(env, loc, explanation, s->exp, s->meth.txt); });
      TypeExpr* typ;
      const TypeDesc* td = get_desc(sr.typ);
      if (auto* p = as<Tpoly>(td)) {
        if (p->vars.empty()) typ = ctype::instance(p->body);
        else {
          if (clflags::principal && get_level(sr.typ) != generic_level)
            prerr_warning(loc, not_principal("this use of a polymorphic method"));
          typ = ctype::instance_poly(p->vars, p->body);
        }
      } else if (td->kind == DescKind::Tvar) {
        TypeExpr* ty2 = ctype::newvar();
        TypeExpr* poly = ctype::newty(tpoly(ty2, {}));
        TypeExpr* it = ctype::instance(sr.typ);
        ctype::unify(env, it, poly);
        typ = ty2;
      } else {
        throw std::logic_error("type_expect_: Pexp_send");
      }
      return rue(mk(make<tt::Texp_send>(tt::Texp_send{{XK::Texp_send}, sr.obj, sr.meth}), loc, typ, env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_new: {
      const pt::LidLoc& cl = as<pt::Pexp_new>(d)->lid;
      auto [cl_path, cl_decl] = env::lookup_class(true, cl.loc, cl.txt, env);
      if (!cl_decl->cty_new) {
        Error e = err(loc, env, EK::Virtual_class);
        e.lid = cl.txt;
        raise_error(e);
      }
      return rue(mk(mkd(tt::Texp_new{{XK::Texp_new}, cl_path, cl, cl_decl}), loc, ctype::instance(cl_decl->cty_new),
                    env, sexp->pexp_attributes));
    }
    case SXK::Pexp_setinstvar: {
      auto* s = as<pt::Pexp_setinstvar>(d);
      env::InstanceVariable iv = env::lookup_instance_variable(true, loc, s->name.txt, env);
      if (iv.mut != MutableFlag::Mutable) {
        Error e = err(loc, env, EK::Instance_variable_not_mutable);
        e.name = std::string(s->name.txt);
        raise_error(e);
      }
      const tt::Expression* newval = type_expect(env, s->value, mk_expected(ctype::instance(iv.ty)));
      Path::t path_self = env::find_value_by_name(self_lid(iv.cl_num), env).first;
      return rue(mk(mkd(tt::Texp_setinstvar{{XK::Texp_setinstvar}, path_self, iv.path, s->name, newval}), loc,
                    ctype::instance(predef::type_unit()), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_override: {
      auto lst = as<pt::Pexp_override>(d)->fields;
      {  // List.fold_right: duplicates are detected from the right
        std::vector<std::string_view> l;
        for (std::size_t k = lst.size(); k-- > 0;) {
          if (std::find(l.begin(), l.end(), lst[k].first.txt) != l.end()) {
            Error e = err(loc, env, EK::Value_multiply_overridden);
            e.name = std::string(lst[k].first.txt);
            raise_error(e);
          }
          l.push_back(lst[k].first.txt);
        }
      }
      std::pair<Path::t, const ValueDescription*> self, selfpat;
      try {
        // (find_value_by_name "selfpat-*", find_value_by_name "self-*"): right to left
        self = env::find_value_by_name(Longident::lident(OCAML_LIT("self-*")), env);
        selfpat = env::find_value_by_name(Longident::lident(OCAML_LIT("selfpat-*")), env);
      } catch (const env::NotFound&) {
        raise_error(err(loc, env, EK::Outside_class));
      }
      if (selfpat.second->val_kind.kind != ValueKind::Kind::Val_self)
        throw std::logic_error("type_expect_: Pexp_override");
      const ValueKind& vk = selfpat.second->val_kind;
      TypeExpr* self_ty = selfpat.second->val_type;
      std::vector<tt::OverrideField> modifs;
      for (auto& [lab, snewval] : lst) {
        // (Btype.instance_variable_type finds every variable of [vars])
        const Ident::t* id = vk.vars.find_opt(lab.txt);
        if (!id) {
          std::vector<std::string> vars;
          vk.vars.iter([&](std::string_view var, Ident::t) { vars.insert(vars.begin(), std::string(var)); });
          Error e = err(loc, env, EK::Unbound_instance_variable);
          e.name = std::string(lab.txt);
          e.names = vars;
          raise_error(e);
        }
        TypeExpr* ty = instance_variable_type(lab.txt, vk.sign);
        const tt::Expression* e = type_expect(env, snewval, mk_expected(ctype::instance(ty)));
        modifs.push_back({*id, lab, e});
      }
      return rue(mk(mkd(tt::Texp_override{{XK::Texp_override}, self.first, slice(modifs)}), loc, self_ty, env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_assert: {
      const tt::Expression* cond = type_expect(env, as<pt::Pexp_assert>(d)->exp,
                                               mk_expected(predef::type_bool(), TypeForcingContext::Assert_condition));
      TypeExpr* exp_type;
      auto* c = as<tt::Texp_construct>(cond->exp_desc);
      if (c && c->cstr->cstr_name == "false") exp_type = ctype::instance(ty_expected);
      else exp_type = ctype::instance(predef::type_unit());
      Location inner = sexp->pexp_loc_stack.empty() ? loc : sexp->pexp_loc_stack[sexp->pexp_loc_stack.size() - 1];
      return rue(mk(mkd(tt::Texp_assert{{XK::Texp_assert}, cond, inner}), loc, exp_type, env, sexp->pexp_attributes));
    }
    case SXK::Pexp_lazy: {
      TypeExpr* ty = newgenvar();
      TypeExpr* to_unify = predef::type_lazy_t(ty);
      with_explanation(explanation, [&] {
        TypeExpr* gi = ctype::generic_instance(ty_expected);
        unify_exp_types(loc, env, to_unify, gi);
      });
      const tt::Expression* arg = type_expect(env, as<pt::Pexp_lazy>(d)->exp, mk_expected(ty));
      return re(mk(mkd(tt::Texp_lazy{{XK::Texp_lazy}, arg}), loc, ctype::instance(ty_expected), env,
                sexp->pexp_attributes));
    }
    case SXK::Pexp_object: {
      auto [desc, meths] = type_object(env, loc, as<pt::Pexp_object>(d)->cs);
      std::vector<std::string_view> ms(meths.begin(), meths.end());
      return rue(mk(mkd(tt::Texp_object{{XK::Texp_object}, desc, slice(ms)}), loc, desc->cstr_type->csig_self, env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_poly: {
      auto* p = as<pt::Pexp_poly>(d);
      const pt::CoreType* sty = p->ty;
      auto [ty, cty] = ctype::with_local_level_generalize_structure_if_principal(
          [&]() -> std::pair<TypeExpr*, const tt::CoreType*> {
            if (!sty) return {protect_expansion(env, ty_expected), nullptr};
            // Ast_helper.Typ.force_poly
            const pt::CoreType* s = sty->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly
                                        ? sty
                                        : ah::typ_poly(sty->ptyp_loc, {}, sty);
            const tt::CoreType* c = typetexp::transl_simple_type(env, nullptr, false, s);
            return {c->ctyp_type, c};
          });
      if (sty)
        with_explanation(explanation, [&] {
          TypeExpr* b = ctype::instance(ty_expected);
          TypeExpr* a2 = ctype::instance(ty);
          unify_exp_types(loc, env, a2, b);
        });
      const tt::Expression* exp;
      const TypeDesc* td = get_desc(ctype::expand_head(env, ty));
      if (auto* tp = as<Tpoly>(td); tp && tp->vars.empty()) {
        const tt::Expression* e = type_expect(env, p->exp, mk_expected(tp->body));
        tt::Expression* e2 = make<tt::Expression>(*e);
        e2->exp_type = ctype::instance(ty);
        exp = e2;
      } else if (tp) {
        // One more level to generalize locally
        auto [e, vars] = ctype::with_local_level_generalize([&] {
          auto [vs, ty2] = ctype::with_local_level_generalize_structure_if_principal(
              [&] { return ctype::instance_poly_fixed(tp->vars, tp->body); });
          const tt::Expression* ex = type_expect(env, p->exp, mk_expected(ty2));
          return std::make_pair(ex, vs);
        });
        check_univars(env, "method", e, ty_expected, vars);
        tt::Expression* e2 = make<tt::Expression>(*e);
        e2->exp_type = ctype::instance(ty);
        exp = e2;
      } else if (td->kind == DescKind::Tvar) {
        const tt::Expression* e = type_exp(env, p->exp);
        tt::Expression* e2 = make<tt::Expression>(*e);
        e2->exp_type = ctype::newty(tpoly(e->exp_type, {}));
        unify_exp(sexp, env, e2, ty);
        exp = e2;
      } else {
        throw std::logic_error("type_expect_: Pexp_poly");
      }
      tt::ExpExtra x{tt::ExpExtra::Kind::Texp_poly};
      x.cty = cty;
      tt::Expression* r = make<tt::Expression>(*exp);
      r->exp_extra = cons_extra(x, loc, sexp->pexp_attributes, exp->exp_extra);
      return re(r);
    }
    case SXK::Pexp_newtype: {
      auto* n = as<pt::Pexp_newtype>(d);
      auto [body, ety] = type_newtype<const tt::Expression*>(env, n->name, [&](env::t env2) {
        const tt::Expression* expr = type_exp(env2, n->body);
        return std::make_pair(expr, expr->exp_type);
      });
      // non-expansive if the body is non-expansive, so we don't introduce
      // any new extra node in the typed AST.
      tt::ExpExtra x{tt::ExpExtra::Kind::Texp_newtype};
      x.name = n->name.txt;
      tt::Expression* e = make<tt::Expression>(*body);
      e->exp_loc = loc;
      e->exp_type = ety;
      e->exp_extra = cons_extra(x, loc, sexp->pexp_attributes, body->exp_extra);
      return rue(e);
    }
    case SXK::Pexp_pack: {
      auto* p = as<pt::Pexp_pack>(d);
      if (p->pack) {
        const pt::CoreType* t = ah::typ_package(p->pack->ppt_loc, p->pack);
        auto [pty, exp_extra] = type_constraint(env, t);
        auto* tp = as<Tpackage>(get_desc(ctype::instance(pty)));
        if (!tp) throw std::logic_error("[type_expect] Package not translated to a package");
        auto [modl, pack2] = type_package(env, p->me, tp->pack);
        TypeExpr* ty = ctype::newty(tpackage(pack2));
        unify_exp_types(p->me->pmod_loc, env, ctype::instance(pty), ty);
        tt::Expression* e = mk(mkd(tt::Texp_pack{{XK::Texp_pack}, modl}), loc, ctype::instance(pty), env,
                               sexp->pexp_attributes);
        std::vector<tt::ExpExtraItem> extra{{exp_extra, loc, {}}};
        e->exp_extra = slice(extra);
        return rue(e);
      }
      const Package* pack;
      const TypeDesc* td = get_desc(ctype::expand_head(env, ctype::instance(ty_expected)));
      if (auto* tp = as<Tpackage>(td)) {
        if (clflags::principal &&
            get_level(ctype::expand_head(env, protect_expansion(env, ty_expected))) < generic_level)
          prerr_warning(loc, not_principal("this module packing"));
        pack = tp->pack;
      } else if (td->kind == DescKind::Tvar) {
        raise_error(err(loc, env, EK::Cannot_infer_signature));
      } else {
        Error e = err(loc, env, EK::Not_a_packed_module);
        e.ty = ty_expected;
        raise_error(e);
      }
      auto [modl, pack2] = type_package(env, p->me, pack);
      return rue(mk(mkd(tt::Texp_pack{{XK::Texp_pack}, modl}), loc, ctype::newty(tpackage(pack2)), env,
                    sexp->pexp_attributes));
    }
    case SXK::Pexp_letop: {
      const pt::Letop* lo = as<pt::Pexp_letop>(d)->letop;
      const pt::BindingOp* slet = lo->let_;
      Slice<const pt::BindingOp*> sands = lo->ands;
      struct R {
        Path::t op_path;
        const ValueDescription* op_desc;
        TypeExpr* op_type;
        const pt::Pattern* spat_params;
        TypeExpr* ty_params;
        TypeExpr* ty_func_result;
        TypeExpr* ty_result;
        TypeExpr* ty_andops;
      };
      R r = ctype::with_local_level_generalize_structure_if_principal([&] {
        const Location& let_loc = slet->pbop_op.loc;
        auto [op_path, op_desc] = type_binding_op_ident(env, slet->pbop_op);
        TypeExpr* op_type = ctype::instance(op_desc->val_type);
        const pt::Pattern* spat_acc = slet->pbop_pat;
        TypeExpr* ty_acc = ctype::newvar();
        for (auto* sand : sands) {
          TypeExpr* ty = ctype::newvar();
          Location l2 = slet->pbop_op.loc;
          l2.loc_ghost = true;
          l2 = location::distinct_record(l2);  // {... with loc_ghost = true}
          std::vector<pt::LabeledPattern> pl{{OptStr::none(), spat_acc}, {OptStr::none(), sand->pbop_pat}};
          spat_acc = ah::pat_mk(mkd(pt::Ppat_tuple{{SPK::Ppat_tuple}, slice(pl), ClosedFlag::Closed}), l2);
          std::vector<LabeledTy> tl{{OptStr::none(), ty_acc}, {OptStr::none(), ty}};
          ty_acc = ctype::newty(ttuple(slice(tl)));
        }
        TypeExpr* ty_func_result = ctype::newvar();
        TypeExpr* ty_func =
            ctype::newty(tarrow(ArgLabel::nolabel(), ctype::newmono(ty_acc), ty_func_result, commu_ok()));
        TypeExpr* ty_result = ctype::newvar();
        TypeExpr* ty_andops = ctype::newvar();
        TypeExpr* ty_fun = ctype::newty(tarrow(ArgLabel::nolabel(), ctype::newmono(ty_func), ty_result, commu_ok()));
        TypeExpr* ty_op = ctype::newty(tarrow(ArgLabel::nolabel(), ctype::newmono(ty_andops), ty_fun, commu_ok()));
        try {
          ctype::unify(env, op_type, ty_op);
        } catch (const ctype::Unify& u) {
          Error e = err(let_loc, env, EK::Letop_type_clash);
          e.name = std::string(slet->pbop_op.txt);
          e.trace = u.err;
          raise_error(e);
        }
        return R{op_path, op_desc, op_type, spat_acc, ty_acc, ty_func_result, ty_result, ty_andops};
      });
      auto [exp, ands] = type_andops(env, slet->pbop_exp, sands, r.ty_andops);
      std::vector<const pt::Case*> scase{make<pt::Case>(r.spat_params, nullptr, lo->body)};
      auto [cases, partial] = type_cases(tt::PatternCategory::Value, env, r.ty_params, mk_expected(r.ty_func_result),
                                         nullptr, true, loc, slice(scase));
      if (cases.size() != 1) throw std::logic_error("type_expect_: Pexp_letop");
      const tt::Case* body = cases[0];
      Ident::t param = name_cases(OCAML_LIT("param"), cases);
      auto* let_ = make<tt::BindingOp>(r.op_path, slet->pbop_op, r.op_desc, r.op_type, exp, slet->pbop_loc);
      return rue(mk(mkd(tt::Texp_letop{{XK::Texp_letop}, let_, ands, param, body, partial}), sexp->pexp_loc,
                    ctype::instance(r.ty_result), env, sexp->pexp_attributes));
    }
    case SXK::Pexp_extension: {
      const pt::Extension* ext = as<pt::Pexp_extension>(d)->ext;
      std::string_view name = ext->name.txt;
      if (name == "ocaml.extension_constructor" || name == "extension_constructor") {
        const pt::Expression* e = single_eval_payload(ext->payload);
        auto* c = e ? as<pt::Pexp_construct>(e->pexp_desc) : nullptr;
        if (!c || c->arg) raise_error(err(loc, env, EK::Invalid_extension_constructor_payload));
        const ConstructorDescription* cd =
            env::lookup_constructor(true, c->lid.loc, env::ConstructorUsage::Positive, c->lid.txt, env);
        if (cd->cstr_tag.kind != ConstructorTag::Kind::Cstr_extension)
          raise_error(err(c->lid.loc, env, EK::Not_an_extension_constructor));
        return rue(mk(mkd(tt::Texp_extension_constructor{{XK::Texp_extension_constructor}, c->lid, cd->cstr_tag.ext_path}),
                      loc, ctype::instance(predef::type_extension_constructor()), env, sexp->pexp_attributes));
      }
      if (name == "ocaml.atomic.loc" || name == "atomic.loc") {
        const pt::Expression* inner = single_eval_payload(ext->payload);
        auto* f = inner ? as<pt::Pexp_field>(inner->pexp_desc) : nullptr;
        if (!f) raise_error(err(loc, env, EK::Invalid_atomic_loc_payload));
        SolvedField sf = solve_Pexp_field(env::LabelUsage::Mutation, env, inner, f->exp, f->lid);
        env::mark_label_used(env::LabelUsage::Projection, sf.label->lbl_uid);
        if (sf.label->lbl_atomic == AtomicFlag::Nonatomic) {
          Error e = err(loc, env, EK::Label_not_atomic);
          e.lid = f->lid.txt;
          raise_error(e);
        }
        // exp_attributes are the inner (shadowing) sexp's
        return rue(mk(mkd(tt::Texp_atomic_loc{{XK::Texp_atomic_loc}, sf.record, f->lid, sf.label}), loc,
                      ctype::instance(predef::type_atomic_loc(sf.ty_arg)), env, inner->pexp_attributes));
      }
      throw ErrorForward(ext);
    }
    case SXK::Pexp_unreachable:
      return re(mk(make<tt::Texp_unreachable>(tt::Texp_unreachable{{XK::Texp_unreachable}}), loc,
                ctype::instance(ty_expected), env, sexp->pexp_attributes));
    case SXK::Pexp_struct_item: {
      auto* s = as<pt::Pexp_struct_item>(d);
      TypeExpr* tv = ctype::newvar();
      struct R {
        env::t newenv;
        const tt::StructureItem* si;
        const tt::Expression* exp;
      };
      R r = ctype::with_local_level_generalize(
          [&] {
            auto [si, newenv] = typetexp::ty_var_env::with_local_scope([&] { return type_str_item(env, s->item); });
            const tt::Expression* exp = type_expect(newenv, s->body, ty_expected_explained);
            return R{newenv, si, exp};
          },
          [&](const R& r) {
            // Ensure that local definitions do not leak.  (Required for
            // implicit unpack)
            ctype::unify_var(r.newenv, tv, r.exp->exp_type);
          });
      return re(mk(mkd(tt::Texp_struct_item{{XK::Texp_struct_item}, r.si, r.exp}), loc, r.exp->exp_type, env,
                sexp->pexp_attributes));
    }
  }
  throw std::logic_error("type_expect_");
}

ConstraintArg<const tt::Expression*> expression_constraint(const pt::Expression* pexp) {
  ConstraintArg<const tt::Expression*> c;
  c.type_without_constraint = [pexp](env::t env) {
    const tt::Expression* expr = type_exp(env, pexp);
    return std::make_pair(expr, expr->exp_type);
  };
  c.type_with_constraint = [pexp](env::t env, TypeExpr* ty) {
    TypeExpr* ity = ctype::instance(ty);
    return type_argument(env, pexp, ty, ity);
  };
  c.is_self = [](const tt::Expression* const& expr) {
    auto* i = as<tt::Texp_ident>(expr->exp_desc);
    return i && i->vd->val_kind.kind == ValueKind::Kind::Val_self;
  };
  return c;
}

// Types a body in the scope of a coercion (with an optional constraint) and
// returns the inferred type.
template <class Ret>
Constrained<Ret> type_coerce(const ConstraintArg<Ret>& c, env::t env, const Location& loc,
                             const pt::CoreType* sty, const pt::CoreType* sty2, const Location& loc_arg) {
  // Pretend separate = true, 1% slowdown for lablgtk
  if (!sty) {
    typetexp::Delayed dl2 =
        ctype::with_local_level_generalize_structure([&] { return typetexp::transl_simple_type_delayed(env, sty2); });
    TypeExpr* ty2 = dl2.ty;
    struct A {
      Ret arg;
      TypeExpr* arg_type;
      bool gen;
    };
    long lv = ctype::get_current_level();
    A a = ctype::with_local_level_generalize(
        [&] {
          auto [arg, arg_type] = c.type_without_constraint(env);
          return A{arg, arg_type, generalizable(lv, arg_type)};
        },
        [&](const A& a) { ctype::enforce_current_level(env, a.arg_type); });
    auto* tc = as<Tconstr>(get_desc(ty2));
    if (!self_coercion.empty() && tc && c.is_self(a.arg) && path::same(self_coercion[0].first, tc->path)) {
      self_coercion[0].second->insert(self_coercion[0].second->begin(), loc);
      dl2.force();
    } else if (ctype::closed_type_expr(a.arg_type, env) && ctype::closed_type_expr(ty2, env)) {
      bool single = false;
      if (!a.gen) {
        // first try a single coercion
        Snapshot snap = btype::snapshot();
        auto [ty, b] = ctype::enlarge_type(env, ctype::generic_instance(ty2));
        (void)b;
        try {
          dl2.force();
          ctype::unify(env, a.arg_type, ty);
          single = true;
        } catch (const ctype::Unify&) {
          btype::backtrack(snap);
        }
      }
      if (!single) {
        try {
          TypeExpr* gi = ctype::generic_instance(ty2);
          std::function<void()> force2 = ctype::subtype(env, a.arg_type, gi);
          dl2.force();
          force2();
          if (!a.gen && clflags::principal) prerr_warning(loc, not_principal("this ground coercion"));
        } catch (const ctype::Subtype& s) {
          Error e = err(loc, env, EK::Not_subtype);
          e.subtype = s.err;
          raise_error(e);
        }
      }
    } else {
      auto [ty, b] = ctype::enlarge_type(env, ctype::generic_instance(ty2));
      dl2.force();
      try {
        ctype::unify(env, a.arg_type, ty);
      } catch (const ctype::Unify& u) {
        TypeExpr* expanded = ctype::full_expand(true, env, ty2);
        Error e = err(loc_arg, env, EK::Coercion_failure);
        e.expanded = et::ExpandedType{ty2, expanded};
        e.trace = u.err;
        e.flag = b;
        raise_error(e);
      }
    }
    tt::ExpExtra x{tt::ExpExtra::Kind::Texp_coerce};
    x.cty = dl2.cty;
    return {a.arg, ty2, x};
  }
  struct D2 {
    typetexp::Delayed d1, d2;
  };
  D2 dd = ctype::with_local_level_generalize_structure([&] {
    typetexp::Delayed d1 = typetexp::transl_simple_type_delayed(env, sty);
    typetexp::Delayed d2 = typetexp::transl_simple_type_delayed(env, sty2);
    return D2{d1, d2};
  });
  try {
    // subtype env (generic_instance ty) (generic_instance ty'): right to left
    TypeExpr* g2 = ctype::generic_instance(dd.d2.ty);
    TypeExpr* g1 = ctype::generic_instance(dd.d1.ty);
    std::function<void()> force3 = ctype::subtype(env, g1, g2);
    dd.d1.force();
    dd.d2.force();
    force3();
  } catch (const ctype::Subtype& s) {
    Error e = err(loc, env, EK::Not_subtype);
    e.subtype = s.err;
    raise_error(e);
  }
  // (type_with_constraint env ty, instance ty', Texp_coerce ..): right to left
  TypeExpr* ity2 = ctype::instance(dd.d2.ty);
  Ret ret = c.type_with_constraint(env, dd.d1.ty);
  tt::ExpExtra x{tt::ExpExtra::Kind::Texp_coerce};
  x.from = dd.d1.cty;
  x.cty = dd.d2.cty;
  return {ret, ity2, x};
}

std::pair<TypeExpr*, tt::ExpExtra> type_constraint(env::t env, const pt::CoreType* sty) {
  // Pretend separate = true, 1% slowdown for lablgtk
  const tt::CoreType* cty = ctype::with_local_level_generalize_structure(
      [&] { return typetexp::transl_simple_type(env, nullptr, false, sty); });
  tt::ExpExtra x{tt::ExpExtra::Kind::Texp_constraint};
  x.cty = cty;
  return {cty->ctyp_type, x};
}

// Types a body in the scope of a coercion (:>) or a constraint (:), and
// unifies the inferred type with the expected type.
template <class Ret>
Constrained<Ret> type_constraint_expect(const ConstraintArg<Ret>& c, env::t env, const Location& loc,
                                        const Location& loc_arg, const pt::TypeConstraint& constraint_,
                                        TypeExpr* ty_expected) {
  Constrained<Ret> r;
  if (constraint_.kind == pt::TypeConstraint::Kind::Pcoerce) {
    r = type_coerce(c, env, loc, constraint_.from, constraint_.ty, loc_arg);
  } else {
    auto [ty, exp_extra] = type_constraint(env, constraint_.ty);
    r = Constrained<Ret>{c.type_with_constraint(env, ty), ty, exp_extra};
  }
  TypeExpr* ie = ctype::instance(ty_expected);
  unify_exp_types(loc, env, r.ty, ie);
  return r;
}

template Constrained<const tt::Expression*> type_coerce(const ConstraintArg<const tt::Expression*>&, env::t,
                                                        const Location&, const pt::CoreType*, const pt::CoreType*,
                                                        const Location&);
template Constrained<const tt::Expression*> type_constraint_expect(const ConstraintArg<const tt::Expression*>&,
                                                                   env::t, const Location&, const Location&,
                                                                   const pt::TypeConstraint&, TypeExpr*);
template Constrained<FunctionCases> type_constraint_expect(const ConstraintArg<FunctionCases>&, env::t,
                                                           const Location&, const Location&,
                                                           const pt::TypeConstraint&, TypeExpr*);

// Typecheck the body of a newtype (an expression, or a suffix of function
// parameters together with a function body).
template <class A>
std::pair<A, TypeExpr*> type_newtype(env::t env, const pt::StrLoc& name,
                                     const std::function<std::pair<A, TypeExpr*>(env::t)>& type_body) {
  TypeExpr* ty = typetexp::valid_tyvar_name(name.txt) ? ctype::newvar(OptStr::of(name.txt)) : ctype::newvar();
  // Use [with_local_level_generalize] just for scoping
  return ctype::with_local_level_generalize(
      [&] {
        // Create a fake abstract type declaration for [name].
        const TypeDeclaration* decl = ctype::new_local_type(TypeOrigin{}, name.loc);
        long scope = ctype::create_scope();
        auto [id, new_env] = env::enter_type(static_cast<int>(scope), name.txt, decl, env);
        auto [result, exp_type] = type_body(new_env);
        // Replace every instance of this type constructor in the resulting
        // type.
        std::unordered_set<long> seen;
        std::function<void(TypeExpr*)> replace = [&](TypeExpr* t) {
          if (!seen.insert(get_id(t)).second) return;
          auto* tc = as<Tconstr>(get_desc(t));
          if (tc && tc->path->kind == Path::Kind::Pident && tc->path->id == id) link_type(t, ty);
          else iter_type_expr(replace, t);
        };
        TypeExpr* ety = subst::type_expr(subst::identity(), exp_type);
        replace(ety);
        return std::make_pair(result, ety);
      },
      [&](const std::pair<A, TypeExpr*>& r) { ctype::enforce_current_level(env, r.second); });
}
template std::pair<const tt::Expression*, TypeExpr*> type_newtype(
    env::t, const pt::StrLoc&, const std::function<std::pair<const tt::Expression*, TypeExpr*>(env::t)>&);
template std::pair<TypeFunctionResult, TypeExpr*> type_newtype(
    env::t, const pt::StrLoc&, const std::function<std::pair<TypeFunctionResult, TypeExpr*>(env::t)>&);

std::pair<Path::t, const ValueDescription*> type_ident(env::t env, Recarg recarg, const pt::LidLoc& lid) {
  auto [path, desc] = env::lookup_value(true, lid.loc, lid.txt, env);
  const TypeDesc* td = get_desc(desc->val_type);
  bool is_recarg = false;
  if (auto* tc = as<Tconstr>(td)) is_recarg = path::is_constructor_typath(tc->path);
  bool escape = (is_recarg && recarg == Recarg::Rejected) ||
                (!is_recarg && recarg == Recarg::Required &&
                 (td->kind == DescKind::Tvar || td->kind == DescKind::Tconstr));
  if (escape) raise_error(err(lid.loc, env, EK::Inlined_record_escape));
  return {path, desc};
}

std::pair<Path::t, const ValueDescription*> type_binding_op_ident(env::t env, const pt::StrLoc& s) {
  pt::LidLoc lid{Longident::lident(s.txt), s.loc};
  auto [path, desc] = type_ident(env, Recarg::Rejected, lid);
  switch (desc->val_kind.kind) {
    case ValueKind::Kind::Val_ivar: throw std::logic_error("Illegal name for instance variable");
    case ValueKind::Kind::Val_self:
      path = env::find_value_by_name(self_lid(desc->val_kind.cl_num), env).first;
      break;
    default: break;
  }
  return {path, desc};
}

// Returns the argument type and then the return type.
SplitFunctionTy split_function_ty(env::t env, TypeExpr* ty_expected, const ArgLabel& arg_label, bool has_poly,
                                  bool first, const InFunction& in_function) {
  TypeExpr* ty_fun = in_function.ty_fun.ty;
  Explanation explanation = in_function.ty_fun.explanation;
  const Location& loc = in_function.loc;
  bool separate = clflags::principal || env::has_local_constraints(env);
  ctype::FilteredArrow fa = ctype::with_local_level_generalize_structure_if(separate, [&] {
    // If [has_poly] is true then we rely on the later call to type_pat to
    // enforce the invariant that the parameter type be a [Tpoly] node
    TypeExpr* ie = ctype::instance(ty_expected);
    auto r = ctype::filter_arrow(env, false, ie, arg_label, has_poly);
    if (!r.ok) raise_error(error_of_filter_arrow_failure(loc, env, explanation, first, ty_fun, r.error));
    return r.value;
  });
  if (clflags::principal && !has_poly && !tpoly_is_mono(fa.ty_param) && get_level(fa.ty_param) < generic_level &&
      ctype::is_really_poly(env, fa.ty_param))
    prerr_warning(loc, not_principal("this higher-rank function"));
  TypeExpr* ty_param = fa.ty_param;
  if (!has_poly) {
    auto [ty, vars] = tpoly_get_poly(fa.ty_param);
    if (vars.empty()) ty_param = ty;
    else ty_param = ctype::with_level(generic_level, [&] { return ctype::instance_poly(vars, ty, true); });
  }
  return {fa, ty_param};
}

std::optional<ctype::FunctorView> split_function_mty(env::t env, TypeExpr* ty_expected, const ArgLabel& arg_label,
                                                     bool first, const InFunction& in_function) {
  return ctype::with_local_level_generalize_structure([&] {
    TypeExpr* ie = ctype::instance(ty_expected);
    auto r = ctype::filter_functor(env, ie, arg_label);
    if (!r.ok)
      raise_error(error_of_filter_arrow_failure(in_function.loc, env, in_function.ty_fun.explanation, first,
                                                in_function.ty_fun.ty, r.error));
    return r.value;
  });
}

}  // namespace cppcaml::typing::typecore
