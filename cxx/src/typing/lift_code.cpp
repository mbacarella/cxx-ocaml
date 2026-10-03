// Port of middle_end/flambda/lift_code.ml (see lift_code.hpp).
#include "cppcaml/typing/lift_code.hpp"

#include <algorithm>

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

}  // namespace cppcaml::typing::lift_code
