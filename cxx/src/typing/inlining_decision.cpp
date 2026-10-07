// Port of middle_end/flambda/inlining_decision.ml (see
// inlining_decision.hpp).
#include "cppcaml/typing/inlining_decision.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/inlining_transforms.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inlining_decision {

using namespace flambda;
namespace S = inlining_stats;
namespace W = inlining_cost::whether_sufficient_benefit;
namespace T = inlining_cost::threshold;
using inlining_cost::Threshold;
using format::Formatter;
using format::fprintf;

namespace {
const A::FunctionBody* get_function_body(const A::FunctionDeclaration* function_decl) {
  if (!function_decl->function_body) misc::fatal_error("Inlining_decision.get_function_body");  // (assert false)
  return function_decl->function_body;
}

// ('a, 'b) inlining_result = Changed of (Flambda.t * R.t) * 'a | Original of 'b
template <class Ch, class Or>
struct InliningResult {
  bool changed;
  std::pair<t, Result> res;
  Ch changed_reason{};
  Or original_reason{};
};

template <class F>
struct LazyVal {
  F f;
  using V = decltype(std::declval<F&>()());
  std::optional<V> v;
  const V& force() {
    if (!v) v = f();
    return *v;
  }
};
template <class F>
LazyVal<F> lazy(F f) {
  return LazyVal<F>{std::move(f), std::nullopt};
}

using Inlined = S::Inlined;
using NotInlined = S::NotInlined;
using Specialised = S::Specialised;
using NotSpecialised = S::NotSpecialised;

template <class FunCost, class Recursive>
InliningResult<Inlined, NotInlined> inline_(
    Env env, const Result& r, variable::t lhs_of_application, variable::t closure_id_being_applied,
    const A::FunctionDeclaration* function_decl, const A::FunctionBody* function_body,
    const A::ValueSetOfClosures* value_set_of_closures, bool only_use_of_function, t original, Recursive& recursive,
    Slice<variable::t> args, std::optional<long> size_from_approximation, const debuginfo::t& dbg,
    const Simplify& simplify, lambda::InlineAttribute inline_requested,
    lambda::SpecialiseAttribute specialise_requested, const variable::Set& fun_vars,
    set_of_closures_id::t set_of_closures_origin, bool self_call, FunCost& fun_cost, Threshold inlining_threshold) {
  using IK = lambda::InlineAttribute::Kind;
  bool toplevel = env.at_toplevel();
  long branch_depth = env.branch_depth();
  bool unrolling = false, always_inline = false, never_inline = false;
  if (std::optional<long> count = env.actively_unrolling(set_of_closures_origin)) {
    if (*count > 0) {
      env = env.continue_actively_unrolling(set_of_closures_origin);
      unrolling = true;
      always_inline = true;
    } else {
      never_inline = true;
    }
  } else {
    // Merge call site annotation and function annotation.  The call site
    // annotation takes precedence
    lambda::InlineAttribute inline_annotation =
        inline_requested.kind == IK::Default_inline ? function_body->inline_ : inline_requested;
    switch (inline_annotation.kind) {
      case IK::Always_inline: case IK::Hint_inline: always_inline = true; break;
      case IK::Never_inline: never_inline = true; break;
      case IK::Default_inline: break;
      case IK::Unroll:
        if (inline_annotation.unroll > 0) {
          env = env.start_actively_unrolling(set_of_closures_origin, inline_annotation.unroll - 1);
          unrolling = true;
          always_inline = true;
        } else {
          never_inline = true;
        }
        break;
    }
  }
  Threshold remaining_inlining_threshold = always_inline ? inlining_threshold : fun_cost.force();
  // try_inlining: Try_it (nullopt) | Don't_try_it of decision
  std::optional<NotInlined> dont;
  auto ni = [](NotInlined::Kind k) { return NotInlined{k}; };
  if (unrolling) {
  } else if (self_call) {
    dont = ni(NotInlined::Kind::Self_call);
  } else if (!env.inlining_allowed(function_decl->closure_origin)) {
    dont = ni(NotInlined::Kind::Unrolling_depth_exceeded);
  } else if (only_use_of_function || always_inline) {
  } else if (never_inline) {
    dont = ni(NotInlined::Kind::Annotation);
  } else if (!env.unrolling_allowed(set_of_closures_origin) && recursive.force()) {
    dont = ni(NotInlined::Kind::Unrolling_depth_exceeded);
  } else if (T::equal(remaining_inlining_threshold, Threshold::never())) {
    if (inlining_threshold.never_inline) misc::fatal_error("Inlining_decision.inline");  // (assert false)
    NotInlined d = ni(NotInlined::Kind::Above_threshold);
    d.size = inlining_threshold.size;
    dont = d;
  } else if (!(toplevel && branch_depth == 0) && A::all_not_useful(slice(env.find_list_exn(args)))) {
    // When all of the arguments to the function being inlined are unknown,
    // then we cannot materially simplify the function.  (inlining_decision.ml)
    if (size_from_approximation) {
      long body_size = *size_from_approximation;
      inlining_cost::Benefit benefit = inlining_cost::benefit::remove_call(inlining_cost::benefit::zero());
      function_body->free_variables.iter([&](variable::t v) {
        const A::t* a = value_set_of_closures->bound_vars.find_opt(v);
        if (a && (*a)->var && env.mem((*a)->var)) benefit = inlining_cost::benefit::remove_prim(benefit);
      });
      inlining_cost::WhetherSufficientBenefit wsb =
          W::create_estimate(inlining_cost::direct_call_size, env.at_toplevel(), env.branch_depth(), body_size, benefit,
                             function_body->is_a_functor, env.round());
      if (!W::evaluate(wsb)) {
        NotInlined d = ni(NotInlined::Kind::Without_subfunctions);
        d.wsb1 = wsb;
        dont = d;
      }
    } else {
      // The function is definitely too large to inline given that we don't
      // have any approximations for its arguments.  (inlining_decision.ml)
      dont = ni(NotInlined::Kind::No_useful_approximations);
    }
  } else {
    // There are useful approximations, so we should simplify.
  }
  if (dont) return {false, {}, {}, *dont};
  Result r1 = r.set_inlining_threshold(remaining_inlining_threshold);
  // First we construct the code that would result from copying the body of
  // the function, without doing any further inlining upon it, to the call
  // site.
  auto [body, r_inlined] = inlining_transforms::inline_by_copying_function_body(
      env, r1.reset_benefit(), lhs_of_application, inline_requested, specialise_requested, closure_id_being_applied,
      function_decl, function_body, fun_vars, args, dbg, simplify);
  long num_direct_applications_seen = r_inlined.num_direct_applications - r1.num_direct_applications;
  if (num_direct_applications_seen < 0) misc::fatal_error("Inlining_decision.inline");  // (assert)
  auto keep_inlined_version = [&](Inlined decision) -> InliningResult<Inlined, NotInlined> {
    // Inlining the body of the function was sufficiently beneficial that we
    // will keep it, replacing the call site.  (inlining_decision.ml)
    Result ri = r_inlined;
    if (always_inline)
      ri = ri.map_benefit([&](inlining_cost::Benefit b) {
        return inlining_cost::benefit::max(
            env.round(), inlining_cost::benefit::requested_inline(inlining_cost::benefit::zero(), body), b);
      });
    Result rr = ri.map_benefit([&](inlining_cost::Benefit b) { return inlining_cost::benefit::plus(r1.benefit, b); });
    Env e = env.note_entering_inlined();
    // We decrement the unrolling count even if the function is not
    // recursive to avoid having to check whether or not it is recursive
    e = e.inside_unrolled_function(set_of_closures_origin);
    e = e.inside_inlined_function(function_decl->closure_origin);
    if (e.inlining_level() != 0) e = e.inlining_level_up();
    return {true, simplify(e, rr, body), decision, {}};
  };
  if (always_inline) return keep_inlined_version(Inlined{Inlined::Kind::Annotation});
  if (only_use_of_function) return keep_inlined_version(Inlined{Inlined::Kind::Decl_local_to_application});
  inlining_cost::WhetherSufficientBenefit wsb = W::create(original, env.at_toplevel(), env.branch_depth(), body,
                                                          r_inlined.benefit, function_body->is_a_functor, env.round());
  if (W::evaluate(wsb)) {
    Inlined d{Inlined::Kind::Without_subfunctions};
    d.wsb1 = wsb;
    return keep_inlined_version(d);
  }
  if (num_direct_applications_seen < 1) {
    // Inlining the body of the function did not appear sufficiently
    // beneficial; however, it may become so if we inline within the body
    // first.  (inlining_decision.ml)
    NotInlined d = ni(NotInlined::Kind::Without_subfunctions);
    d.wsb1 = wsb;
    return {false, {}, {}, d};
  }
  Env e = env.inlining_level_up();
  e = e.note_entering_inlined();
  // We decrement the unrolling count even if the function is recursive to
  // avoid having to check whether or not it is recursive
  e = e.inside_unrolled_function(set_of_closures_origin);
  auto [body2, r_inlined2] = simplify(e, r_inlined, body);
  inlining_cost::WhetherSufficientBenefit wsb_with_subfunctions = W::create(
      original, e.at_toplevel(), e.branch_depth(), body2, r_inlined2.benefit, function_body->is_a_functor, e.round());
  if (W::evaluate(wsb_with_subfunctions)) {
    Result rr =
        r_inlined2.map_benefit([&](inlining_cost::Benefit b) { return inlining_cost::benefit::plus(r1.benefit, b); });
    Inlined d{Inlined::Kind::With_subfunctions};
    d.wsb1 = wsb;
    d.wsb2 = wsb_with_subfunctions;
    return {true, {body2, rr}, d, {}};
  }
  // r_inlined contains an approximation that may be invalid for the
  // untransformed expression.  (inlining_decision.ml)
  NotInlined d = ni(NotInlined::Kind::With_subfunctions);
  d.wsb1 = wsb;
  d.wsb2 = wsb_with_subfunctions;
  return {false, {}, {}, d};
}

template <class FunCost, class Recursive>
InliningResult<Specialised, NotSpecialised> specialise(
    const Env& env, const Result& r, variable::t lhs_of_application, const A::FunctionDeclarations* function_decls,
    const A::FunctionDeclaration* function_decl, variable::t closure_id_being_applied,
    const A::ValueSetOfClosures* value_set_of_closures, Slice<variable::t> args, const std::vector<A::t>& args_approxs,
    const debuginfo::t& dbg, const Simplify& simplify, t original, Recursive& recursive, bool self_call,
    Threshold inlining_threshold, FunCost& fun_cost, lambda::InlineAttribute inline_requested,
    lambda::SpecialiseAttribute specialise_requested) {
  using SA = lambda::SpecialiseAttribute;
  const A::Lazy<variable::Map<variable::Set>>& invariant_params = value_set_of_closures->invariant_params;
  const variable::Map<SpecialisedTo>& free_vars = value_set_of_closures->free_vars;
  auto has_no_useful_approxes = lazy([&]() {
    if (function_decl->params.size() != args_approxs.size()) misc::fatal_error("Invalid_argument(\"List.for_all2\")");
    for (std::size_t k = 0; k < args_approxs.size(); ++k)
      if (A::useful(args_approxs[k]) && invariant_params.force().mem(function_decl->params[k].var)) return false;
    return true;
  });
  // Merge call site annotation and function annotation.  The call site
  // annotation takes precedence
  bool always_specialise = false, never_specialise = false;
  switch (specialise_requested) {
    case SA::Always_specialise: always_specialise = true; break;
    case SA::Never_specialise: never_specialise = true; break;
    case SA::Default_specialise:
      if (!function_decl->function_body) never_specialise = true;
      else if (function_decl->function_body->specialise == SA::Always_specialise) always_specialise = true;
      else if (function_decl->function_body->specialise == SA::Never_specialise) never_specialise = true;
      break;
  }
  Threshold remaining_inlining_threshold = always_specialise ? inlining_threshold : fun_cost.force();
  // Try specialising if the function: is recursive; and is closed (it and
  // all other members of the set of closures on which it depends); and has
  // useful approximations for some invariant parameters.
  std::optional<NotSpecialised> dont;
  auto ns = [](NotSpecialised::Kind k) { return NotSpecialised{k}; };
  if (function_decls->is_classic_mode) dont = ns(NotSpecialised::Kind::Classic_mode);
  else if (self_call) dont = ns(NotSpecialised::Kind::Self_call);
  else if (always_specialise && !has_no_useful_approxes.force()) {
  } else if (never_specialise) dont = ns(NotSpecialised::Kind::Annotation);
  else if (T::equal(remaining_inlining_threshold, Threshold::never())) {
    if (inlining_threshold.never_inline) misc::fatal_error("Inlining_decision.specialise");  // (assert false)
    NotSpecialised d = ns(NotSpecialised::Kind::Above_threshold);
    d.size = inlining_threshold.size;
    dont = d;
  } else if (!free_vars.is_empty()) dont = ns(NotSpecialised::Kind::Not_closed);
  else if (!recursive.force()) dont = ns(NotSpecialised::Kind::Not_recursive);
  else if (invariant_params.force().is_empty()) dont = ns(NotSpecialised::Kind::No_invariant_parameters);
  else if (has_no_useful_approxes.force()) dont = ns(NotSpecialised::Kind::No_useful_approximations);
  if (dont) return {false, {}, {}, *dont};
  Result r1 = r.set_inlining_threshold(remaining_inlining_threshold);
  std::optional<std::pair<t, Result>> copied = inlining_transforms::inline_by_copying_function_declaration(
      env, r1.reset_benefit(), function_decls, lhs_of_application, inline_requested, closure_id_being_applied,
      function_decl, args, args_approxs, invariant_params, value_set_of_closures->specialised_args,
      value_set_of_closures->free_vars, value_set_of_closures->direct_call_surrogates, dbg, simplify);
  if (!copied) return {false, {}, {}, ns(NotSpecialised::Kind::No_useful_approximations)};
  auto [expr, r_inlined] = *copied;
  inlining_cost::WhetherSufficientBenefit wsb =
      W::create(original, false, env.branch_depth(), expr, r_inlined.benefit, false, env.round());
  // CR-someday lwhite: could avoid calculating this if stats is turned off
  Env env2 = env.note_entering_specialised(function_decls->funs.keys());
  if (always_specialise || W::evaluate(wsb)) {
    Result ri = r_inlined;
    if (always_specialise)
      ri = ri.map_benefit([&](inlining_cost::Benefit b) {
        return inlining_cost::benefit::max(
            env2.round(), inlining_cost::benefit::requested_inline(inlining_cost::benefit::zero(), expr), b);
      });
    Result rr = ri.map_benefit([&](inlining_cost::Benefit b) { return inlining_cost::benefit::plus(r1.benefit, b); });
    // If the function was considered for specialising without considering
    // its sub-functions, and it is not below another inlining choice, then
    // we are certain that this code will be kept.
    Env closure_env = (env2.inlining_level() == 0 ? env2 : env2.inlining_level_up()).set_never_inline_outside_closures();
    Env application_env = env2.set_never_inline_inside_closures();
    auto [expr2, r2] = simplify(closure_env, rr, expr);
    std::pair<t, Result> res = simplify(application_env, r2, expr2);
    Specialised d{always_specialise ? Specialised::Kind::Annotation : Specialised::Kind::Without_subfunctions};
    if (!always_specialise) d.wsb1 = wsb;
    return {true, res, d, {}};
  }
  Env closure_env = env2.inlining_level_up().set_never_inline_outside_closures();
  auto [expr2, r_inlined2] = simplify(closure_env, r_inlined, expr);
  inlining_cost::WhetherSufficientBenefit wsb_with_subfunctions =
      W::create(original, false, env2.branch_depth(), expr2, r_inlined2.benefit, false, env2.round());
  if (W::evaluate(wsb_with_subfunctions)) {
    Result rr =
        r_inlined2.map_benefit([&](inlining_cost::Benefit b) { return inlining_cost::benefit::plus(r1.benefit, b); });
    Env application_env = env2.set_never_inline_inside_closures();
    std::pair<t, Result> res = simplify(application_env, rr, expr2);
    Specialised d{Specialised::Kind::With_subfunctions};
    d.wsb1 = wsb;
    d.wsb2 = wsb_with_subfunctions;
    return {true, res, d, {}};
  }
  NotSpecialised d = ns(NotSpecialised::Kind::Not_beneficial);
  d.wsb1 = wsb;
  d.wsb2 = wsb_with_subfunctions;
  return {false, {}, {}, d};
}
}  // namespace

std::pair<t, Result> for_call_site(const Env& env0, const Result& r, const A::FunctionDeclarations* function_decls,
                                   variable::t lhs_of_application, variable::t closure_id_being_applied,
                                   const A::FunctionDeclaration* function_decl,
                                   const A::ValueSetOfClosures* value_set_of_closures, Slice<variable::t> args,
                                   const std::vector<A::t>& args_approxs, const debuginfo::t& dbg,
                                   const Simplify& simplify, lambda::InlineAttribute inline_requested,
                                   lambda::SpecialiseAttribute specialise_requested) {
  using IK = lambda::InlineAttribute::Kind;
  if (args.size() != args_approxs.size())
    misc::fatal_error("Inlining_decision.for_call_site: inconsistent lengths of [args] and [args_approxs]");
  // Remove unroll attributes from functions we are already actively
  // unrolling, otherwise they'll be unrolled again next round.
  if (inline_requested.kind == IK::Unroll && env0.actively_unrolling(function_decls->set_of_closures_origin))
    inline_requested = lambda::InlineAttribute{};
  t original = apply(lhs_of_application, args, CallKind{closure_id_being_applied}, dbg, inline_requested,
                     specialise_requested);
  Result original_r = r.seen_direct_application().set_approx(A::value_unknown(A::other()));
  if (!function_decl->function_body) return {original, original_r};
  if (function_decl->function_body->stub) {
    const Env& env = env0;
    variable::Set fun_vars = function_decls->funs.keys();
    const A::FunctionBody* function_body = get_function_body(function_decl);
    auto [body, r2] = inlining_transforms::inline_by_copying_function_body(
        env, r, lhs_of_application, inline_requested, specialise_requested, closure_id_being_applied, function_decl,
        function_body, fun_vars, args, dbg, simplify);
    return simplify(env, r2, body);
  }
  if (env0.never_inline())
    // This case only occurs when examining the body of a stub function but
    // not in the context of inlining said function.  (inlining_decision.ml)
    return {original, original_r};
  if (function_decls->is_classic_mode) {
    Env env = env0.note_entering_call(closure_id_being_applied, dbg);
    InliningResult<Inlined, NotInlined> simpl;
    const A::FunctionBody* function_body = function_decl->function_body;
    bool self_call = env.inside_set_of_closures_declaration(function_decls->set_of_closures_origin);
    if (self_call) simpl = {false, {}, {}, NotInlined{NotInlined::Kind::Self_call}};
    else if (!env.inlining_allowed(function_decl->closure_origin))
      simpl = {false, {}, {}, NotInlined{NotInlined::Kind::Unrolling_depth_exceeded}};
    else {
      variable::Set fun_vars = function_decls->funs.keys();
      auto [body, r2] = inlining_transforms::inline_by_copying_function_body(
          env, r, lhs_of_application, inline_requested, specialise_requested, closure_id_being_applied, function_decl,
          function_body, fun_vars, args, dbg, simplify);
      Env e = env.note_entering_inlined();
      // We decrement the unrolling count even if the function is not
      // recursive to avoid having to check whether or not it is recursive
      e = e.inside_unrolled_function(function_decls->set_of_closures_origin);
      e = e.inside_inlined_function(function_decl->closure_origin);
      simpl = {true, simplify(e, r2, body), Inlined{Inlined::Kind::Classic_mode}, {}};
    }
    std::pair<t, Result> res;
    S::Decision decision{};
    if (!simpl.changed) {
      res = {original, original_r};
      decision.kind = S::Decision::Kind::Unchanged;
      decision.not_specialised = NotSpecialised{NotSpecialised::Kind::Classic_mode};
      decision.not_inlined = simpl.original_reason;
    } else {
      auto [expr, r2] = simpl.res;
      Threshold max_inlining_threshold = env.at_toplevel()
                                             ? inline_and_simplify_aux::initial_inlining_toplevel_threshold(env.round())
                                             : inline_and_simplify_aux::initial_inlining_threshold(env.round());
      std::optional<Threshold> raw_inlining_threshold = r2.inlining_threshold;
      Threshold unthrottled = raw_inlining_threshold ? *raw_inlining_threshold : max_inlining_threshold;
      Threshold inlining_threshold = T::min(unthrottled, max_inlining_threshold);
      Threshold inlining_threshold_diff = T::sub(unthrottled, inlining_threshold);
      if (env.inlining_level() == 0) res = {expr, r2.set_inlining_threshold(raw_inlining_threshold)};
      else res = {expr, r2.add_inlining_threshold(inlining_threshold_diff)};
      decision.kind = S::Decision::Kind::Inlined;
      decision.not_specialised = NotSpecialised{NotSpecialised::Kind::Classic_mode};
      decision.inlined = simpl.changed_reason;
    }
    env.record_decision(decision);
    return res;
  }
  const A::FunctionBody* function_body = get_function_body(function_decl);
  Env env = env0.unset_never_inline_inside_closures();
  env = env.note_entering_call(closure_id_being_applied, dbg);
  long max_level = arg_helper::get(env.round(), clflags::inline_max_depth);
  std::optional<Threshold> raw_inlining_threshold = r.inlining_threshold;
  Threshold max_inlining_threshold = env.at_toplevel()
                                         ? inline_and_simplify_aux::initial_inlining_toplevel_threshold(env.round())
                                         : inline_and_simplify_aux::initial_inlining_threshold(env.round());
  Threshold unthrottled = raw_inlining_threshold ? *raw_inlining_threshold : max_inlining_threshold;
  Threshold inlining_threshold = T::min(unthrottled, max_inlining_threshold);
  Threshold inlining_threshold_diff = T::sub(unthrottled, inlining_threshold);
  bool inlining_prevented = inlining_threshold.never_inline;
  // simpl: Changed (res, decision) | Original decision
  bool changed = false;
  std::pair<t, Result> changed_res;
  S::Decision decision{};
  if (inlining_prevented) {
    decision.kind = S::Decision::Kind::Prevented;
    decision.prevented = S::Prevented::Function_prevented_from_inlining;
  } else if (env.inlining_level() >= max_level) {
    decision.kind = S::Decision::Kind::Prevented;
    decision.prevented = S::Prevented::Level_exceeded;
  } else {
    bool self_call = env.inside_set_of_closures_declaration(function_decls->set_of_closures_origin);
    auto fun_cost = lazy([&]() {
      // CR-someday mshinwell: for the moment, this is None, since the
      // Inlining_cost code isn't checking sizes up to the max inlining
      // threshold---this seems to take too long.
      return inlining_cost::can_try_inlining(function_body->body, inlining_threshold,
                                             static_cast<long>(function_decl->params.size()), std::nullopt);
    });
    auto recursive = lazy([&]() {
      variable::t fun_var = closure_id_being_applied;
      return value_set_of_closures->recursive.force().mem(fun_var);
    });
    InliningResult<Specialised, NotSpecialised> specialise_result =
        specialise(env, r, lhs_of_application, function_decls, function_decl, closure_id_being_applied,
                   value_set_of_closures, args, args_approxs, dbg, simplify, original, recursive, self_call,
                   inlining_threshold, fun_cost, inline_requested, specialise_requested);
    if (specialise_result.changed) {
      changed = true;
      changed_res = specialise_result.res;
      decision.kind = S::Decision::Kind::Specialised;
      decision.specialised = specialise_result.changed_reason;
    } else {
      bool only_use_of_function = false;
      // If we didn't specialise then try inlining
      variable::t fun_var = closure_id_being_applied;
      const std::optional<long>* size = value_set_of_closures->size.force().find_opt(fun_var);
      if (!size) {
        Formatter f;
        fprintf(f,
                "Approximation does not give a size for the function having fun_var %a.  value_set_of_closures: %a",
                pr(variable::print, fun_var), pr(A::print_value_set_of_closures, value_set_of_closures));
        misc::fatal_error(f.contents());
      }
      std::optional<long> size_from_approximation = *size;
      variable::Set fun_vars = function_decls->funs.keys();
      set_of_closures_id::t set_of_closures_origin = function_decls->set_of_closures_origin;
      InliningResult<Inlined, NotInlined> inline_result =
          inline_(env, r, lhs_of_application, closure_id_being_applied, function_decl, function_body,
                  value_set_of_closures, only_use_of_function, original, recursive, args, size_from_approximation, dbg,
                  simplify, inline_requested, specialise_requested, fun_vars, set_of_closures_origin, self_call,
                  fun_cost, inlining_threshold);
      decision.not_specialised = specialise_result.original_reason;
      if (inline_result.changed) {
        changed = true;
        changed_res = inline_result.res;
        decision.kind = S::Decision::Kind::Inlined;
        decision.inlined = inline_result.changed_reason;
      } else {
        decision.kind = S::Decision::Kind::Unchanged;
        decision.not_inlined = inline_result.original_reason;
      }
    }
  }
  std::pair<t, Result> res;
  if (!changed) res = {original, original_r};
  else {
    auto [expr, r2] = changed_res;
    if (env.inlining_level() == 0) res = {expr, r2.set_inlining_threshold(raw_inlining_threshold)};
    else res = {expr, r2.add_inlining_threshold(inlining_threshold_diff)};
  }
  env.record_decision(decision);
  return res;
}

}  // namespace cppcaml::typing::inlining_decision
