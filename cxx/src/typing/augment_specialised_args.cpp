// Port of middle_end/flambda/augment_specialised_args.ml (see
// augment_specialised_args.hpp).
#include "cppcaml/typing/augment_specialised_args.hpp"

#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pass_wrapper.hpp"

namespace cppcaml::typing::augment_specialised_args {

using namespace flambda;
using E = inline_and_simplify_aux::Env;
namespace B = inlining_cost::benefit;
using format::Formatter;
using format::fprintf;

int compare_definition(const Definition& a, const Definition& b) {
  if (a.existing_inner_free_var && b.existing_inner_free_var)
    return variable::compare(a.existing_inner_free_var, b.existing_inner_free_var);
  if (!a.existing_inner_free_var && !b.existing_inner_free_var) return projection::compare(a.projection, b.projection);
  return a.existing_inner_free_var ? -1 : 1;
}

namespace {
struct DefCmp {
  int operator()(const Definition& a, const Definition& b) const { return compare_definition(a, b); }
};
using DefSet = OSet<Definition, DefCmp>;
using ProjMap = OMap<projection::t, variable::t, inline_and_simplify_aux::ProjCmp>;

[[noreturn]] void fatal(const std::function<void(Formatter&)>& msg) {
  Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}

Slice<Definition> cons(const Definition& d, Slice<Definition> l) {
  std::vector<Definition> v;
  v.reserve(l.size() + 1);
  v.push_back(d);
  v.insert(v.end(), l.begin(), l.end());
  return slice(v);
}
}  // namespace

WhatToSpecialise WhatToSpecialise::create(const SetOfClosures* set_of_closures) {
  return WhatToSpecialise{{}, set_of_closures, {}};
}

WhatToSpecialise WhatToSpecialise::new_specialised_arg(variable::t fun_var, variable::t group,
                                                       const Definition& definition) const {
  PairKey key{fun_var, group};
  const Slice<Definition>* ds = definitions.find_opt(key);
  WhatToSpecialise r = *this;
  r.definitions = definitions.add(key, cons(definition, ds ? *ds : Slice<Definition>{}));
  return r;
}

WhatToSpecialise WhatToSpecialise::make_direct_call_surrogate_for(variable::t fun_var) const {
  if (!set_of_closures->function_decls->funs.mem(fun_var))
    fatal([&](Formatter& f) {
      fprintf(f, "use_direct_call_surrogate_for: %a is not a fun_var from the given set of closures",
              pr(variable::print, fun_var));
    });
  WhatToSpecialise r = *this;
  r.make_direct_call_surrogates_for = make_direct_call_surrogates_for.add(fun_var);
  return r;
}

namespace {
// Processed_what_to_specialise
struct ForOneFunction {
  variable::t fun_var;
  const FunctionDeclaration* function_decl;
  bool make_direct_call_surrogates;
  variable::Map<Definition> new_definitions_indexed_by_new_inner_vars;
  DefSet all_new_definitions;
  variable::Map<variable::t> new_inner_to_new_outer_vars;
  long total_number_of_args;
  variable::Map<SpecialisedTo> existing_specialised_args;
};

struct Processed {
  const SetOfClosures* set_of_closures;
  variable::Map<DefSet> existing_definitions_via_spec_args_indexed_by_fun_var;
  // The following two maps' definitions have already been rewritten into
  // their lifted form (i.e. they reference outer rather than inner
  // variables).
  variable::Map<projection::t> new_lifted_defns_indexed_by_new_outer_vars;
  ProjMap new_outer_vars_indexed_by_new_lifted_defns;
  variable::Map<ForOneFunction> functions;
  variable::Set make_direct_call_surrogates_for;

  projection::t lift_projection(projection::t p) const {
    // The lifted definition must be in terms of outer variables, not inner
    // variables.
    return projection::map_projecting_from(p, [&](variable::t inner_var) -> variable::t {
      const SpecialisedTo* outer_var = set_of_closures->specialised_args.find_opt(inner_var);
      if (!outer_var)
        fatal([&](Formatter& f) {
          fprintf(f,
                  "find_outer_var: expected %a to be in [specialised_args], but it is not.  The projection was: %a. "
                  " Set of closures: %a",
                  pr(variable::print, inner_var), pr(projection::print, p), pr(print_set_of_closures, set_of_closures));
        });
      return outer_var->var;
    });
  }

  Processed really_add_new_specialised_arg(variable::t group, const Definition& definition,
                                           ForOneFunction for_one_function) const {
    variable::t fun_var = for_one_function.fun_var;
    // We know here that a new specialised argument must be added.  This
    // needs a "new inner var" and a "new outer var".  (augment_specialised_args.ml)
    Processed t = *this;
    variable::t new_outer_var = nullptr;
    if (definition.projection) {
      projection::t lifted = lift_projection(definition.projection);
      if (const variable::t* v = new_outer_vars_indexed_by_new_lifted_defns.find_opt(lifted)) new_outer_var = *v;
    }
    if (!new_outer_var) {
      if (definition.existing_inner_free_var) {
        const SpecialisedTo* existing_outer_var =
            set_of_closures->free_vars.find_opt(definition.existing_inner_free_var);
        if (!existing_outer_var)
          fatal([&](Formatter& f) {
            fprintf(f, "really_add_new_specialised_arg: Existing_inner_free_var %a is not an inner free variable of %a in %a",
                    pr(variable::print, definition.existing_inner_free_var), pr(variable::print, fun_var),
                    pr(print_set_of_closures, set_of_closures));
          });
        new_outer_var = existing_outer_var->var;
      } else {
        new_outer_var = variable::rename(group);
        projection::t lifted = lift_projection(definition.projection);
        t.new_outer_vars_indexed_by_new_lifted_defns = new_outer_vars_indexed_by_new_lifted_defns.add(lifted, new_outer_var);
        t.new_lifted_defns_indexed_by_new_outer_vars = new_lifted_defns_indexed_by_new_outer_vars.add(new_outer_var, lifted);
      }
    }
    variable::t new_inner_var = variable::rename(group);
    for_one_function.new_inner_to_new_outer_vars =
        for_one_function.new_inner_to_new_outer_vars.add(new_inner_var, new_outer_var);
    // (the record's fields right to left: pure)
    for_one_function.new_definitions_indexed_by_new_inner_vars =
        for_one_function.new_definitions_indexed_by_new_inner_vars.add(new_inner_var, definition);
    for_one_function.all_new_definitions = for_one_function.all_new_definitions.add(definition);
    for_one_function.total_number_of_args = for_one_function.total_number_of_args + 1;
    t.functions = t.functions.add(fun_var, for_one_function);
    return t;
  }

  Processed new_specialised_arg(variable::t fun_var, variable::t group, const Definition& definition) const {
    ForOneFunction for_one_function;
    if (const ForOneFunction* f = functions.find_opt(fun_var)) for_one_function = *f;
    else {
      const FunctionDeclaration* const* fd = set_of_closures->function_decls->funs.find_opt(fun_var);
      if (!fd) misc::fatal_error("Augment_specialised_args.new_specialised_arg");  // (assert false)
      const FunctionDeclaration* function_decl = *fd;
      variable::Set params = parameter::set_vars(function_decl->params);
      variable::Map<SpecialisedTo> existing_specialised_args = set_of_closures->specialised_args.filter(
          [&](variable::t inner_var, const SpecialisedTo&) { return params.mem(inner_var); });
      bool make_direct_call_surrogates = make_direct_call_surrogates_for.mem(fun_var);
      // The "+ 1" is just in case there is a closure environment parameter
      // added later.
      for_one_function = ForOneFunction{fun_var, function_decl, make_direct_call_surrogates, {}, {}, {},
                                        static_cast<long>(function_decl->params.size()) + 1, existing_specialised_args};
    }
    // Determine whether there already exists an existing specialised
    // argument that is known to be equal to the one proposed to this
    // function.  If so, use that instead.  (augment_specialised_args.ml)
    const DefSet* defs = existing_definitions_via_spec_args_indexed_by_fun_var.find_opt(fun_var);
    if (defs && defs->mem(definition)) return *this;
    return really_add_new_specialised_arg(group, definition, for_one_function);
  }

  static Processed create(const WhatToSpecialise& what_to_specialise) {
    const SetOfClosures* soc = what_to_specialise.set_of_closures;
    variable::Map<DefSet> existing = soc->function_decls->funs.map([&](const FunctionDeclaration* function_decl) {
      if (function_decl->stub) return DefSet{};
      variable::Set params = parameter::set_vars(function_decl->params);
      return soc->specialised_args.fold(
          [&](variable::t inner_var, const SpecialisedTo& spec_to, DefSet definitions) {
            if (!params.mem(inner_var)) return definitions;
            Definition d;
            if (!spec_to.projection) d.existing_inner_free_var = inner_var;
            else d.projection = spec_to.projection;
            return definitions.add(d);
          },
          DefSet{});
    });
    Processed t{soc, existing, {}, {}, {}, what_to_specialise.make_direct_call_surrogates_for};
    // It is important to limit the number of arguments added: if arguments
    // end up being passed on the stack, tail call optimization will be
    // disabled (see asmcomp/selectgen.ml).  For each group of new
    // specialised args provided by [T], either all or none of them will be
    // added.  (augment_specialised_args.ml)
    using FunDefs = std::pair<variable::t, Slice<Definition>>;
    // (each group's list newest first: a vector whose back is the head)
    variable::Map<Slice<FunDefs>> by_group;
    what_to_specialise.definitions.iter([&](const PairKey& k, Slice<Definition> definitions) {
      const Slice<FunDefs>* l = by_group.find_opt(k.second);
      std::vector<FunDefs> v;
      v.push_back({k.first, definitions});
      if (l) v.insert(v.end(), l->begin(), l->end());
      by_group = by_group.add(k.second, slice(v));
    });
    return by_group.fold(
        [&](variable::t group, Slice<FunDefs> fun_vars_and_definitions, Processed acc) {
          const Processed original_t = acc;
          // Try adding all specialised args in the current group.
          for (const FunDefs& fd : fun_vars_and_definitions)
            for (const Definition& definition : fd.second) acc = acc.new_specialised_arg(fd.first, group, definition);
          bool some_function_has_too_many_args = acc.functions.exists([](variable::t, const ForOneFunction& f) {
            return f.total_number_of_args > inline_and_simplify_aux::max_sensible_number_of_arguments();
          });
          return some_function_has_too_many_args ? original_t : acc;  // (drop this group)
        },
        t);
  }
};

void check_invariants(const char* pass_name, const SetOfClosures* set_of_closures,
                      const SetOfClosures* original_set_of_closures) {
  if (!clflags::flambda_invariant_checks) return;
  set_of_closures->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration* function_decl) {
    variable::Set params = parameter::set_vars(function_decl->params);
    set_of_closures->specialised_args.iter([&](variable::t inner_var, const SpecialisedTo& outer_var) {
      if (!params.mem(inner_var)) return;
      if (function_decl->free_variables.mem(outer_var.var))
        misc::fatal_error("Augment_specialised_args.check_invariants");  // (assert)
      if (!outer_var.projection) return;
      variable::t from = projection::projecting_from(outer_var.projection);
      if (!params.mem(from))
        fatal([&](Formatter& f) {
          fprintf(f,
                  "Augment_specialised_args (%s): specialised argument (%a -> %a) references a projection variable "
                  "that is not a specialised argument of the function %a. @ The set of closures before the "
                  "transformation was:@  %a. @ The set of closures after the transformation was:@ %a.",
                  pass_name, pr(variable::print, inner_var), pr(print_specialised_to, outer_var),
                  pr(variable::print, fun_var), pr(print_set_of_closures, original_set_of_closures),
                  pr(print_set_of_closures, set_of_closures));
        });
    });
  });
}

struct Wrapper {
  variable::t new_fun_var;
  const FunctionDeclaration* wrapper;
  variable::Map<SpecialisedTo> rewritten_existing_specialised_args;
  inlining_cost::Benefit benefit;
};

Wrapper create_wrapper(const ForOneFunction& for_one_function, inlining_cost::Benefit benefit) {
  variable::t fun_var = for_one_function.fun_var;
  const FunctionDeclaration* function_decl = for_one_function.function_decl;
  // To avoid increasing the free variables of the wrapper, for general
  // cleanliness, we restate the definitions of the newly-specialised
  // arguments in the wrapper itself in terms of the original specialised
  // arguments.  (augment_specialised_args.ml)
  variable::Set params = parameter::set_vars(function_decl->params);
  // rename_function_and_parameters
  variable::t new_fun_var = variable::rename(fun_var);
  std::vector<Parameter> wrapper_params;  // List.map: in order
  for (const Parameter& p : function_decl->params) wrapper_params.push_back(parameter::wrap(variable::rename(p.var)));
  variable::Map<variable::t> params_renaming;  // Variable.Map.of_list
  for (std::size_t k = 0; k < wrapper_params.size(); ++k)
    params_renaming = params_renaming.add(function_decl->params[k].var, wrapper_params[k].var);
  auto find_wrapper_param = [&](variable::t param) -> variable::t {
    if (!params.mem(param)) misc::fatal_error("Augment_specialised_args.find_wrapper_param");  // (assert)
    const variable::t* w = params_renaming.find_opt(param);
    if (!w)
      fatal([&](Formatter& f) {
        fprintf(f, "find_wrapper_param: expected %a to be in [params_renaming], but it is not.",
                pr(variable::print, param));
      });
    return *w;
  };
  variable::Map<variable::t> new_inner_vars_to_spec_args_bound_in_the_wrapper_renaming =
      for_one_function.new_definitions_indexed_by_new_inner_vars.mapi(
          [](variable::t new_inner_var, const Definition&) { return variable::rename(new_inner_var); });
  // N.B.: in the order matching the new specialised argument parameters to
  // the main function.  (Variable.Map.data)
  std::vector<variable::t> args;
  for (const Parameter& p : wrapper_params) args.push_back(p.var);
  new_inner_vars_to_spec_args_bound_in_the_wrapper_renaming.iter([&](variable::t, variable::t v) { args.push_back(v); });
  t apply = flambda::apply(new_fun_var, slice(args), CallKind{new_fun_var}, debuginfo::none(), lambda::InlineAttribute{},
                           lambda::SpecialiseAttribute::Default_specialise);
  t wrapper_body = apply;
  for_one_function.new_definitions_indexed_by_new_inner_vars.iter([&](variable::t new_inner_var, const Definition& d0) {
    Definition definition = d0;
    if (d0.projection) definition.projection = projection::map_projecting_from(d0.projection, find_wrapper_param);
    if (definition.projection) benefit = B::add_projection(definition.projection, benefit);
    const variable::t* new_inner_var_of_wrapper =
        new_inner_vars_to_spec_args_bound_in_the_wrapper_renaming.find_opt(new_inner_var);
    if (!new_inner_var_of_wrapper) misc::fatal_error("Augment_specialised_args.create_wrapper");  // (assert false)
    named n = definition.existing_inner_free_var ? n_expr(var(definition.existing_inner_free_var))
                                                 : flambda_utils::projection_to_named(definition.projection);
    wrapper_body = create_let(*new_inner_var_of_wrapper, n, wrapper_body);
  });
  variable::Map<SpecialisedTo> rewritten_existing_specialised_args = for_one_function.existing_specialised_args.fold(
      [&](variable::t inner_var0, const SpecialisedTo& spec_to, variable::Map<SpecialisedTo> result) {
        variable::t inner_var = find_wrapper_param(inner_var0);
        projection::t p = spec_to.projection
                              ? projection::map_projecting_from(spec_to.projection, find_wrapper_param)
                              : nullptr;
        return result.add(inner_var, SpecialisedTo{spec_to.var, p});
      },
      variable::Map<SpecialisedTo>{});
  const FunctionDeclaration* new_function_decl = create_function_declaration(
      slice(wrapper_params), wrapper_body, true, debuginfo::none(), lambda::InlineAttribute{},
      lambda::SpecialiseAttribute::Default_specialise, false, function_decl->closure_origin,
      lambda::PollAttribute::Default_poll);  // don't propagate attribute to wrappers
  return {new_fun_var, new_function_decl, rewritten_existing_specialised_args, benefit};
}

template <class V>
variable::Map<V> disjoint_union(const variable::Map<V>& a, const variable::Map<V>& b) {
  return variable::Map<V>::union_(
      [](variable::t, const V&, const V&) -> std::optional<V> { misc::fatal_error("Map.disjoint_union"); }, a, b);
}

struct Rewritten {
  variable::Map<const FunctionDeclaration*> funs;
  variable::Map<SpecialisedTo> free_vars;
  variable::Map<SpecialisedTo> specialised_args;
  variable::Map<variable::t> direct_call_surrogates;
  inlining_cost::Benefit benefit;
};

std::optional<Rewritten> rewrite_function_decl(const Processed& t, const E& env,
                                               const DuplicateFunction& duplicate_function,
                                               const ForOneFunction& for_one_function, inlining_cost::Benefit benefit) {
  const SetOfClosures* set_of_closures = t.set_of_closures;
  variable::t fun_var = for_one_function.fun_var;
  const FunctionDeclaration* function_decl = for_one_function.function_decl;
  bool has_no_definition = for_one_function.new_definitions_indexed_by_new_inner_vars.is_empty();
  if (function_decl->stub || has_no_definition || set_of_closures->direct_call_surrogates.mem(fun_var))
    return std::nullopt;
  Wrapper w = create_wrapper(for_one_function, benefit);
  variable::Map<SpecialisedTo> new_specialised_args = for_one_function.new_definitions_indexed_by_new_inner_vars.mapi(
      [&](variable::t new_inner_var, const Definition& definition) -> SpecialisedTo {
        if (set_of_closures->specialised_args.mem(new_inner_var))
          misc::fatal_error("Augment_specialised_args.rewrite_function_decl");  // (assert)
        const variable::t* new_outer_var = for_one_function.new_inner_to_new_outer_vars.find_opt(new_inner_var);
        if (!new_outer_var) misc::fatal_error("Augment_specialised_args.rewrite_function_decl");  // (assert false)
        if (definition.existing_inner_free_var) return SpecialisedTo{*new_outer_var, nullptr};
        variable::t projecting_from = projection::projecting_from(definition.projection);
        if (!set_of_closures->specialised_args.mem(projecting_from) ||
            !parameter::set_vars(function_decl->params).mem(projecting_from))
          misc::fatal_error("Augment_specialised_args.rewrite_function_decl");  // (assert)
        return SpecialisedTo{*new_outer_var, definition.projection};
      });
  variable::Map<SpecialisedTo> specialised_args =
      disjoint_union(w.rewritten_existing_specialised_args, new_specialised_args);
  const FunctionDeclaration* existing_function_decl = nullptr;
  if (for_one_function.make_direct_call_surrogates) {
    auto [fd, nsa] = duplicate_function(env, set_of_closures, fun_var, w.new_fun_var);
    specialised_args = disjoint_union(specialised_args, nsa);
    existing_function_decl = fd;
  }
  std::vector<Parameter> all_params(function_decl->params.begin(), function_decl->params.end());
  for (variable::t v : for_one_function.new_inner_to_new_outer_vars.keys().elements()) all_params.push_back(parameter::wrap(v));
  variable::t closure_origin = w.new_fun_var;  // Closure_origin.create (Closure_id.wrap new_fun_var)
  const FunctionDeclaration* rewritten_function_decl = create_function_declaration(
      slice(all_params), function_decl->body, function_decl->stub, function_decl->dbg, function_decl->inline_,
      function_decl->specialise, function_decl->is_a_functor, closure_origin, function_decl->poll);
  variable::Map<const FunctionDeclaration*> funs;
  variable::Map<variable::t> direct_call_surrogates;
  if (for_one_function.make_direct_call_surrogates) {
    variable::t surrogate = variable::rename(fun_var);
    // In this case, the original function declaration remains untouched up
    // to alpha-equivalence.  (augment_specialised_args.ml)
    if (!existing_function_decl) misc::fatal_error("Augment_specialised_args.rewrite_function_decl");  // (assert false)
    funs = variable::Map<const FunctionDeclaration*>{}
               .add(fun_var, existing_function_decl)
               .add(surrogate, w.wrapper)
               .add(w.new_fun_var, rewritten_function_decl);
    direct_call_surrogates = direct_call_surrogates.add(fun_var, surrogate);
  } else {
    funs = variable::Map<const FunctionDeclaration*>{}.add(fun_var, w.wrapper).add(w.new_fun_var, rewritten_function_decl);
  }
  return Rewritten{funs, {}, specialised_args, direct_call_surrogates, w.benefit};
}

std::pair<t, inlining_cost::Benefit> add_lifted_projections_around_set_of_closures(
    const SetOfClosures* set_of_closures, inlining_cost::Benefit benefit,
    const variable::Map<projection::t>& new_lifted_defns_indexed_by_new_outer_vars) {
  t body = flambda_utils::name_expr(n_set_of_closures(set_of_closures), internal_variable_names::set_of_closures);
  new_lifted_defns_indexed_by_new_outer_vars.iter([&](variable::t new_outer_var, projection::t p) {
    named n = flambda_utils::projection_to_named(p);
    benefit = B::add_projection(p, benefit);
    body = create_let(new_outer_var, n, body);
  });
  return {body, benefit};
}
}  // namespace

std::optional<std::pair<t, inlining_cost::Benefit>> Make::rewrite_set_of_closures(
    const E& env, const DuplicateFunction& duplicate_function, const SetOfClosures* set_of_closures) const {
  auto core = [&]() -> std::optional<std::pair<t, inlining_cost::Benefit>> {
    inlining_cost::Benefit benefit = B::zero();
    Processed what = Processed::create(what_to_specialise(env, set_of_closures));
    const SetOfClosures* original_set_of_closures = set_of_closures;
    variable::Map<const FunctionDeclaration*> funs;
    variable::Map<SpecialisedTo> free_vars = set_of_closures->free_vars;
    variable::Map<SpecialisedTo> specialised_args = set_of_closures->specialised_args;
    variable::Map<variable::t> direct_call_surrogates = set_of_closures->direct_call_surrogates;
    bool done_something = false;
    set_of_closures->function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration* function_decl) {
      const ForOneFunction* f = what.functions.find_opt(fun_var);
      if (!f) {
        funs = funs.add(fun_var, function_decl);
        return;
      }
      std::optional<Rewritten> r = rewrite_function_decl(what, env, duplicate_function, *f, benefit);
      if (!r) {
        funs = funs.add(fun_var, f->function_decl);
        return;
      }
      funs = disjoint_union(funs, r->funs);
      direct_call_surrogates = disjoint_union(direct_call_surrogates, r->direct_call_surrogates);
      free_vars = disjoint_union(free_vars, r->free_vars);
      specialised_args = disjoint_union(specialised_args, r->specialised_args);
      done_something = true;
      benefit = r->benefit;
    });
    if (!done_something) return std::nullopt;
    const FunctionDeclarations* function_decls = update_function_declarations(set_of_closures->function_decls, funs);
    if (specialised_args.cardinal() < original_set_of_closures->specialised_args.cardinal())
      misc::fatal_error("Augment_specialised_args.rewrite_set_of_closures");  // (assert)
    const SetOfClosures* soc = create_set_of_closures(function_decls, free_vars, specialised_args, direct_call_surrogates);
    if (clflags::flambda_invariant_checks) check_invariants(pass_name, soc, original_set_of_closures);
    return add_lifted_projections_around_set_of_closures(soc, benefit, what.new_lifted_defns_indexed_by_new_outer_vars);
  };
  return pass_wrapper::with_dump<std::pair<t, inlining_cost::Benefit>>(
      *env.ppf_dump(), pass_name, core, [&](Formatter& f) { print_set_of_closures(f, set_of_closures); },
      [](Formatter& f, const std::pair<t, inlining_cost::Benefit>& r) { print(f, r.first); });
}

}  // namespace cppcaml::typing::augment_specialised_args
