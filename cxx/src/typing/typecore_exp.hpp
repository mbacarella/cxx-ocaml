// The mutually recursive core of typecore.ml ("type_exp" to "type_send"),
// split over typecore_exp.cpp (type_expect_ and the constraint / newtype /
// ident helpers), typecore_fun.cpp (functions, formats, labels, arguments,
// applications, constructors, statements) and typecore_let.cpp (cases, let,
// letops, method calls, toplevel bindings).
#pragma once

#include "typecore_app.hpp"

namespace cppcaml::typing::typecore {

enum class Recarg { Allowed, Required, Rejected };  // Typecore.recarg

const tt::Expression* type_exp_r(Recarg recarg, env::t env, const pt::Expression* sexp);
const tt::Expression* type_expect_r(Recarg recarg, env::t env, const pt::Expression* sexp,
                                    const TypeExpected& ty_expected_explained);
const tt::Expression* type_argument_x(Explanation explanation, Recarg recarg, env::t env,
                                      const pt::Expression* sarg, TypeExpr* ty_expected_,
                                      TypeExpr* ty_expected);

// 'ret constraint_arg
template <class Ret>
struct ConstraintArg {
  std::function<std::pair<Ret, TypeExpr*>(env::t)> type_without_constraint;
  std::function<Ret(env::t, TypeExpr*)> type_with_constraint;
  std::function<bool(const Ret&)> is_self;
};
ConstraintArg<const tt::Expression*> expression_constraint(const pt::Expression* pexp);
template <class Ret>
struct Constrained {
  Ret ret;
  TypeExpr* ty;
  tt::ExpExtra extra;
};
template <class Ret>
Constrained<Ret> type_coerce(const ConstraintArg<Ret>& c, env::t env, const Location& loc,
                             const pt::CoreType* sty, const pt::CoreType* sty2,
                             const Location& loc_arg);
std::pair<TypeExpr*, tt::ExpExtra> type_constraint(env::t env, const pt::CoreType* sty);
template <class Ret>
Constrained<Ret> type_constraint_expect(const ConstraintArg<Ret>& c, env::t env, const Location& loc,
                                        const Location& loc_arg, const pt::TypeConstraint& constraint_,
                                        TypeExpr* ty_expected);
using FunctionCases = std::pair<Slice<const tt::Case*>, tt::Partial>;

template <class A>
std::pair<A, TypeExpr*> type_newtype(env::t env, const pt::StrLoc& name,
                                     const std::function<std::pair<A, TypeExpr*>(env::t)>& type_body);
std::pair<Path::t, const ValueDescription*> type_ident(env::t env, Recarg recarg, const pt::LidLoc& lid);
std::pair<Path::t, const ValueDescription*> type_binding_op_ident(env::t env, const pt::StrLoc& s);

// in_function: (type_expected * Location.t)
struct InFunction {
  TypeExpected ty_fun;
  Location loc;
};
struct SplitFunctionTy {
  ctype::FilteredArrow filtered_arrow;
  TypeExpr* ty_arg_mono;
};
SplitFunctionTy split_function_ty(env::t env, TypeExpr* ty_expected, const ArgLabel& arg_label,
                                  bool has_poly, bool first, const InFunction& in_function);
std::optional<ctype::FunctorView> split_function_mty(env::t env, TypeExpr* ty_expected,
                                                     const ArgLabel& arg_label, bool first,
                                                     const InFunction& in_function);

// ---- typecore_fun.cpp ------------------------------------------------------------------
struct TypeFunctionResult {  // (exp_type, params, body, newtypes, contains_gadt)
  TypeExpr* exp_type;
  std::vector<TypeFunctionResultParam> params;
  const tt::FunctionBody* body;
  std::vector<pt::StrLoc> newtypes;
  bool contains_gadt;
};
TypeFunctionResult type_function(env::t env, Slice<const pt::FunctionParam*> params_suffix,
                                 const pt::TypeConstraint* body_constraint,
                                 const pt::FunctionBody* body, TypeExpr* ty_expected, bool first,
                                 const InFunction& in_function);
struct LabelAccess {
  const tt::Expression* record;
  const LabelDescription* label;
  std::optional<ExpectedTypePath> expected_type;
};
LabelAccess type_label_access(env::t env, const pt::Expression* srecord, env::LabelUsage usage,
                              const pt::LidLoc& lid);
struct SolvedField {
  const tt::Expression* record;
  const LabelDescription* label;
  TypeExpr* ty_arg;
};
SolvedField solve_Pexp_field(env::LabelUsage label_usage, env::t env, const pt::Expression* sexp,
                             const pt::Expression* srecord, const pt::LidLoc& lid);
const pt::Expression* type_format(const Location& loc, std::string_view str, env::t env);
struct LabelExp {
  pt::LidLoc lid;
  const LabelDescription* label;
  const tt::Expression* exp;
};
LabelExp type_label_exp(bool create, env::t env, const Location& loc, TypeExpr* ty_expected,
                        const pt::LidLoc& lid, const LabelDescription* label,
                        const pt::Expression* sarg);
std::pair<Slice<tt::LabeledArg>, TypeExpr*> type_application(
    env::t env, const Location& app_loc, const tt::Expression* funct,
    const std::vector<std::pair<ArgLabel, const pt::Expression*>>& sargs);
const tt::Expression* type_construct(env::t env, const pt::Expression* sexp, const pt::LidLoc& lid,
                                     const pt::Expression* sarg, const TypeExpected& ty_expected_explained);
const tt::Expression* type_statement(Explanation explanation, env::t env, const pt::Expression* sexp);

// ---- typecore_let.cpp ------------------------------------------------------------------
std::pair<Slice<const tt::Case*>, tt::Partial> type_cases(
    tt::PatternCategory category, env::t env, TypeExpr* ty_arg, const TypeExpected& ty_res_explained,
    const std::vector<std::optional<ContinuationVar>>* conts, bool check_if_total, const Location& loc,
    Slice<const pt::Case*> caselist);
struct FunctionCasesResult {
  Slice<const tt::Case*> cases;
  tt::Partial partial;
  TypeExpr* ty_fun;
};
FunctionCasesResult type_function_cases_expect(env::t env, TypeExpr* ty_expected, const Location& loc,
                                               Slice<const pt::Case*> cases, pt::Attributes attrs,
                                               bool first, const InFunction& in_function);
Slice<const tt::Case*> type_effect_cases(tt::PatternCategory category, env::t env,
                                         const TypeExpected& ty_res_explained, const Location& loc,
                                         Slice<const pt::Case*> caselist,
                                         const std::vector<const pt::Pattern*>& conts);
std::pair<Slice<const tt::ValueBinding*>, env::t> type_let_rec(bool reset_tyvarenv, env::t env,
                                                               Slice<const pt::ValueBinding*> spat_sexp_list,
                                                               const env::CheckFn& check = nullptr,
                                                               const env::CheckFn& check_strict = nullptr);
std::pair<Slice<const tt::ValueBinding*>, env::t> type_let_nonrec(
    bool reset_tyvarenv, std::optional<ExistentialRestriction> existential_context,
    const ModulePatternsRestriction& allow_modules, env::t env,
    Slice<const pt::ValueBinding*> spat_sexp_list, const env::CheckFn& check = nullptr,
    const env::CheckFn& check_strict = nullptr);
std::pair<const tt::Expression*, Slice<const tt::BindingOp*>> type_andops(
    env::t env, const pt::Expression* sarg, Slice<const pt::BindingOp*> sands, TypeExpr* expected_ty);
struct SendResult {
  const tt::Expression* obj;
  tt::Meth meth;
  TypeExpr* typ;
};
SendResult type_send(env::t env, const Location& loc, Explanation explanation, const pt::Expression* e,
                     std::string_view met);

// half-typed cases (typecore.ml "Typing of patterns")
using UntypedCase = parmatch::UntypedCase;
template <class CaseData>
struct HalfTypedCase {
  const tt::Pattern* typed_pat;
  TypeExpr* pat_type_for_unif;
  UntypedCase untyped_case;
  CaseData case_data;
  env::t branch_env;
  std::vector<PatternVariable> pat_vars;
  ModuleVariables module_vars;
  bool contains_gadt;
};
bool is_unpack(const pt::Pattern* pat);
bool could_be_functor(env::t env, TypeExpr* ty);

}  // namespace cppcaml::typing::typecore
