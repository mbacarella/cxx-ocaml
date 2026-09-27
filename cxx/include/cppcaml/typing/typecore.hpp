// Port of typing/typecore.ml (TYPECHECKER.md, stage 4c): typechecking of the
// core language (patterns and expressions).  Errors are `typecore::Error`
// (the OCaml `Error.In_context`); reporting them comes with Printtyp.
//
// The implementation is split by area, following typecore.ml's order:
// typecore_util.cpp (errors, constants, pattern variables, as-types,
// pattern constraint solving, name disambiguation), typecore_pat.cpp
// (type_pat and counter-example checking), typecore_app.cpp (application
// arguments, expansiveness, approximations), typecore_exp.cpp (type_expect),
// typecore_fun.cpp (functions, formats, labels, arguments, applications,
// constructors), typecore_let.cpp (cases, let, letops, send, toplevel).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/typedtree.hpp"
#include "cppcaml/typing/typetexp.hpp"

namespace cppcaml::typing::typecore {

namespace tt = typedtree;
namespace pt = parsetree;
namespace et = errortrace;

enum class TypeForcingContext {
  If_conditional, If_no_else_branch, While_loop_conditional, While_loop_body,
  For_loop_start_index, For_loop_stop_index, For_loop_body, Assert_condition,
  Sequence_left_hand_side, When_guard
};
using Explanation = std::optional<TypeForcingContext>;

struct TypeExpected {
  TypeExpr* ty;
  Explanation explanation;
};
inline TypeExpected mk_expected(TypeExpr* ty, Explanation e = std::nullopt) { return {ty, e}; }

enum class DatatypeKind { Record, Variant };
struct WrongName {
  Path::t type_path;
  DatatypeKind kind;
  pt::StrLoc name;
  std::vector<std::string_view> valid_names;
};
struct WrongKindContext {  // Pattern | Expression of type_forcing_context option
  bool is_pattern;
  Explanation explanation;
};
enum class WrongKindSort { Constructor, Record, Boolean, List, Unit };
enum class ExistentialRestriction {
  At_toplevel, In_group, In_rec, With_attributes, In_class_args, In_class_def, In_self_pattern
};
enum class ExistentialBinding { Bind_already_bound, Bind_not_in_scope, Bind_non_locally_abstract };

struct Error : std::runtime_error {
  enum class Kind {
    Constructor_arity_mismatch, Label_mismatch, Pattern_type_clash, Or_pattern_type_clash,
    Multiply_bound_variable, Orpat_vars, Expr_type_clash, Function_arity_type_clash,
    Apply_non_function, Apply_wrong_label, Label_multiply_defined, Label_missing,
    Label_not_mutable, Wrong_name, Name_type_mismatch, Invalid_format, Not_an_object,
    Undefined_method, Undefined_self_method, Virtual_class, Private_type, Private_label,
    Private_constructor, Unbound_instance_variable, Instance_variable_not_mutable, Not_subtype,
    Outside_class, Value_multiply_overridden, Coercion_failure, Not_a_function,
    Too_many_arguments, Abstract_wrong_label, Not_a_polymorphic_variant_type,
    Incoherent_label_order, Less_general, Modules_not_allowed, Cannot_infer_signature,
    Not_a_packed_module, Unexpected_existential, Invalid_interval, Invalid_for_loop_index,
    No_value_clauses, Exception_pattern_disallowed, Mixed_value_and_exception_patterns_under_guard,
    Effect_pattern_below_toplevel, Invalid_continuation_pattern, Inlined_record_escape,
    Inlined_record_expected, Unrefuted_pattern, Invalid_extension_constructor_payload,
    Not_an_extension_constructor, Invalid_atomic_loc_payload, Label_not_atomic,
    Atomic_in_pattern, Literal_overflow, Unknown_literal, Illegal_letrec_pat,
    Illegal_letrec_expr, Illegal_class_expr, Letop_type_clash, Andop_type_clash,
    Bindings_type_clash, Unbound_existential, Bind_existential, Missing_type_constraint,
    Wrong_expected_kind, Expr_not_a_record_type, Constructor_labeled_arg,
    Partial_tuple_pattern_bad_type, Extra_tuple_label, Missing_tuple_label,
    Repeated_tuple_exp_label, Repeated_tuple_pat_label, Optional_poly_param,
    Cannot_unify_tfunctor_to_tarrow, Cannot_omit_tfunctor_argument
  };
  Location loc;
  env::t env;
  Kind kind;
  // payloads (which ones are set depends on the kind)
  Longident::t lid = nullptr;
  long n1 = 0, n2 = 0;
  et::UnificationError trace;
  const pt::PatternDesc* sdesc_for_hint = nullptr;       // Pattern_type_clash
  const pt::Expression* sexp = nullptr;                   // Expr_type_clash
  Explanation explanation;
  Ident::t id = nullptr;
  std::vector<Ident::t> ids;
  std::string name;
  std::vector<std::string> names;
  bool has_names = false;                                  // `string list option`
  TypeExpr* ty = nullptr;
  TypeExpr* ty2 = nullptr;
  ArgLabel label, label2;
  bool flag = false;
  const tt::Expression* texp = nullptr;
  Location loc2, loc3;
  WrongName wrong_name{};
  TypeExpected expected{};
  DatatypeKind dkind = DatatypeKind::Record;
  std::pair<Path::t, Path::t> tp{};
  std::vector<std::pair<Path::t, Path::t>> tpl;
  const ConstructorDescription* cstr = nullptr;
  et::subtype::Error subtype;
  et::ExpandedType expanded{};
  ExistentialRestriction restriction = ExistentialRestriction::At_toplevel;
  ExistentialBinding binding = ExistentialBinding::Bind_already_bound;
  WrongKindSort sort = WrongKindSort::Constructor;
  WrongKindContext ctx{};
  char c = 0;
  const tt::Pattern* pat = nullptr;
  ident::Unscoped* us = nullptr;
  OptStr optlabel;

  Error(const Location& l, env::t e, Kind k)
      : std::runtime_error("Typecore.Error"), loc(l), env(e), kind(k) {}
};
// Syntaxerr.Error (Variable_in_scope (loc, v)), raised by
// Ast_helper.Typ.varify_constructors while typing `let x : type a. t = ..`
struct VariableInScope : std::runtime_error {
  Location loc;
  std::string name;
  VariableInScope(const Location& l, std::string_view n)
      : std::runtime_error("Syntaxerr.Error"), loc(l), name(n) {}
};
// Error_forward of Location.error (an uninterpreted extension node)
using ErrorForward = typetexp::ErrorForward;

// Forward declarations, set by Typemod / Typeclass
extern std::function<std::pair<const tt::ModuleExpr*, const void*>(env::t, const pt::ModuleExpr*)>
    type_module;
extern std::function<std::pair<const tt::StructureItem*, env::t>(env::t, const pt::StructureItem*)>
    type_str_item;
extern std::function<std::pair<Path::t, env::t>(std::shared_ptr<bool> used_slot, OverrideFlag, env::t,
                                                const Location&, const pt::LidLoc&)>
    type_open;
struct TypeOpenDeclResult {
  const tt::OpenDeclaration* od;
  Signature sg;
  env::t env;
};
extern std::function<TypeOpenDeclResult(std::shared_ptr<bool> used_slot, env::t, const pt::OpenDeclaration*)>
    type_open_decl;
// type_package env m pack -> (module_expr, pack')
extern std::function<std::pair<const tt::ModuleExpr*, const Package*>(env::t, const pt::ModuleExpr*,
                                                                     const Package*)>
    type_package;
extern std::function<void(const Location&, env::t, TypeExpr*,
                          const std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>&)>
    check_package_closed;
extern std::function<std::pair<const tt::ClassStructure*, std::vector<std::string_view>>(
    env::t, const Location&, const pt::ClassStructure*)>
    type_object;

// ---- the typecore.mli interface --------------------------------------------------------
const tt::Expression* type_expression(env::t env, const pt::Expression* sexp);
const tt::Expression* type_exp(env::t env, const pt::Expression* sexp);
const tt::Expression* type_expect(env::t env, const pt::Expression* sexp, const TypeExpected& ty);
const tt::Expression* type_argument(env::t env, const pt::Expression* sexp, TypeExpr* t1,
                                    TypeExpr* t2);
struct TypeBindingResult {
  Slice<const tt::ValueBinding*> vbs;
  env::t env;
};
TypeBindingResult type_binding(env::t env, RecFlag rec_flag,
                               Slice<const pt::ValueBinding*> spat_sexp_list);
TypeBindingResult type_let(std::optional<ExistentialRestriction> existential_context, env::t env,
                           RecFlag rec_flag, Slice<const pt::ValueBinding*> spat_sexp_list);
void reset_delayed_checks();
void force_delayed_checks();
void add_delayed_check(std::function<void()> f);
bool is_nonexpansive(const tt::Expression* exp);
TypeExpr* type_constant(const tt::Constant& c);

}  // namespace cppcaml::typing::typecore
