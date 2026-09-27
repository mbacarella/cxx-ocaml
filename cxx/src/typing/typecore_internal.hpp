// Definitions shared by the typecore*.cpp parts of the typing/typecore.ml
// port that typecore.mli does not export.
#pragma once

#include <functional>
#include <memory>

#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/warnings.hpp"
#include "cppcaml/typing/ocaml_list.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typing_recovery.hpp"

namespace cppcaml::typing::typecore {

// ---- warnings ----
using WK = warnings::Warning::K;
inline void prerr_warning(const Location& loc, const warnings::Warning& w) { location::prerr_warning(loc, w); }
inline void prerr_warning(const Location& loc, WK k) { location::prerr_warning(loc, warnings::Warning::make(k)); }
inline void prerr_warning(const Location& loc, WK k, std::string s) {
  location::prerr_warning(loc, warnings::Warning::with_s(k, std::move(s)));
}
// not_principal fmt: Warnings.Not_principal of the format's document
template <class... Args>
warnings::Warning not_principal(std::string_view fmt, Args&&... args) {
  warnings::Warning w = warnings::Warning::make(WK::Not_principal);
  w.doc = format_doc::doc_printf(fmt, std::forward<Args>(args)...);
  return w;
}
// Style.as_inline_code Printtyp.Doc.type_expr, as a %a argument
inline auto inline_type_expr(TypeExpr* ty) {
  return [ty](format_doc::Formatter& ppf) {
    misc::style::as_inline_code([](format_doc::Formatter& f, TypeExpr* t) { printtyp::type_expr(f, t); }, ppf, ty);
  };
}

using EK = Error::Kind;

// Error.log_and_raise / log_or_raise (batch ocamlc: both raise)
[[noreturn]] inline void raise_error(const Error& e) { typing_recovery::log_and_raise(e); }
inline Error err(const Location& loc, env::t env, EK k) { return Error(loc, env, k); }
[[noreturn]] void raise_variant_tags(const Location& loc, env::t env, const ctype::Tags& t);

// Parmatch.some_private_tag
inline constexpr std::string_view some_private_tag = "<some private tag>";

WrongKindSort wrong_kind_sort_of_constructor(Longident::t lid);
void check_scope_escape(const Location& loc, env::t env, long level, TypeExpr* ty);
Error error_of_filter_arrow_failure(const Location& loc, env::t env, Explanation explanation,
                                    bool first, TypeExpr* ty_fun,
                                    const ctype::FilterArrowFailure& f);

// ---- constants -----------------------------------------------------------------------
std::optional<tt::Constant> constant(const pt::Constant& c, Error* err_out);
tt::Constant constant_or_raise(env::t env, const Location& loc, const pt::Constant& cst);
TypeExpr* type_option(TypeExpr* ty);
tt::Expression* mkexp(const tt::ExpressionDesc* d, TypeExpr* ty, const Location& loc, env::t env);
const tt::Expression* option_none(env::t env, TypeExpr* ty, const Location& loc);
const tt::Expression* option_some(env::t env, const tt::Expression* texp);
TypeExpr* extract_option_type(env::t env, TypeExpr* ty);
bool is_floatarray_type(env::t env, TypeExpr* ty);
bool is_iarray_type(env::t env, TypeExpr* ty);
TypeExpr* protect_expansion(env::t env, TypeExpr* ty);
struct RecordExtraction {  // Record_type | Not_a_record_type | Maybe_a_record_type
  enum class Kind { Record_type, Not_a_record_type, Maybe_a_record_type };
  Kind kind;
  Path::t p0 = nullptr, p = nullptr;
  Slice<const LabelDeclaration*> fields;
};
RecordExtraction extract_concrete_record(env::t env, TypeExpr* ty);
struct VariantExtraction {
  enum class Kind { Variant_type, Not_a_variant_type, Maybe_a_variant_type };
  Kind kind;
  Path::t p0 = nullptr, p = nullptr;
  Slice<const ConstructorDeclaration*> cstrs;
};
VariantExtraction extract_concrete_variant(env::t env, TypeExpr* ty);
std::vector<Ident::t> extract_label_names(env::t env, TypeExpr* ty);
bool is_principal(TypeExpr* ty);
struct ArrayInfo {
  TypeExpr* ty_elt;  // option
  MutableFlag mut;
};
ArrayInfo disambiguate_array_literal(const Location& loc, env::t env, TypeExpr* expected_ty);
bool has_poly_constraint(const pt::Pattern* spat);
bool check_poly_constraint(const pt::Pattern* spat, env::t env, const ArgLabel& arg_label);

// ---- unification helpers ------------------------------------------------------------
struct ContinuationVar {
  Ident::t id;
  const ValueDescription* desc;
};
std::optional<ContinuationVar> type_continuation_pat(env::t env, TypeExpr* expected_ty,
                                                     const pt::Pattern* sp);
void unify_exp_types(const Location& loc, env::t env, TypeExpr* ty, TypeExpr* expected_ty,
                     const pt::Expression* sexp = nullptr);
Location proper_exp_loc(const tt::Expression* exp);
void unify_exp(const pt::Expression* sexp, env::t env, const tt::Expression* exp,
               TypeExpr* expected_ty);
void unify_pat_types(const Location& loc, env::t env, TypeExpr* ty, TypeExpr* ty2,
                     const pt::PatternDesc* sdesc_for_hint = nullptr);
btype::TypePairs* unify_pat_types_return_equated_pairs(bool refine, const Location& loc,
                                                       ctype::PatternEnv* penv, TypeExpr* pat,
                                                       TypeExpr* expected);
void unify_pat_types_penv(const Location& loc, ctype::PatternEnv* penv, TypeExpr* ty,
                          TypeExpr* ty2);
void unify_pat(env::t env, const tt::Pattern* pat, TypeExpr* expected_ty,
               const pt::PatternDesc* sdesc_for_hint = nullptr);
void unify_head_only(const Location& loc, ctype::PatternEnv* penv,
                     const ConstructorDescription* constr, TypeExpr* expected);
bool has_variants(const tt::Pattern* p);
void finalize_variants(const tt::Pattern* p);

// ---- pattern variables -----------------------------------------------------------------
enum class PatternVariableKind { Std_var, As_var, Continuation_var };
struct PatternVariable {
  Ident::t pv_id;
  TypeExpr* pv_type;
  Location pv_loc;
  PatternVariableKind pv_kind;
  pt::Attributes pv_attributes;
  Uid pv_uid;
};
struct ModuleVariable {
  Ident::t mv_id;
  pt::StrLoc mv_name;
  Location mv_loc;
  Uid mv_uid;
};
struct ModulePatternsRestriction {  // Modules_allowed {scope} | Modules_rejected | Modules_ignored
  enum class Kind { Modules_allowed, Modules_rejected, Modules_ignored };
  Kind kind;
  int scope = 0;
};
struct ModuleVariables {  // Modvars_allowed {scope; module_variables} | ..
  enum class Kind { Modvars_allowed, Modvars_rejected, Modvars_ignored };
  Kind kind;
  int scope = 0;
  std::vector<ModuleVariable> module_variables;  // head first
};
struct TypePatState {
  std::vector<PatternVariable> tps_pattern_variables;       // head first
  std::vector<std::function<void()>> tps_pattern_force;     // head first
  ModuleVariables tps_module_variables;
};
std::shared_ptr<TypePatState> create_type_pat_state(const std::optional<ContinuationVar>& cont,
                                                    const ModulePatternsRestriction& allow_modules);
TypePatState copy_type_pat_state(const TypePatState& s);
void blit_type_pat_state(const TypePatState& src, TypePatState& dst);
env::t maybe_add_pattern_variables_ghost(const Location& loc_let, env::t env,
                                         const std::vector<PatternVariable>& pv);
std::pair<Ident::t, Uid> enter_variable(TypePatState& tps, const Location& loc,
                                        const pt::StrLoc& name, TypeExpr* ty,
                                        const pt::Attributes& attrs, bool is_module = false,
                                        bool is_as_variable = false);
std::vector<std::pair<Ident::t, Ident::t>> enter_orpat_variables(
    const Location& loc, env::t env, const std::vector<PatternVariable>& p1_vs,
    const std::vector<PatternVariable>& p2_vs);
TypeExpr* build_as_type(env::t env, const tt::Pattern* p);

// ---- constraint solving during typing of patterns --------------------------------------
TypeExpr* solve_Ppat_alias(env::t env, const tt::Pattern* pat);
std::vector<pt::LabeledPattern> reorder_pat(const Location& loc, ctype::PatternEnv* penv,
                                            const std::vector<pt::LabeledPattern>& patl,
                                            ClosedFlag closed, Slice<LabeledTy> labeled_tl,
                                            TypeExpr* expected_ty);
std::vector<LabeledTy> solve_Ppat_tuple(const Location& loc, ctype::PatternEnv* env,
                                        const std::vector<pt::LabeledPattern>& args,
                                        TypeExpr* expected_ty);
struct ExistentialStyp {  // (string loc list * core_type)
  Slice<pt::StrLoc> name_list;
  const pt::CoreType* sty;
};
struct SolvedConstruct {
  std::vector<TypeExpr*> ty_args;
  const tt::ConstructTypeAnnot* existential_ctyp;  // option
};
SolvedConstruct solve_Ppat_construct(TypePatState& tps, ctype::PatternEnv* penv,
                                     const Location& loc, const ConstructorDescription* constr,
                                     std::optional<ExistentialRestriction> no_existentials,
                                     const ExistentialStyp* existential_styp,
                                     TypeExpr* expected_ty);
TypeExpr* solve_Ppat_record_field(const Location& loc, ctype::PatternEnv* penv,
                                  const LabelDescription* label, const pt::LidLoc& label_lid,
                                  TypeExpr* record_ty);
std::pair<TypeExpr*, MutableFlag> solve_Ppat_array(const Location& loc, ctype::PatternEnv* env,
                                                   TypeExpr* expected_ty);
TypeExpr* solve_Ppat_lazy(const Location& loc, ctype::PatternEnv* env, TypeExpr* expected_ty);
struct SolvedConstraint {
  const tt::CoreType* cty;
  TypeExpr* ty;
  TypeExpr* expected_ty;
};
SolvedConstraint solve_Ppat_constraint(TypePatState& tps, const Location& loc, env::t env,
                                       const pt::CoreType* sty, TypeExpr* expected_ty);
struct SolvedVariant {
  std::vector<TypeExpr*> arg_type;
  const RowDesc* row;
  TypeExpr* expected_ty;
};
SolvedVariant solve_Ppat_variant(const Location& loc, ctype::PatternEnv* env,
                                 std::string_view tag, bool no_arg, TypeExpr* expected_ty);
std::pair<Path::t, const tt::Pattern*> build_or_pat(env::t env, const Location& loc,
                                                    const pt::LidLoc& lid);

// ---- type paths and disambiguation ------------------------------------------------------
Path::t expand_path(env::t env, Path::t p);
bool compare_type_path(env::t env, Path::t tpath1, Path::t tpath2);
Path::t get_constr_type_path(TypeExpr* ty);
struct WrongNameDisambiguation {
  env::t env;
  WrongName wrong_name;
};
// expected_type of NameChoice.disambiguate: Some (tpath0, tpath, principal)
struct ExpectedTypePath {
  Path::t tpath0;
  Path::t tpath;
  bool principal;
};
// ?warn (default Location.prerr_warning)
using LabelWarn = std::function<void(const Location&, const warnings::Warning&)>;
const LabelDescription* disambiguate_label(env::LabelUsage usage, const pt::LidLoc& lid, env::t env,
                                           const std::optional<ExpectedTypePath>& expected_type,
                                           const env::LookupAllLabels& candidates_in_scope,
                                           const std::vector<std::string_view>* filter_ids = nullptr,
                                           bool filter_closed = false, const LabelWarn& warn = nullptr);
const ConstructorDescription* disambiguate_constructor(
    env::ConstructorUsage usage, const pt::LidLoc& lid, env::t env,
    const std::optional<ExpectedTypePath>& expected_type,
    const env::LookupAllCstrs& candidates_in_scope);
struct LidLabel {
  pt::LidLoc lid;
  const LabelDescription* label;
};
template <class A>
struct LidLabelA {
  pt::LidLoc lid;
  const LabelDescription* label;
  A a;
};
// disambiguate_lid_a_list, generic over the payload
std::vector<const LabelDescription*> disambiguate_lid_list(
    const Location& loc, bool closed, env::t env, env::LabelUsage usage,
    const std::optional<ExpectedTypePath>& expected_type, const std::vector<pt::LidLoc>& lids);
void check_recordpat_labels(const Location& loc,
                            const std::vector<tt::RecordPatField>& lbl_pat_list, ClosedFlag closed);
// wrap_disambiguate msg ty f x
template <class F>
auto wrap_disambiguate(std::string_view msg, const TypeExpected& ty, F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const WrongNameDisambiguation& w) {
    Error e = err(w.wrong_name.name.loc, w.env, EK::Wrong_name);
    e.name = std::string(msg);
    e.expected = ty;
    e.wrong_name = w.wrong_name;
    raise_error(e);
  }
}

}  // namespace cppcaml::typing::typecore
