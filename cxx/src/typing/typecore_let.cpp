// Port of typing/typecore.ml, part 6: match cases, effect cases, let
// bindings, binding operators, method calls and the toplevel entry points
// ("type_cases" to "type_expression").
#include "cppcaml/typing/subst.hpp"
#include "typecore_cases.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using XK = tt::ExpressionDesc::Kind;
using SXK = pt::ExpressionDesc::Kind;

// Typing of match cases
std::pair<Slice<const tt::Case*>, tt::Partial> type_cases(
    tt::PatternCategory category, env::t env, TypeExpr* ty_arg, const TypeExpected& ty_res_explained,
    const std::vector<std::optional<ContinuationVar>>* conts, bool check_if_total, const Location& loc,
    Slice<const pt::Case*> caselist) {
  TypeExpr* ty_res = ty_res_explained.ty;
  Explanation explanation = ty_res_explained.explanation;
  std::vector<std::pair<UntypedCase, const pt::Case*>> cl;
  for (auto* c : caselist) cl.push_back({parmatch::untyped_case(c), c});
  // Most of the work is done by [map_half_typed_cases].  All that's left is
  // to typecheck the guards and the cases.
  TypeBody<const pt::Case*, const tt::Case*> tb =
      [&](const pt::Case* const& c, const tt::Pattern* pat, env::t when_env, env::t ext_env,
          const std::optional<ContinuationVar>& cont, TypeExpr* ty_expected, TypeExpr* ty_infer, bool) {
        Ident::t c_cont = cont ? cont->id : nullptr;
        const tt::Expression* guard = nullptr;
        if (c->pc_guard)
          // The continuation is made inaccessible in the `when' expression
          // by typing it in [when_env], which does not bind it.
          guard = type_expect(when_env, c->pc_guard, mk_expected(predef::type_bool(), TypeForcingContext::When_guard));
        const tt::Expression* exp = type_expect(ext_env, c->pc_rhs, mk_expected(ty_expected, explanation));
        tt::Expression* rhs = make<tt::Expression>(*exp);
        rhs->exp_type = ty_infer;
        return static_cast<const tt::Case*>(make<tt::Case>(pat, c_cont, guard, rhs));
      };
  std::function<void(const SplitCases<const pt::Case*, const tt::Case*>&)> additional =
      [](const SplitCases<const pt::Case*, const tt::Case*>& cases) {
        std::vector<const tt::Case*> cs;
        for (auto& [case_with_pat, c] : cases) {
          tt::Case* c2 = make<tt::Case>(*c);
          c2->c_lhs = case_with_pat.pattern;
          cs.push_back(c2);
        }
        parmatch::check_ambiguous_bindings(cs);
      };
  auto [result, partial] = map_half_typed_cases<const pt::Case*, const tt::Case*>(
      &additional, conts, category, env, ty_arg, ty_res, loc, cl, tb, check_if_total);
  return {slice(result), partial};
}

// A version of [type_expect] over function cases instead of expressions.
FunctionCasesResult type_function_cases_expect(env::t env, TypeExpr* ty_expected, const Location& loc,
                                               Slice<const pt::Case*> cases, pt::Attributes attrs, bool first,
                                               const InFunction& in_function) {
  return builtin_attributes::warning_scope(attrs, [&] {
    SplitFunctionTy sft = split_function_ty(env, ty_expected, ArgLabel::nolabel(), false, first, in_function);
    auto [tcases, partial] = type_cases(tt::PatternCategory::Value, env, sft.ty_arg_mono,
                                        mk_expected(sft.filtered_arrow.ty_ret), nullptr, true, loc, cases);
    TypeExpr* ty_fun = ctype::instance(
        newgenty(tarrow(ArgLabel::nolabel(), sft.filtered_arrow.ty_param, sft.filtered_arrow.ty_ret, commu_ok())));
    TypeExpr* ie = ctype::instance(ty_expected);
    unify_exp_types(loc, env, ty_fun, ie);
    return FunctionCasesResult{tcases, partial, ty_fun};
  });
}

Slice<const tt::Case*> type_effect_cases(tt::PatternCategory category, env::t env, const TypeExpected& ty_res_explained,
                                         const Location& loc, Slice<const pt::Case*> caselist,
                                         const std::vector<const pt::Pattern*>& conts) {
  TypeExpr* ty_res = ty_res_explained.ty;
  // remember original level
  return ctype::with_local_level([&] {
    // Create a locally abstract type for effect type.
    const TypeDeclaration* decl = ctype::new_local_type(TypeOrigin{}, loc);
    long scope = ctype::create_scope();
    std::string_view name = ctype::get_new_abstract_name(env, OCAML_LIT("%eff"));
    Ident::t id = Ident::create_scoped(static_cast<int>(scope), name);
    env::t new_env = env::add_type(false, id, decl, env);
    TypeExpr* ty_eff = newgenty(tconstr(Path::pident(id), {}, make<MemoRef>(mnil())));
    // (new_env, Predef.type_eff ty_eff, Predef.type_continuation ty_eff ty_res): right to left
    TypeExpr* ty_cont = predef::type_continuation(ty_eff, ty_res);
    TypeExpr* ty_arg = predef::type_eff(ty_eff);
    std::vector<std::optional<ContinuationVar>> cs;
    for (auto* c : conts) cs.push_back(type_continuation_pat(env, ty_cont, c));
    return type_cases(category, new_env, ty_arg, ty_res_explained, &cs, false, loc, caselist).first;
  });
}

using PatList = std::vector<std::pair<const tt::Pattern*, TypeExpr*>>;
using ExpList = std::vector<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>>;

static Slice<const tt::ValueBinding*> value_bindings_of_pat_exp_lists(const PatList& pat_list,
                                                                      const ExpList& exp_list,
                                                                      Slice<const pt::ValueBinding*> spat_sexp_list) {
  if (pat_list.size() != exp_list.size()) throw std::invalid_argument("List.combine");
  if (pat_list.size() != spat_sexp_list.size()) throw std::invalid_argument("List.map2");
  std::vector<const tt::ValueBinding*> l;
  for (std::size_t k = 0; k < pat_list.size(); ++k)
    // vb_rec_kind will be computed later for recursive bindings
    l.push_back(make<tt::ValueBinding>(pat_list[k].first, exp_list[k].first, tt::RecursiveBindingKind::Dynamic,
                                       spat_sexp_list[k]->pvb_attributes, spat_sexp_list[k]->pvb_loc));
  return slice(l);
}

static ExpList type_let_def_wrap_warnings(
    env::CheckFn check, env::CheckFn check_strict, bool is_recursive, env::t exp_env, env::t new_env,
    Slice<const pt::ValueBinding*> spat_sexp_list, const std::vector<pt::Attributes>& attrs_list,
    const PatList& pat_list, const std::vector<PatternVariable>& pvs,
    const std::function<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>(
        env::t, const pt::ValueBinding*, TypeExpr*)>& type_def) {
  if (!check) check = [](std::string s) { return warnings::Warning::with_s(WK::Unused_var, s); };
  if (!check_strict) check_strict = [](std::string s) { return warnings::Warning::with_s(WK::Unused_var_strict, s); };
  // the fake let-declaration introduced by fun ?(x = e) -> ...
  bool is_fake_let = false;
  if (spat_sexp_list.size() == 1)
    if (auto* m = as<pt::Pexp_match>(spat_sexp_list[0]->pvb_expr->pexp_desc))
      if (auto* id = as<pt::Pexp_ident>(m->exp->pexp_desc))
        if (id->lid.txt->kind == Longident::Kind::Lident && id->lid.txt->s == "*opt*") is_fake_let = true;
  if (is_fake_let) check = check_strict;
  bool warn_about_unused_bindings = false;
  for (auto& attrs : attrs_list) {
    bool b = builtin_attributes::warning_scope(
        attrs,
        [&] {
          return warnings::is_active(check("")) || warnings::is_active(check_strict("")) ||
                 (is_recursive && warnings::is_active(39));
        },
        false);
    if (b) {
      warn_about_unused_bindings = true;
      break;
    }
  }
  bool all_fun = true;
  for (auto* vb : spat_sexp_list)
    if (vb->pvb_expr->pexp_desc->kind != SXK::Pexp_function) all_fun = false;
  if (!is_recursive && all_fun) {
    // Add ghost bindings to help detecting missing "rec" keywords.
    if (spat_sexp_list.empty()) throw std::logic_error("type_let_def_wrap_warnings");
    exp_env = maybe_add_pattern_variables_ghost(spat_sexp_list[0]->pvb_loc, exp_env, pvs);
  }
  // Algorithm to detect unused declarations in recursive bindings (see
  // typecore.ml): value_used events during the definitions are recorded in
  // the current definition's slot, and replayed when one of its identifiers
  // is used afterwards.
  using Slot = std::shared_ptr<std::vector<Uid>>;
  auto current_slot = std::make_shared<Slot>();
  auto rec_needed = std::make_shared<bool>(false);
  if (attrs_list.size() != pat_list.size()) throw std::invalid_argument("List.map2");
  std::vector<Slot> slots;
  for (std::size_t k = 0; k < pat_list.size(); ++k) {
    Slot slot = builtin_attributes::warning_scope(
        attrs_list[k],
        [&]() -> Slot {
          if (!warn_about_unused_bindings) return nullptr;
          auto some_used = std::make_shared<bool>(false);
          Slot sl = std::make_shared<std::vector<Uid>>();
          for (Ident::t id : tt::pat_bound_idents(pat_list[k].first)) {
            const ValueDescription* vd = env::find_value(Path::pident(id), new_env);
            std::string name(ident::name(id));
            auto used = std::make_shared<bool>(false);
            if (!(name.empty() || name[0] == '_' || name[0] == '#')) {
              Location vloc = vd->val_loc;
              add_delayed_check([used, some_used, vloc, name, check, check_strict] {
                if (!*used) location::prerr_warning(vloc, (*some_used ? check_strict : check)(name));
              });
            }
            Uid vuid = vd->val_uid;
            env::set_value_used_callback(vd, [current_slot, rec_needed, sl, used, some_used, vuid] {
              if (*current_slot) {
                (*current_slot)->insert((*current_slot)->begin(), vuid);
                *rec_needed = true;
              } else {
                std::vector<Uid> l = *sl;  // get_ref slot
                sl->clear();
                for (const Uid& u : l) env::mark_value_used(u);
                *used = true;
                *some_used = true;
              }
            });
          }
          return sl;
        },
        false);
    slots.push_back(slot);
  }
  if (spat_sexp_list.size() != pat_list.size()) throw std::invalid_argument("List.map2");
  ExpList exp_list;
  for (std::size_t k = 0; k < spat_sexp_list.size(); ++k) {
    if (is_recursive) *current_slot = slots[k];
    exp_list.push_back(type_def(exp_env, spat_sexp_list[k], pat_list[k].second));
  }
  *current_slot = nullptr;
  if (is_recursive && !*rec_needed) {
    const pt::ValueBinding* vb = spat_sexp_list[0];
    // See PR#6677
    builtin_attributes::warning_scope(
        vb->pvb_attributes, [&] { prerr_warning(vb->pvb_pat->ppat_loc, WK::Unused_rec_flag); }, false);
  }
  return exp_list;
}

static std::pair<PatList, ExpList> type_let_exps(const env::CheckFn& check, const env::CheckFn& check_strict,
                                                 bool is_recursive, env::t exp_env, env::t new_env,
                                                 const std::vector<pt::Attributes>& attrs_list,
                                                 const std::vector<const tt::Pattern*>& pats,
                                                 const std::vector<PatternVariable>& pvs,
                                                 Slice<const pt::ValueBinding*> spat_sexp_list) {
  // Instantiate the pattern types: the instantiated type is the pattern
  // type, the non-instantiated one the expected type in check_let_univars.
  PatList pat_list;
  for (auto* pat : pats) {
    tt::Pattern* p2 = make<tt::Pattern>(*pat);
    p2->pat_type = ctype::instance(pat->pat_type);
    pat_list.push_back({p2, pat->pat_type});
  }
  ExpList exp_list = type_let_def_wrap_warnings(
      check, check_strict, is_recursive, exp_env, new_env, spat_sexp_list, attrs_list, pat_list, pvs,
      [](env::t exp_env2, const pt::ValueBinding* vb, TypeExpr* expected_ty)
          -> std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>> {
        const pt::Expression* sexp = vb_exp_constraint(vb);
        // Type annotations of the form ['a ... 'c. tau] on patterns
        // introduce polytypes: instantiate them, and check that the
        // instantiated univars are generalized (check_let_univars).
        if (auto* p = as<Tpoly>(get_desc(expected_ty))) {
          auto [vars, ty2] = ctype::with_local_level_generalize_structure_if_principal(
              [&] { return ctype::instance_poly_fixed(p->vars, p->body, true); });
          const tt::Expression* exp = builtin_attributes::warning_scope(
              vb->pvb_attributes, [&] { return type_expect(exp_env2, sexp, mk_expected(ty2)); });
          return {exp, vars};
        }
        const tt::Expression* exp = builtin_attributes::warning_scope(
            vb->pvb_attributes, [&] { return type_expect(exp_env2, sexp, mk_expected(expected_ty)); });
        return {exp, std::nullopt};
      });
  return {pat_list, exp_list};
}

std::pair<Slice<const tt::ValueBinding*>, env::t> type_let_rec(bool reset_tyvarenv, env::t env,
                                                               Slice<const pt::ValueBinding*> spat_sexp_list,
                                                               const env::CheckFn& check,
                                                               const env::CheckFn& check_strict) {
  std::vector<std::pair<pt::Attributes, const pt::Pattern*>> spatl;
  for (auto* vb : spat_sexp_list) spatl.push_back(vb_pat_constraint(vb));
  std::vector<pt::Attributes> attrs_list;
  for (auto& x : spatl) attrs_list.push_back(x.first);
  // Recursive patterns can only consist of (possibly annotated) variables.
  for (auto* vb : spat_sexp_list)
    if (!is_var_pat(vb->pvb_pat)) raise_error(err(vb->pvb_pat->ppat_loc, env, EK::Illegal_letrec_pat));
  struct R {
    PatList pat_list;
    ExpList exp_list;
    env::t new_env;
  };
  R r = ctype::with_local_level_generalize(
      [&] {
        // We must reset the tyvarenv in this local region since it resets
        // the global level
        if (reset_tyvarenv) typetexp::ty_var_env::reset();
        struct P {
          std::vector<const tt::Pattern*> pat_list;
          env::t new_env;
          std::vector<std::function<void()>> force;
          std::vector<PatternVariable> pvs;
        };
        P p = ctype::with_local_level_generalize_structure_if_principal([&] {
          // Typecheck the patterns
          std::vector<TypeExpr*> nvs;
          for (std::size_t k = 0; k < spatl.size(); ++k) nvs.push_back(ctype::newvar());
          TypePatternListResult tp = ctype::with_local_level_generalize([&] {
            return type_pattern_list(tt::PatternCategory::Value, ExistentialRestriction::In_rec, env, spatl, nvs,
                                     ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_rejected});
          });
          // Approximate the type of the recursive binding
          if (tp.patl.size() != spat_sexp_list.size()) throw std::invalid_argument("List.iter2");
          for (std::size_t k = 0; k < tp.patl.size(); ++k) {
            TypeExpr* pat_type = tp.patl[k]->pat_type;
            if (auto* poly = as<Tpoly>(get_desc(pat_type)))
              pat_type = ctype::instance_poly(poly->vars, poly->body, true);
            const pt::Expression* bound_expr = vb_exp_constraint(spat_sexp_list[k]);
            type_approx(env, bound_expr, pat_type);
          }
          return P{tp.patl, tp.env, tp.pattern_forces, tp.pvs};
        });
        env::t new_env = add_let_pattern_vars(p.new_env, p.pvs, p.force);
        auto [pat_list, exp_list] =
            type_let_exps(check, check_strict, true, new_env, new_env, attrs_list, p.pat_list, p.pvs, spat_sexp_list);
        return R{pat_list, exp_list, new_env};
      },
      [&](const R& r) { do_relaxed_value_restriction(env, r.pat_list, r.exp_list); });
  check_let_univars(env, r.pat_list, r.exp_list);
  return {value_bindings_of_pat_exp_lists(r.pat_list, r.exp_list, spat_sexp_list), r.new_env};
}

std::pair<Slice<const tt::ValueBinding*>, env::t> type_let_nonrec(
    bool reset_tyvarenv, std::optional<ExistentialRestriction> existential_context,
    const ModulePatternsRestriction& allow_modules, env::t env, Slice<const pt::ValueBinding*> spat_sexp_list,
    const env::CheckFn& check, const env::CheckFn& check_strict) {
  std::vector<std::pair<pt::Attributes, const pt::Pattern*>> spatl;
  for (auto* vb : spat_sexp_list) spatl.push_back(vb_pat_constraint(vb));
  std::vector<pt::Attributes> attrs_list;
  for (auto& x : spatl) attrs_list.push_back(x.first);
  struct R {
    PatList pat_list;
    ExpList exp_list;
    env::t new_env;
    ModuleVariables mvs;
  };
  R r = ctype::with_local_level_generalize(
      [&] {
        // We must reset the tyvarenv in this local region since it resets
        // the global level
        if (reset_tyvarenv) typetexp::ty_var_env::reset();
        TypePatternListResult tp = ctype::with_local_level_generalize_structure_if_principal([&] {
          std::vector<TypeExpr*> nvs;
          for (std::size_t k = 0; k < spatl.size(); ++k) nvs.push_back(ctype::newvar());
          TypePatternListResult res =
              type_pattern_list(tt::PatternCategory::Value, existential_context, env, spatl, nvs, allow_modules);
          // Polymorphic variant processing
          for (auto* pat : res.patl)
            if (has_variants(pat)) {
              parmatch::pressure_variants(env, {pat});
              finalize_variants(pat);
            }
          return res;
        });
        // Note [add_module_variables after checking expressions]: the
        // module variables are added after the expressions are typed.
        env::t new_env = add_let_pattern_vars(tp.env, tp.pvs, tp.pattern_forces);
        auto [pat_list, exp_list] =
            type_let_exps(check, check_strict, false, env, new_env, attrs_list, tp.patl, tp.pvs, spat_sexp_list);
        // Do exhaustiveness checks on patterns
        if (pat_list.size() != spatl.size() || exp_list.size() != spatl.size())
          throw std::invalid_argument("List.map2");
        for (std::size_t k = 0; k < pat_list.size(); ++k) {
          builtin_attributes::warning_scope(spatl[k].first, [&] {
            const tt::Pattern* pat = pat_list[k].first;
            auto* c = make<tt::Case>(pat, nullptr, nullptr, exp_list[k].first);
            std::vector<parmatch::TypedCase> cases{parmatch::typed_case(c)};
            check_partial(ctype::get_current_level(), env, pat->pat_type, pat->pat_loc, cases);
            return 0;
          });
        }
        return R{pat_list, exp_list, new_env, tp.mvs};
      },
      [&](const R& r) { do_relaxed_value_restriction(env, r.pat_list, r.exp_list); });
  check_let_univars(env, r.pat_list, r.exp_list);
  Slice<const tt::ValueBinding*> l = value_bindings_of_pat_exp_lists(r.pat_list, r.exp_list, spat_sexp_list);
  for (auto* vb : l)
    if (pattern_needs_partial_application_check(vb->vb_pat)) check_partial_application(false, vb->vb_expr);
  // See Note [add_module_variables after checking expressions]
  env::t new_env = add_module_variables(r.new_env, r.mvs);
  return {l, new_env};
}

std::pair<const tt::Expression*, Slice<const tt::BindingOp*>> type_andops(
    env::t env, const pt::Expression* sarg, Slice<const pt::BindingOp*> sands, TypeExpr* expected_ty) {
  std::function<std::pair<const tt::Expression*, std::vector<const tt::BindingOp*>>(
      const pt::Expression*, std::size_t, TypeExpr*)>
      loop;
  // rev_sands = List.rev sands: element k of rev_sands is sands[n-1-k]
  loop = [&](const pt::Expression* let_sarg, std::size_t k,
             TypeExpr* expected_ty2) -> std::pair<const tt::Expression*, std::vector<const tt::BindingOp*>> {
    if (k == sands.size()) return {type_expect(env, let_sarg, mk_expected(expected_ty2)), {}};
    const pt::BindingOp* b = sands[sands.size() - 1 - k];
    const pt::StrLoc& sop = b->pbop_op;
    struct R {
      Path::t op_path;
      const ValueDescription* op_desc;
      TypeExpr* op_type;
      TypeExpr* ty_arg;
      TypeExpr* ty_rest;
      TypeExpr* ty_result;
    };
    R r = ctype::with_local_level_generalize_structure_if_principal([&] {
      auto [op_path, op_desc] = type_binding_op_ident(env, sop);
      TypeExpr* op_type = ctype::instance(op_desc->val_type);
      TypeExpr* ty_arg = ctype::newvar();
      TypeExpr* ty_rest = ctype::newvar();
      TypeExpr* ty_result = ctype::newvar();
      TypeExpr* ty_rest_fun =
          ctype::newty(tarrow(ArgLabel::nolabel(), ctype::newmono(ty_arg), ty_result, commu_ok()));
      TypeExpr* ty_op = ctype::newty(tarrow(ArgLabel::nolabel(), ctype::newmono(ty_rest), ty_rest_fun, commu_ok()));
      try {
        ctype::unify(env, op_type, ty_op);
      } catch (const ctype::Unify& u) {
        Error e = err(sop.loc, env, EK::Andop_type_clash);
        e.name = std::string(sop.txt);
        e.trace = u.err;
        raise_error(e);
      }
      return R{op_path, op_desc, op_type, ty_arg, ty_rest, ty_result};
    });
    auto [let_arg, rest] = loop(let_sarg, k + 1, r.ty_rest);
    const tt::Expression* exp = type_expect(env, b->pbop_exp, mk_expected(r.ty_arg));
    try {
      TypeExpr* e2 = ctype::instance(expected_ty2);
      TypeExpr* e1 = ctype::instance(r.ty_result);
      ctype::unify(env, e1, e2);
    } catch (const ctype::Unify& u) {
      Error e = err(b->pbop_loc, env, EK::Bindings_type_clash);
      e.trace = u.err;
      raise_error(e);
    }
    auto* andop = make<tt::BindingOp>(r.op_path, sop, r.op_desc, r.op_type, exp, b->pbop_loc);
    rest.insert(rest.begin(), andop);
    return {let_arg, rest};
  };
  auto [let_arg, rev_ands] = loop(sarg, 0, expected_ty);
  std::reverse(rev_ands.begin(), rev_ands.end());
  return {let_arg, slice(rev_ands)};
}

// Typing of method call
SendResult type_send(env::t env, const Location&, Explanation explanation, const pt::Expression* e,
                     std::string_view met) {
  const tt::Expression* obj = type_exp(env, e);
  auto undefined_self_method = [&](const StrMap<Ident::t>& meths) {
    // Meths.fold (fun lab _ acc -> lab :: acc) meths []
    std::vector<std::string> valid_methods;
    meths.iter([&](std::string_view lab, Ident::t) { valid_methods.insert(valid_methods.begin(), std::string(lab)); });
    Error er = err(e->pexp_loc, env, EK::Undefined_self_method);
    er.name = std::string(met);
    er.names = valid_methods;
    raise_error(er);
  };
  auto* i = as<tt::Texp_ident>(obj->exp_desc);
  if (i && i->vd->val_kind.kind == ValueKind::Kind::Val_self) {
    const ValueKind& vk = i->vd->val_kind;
    Ident::t id;
    TypeExpr* typ;
    if (!vk.self_virtual) {
      const Ident::t* f = vk.meths->find_opt(met);
      if (!f) undefined_self_method(*vk.meths);
      id = *f;
      typ = method_type(met, vk.sign);
    } else if (const Ident::t* f = vk.meths->find_opt(met)) {
      id = *f;
      typ = method_type(met, vk.sign);
    } else {
      id = Ident::create_local(met);
      typ = ctype::newvar();
      *vk.meths = vk.meths->add(zborrow(met), id);
      ctype::add_method(env, met, PrivateFlag::Private, VirtualFlag::Virtual, typ, vk.sign);
      // (the Undeclared_virtual_method warning is not emitted)
    }
    tt::Meth m{tt::Meth::Kind::Tmeth_val};
    m.id = id;
    return {obj, m, typ};
  }
  if (i && i->vd->val_kind.kind == ValueKind::Kind::Val_anc) {
    const ValueKind& vk = i->vd->val_kind;
    const Ident::t* f = vk.meths->find_opt(met);
    if (!f) undefined_self_method(*vk.meths);
    TypeExpr* typ = method_type(met, vk.sign);
    Path::t self_path =
        env::find_value_by_name(Longident::lident(zborrow(std::string("self-") + std::string(vk.cl_num))), env).first;
    tt::Meth m{tt::Meth::Kind::Tmeth_ancestor};
    m.id = *f;
    m.path = self_path;
    return {obj, m, typ};
  }
  TypeExpr* ty;
  try {
    ty = ctype::filter_method(env, met, obj->exp_type);
  } catch (const ctype::FilterMethodFailed& f) {
    using FK = ctype::FilterMethodFailed::Kind;
    switch (f.kind) {
      case FK::Unification_error: {
        Error er = err(e->pexp_loc, env, EK::Expr_type_clash);
        er.trace = f.err;
        er.explanation = explanation;
        raise_error(er);
      }
      case FK::Not_an_object: {
        Error er = err(e->pexp_loc, env, EK::Not_an_object);
        er.ty = f.ty;
        er.explanation = explanation;
        raise_error(er);
      }
      case FK::Not_a_method: {
        Error er = err(e->pexp_loc, env, EK::Undefined_method);
        if (auto* o = as<Tobject>(get_desc(ctype::expand_head(env, obj->exp_type)))) {
          auto [fields, rest] = ctype::flatten_fields(o->fields);
          (void)rest;
          std::vector<std::string> valid;
          for (auto& fe : fields)
            if (field_kind_repr(fe.kind) == FieldKindView::Fpublic)
              valid.insert(valid.begin(), std::string(fe.name));
          er.names = valid;
          er.has_names = true;
        }
        // We embed the entire object to avoid rebuilding it in recovery-case.
        er.texp = obj;
        er.name = std::string(met);
        raise_error(er);
      }
    }
    throw;
  }
  tt::Meth m{tt::Meth::Kind::Tmeth_name};
  m.name = zborrow(met);
  return {obj, m, ty};
}

// Typing of toplevel bindings
TypeBindingResult type_binding(env::t env, RecFlag rec_flag, Slice<const pt::ValueBinding*> spat_sexp_list) {
  env::CheckFn check = [](std::string s) { return warnings::Warning::with_s(WK::Unused_value_declaration, s); };
  env::CheckFn check_strict = check;
  if (rec_flag == RecFlag::Recursive) {
    auto [vbs, e] = type_let_rec(true, env, spat_sexp_list, check, check_strict);
    return {vbs, e};
  }
  auto [vbs, e] = type_let_nonrec(true, ExistentialRestriction::At_toplevel,
                                  ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_rejected}, env,
                                  spat_sexp_list, check, check_strict);
  return {vbs, e};
}

TypeBindingResult type_let(std::optional<ExistentialRestriction> existential_context, env::t env, RecFlag rec_flag,
                           Slice<const pt::ValueBinding*> spat_sexp_list) {
  bool reset_tyvarenv = existential_context == ExistentialRestriction::At_toplevel;
  if (rec_flag == RecFlag::Recursive) {
    auto [vbs, e] = type_let_rec(reset_tyvarenv, env, spat_sexp_list);
    return {vbs, e};
  }
  auto [vbs, e] = type_let_nonrec(reset_tyvarenv, existential_context,
                                  ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_rejected}, env,
                                  spat_sexp_list);
  return {vbs, e};
}

// Typing of toplevel expressions
const tt::Expression* type_expression(env::t env, const pt::Expression* sexp) {
  const tt::Expression* exp = ctype::with_local_level_generalize(
      [&] {
        typetexp::ty_var_env::reset();
        return type_exp(env, sexp);
      },
      [&](const tt::Expression* e) { may_lower_contravariant(env, e); });
  if (auto* i = as<pt::Pexp_ident>(sexp->pexp_desc)) {
    // Special case for keeping type variables when looking-up a variable
    auto [path, desc] = env::lookup_value(false, sexp->pexp_loc, i->lid.txt, env);
    (void)path;
    tt::Expression* e2 = make<tt::Expression>(*exp);
    e2->exp_type = desc->val_type;
    return e2;
  }
  return exp;
}

// let () = type_argument' := type_argument
std::function<const tt::Expression*(env::t, const pt::Expression*, TypeExpr*, TypeExpr*, Explanation)>
    type_argument_forward = [](env::t env, const pt::Expression* sarg, TypeExpr* t1, TypeExpr* t2, Explanation) {
      return type_argument(env, sarg, t1, t2);
    };

}  // namespace cppcaml::typing::typecore
