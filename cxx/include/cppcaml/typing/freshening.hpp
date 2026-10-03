// Port of middle_end/flambda/freshening.ml: the renaming of bound
// variables, mutable variables and static exceptions as terms are copied
// (inlining, specialisation), and Project_var: the freshening of closure
// ids and closure variables an approximation records.
#pragma once

#include <utility>
#include <vector>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::simple_value_approx {
struct Approx;
}

namespace cppcaml::typing::freshening {

struct Tbl {
  variable::Map<variable::t> sb_var;
  variable::Map<variable::t> sb_mutable_var;  // Mutable_variable.Map
  static_exception::Map<static_exception::t> sb_exn;
  // Used to handle substitution sequences: we cannot call the substitution
  // recursively because there can be name clashes.
  variable::Map<Slice<variable::t>> back_var;
  variable::Map<Slice<variable::t>> back_mutable_var;
};

// t = Inactive | Active of tbl
struct T {
  const Tbl* active = nullptr;  // null: Inactive
};
using subst = T;

inline T empty() { return {}; }
inline bool is_empty(const T& t) { return !t.active; }
T empty_preserving_activation_state(const T& t);
T activate(const T& t);
void print(format::Formatter& ppf, const T& t);

static_exception::t apply_static_exception(const T& t, static_exception::t i);
std::pair<static_exception::t, T> add_static_exception(const T& t, static_exception::t i);
std::pair<variable::t, T> add_variable(const T& t, variable::t id);
// add_variables' (List.fold_right: the last first)
std::pair<std::vector<variable::t>, T> add_variables_prime(const T& t, Slice<variable::t> ids);
std::pair<variable::t, T> add_mutable_variable(const T& t, variable::t id);
variable::t apply_variable(const T& t, variable::t var);
variable::t apply_mutable_variable(const T& t, variable::t mut_var);
const flambda::FunctionDeclarations* rewrite_recursive_calls_with_symbols(
    const T& t, const flambda::FunctionDeclarations* function_declarations,
    FnRef<symbol::t(variable::t)> make_closure_symbol);
bool does_not_freshen(const T& t, Slice<variable::t> vars);

namespace project_var {
struct T {
  variable::Map<variable::t> vars_within_closure;  // Var_within_closure.Map
  variable::Map<variable::t> closure_id;           // Closure_id.Map
};
inline T empty() { return {}; }
void print(format::Formatter& ppf, const T& t);
variable::t apply_closure_id(const T& t, variable::t closure_id);
variable::t apply_var_within_closure(const T& t, variable::t var_in_closure);
T compose(const T& earlier, const T& later);
}  // namespace project_var

// (Flambda.specialised_to * approximation): the free variables'
// data in Inline_and_simplify_aux.prepare_to_simplify_set_of_closures
struct SpecApprox {
  flambda::SpecialisedTo spec;
  const simple_value_approx::Approx* approx;
};

// apply_function_decls_and_free_vars t fv func_decls
//   ~only_freshen_parameters
struct AppliedFunctionDecls {
  variable::Map<SpecApprox> fv;
  const flambda::FunctionDeclarations* func_decls;
  freshening::T t;
  project_var::T of_closures;
};
AppliedFunctionDecls apply_function_decls_and_free_vars(const freshening::T& t, const variable::Map<SpecApprox>& fv,
                                                        const flambda::FunctionDeclarations* func_decls,
                                                        bool only_freshen_parameters);

projection::t freshen_projection(projection::t projection, const T& freshening,
                                 const project_var::T& closure_freshening);
variable::Map<flambda::SpecialisedTo> freshen_projection_relation(const variable::Map<flambda::SpecialisedTo>& relation,
                                                                  const T& freshening,
                                                                  const project_var::T& closure_freshening);
variable::Map<SpecApprox> freshen_projection_relation_prime(const variable::Map<SpecApprox>& relation,
                                                            const T& freshening,
                                                            const project_var::T& closure_freshening);

}  // namespace cppcaml::typing::freshening
