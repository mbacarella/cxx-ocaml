// Port of middle_end/flambda/flambda_utils.ml.
#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "cppcaml/typing/flambda.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"

namespace cppcaml::typing::flambda_utils {

using flambda::named;
using flambda::t;

// name_expr ~name named: let var = named in var, var fresh
t name_expr(named n, internal_variable_names::t name);
// name_expr_from_var ~var named: the same, var renamed
t name_expr_from_var(named n, variable::t var);

// find_declaration cf decls (null: Not_found)
const flambda::FunctionDeclaration* find_declaration(variable::t closure_id, const flambda::FunctionDeclarations* d);
// find_declaration_variable cf decls (null: Not_found)
variable::t find_declaration_variable(variable::t closure_id, const flambda::FunctionDeclarations* d);
// find_free_variable cv set (null: Not_found)
variable::t find_free_variable(variable::t var_within_closure, const flambda::SetOfClosures* set);
inline long function_arity(const flambda::FunctionDeclaration* f) { return static_cast<long>(f->params.size()); }
variable::Set variables_bound_by_the_closure(variable::t closure_id, const flambda::FunctionDeclarations* decls);
std::string description_of_toplevel_node(t expr);

bool same(t l1, t l2);
bool same_named(named n1, named n2);
inline bool can_be_merged(t l1, t l2) { return same(l1, l2); }

t toplevel_substitution(const variable::Map<variable::t>& sb, t tree);
named toplevel_substitution_named(const variable::Map<variable::t>& sb, named n);
t make_closure_declaration(bool is_classic_mode, variable::t id, t body, Slice<Parameter> params);
// bind ~bindings ~body (the first binding innermost)
t bind(const std::vector<std::pair<variable::t, named>>& bindings, t body);

std::vector<flambda::SymbolBinding> all_lifted_constants(const flambda::Program& program);
symbol::Map<flambda::constant_defining_value> all_lifted_constants_as_map(const flambda::Program& program);
struct InitializeSymbol {
  symbol::t sym;
  tag::t tag;
  Slice<t> fields;
};
std::vector<InitializeSymbol> initialize_symbols(const flambda::Program& program);
inline symbol::Set imported_symbols(const flambda::Program& program) { return program.imported_symbols; }
symbol::Set needed_import_symbols(const flambda::Program& program);
flambda::Program introduce_needed_import_symbols(const flambda::Program& program);
symbol::t root_symbol(const flambda::Program& program);
bool might_raise_static_exn(named flam, static_exception::t stexn);
variable::Map<set_of_closures_id::t> make_closure_map(const flambda::Program& program);
variable::Set all_lifted_constant_closures(const flambda::Program& program);
set_of_closures_id::Set all_lifted_constant_sets_of_closures(const flambda::Program& program);
std::vector<const flambda::SetOfClosures*> all_sets_of_closures(const flambda::Program& program);
set_of_closures_id::Map<const flambda::SetOfClosures*> all_sets_of_closures_map(const flambda::Program& program);
// substitute_read_symbol_field_for_variables (Symbol.t * int list) Variable.Map.t
struct SymbolPath {
  symbol::t sym;
  std::vector<long> path;
};
t substitute_read_symbol_field_for_variables(const variable::Map<const SymbolPath*>& substitution, t expr);
variable::Map<variable::Set> fun_vars_referenced_in_decls(const flambda::FunctionDeclarations* function_decls,
                                                          FnRef<symbol::t(variable::t)> closure_symbol);
variable::Set closures_required_by_entry_point(variable::t entry_point, FnRef<symbol::t(variable::t)> closure_symbol,
                                               const flambda::FunctionDeclarations* function_decls);
variable::Set all_functions_parameters(const flambda::FunctionDeclarations* function_decls);
symbol::Set all_free_symbols(const flambda::FunctionDeclarations* function_decls);
bool contains_stub(const flambda::FunctionDeclarations* fun_decls);
variable::Map<flambda::SpecialisedTo> clean_projections(const variable::Map<flambda::SpecialisedTo>& which_variables);
named projection_to_named(projection::t p);
// specialised_to_same_as = Not_specialised | Specialised_and_aliased_to of
// Variable.Set.t
struct SpecialisedToSameAs {
  bool specialised = false;
  variable::Set aliased_to;
};
variable::Map<std::vector<SpecialisedToSameAs>> parameters_specialised_to_the_same_variable(
    const flambda::FunctionDeclarations* function_decls,
    const variable::Map<flambda::SpecialisedTo>& specialised_args);

// Switch_storer = Switch.Store (...): the stored branches' key is the
// term itself where it is comparable (only variables, lets of symbols /
// constants / primitives / expressions, static raises); two keys are the
// same when compare_key finds them equal up to the renaming of the
// variables they bind.  (Switch_storer itself: switch.hpp's
// Store<SwitchStorerPolicy>.)
struct SwitchStorerPolicy {
  using t = flambda::t;
  using key = flambda::t;
  static std::optional<key> make_key(const t& expr);
  static bool same_key(const key& a, const key& b);
};

}  // namespace cppcaml::typing::flambda_utils
