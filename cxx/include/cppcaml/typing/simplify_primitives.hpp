// Port of middle_end/flambda/simplify_primitives.ml: a primitive
// application simplified from its arguments' approximations.
#pragma once

#include <vector>

#include "cppcaml/typing/simplify_common.hpp"

namespace cppcaml::typing::simplify_primitives {

namespace A = simple_value_approx;

// primitive p (args, approxs) expr dbg ~size_int
simplify_common::Result primitive(const clambda::Primitive& p, Slice<variable::t> args,
                                  const std::vector<A::t>& approxs, flambda::named expr, const debuginfo::t& dbg,
                                  long size_int);

}  // namespace cppcaml::typing::simplify_primitives
