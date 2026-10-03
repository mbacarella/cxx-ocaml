// Port of middle_end/flambda/invariant_params.ml and
// find_recursive_functions.ml (see invariant_params.hpp).
#include "cppcaml/typing/invariant_params.hpp"

#include <cstdio>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/strongly_connected_components.hpp"

namespace cppcaml::typing::invariant_params {

using namespace flambda;

namespace {
// t = Top | Implication of Variable.Pair.Set.t
struct Rel {
  bool top;
  PairSet set;
};
using Relation = OMap<Pair, Rel, PairCmp>;

Relation top(const Relation& relation, const Pair& p) { return relation.add(p, Rel{true, {}}); }

Relation implies(const Relation& relation, const Pair& from, const Pair& to) {
  const Rel* r = relation.find_opt(to);
  if (!r) return relation.add(to, Rel{false, PairSet::singleton(from)});
  if (r->top) return relation;
  return relation.add(to, Rel{false, r->set.add(from)});
}

Relation transitive_closure(const Relation& state) {
  // Depth-first search for all implications for one argument.  Arguments
  // are moved from candidate to frontier, assuming they are newly added to
  // the result.
  return state.map([&](const Rel& r) -> Rel {
    if (r.top) return r;
    std::vector<Pair> candidate;  // (head: back)
    std::vector<Pair> frontier;   // (head: back)
    {
      std::vector<Pair> e = r.set.elements();
      frontier.assign(e.rbegin(), e.rend());
    }
    PairSet result = r.set;
    for (;;) {
      if (candidate.empty()) {
        if (frontier.empty()) return Rel{false, result};
        Pair f = frontier.back();
        frontier.pop_back();
        // Obtain fresh candidate for the frontier argument.
        const Rel* c = state.find_opt(f);
        if (!c) continue;
        if (c->top) return Rel{true, {}};
        std::vector<Pair> e = c->set.elements();
        candidate.assign(e.rbegin(), e.rend());
        continue;
      }
      Pair c = candidate.back();
      candidate.pop_back();
      PairSet result2 = result.add(c);
      if (!result2.same_as(result)) {
        // Result change means candidate becomes part of frontier.
        frontier.push_back(c);
        result = result2;
      }
    }
  });
}

// CR-soon pchambart: to move to Flambda_utils and document
// Finds variables that represent the functions.  (invariant_params.ml)
variable::Map<variable::t> function_variable_alias(const FunctionDeclarations* function_decls) {
  variable::Set fun_vars = function_decls->funs.keys();
  symbol::Map<variable::t> symbols_to_fun_vars = fun_vars.fold(
      [](variable::t fun_var, symbol::Map<variable::t> acc) {
        return acc.add(compilenv::closure_symbol(fun_var), fun_var);
      },
      symbol::Map<variable::t>{});
  variable::Map<variable::t> fun_var_bindings;
  function_decls->funs.iter([&](variable::t, const FunctionDeclaration* function_decl) {
    flambda_iterators::iter_all_toplevel_immutable_let_bindings(function_decl->body, [&](variable::t var, named n) {
      if (auto* s = as<NSymbol>(n))
        if (const variable::t* fun_var = symbols_to_fun_vars.find_opt(s->sym))
          fun_var_bindings = fun_var_bindings.add(var, *fun_var);
    });
  });
  return fun_var_bindings;
}

using ParamToParam = std::function<Relation(variable::t caller, variable::t caller_arg, variable::t callee,
                                            variable::t callee_arg, const Relation&)>;
using AnythingToParam = std::function<Relation(variable::t callee, variable::t callee_arg, const Relation&)>;
using ParamToAnywhere = std::function<Relation(variable::t caller, variable::t caller_arg, const Relation&)>;

Relation analyse_functions(const ParamToParam& param_to_param, const AnythingToParam& anything_to_param,
                           const ParamToAnywhere& param_to_anywhere, const FunctionDeclarations* decls) {
  variable::Map<variable::t> fva = function_variable_alias(decls);
  variable::Map<Slice<Parameter>> param_indexes_by_fun_vars =
      decls->funs.map([](const FunctionDeclaration* decl) { return decl->params; });
  auto find_callee_arg = [&](variable::t callee, long callee_pos) -> variable::t {
    const Slice<Parameter>* arr = param_indexes_by_fun_vars.find_opt(callee);
    if (!arr) return nullptr;  // not a recursive call
    // Ignore overapplied parameters: they are applied to a different
    // function.
    if (callee_pos < static_cast<long>(arr->size())) return (*arr)[static_cast<std::size_t>(callee_pos)].var;
    return nullptr;
  };
  variable::Set escaping_functions, used_variables;
  auto escaping_function = [&](variable::t fun_var) {
    if (const variable::t* a = fva.find_opt(fun_var)) fun_var = *a;
    if (decls->funs.mem(fun_var)) escaping_functions = escaping_functions.add(fun_var);
  };
  auto used_variable = [&](variable::t var) { used_variables = used_variables.add(var); };
  Relation relation;
  // If the called closure is in the current set of closures, record the
  // relation (callee, callee_arg) <- (caller, caller_arg)
  auto check_argument = [&](variable::t caller, variable::t callee, long callee_pos, variable::t caller_arg) {
    escaping_function(caller_arg);
    variable::t callee_arg = find_callee_arg(callee, callee_pos);
    if (!callee_arg) {
      used_variable(caller_arg);  // not a recursive call
      return;
    }
    const FunctionDeclaration* const* d = decls->funs.find_opt(caller);
    if (!d) misc::fatal_error("Invariant_params.analyse_functions");  // (assert false)
    bool is_param = false;
    for (const Parameter& p : (*d)->params) is_param = is_param || variable::equal(p.var, caller_arg);
    // We only track dataflow for parameters of functions, not arbitrary
    // variables.
    if (is_param) relation = param_to_param(caller, caller_arg, callee, callee_arg, relation);
    else {
      used_variable(caller_arg);
      relation = anything_to_param(callee, callee_arg, relation);
    }
  };
  auto arity = [&](variable::t callee) -> long {
    const FunctionDeclaration* const* d = decls->funs.find_opt(callee);
    return d ? static_cast<long>((*d)->params.size()) : 0;
  };
  auto check_expr = [&](variable::t caller, t expr) {
    auto* a = as<Apply>(expr);
    if (!a) return;
    used_variable(a->func);
    variable::t callee = a->func;
    if (const variable::t* c = fva.find_opt(a->func)) callee = *c;
    long num_args = static_cast<long>(a->args.size());
    for (long callee_pos = num_args; callee_pos <= arity(callee) - 1; ++callee_pos) {
      // If a function is partially applied, consider all missing arguments
      // as "anything".
      if (variable::t callee_arg = find_callee_arg(callee, callee_pos))
        relation = anything_to_param(callee, callee_arg, relation);
    }
    for (std::size_t k = 0; k < a->args.size(); ++k)
      check_argument(caller, callee, static_cast<long>(k), a->args[k]);
  };
  decls->funs.iter([&](variable::t caller, const FunctionDeclaration* decl) {
    flambda_iterators::iter([&](t e) { check_expr(caller, e); }, [](named) {}, decl->body);
    FvOpts o;
    o.ignore_uses_as_callee = true;
    o.ignore_uses_as_argument = true;
    free_variables(decl->body, o).iter([&](variable::t var) {
      escaping_function(var);
      used_variable(var);
    });
  });
  decls->funs.iter([&](variable::t func_var, const FunctionDeclaration* decl) {
    for (const Parameter& param : decl->params) {
      if (used_variables.mem(param.var)) relation = param_to_anywhere(func_var, param.var, relation);
      if (escaping_functions.mem(func_var)) relation = anything_to_param(func_var, param.var, relation);
    }
  });
  return transitive_closure(relation);
}

// (a registered pass: -dump-pass unused-arguments)
const bool registered = (clflags::all_passes.insert(clflags::all_passes.begin(), "unused-arguments"), true);
}  // namespace

variable::Map<variable::Set> invariant_params_in_recursion(const FunctionDeclarations* decls) {
  Relation relation = analyse_functions(
      [](variable::t caller, variable::t caller_arg, variable::t callee, variable::t callee_arg, const Relation& r) {
        return implies(r, {caller, caller_arg}, {callee, callee_arg});
      },
      [](variable::t callee, variable::t callee_arg, const Relation& r) { return top(r, {callee, callee_arg}); },
      [](variable::t, variable::t, const Relation& r) { return r; }, decls);
  variable::Set not_unchanging = relation.fold(
      [](const Pair& k, const Rel& set, variable::Set acc) {
        if (set.top) return acc.add(k.second);
        if (set.set.exists([&](const Pair& p) {
              return variable::equal(k.first, p.first) && !variable::equal(k.second, p.second);
            }))
          return acc.add(k.second);
        return acc;
      },
      variable::Set{});
  variable::Set params = decls->funs.fold(
      [](variable::t, const FunctionDeclaration* d, variable::Set acc) {
        return variable::Set::union_(parameter::set_vars(d->params), acc);
      },
      variable::Set{});
  variable::Set unchanging = variable::Set::diff(params, not_unchanging);
  variable::Map<variable::Set> aliased_to = relation.fold(
      [&](const Pair& k, const Rel& set, variable::Map<variable::Set> aliases) {
        variable::t var = k.second;
        if (set.top || !unchanging.mem(var)) return aliases;
        return set.set.fold(
            [&](const Pair& p, variable::Map<variable::Set> al) {
              variable::t caller_args = p.second;
              if (!unchanging.mem(caller_args)) return al;
              const variable::Set* s = al.find_opt(caller_args);
              return al.add(caller_args, s ? s->add(var) : variable::Set::singleton(var));
            },
            aliases);
      },
      variable::Map<variable::Set>{});
  // We complete the set of aliases such that there does not miss any
  // unchanging param  (Variable.Map.of_set)
  return unchanging.fold(
      [&](variable::t var, variable::Map<variable::Set> m) {
        const variable::Set* s = aliased_to.find_opt(var);
        return m.add(var, s ? *s : variable::Set{});
      },
      variable::Map<variable::Set>{});
}

variable::Map<PairSet> invariant_param_sources(const FunctionDeclarations* decls) {
  Relation relation = analyse_functions(
      [](variable::t caller, variable::t caller_arg, variable::t callee, variable::t callee_arg, const Relation& r) {
        return implies(r, {caller, caller_arg}, {callee, callee_arg});
      },
      [](variable::t, variable::t, const Relation& r) { return r; },
      [](variable::t, variable::t, const Relation& r) { return r; }, decls);
  return relation.fold(
      [](const Pair& k, const Rel& set, variable::Map<PairSet> r) {
        if (set.top) return r;
        return r.add(k.second, set.set);
      },
      variable::Map<PairSet>{});
}

variable::Set unused_arguments(const FunctionDeclarations* decls) {
  bool dump = false;
  for (const std::string& p : clflags::dumped_passes_list) dump = dump || p == "unused-arguments";
  Relation relation = analyse_functions(
      [](variable::t caller, variable::t caller_arg, variable::t callee, variable::t callee_arg, const Relation& r) {
        return implies(r, {callee, callee_arg}, {caller, caller_arg});
      },
      [](variable::t, variable::t, const Relation& r) { return r; },
      [](variable::t caller, variable::t caller_arg, const Relation& r) { return top(r, {caller, caller_arg}); },
      decls);
  variable::Set arguments = decls->funs.fold(
      [&](variable::t fun_var, const FunctionDeclaration* decl, variable::Set acc) {
        for (const Parameter& p : decl->params) {
          const Rel* r = relation.find_opt({fun_var, p.var});
          if (!r || !r->top) acc = acc.add(p.var);
        }
        return acc;
      },
      variable::Set{});
  if (dump) {
    format::Formatter f;
    format::fprintf(f, "Unused arguments: %a@.", [&](format::Formatter& g) { variable::print_set(g, arguments); });
    std::fwrite(f.contents().data(), 1, f.contents().size(), stdout);
  }
  return arguments;
}

}  // namespace cppcaml::typing::invariant_params

namespace cppcaml::typing::find_recursive_functions {

variable::Set in_function_declarations(const flambda::FunctionDeclarations* function_decls) {
  variable::Map<variable::Set> directed_graph =
      flambda_utils::fun_vars_referenced_in_decls(function_decls, compilenv::closure_symbol);
  auto connected_components =
      strongly_connected_components::connected_components_sorted_from_roots_to_leaf(directed_graph);
  variable::Set rec_fun;
  for (const auto& c : connected_components)
    if (c.has_loop)
      for (std::size_t k = c.ids.size(); k-- > 0;) rec_fun = rec_fun.add(c.ids[k]);  // List.fold_right
  return rec_fun;
}

}  // namespace cppcaml::typing::find_recursive_functions
