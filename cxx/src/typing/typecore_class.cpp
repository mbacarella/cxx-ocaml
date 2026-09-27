// Port of typing/typecore.ml's class-pattern entry points:
// type_class_arg_pattern and type_self_pattern (from the "Typing of
// patterns" section) and check_recursive_class_bindings.
#include "ast_helper.hpp"
#include "cppcaml/typing/value_rec_check.hpp"
#include "typecore_class.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;

ClassArgPatternResult type_class_arg_pattern(std::string_view cl_num, env::t val_env, env::t met_env,
                                             const ArgLabel& l, const pt::Pattern* spat) {
  auto [pattern_variables, pat] = ctype::with_local_level_generalize_structure_if_principal([&] {
    std::shared_ptr<TypePatState> tps = create_type_pat_state(
        std::nullopt, ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_rejected});
    TypeExpr* nv = ctype::newvar();
    long equations_scope = ctype::get_current_level();
    ctype::PatternEnv* new_penv = ctype::PatternEnv::make_(val_env, equations_scope, false);
    const tt::Pattern* p =
        type_pat(*tps, tt::PatternCategory::Value, ExistentialRestriction::In_class_args, new_penv, spat, nv);
    if (has_variants(p)) {
      parmatch::pressure_variants(val_env, {p});
      finalize_variants(p);
    }
    for (auto& f : tps->tps_pattern_force) f();
    if (is_optional(l)) unify_pat(val_env, p, type_option(ctype::newvar()));
    return std::make_pair(tps->tps_pattern_variables, p);
  });
  // List.fold_right over the pattern variables (head first): the last first
  std::vector<ClassArgPatternVar> pv;
  for (std::size_t k = pattern_variables.size(); k-- > 0;) {
    const PatternVariable& v = pattern_variables[k];
    Ident::t id2 = ident::rename(v.pv_id);
    Uid val_uid = uid::mk(env::get_current_unit());
    auto* d1 = make<ValueDescription>(v.pv_type, ValueKind{}, v.pv_loc, pt::types_attributes(v.pv_attributes), val_uid);
    val_env = env::add_value(v.pv_id, d1, val_env);
    ValueKind ivar{ValueKind::Kind::Val_ivar};
    ivar.ivar_mut = MutableFlag::Immutable;
    ivar.ivar_name = zborrow(cl_num);
    auto* d2 = make<ValueDescription>(v.pv_type, ivar, v.pv_loc, pt::types_attributes(v.pv_attributes), val_uid);
    WK wk = v.pv_kind == PatternVariableKind::As_var ? WK::Unused_var : WK::Unused_var_strict;
    met_env = env::add_value(id2, d2, met_env, [wk](std::string s) { return warnings::Warning::with_s(wk, s); });
    pv.insert(pv.begin(), ClassArgPatternVar{id2, v.pv_id, v.pv_type});
  }
  return {pat, pv, val_env, met_env};
}

std::pair<const tt::Pattern*, std::vector<PatternVariable>> type_self_pattern(env::t env, const pt::Pattern* spat0) {
  // Pat.mk (Ppat_alias (spat, mknoloc "selfpat-*"))
  const pt::Pattern* spat = ast_helper::pat_mk(
      make<pt::Ppat_alias>(pt::Ppat_alias{{pt::PatternDesc::Kind::Ppat_alias}, spat0,
                                          pt::StrLoc{OCAML_LIT("selfpat-*"), location::none()}}),
      location::none());
  std::shared_ptr<TypePatState> tps =
      create_type_pat_state(std::nullopt, ModulePatternsRestriction{ModulePatternsRestriction::Kind::Modules_rejected});
  TypeExpr* nv = ctype::newvar();
  long equations_scope = ctype::get_current_level();
  ctype::PatternEnv* new_penv = ctype::PatternEnv::make_(env, equations_scope, false);
  const tt::Pattern* pat =
      type_pat(*tps, tt::PatternCategory::Value, ExistentialRestriction::In_self_pattern, new_penv, spat, nv);
  for (auto& f : tps->tps_pattern_force) f();
  return {pat, tps->tps_pattern_variables};
}

void check_recursive_class_bindings(env::t env, const std::vector<Ident::t>& ids,
                                    const std::vector<const tt::ClassExpr*>& exprs) {
  for (auto* expr : exprs)
    if (!value_rec_check::is_valid_class_expr(ids, expr))
      raise_error(err(expr->cl_loc, env, EK::Illegal_class_expr));  // log_or_raise
}

}  // namespace cppcaml::typing::typecore
