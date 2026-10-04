// Port of middle_end/flambda/remove_free_vars_equal_to_args.ml and
// remove_unused_arguments.ml (see remove_free_vars_equal_to_args.hpp).
#include "cppcaml/typing/remove_free_vars_equal_to_args.hpp"

#include <cstdio>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/invariant_params.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pass_wrapper.hpp"

namespace cppcaml::typing {

using namespace flambda;

namespace remove_free_vars_equal_to_args {
namespace {
const char* const pass_name = "remove-free-vars-equal-to-args";
const bool registered = pass_wrapper::register_pass(pass_name);

const FunctionDeclaration* rewrite_one_function_decl(const FunctionDeclaration* function_decl,
                                                     const variable::Map<variable::Set>& back_free_vars,
                                                     const variable::Map<SpecialisedTo>& specialised_args) {
  variable::Map<variable::t> params_for_equal_free_vars;
  for (const Parameter& p : function_decl->params) {
    const SpecialisedTo* spec_to = specialised_args.find_opt(p.var);
    if (!spec_to) continue;  // param is not specialised
    const variable::Set* set = back_free_vars.find_opt(spec_to->var);
    if (!set) continue;  // No free variables equal to the param
    // Replace the free variables equal to a parameter
    set->iter([&](variable::t free_var) { params_for_equal_free_vars = params_for_equal_free_vars.add(free_var, p.var); });
  }
  if (params_for_equal_free_vars.is_empty()) return function_decl;
  t body = flambda_utils::toplevel_substitution(params_for_equal_free_vars, function_decl->body);
  return update_function_declaration(function_decl, function_decl->params, body);
}

const SetOfClosures* rewrite_one_set_of_closures(const SetOfClosures* set_of_closures) {
  variable::Map<variable::Set> back_free_vars = set_of_closures->free_vars.fold(
      [](variable::t var, const SpecialisedTo& outside_var, variable::Map<variable::Set> map) {
        const variable::Set* s = map.find_opt(outside_var.var);
        return map.add(outside_var.var, s ? s->add(var) : variable::Set::singleton(var));
      },
      variable::Map<variable::Set>{});
  bool done_something = false;
  variable::Map<const FunctionDeclaration*> funs =
      set_of_closures->function_decls->funs.map([&](const FunctionDeclaration* function_decl) {
        const FunctionDeclaration* nd =
            rewrite_one_function_decl(function_decl, back_free_vars, set_of_closures->specialised_args);
        if (nd != function_decl) done_something = true;
        return nd;
      });
  if (!done_something) return nullptr;
  const FunctionDeclarations* function_decls = update_function_declarations(set_of_closures->function_decls, funs);
  return create_set_of_closures(function_decls, set_of_closures->free_vars, set_of_closures->specialised_args,
                                set_of_closures->direct_call_surrogates);
}
}  // namespace

const SetOfClosures* run(format::Formatter& ppf_dump, const SetOfClosures* set_of_closures) {
  std::optional<const SetOfClosures*> r = pass_wrapper::with_dump<const SetOfClosures*>(
      ppf_dump, pass_name,
      [&]() -> std::optional<const SetOfClosures*> {
        const SetOfClosures* s = rewrite_one_set_of_closures(set_of_closures);
        if (!s) return std::nullopt;
        return s;
      },
      [&](format::Formatter& f) { print_set_of_closures(f, set_of_closures); },
      [](format::Formatter& f, const SetOfClosures* const& s) { print_set_of_closures(f, s); });
  return r ? *r : nullptr;
}
}  // namespace remove_free_vars_equal_to_args

namespace remove_unused_arguments {
namespace {
const char* const pass_name = "remove-unused-arguments";
const bool registered = pass_wrapper::register_pass(pass_name);

variable::t rename_var(variable::t var) { return variable::rename(var, compilation_unit::get_current_exn()); }

const FunctionDeclaration* remove_params(const variable::Set& unused, const FunctionDeclaration* fun_decl,
                                         variable::t new_fun_var) {
  std::vector<Parameter> unused_params, used_params;  // List.partition
  for (const Parameter& p : fun_decl->params) (unused.mem(p.var) ? unused_params : used_params).push_back(p);
  std::vector<Parameter> unused_free;  // List.filter
  for (const Parameter& p : unused_params)
    if (fun_decl->free_variables.mem(p.var)) unused_free.push_back(p);
  t body = fun_decl->body;
  for (const Parameter& p : unused_free) body = create_let(p.var, FLAMBDA_NAMED_INT_LITERAL(0), body);
  return create_function_declaration(slice(used_params), body, fun_decl->stub, fun_decl->dbg, fun_decl->inline_,
                                     fun_decl->specialise, fun_decl->is_a_functor, new_fun_var, fun_decl->poll);
}

struct Stub {
  const FunctionDeclaration* function_decl;
  variable::t renamed;
  variable::Map<SpecialisedTo> additional_specialised_args;
};

Stub make_stub(const variable::Set& unused, variable::t var, const FunctionDeclaration* fun_decl,
               const variable::Map<SpecialisedTo>& specialised_args,
               variable::Map<SpecialisedTo> additional_specialised_args) {
  variable::t renamed = rename_var(var);
  std::vector<std::pair<Parameter, Parameter>> args2;  // List.map: in order
  for (const Parameter& p : fun_decl->params) args2.emplace_back(p, parameter::wrap(variable::rename(p.var)));
  std::vector<std::pair<Parameter, Parameter>> used_args2;
  for (const auto& a : args2)
    if (!unused.mem(a.first.var)) used_args2.push_back(a);
  variable::Map<variable::t> args_renaming;  // Variable.Map.of_list
  for (const auto& a : args2) args_renaming = args_renaming.add(a.first.var, a.second.var);
  for (const auto& a : args2) {
    variable::t original_arg = a.first.var, arg = a.second.var;
    const SpecialisedTo* outer = specialised_args.find_opt(original_arg);
    if (!outer) continue;
    // CR-soon mshinwell: share with Augment_specialised_args
    SpecialisedTo outer_var = *outer;
    if (outer->projection)
      outer_var.projection = projection::map_projecting_from(outer->projection, [&](variable::t v) {
        const variable::t* w = args_renaming.find_opt(v);
        // Must always be a parameter of this [function_decl].
        if (!w) misc::fatal_error("Remove_unused_arguments.make_stub");  // (assert false)
        return *w;
      });
    additional_specialised_args = additional_specialised_args.add(arg, outer_var);
  }
  std::vector<variable::t> args;
  for (const auto& a : used_args2) args.push_back(a.second.var);
  t body = apply(renamed, slice(args), CallKind{renamed}, fun_decl->dbg, lambda::InlineAttribute{},
                 lambda::SpecialiseAttribute::Default_specialise);
  std::vector<Parameter> params;
  for (const auto& a : args2) params.push_back(a.second);
  const FunctionDeclaration* function_decl = create_function_declaration(
      slice(params), body, true, fun_decl->dbg, lambda::InlineAttribute{},
      lambda::SpecialiseAttribute::Default_specialise, fun_decl->is_a_functor, fun_decl->closure_origin,
      lambda::PollAttribute::Default_poll);  // don't propagate attribute to wrappers
  return {function_decl, renamed, additional_specialised_args};
}

const SetOfClosures* separate_unused_arguments(bool only_specialised, const SetOfClosures* set_of_closures) {
  const FunctionDeclarations* function_decls = set_of_closures->function_decls;
  variable::Set unused = invariant_params::unused_arguments(function_decls);
  variable::Set non_stub_arguments = function_decls->funs.fold(
      [](variable::t, const FunctionDeclaration* decl, variable::Set acc) {
        if (decl->stub) return acc;
        return variable::Set::union_(acc, parameter::set_vars(decl->params));
      },
      variable::Set{});
  unused = variable::Set::inter(non_stub_arguments, unused);
  variable::Set specialised_args = set_of_closures->specialised_args.keys();
  if (only_specialised) unused = variable::Set::inter(specialised_args, unused);
  if (unused.is_empty()) return nullptr;
  variable::Map<const FunctionDeclaration*> funs;
  variable::Map<SpecialisedTo> additional_specialised_args;
  function_decls->funs.iter([&](variable::t fun_id, const FunctionDeclaration* fun_decl) {
    bool any = false;
    for (const Parameter& p : fun_decl->params) any = any || unused.mem(p.var);
    if (!any) {
      funs = funs.add(fun_id, fun_decl);
      return;
    }
    Stub s = make_stub(unused, fun_id, fun_decl, set_of_closures->specialised_args, additional_specialised_args);
    additional_specialised_args = s.additional_specialised_args;
    const FunctionDeclaration* cleaned = remove_params(unused, fun_decl, s.renamed);
    funs = funs.add(s.renamed, cleaned).add(fun_id, s.function_decl);
  });
  variable::Map<SpecialisedTo> sa = variable::Map<SpecialisedTo>::union_(
      [](variable::t, const SpecialisedTo&, const SpecialisedTo&) -> std::optional<SpecialisedTo> {
        misc::fatal_error("Map.disjoint_union");
      },
      additional_specialised_args,
      set_of_closures->specialised_args.filter([&](variable::t param, const SpecialisedTo&) { return !unused.mem(param); }));
  sa = flambda_utils::clean_projections(sa);
  const FunctionDeclarations* fds = update_function_declarations(function_decls, funs);
  // CR-soon mshinwell: Use direct_call_surrogates for this transformation.
  return create_set_of_closures(fds, set_of_closures->free_vars, sa, set_of_closures->direct_call_surrogates);
}

// Splitting is not always beneficial.  (remove_unused_arguments.ml)
bool should_split_only_specialised_args(const FunctionDeclarations* fun_decls) {
  if (!clflags::remove_unused_arguments) return true;
  bool no_recursive_functions = find_recursive_functions::in_function_declarations(fun_decls).is_empty();
  long number_of_non_stub_functions =
      fun_decls->funs.filter([](variable::t, const FunctionDeclaration* d) { return !d->stub; }).cardinal();
  // CR-soon lwhite: this criteria could use some justification.
  return no_recursive_functions && number_of_non_stub_functions <= 1;
}
}  // namespace

const SetOfClosures* separate_unused_arguments_in_set_of_closures(const SetOfClosures* set_of_closures) {
  bool dump = pass_wrapper::dumped_pass(pass_name);
  bool only_specialised = should_split_only_specialised_args(set_of_closures->function_decls);
  const SetOfClosures* r = separate_unused_arguments(only_specialised, set_of_closures);
  if (dump) {
    format::Formatter f;
    if (!r)
      format::fprintf(f, "No change for Remove_unused_arguments:@ %a@.@.", pr(print_set_of_closures, set_of_closures));
    else
      format::fprintf(f, "Before Remove_unused_arguments:@ %a@.@.After Remove_unused_arguments:@ %a@.@.",
                      pr(print_set_of_closures, set_of_closures), pr(print_set_of_closures, r));
    std::fwrite(f.contents().data(), 1, f.contents().size(), stderr);
  }
  return r;
}

Program separate_unused_arguments_in_closures(const Program& program) {
  return flambda_iterators::map_exprs_at_toplevel_of_program(program, [](t expr) {
    return flambda_iterators::map_named(
        [](named n) -> named {
          auto* s = as<NSet_of_closures>(n);
          if (!s) return n;
          bool only_specialised = should_split_only_specialised_args(s->set->function_decls);
          const SetOfClosures* r = separate_unused_arguments(only_specialised, s->set);
          return r ? n_set_of_closures(r) : n;
        },
        expr);
  });
}

}  // namespace remove_unused_arguments
}  // namespace cppcaml::typing
