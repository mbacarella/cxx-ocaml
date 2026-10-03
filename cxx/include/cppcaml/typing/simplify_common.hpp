// Port of middle_end/flambda/simplify_common.ml: a primitive application
// replaced by a constant, with the benefit of the removed code.  OCaml's
// `int` arithmetic is 63-bit (ocaml_int helpers).
#pragma once

#include <cstdint>
#include <tuple>

#include "cppcaml/typing/inlining_cost.hpp"
#include "cppcaml/typing/simple_value_approx.hpp"

namespace cppcaml::typing::simplify_common {

namespace A = simple_value_approx;
using Result = std::tuple<flambda::named, A::t, inlining_cost::Benefit>;

// OCaml's native int: 63 bits, arithmetic wrapping
inline long wrap63(std::uint64_t x) { return static_cast<long>(x << 1) >> 1; }
inline long int_add(long a, long b) { return wrap63(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b)); }
inline long int_sub(long a, long b) { return wrap63(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)); }
inline long int_mul(long a, long b) { return wrap63(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)); }
inline long int_neg(long a) { return wrap63(0 - static_cast<std::uint64_t>(a)); }

long swap16(long x);           // %bswap16
std::int32_t swap32(std::int32_t x);
std::int64_t swap64(std::int64_t x);

Result const_int_expr(flambda::named expr, long n);
Result const_char_expr(flambda::named expr, long c);
Result const_bool_expr(flambda::named expr, bool b);
Result const_float_expr(flambda::named expr, double f);
Result const_boxed_int_expr(flambda::named expr, A::BoxedInt t, std::int64_t i);
Result const_integer_comparison_expr(flambda::named expr, lambda::IntegerComparison cmp, std::int64_t x, std::int64_t y);
Result const_float_comparison_expr(flambda::named expr, lambda::FloatComparison cmp, double x, double y);

}  // namespace cppcaml::typing::simplify_common
