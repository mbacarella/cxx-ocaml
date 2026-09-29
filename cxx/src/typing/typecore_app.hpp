// typecore_app.cpp's entry points (typecore.ml "final_subexpression" to
// "type_exp"), shared with the other typecore*.cpp parts.
#pragma once

#include "typecore_pat.hpp"

namespace cppcaml::typing::typecore {

const tt::Expression* final_subexpression(const tt::Expression* exp);
bool is_prim(std::string_view name, const tt::Expression* funct);

// ---- collecting arguments for function applications ------------------------------------
struct UntypedApplyArg {
  enum class Kind { Known_arg, Unknown_arg, Eliminated_optional_arg, Typed_arg };
  Kind kind;
  const pt::Expression* sarg = nullptr;  // Known_arg / Unknown_arg
  TypeExpr* ty_arg = nullptr;            // Known_arg / Unknown_arg / Eliminated_optional_arg
  TypeExpr* ty_arg0 = nullptr;           // Known_arg
  bool wrapped_in_some = false;          // Known_arg
  long level = 0;                        // Eliminated_optional_arg
  const tt::Expression* targ = nullptr;  // Typed_arg
};
// (arg_label * (untyped_apply_arg, untyped_omitted_param) arg_or_omitted)
struct UntypedArg {
  ArgLabel label;
  bool omitted;         // Omitted {ty_arg; level}
  UntypedApplyArg arg;  // Arg _; for Omitted: ty_arg / level
};
struct CollectedArgs {
  TypeExpr* ty_ret;
  std::vector<UntypedArg> args;
};
extern std::function<const tt::Expression*(env::t, const pt::Expression*, TypeExpr*, TypeExpr*,
                                           Explanation)>
    type_argument_forward;  // type_argument'
CollectedArgs collect_apply_args(env::t env, const tt::Expression* funct, bool ignore_labels,
                                 TypeExpr* ty_fun, TypeExpr* ty_fun0,
                                 const std::vector<std::pair<ArgLabel, const pt::Expression*>>& sargs);
// returns (ty_ret, args) with Omitted args as `omitted`
std::pair<TypeExpr*, std::vector<UntypedArg>> type_omitted_parameters_and_build_result_type(
    TypeExpr* ty_ret, const std::vector<UntypedArg>& args);

// ---- generalization criterion ---------------------------------------------------------
bool maybe_expansive(const tt::Expression* e);
Slice<const tt::ValueBinding*> annotate_recursive_bindings(env::t env,
                                                           Slice<const tt::ValueBinding*> valbinds);

// ---- approximations -------------------------------------------------------------------
Location loc_rest_of_function(const Location& loc_function, bool first,
                              Slice<const pt::FunctionParam*> params_suffix,
                              const pt::FunctionBody* body);
TypeExpr* approx_type(env::t env, const pt::CoreType* sty);
void type_approx(env::t env, const pt::Expression* sexp, TypeExpr* ty_expected);

// ---- checks ---------------------------------------------------------------------------
void check_univars(env::t env, std::string_view kind, const tt::Expression* exp,
                   TypeExpr* ty_expected, const std::vector<TypeExpr*>& vars);
void check_statement(const tt::Expression* exp);
void check_partial_application(bool statement, const tt::Expression* exp);
bool pattern_needs_partial_application_check(const tt::Pattern* p);
bool generalizable(long level, TypeExpr* ty);
extern std::vector<std::pair<Path::t, std::shared_ptr<std::vector<Location>>>> self_coercion;
bool contains_variant_either(TypeExpr* ty);
bool exists_ppat(const std::function<bool(const pt::Pattern*)>& f, const pt::Pattern* p);
bool contains_polymorphic_variant(const pt::Pattern* p);
bool contains_gadt(const tt::Pattern* p);
bool is_var_pat(const pt::Pattern* p);
bool may_contain_gadts(const pt::Pattern* p);
bool turn_let_into_match(const pt::Pattern* p);
bool may_contain_modules(const pt::Pattern* p);
void check_absent_variant(env::t env, const tt::Pattern* p);
Ident::t name_pattern(std::string_view dflt, const std::vector<const tt::Pattern*>& pats);
Ident::t name_cases(std::string_view dflt, Slice<const tt::Case*> lst);
bool is_inferred(const pt::Expression* sexp);
enum class ApplyPrim { Apply, Revapply };
bool check_apply_prim_type(ApplyPrim prim, TypeExpr* typ);
// with_explanation explanation f
template <class F>
auto with_explanation(Explanation explanation, F&& f) -> decltype(f()) {
  if (!explanation) return f();
  try {
    return f();
  } catch (const Error& e) {
    if (e.kind == EK::Expr_type_clash && !e.explanation && !e.loc.loc_ghost) {
      Error e2 = e;
      e2.explanation = explanation;
      raise_error(e2);
    }
    throw;
  }
}
void lower_args(long outer_level, env::t env, TypeExpr* ty_fun);
struct TypeFunctionResultParam {
  const tt::FunctionParam* param;
  bool has_poly;
};
void enforce_syntactic_arity(const Location& loc, env::t env, TypeExpr* exp_type,
                             const std::vector<TypeFunctionResultParam>& result_params,
                             const tt::FunctionBody* body);
void may_lower_contravariant(env::t env, const tt::Expression* exp);
const pt::Expression* vb_exp_constraint(const pt::ValueBinding* vb);
std::pair<pt::Attributes, const pt::Pattern*> vb_pat_constraint(const pt::ValueBinding* vb);
void do_relaxed_value_restriction(env::t env,
                                  const std::vector<std::pair<const tt::Pattern*, TypeExpr*>>& pat_list,
                                  const std::vector<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>>& exp_list);
void check_let_univars(env::t env,
                       const std::vector<std::pair<const tt::Pattern*, TypeExpr*>>& pat_list,
                       const std::vector<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>>& exp_list);

}  // namespace cppcaml::typing::typecore
