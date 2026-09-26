// typecore.ml's map_half_typed_cases (shared by type_function in
// typecore_fun.cpp and type_cases in typecore_let.cpp): type the patterns of
// a list of cases at an increased level, then hand each case to [type_body].
#pragma once

#include "typecore_exp.hpp"

namespace cppcaml::typing::typecore {

template <class CaseData, class Ret>
using SplitCases = std::vector<std::pair<parmatch::TypedCase, Ret>>;

// Used to split patterns into value cases and exception cases.
template <class CaseData, class Ret>
std::pair<SplitCases<CaseData, Ret>, SplitCases<CaseData, Ret>> split_half_typed_cases(
    env::t env, const std::vector<std::pair<HalfTypedCase<CaseData>, Ret>>& zipped_cases) {
  SplitCases<CaseData, Ret> vals, exns;
  // List.fold_right: the last case is processed first
  for (std::size_t k = zipped_cases.size(); k-- > 0;) {
    const auto& [htc, data] = zipped_cases[k];
    const tt::Pattern* pat = htc.typed_pat;
    auto [vp, ep] = tt::split_pattern(pat);
    if (vp && ep && htc.untyped_case.has_guard)
      raise_error(err(pat->pat_loc, env, EK::Mixed_value_and_exception_patterns_under_guard));
    if (vp)
      vals.insert(vals.begin(),
                  {parmatch::TypedCase{vp, htc.untyped_case.has_guard, htc.untyped_case.needs_refute}, data});
    if (ep)
      exns.insert(exns.begin(),
                  {parmatch::TypedCase{ep, htc.untyped_case.has_guard, htc.untyped_case.needs_refute}, data});
  }
  return {vals, exns};
}

// type_body case_data pat ~when_env ~ext_env ~cont ~ty_expected ~ty_infer ~contains_gadt
template <class CaseData, class Ret>
using TypeBody = std::function<Ret(const CaseData&, const tt::Pattern*, env::t when_env, env::t ext_env,
                                   const std::optional<ContinuationVar>& cont, TypeExpr* ty_expected,
                                   TypeExpr* ty_infer, bool contains_gadt)>;

template <class CaseData, class Ret>
std::pair<std::vector<Ret>, tt::Partial> map_half_typed_cases(
    const std::function<void(const SplitCases<CaseData, Ret>&)>* additional_checks_for_split_cases,
    const std::vector<std::optional<ContinuationVar>>* conts, tt::PatternCategory category, env::t env,
    TypeExpr* ty_arg, TypeExpr* ty_res, const Location& loc,
    const std::vector<std::pair<UntypedCase, CaseData>>& caselist, const TypeBody<CaseData, Ret>& type_body,
    bool check_if_total) {
  // ty_arg is _fully_ generalized
  bool contains_polyvars = false, may_contain_gadts_ = false, may_contain_modules_ = false;
  for (auto& c : caselist) contains_polyvars = contains_polyvars || contains_polymorphic_variant(c.first.pattern);
  bool erase_either = contains_polyvars && contains_variant_either(ty_arg);
  for (auto& c : caselist) may_contain_gadts_ = may_contain_gadts_ || may_contain_gadts(c.first.pattern);
  for (auto& c : caselist) may_contain_modules_ = may_contain_modules_ || may_contain_modules(c.first.pattern);
  bool create_inner_level = may_contain_gadts_ || may_contain_modules_;
  if ((may_contain_gadts_ || erase_either) && !clflags::principal) ty_arg = ctype::duplicate_type(ty_arg);
  std::function<bool(const pt::Pattern*)> is_var = [&](const pt::Pattern* spat) {
    const pt::PatternDesc* d = spat->ppat_desc;
    if (d->kind == pt::PatternDesc::Kind::Ppat_any || d->kind == pt::PatternDesc::Kind::Ppat_var) return true;
    if (auto* a = pt::as<pt::Ppat_alias>(d)) return is_var(a->pat);
    return false;
  };
  bool needs_exhaust_check = true;
  if (caselist.size() == 1)
    needs_exhaust_check = caselist[0].first.needs_refute || !is_var(caselist[0].first.pattern);
  long outer_level = ctype::get_current_level();
  using Result = std::pair<std::vector<Ret>, tt::Partial>;
  return ctype::with_local_level_iter_if(
      create_inner_level,
      [&]() -> std::pair<Result, std::vector<TypeExpr*>> {
        long lev = ctype::get_current_level();
        // The check for scope escape is done together with the check for
        // GADT-induced existentials by [with_local_level_iter_if].
        ModulePatternsRestriction allow_modules{ModulePatternsRestriction::Kind::Modules_rejected};
        if (may_contain_modules_)
          allow_modules =
              ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_allowed, static_cast<int>(lev)};
        std::optional<bool> take_partial_instance;
        if (erase_either) take_partial_instance = false;
        auto cont_of = [&](std::size_t k) -> std::optional<ContinuationVar> {
          if (!conts) return std::nullopt;
          if (conts->size() != caselist.size()) throw std::invalid_argument("List.map2");
          return (*conts)[k];
        };
        struct Propagated {
          std::vector<HalfTypedCase<CaseData>> half_typed_cases;
          TypeExpr* ty_res;
          std::function<env::t(env::t)> do_copy_types;
          TypeExpr* ty_arg2;
        };
        // propagation of the argument
        Propagated pr = ctype::with_local_level_generalize([&] {
          std::vector<std::function<void()>> pattern_force;  // head first
          std::vector<HalfTypedCase<CaseData>> half_typed_cases;
          for (std::size_t k = 0; k < caselist.size(); ++k) {
            std::optional<ContinuationVar> cont = cont_of(k);
            HalfTypedCase<CaseData> htc = ctype::with_local_level_generalize_structure_if_principal([&] {
              // propagation of pattern
              TypeExpr* ty_arg1 = ctype::with_local_level_generalize_structure(
                  [&] { return ctype::instance(ty_arg, take_partial_instance); });
              TypePatternResult tp =
                  type_pattern(category, lev, env, caselist[k].first.pattern, ty_arg1, cont, allow_modules);
              std::vector<std::function<void()>> pf = tp.pattern_forces;
              pf.insert(pf.end(), pattern_force.begin(), pattern_force.end());
              pattern_force = pf;
              return HalfTypedCase<CaseData>{tp.pat,  ty_arg1, caselist[k].first,
                                             caselist[k].second, tp.env, tp.pvs, tp.mvs,
                                             contains_gadt(as_comp_pattern(category, tp.pat))};
            });
            // Ensure that no ambivalent pattern type escapes its branch
            check_scope_escape(htc.typed_pat->pat_loc, env, outer_level, htc.pat_type_for_unif);
            tt::Pattern* p2 = make<tt::Pattern>(*htc.typed_pat);
            p2->pat_type = ctype::instance(htc.typed_pat->pat_type);
            htc.typed_pat = p2;
            half_typed_cases.push_back(htc);
          }
          std::vector<const tt::Pattern*> patl;
          for (auto& h : half_typed_cases) patl.push_back(h.typed_pat);
          bool does_contain_gadt = false;
          for (auto& h : half_typed_cases) does_contain_gadt = does_contain_gadt || h.contains_gadt;
          TypeExpr* ty_res2 = ty_res;
          std::function<env::t(env::t)> do_copy_types = [](env::t e) { return e; };
          if (does_contain_gadt && !clflags::principal) {
            // (duplicate_type ty_res, Env.make_copy_of_types env): right to left
            do_copy_types = env::make_copy_of_types(env);
            ty_res2 = ctype::duplicate_type(ty_res);
          }
          // Unify all cases (delayed to keep it order-free)
          TypeExpr* ty_arg2 = ctype::newvar();
          auto unify_pats = [&](TypeExpr* ty) {
            for (auto& h : half_typed_cases) unify_pat_types(h.typed_pat->pat_loc, env, h.pat_type_for_unif, ty);
          };
          unify_pats(ty_arg2);
          // Check for polymorphic variants to close
          bool any_variants = false;
          for (auto* p : patl) any_variants = any_variants || has_variants(p);
          if (any_variants) {
            std::vector<const tt::Pattern*> cps;
            for (auto* p : patl) cps.push_back(as_comp_pattern(category, p));
            parmatch::pressure_variants_in_computation_pattern(env, cps);
            for (auto* p : patl) finalize_variants(p);
          }
          // `Contaminating' unifications start here
          for (auto& f : pattern_force) f();
          // Post-processing and generalization
          if (take_partial_instance) unify_pats(ctype::instance(ty_arg));
          for (auto& h : half_typed_cases)
            for (auto& pv : h.pat_vars) ctype::enforce_current_level(env, pv.pv_type);
          return Propagated{half_typed_cases, ty_res2, do_copy_types, ty_arg2};
        });
        // type bodies
        TypeExpr* ty_res_inst = ctype::instance(pr.ty_res);
        // Why is it needed to keep the level of result raised ?
        std::vector<Ret> result = ctype::with_local_level_if_principal(
            [&] {
              std::vector<Ret> out;
              for (std::size_t k = 0; k < pr.half_typed_cases.size(); ++k) {
                std::optional<ContinuationVar> cont = cont_of(k);
                const HalfTypedCase<CaseData>& htc = pr.half_typed_cases[k];
                env::t ext_env = htc.contains_gadt ? pr.do_copy_types(htc.branch_env) : htc.branch_env;
                // Before handing off the cases to the callback, first set up
                // the branch environments by adding the variables (and module
                // variables) from the patterns.
                std::vector<PatternVariable> cont_vars, pvs;
                for (auto& pv : htc.pat_vars)
                  (pv.pv_kind == PatternVariableKind::Continuation_var ? cont_vars : pvs).push_back(pv);
                env::t when_env = add_pattern_variables(ext_env, pvs);
                when_env = add_module_variables(when_env, htc.module_vars);
                env::t ext_env2 = add_pattern_variables(when_env, cont_vars);
                // Take a generic copy of [ty_res] again to allow propagation
                // of type information from preceding branches
                TypeExpr* ty_expected = htc.contains_gadt && !clflags::principal ? ctype::duplicate_type(pr.ty_res)
                                                                                  : pr.ty_res;
                out.push_back(type_body(htc.case_data, htc.typed_pat, when_env, ext_env2, cont, ty_expected,
                                        ty_res_inst, htc.contains_gadt));
              }
              return out;
            },
            [](const std::vector<Ret>&) {});
        bool do_init = may_contain_gadts_ || needs_exhaust_check;
        // Hack: use for_saving to copy variables too
        TypeExpr* ty_arg_check =
            do_init ? subst::type_expr(subst::for_saving(subst::identity()), pr.ty_arg2) : pr.ty_arg2;
        // Split the cases into val and exn cases so we can do the
        // appropriate checks for exhaustivity and unused variables.
        SplitCases<CaseData, Ret> val_cases_with_result, exn_cases_with_result;
        if (category == tt::PatternCategory::Value) {
          for (std::size_t k = 0; k < pr.half_typed_cases.size(); ++k) {
            const auto& htc = pr.half_typed_cases[k];
            val_cases_with_result.push_back(
                {parmatch::TypedCase{htc.typed_pat, htc.untyped_case.has_guard, htc.untyped_case.needs_refute},
                 result[k]});
          }
        } else {
          std::vector<std::pair<HalfTypedCase<CaseData>, Ret>> zipped;
          for (std::size_t k = 0; k < pr.half_typed_cases.size(); ++k)
            zipped.push_back({pr.half_typed_cases[k], result[k]});
          std::tie(val_cases_with_result, exn_cases_with_result) = split_half_typed_cases(env, zipped);
        }
        std::vector<parmatch::TypedCase> val_cases, exn_cases;
        for (auto& c : val_cases_with_result) val_cases.push_back(c.first);
        for (auto& c : exn_cases_with_result) exn_cases.push_back(c.first);
        if (val_cases.empty() && !exn_cases.empty()) raise_error(err(loc, env, EK::No_value_clauses));
        tt::Partial partial = check_if_total ? check_partial(lev, env, ty_arg_check, loc, val_cases)
                                             : tt::Partial::Partial;
        auto half = pr.half_typed_cases;
        auto unused_check = [half, category, lev, env, ty_arg_check, val_cases, exn_cases](bool delayed) {
          for (auto& h : half) check_absent_variant(h.branch_env, as_comp_pattern(category, h.typed_pat));
          ctype::with_level_if(delayed, lev, [&] {
            check_unused(lev, env, ty_arg_check, val_cases);
            check_unused(lev, env, predef::type_exn(), exn_cases);
            return 0;
          });
        };
        if (contains_polyvars) add_delayed_check([unused_check] { unused_check(true); });
        else
          // Check for unused cases, do not delay because of gadts
          unused_check(false);
        if (additional_checks_for_split_cases) {
          (*additional_checks_for_split_cases)(val_cases_with_result);
          (*additional_checks_for_split_cases)(exn_cases_with_result);
        }
        return {Result{result, partial}, std::vector<TypeExpr*>{ty_res_inst}};
      },
      // Ensure that existential types do not escape
      [&](TypeExpr* ty_res2) {
        TypeExpr* v = ctype::newvar();
        unify_exp_types(loc, env, ty_res2, v);
      });
}

}  // namespace cppcaml::typing::typecore
