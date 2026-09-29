// The Typecore entry points that only Typeclass uses (typecore.mli
// type_class_arg_pattern, type_self_pattern, check_recursive_class_bindings),
// implemented in typecore_class.cpp.
#pragma once

#include "typecore_pat.hpp"

namespace cppcaml::typing::typecore {

struct ClassArgPatternVar {  // (Ident.t * Ident.t * type_expr): the renamed id, the pattern id
  Ident::t id2;
  Ident::t id;
  TypeExpr* ty;
};
struct ClassArgPatternResult {
  const tt::Pattern* pat;
  std::vector<ClassArgPatternVar> pv;
  env::t val_env;
  env::t met_env;
};
ClassArgPatternResult type_class_arg_pattern(std::string_view cl_num, env::t val_env, env::t met_env,
                                             const ArgLabel& l, const pt::Pattern* spat);
// (pattern, pattern variables, head first)
std::pair<const tt::Pattern*, std::vector<PatternVariable>> type_self_pattern(env::t env, const pt::Pattern* spat);
void check_recursive_class_bindings(env::t env, const std::vector<Ident::t>& ids,
                                    const std::vector<const tt::ClassExpr*>& exprs);

}  // namespace cppcaml::typing::typecore
