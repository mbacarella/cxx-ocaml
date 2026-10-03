// Port of middle_end/flambda/simplify_boxed_integer_ops.ml (see
// simplify_boxed_integer_ops.hpp).  An Int32 is kept sign-extended in the
// int64; every result is normalized to its kind's width.
#include "cppcaml/typing/simplify_boxed_integer_ops.hpp"

namespace cppcaml::typing::simplify_boxed_integer_ops {

namespace S = simplify_common;
namespace C = inlining_cost;
using K = clambda::Primitive::K;

namespace {
BoxedInteger lambda_kind(A::BoxedInt k) {
  switch (k) {
    case A::BoxedInt::Int32: return BoxedInteger::Pint32;
    case A::BoxedInt::Int64: return BoxedInteger::Pint64;
    case A::BoxedInt::Nativeint: return BoxedInteger::Pnativeint;
  }
  return BoxedInteger::Pnativeint;
}
std::int64_t norm(A::BoxedInt k, std::uint64_t v) {
  if (k == A::BoxedInt::Int32) return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
  return static_cast<std::int64_t>(v);
}
Result unknown(flambda::named expr) { return {expr, A::value_unknown(A::other()), C::benefit::zero()}; }
}  // namespace

Result simplify_unop(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n) {
  BoxedInteger lk = lambda_kind(kind);
  auto eval = [&](std::int64_t v) { return S::const_boxed_int_expr(expr, kind, v); };
  switch (p.kind) {
    case K::Pintofbint:
      // I.to_int: the low 63 bits (an int32 sign-extended)
      if (p.bi == lk) return S::const_int_expr(expr, S::wrap63(static_cast<std::uint64_t>(n)));
      break;
    case K::Pcvtbint:
      if (p.bi == lk) {
        if (p.bi2 == BoxedInteger::Pint32)
          return S::const_boxed_int_expr(expr, A::BoxedInt::Int32, norm(A::BoxedInt::Int32, static_cast<std::uint64_t>(n)));
        if (p.bi2 == BoxedInteger::Pint64) return S::const_boxed_int_expr(expr, A::BoxedInt::Int64, n);
      }
      break;
    case K::Pnegbint:
      if (p.bi == lk) return eval(norm(kind, 0 - static_cast<std::uint64_t>(n)));
      break;
    case K::Pbbswap:
      if (p.bi == lk)
        return eval(kind == A::BoxedInt::Int32 ? S::swap32(static_cast<std::int32_t>(n)) : S::swap64(n));
      break;
    default: break;
  }
  return unknown(expr);
}

Result simplify_binop(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n1,
                      std::int64_t n2) {
  BoxedInteger lk = lambda_kind(kind);
  auto eval = [&](std::uint64_t v) { return S::const_boxed_int_expr(expr, kind, norm(kind, v)); };
  auto u1 = static_cast<std::uint64_t>(n1), u2 = static_cast<std::uint64_t>(n2);
  bool non_zero = n2 != 0;
  // the minimum of the kind divided by -1: no trap (OCaml's div / rem)
  auto div = [&]() -> std::uint64_t { return n2 == -1 ? 0 - u1 : static_cast<std::uint64_t>(n1 / n2); };
  auto rem = [&]() -> std::uint64_t { return n2 == -1 ? 0 : static_cast<std::uint64_t>(n1 % n2); };
  if (p.bi != lk) return unknown(expr);
  switch (p.kind) {
    case K::Paddbint: return eval(u1 + u2);
    case K::Psubbint: return eval(u1 - u2);
    case K::Pmulbint: return eval(u1 * u2);
    case K::Pdivbint:
      if (non_zero) return eval(div());
      break;
    case K::Pmodbint:
      if (non_zero) return eval(rem());
      break;
    case K::Pandbint: return eval(u1 & u2);
    case K::Porbint: return eval(u1 | u2);
    case K::Pxorbint: return eval(u1 ^ u2);
    case K::Pbintcomp: return S::const_integer_comparison_expr(expr, p.icmp, n1, n2);
    case K::Pcompare_bints: return S::const_int_expr(expr, n1 < n2 ? -1 : n1 > n2 ? 1 : 0);
    default: break;
  }
  return unknown(expr);
}

Result simplify_binop_int(const clambda::Primitive& p, A::BoxedInt kind, flambda::named expr, std::int64_t n1,
                          long n2, long size_int) {
  BoxedInteger lk = lambda_kind(kind);
  bool precond = 0 <= n2 && n2 < 8 * size_int;
  if (p.bi != lk || !precond) return unknown(expr);
  auto eval = [&](std::uint64_t v) { return S::const_boxed_int_expr(expr, kind, norm(kind, v)); };
  // (ocamlopt's own Int32 shifts: on the 64-bit register, the count below
  // 64 here, then sign-extended from 32 bits; Int64 / Nativeint: 64-bit)
  unsigned sh = static_cast<unsigned>(n2);
  switch (p.kind) {
    case K::Plslbint: return eval(static_cast<std::uint64_t>(n1) << sh);
    case K::Plsrbint:
      if (kind == A::BoxedInt::Int32) return eval(static_cast<std::uint64_t>(static_cast<std::uint32_t>(n1)) >> sh);
      return eval(static_cast<std::uint64_t>(n1) >> sh);
    case K::Pasrbint: return eval(static_cast<std::uint64_t>(n1 >> sh));
    default: break;
  }
  return unknown(expr);
}

}  // namespace cppcaml::typing::simplify_boxed_integer_ops
