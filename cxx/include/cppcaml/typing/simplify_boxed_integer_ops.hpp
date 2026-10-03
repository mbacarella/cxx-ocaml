// Port of middle_end/flambda/simplify_boxed_integer_ops.ml: constant
// folding of the operations on boxed integers (nativeint, int32, int64),
// with their widths' wrapping arithmetic.
#pragma once

#include "cppcaml/typing/simplify_common.hpp"

namespace cppcaml::typing::simplify_boxed_integer_ops {

namespace A = simple_value_approx;
using simplify_common::Result;

// Simplify_boxed_<kind>.simplify_unop / simplify_binop / simplify_binop_int
// (kind: the module's own; n, n1, n2 its values)
Result simplify_unop(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n);
Result simplify_binop(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n1,
                      std::int64_t n2);
Result simplify_binop_int(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n1,
                          long n2, long size_int);

}  // namespace cppcaml::typing::simplify_boxed_integer_ops
