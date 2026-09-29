// typecore_pat.cpp's entry points, shared with the other typecore*.cpp parts.
#pragma once

#include "cppcaml/typing/parmatch.hpp"
#include "typecore_internal.hpp"

namespace cppcaml::typing::typecore {

const tt::Pattern* type_pat(TypePatState& tps, tt::PatternCategory category,
                            std::optional<ExistentialRestriction> no_existentials,
                            ctype::PatternEnv* penv, const pt::Pattern* sp, TypeExpr* expected_ty);
const tt::Pattern* as_comp_pattern(tt::PatternCategory category, const tt::Pattern* pat);

env::t add_pattern_variables(env::t env, const std::vector<PatternVariable>& pv, const env::CheckFn& check = nullptr,
                             const env::CheckFn& check_as = nullptr);
env::t add_module_variables(env::t env, const ModuleVariables& module_variables);

struct TypePatternResult {
  const tt::Pattern* pat;
  env::t env;
  std::vector<std::function<void()>> pattern_forces;  // head first
  std::vector<PatternVariable> pvs;                   // head first
  ModuleVariables mvs;
};
TypePatternResult type_pattern(tt::PatternCategory category, long lev, env::t env,
                               const pt::Pattern* spat, TypeExpr* expected_ty,
                               const std::optional<ContinuationVar>& cont,
                               const ModulePatternsRestriction& allow_modules);
struct TypePatternListResult {
  std::vector<const tt::Pattern*> patl;
  env::t env;
  std::vector<std::function<void()>> pattern_forces;
  std::vector<PatternVariable> pvs;
  ModuleVariables mvs;
};
TypePatternListResult type_pattern_list(
    tt::PatternCategory category, std::optional<ExistentialRestriction> no_existentials, env::t env,
    const std::vector<std::pair<pt::Attributes, const pt::Pattern*>>& spatl,
    const std::vector<TypeExpr*>& expected_tys, const ModulePatternsRestriction& allow_modules);

// counter-example checking
enum class SplittingMode { Backtrack_or, Refine_or };
struct CounterExampleInfo {
  int explosion_fuel;
  SplittingMode mode;
  bool inside_nonsplit_or;  // Refine_or {inside_nonsplit_or}
};
const tt::Pattern* partial_pred(long lev, SplittingMode splitting_mode, int explode, env::t env,
                                TypeExpr* expected_ty, const tt::Pattern* p);
tt::Partial check_partial(long lev, env::t env, TypeExpr* expected_ty, const Location& loc,
                          const std::vector<parmatch::TypedCase>& cases);
void check_unused(long lev, env::t env, TypeExpr* expected_ty,
                  const std::vector<parmatch::TypedCase>& cases);
void add_delayed_check(std::function<void()> f);

}  // namespace cppcaml::typing::typecore
