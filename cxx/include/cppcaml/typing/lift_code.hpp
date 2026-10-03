// Port of middle_end/flambda/lift_code.ml (lifting_helper so far).
#pragma once

#include <functional>

#include "cppcaml/typing/flambda.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"

namespace cppcaml::typing::lift_code {

enum class EvaluationOrder : unsigned char { Left_to_right, Right_to_left };

// lifting_helper exprs ~evaluation_order ~create_body ~name: the
// expressions that are not variables let-bound (fresh variables named
// [name], created last to first) around [create_body vars]
flambda::t lifting_helper(const std::vector<flambda::t>& exprs, EvaluationOrder evaluation_order,
                          const std::function<flambda::t(Slice<variable::t>)>& create_body,
                          internal_variable_names::t name);

}  // namespace cppcaml::typing::lift_code
