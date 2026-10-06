// Port of middle_end/flambda/inline_and_simplify.ml (see
// inline_and_simplify.hpp).
//
// Values of two types hold the information propagated during
// simplification: [E.t] "environments", top-down, almost always called
// "env"; [R.t] "results", bottom-up approximately following the evaluation
// order, almost always called "r".  (inline_and_simplify.ml)
#include "cppcaml/typing/inline_and_simplify.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/effect_analysis.hpp"
#include "cppcaml/typing/flambda_evacuate.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/import_approx.hpp"
#include "cppcaml/typing/inline_and_simplify_aux.hpp"
#include "cppcaml/typing/inlining_decision.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/invariant_params.hpp"
#include "cppcaml/typing/lift_code.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/remove_free_vars_equal_to_args.hpp"
#include "cppcaml/typing/simplify_primitives.hpp"
#include "cppcaml/typing/unbox_closures.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::inline_and_simplify {

using namespace flambda;
namespace A = simple_value_approx;
namespace B = inlining_cost::benefit;
using E = inline_and_simplify_aux::Env;
using R = inline_and_simplify_aux::Result;
namespace Names = internal_variable_names;
using format::Formatter;
using format::fprintf;

namespace {

constexpr long size_int = 8;  // Backend.size_int (amd64)

[[noreturn]] void fatal(const std::function<void(Formatter&)>& msg) {
  Formatter f;
  msg(f);
  misc::fatal_error(f.contents());
}

R ret(const R& r, A::t a) { return r.set_approx(a); }

using ExprResult = std::pair<t, R>;
using NamedResult = std::pair<named, R>;

// simplify_variable_result = No_binding of Variable.t | Binding of
// Variable.t * (Flambda.named Flambda.With_free_variables.t)
struct FvInternal {
  bool binding;
  variable::t var;
  WithFvNamed named_;
  A::t approx;
};

FvInternal simplify_free_variable_internal(const E& env, variable::t original_var0) {
  variable::t var = freshening::apply_variable(env.freshening(), original_var0);
  variable::t original_var = var;
  // In the case where an approximation is useful, we introduce a [let] to
  // bind (e.g.) the constant or symbol replacing [var], unless this would
  // introduce a useless [let] as a consequence of [var] already being in
  // the current scope.  (inline_and_simplify.ml)
  {
    A::t approx = env.find_exn(var);
    if (approx->var && env.mem(approx->var)) var = approx->var;
  }
  // CR-soon mshinwell: Should we update [r] when we *add* code?
  E::ScopedApprox sa = env.find_with_scope_exn(var);
  if (sa.scope == E::Scope::Current) return {false, var, {}, sa.approx};  // avoid useless [let]
  std::optional<std::pair<named, A::t>> sv = A::simplify_var(sa.approx);
  if (!sv) return {false, var, {}, sa.approx};
  return {true, original_var, with_free_variables::of_named(sv->first), sv->second};
}

using ExprK = std::function<ExprResult(const E&, variable::t, A::t)>;
using ExprKs = std::function<ExprResult(const E&, Slice<variable::t>, const std::vector<A::t>&)>;
using NamedK = std::function<NamedResult(const E&, variable::t, A::t)>;
using NamedKs = std::function<NamedResult(const E&, Slice<variable::t>, const std::vector<A::t>&)>;

ExprResult simplify_free_variable(const E& env, variable::t v, const ExprK& f) {
  FvInternal fi = simplify_free_variable_internal(env, v);
  if (!fi.binding) return f(env, fi.var, fi.approx);
  variable::t var = variable::rename(fi.var);
  E env2 = env.add(var, fi.approx);
  auto [body, r] = f(env2, var, fi.approx);
  return {with_free_variables::create_let_reusing_defining_expr(var, fi.named_, body), r};
}

ExprResult simplify_free_variables(const E& env0, Slice<variable::t> vars, const ExprKs& f) {
  std::vector<variable::t> bound_vars;
  std::vector<A::t> approxs;
  std::function<ExprResult(std::size_t, const E&)> collect = [&](std::size_t k, const E& env) -> ExprResult {
    if (k == vars.size()) return f(env, slice(bound_vars), approxs);
    FvInternal fi = simplify_free_variable_internal(env, vars[k]);
    if (!fi.binding) {
      bound_vars.push_back(fi.var);
      approxs.push_back(fi.approx);
      return collect(k + 1, env);
    }
    variable::t var = variable::rename(fi.var);
    E env2 = env.add(var, fi.approx);
    bound_vars.push_back(var);
    approxs.push_back(fi.approx);
    auto [body, r] = collect(k + 1, env2);
    return {with_free_variables::create_let_reusing_defining_expr(var, fi.named_, body), r};
  };
  return collect(0, env0);
}

NamedResult simplify_free_variables_named(const E& env0, Slice<variable::t> vars, const NamedKs& f) {
  std::vector<variable::t> bound_vars;
  std::vector<A::t> approxs;
  // maybe_named = Is_expr of t | Is_named of named
  struct MaybeNamed {
    t expr;
    named n;
  };
  std::function<std::pair<MaybeNamed, R>(std::size_t, const E&)> collect =
      [&](std::size_t k, const E& env) -> std::pair<MaybeNamed, R> {
    if (k == vars.size()) {
      auto [n, r] = f(env, slice(bound_vars), approxs);
      return {MaybeNamed{nullptr, n}, r};
    }
    FvInternal fi = simplify_free_variable_internal(env, vars[k]);
    if (!fi.binding) {
      bound_vars.push_back(fi.var);
      approxs.push_back(fi.approx);
      return collect(k + 1, env);
    }
    variable::t var = variable::rename(fi.var);
    E env2 = env.add(var, fi.approx);
    bound_vars.push_back(var);
    approxs.push_back(fi.approx);
    auto [body, r] = collect(k + 1, env2);
    t b = body.n ? flambda_utils::name_expr(body.n, Names::simplify_fv) : body.expr;
    return {MaybeNamed{with_free_variables::create_let_reusing_defining_expr(var, fi.named_, b), nullptr}, r};
  };
  auto [mn, r] = collect(0, env0);
  if (mn.n) return {mn.n, r};
  return {n_expr(mn.expr), r};
}

// CR-soon mshinwell: tidy this up
NamedResult simplify_free_variable_named(const E& env, variable::t var, const NamedK& f) {
  return simplify_free_variables_named(
      env, slice(std::vector<variable::t>{var}),
      [&](const E& e, Slice<variable::t> vs, const std::vector<A::t>& as) { return f(e, vs[0], as[0]); });
}

NamedResult simplify_named_using_approx(const R& r, named lam, A::t approx) {
  A::SimplificationResultNamed s = A::simplify_named(approx, lam);
  return {s.named, r.set_approx(s.approx)};
}

ExprResult simplify_using_approx_and_env(const E& env, const R& r, t original_lam, A::t approx) {
  A::SimplificationResult s =
      A::simplify_using_env(approx, [&](variable::t v) { return env.mem(v); }, original_lam);
  R r2 = ret(r, s.approx);
  // CR-soon mshinwell: Why is [r] not updated with the cost of adding the
  // new code?
  if (s.summary == A::SimplificationSummary::Replaced_term)
    r2 = r2.map_benefit([&](inlining_cost::Benefit b) { return B::remove_code(original_lam, b); });
  return {s.expr, r2};
}

NamedResult simplify_named_using_approx_and_env(const E& env, const R& r, named original_named, A::t approx) {
  A::SimplificationResultNamed s =
      A::simplify_named_using_env(approx, [&](variable::t v) { return env.mem(v); }, original_named);
  R r2 = ret(r, s.approx);
  if (s.summary == A::SimplificationSummary::Replaced_term)
    r2 = r2.map_benefit([&](inlining_cost::Benefit b) { return B::remove_code_named(original_named, b); });
  return {s.named, r2};
}

A::t simplify_const(const Const& c) { return c.kind == Const::Kind::Int ? A::value_int(c.n) : A::value_char(c.n); }

A::t approx_for_allocated_const(allocated_const::t c) {
  using K = AllocatedConst::Kind;
  switch (c->kind) {
    case K::String: return A::value_string(static_cast<long>(c->s.size()), std::nullopt);
    case K::Immutable_string: return A::value_string(static_cast<long>(c->s.size()), c->s);
    case K::Int32: return A::value_boxed_int(A::BoxedInt::Int32, c->i);
    case K::Int64: return A::value_boxed_int(A::BoxedInt::Int64, c->i);
    case K::Nativeint: return A::value_boxed_int(A::BoxedInt::Nativeint, c->i);
    case K::Float: return A::value_float(c->f);
    case K::Float_array: return A::value_mutable_float_array(static_cast<long>(c->floats.size()));
    case K::Immutable_float_array: {
      std::vector<A::t> fs;  // Array.map: in order
      for (double f : c->floats) fs.push_back(A::value_float(f));
      return A::value_immutable_float_array(slice(fs));
    }
  }
  return A::value_unknown(A::other());
}

// Determine whether a given closure ID corresponds directly to a variable
// (bound to a closure) in the given environment.  (inline_and_simplify.ml)
std::optional<std::pair<named, A::t>> reference_recursive_function_directly(const E& env, variable::t closure_id) {
  A::t approx = env.find_opt(closure_id);
  if (!approx) return std::nullopt;
  return std::pair<named, A::t>{n_expr(var(closure_id)), approx};
}

NamedResult with_projection_var(const E& env, const R& r, variable::t v, projection::t projection) {
  return simplify_free_variable_named(env, v, [&](const E&, variable::t var2, A::t var_approx) {
    R r2 = r.map_benefit([&](inlining_cost::Benefit b) { return B::remove_projection(projection, b); });
    return NamedResult{n_expr(var(var2)), ret(r2, var_approx)};
  });
}

projection::t mk_projection(projection::T p) { return make<projection::T>(p); }

void prerr_inlining_impossible(const debuginfo::t& dbg, const char* msg) {
  location::prerr_warning(debuginfo::to_location(dbg),
                          warnings::Warning::with_s(warnings::Warning::K::Inlining_impossible, msg));
}

// ---- the nursery ------------------------------------------------------------
// The pass allocates in a young zone of its own.  At a few points -- between
// the program's definitions, and between the functions of a set of closures
// bound at toplevel -- what is live is known: the roots the frames on the
// stack register (Nursery::Roots).  There, once the young zone has grown
// enough, the roots are copied into the pass's zone (the old one: what is
// in it never points into the young one) and the young zone is dropped: what
// the OCaml GC reclaims during the pass.  Approximations' lazy values are
// forced as they are copied (their computations create no variables:
// forcing them early changes nothing but when the work is done).
// CPPCAML_FLAMBDA_EVACUATE=always promotes at every point, =never not at
// all; with CPPCAML_ZONE_PROTECT the dropped zones' storage is made
// inaccessible.
class Promoter : public flambda_evacuate::Evacuator {
 public:
  using Evacuator::Evacuator;

  A::t approx(A::t a) {
    return memo(a, [&](const A::Approx& x) {
      A::Approx c = x;
      id(x.var);
      if (x.symbol) id(x.symbol->sym);
      const A::Descr& d = x.descr;
      A::Descr& cd = c.descr;
      switch (d.kind) {
        case A::DK::Value_block: cd.fields = slice(d.fields, [&](A::t f) { return approx(f); }); break;
        case A::DK::Value_set_of_closures: cd.set = value_set_of_closures(d.set); break;
        case A::DK::Value_closure:
          cd.closure.set_of_closures = approx(d.closure.set_of_closures);
          id(d.closure.closure_id);
          break;
        case A::DK::Value_string:
          if (d.str.contents) cd.str.contents = str(*d.str.contents);
          break;
        case A::DK::Value_float_array:
          cd.float_array.contents = slice(d.float_array.contents, [&](A::t f) { return approx(f); });
          break;
        case A::DK::Value_unknown:
          if (d.unknown.unresolved) unresolved(d.unknown.value);
          break;
        case A::DK::Value_extern: id(d.ex); break;
        case A::DK::Value_symbol: id(d.sym); break;
        case A::DK::Value_unresolved: unresolved(d.unresolved); break;
        case A::DK::Value_int: case A::DK::Value_char: case A::DK::Value_float: case A::DK::Value_boxed_int:
        case A::DK::Value_bottom: break;
      }
      return make<A::Approx>(c);
    });
  }
  void unresolved(const A::UnresolvedValue& u) {
    id(u.set_of_closures_id);
    id(u.sym);
  }
  const A::FunctionBody* function_body(const A::FunctionBody* b) {
    return memo(b, [&](const A::FunctionBody& x) {
      A::FunctionBody c = x;
      c.free_variables = set(x.free_variables);
      c.free_symbols = set(x.free_symbols);
      dbg(x.dbg);
      c.body = expr(x.body);
      return make<A::FunctionBody>(c);
    });
  }
  const A::FunctionDeclarations* approx_fun_decls(const A::FunctionDeclarations* d) {
    return memo(d, [&](const A::FunctionDeclarations& x) {
      A::FunctionDeclarations c = x;
      id(x.set_of_closures_id);
      id(x.set_of_closures_origin);
      c.funs = map(x.funs, [&](const A::FunctionDeclaration* f) {
        return memo(f, [&](const A::FunctionDeclaration& y) {
          A::FunctionDeclaration fc = y;
          id(y.closure_origin);
          fc.params = slice(y.params, [&](const Parameter& q) { return Parameter{id(q.var)}; });
          fc.function_body = function_body(y.function_body);
          return make<A::FunctionDeclaration>(fc);
        });
      });
      return make<A::FunctionDeclarations>(c);
    });
  }
  template <class T, class F>
  A::Lazy<T> lazy(const A::Lazy<T>& l, F copy) {
    if (l.is_null()) return l;
    return A::Lazy<T>::from_val(copy(l.force()));
  }
  freshening::project_var::T project_var(const freshening::project_var::T& f) {
    auto ident = [&](variable::t v) { return id(v); };
    return {map(f.vars_within_closure, ident), map(f.closure_id, ident)};
  }
  const A::ValueSetOfClosures* value_set_of_closures(const A::ValueSetOfClosures* s) {
    return memo(s, [&](const A::ValueSetOfClosures& x) {
      A::ValueSetOfClosures c = x;
      c.function_decls = approx_fun_decls(x.function_decls);
      c.bound_vars = map(x.bound_vars, [&](A::t a) { return approx(a); });
      c.free_vars = spec_map(x.free_vars);
      c.invariant_params = lazy(x.invariant_params, [&](const variable::Map<variable::Set>& m) {
        return map(m, [&](const variable::Set& vs) { return set(vs); });
      });
      c.recursive = lazy(x.recursive, [&](const variable::Set& vs) { return set(vs); });
      c.size = lazy(x.size, [&](const variable::Map<std::optional<long>>& m) {
        return map(m, [](std::optional<long> n) { return n; });
      });
      c.specialised_args = spec_map(x.specialised_args);
      c.freshening = project_var(x.freshening);
      c.direct_call_surrogates = map(x.direct_call_surrogates, [&](variable::t v) { return id(v); });
      return make<A::ValueSetOfClosures>(c);
    });
  }

  freshening::T freshening(const freshening::T& f) {
    auto ident = [&](variable::t v) { return id(v); };
    auto vars = [&](Slice<variable::t> vs) { return slice(vs, ident); };
    return {memo(f.active, [&](const freshening::Tbl& x) {
      freshening::Tbl c = x;
      c.sb_var = map(x.sb_var, ident);
      c.sb_mutable_var = map(x.sb_mutable_var, ident);
      c.sb_exn = map(x.sb_exn, [](long n) { return n; });
      c.back_var = map(x.back_var, vars);
      c.back_mutable_var = map(x.back_mutable_var, vars);
      return make<freshening::Tbl>(c);
    })};
  }
  E env(const E& e) {
    auto approxs = [&](A::t a) { return approx(a); };
    auto plain = [](long n) { return n; };
    E c = e;
    c.approx = map(e.approx, [&](const E::ScopedApprox& sa) { return E::ScopedApprox{sa.scope, approx(sa.approx)}; });
    c.approx_mutable = map(e.approx_mutable, approxs);
    c.approx_sym = map(e.approx_sym, approxs);
    c.projections = map(e.projections, [&](variable::t v) { return id(v); });
    c.current_functions = set(e.current_functions);
    c.freshening_ = freshening(e.freshening_);
    c.unroll_counts = map(e.unroll_counts, plain);
    c.inlining_counts = map(e.inlining_counts, plain);
    c.inlined_stub = set(e.inlined_stub);
    c.actively_unrolling_ = map(e.actively_unrolling_, plain);
    if (e.inlining_stats_closure_stack) leak("an inlining report's closure stack");
    dbg(e.inlined_debuginfo);
    return c;
  }
  R result(const R& r) {
    R c = r;
    c.approx = approx(r.approx);
    c.used_static_exceptions = set(r.used_static_exceptions);
    return c;
  }
  inline_and_simplify_aux::PreparedSetOfClosures prepared(const inline_and_simplify_aux::PreparedSetOfClosures& p) {
    inline_and_simplify_aux::PreparedSetOfClosures c = p;
    c.free_vars = map(p.free_vars, [&](const freshening::SpecApprox& sa) {
      return freshening::SpecApprox{spec(sa.spec), approx(sa.approx)};
    });
    c.specialised_args = spec_map(p.specialised_args);
    c.function_decls = fun_decls(p.function_decls);
    c.parameter_approximations = map(p.parameter_approximations, [&](A::t a) { return approx(a); });
    c.internal_value_set_of_closures = value_set_of_closures(p.internal_value_set_of_closures);
    c.set_of_closures_env = env(p.set_of_closures_env);
    return c;
  }
};

class Nursery {
 public:
  using Root = std::function<void(Promoter&)>;
  // the roots of a frame, registered while it lives
  class Roots {
   public:
    Roots(Nursery& n, Root root) : n_(n), root_(std::move(root)) { n_.roots_.push_back(&root_); }
    ~Roots() { n_.roots_.pop_back(); }
    Roots(const Roots&) = delete;
    Roots& operator=(const Roots&) = delete;

   private:
    Nursery& n_;
    Root root_;
  };

  explicit Nursery(bool always) : always_(always), protect_(std::getenv("CPPCAML_ZONE_PROTECT") != nullptr) {
    young_ = fresh();
    in_young_.emplace(*young_);
  }
  ~Nursery() {
    in_young_.reset();
    drop();
  }
  Nursery(const Nursery&) = delete;
  Nursery& operator=(const Nursery&) = delete;

  // a point where every pointer into the young zone that is used later is
  // in a root
  void point() {
    if (always_ || young_->bytes() >= (std::size_t{8} << 20)) promote();
  }
  void promote() {
    in_young_.reset();
    {
      std::vector<const Zone*> dying{young_.get()};
      Promoter p(dying);
      for (const Root* r : roots_) (*r)(p);
    }
    drop();
    young_ = fresh();
    in_young_.emplace(*young_);
  }

 private:
  std::unique_ptr<Zone> fresh() {
    auto z = std::make_unique<Zone>(protect_);
    transient_zones().push_back(z.get());
    return z;
  }
  void drop() {
    auto& tz = transient_zones();
    tz.erase(std::remove(tz.begin(), tz.end(), young_.get()), tz.end());
    if (protect_) young_->drop_protected();
    young_.reset();
  }

  bool always_, protect_;
  std::unique_ptr<Zone> young_;
  std::optional<ZoneScope> in_young_;
  std::vector<const Root*> roots_;
};

struct Simplifier {
  ExprResult simplify(const E& env, const R& r, t tree);
  NamedResult simplify_named(const E& env, const R& r, named tree);
  std::tuple<const SetOfClosures*, R, freshening::project_var::T> simplify_set_of_closures(
      const E& original_env, const R& r, const SetOfClosures* set_of_closures);
  NamedResult simplify_project_closure(const E& env, const R& r, const projection::ProjectClosure& project_closure);
  NamedResult simplify_move_within_set_of_closures(const E& env, const R& r,
                                                   const projection::MoveWithinSetOfClosures& m);
  NamedResult simplify_project_var(const E& env, const R& r, const projection::ProjectVar& project_var);
  ExprResult simplify_apply(const E& env, const R& r, const Apply* apply);
  ExprResult simplify_full_application(const E& env, const R& r, const A::FunctionDeclarations* function_decls,
                                       variable::t lhs_of_application, variable::t closure_id_being_applied,
                                       const A::FunctionDeclaration* function_decl,
                                       const A::ValueSetOfClosures* value_set_of_closures, Slice<variable::t> args,
                                       const std::vector<A::t>& args_approxs, const debuginfo::t& dbg,
                                       lambda::InlineAttribute inline_requested,
                                       lambda::SpecialiseAttribute specialise_requested);
  ExprResult simplify_partial_application(const E& env, const R& r, variable::t lhs_of_application,
                                          variable::t closure_id_being_applied,
                                          const A::FunctionDeclaration* function_decl, Slice<variable::t> args,
                                          const debuginfo::t& dbg, lambda::InlineAttribute inline_requested,
                                          lambda::SpecialiseAttribute specialise_requested);
  ExprResult simplify_over_application(const E& env, const R& r, Slice<variable::t> args,
                                       const std::vector<A::t>& args_approxs,
                                       const A::FunctionDeclarations* function_decls, variable::t lhs_of_application,
                                       variable::t closure_id_being_applied,
                                       const A::FunctionDeclaration* function_decl,
                                       const A::ValueSetOfClosures* value_set_of_closures, const debuginfo::t& dbg,
                                       lambda::InlineAttribute inline_requested,
                                       lambda::SpecialiseAttribute specialise_requested);
  std::tuple<Slice<t>, std::vector<A::t>, R> simplify_list(const E& env, const R& r, Slice<t> l);
  std::pair<const FunctionDeclaration*, variable::Map<SpecialisedTo>> duplicate_function(
      const E& env, const SetOfClosures* set_of_closures, variable::t fun_var, variable::t new_fun_var);

  Nursery* nursery = nullptr;  // (null: none)
  // the next set of closures simplified is bound at toplevel (its
  // functions' simplifications are separated by nursery points)
  bool toplevel_set_of_closures = false;

  inlining_decision::Simplify simplify_fn() {
    return [this](const E& env, const R& r, t e) { return simplify(env, r, e); };
  }
  augment_specialised_args::DuplicateFunction duplicate_fn() {
    return [this](const E& env, const SetOfClosures* s, variable::t fun_var, variable::t new_fun_var) {
      return duplicate_function(env, s, fun_var, new_fun_var);
    };
  }
};

// Simplify an expression that takes a set of closures and projects an
// individual closure from it.
NamedResult Simplifier::simplify_project_closure(const E& env, const R& r,
                                                 const projection::ProjectClosure& project_closure) {
  return simplify_free_variable_named(
      env, project_closure.set_of_closures,
      [&](const E&, variable::t set_of_closures, A::t set_of_closures_approx) -> NamedResult {
        using CK = A::CheckedSetOfClosures::Kind;
        A::CheckedSetOfClosures c = A::check_approx_for_set_of_closures(set_of_closures_approx);
        named same = n_project_closure(projection::ProjectClosure{set_of_closures, project_closure.closure_id});
        switch (c.kind) {
          case CK::Wrong:
            fatal([&](Formatter& f) {
              fprintf(f, "Wrong approximation when projecting closure: %a",
                      pr(projection::print_project_closure, project_closure));
            });
          case CK::Unresolved:
            // A set of closures coming from another compilation unit, whose
            // .cmx is missing; as such, we cannot have rewritten the function
            // and don't need to do any freshening.
            return {same, ret(r, A::value_unresolved(c.value))};
          case CK::Unknown: return {same, ret(r, A::value_unknown(A::other()))};
          case CK::Unknown_because_of_unresolved_value:
            return {same, ret(r, A::value_unknown(A::UnknownBecauseOf{true, c.value}))};
          case CK::Ok: break;
        }
        variable::t closure_id = A::freshen_and_check_closure_id(c.set, project_closure.closure_id);
        if (c.var) {
          projection::T p{projection::T::Kind::Project_closure};
          p.project_closure = projection::ProjectClosure{c.var, closure_id};
          projection::t projection = mk_projection(p);
          if (variable::t v = env.find_projection(projection)) return with_projection_var(env, r, v, projection);
        }
        if (auto rr = reference_recursive_function_directly(env, closure_id)) return {rr->first, ret(r, rr->second)};
        variable::t set_of_closures_var = c.var && env.mem(c.var) ? c.var : nullptr;
        A::t approx = A::value_closure(c.set, closure_id, nullptr, set_of_closures_var);
        return {n_project_closure(projection::ProjectClosure{set_of_closures, closure_id}), ret(r, approx)};
      });
}

// Simplify an expression that, given one closure within some set of
// closures, returns another closure (possibly the same one) within the same
// set.
NamedResult Simplifier::simplify_move_within_set_of_closures(const E& env, const R& r,
                                                             const projection::MoveWithinSetOfClosures& m) {
  return simplify_free_variable_named(env, m.closure, [&](const E&, variable::t closure, A::t closure_approx) -> NamedResult {
    using CK = A::CheckedClosure::Kind;
    A::CheckedClosure c = A::check_approx_for_closure_allowing_unresolved(closure_approx);
    named same = n_move_within_set_of_closures(projection::MoveWithinSetOfClosures{closure, m.start_from, m.move_to});
    switch (c.kind) {
      case CK::Wrong:
        fatal([&](Formatter& f) {
          fprintf(f, "Wrong approximation when moving within set of closures.  Approximation: %a  Term: %a",
                  pr(A::print, closure_approx), pr(projection::print_move_within_set_of_closures, m));
        });
      case CK::Unresolved: return {same, ret(r, A::value_unresolved(c.value))};
      case CK::Unknown: return {same, ret(r, A::value_unknown(A::other()))};
      case CK::Unknown_because_of_unresolved_value:
        // For example: a move upon a (move upon a closure whose .cmx file is
        // missing).
        return {same, ret(r, A::value_unknown(A::UnknownBecauseOf{true, c.value}))};
      case CK::Ok: break;
    }
    // CR-soon mshinwell: potentially misleading name---not freshening with
    // new names, but with previously fresh names
    variable::t move_to = A::freshen_and_check_closure_id(c.set, m.move_to);
    variable::t start_from = A::freshen_and_check_closure_id(c.set, m.start_from);
    projection::T p{projection::T::Kind::Move_within_set_of_closures};
    p.move = projection::MoveWithinSetOfClosures{closure, start_from, move_to};
    projection::t projection = mk_projection(p);
    if (variable::t v = env.find_projection(projection)) return with_projection_var(env, r, v, projection);
    if (auto rr = reference_recursive_function_directly(env, move_to)) return {rr->first, ret(r, rr->second)};
    // Moving from one closure to itself is a no-op.  We can return an [Var]
    // since we already have a variable bound to the closure.
    if (variable::equal(start_from, move_to)) return {n_expr(var(closure)), ret(r, closure_approx)};
    if (c.set_of_closures_var && env.mem(c.set_of_closures_var)) {
      // A variable bound to the set of closures is in scope, meaning we can
      // rewrite the [Move_within_set_of_closures] to a [Project_closure].
      A::t approx = A::value_closure(c.set, move_to, nullptr, c.set_of_closures_var);
      return {n_project_closure(projection::ProjectClosure{c.set_of_closures_var, move_to}), ret(r, approx)};
    }
    if (c.set_of_closures_symbol) {
      variable::t set_of_closures_var = variable::create(Names::symbol);
      projection::ProjectClosure project_closure{set_of_closures_var, move_to};
      variable::t project_closure_var = variable::create(Names::project_closure);
      t let1 = create_let(project_closure_var, n_project_closure(project_closure), var(project_closure_var));
      t expr = create_let(set_of_closures_var, n_symbol(c.set_of_closures_symbol), let1);
      A::t approx = A::value_closure(c.set, move_to, nullptr, set_of_closures_var, c.set_of_closures_symbol);
      return {n_expr(expr), ret(r, approx)};
    }
    // The set of closures is not available in scope, and we have no other
    // information by which to simplify the move.
    A::t approx = A::value_closure(c.set, move_to);
    return {n_move_within_set_of_closures(projection::MoveWithinSetOfClosures{closure, start_from, move_to}),
            ret(r, approx)};
  });
}

// Transform an expression denoting an access to a variable bound in a
// closure.  (inline_and_simplify.ml)
NamedResult Simplifier::simplify_project_var(const E& env, const R& r, const projection::ProjectVar& project_var) {
  return simplify_free_variable_named(env, project_var.closure, [&](const E&, variable::t closure, A::t approx) -> NamedResult {
    using CK = A::CheckedClosure::Kind;
    A::CheckedClosure c = A::check_approx_for_closure_allowing_unresolved(approx);
    projection::ProjectVar same{closure, project_var.closure_id, project_var.var};
    switch (c.kind) {
      case CK::Ok: break;
      // This value comes from a symbol for which we couldn't find any
      // approximation, telling us that names within the closure couldn't
      // have been renamed.  (inline_and_simplify.ml)
      case CK::Unresolved: return {n_project_var(same), ret(r, A::value_unresolved(c.value))};
      case CK::Unknown: return {n_project_var(same), ret(r, A::value_unknown(A::other()))};
      case CK::Unknown_because_of_unresolved_value:
        return {n_project_var(same), ret(r, A::value_unknown(A::UnknownBecauseOf{true, c.value}))};
      case CK::Wrong:
        // We must have the correct approximation of the value to ensure we
        // take account of all freshenings.
        fatal([&](Formatter& f) {
          fprintf(f, "[Project_var] from a value with wrong approximation: %a@.closure=%a@.approx of closure=%a@.",
                  pr(projection::print_project_var, project_var), pr(variable::print, closure), pr(A::print, approx));
        });
    }
    const freshening::project_var::T& fr = c.set->freshening;
    variable::t v = freshening::project_var::apply_var_within_closure(fr, project_var.var);
    variable::t closure_id = freshening::project_var::apply_closure_id(fr, project_var.closure_id);
    variable::t closure_id_in_approx = c.closure->closure_id;
    if (!variable::equal(closure_id, closure_id_in_approx))
      fatal([&](Formatter& f) {
        fprintf(f,
                "When simplifying [Project_var], the closure ID %a in the approximation of the set of closures did not "
                "match the closure ID %a in the [Project_var] term.  Approximation: %a@. Var-within-closure being "
                "projected: %a@.",
                pr(variable::print, closure_id_in_approx), pr(variable::print, closure_id), pr(A::print, approx),
                pr(variable::print, v));
      });
    projection::T p{projection::T::Kind::Project_var};
    p.project_var = projection::ProjectVar{closure, closure_id, v};
    projection::t projection = mk_projection(p);
    if (variable::t pv = env.find_projection(projection)) return with_projection_var(env, r, pv, projection);
    A::t bound_approx = A::approx_for_bound_var(c.set, v);
    named expr = n_project_var(projection::ProjectVar{closure, closure_id, v});
    if (env.mem(v)) expr = n_expr(var(v));
    return simplify_named_using_approx_and_env(env, r, expr, bound_approx);
  });
}

// Transforms closure definitions by applying [loop] on the code of every
// one of the set and on the expressions of the free variables.
// (inline_and_simplify.ml)
std::tuple<const SetOfClosures*, R, freshening::project_var::T> Simplifier::simplify_set_of_closures(
    const E& original_env, const R& r0, const SetOfClosures* set_of_closures) {
  // CR-soon mshinwell: Does this affect
  // [reference_recursive_function_directly]?
  const FunctionDeclarations* function_decls0 = freshening::rewrite_recursive_calls_with_symbols(
      original_env.freshening(), set_of_closures->function_decls, compilenv::closure_symbol);
  E env = original_env.increase_closure_depth();
  inline_and_simplify_aux::PreparedSetOfClosures prep =
      inline_and_simplify_aux::prepare_to_simplify_set_of_closures(env, set_of_closures, function_decls0, true, nullptr);
  variable::Map<const FunctionDeclaration*> funs;
  variable::Set used_params;
  R r = r0;
  bool points = std::exchange(toplevel_set_of_closures, false) && nursery;
  std::optional<Nursery::Roots> roots;
  if (points)
    roots.emplace(*nursery, [&](Promoter& pr) {
      prep = pr.prepared(prep);
      funs = pr.map(funs, [&](const FunctionDeclaration* d) { return pr.fun_decl(d); });
      used_params = pr.set(used_params);
      r = pr.result(r);
    });
  // (Map.iter: in key order; the declarations looked up again after a
  // nursery point)
  std::vector<variable::t> fun_vars;
  prep.function_decls->funs.iter([&](variable::t fun_var, const FunctionDeclaration*) { fun_vars.push_back(fun_var); });
  for (variable::t fun_var : fun_vars) {
    const FunctionDeclaration* function_decl = *prep.function_decls->funs.find_opt(fun_var);
    E closure_env = inline_and_simplify_aux::prepare_to_simplify_closure(
        function_decl, prep.free_vars, prep.specialised_args, prep.parameter_approximations, prep.set_of_closures_env);
    E body_env = closure_env.enter_closure(fun_var, inlining_decision::should_inline_inside_declaration(function_decl),
                                           function_decl->dbg);
    if (!body_env.inside_set_of_closures_declaration(prep.function_decls->set_of_closures_origin))
      misc::fatal_error("Inline_and_simplify.simplify_set_of_closures");  // (assert)
    auto [body, r2] = simplify(body_env, r, function_decl->body);
    r = r2;
    const FunctionDeclaration* fd = create_function_declaration(
        function_decl->params, body, function_decl->stub, function_decl->dbg, function_decl->inline_,
        function_decl->specialise, function_decl->is_a_functor, function_decl->closure_origin, function_decl->poll);
    variable::Set used_params2 = flambda::used_params(fd);
    funs = funs.add(fun_var, fd);
    used_params = variable::Set::union_(used_params, used_params2);
    if (points) nursery->point();
  }
  const FunctionDeclarations* function_decls = prep.function_decls;
  const FunctionDeclarations* function_decls2 = update_function_declarations(function_decls, funs);
  auto invariant_params = A::Lazy<variable::Map<variable::Set>>::of_fun(
      [function_decls2] { return invariant_params::invariant_params_in_recursion(function_decls2); });
  auto recursive = A::Lazy<variable::Set>::of_fun(
      [function_decls2] { return find_recursive_functions::in_function_declarations(function_decls2); });
  auto keep_body = inline_and_simplify_aux::keep_body_check(function_decls2->is_classic_mode, recursive);
  const A::FunctionDeclarations* function_decls_approx = A::function_declarations_approx(keep_body, function_decls2);
  const A::ValueSetOfClosures* ivsoc = prep.internal_value_set_of_closures;
  const A::ValueSetOfClosures* value_set_of_closures = A::create_value_set_of_closures(
      function_decls_approx, ivsoc->bound_vars, ivsoc->free_vars, invariant_params, recursive, ivsoc->specialised_args,
      ivsoc->freshening, ivsoc->direct_call_surrogates);
  variable::Map<variable::t> direct_call_surrogates = ivsoc->direct_call_surrogates.fold(
      [](variable::t existing, variable::t surrogate, variable::Map<variable::t> acc) { return acc.add(existing, surrogate); },
      variable::Map<variable::t>{});
  const SetOfClosures* soc = create_set_of_closures(
      function_decls2, prep.free_vars.map([](const freshening::SpecApprox& sa) { return sa.spec; }), prep.specialised_args,
      direct_call_surrogates);
  R r3 = ret(r, A::value_set_of_closures(value_set_of_closures));
  return {soc, r3, value_set_of_closures->freshening};
}

ExprResult Simplifier::simplify_apply(const E& env0, const R& r, const Apply* apply) {
  variable::t lhs_of_application0 = apply->func;
  debuginfo::t dbg = env0.add_inlined_debuginfo(apply->dbg);
  lambda::InlineAttribute inline_requested = apply->inline_;
  lambda::SpecialiseAttribute specialise_requested = apply->specialise;
  return simplify_free_variable(env0, lhs_of_application0, [&](const E& env1, variable::t lhs, A::t lhs_approx) {
    return simplify_free_variables(env1, apply->args, [&](const E& env2, Slice<variable::t> args,
                                                          const std::vector<A::t>& args_approxs) -> ExprResult {
      // By using the approximation of the left-hand side of the
      // application, attempt to determine which function is being applied
      // (even if the application is currently [Indirect]).  If
      // successful---in which case we then have a direct application---
      // consider inlining.
      A::CheckedClosure c = A::check_approx_for_closure(lhs_approx);
      if (c.kind != A::CheckedClosure::Kind::Ok)
        // Insufficient approximation information to simplify.
        return {flambda::apply(lhs, args, CallKind{}, dbg, inline_requested, specialise_requested),
                ret(r, A::value_unknown(A::other()))};
      variable::t lhs_of_application = lhs;
      variable::t closure_id_being_applied = c.closure->closure_id;
      const A::ValueSetOfClosures* value_set_of_closures = c.set;
      E env = env2;
      std::function<t(t)> wrap = [](t e) { return e; };
      // If the call site is a direct call to a function that has a "direct
      // call surrogate" (see inline_and_simplify_aux.mli), repoint the call
      // to the surrogate.
      const variable::Map<variable::t>& surrogates = value_set_of_closures->direct_call_surrogates;
      if (const variable::t* s0 = surrogates.find_opt(closure_id_being_applied)) {
        variable::t surrogate = *s0;
        while (const variable::t* s = surrogates.find_opt(surrogate)) surrogate = *s;  // find_transitively
        variable::t surrogate_var = variable::rename(lhs_of_application);
        projection::MoveWithinSetOfClosures move_to_surrogate{lhs_of_application, closure_id_being_applied, surrogate};
        A::t approx_for_surrogate = A::value_closure(value_set_of_closures, surrogate, surrogate_var,
                                                     c.set_of_closures_var, c.set_of_closures_symbol);
        env = env.add(surrogate_var, approx_for_surrogate);
        wrap = [surrogate_var, move_to_surrogate](t e) {
          return create_let(surrogate_var, n_move_within_set_of_closures(move_to_surrogate), e);
        };
        lhs_of_application = surrogate_var;
        closure_id_being_applied = surrogate;
      }
      const A::FunctionDeclarations* function_decls = value_set_of_closures->function_decls;
      const A::FunctionDeclaration* const* fd = function_decls->funs.find_opt(closure_id_being_applied);
      if (!fd)
        fatal([&](Formatter& f) {
          fprintf(f, "When handling application expression, approximation references non-existent closure %a@.",
                  pr(variable::print, closure_id_being_applied));
        });
      const A::FunctionDeclaration* function_decl = *fd;
      R r2 = apply->call_kind.direct ? r : r.map_benefit(B::direct_call_of_indirect);
      long nargs = static_cast<long>(args.size());
      long arity = A::function_arity(function_decl);
      ExprResult res;
      if (nargs == arity)
        res = simplify_full_application(env, r2, function_decls, lhs_of_application, closure_id_being_applied,
                                        function_decl, value_set_of_closures, args, args_approxs, dbg, inline_requested,
                                        specialise_requested);
      else if (nargs > arity)
        res = simplify_over_application(env, r2, args, args_approxs, function_decls, lhs_of_application,
                                        closure_id_being_applied, function_decl, value_set_of_closures, dbg,
                                        inline_requested, specialise_requested);
      else if (nargs > 0 && nargs < arity)
        res = simplify_partial_application(env, r2, lhs_of_application, closure_id_being_applied, function_decl, args,
                                           dbg, inline_requested, specialise_requested);
      else
        fatal([&](Formatter& f) {
          fprintf(f, "Function with arity %d when simplifying application expression: %a", arity,
                  pr(print, static_cast<t>(apply)));
        });
      return {wrap(res.first), res.second};
    });
  });
}

ExprResult Simplifier::simplify_full_application(const E& env, const R& r,
                                                 const A::FunctionDeclarations* function_decls,
                                                 variable::t lhs_of_application, variable::t closure_id_being_applied,
                                                 const A::FunctionDeclaration* function_decl,
                                                 const A::ValueSetOfClosures* value_set_of_closures,
                                                 Slice<variable::t> args, const std::vector<A::t>& args_approxs,
                                                 const debuginfo::t& dbg, lambda::InlineAttribute inline_requested,
                                                 lambda::SpecialiseAttribute specialise_requested) {
  return inlining_decision::for_call_site(env, r, function_decls, lhs_of_application, closure_id_being_applied,
                                          function_decl, value_set_of_closures, args, args_approxs, dbg, simplify_fn(),
                                          inline_requested, specialise_requested);
}

ExprResult Simplifier::simplify_partial_application(const E& env, const R& r, variable::t lhs_of_application,
                                                    variable::t closure_id_being_applied,
                                                    const A::FunctionDeclaration* function_decl,
                                                    Slice<variable::t> args, const debuginfo::t& dbg,
                                                    lambda::InlineAttribute inline_requested,
                                                    lambda::SpecialiseAttribute specialise_requested) {
  long arity = A::function_arity(function_decl);
  if (!(arity > static_cast<long>(args.size()))) misc::fatal_error("Inline_and_simplify.simplify_partial_application");
  // For simplicity, we disallow [@inline] attributes on partial
  // applications.  The user may always write an explicit wrapper instead
  // with such an attribute.
  using IK = lambda::InlineAttribute::Kind;
  switch (inline_requested.kind) {
    case IK::Always_inline: case IK::Never_inline:
      prerr_inlining_impossible(dbg, "[@inlined] attributes may not be used on partial applications");
      break;
    case IK::Unroll:
      prerr_inlining_impossible(dbg, "[@unrolled] attributes may not be used on partial applications");
      break;
    case IK::Hint_inline: case IK::Default_inline: break;
  }
  if (specialise_requested != lambda::SpecialiseAttribute::Default_specialise)
    prerr_inlining_impossible(dbg, "[@specialised] attributes may not be used on partial applications");
  std::vector<Parameter> freshened_params;  // List.map: in order
  for (const Parameter& p : function_decl->params) freshened_params.push_back(parameter::wrap(variable::rename(p.var)));
  // Misc.Stdlib.List.map2_prefix (fun arg id' -> id', arg) args
  // freshened_params
  std::vector<std::pair<Parameter, variable::t>> applied_args;
  for (std::size_t k = 0; k < args.size(); ++k) applied_args.emplace_back(freshened_params[k], args[k]);
  std::vector<Parameter> remaining_args(freshened_params.begin() + static_cast<long>(args.size()),
                                        freshened_params.end());
  std::vector<variable::t> all;
  for (const Parameter& p : freshened_params) all.push_back(p.var);
  t body = apply(lhs_of_application, slice(all), CallKind{closure_id_being_applied}, dbg, lambda::InlineAttribute{},
                 lambda::SpecialiseAttribute::Default_specialise);
  variable::t closure_variable = variable::rename(closure_id_being_applied);
  t wrapper_accepting_remaining_args =
      flambda_utils::make_closure_declaration(false, closure_variable, body, slice(remaining_args));
  std::vector<std::pair<variable::t, named>> bindings;  // List.map: in order
  for (const auto& [param, arg] : applied_args) bindings.emplace_back(param.var, n_expr(var(arg)));
  t with_known_args = flambda_utils::bind(bindings, wrapper_accepting_remaining_args);
  return simplify(env, r, with_known_args);
}

ExprResult Simplifier::simplify_over_application(const E& env, const R& r, Slice<variable::t> args,
                                                 const std::vector<A::t>& args_approxs,
                                                 const A::FunctionDeclarations* function_decls,
                                                 variable::t lhs_of_application, variable::t closure_id_being_applied,
                                                 const A::FunctionDeclaration* function_decl,
                                                 const A::ValueSetOfClosures* value_set_of_closures,
                                                 const debuginfo::t& dbg, lambda::InlineAttribute inline_requested,
                                                 lambda::SpecialiseAttribute specialise_requested) {
  std::size_t arity = static_cast<std::size_t>(A::function_arity(function_decl));
  if (!(arity < args.size()) || args.size() != args_approxs.size())
    misc::fatal_error("Inline_and_simplify.simplify_over_application");  // (assert)
  Slice<variable::t> full_app_args{args.p, arity};
  Slice<variable::t> remaining_args{args.p + arity, args.size() - arity};
  std::vector<A::t> full_app_approxs(args_approxs.begin(), args_approxs.begin() + static_cast<long>(arity));
  auto [expr, r2] =
      simplify_full_application(env, r, function_decls, lhs_of_application, closure_id_being_applied, function_decl,
                                value_set_of_closures, slice(std::vector<variable::t>(full_app_args.begin(), full_app_args.end())),
                                full_app_approxs, dbg, inline_requested, specialise_requested);
  variable::t func_var = variable::create(Names::full_apply);
  t e = create_let(func_var, n_expr(expr),
                   apply(func_var, slice(std::vector<variable::t>(remaining_args.begin(), remaining_args.end())),
                         CallKind{}, dbg, inline_requested, specialise_requested));
  e = lift_code::lift_lets_expr(e, true);
  return simplify(env.set_never_inline(), r2, e);
}

NamedResult Simplifier::simplify_named(const E& env, const R& r, named tree) {
  switch (tree->kind) {
    case NK::Symbol: {
      // New Symbol construction could have been introduced during
      // transformation (by simplify_named_using_approx_and_env).  When this
      // comes from another compilation unit, we must load it.
      A::t approx = env.find_or_load_symbol(as<NSymbol>(tree)->sym);
      return simplify_named_using_approx(r, tree, approx);
    }
    case NK::Const: return {tree, ret(r, simplify_const(as<NConst>(tree)->c))};
    case NK::Allocated_const: return {tree, ret(r, approx_for_allocated_const(as<NAllocated_const>(tree)->c))};
    case NK::Read_mutable: {
      // See comment on the [Assign] case.
      variable::t mut_var = freshening::apply_mutable_variable(env.freshening(), as<NRead_mutable>(tree)->var);
      return {n_read_mutable(mut_var), ret(r, A::value_unknown(A::other()))};
    }
    case NK::Read_symbol_field: {
      auto* rs = as<NRead_symbol_field>(tree);
      A::t approx = env.find_or_load_symbol(rs->sym);
      // CR-someday mshinwell: Think about [Unreachable] vs. [Value_bottom].
      A::t field = A::get_field(approx, rs->field);
      if (!field) return {n_expr(proved_unreachable()), r};
      field = A::augment_with_symbol_field(field, rs->sym, rs->field);
      return simplify_named_using_approx_and_env(env, r, tree, field);
    }
    case NK::Set_of_closures: {
      auto [set_of_closures, r2, first_freshening] =
          simplify_set_of_closures(env, r, as<NSet_of_closures>(tree)->set);
      auto simplify_result = [&, first_freshening = first_freshening](const R& rr, t expr,
                                                                       const char* pass_name) -> NamedResult {
        // If simplifying a set of closures more than once during any given
        // round of simplification, the [Freshening.Project_var]
        // substitutions arising from each call to [simplify_set_of_closures]
        // must be composed.  (inline_and_simplify.ml)
        auto [e2, r3] = simplify(env.set_never_inline(), rr, expr);
        A::t approx = r3.approx;
        A::CheckedSetOfClosures c = A::strict_check_approx_for_set_of_closures(approx);
        if (c.kind != A::CheckedSetOfClosures::Kind::Ok)
          fatal([&](Formatter& f) {
            fprintf(f, "Unexpected approximation returned from simplification of [%s] result: %a", pass_name,
                    pr(A::print, approx));
          });
        freshening::project_var::T fr = freshening::project_var::compose(first_freshening, c.set->freshening);
        const A::ValueSetOfClosures* vsoc = A::update_freshening_of_value_set_of_closures(c.set, fr);
        return {n_expr(e2), ret(r3, A::value_set_of_closures(vsoc))};
      };
      // This does the actual substitutions of specialised args introduced
      // by [Unbox_closures] for free variables.  (inline_and_simplify.ml)
      if (const SetOfClosures* s = remove_free_vars_equal_to_args::run(*env.ppf_dump(), set_of_closures))
        set_of_closures = s;
      // Do [Unbox_closures] next to try to decide which things are free
      // variables and which things are specialised arguments before
      // unboxing them.
      if (auto u = unbox_closures::pass().rewrite_set_of_closures(env, duplicate_fn(), set_of_closures))
        return simplify_result(r2.add_benefit(u->second), u->first, "Unbox_closures");
      if (auto u = unbox_free_vars_of_closures::run(env, set_of_closures))
        return simplify_result(r2.add_benefit(u->second), u->first, "Unbox_free_vars_of_closures");
      // CR-soon mshinwell: should maybe add one allocation for the stub
      if (auto u = unbox_specialised_args::pass().rewrite_set_of_closures(env, duplicate_fn(), set_of_closures))
        return simplify_result(r2.add_benefit(u->second), u->first, "Unbox_specialised_args");
      if (const SetOfClosures* s = remove_unused_arguments::separate_unused_arguments_in_set_of_closures(set_of_closures)) {
        t expr = flambda_utils::name_expr(n_set_of_closures(s), Names::remove_unused_arguments);
        return simplify_result(r2, expr, "Remove_unused_arguments");
      }
      return {n_set_of_closures(set_of_closures), r2};
    }
    case NK::Project_closure: return simplify_project_closure(env, r, as<NProject_closure>(tree)->p);
    case NK::Project_var: return simplify_project_var(env, r, as<NProject_var>(tree)->p);
    case NK::Move_within_set_of_closures:
      return simplify_move_within_set_of_closures(env, r, as<NMove_within_set_of_closures>(tree)->m);
    case NK::Prim: {
      auto* p = as<NPrim>(tree);
      const clambda::Primitive* prim = p->prim;
      debuginfo::t dbg = env.add_inlined_debuginfo(p->dbg);
      return simplify_free_variables_named(env, p->args, [&](const E& env2, Slice<variable::t> args,
                                                              const std::vector<A::t>& args_approxs) -> NamedResult {
        using K = clambda::Primitive::K;
        named tree2 = make<NPrim>(NPrim{{NK::Prim}, prim, args, dbg});
        // CR-someday mshinwell: Optimise [Pfield_computed].
        if (prim->kind == K::Pfield) {
          if (args.size() != 1) misc::fatal_error("Pfield arity error");
          long field_index = prim->n;
          projection::T pt{projection::T::Kind::Field};
          pt.field_index = field_index;
          pt.field_var = args[0];
          projection::t projection = mk_projection(pt);
          if (variable::t v = env2.find_projection(projection)) return with_projection_var(env2, r, v, projection);
          A::t arg_approx = args_approxs[0];
          A::t approx = A::get_field(arg_approx, field_index);
          if (!approx) return {n_expr(proved_unreachable()), r};
          named t2 = tree2;
          if (arg_approx->symbol && !arg_approx->symbol->field) {
            // If the [Pfield] is projecting directly from a symbol, rewrite
            // the expression to [Read_symbol_field].
            symbol::t symbol = arg_approx->symbol->sym;
            approx = A::augment_with_symbol_field(approx, symbol, field_index);
            t2 = n_read_symbol_field(symbol, field_index);
          } else {
            // This [Pfield] is either not projecting from a symbol at all,
            // or it is the projection of a projection from a symbol.
            approx = E::really_import_approx(approx);
          }
          return simplify_named_using_approx_and_env(env2, r, t2, approx);
        }
        if ((prim->kind == K::Parraysetu || prim->kind == K::Parraysets) && args.size() == 3) {
          A::t block_approx = args_approxs[0];
          A::t value_approx = args_approxs[2];
          if (A::warn_on_mutation(block_approx))
            location::prerr_warning(debuginfo::to_location(dbg),
                                    warnings::Warning::make(warnings::Warning::K::Flambda_assignment_to_non_mutable_value));
          lambda::ArrayKind kind = prim->array;
          auto check = [&]() {
            if (kind == lambda::ArrayKind::Paddrarray || kind == lambda::ArrayKind::Pintarray)
              // CR pchambart: Do a proper warning here
              fatal([&](Formatter& f) {
                fprintf(f, "Assignment of a float to a specialised non-float array: %a", pr(print_named, tree2));
              });
          };
          if (block_approx->descr.kind == A::DK::Value_float_array) {
            check();
            kind = lambda::ArrayKind::Pfloatarray;
          } else if (value_approx->descr.kind == A::DK::Value_float && config::flat_float_array) {
            check();
            kind = lambda::ArrayKind::Pfloatarray;
            // CR pchambart: This should be accounted by the benefit
          }
          clambda::Primitive np = *prim;
          np.array = kind;
          np.id = clambda::fresh_uconstant_id();
          return {n_prim(np, args, dbg), ret(r, A::value_unknown(A::other()))};
        }
        if (prim->kind == K::Psetfield && !args.empty()) {
          if (A::warn_on_mutation(args_approxs[0]))
            location::prerr_warning(debuginfo::to_location(dbg),
                                    warnings::Warning::make(warnings::Warning::K::Flambda_assignment_to_non_mutable_value));
          return {tree2, ret(r, A::value_unknown(A::other()))};
        }
        if (prim->kind == K::Psetfield || prim->kind == K::Parraysetu || prim->kind == K::Parraysets)
          misc::fatal_error("Psetfield / Parraysetu / Parraysets arity error");
        if (prim->kind == K::Psequand || prim->kind == K::Psequor)
          misc::fatal_error("Psequand and Psequor must be expanded (see handling in closure_conversion.ml)");
        auto [expr, approx, benefit] =
            simplify_primitives::primitive(*prim, args, args_approxs, tree2, dbg, size_int);
        R r2 = r.map_benefit([&](inlining_cost::Benefit b) { return B::plus(benefit, b); });
        if (prim->kind == K::Popaque) approx = A::value_unknown(A::other());
        return {expr, ret(r2, approx)};
      });
    }
    case NK::Expr: {
      auto [e, r2] = simplify(env, r, as<NExpr>(tree)->expr);
      return {n_expr(e), r2};
    }
  }
  misc::fatal_error("Inline_and_simplify.simplify_named");
}

ExprResult Simplifier::simplify(const E& env, const R& r, t tree) {
  switch (tree->kind) {
    case EK::Var: {
      variable::t v = freshening::apply_variable(env.freshening(), as<Var>(tree)->var);
      // If from the approximations we can simplify [var], then we will be
      // forced to insert [let]-expressions (done using [name_expr], in
      // [Simple_value_approx]) to bind a [named].  (inline_and_simplify.ml)
      return simplify_using_approx_and_env(env, r, var(v), env.find_exn(v));
    }
    case EK::Apply: return simplify_apply(env, r, as<Apply>(tree));
    case EK::Let: {
      using Acc = std::pair<E, R>;
      auto for_defining_expr = [&](Acc acc, variable::t v, named defining_expr) {
        auto [de, r2] = simplify_named(acc.first, acc.second, defining_expr);
        auto [v2, sb] = freshening::add_variable(acc.first.freshening(), v);
        E e2 = acc.first.set_freshening(sb);
        e2 = e2.add(v2, r2.approx);
        return std::tuple<Acc, variable::t, named>{Acc{e2, r2}, v2, de};
      };
      auto for_last_body = [&](Acc acc, t body) { return simplify(acc.first, acc.second, body); };
      auto filter_defining_expr = [&](R rr, variable::t v, named defining_expr, const variable::Set& free_vars_of_body) {
        if (free_vars_of_body.mem(v)) return std::tuple<R, variable::t, named>{rr, v, defining_expr};
        if (effect_analysis::no_effects_named(defining_expr)) {
          R r2 = rr.map_benefit([&](inlining_cost::Benefit b) { return B::remove_code_named(defining_expr, b); });
          return std::tuple<R, variable::t, named>{r2, v, nullptr};
        }
        return std::tuple<R, variable::t, named>{rr, v, defining_expr};
      };
      return fold_lets_option<Acc, R>(tree, Acc{env, r}, for_defining_expr, for_last_body, filter_defining_expr);
    }
    case EK::Let_mutable: {
      auto* lm = as<Let_mutable>(tree);
      // CR-someday mshinwell: add the dead let elimination, as above.
      return simplify_free_variable(env, lm->initial_value, [&](const E& env2, variable::t v, A::t) -> ExprResult {
        auto [mut_var, sb] = freshening::add_mutable_variable(env2.freshening(), lm->var);
        E env3 = env2.set_freshening(sb);
        auto [body, r2] = simplify(env3.add_mutable(mut_var, A::value_unknown(A::other())), r, lm->body);
        return {let_mutable(mut_var, v, lm->contents_kind, body), r2};
      });
    }
    case EK::Static_raise: {
      auto* sr = as<Static_raise>(tree);
      static_exception::t i = freshening::apply_static_exception(env.freshening(), sr->exn);
      return simplify_free_variables(env, sr->args, [&](const E&, Slice<variable::t> args, const std::vector<A::t>&) {
        R r2 = r.use_static_exception(i);
        return ExprResult{static_raise(i, args), ret(r2, A::value_bottom())};
      });
    }
    case EK::Static_catch: {
      auto* sc = as<Static_catch>(tree);
      if (auto* l = as<Let>(sc->body); l && !flambda_utils::might_raise_static_exn(l->defining_expr, sc->exn))
        return simplify(env, r, create_let(l->var, l->defining_expr, static_catch(sc->exn, sc->vars, l->body, sc->handler)));
      auto [i, sb] = freshening::add_static_exception(env.freshening(), sc->exn);
      E env2 = env.set_freshening(sb);
      auto [body, r2] = simplify(env2, r, sc->body);
      // CR-soon mshinwell: for robustness, R.used_static_exceptions should
      // maybe be removed.
      if (!r2.used_static_exceptions.mem(i))
        // If the static exception is not used, we can drop the declaration
        return {body, r2};
      if (auto* raise = as<Static_raise>(body)) {
        if (raise->exn != i) misc::fatal_error("Inline_and_simplify: Static_catch");  // (assert)
        if (sc->vars.size() != raise->args.size()) misc::fatal_error("Invalid_argument(\"List.fold_left2\")");
        t handler = sc->handler;
        for (std::size_t k = 0; k < sc->vars.size(); ++k)
          handler = create_let(sc->vars[k].var, n_expr(var(raise->args[k])), handler);
        R r3 = r2.exit_scope_catch(i);
        return simplify(env2, r3, handler);
      }
      std::vector<variable::t> vs;
      for (const CatchVar& cv : sc->vars) vs.push_back(cv.var);
      auto [vars2, sb2] = freshening::add_variables_prime(env2.freshening(), slice(vs));
      std::vector<CatchVar> vars;
      for (std::size_t k = 0; k < vars2.size(); ++k) vars.push_back(CatchVar{vars2[k], sc->vars[k].kind});
      A::t approx = r2.approx;
      E env3 = env2.set_freshening(sb2);
      for (const CatchVar& cv : vars) env3 = env3.add(cv.var, A::value_unknown(A::other()));
      env3 = env3.inside_branch();
      auto [handler, r3] = simplify(env3, r2, sc->handler);
      R r4 = r3.exit_scope_catch(i);
      return {static_catch(i, slice(vars), body, handler), r4.meet_approx(env3, approx)};
    }
    case EK::Try_with: {
      auto* tw = as<Try_with>(tree);
      auto [body, r2] = simplify(env, r, tw->body);
      auto [id, sb] = freshening::add_variable(env.freshening(), tw->var);
      E env2 = env.set_freshening(sb).add(id, A::value_unknown(A::other()));
      env2 = env2.inside_branch();
      auto [handler, r3] = simplify(env2, r2, tw->handler);
      return {try_with(body, id, handler), ret(r3, A::value_unknown(A::other()))};
    }
    case EK::If_then_else: {
      auto* ite = as<If_then_else>(tree);
      // When arg is the constant false or true (or something considered as
      // true), we can drop the if and replace it by a sequence.
      return simplify_free_variable(env, ite->cond, [&](const E& env2, variable::t arg, A::t arg_approx) -> ExprResult {
        const A::Descr& d = arg_approx->descr;
        if (d.kind == A::DK::Value_int && d.i == 0) {  // Constant [false]: keep [ifnot]
          auto [ifnot, r2] = simplify(env2, r, ite->ifnot);
          return {ifnot, r2.map_benefit(B::remove_branch)};
        }
        if (d.kind == A::DK::Value_int || d.kind == A::DK::Value_block) {  // Constant [true]: keep [ifso]
          auto [ifso, r2] = simplify(env2, r, ite->ifso);
          return {ifso, r2.map_benefit(B::remove_branch)};
        }
        E env3 = env2.inside_branch();
        auto [ifso, r2] = simplify(env3, r, ite->ifso);
        A::t ifso_approx = r2.approx;
        auto [ifnot, r3] = simplify(env3, r2, ite->ifnot);
        return {if_then_else(arg, ifso, ifnot), r3.meet_approx(env3, ifso_approx)};
      });
    }
    case EK::While: {
      auto* w = as<While>(tree);
      auto [cond, r2] = simplify(env, r, w->cond);
      auto [body, r3] = simplify(env, r2, w->body);
      return {while_(cond, body), ret(r3, A::value_unknown(A::other()))};
    }
    case EK::Send: {
      auto* s = as<Send>(tree);
      debuginfo::t dbg = env.add_inlined_debuginfo(s->dbg);
      return simplify_free_variable(env, s->meth, [&](const E& env2, variable::t meth, A::t) {
        return simplify_free_variable(env2, s->obj, [&](const E& env3, variable::t obj, A::t) {
          return simplify_free_variables(env3, s->args, [&](const E&, Slice<variable::t> args, const std::vector<A::t>&) {
            return ExprResult{send(s->meth_kind, meth, obj, args, dbg), ret(r, A::value_unknown(A::other()))};
          });
        });
      });
    }
    case EK::For: {
      auto* f = as<For>(tree);
      return simplify_free_variable(env, f->from_value, [&](const E& env2, variable::t from_value, A::t) {
        return simplify_free_variable(env2, f->to_value, [&](const E& env3, variable::t to_value, A::t) -> ExprResult {
          auto [bound_var, sb] = freshening::add_variable(env3.freshening(), f->bound_var);
          E env4 = env3.set_freshening(sb).add(bound_var, A::value_unknown(A::other()));
          auto [body, r2] = simplify(env4, r, f->body);
          return {for_(bound_var, from_value, to_value, f->direction, body), ret(r2, A::value_unknown(A::other()))};
        });
      });
    }
    case EK::Assign: {
      auto* a = as<Assign>(tree);
      // No need to use something like [simplify_free_variable]: the
      // approximation of [being_assigned] is always unknown.
      variable::t being_assigned = freshening::apply_mutable_variable(env.freshening(), a->being_assigned);
      return simplify_free_variable(env, a->new_value, [&](const E&, variable::t new_value, A::t) {
        return ExprResult{assign(being_assigned, new_value), ret(r, A::value_unknown(A::other()))};
      });
    }
    case EK::Switch: {
      auto* sw = as<Switch>(tree);
      // When [arg] is known to be a variable whose approximation is that of a
      // block with a fixed tag or a fixed integer, we can eliminate the
      // [Switch].  (inline_and_simplify.ml)
      return simplify_free_variable(env, sw->scrutinee, [&](const E& env2, variable::t arg, A::t arg_approx) -> ExprResult {
        // filtered_switch_branches = Must_be_taken of Flambda.t | Can_be_taken
        // of (int * Flambda.t) list (the compatible branches consed: in
        // reverse order)
        struct Filtered {
          t must;
          std::vector<SwitchCase> can;
        };
        auto filter_branches = [&](auto filter, Slice<SwitchCase> branches) {
          Filtered f{nullptr, {}};
          for (const SwitchCase& c : branches) {
            A::SwitchBranchSelection s = filter(arg_approx, c.key);
            if (s == A::SwitchBranchSelection::Cannot_be_taken) continue;
            if (s == A::SwitchBranchSelection::Can_be_taken) {
              f.can.insert(f.can.begin(), c);
              continue;
            }
            return Filtered{c.action, {}};
          }
          return f;
        };
        Filtered filtered_consts = filter_branches(A::potentially_taken_const_switch_branch, sw->consts);
        Filtered filtered_blocks = filter_branches(A::potentially_taken_block_switch_branch, sw->blocks);
        if (filtered_consts.must && filtered_blocks.must) misc::fatal_error("Inline_and_simplify: Switch");  // (assert false)
        if (filtered_consts.must || filtered_blocks.must) {
          t branch = filtered_consts.must ? filtered_consts.must : filtered_blocks.must;
          auto [lam, r2] = simplify(env2, r, branch);
          return {lam, r2.map_benefit(B::remove_branch)};
        }
        const std::vector<SwitchCase>& consts = filtered_consts.can;
        const std::vector<SwitchCase>& blocks = filtered_blocks.can;
        if (consts.empty() && blocks.empty() && !sw->failaction)
          // If the switch is applied to a statically-known value that does
          // not match any case: (inline_and_simplify.ml)
          return {proved_unreachable(), ret(r, A::value_bottom())};
        t single = nullptr;
        if (consts.size() == 1 && blocks.empty() && !sw->failaction) single = consts[0].action;
        else if (consts.empty() && blocks.size() == 1 && !sw->failaction) single = blocks[0].action;
        else if (consts.empty() && blocks.empty() && sw->failaction) single = sw->failaction;
        if (single) {
          auto [lam, r2] = simplify(env2, r, single);
          return {lam, r2.map_benefit(B::remove_branch)};
        }
        E env3 = env2.inside_branch();
        R rr = r.set_approx(A::value_bottom());
        // List.fold_right f consts ([], r): the last case first
        auto fold = [&](const std::vector<SwitchCase>& cases) {
          std::vector<SwitchCase> acc(cases.size());
          for (std::size_t k = cases.size(); k-- > 0;) {
            A::t approx = rr.approx;
            auto [lam, r2] = simplify(env3, rr, cases[k].action);
            acc[k] = SwitchCase{cases[k].key, lam};
            rr = r2.meet_approx(env3, approx);
          }
          return acc;
        };
        std::vector<SwitchCase> nconsts = fold(consts);
        std::vector<SwitchCase> nblocks = fold(blocks);
        t failaction = nullptr;
        if (sw->failaction) {
          A::t approx = rr.approx;
          auto [l, r2] = simplify(env3, rr, sw->failaction);
          failaction = l;
          rr = r2.meet_approx(env3, approx);
        }
        return {switch_(arg, sw->numconsts, slice(nconsts), sw->numblocks, slice(nblocks), failaction), rr};
      });
    }
    case EK::String_switch: {
      auto* ss = as<String_switch>(tree);
      return simplify_free_variable(env, ss->scrutinee, [&](const E& env2, variable::t arg, A::t arg_approx) -> ExprResult {
        std::optional<std::string_view> arg_string = A::check_approx_for_string(arg_approx);
        if (!arg_string) {
          E env3 = env2.inside_branch();
          R rr = r;
          std::vector<StringCase> sw(ss->cases.size());
          for (std::size_t k = ss->cases.size(); k-- > 0;) {  // List.fold_right
            A::t approx = rr.approx;
            auto [lam, r2] = simplify(env3, rr, ss->cases[k].action);
            sw[k] = StringCase{ss->cases[k].s, lam};
            rr = r2.meet_approx(env3, approx);
          }
          t def = ss->def;
          if (def) {
            A::t approx = rr.approx;
            auto [d, r2] = simplify(env3, rr, def);
            def = d;
            rr = r2.meet_approx(env3, approx);
          }
          return {string_switch(arg, slice(sw), def), ret(rr, A::value_unknown(A::other()))};
        }
        t branch = nullptr;
        for (const StringCase& c : ss->cases)
          if (c.s == *arg_string) {
            branch = c.action;
            break;
          }
        if (!branch) branch = ss->def ? ss->def : proved_unreachable();
        auto [b, r2] = simplify(env2, r, branch);
        return {b, r2.map_benefit(B::remove_branch)};
      });
    }
    case EK::Proved_unreachable: return {tree, ret(r, A::value_bottom())};
  }
  misc::fatal_error("Inline_and_simplify.simplify");
}

std::tuple<Slice<t>, std::vector<A::t>, R> Simplifier::simplify_list(const E& env, const R& r0, Slice<t> l) {
  // the tail first (the recursion), then the head
  std::vector<t> out(l.size());
  std::vector<A::t> approxs(l.size());
  R r = r0;
  bool same = true;
  for (std::size_t k = l.size(); k-- > 0;) {
    auto [h, r2] = simplify(env, r, l[k]);
    r = r2;
    out[k] = h;
    approxs[k] = r.approx;
    same = same && h == l[k];
  }
  return {same ? l : slice(out), approxs, r};
}

std::pair<const FunctionDeclaration*, variable::Map<SpecialisedTo>> Simplifier::duplicate_function(
    const E& env0, const SetOfClosures* set_of_closures, variable::t fun_var, variable::t new_fun_var) {
  const FunctionDeclaration* const* fd0 = set_of_closures->function_decls->funs.find_opt(fun_var);
  if (!fd0)
    fatal([&](Formatter& f) { fprintf(f, "duplicate_function: cannot find function %a", pr(variable::print, fun_var)); });
  E env = env0.set_never_inline().activate_freshening();
  inline_and_simplify_aux::PreparedSetOfClosures prep = inline_and_simplify_aux::prepare_to_simplify_set_of_closures(
      env, set_of_closures, set_of_closures->function_decls, false, *fd0);
  const FunctionDeclaration* const* fd = prep.function_decls->funs.find_opt(fun_var);
  if (!fd)
    fatal([&](Formatter& f) {
      fprintf(f, "duplicate_function: cannot find function %a (2)", pr(variable::print, fun_var));
    });
  const FunctionDeclaration* function_decl = *fd;
  E closure_env = inline_and_simplify_aux::prepare_to_simplify_closure(
      function_decl, prep.free_vars, prep.specialised_args, prep.parameter_approximations, prep.set_of_closures_env);
  E body_env = closure_env.enter_closure(fun_var, false, function_decl->dbg);
  if (!body_env.inside_set_of_closures_declaration(prep.function_decls->set_of_closures_origin))
    misc::fatal_error("Inline_and_simplify.duplicate_function");  // (assert)
  auto [body, r_] = simplify(body_env, R::create(), function_decl->body);
  const FunctionDeclaration* nfd = create_function_declaration(
      function_decl->params, body, function_decl->stub, function_decl->dbg, function_decl->inline_,
      function_decl->specialise, function_decl->is_a_functor, new_fun_var, function_decl->poll);
  return {nfd, prep.specialised_args};
}

A::t constant_defining_value_approx(const E& env, constant_defining_value c) {
  using K = ConstantDefiningValue::Kind;
  switch (c->kind) {
    case K::Allocated_const: return approx_for_allocated_const(c->c);
    case K::Block: {
      std::vector<A::t> fields;  // List.map: in order
      for (const BlockField& f : c->fields) {
        if (f.sym) {
          A::t a = env.find_symbol_opt(f.sym);
          fields.push_back(a ? a : A::value_unresolved(A::UnresolvedValue{nullptr, f.sym}));
        } else {
          fields.push_back(simplify_const(f.c));
        }
      }
      return A::value_block(c->tag, slice(fields));
    }
    case K::Set_of_closures: {
      // At toplevel, there is no freshening currently happening (this
      // cannot be the body of a currently inlined function), so we can keep
      // the original set_of_closures in the approximation.
      const SetOfClosures* s = c->set;
      if (!freshening::is_empty(env.freshening()) || !s->free_vars.is_empty() || !s->specialised_args.is_empty())
        misc::fatal_error("Inline_and_simplify.constant_defining_value_approx");  // (assert)
      const FunctionDeclarations* function_decls = s->function_decls;
      auto invariant_params = A::Lazy<variable::Map<variable::Set>>::of_fun(
          [function_decls] { return invariant_params::invariant_params_in_recursion(function_decls); });
      auto recursive = A::Lazy<variable::Set>::of_fun(
          [function_decls] { return find_recursive_functions::in_function_declarations(function_decls); });
      auto keep_body = inline_and_simplify_aux::keep_body_check(function_decls->is_classic_mode, recursive);
      const A::FunctionDeclarations* fd = A::function_declarations_approx(keep_body, function_decls);
      const A::ValueSetOfClosures* vsoc = A::create_value_set_of_closures(
          fd, {}, {}, invariant_params, recursive, {}, freshening::project_var::empty(), {});
      return A::value_set_of_closures(vsoc);
    }
    case K::Project_closure: {
      A::t set_of_closures_approx = env.find_symbol_opt(c->sym);
      if (!set_of_closures_approx) return A::value_unresolved(A::UnresolvedValue{nullptr, c->sym});
      using CK = A::CheckedSetOfClosures::Kind;
      A::CheckedSetOfClosures ch = A::check_approx_for_set_of_closures(set_of_closures_approx);
      switch (ch.kind) {
        case CK::Ok: return A::value_closure(ch.set, A::freshen_and_check_closure_id(ch.set, c->closure_id));
        case CK::Unresolved: return A::value_unresolved(ch.value);
        case CK::Unknown: return A::value_unknown(A::other());
        case CK::Unknown_because_of_unresolved_value: return A::value_unknown(A::UnknownBecauseOf{true, ch.value});
        case CK::Wrong:
          fatal([&](Formatter& f) {
            fprintf(f, "Wrong approximation for [Project_closure] when being used as a [constant_defining_value]: %a",
                    pr(print_constant_defining_value, c));
          });
      }
    }
  }
  return A::value_unknown(A::other());
}

// See documentation on [Let_rec_symbol] in flambda.mli.
E define_let_rec_symbol_approx(const E& orig_env, const std::vector<SymbolBinding>& defs) {
  // First declare an empty version of the symbols
  E init_env = orig_env;
  for (const SymbolBinding& d : defs)
    init_env = init_env.add_symbol(d.sym, A::value_unresolved(A::UnresolvedValue{nullptr, d.sym}));
  E lookup_env = init_env;
  for (int times = 2; times > 0; --times) {
    E env = orig_env;
    for (const SymbolBinding& d : defs) {
      A::t approx = constant_defining_value_approx(lookup_env, d.def);
      approx = A::augment_with_symbol(approx, d.sym);
      env = env.add_symbol(d.sym, approx);
    }
    lookup_env = env;
  }
  return lookup_env;
}

struct CdvResult {
  R r;
  constant_defining_value def;
  A::t approx;
};

CdvResult simplify_constant_defining_value(Simplifier& s, const E& env, const R& r, symbol::t sym,
                                           constant_defining_value c) {
  using K = ConstantDefiningValue::Kind;
  CdvResult res{r, c, nullptr};
  switch (c->kind) {
    // No simplifications are possible for [Allocated_const] or [Block].
    case K::Allocated_const: res.approx = approx_for_allocated_const(c->c); break;
    case K::Block: {
      std::vector<A::t> fields;
      for (const BlockField& f : c->fields) {
        if (f.sym) {
          A::t a = env.find_symbol_exn(f.sym);
          if (!a) misc::fatal_error("Inline_and_simplify.simplify_constant_defining_value");  // (Not_found)
          fields.push_back(a);
        } else {
          fields.push_back(simplify_const(f.c));
        }
      }
      res.approx = A::value_block(c->tag, slice(fields));
      break;
    }
    case K::Set_of_closures: {
      if (!c->set->free_vars.is_empty())
        fatal([&](Formatter& f) {
          fprintf(f, "Set of closures bound by [Let_symbol] is not closed: %a", pr(print_set_of_closures, c->set));
        });
      s.toplevel_set_of_closures = true;
      auto [soc, r2, fr] = s.simplify_set_of_closures(env, r, c->set);
      ConstantDefiningValue v{K::Set_of_closures};
      v.set = soc;
      res.r = r2;
      res.def = make<ConstantDefiningValue>(v);
      res.approx = r2.approx;
      break;
    }
    case K::Project_closure: {
      // No simplifications are necessary here.
      A::t set_of_closures_approx = env.find_symbol_exn(c->sym);
      if (!set_of_closures_approx) misc::fatal_error("Inline_and_simplify.simplify_constant_defining_value");
      using CK = A::CheckedSetOfClosures::Kind;
      A::CheckedSetOfClosures ch = A::check_approx_for_set_of_closures(set_of_closures_approx);
      switch (ch.kind) {
        case CK::Ok: res.approx = A::value_closure(ch.set, A::freshen_and_check_closure_id(ch.set, c->closure_id)); break;
        case CK::Unresolved: res.approx = A::value_unresolved(ch.value); break;
        case CK::Unknown: res.approx = A::value_unknown(A::other()); break;
        case CK::Unknown_because_of_unresolved_value:
          res.approx = A::value_unknown(A::UnknownBecauseOf{true, ch.value});
          break;
        case CK::Wrong:
          fatal([&](Formatter& f) {
            fprintf(f, "Wrong approximation for [Project_closure] when being used as a [constant_defining_value]: %a",
                    pr(print_constant_defining_value, c));
          });
      }
      break;
    }
  }
  res.approx = A::augment_with_symbol(res.approx, sym);
  res.r = ret(res.r, res.approx);
  return res;
}

std::pair<program_body, R> simplify_program_body(Simplifier& s, E env, R r, program_body program) {
  // each node's work before the rest's (the recursion); the program
  // rebuilt bottom-up
  struct Node {
    program_body orig;
    std::vector<SymbolBinding> defs;
    constant_defining_value def = nullptr;
    Slice<t> fields;
    t expr = nullptr;
  };
  std::vector<Node> nodes;
  auto bindings = [](Promoter& pr, std::vector<SymbolBinding>& bs) {
    for (SymbolBinding& b : bs) b.def = pr.cdv(b.def);
  };
  std::optional<Nursery::Roots> roots;
  if (s.nursery)
    roots.emplace(*s.nursery, [&](Promoter& pr) {
      env = pr.env(env);
      r = pr.result(r);
      for (Node& n : nodes) {
        bindings(pr, n.defs);
        n.def = pr.cdv(n.def);
        n.fields = pr.slice(n.fields, [&](t f) { return pr.expr(f); });
        n.expr = pr.expr(n.expr);
      }
    });
  program_body p = program;
  for (; p->kind != ProgramBody::Kind::End; p = p->body) {
    Node n{p, {}, nullptr, {}, nullptr};
    switch (p->kind) {
      case ProgramBody::Kind::Let_rec_symbol: {
        std::vector<SymbolBinding> set_of_closures_defs, other_defs;  // List.partition
        for (const SymbolBinding& b : p->defs)
          (b.def->kind == ConstantDefiningValue::Kind::Set_of_closures ? set_of_closures_defs : other_defs).push_back(b);
        std::vector<SymbolBinding> all(p->defs.begin(), p->defs.end());
        auto process_defs = [&](const E& lookup_env0, E building_env, const std::vector<SymbolBinding>& defs) {
          E lookup_env = lookup_env0;
          std::vector<SymbolBinding> out;  // (consed: reversed below)
          std::optional<Nursery::Roots> roots;
          if (s.nursery)
            roots.emplace(*s.nursery, [&](Promoter& pr) {
              lookup_env = pr.env(lookup_env);
              building_env = pr.env(building_env);
              bindings(pr, out);
            });
          for (const SymbolBinding& b : defs) {
            CdvResult cr = simplify_constant_defining_value(s, lookup_env, r, b.sym, b.def);
            r = cr.r;
            A::t approx = A::augment_with_symbol(cr.approx, b.sym);
            building_env = building_env.add_symbol(b.sym, approx);
            out.push_back({b.sym, cr.def});
          }
          return std::pair<E, std::vector<SymbolBinding>>{building_env,
                                                          std::vector<SymbolBinding>(out.rbegin(), out.rend())};
        };
        E lookup1 = define_let_rec_symbol_approx(env, all);
        auto [env1, socd] = process_defs(lookup1, env, set_of_closures_defs);
        E lookup2 = define_let_rec_symbol_approx(env1, other_defs);
        auto [env2, otd] = process_defs(lookup2, env1, other_defs);
        env = env2;
        n.defs = socd;
        n.defs.insert(n.defs.end(), otd.begin(), otd.end());
        break;
      }
      case ProgramBody::Kind::Let_symbol: {
        CdvResult cr = simplify_constant_defining_value(s, env, r, p->sym, p->def);
        r = cr.r;
        A::t approx = A::augment_with_symbol(cr.approx, p->sym);
        env = env.add_symbol(p->sym, approx);
        n.def = cr.def;
        break;
      }
      case ProgramBody::Kind::Initialize_symbol: {
        auto [fields, approxs, r2] = s.simplify_list(env, r, p->fields);
        r = r2;
        A::t approx = A::augment_with_symbol(A::value_block(p->tag, slice(approxs)), p->sym);
        env = env.add_symbol(p->sym, approx);
        n.fields = fields;
        break;
      }
      case ProgramBody::Kind::Effect: {
        auto [e, r2] = s.simplify(env, r, p->expr);
        r = r2;
        n.expr = e;
        break;
      }
      case ProgramBody::Kind::End: break;
    }
    nodes.push_back(std::move(n));
    if (s.nursery) s.nursery->point();
  }
  program_body body = end(p->sym);
  for (std::size_t k = nodes.size(); k-- > 0;) {
    const Node& n = nodes[k];
    switch (n.orig->kind) {
      case ProgramBody::Kind::Let_rec_symbol: body = let_rec_symbol(slice(n.defs), body); break;
      case ProgramBody::Kind::Let_symbol: body = let_symbol(n.orig->sym, n.def, body); break;
      case ProgramBody::Kind::Initialize_symbol: body = initialize_symbol(n.orig->sym, n.orig->tag, n.fields, body); break;
      case ProgramBody::Kind::Effect: body = effect(n.expr, body); break;
      case ProgramBody::Kind::End: break;
    }
  }
  return {body, r};
}

std::pair<Program, R> simplify_program(Simplifier& s, E env, R r, const Program& program) {
  program.imported_symbols.iter([&](symbol::t sym) {
    A::t approx = env.find_symbol_exn(sym);
    if (!approx) {
      // CR-someday mshinwell for mshinwell: Is there a reason we cannot use
      // [simplify_named_using_approx_and_env] here?
      approx = import_approx::import_symbol(sym);
      env = env.add_symbol(sym, approx);
    }
    r = ret(r, approx);
  });
  auto [program_body, r2] = simplify_program_body(s, env, r, program.program_body);
  return {Program{program.imported_symbols, program_body}, r2};
}

E add_predef_exns_to_environment(E env) {
  for (Ident::t predef_exn : predef::all_predef_exns()) {
    if (!ident::is_predef(predef_exn)) misc::fatal_error("Inline_and_simplify.add_predef_exns_to_environment");
    symbol::t sym = compilenv::symbol_for_global_prime(predef_exn);
    std::string_view name = ident::name(predef_exn);
    constexpr long object_tag = 248;  // Tag.object_tag
    A::t approx = A::value_block(object_tag, slice(std::vector<A::t>{A::value_string(static_cast<long>(name.size()), name),
                                                                     A::value_unknown(A::other())}));
    env = env.add_symbol(sym, A::augment_with_symbol(approx, sym));
  }
  return env;
}
}  // namespace

Program run(bool never_inline, const std::string& prefixname, long round, Formatter& ppf_dump, const Program& program) {
  R r = R::create();
  bool report = clflags::inlining_report;
  if (never_inline) clflags::inlining_report = false;
  std::optional<Nursery> nursery;
  const char* ev = std::getenv("CPPCAML_FLAMBDA_EVACUATE");
  if (!ev || std::string_view(ev) != "never") nursery.emplace(ev && std::string_view(ev) == "always");
  E initial_env = add_predef_exns_to_environment(E::create(never_inline, round, &ppf_dump));
  Simplifier s;
  if (nursery) s.nursery = &*nursery;
  auto [result0, r2] = simplify_program(s, initial_env, r, program);
  Program result = flambda_utils::introduce_needed_import_symbols(result0);
  if (!r2.used_static_exceptions.is_empty())
    fatal([&](Formatter& f) {
      fprintf(f, "Remaining static exceptions: %a@.%a@.", [&](Formatter& g) {
        auto elts = [&](Formatter& h) { r2.used_static_exceptions.iter([&](long e) { fprintf(h, "@ %d", e); }); };
        fprintf(g, "@[<1>{@[%a@ @]}@]", elts);
      }, pr(print_program, result));
    });
  if (clflags::inlining_report) inlining_stats::save_then_forget_decisions(prefixname + "." + std::to_string(round));
  clflags::inlining_report = report;
  if (nursery) {
    // the result out of the young zone
    Nursery::Roots root(*nursery, [&](Promoter& pr) { result = pr.program(result); });
    nursery->promote();
  }
  return result;
}

}  // namespace cppcaml::typing::inline_and_simplify
