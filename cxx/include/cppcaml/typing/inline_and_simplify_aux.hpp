// Port of middle_end/flambda/inline_and_simplify_aux.ml: the environment
// and the result of Inline_and_simplify, and the preparation of a set of
// closures for simplification.
#pragma once

#include <optional>

#include "cppcaml/typing/freshening.hpp"
#include "cppcaml/typing/inlining_cost.hpp"
#include "cppcaml/typing/inlining_stats.hpp"
#include "cppcaml/typing/mach.hpp"
#include "cppcaml/typing/simple_value_approx.hpp"

namespace cppcaml::typing::inline_and_simplify_aux {

namespace A = simple_value_approx;

// Backend_intf.S: Proc.max_arguments_for_tailcalls - 1
inline long max_sensible_number_of_arguments() { return proc::max_arguments_for_tailcalls - 1; }

struct ProjCmp {
  int operator()(projection::t a, projection::t b) const { return projection::compare(a, b); }
};

// Env.t (a value: OCaml's record, copied on update)
struct Env {
  enum class Scope : unsigned char { Current, Outer };
  struct ScopedApprox {
    Scope scope;
    A::t approx;
  };
  long round_ = 0;
  format::Formatter* ppf_dump_ = nullptr;
  variable::Map<ScopedApprox> approx;
  variable::Map<A::t> approx_mutable;  // Mutable_variable.Map
  symbol::Map<A::t> approx_sym;
  OMap<projection::t, variable::t, ProjCmp> projections;
  set_of_closures_id::Set current_functions;  // Set_of_closures_origin.Set
  // The functions currently being declared: used to avoid inlining
  // recursively
  long inlining_level_ = 0;  // Number of times "inline" has been called recursively
  long inside_branch_ = 0;
  freshening::T freshening_;
  bool never_inline_ = false;
  bool never_inline_inside_closures = false;
  bool never_inline_outside_closures = false;
  set_of_closures_id::Map<long> unroll_counts;  // Set_of_closures_origin.Map
  variable::Map<long> inlining_counts;          // Closure_origin.Map
  set_of_closures_id::Map<long> actively_unrolling_;
  long closure_depth = 0;
  inlining_stats::ClosureStack inlining_stats_closure_stack = nullptr;
  debuginfo::t inlined_debuginfo;

  static Env create(bool never_inline, long round, format::Formatter* ppf_dump);
  long round() const { return round_; }
  format::Formatter* ppf_dump() const { return ppf_dump_; }
  Env local() const;
  Env inlining_level_up() const;
  void print(format::Formatter& ppf) const;
  bool mem(variable::t var) const { return approx.mem(var); }
  Env add_internal(variable::t var, A::t a, Scope scope) const;
  Env add(variable::t var, A::t a) const { return add_internal(var, a, Scope::Current); }
  Env add_outer_scope(variable::t var, A::t a) const { return add_internal(var, a, Scope::Outer); }
  Env add_mutable(variable::t mut_var, A::t a) const;
  static A::t really_import_approx(A::t a);
  // find_symbol_exn (null: Not_found)
  A::t find_symbol_exn(symbol::t s) const;
  A::t find_symbol_opt(symbol::t s) const { return find_symbol_exn(s); }
  A::t find_symbol_fatal(symbol::t s) const;
  A::t find_or_load_symbol(symbol::t s) const;
  Env add_projection(projection::t projection, variable::t bound_to) const;
  variable::t find_projection(projection::t projection) const;  // (null: None)
  bool does_not_bind(Slice<variable::t> vars) const;
  bool does_not_freshen(Slice<variable::t> vars) const;
  Env add_symbol(symbol::t s, A::t a) const;
  Env redefine_symbol(symbol::t s, A::t a) const;
  ScopedApprox find_with_scope_exn(variable::t id) const;
  A::t find_exn(variable::t id) const { return find_with_scope_exn(id).approx; }
  A::t find_mutable_exn(variable::t mut_var) const;
  std::vector<A::t> find_list_exn(Slice<variable::t> vars) const;
  A::t find_opt(variable::t id) const;  // (null: None)
  Env activate_freshening() const;
  Env enter_set_of_closures_declaration(set_of_closures_id::t origin) const;
  bool inside_set_of_closures_declaration(set_of_closures_id::t origin) const {
    return current_functions.mem(origin);
  }
  bool at_toplevel() const { return closure_depth == 0; }
  bool is_inside_branch() const { return inside_branch_ > 0; }
  long branch_depth() const { return inside_branch_; }
  Env inside_branch() const;
  Env set_freshening(freshening::T f) const;
  Env increase_closure_depth() const;
  Env set_never_inline() const;
  Env set_never_inline_inside_closures() const;
  Env unset_never_inline_inside_closures() const;
  Env set_never_inline_outside_closures() const;
  Env unset_never_inline_outside_closures() const;
  std::optional<long> actively_unrolling(set_of_closures_id::t origin) const;
  Env start_actively_unrolling(set_of_closures_id::t origin, long i) const;
  Env continue_actively_unrolling(set_of_closures_id::t origin) const;
  bool unrolling_allowed(set_of_closures_id::t origin) const;
  Env inside_unrolled_function(set_of_closures_id::t origin) const;
  bool inlining_allowed(variable::t id) const;
  Env inside_inlined_function(variable::t id) const;
  long inlining_level() const { return inlining_level_; }
  const freshening::T& freshening() const { return freshening_; }
  bool never_inline() const { return never_inline_ || never_inline_outside_closures; }
  Env note_entering_closure(variable::t closure_id, const debuginfo::t& dbg) const;
  Env note_entering_call(variable::t closure_id, const debuginfo::t& dbg) const;
  Env note_entering_inlined() const;
  Env note_entering_specialised(const variable::Set& closure_ids) const;
  // enter_closure t ~closure_id ~inline_inside ~dbg ~f: the environment
  // f is to be called with
  Env enter_closure(variable::t closure_id, bool inline_inside, const debuginfo::t& dbg) const;
  void record_decision(const inlining_stats::Decision& decision) const;
  Env set_inline_debuginfo(const debuginfo::t& dbg) const;
  debuginfo::t add_inlined_debuginfo(const debuginfo::t& dbg) const;
};

inlining_cost::Threshold initial_inlining_threshold(long round);
inlining_cost::Threshold initial_inlining_toplevel_threshold(long round);

// Result.t
struct Result {
  A::t approx = A::value_unknown(A::other());
  static_exception::Set used_static_exceptions;
  std::optional<inlining_cost::Threshold> inlining_threshold;
  inlining_cost::Benefit benefit;
  long num_direct_applications = 0;

  static Result create() { return Result{}; }
  Result set_approx(A::t a) const;
  Result meet_approx(const Env& env, A::t a) const;
  Result use_static_exception(static_exception::t i) const;
  Result exit_scope_catch(static_exception::t i) const;
  Result map_benefit(FnRef<inlining_cost::Benefit(inlining_cost::Benefit)> f) const;
  Result add_benefit(const inlining_cost::Benefit& b) const;
  Result reset_benefit() const;
  Result set_inlining_threshold(std::optional<inlining_cost::Threshold> t) const;
  Result add_inlining_threshold(inlining_cost::Threshold j) const;
  Result sub_inlining_threshold(inlining_cost::Threshold j) const;
  Result seen_direct_application() const;
};

// keep_body_check ~is_classic_mode ~recursive
std::function<bool(variable::t, const flambda::FunctionDeclaration*)> keep_body_check(bool is_classic_mode,
                                                                                    A::Lazy<variable::Set> recursive);

// prepare_to_simplify_set_of_closures ~env ~set_of_closures ~function_decls
//   ~freshen ~only_for_function_decl
struct PreparedSetOfClosures {
  variable::Map<freshening::SpecApprox> free_vars;
  variable::Map<flambda::SpecialisedTo> specialised_args;
  const flambda::FunctionDeclarations* function_decls;
  variable::Map<A::t> parameter_approximations;
  const A::ValueSetOfClosures* internal_value_set_of_closures;
  Env set_of_closures_env;
};
PreparedSetOfClosures prepare_to_simplify_set_of_closures(const Env& env, const flambda::SetOfClosures* set_of_closures,
                                                          const flambda::FunctionDeclarations* function_decls,
                                                          bool freshen,
                                                          const flambda::FunctionDeclaration* only_for_function_decl);
Env prepare_to_simplify_closure(const flambda::FunctionDeclaration* function_decl,
                                const variable::Map<freshening::SpecApprox>& free_vars,
                                const variable::Map<flambda::SpecialisedTo>& specialised_args,
                                const variable::Map<A::t>& parameter_approximations, const Env& set_of_closures_env);

}  // namespace cppcaml::typing::inline_and_simplify_aux
