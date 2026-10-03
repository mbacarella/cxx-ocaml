// Port of middle_end/flambda/lift_code.ml (see lift_code.hpp).
#include "cppcaml/typing/lift_code.hpp"

#include <algorithm>

#include "cppcaml/typing/flambda_iterators.hpp"

namespace cppcaml::typing::lift_code {

flambda::t lifting_helper(const std::vector<flambda::t>& exprs, EvaluationOrder evaluation_order,
                          const std::function<flambda::t(Slice<variable::t>)>& create_body,
                          internal_variable_names::t name) {
  // [vars] corresponds elementwise to [exprs]; the order is unchanged.
  // (List.fold_right: the last expression first)
  std::vector<variable::t> vars(exprs.size());
  struct Bound {
    variable::t v;
    flambda::t expr;
  };
  std::vector<Bound> lets;  // `(v, expr) :: lets` built back to front: lets[0] is the first expression's
  for (std::size_t k = exprs.size(); k-- > 0;) {
    if (auto* v = flambda::as<flambda::Var>(exprs[k])) {
      // Note that [v] is (statically) always an immutable variable.
      vars[k] = v->var;
    } else {
      variable::t v2 = variable::create(name, compilation_unit::get_current_exn());
      vars[k] = v2;
      lets.push_back({v2, exprs[k]});
    }
  }
  std::reverse(lets.begin(), lets.end());
  if (evaluation_order == EvaluationOrder::Left_to_right) std::reverse(lets.begin(), lets.end());
  flambda::t body = create_body(slice(vars));
  for (const Bound& b : lets) body = flambda::create_let(b.v, flambda::n_expr(b.expr), body);
  return body;
}

namespace {
using namespace flambda;
namespace W = flambda::with_free_variables;

// def = Immutable of Variable.t * Flambda.named With_free_variables.t
//     | Mutable of Mutable_variable.t * Variable.t * Lambda.value_kind
struct Def {
  bool is_mutable;
  variable::t var;
  WithFvNamed named_;                // Immutable
  variable::t initial_value = nullptr;  // Mutable
  lambda::ValueKind contents_kind;      // Mutable
};
Def immutable(variable::t v, WithFvNamed n) { return {false, v, n, nullptr, {}}; }

// The defs accumulate as a vector whose back is the list's head (the
// innermost binding).
WithFvExpr extract(std::vector<Def>& acc, WithFvExpr expr);
WithFvExpr extract_let_mutable(std::vector<Def>& acc, const Let_mutable* let_mut);

WithFvExpr extract_let_expr(std::vector<Def>& acc, const Let* let_expr) {
  named d = let_expr->defining_expr;
  const Let* let2 = nullptr;
  const Let_mutable* let_mut = nullptr;
  if (auto* e = as<NExpr>(d)) {
    let2 = as<Let>(e->expr);
    let_mut = as<Let_mutable>(e->expr);
  }
  if (let2) {
    WithFvExpr body2 = extract_let_expr(acc, let2);
    acc.push_back(immutable(let_expr->var, W::expr(body2)));
  } else if (let_mut) {
    WithFvExpr body2 = extract_let_mutable(acc, let_mut);
    acc.push_back(immutable(let_expr->var, W::expr(body2)));
  } else {
    acc.push_back(immutable(let_expr->var, W::of_defining_expr_of_let(let_expr)));
  }
  return extract(acc, W::of_body_of_let(let_expr));
}

WithFvExpr extract_let_mutable(std::vector<Def>& acc, const Let_mutable* let_mut) {
  acc.push_back({true, let_mut->var, {}, let_mut->initial_value, let_mut->contents_kind});
  return extract(acc, W::of_expr(let_mut->body));
}

WithFvExpr extract(std::vector<Def>& acc, WithFvExpr expr) {
  if (auto* l = as<Let>(expr.expr)) return extract_let_expr(acc, l);
  if (auto* m = as<Let_mutable>(expr.expr)) return extract_let_mutable(acc, m);
  return expr;
}

t rebuild_let(const std::vector<Def>& mapped, t body) {
  // List.fold_left over the defs (the innermost first)
  for (std::size_t k = mapped.size(); k-- > 0;) {
    const Def& d = mapped[k];
    if (d.is_mutable) body = let_mutable(d.var, d.initial_value, d.contents_kind, body);
    else body = W::create_let_reusing_defining_expr(d.var, d.named_, body);
  }
  return body;
}

named lift_lets_named(named n, bool toplevel);

Def lift_lets_def(const Def& def, bool toplevel) {
  if (def.is_mutable) return def;
  named n = def.named_.n;
  if (auto* e = as<NExpr>(n)) return immutable(def.var, W::expr(W::of_expr(lift_lets_expr(e->expr, toplevel))));
  if (auto* s = as<NSet_of_closures>(n); s && !toplevel)
    return immutable(def.var, W::of_named(n_set_of_closures(flambda_iterators::map_function_bodies(
                                  s->set, [&](t b) { return lift_lets_expr(b, toplevel); }))));
  return def;
}

named lift_lets_named(named n, bool toplevel) {
  if (auto* e = as<NExpr>(n)) return n_expr(lift_lets_expr(e->expr, toplevel));
  if (auto* s = as<NSet_of_closures>(n); s && !toplevel)
    return n_set_of_closures(
        flambda_iterators::map_function_bodies(s->set, [&](t b) { return lift_lets_expr(b, toplevel); }));
  return n;
}

t lift_defs(std::vector<Def>& defs, WithFvExpr body, bool toplevel) {
  // List.rev_map (lift_lets_def ~toplevel) defs: the head (the innermost)
  // first
  std::vector<Def> mapped(defs.size());
  for (std::size_t k = defs.size(); k-- > 0;) mapped[k] = lift_lets_def(defs[k], toplevel);
  t b = lift_lets_expr(body.expr, toplevel);
  return rebuild_let(mapped, b);
}
}  // namespace

t lift_lets_expr(t expr, bool toplevel) {
  if (auto* l = as<Let>(expr)) {
    std::vector<Def> defs;
    WithFvExpr body = extract_let_expr(defs, l);
    return lift_defs(defs, body, toplevel);
  }
  if (auto* m = as<Let_mutable>(expr)) {
    std::vector<Def> defs;
    WithFvExpr body = extract_let_mutable(defs, m);
    return lift_defs(defs, body, toplevel);
  }
  return flambda_iterators::map_subexpressions([&](t e) { return lift_lets_expr(e, toplevel); },
                                               [&](variable::t, named n) { return lift_lets_named(n, toplevel); },
                                               expr);
}

Program lift_lets(const Program& program) {
  return flambda_iterators::map_exprs_at_toplevel_of_program(program,
                                                             [](t e) { return lift_lets_expr(e, false); });
}

}  // namespace cppcaml::typing::lift_code
