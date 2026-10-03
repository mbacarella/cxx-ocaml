// Port of middle_end/flambda/flambda_iterators.ml: traversals of Flambda
// terms and programs.  The maps keep a term (the same pointer) when nothing
// below it changed, as OCaml's `==` checks do; rebuilt sets of closures get
// fresh ids (Flambda.update_function_declarations), in ocamlopt's order.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::flambda_iterators {

using flambda::named;
using flambda::t;

void apply_on_subexpressions(FnRef<void(t)> f, FnRef<void(named)> f_named, t flam);
t map_subexpressions(FnRef<t(t)> f, FnRef<named(variable::t, named)> f_named, t tree);

void iter(FnRef<void(t)> f, FnRef<void(named)> f_named, t e);
void iter_expr(FnRef<void(t)> f, t e);
void iter_on_named(FnRef<void(t)> f, FnRef<void(named)> f_named, named n);
void iter_named(FnRef<void(named)> f_named, t e);
void iter_named_on_named(FnRef<void(named)> f_named, named n);
void iter_toplevel(FnRef<void(t)> f, FnRef<void(named)> f_named, t e);
void iter_named_toplevel(FnRef<void(t)> f, FnRef<void(named)> f_named, named n);
void iter_all_immutable_let_bindings(t e, FnRef<void(variable::t, named)> f);
void iter_all_toplevel_immutable_let_bindings(t e, FnRef<void(variable::t, named)> f);
void iter_on_sets_of_closures(FnRef<void(const flambda::SetOfClosures*)> f, t e);
void iter_exprs_at_toplevel_of_program(const flambda::Program& program, FnRef<void(t)> f);
void iter_named_of_program(const flambda::Program& program, FnRef<void(named)> f);
// f ~constant set_of_closures
void iter_on_set_of_closures_of_program(const flambda::Program& program,
                                        FnRef<void(bool, const flambda::SetOfClosures*)> f);
void iter_constant_defining_values_on_program(const flambda::Program& program,
                                              FnRef<void(flambda::constant_defining_value)> f);
void iter_apply_on_program(const flambda::Program& program, FnRef<void(const flambda::Apply*)> f);

t map_general(bool toplevel, FnRef<t(t)> f, FnRef<named(variable::t, named)> f_named, t tree);
t map(FnRef<t(t)> f, FnRef<named(named)> f_named, t tree);
t map_expr(FnRef<t(t)> f, t tree);
t map_named(FnRef<named(named)> f_named, t tree);
t map_named_with_id(FnRef<named(variable::t, named)> f_named, t tree);
t map_toplevel(FnRef<t(t)> f, FnRef<named(named)> f_named, t tree);
t map_toplevel_expr(FnRef<t(t)> f, t tree);
t map_toplevel_named(FnRef<named(named)> f_named, t tree);
t map_symbols(t tree, FnRef<symbol::t(symbol::t)> f);
const flambda::SetOfClosures* map_symbols_on_set_of_closures(const flambda::SetOfClosures* set,
                                                             FnRef<symbol::t(symbol::t)> f);
t map_toplevel_sets_of_closures(t tree,
                                FnRef<const flambda::SetOfClosures*(const flambda::SetOfClosures*)> f);
// (an Apply node stands for its apply record: f returns it, or a new one)
t map_apply(t tree, FnRef<const flambda::Apply*(const flambda::Apply*)> f);
t map_sets_of_closures(t tree, FnRef<const flambda::SetOfClosures*(const flambda::SetOfClosures*)> f);
// (null: None)
t map_project_var_to_expr_opt(t tree, FnRef<t(const projection::ProjectVar&)> f);
t map_project_var_to_named_opt(t tree, FnRef<named(const projection::ProjectVar&)> f);
const flambda::SetOfClosures* map_function_bodies(const flambda::SetOfClosures* set, FnRef<t(t)> f);
flambda::Program map_sets_of_closures_of_program(
    const flambda::Program& program, FnRef<const flambda::SetOfClosures*(const flambda::SetOfClosures*)> f);
flambda::Program map_exprs_at_toplevel_of_program(const flambda::Program& program, FnRef<t(t)> f);
flambda::Program map_named_of_program(const flambda::Program& program, FnRef<named(variable::t, named)> f);
t map_all_immutable_let_and_let_rec_bindings(t expr, FnRef<named(variable::t, named)> f);

}  // namespace cppcaml::typing::flambda_iterators
