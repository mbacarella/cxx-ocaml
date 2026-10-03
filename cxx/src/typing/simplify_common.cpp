// Port of middle_end/flambda/simplify_common.ml (see simplify_common.hpp).
#include "cppcaml/typing/simplify_common.hpp"

#include "cppcaml/typing/effect_analysis.hpp"

namespace cppcaml::typing::simplify_common {

namespace C = inlining_cost;

long swap16(long x) { return ((x & 0xff) << 8) | ((x >> 8) & 0xff); }
std::int32_t swap32(std::int32_t x) {
  return static_cast<std::int32_t>(__builtin_bswap32(static_cast<std::uint32_t>(x)));
}
std::int64_t swap64(std::int64_t x) {
  return static_cast<std::int64_t>(__builtin_bswap64(static_cast<std::uint64_t>(x)));
}


Result const_int_expr(flambda::named expr, long n) {
  if (effect_analysis::no_effects_named(expr)) {
    auto c = A::make_const_int_named(n);
    return {c.first, c.second, C::benefit::remove_code_named(expr, C::benefit::zero())};
  }
  return {expr, A::value_int(n), C::benefit::zero()};
}
Result const_char_expr(flambda::named expr, long c) {
  if (effect_analysis::no_effects_named(expr)) {
    auto r = A::make_const_char_named(c);
    return {r.first, r.second, C::benefit::remove_code_named(expr, C::benefit::zero())};
  }
  return {expr, A::value_char(c), C::benefit::zero()};
}
Result const_bool_expr(flambda::named expr, bool b) { return const_int_expr(expr, b ? 1 : 0); }
Result const_float_expr(flambda::named expr, double f) {
  if (effect_analysis::no_effects_named(expr)) {
    auto c = A::make_const_float_named(f);
    return {c.first, c.second, C::benefit::remove_code_named(expr, C::benefit::zero())};
  }
  return {expr, A::value_float(f), C::benefit::zero()};
}
Result const_boxed_int_expr(flambda::named expr, A::BoxedInt t, std::int64_t i) {
  if (effect_analysis::no_effects_named(expr)) {
    auto c = A::make_const_boxed_int_named(t, i);
    return {c.first, c.second, C::benefit::remove_code_named(expr, C::benefit::zero())};
  }
  return {expr, A::value_boxed_int(t, i), C::benefit::zero()};
}

Result const_integer_comparison_expr(flambda::named expr, lambda::IntegerComparison cmp, std::int64_t x,
                                     std::int64_t y) {
  // Using the [Stdlib] comparison functions here in the compiler coincides
  // with the definitions of such functions in the code compiled by the
  // user, and is thus correct.
  using C2 = lambda::IntegerComparison;
  bool r = false;
  switch (cmp) {
    case C2::Ceq: r = x == y; break;
    case C2::Cne: r = x != y; break;
    case C2::Clt: r = x < y; break;
    case C2::Cgt: r = x > y; break;
    case C2::Cle: r = x <= y; break;
    case C2::Cge: r = x >= y; break;
  }
  return const_bool_expr(expr, r);
}

Result const_float_comparison_expr(flambda::named expr, lambda::FloatComparison cmp, double x, double y) {
  using F = lambda::FloatComparison;
  bool r = false;
  switch (cmp) {
    case F::CFeq: r = x == y; break;
    case F::CFneq: r = !(x == y); break;
    case F::CFlt: r = x < y; break;
    case F::CFnlt: r = !(x < y); break;
    case F::CFgt: r = x > y; break;
    case F::CFngt: r = !(x > y); break;
    case F::CFle: r = x <= y; break;
    case F::CFnle: r = !(x <= y); break;
    case F::CFge: r = x >= y; break;
    case F::CFnge: r = !(x >= y); break;
  }
  return const_bool_expr(expr, r);
}

}  // namespace cppcaml::typing::simplify_common
