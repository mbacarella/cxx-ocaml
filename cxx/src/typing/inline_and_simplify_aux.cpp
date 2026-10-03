// Port of middle_end/flambda/inline_and_simplify_aux.ml (see
// inline_and_simplify_aux.hpp).
#include "cppcaml/typing/inline_and_simplify_aux.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/import_approx.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inline_and_simplify_aux {

using format::Formatter;
using format::fprintf;
namespace F = flambda;

namespace {
[[noreturn]] void fatal(const std::function<void(Formatter&)>& msg) {
  Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}
}  // namespace

Env Env::create(bool never_inline, long round, Formatter* ppf_dump) {
  Env e;
  e.round_ = round;
  e.ppf_dump_ = ppf_dump;
  e.never_inline_ = never_inline;
  e.inlining_stats_closure_stack = inlining_stats::closure_stack::create();
  e.inlined_debuginfo = debuginfo::none();
  return e;
}

Env Env::local() const {
  Env e = *this;
  e.approx = {};
  e.projections = {};
  e.freshening_ = freshening::empty_preserving_activation_state(freshening_);
  e.inlined_debuginfo = debuginfo::none();
  return e;
}

Env Env::inlining_level_up() const {
  long max_level = arg_helper::get(round_, clflags::inline_max_depth);
  if (inlining_level_ + 1 > max_level) misc::fatal_error("Inlining level increased above maximum");
  Env e = *this;
  e.inlining_level_ = inlining_level_ + 1;
  return e;
}

void Env::print(Formatter& ppf) const {
  auto projs = [&](Formatter& f) {
    auto elts = [&](Formatter& g) {
      projections.iter([&](projection::t p, variable::t v) {
        fprintf(g, "@ (@[%a@ %a@])", pr(projection::print, p), pr(variable::print, v));
      });
    };
    fprintf(f, "@[<1>{@[%a@ @]}@]", elts);
  };
  fprintf(ppf, "Environment maps: %a@.Projections: %a@.Freshening: %a@.",
          [&](Formatter& f) { variable::print_set(f, approx.keys()); }, projs,
          [&](Formatter& f) { freshening::print(f, freshening_); });
}

Env Env::add_internal(variable::t var, A::t a, Scope scope) const {
  // The semantics of this [match] are what preserve the property described
  // at the top of simple_value_approx.mli, namely that when a [var] is mem
  // on an approximation (amongst many possible [var]s), it is the one with
  // the outermost scope.
  if (!(a->var && mem(a->var))) a = A::augment_with_variable(a, var);
  Env e = *this;
  e.approx = approx.add(var, ScopedApprox{scope, a});
  return e;
}

Env Env::add_mutable(variable::t mut_var, A::t a) const {
  Env e = *this;
  e.approx_mutable = approx_mutable.add(mut_var, a);
  return e;
}

A::t Env::really_import_approx(A::t a) { return import_approx::really_import_approx(a); }

A::t Env::find_symbol_exn(symbol::t s) const {
  const A::t* a = approx_sym.find_opt(s);
  if (!a) return nullptr;
  return really_import_approx(*a);
}

A::t Env::find_symbol_fatal(symbol::t s) const {
  A::t a = find_symbol_exn(s);
  if (!a)
    fatal([&](Formatter& f) {
      fprintf(f, "Symbol %a is unbound.  Maybe there is a missing [Let_symbol], [Import_symbol] or similar?",
              pr(symbol::print, s));
    });
  return a;
}

A::t Env::find_or_load_symbol(symbol::t s) const {
  A::t a = find_symbol_exn(s);
  if (a) return a;
  if (compilation_unit::equal(compilation_unit::get_current_exn(), symbol::compilation_unit(s)))
    fatal([&](Formatter& f) {
      fprintf(f, "Symbol %a from the current compilation unit is unbound.  Maybe there is a missing [Let_symbol] or "
                 "similar?",
              pr(symbol::print, s));
    });
  return import_approx::import_symbol(s);
}

Env Env::add_projection(projection::t projection, variable::t bound_to) const {
  Env e = *this;
  e.projections = projections.add(projection, bound_to);
  return e;
}

variable::t Env::find_projection(projection::t projection) const {
  const variable::t* v = projections.find_opt(projection);
  return v ? *v : nullptr;
}

bool Env::does_not_bind(Slice<variable::t> vars) const {
  for (variable::t v : vars)
    if (mem(v)) return false;
  return true;
}

bool Env::does_not_freshen(Slice<variable::t> vars) const { return freshening::does_not_freshen(freshening_, vars); }

Env Env::add_symbol(symbol::t s, A::t a) const {
  if (find_symbol_exn(s))
    fatal([&](Formatter& f) {
      fprintf(f, "Attempt to redefine symbol %a (to %a) in environment for [Inline_and_simplify]",
              pr(symbol::print, s), pr(A::print, a));
    });
  Env e = *this;
  e.approx_sym = approx_sym.add(s, a);
  return e;
}

Env Env::redefine_symbol(symbol::t s, A::t a) const {
  if (!find_symbol_exn(s)) misc::fatal_error("Inline_and_simplify_aux.Env.redefine_symbol");  // (assert false)
  Env e = *this;
  e.approx_sym = approx_sym.add(s, a);
  return e;
}

Env::ScopedApprox Env::find_with_scope_exn(variable::t id) const {
  const ScopedApprox* sa = approx.find_opt(id);
  if (!sa)
    fatal([&](Formatter& f) {
      fprintf(f, "Env.find_with_scope_exn: Unbound variable %a@.%s@. Environment: %a@.", pr(variable::print, id), "",
              [&](Formatter& g) { print(g); });
    });
  return ScopedApprox{sa->scope, really_import_approx(sa->approx)};
}

A::t Env::find_mutable_exn(variable::t mut_var) const {
  const A::t* a = approx_mutable.find_opt(mut_var);
  if (!a)
    fatal([&](Formatter& f) {
      fprintf(f, "Env.find_mutable_exn: Unbound variable %a@.%s@. Environment: %a@.", pr(variable::print, mut_var), "",
              [&](Formatter& g) { print(g); });
    });
  return *a;
}

std::vector<A::t> Env::find_list_exn(Slice<variable::t> vars) const {
  std::vector<A::t> out;  // List.map: in order
  for (variable::t v : vars) out.push_back(find_exn(v));
  return out;
}

A::t Env::find_opt(variable::t id) const {
  const ScopedApprox* sa = approx.find_opt(id);
  if (!sa) return nullptr;
  return really_import_approx(sa->approx);
}

Env Env::activate_freshening() const {
  Env e = *this;
  e.freshening_ = freshening::activate(freshening_);
  return e;
}

Env Env::enter_set_of_closures_declaration(set_of_closures_id::t origin) const {
  Env e = *this;
  e.current_functions = current_functions.add(origin);
  return e;
}

Env Env::inside_branch() const {
  Env e = *this;
  e.inside_branch_ = inside_branch_ + 1;
  return e;
}

Env Env::set_freshening(freshening::T f) const {
  Env e = *this;
  e.freshening_ = f;
  return e;
}

Env Env::increase_closure_depth() const {
  Env e = *this;
  e.approx = approx.map([](const ScopedApprox& sa) { return ScopedApprox{Scope::Outer, sa.approx}; });
  e.closure_depth = closure_depth + 1;
  return e;
}

Env Env::set_never_inline() const {
  if (never_inline_) return *this;
  Env e = *this;
  e.never_inline_ = true;
  return e;
}
Env Env::set_never_inline_inside_closures() const {
  if (never_inline_inside_closures) return *this;
  Env e = *this;
  e.never_inline_inside_closures = true;
  return e;
}
Env Env::unset_never_inline_inside_closures() const {
  if (!never_inline_inside_closures) return *this;
  Env e = *this;
  e.never_inline_inside_closures = false;
  return e;
}
Env Env::set_never_inline_outside_closures() const {
  if (never_inline_outside_closures) return *this;
  Env e = *this;
  e.never_inline_outside_closures = true;
  return e;
}
Env Env::unset_never_inline_outside_closures() const {
  if (!never_inline_outside_closures) return *this;
  Env e = *this;
  e.never_inline_outside_closures = false;
  return e;
}

std::optional<long> Env::actively_unrolling(set_of_closures_id::t origin) const {
  const long* c = actively_unrolling_.find_opt(origin);
  if (!c) return std::nullopt;
  return *c;
}

Env Env::start_actively_unrolling(set_of_closures_id::t origin, long i) const {
  Env e = *this;
  e.actively_unrolling_ = actively_unrolling_.add(origin, i);
  return e;
}

Env Env::continue_actively_unrolling(set_of_closures_id::t origin) const {
  const long* unrolling = actively_unrolling_.find_opt(origin);
  if (!unrolling) misc::fatal_error("Unexpected actively unrolled function");
  Env e = *this;
  e.actively_unrolling_ = actively_unrolling_.add(origin, *unrolling - 1);
  return e;
}

bool Env::unrolling_allowed(set_of_closures_id::t origin) const {
  const long* c = unroll_counts.find_opt(origin);
  long unroll_count = c ? *c : arg_helper::get(round_, clflags::inline_max_unroll);
  return unroll_count > 0;
}

Env Env::inside_unrolled_function(set_of_closures_id::t origin) const {
  const long* c = unroll_counts.find_opt(origin);
  long unroll_count = c ? *c : arg_helper::get(round_, clflags::inline_max_unroll);
  Env e = *this;
  e.unroll_counts = unroll_counts.add(origin, unroll_count - 1);
  e.inlined_stub = {};
  return e;
}

bool Env::inlining_allowed(variable::t id) const {
  const long* c = inlining_counts.find_opt(id);
  long inlining_count = c ? *c : std::max(1L, arg_helper::get(round_, clflags::inline_max_unroll));
  return inlining_count > 0;
}

Env Env::inside_inlined_function(variable::t id) const {
  const long* c = inlining_counts.find_opt(id);
  long inlining_count = c ? *c : std::max(1L, arg_helper::get(round_, clflags::inline_max_unroll));
  Env e = *this;
  e.inlining_counts = inlining_counts.add(id, inlining_count - 1);
  // inlining_stubs prevents recursive stub inlining.  But as soon as we
  // inline another kind of function, we can reactivate stub inlining.
  e.inlined_stub = {};
  return e;
}

Env Env::inside_inlined_stub_function(set_of_closures_id::t id) const {
  Env e = *this;
  e.inlined_stub = inlined_stub.add(id);
  return e;
}

Env Env::note_entering_closure(variable::t closure_id, const debuginfo::t& dbg) const {
  if (never_inline_) return *this;
  Env e = *this;
  e.inlining_stats_closure_stack =
      inlining_stats::closure_stack::note_entering_closure(inlining_stats_closure_stack, closure_id, dbg);
  return e;
}
Env Env::note_entering_call(variable::t closure_id, const debuginfo::t& dbg) const {
  if (never_inline_) return *this;
  Env e = *this;
  e.inlining_stats_closure_stack =
      inlining_stats::closure_stack::note_entering_call(inlining_stats_closure_stack, closure_id, dbg);
  return e;
}
Env Env::note_entering_inlined() const {
  if (never_inline_) return *this;
  Env e = *this;
  e.inlining_stats_closure_stack = inlining_stats::closure_stack::note_entering_inlined(inlining_stats_closure_stack);
  return e;
}
Env Env::note_entering_specialised(const variable::Set& closure_ids) const {
  if (never_inline_) return *this;
  Env e = *this;
  e.inlining_stats_closure_stack =
      inlining_stats::closure_stack::note_entering_specialised(inlining_stats_closure_stack, closure_ids);
  return e;
}

Env Env::enter_closure(variable::t closure_id, bool inline_inside, const debuginfo::t& dbg) const {
  Env t = (inline_inside && !never_inline_inside_closures) ? *this : set_never_inline();
  t = t.unset_never_inline_outside_closures();
  return t.note_entering_closure(closure_id, dbg);
}

void Env::record_decision(const inlining_stats::Decision& decision) const {
  inlining_stats::record_decision(decision, inlining_stats_closure_stack);
}

Env Env::set_inline_debuginfo(const debuginfo::t& dbg) const {
  Env e = *this;
  e.inlined_debuginfo = dbg;
  return e;
}

debuginfo::t Env::add_inlined_debuginfo(const debuginfo::t& dbg) const { return debuginfo::inline_(inlined_debuginfo, dbg); }

namespace {
// int_of_float (in range here)
long int_of_float(double f) { return static_cast<long>(f); }
}  // namespace

inlining_cost::Threshold initial_inlining_threshold(long round) {
  double unscaled = arg_helper::get(round, clflags::inline_threshold);
  // CR-soon pchambart: Add a warning if this is too big
  return inlining_cost::Threshold::can_inline_if_no_larger_than(
      int_of_float(unscaled * static_cast<double>(inlining_cost::scale_inline_threshold_by)));
}

inlining_cost::Threshold initial_inlining_toplevel_threshold(long round) {
  double ordinary_threshold = arg_helper::get(round, clflags::inline_threshold);
  long toplevel_threshold = arg_helper::get(round, clflags::inline_toplevel_threshold);
  long unscaled = int_of_float(ordinary_threshold) + toplevel_threshold;
  // CR-soon pchambart: Add a warning if this is too big
  return inlining_cost::Threshold::can_inline_if_no_larger_than(unscaled * inlining_cost::scale_inline_threshold_by);
}

// ---- Result ---------------------------------------------------------------------
Result Result::set_approx(A::t a) const {
  Result r = *this;
  r.approx = a;
  return r;
}
Result Result::meet_approx(const Env&, A::t a) const {
  return set_approx(A::meet(Env::really_import_approx, approx, a));
}
Result Result::use_static_exception(static_exception::t i) const {
  Result r = *this;
  r.used_static_exceptions = used_static_exceptions.add(i);
  return r;
}
Result Result::exit_scope_catch(static_exception::t i) const {
  Result r = *this;
  r.used_static_exceptions = used_static_exceptions.remove(i);
  return r;
}
Result Result::map_benefit(FnRef<inlining_cost::Benefit(inlining_cost::Benefit)> f) const {
  Result r = *this;
  r.benefit = f(benefit);
  return r;
}
Result Result::add_benefit(const inlining_cost::Benefit& b) const {
  Result r = *this;
  r.benefit = inlining_cost::benefit::plus(benefit, b);
  return r;
}
Result Result::reset_benefit() const {
  Result r = *this;
  r.benefit = inlining_cost::benefit::zero();
  return r;
}
Result Result::set_inlining_threshold(std::optional<inlining_cost::Threshold> t) const {
  Result r = *this;
  r.inlining_threshold = t;
  return r;
}
Result Result::add_inlining_threshold(inlining_cost::Threshold j) const {
  if (!inlining_threshold) return *this;
  Result r = *this;
  r.inlining_threshold = inlining_cost::threshold::add(*inlining_threshold, j);
  return r;
}
Result Result::sub_inlining_threshold(inlining_cost::Threshold j) const {
  if (!inlining_threshold) return *this;
  Result r = *this;
  r.inlining_threshold = inlining_cost::threshold::sub(*inlining_threshold, j);
  return r;
}
Result Result::seen_direct_application() const {
  Result r = *this;
  r.num_direct_applications = num_direct_applications + 1;
  return r;
}

std::function<bool(variable::t, const F::FunctionDeclaration*)> keep_body_check(bool is_classic_mode,
                                                                              A::Lazy<variable::Set> recursive) {
  if (!is_classic_mode) return [](variable::t, const F::FunctionDeclaration*) { return true; };
  return [recursive](variable::t var, const F::FunctionDeclaration* fun_decl) {
    // In classic-inlining mode, the inlining decision is taken at
    // definition site (here).  If the function is small enough (below the
    // -inline threshold) it will always be inlined.  (inline_and_simplify_aux.ml)
    if (fun_decl->stub) return true;
    if (recursive.force().mem(var)) return false;
    using IK = lambda::InlineAttribute::Kind;
    switch (fun_decl->inline_.kind) {
      case IK::Default_inline: {
        inlining_cost::Threshold inlining_threshold = initial_inlining_threshold(0);
        long bonus = flambda_utils::function_arity(fun_decl);
        return inlining_cost::can_inline(fun_decl->body, inlining_threshold, bonus);
      }
      case IK::Unroll: return fun_decl->inline_.unroll > 0;
      case IK::Always_inline: case IK::Hint_inline: return true;
      case IK::Never_inline: return false;
    }
    return false;
  };
}

PreparedSetOfClosures prepare_to_simplify_set_of_closures(const Env& env0, const F::SetOfClosures* set_of_closures,
                                                          const F::FunctionDeclarations* function_decls0, bool freshen,
                                                          const F::FunctionDeclaration* only_for_function_decl) {
  Env env = env0;
  variable::Map<freshening::SpecApprox> free_vars0 =
      set_of_closures->free_vars.map([&](const F::SpecialisedTo& external_var) {
        variable::t var = freshening::apply_variable(env.freshening(), external_var.var);
        if (variable::t v = A::simplify_var_to_var_using_env(env.find_exn(var), [&](variable::t x) { return env.mem(x); }))
          var = v;
        A::t a = env.find_exn(var);
        // The projections are freshened below in one step, once we know the
        // closure freshening substitution.
        return freshening::SpecApprox{F::SpecialisedTo{var, external_var.projection}, a};
      });
  variable::Map<F::SpecialisedTo> specialised_args0 = set_of_closures->specialised_args.filter_map(
      [&](variable::t param, const F::SpecialisedTo& spec_to) -> std::optional<F::SpecialisedTo> {
        bool keep = !only_for_function_decl || parameter::set_vars(only_for_function_decl->params).mem(param);
        if (!keep) return std::nullopt;
        variable::t var = freshening::apply_variable(env.freshening(), spec_to.var);
        if (variable::t v = A::simplify_var_to_var_using_env(env.find_exn(var), [&](variable::t x) { return env.mem(x); }))
          var = v;
        return F::SpecialisedTo{var, spec_to.projection};
      });
  const Env environment_before_cleaning = env;
  // [E.local] helps us to catch bugs whereby variables escape their scope.
  env = env.local();
  freshening::AppliedFunctionDecls applied =
      freshening::apply_function_decls_and_free_vars(env.freshening(), free_vars0, function_decls0, !freshen);
  const F::FunctionDeclarations* function_decls = applied.func_decls;
  env = env.set_freshening(applied.t);
  const freshening::project_var::T& fr = applied.of_closures;
  variable::Map<freshening::SpecApprox> free_vars =
      freshening::freshen_projection_relation_prime(applied.fv, env.freshening(), fr);
  // Variable.Map.map_keys (Freshening.apply_variable ...)
  variable::Map<F::SpecialisedTo> specialised_args = freshening::freshen_projection_relation(
      specialised_args0.map_keys([&](variable::t v) { return freshening::apply_variable(env.freshening(), v); }),
      env.freshening(), fr);
  // Approximations of parameters that are known to always hold the same
  // argument throughout the body of the function.
  variable::Map<A::t> parameter_approximations =
      specialised_args
          .map([&](const F::SpecialisedTo& spec_to) { return environment_before_cleaning.find_exn(spec_to.var); })
          .map_keys([&](variable::t v) { return freshening::apply_variable(env.freshening(), v); });
  variable::Map<variable::t> direct_call_surrogates = set_of_closures->direct_call_surrogates.fold(
      [&](variable::t existing0, variable::t surrogate0, variable::Map<variable::t> surrogates) {
        variable::t existing = freshening::project_var::apply_closure_id(fr, existing0);
        variable::t surrogate = freshening::project_var::apply_closure_id(fr, surrogate0);
        if (surrogates.mem(existing)) misc::fatal_error("Inline_and_simplify_aux: direct_call_surrogates");
        return surrogates.add(existing, surrogate);
      },
      variable::Map<variable::t>{});
  env = env.enter_set_of_closures_declaration(function_decls->set_of_closures_origin);
  // we use the previous closure for evaluating the functions
  variable::Map<A::t> bound_vars = free_vars.fold(
      [](variable::t id, const freshening::SpecApprox& sa, variable::Map<A::t> map) { return map.add(id, sa.approx); },
      variable::Map<A::t>{});
  variable::Map<F::SpecialisedTo> fv_spec = free_vars.map([](const freshening::SpecApprox& sa) { return sa.spec; });
  auto invariant_params = A::Lazy<variable::Map<variable::Set>>::from_val({});
  auto recursive = A::Lazy<variable::Set>::of_fun([function_decls] { return function_decls->funs.keys(); });
  bool is_classic_mode = function_decls->is_classic_mode;
  auto keep_body = keep_body_check(is_classic_mode, recursive);
  const A::FunctionDeclarations* approx_decls = A::function_declarations_approx(keep_body, function_decls);
  const A::ValueSetOfClosures* internal_value_set_of_closures = A::create_value_set_of_closures(
      approx_decls, bound_vars, fv_spec, invariant_params, recursive, specialised_args, fr, direct_call_surrogates);
  // Populate the environment with the approximation of each closure.  This
  // part of the environment is shared between all of the closures in the
  // set of closures.
  Env set_of_closures_env = function_decls->funs.fold(
      [&](variable::t closure, const F::FunctionDeclaration*, Env e) {
        A::t a = A::value_closure(internal_value_set_of_closures, closure, closure);
        return e.add(closure, a);
      },
      env);
  return {free_vars, specialised_args, function_decls, parameter_approximations, internal_value_set_of_closures,
          set_of_closures_env};
}

namespace {
// This adds only the minimal set of approximations to the closures.  It is
// not strictly necessary to have this restriction, but it helps to catch
// potential substitution bugs.
Env populate_closure_approximations(const F::FunctionDeclaration* function_decl,
                                    const variable::Map<freshening::SpecApprox>& free_vars,
                                    const variable::Map<A::t>& parameter_approximations, const Env& set_of_closures_env) {
  // Add approximations of free variables
  Env env = free_vars.fold(
      [](variable::t id, const freshening::SpecApprox& sa, Env e) { return e.add_outer_scope(id, sa.approx); },
      set_of_closures_env);
  // Add known approximations of function parameters
  for (const Parameter& p : function_decl->params) {
    const A::t* a = parameter_approximations.find_opt(p.var);
    env = env.add(p.var, a ? *a : A::value_unknown(A::other()));
  }
  return env;
}
}  // namespace

Env prepare_to_simplify_closure(const F::FunctionDeclaration* function_decl,
                                const variable::Map<freshening::SpecApprox>& free_vars,
                                const variable::Map<F::SpecialisedTo>& specialised_args,
                                const variable::Map<A::t>& parameter_approximations, const Env& set_of_closures_env) {
  Env closure_env =
      populate_closure_approximations(function_decl, free_vars, parameter_approximations, set_of_closures_env);
  // Add definitions of known projections to the environment.
  auto add_projection = [&](variable::t inner_var, const F::SpecialisedTo& spec_arg, Env env) {
    if (!spec_arg.projection) return env;
    variable::t from = projection::projecting_from(spec_arg.projection);
    if (function_decl->free_variables.mem(from)) return env.add_projection(spec_arg.projection, inner_var);
    return env;
  };
  closure_env = specialised_args.fold(
      [&](variable::t inner_var, const F::SpecialisedTo& spec_arg, Env env) {
        return add_projection(inner_var, spec_arg, env);
      },
      closure_env);
  return free_vars.fold(
      [&](variable::t inner_var, const freshening::SpecApprox& sa, Env env) {
        return add_projection(inner_var, sa.spec, env);
      },
      closure_env);
}

}  // namespace cppcaml::typing::inline_and_simplify_aux
