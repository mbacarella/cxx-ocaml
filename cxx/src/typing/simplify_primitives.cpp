// Port of middle_end/flambda/simplify_primitives.ml (see
// simplify_primitives.hpp).
#include "cppcaml/typing/simplify_primitives.hpp"

#include <cmath>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/convert_primitives.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/simplify_boxed_integer_ops.hpp"

namespace cppcaml::typing::simplify_primitives {

namespace S = simplify_common;
namespace I = simplify_boxed_integer_ops;
namespace C = inlining_cost;
using K = clambda::Primitive::K;
using DK = A::DK;
using S::Result;

namespace {
bool phys_equal(const std::vector<A::t>& approxs) {
  if (approxs.size() != 2) misc::fatal_error("wrong number of arguments for equality");
  // N.B. The following would be incorrect if the variables are not bound
  // in the environment (simplify_primitives.ml)
  const auto& s1 = approxs[0]->symbol;
  const auto& s2 = approxs[1]->symbol;
  if (!s1 || !s2) return false;
  if (!s1->field && !s2->field) return symbol::equal(s1->sym, s2->sym);
  if (s1->field && s2->field) return symbol::equal(s1->sym, s2->sym) && *s1->field == *s2->field;
  return false;
}

bool is_known_to_be_some_kind_of_int(const A::Descr& d) { return d.kind == DK::Value_int || d.kind == DK::Value_char; }

bool is_known_to_be_some_kind_of_block(const A::Descr& d) {
  switch (d.kind) {
    case DK::Value_block: case DK::Value_float: case DK::Value_float_array: case DK::Value_boxed_int:
    case DK::Value_closure: case DK::Value_string: return true;
    default: return false;
  }
}

bool structurally_different(A::t a1, A::t a2) {
  const A::Descr& d1 = a1->descr;
  const A::Descr& d2 = a2->descr;
  if (d1.kind == DK::Value_int && d2.kind == DK::Value_int && d1.i != d2.i) return true;
  if (d1.kind == DK::Value_block && d2.kind == DK::Value_block) {
    if (d1.tag != d2.tag || d1.fields.size() != d2.fields.size()) return true;
    for (std::size_t k = 0; k < d1.fields.size(); ++k)  // Misc.Stdlib.Array.exists2
      if (structurally_different(d1.fields[k], d2.fields[k])) return true;
    return false;
  }
  // This is not very precise as this won't allow to distinguish blocks
  // from strings for instance.
  return (is_known_to_be_some_kind_of_int(d1) && is_known_to_be_some_kind_of_block(d2)) ||
         (is_known_to_be_some_kind_of_block(d1) && is_known_to_be_some_kind_of_int(d2));
}

bool phys_different(const std::vector<A::t>& approxs) {
  if (approxs.size() != 2) misc::fatal_error("wrong number of arguments for equality");
  return structurally_different(approxs[0], approxs[1]);
}

// OCaml's int shifts, as the native code computes them on the tagged
// representation (2x+1)
std::uint64_t tag(long x) { return static_cast<std::uint64_t>(x) * 2 + 1; }
long untag(std::uint64_t t) { return static_cast<long>(t) >> 1; }
long ocaml_lsl(long x, long y) { return untag(((tag(x) - 1) << y) + 1); }
long ocaml_lsr(long x, long y) { return untag((tag(x) >> y) | 1); }
long ocaml_asr(long x, long y) { return untag(static_cast<std::uint64_t>(static_cast<long>(tag(x)) >> y) | 1); }
// int_of_float: cvttsd2si (INT64_MIN when out of range or NaN), then tagged
long ocaml_int_of_float(double f) {
  std::int64_t v;
  if (std::isnan(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0) v = INT64_MIN;
  else v = static_cast<std::int64_t>(f);
  return S::wrap63(static_cast<std::uint64_t>(v));
}
int float_compare(double a, double b) {
  if (a < b) return -1;
  if (a > b) return 1;
  if (a == b) return 0;
  if (a != a) return b != b ? 0 : -1;
  return 1;
}
Result unknown(flambda::named expr) { return {expr, A::value_unknown(A::other()), C::benefit::zero()}; }
}  // namespace

Result primitive(const clambda::Primitive& p, Slice<variable::t> args, const std::vector<A::t>& approxs0,
                 flambda::named expr, const debuginfo::t& dbg, long size_int) {
  std::vector<A::t> approxs = approxs0;
  bool fpc = clflags::float_const_prop;
  if (p.kind == K::Pmakeblock && p.mut == MutableFlag::Immutable) {
    tag::t tg = tag::create_exn(p.n);
    std::vector<lambda::ValueKind> shape;
    if (!p.shape.some) shape.assign(args.size(), lambda::ValueKind::gen());
    else shape.assign(p.shape.kinds.begin(), p.shape.kinds.end());
    if (shape.size() != approxs.size()) misc::fatal_error("Invalid_argument(\"List.map2\")");
    // List.map2: in order
    for (std::size_t k = 0; k < approxs.size(); ++k) approxs[k] = A::augment_with_kind(approxs[k], shape[k]);
    for (std::size_t k = 0; k < approxs.size(); ++k) shape[k] = A::augment_kind_with_approx(approxs[k], shape[k]);
    clambda::Primitive mb = p;
    mb.shape.some = true;
    mb.shape.kinds = slice(shape);
    mb.id = clambda::fresh_uconstant_id();
    return {flambda::n_prim(mb, args, dbg), A::value_block(tg, slice(approxs)), C::benefit::zero()};
  }
  if (p.kind == K::Praise) return {expr, A::value_bottom(), C::benefit::zero()};
  if (p.kind == K::Pmakearray && approxs.empty()) {
    clambda::Primitive mb = clambda::prim(K::Pmakeblock);
    mb.n = 0;
    mb.mut = MutableFlag::Immutable;
    mb.shape.some = true;
    mb = CLAMBDA_PRIM_LITERAL(mb, "Pmakeblock(0, Immutable, Some [])");
    return {flambda::n_prim(mb, {}, dbg), A::value_block(tag::create_exn(0), {}), C::benefit::zero()};
  }
  if (p.kind == K::Pmakearray && p.array == lambda::ArrayKind::Pfloatarray) {
    if (p.mut == MutableFlag::Mutable)
      return {expr, A::value_mutable_float_array(static_cast<long>(args.size())), C::benefit::zero()};
    return {expr, A::value_immutable_float_array(slice(approxs)), C::benefit::zero()};
  }
  if (p.kind == K::Pintcomp && p.icmp == lambda::IntegerComparison::Ceq && phys_equal(approxs))
    return S::const_bool_expr(expr, true);
  if (p.kind == K::Pintcomp && p.icmp == lambda::IntegerComparison::Cne && phys_equal(approxs))
    return S::const_bool_expr(expr, false);
  // N.B. Having [not (phys_equal approxs)] would not on its own tell us
  // anything about whether the two values concerned are unequal.
  // (simplify_primitives.ml)
  if (p.kind == K::Pintcomp && p.icmp == lambda::IntegerComparison::Ceq && phys_different(approxs))
    return S::const_bool_expr(expr, false);
  if (p.kind == K::Pintcomp && p.icmp == lambda::IntegerComparison::Cne && phys_different(approxs))
    return S::const_bool_expr(expr, true);
  // If two values are structurally different we are certain they can never
  // be shared
  auto d = [&](std::size_t k) -> const A::Descr& { return approxs[k]->descr; };
  std::size_t n = approxs.size();
  if (n == 1 && d(0).kind == DK::Value_int) {
    long x = d(0).i;
    switch (p.kind) {
      case K::Pnot: return S::const_bool_expr(expr, x == 0);
      case K::Pnegint: return S::const_int_expr(expr, S::int_neg(x));
      case K::Pbswap16: return S::const_int_expr(expr, S::swap16(x));
      case K::Pisint: return S::const_bool_expr(expr, true);
      case K::Poffsetint: return S::const_int_expr(expr, S::int_add(x, p.n));
      case K::Pfloatofint:
        if (fpc) return S::const_float_expr(expr, static_cast<double>(x));
        break;
      case K::Pbintofint:
        switch (p.bi) {
          case BoxedInteger::Pnativeint: return S::const_boxed_int_expr(expr, A::BoxedInt::Nativeint, x);
          case BoxedInteger::Pint32:
            return S::const_boxed_int_expr(expr, A::BoxedInt::Int32,
                                           static_cast<std::int32_t>(static_cast<std::uint32_t>(x)));
          case BoxedInteger::Pint64: return S::const_boxed_int_expr(expr, A::BoxedInt::Int64, x);
        }
        break;
      default: break;
    }
    return unknown(expr);
  }
  if (n == 2 && d(0).kind == DK::Value_int && d(1).kind == DK::Value_int) {
    long x = d(0).i, y = d(1).i;
    bool shift_precond = 0 <= y && y < 8 * size_int;
    switch (p.kind) {
      case K::Paddint: return S::const_int_expr(expr, S::int_add(x, y));
      case K::Psubint: return S::const_int_expr(expr, S::int_sub(x, y));
      case K::Pmulint: return S::const_int_expr(expr, S::int_mul(x, y));
      case K::Pdivint:
        if (y != 0) return S::const_int_expr(expr, S::wrap63(static_cast<std::uint64_t>(x / y)));
        break;
      case K::Pmodint:
        if (y != 0) return S::const_int_expr(expr, x % y);
        break;
      case K::Pandint: return S::const_int_expr(expr, x & y);
      case K::Porint: return S::const_int_expr(expr, x | y);
      case K::Pxorint: return S::const_int_expr(expr, x ^ y);
      case K::Plslint:
        if (shift_precond) return S::const_int_expr(expr, ocaml_lsl(x, y));
        break;
      case K::Plsrint:
        if (shift_precond) return S::const_int_expr(expr, ocaml_lsr(x, y));
        break;
      case K::Pasrint:
        if (shift_precond) return S::const_int_expr(expr, ocaml_asr(x, y));
        break;
      case K::Pintcomp: return S::const_integer_comparison_expr(expr, p.icmp, x, y);
      case K::Pcompare_ints: return S::const_int_expr(expr, x < y ? -1 : x > y ? 1 : 0);
      case K::Pisout: return S::const_bool_expr(expr, y > x || y < 0);
      default: break;
    }
    return unknown(expr);
  }
  if (n == 2 && d(0).kind == DK::Value_char && d(1).kind == DK::Value_char) {
    long x = d(0).i, y = d(1).i;
    if (p.kind == K::Pintcomp) return S::const_integer_comparison_expr(expr, p.icmp, x, y);
    if (p.kind == K::Pcompare_ints) return S::const_int_expr(expr, x - y);  // Char.compare
    return unknown(expr);
  }
  if (n == 1 && d(0).kind == DK::Value_float && d(0).f && fpc) {
    double x = *d(0).f;
    switch (p.kind) {
      case K::Pintoffloat: return S::const_int_expr(expr, ocaml_int_of_float(x));
      case K::Pnegfloat: return S::const_float_expr(expr, -x);
      case K::Pabsfloat: return S::const_float_expr(expr, std::fabs(x));
      default: return unknown(expr);
    }
  }
  if (n == 2 && d(0).kind == DK::Value_float && d(0).f && d(1).kind == DK::Value_float && d(1).f && fpc) {
    double n1 = *d(0).f, n2 = *d(1).f;
    switch (p.kind) {
      case K::Paddfloat: return S::const_float_expr(expr, n1 + n2);
      case K::Psubfloat: return S::const_float_expr(expr, n1 - n2);
      case K::Pmulfloat: return S::const_float_expr(expr, n1 * n2);
      case K::Pdivfloat: return S::const_float_expr(expr, n1 / n2);
      case K::Pfloatcomp: return S::const_float_comparison_expr(expr, p.fcmp, n1, n2);
      case K::Pcompare_floats: return S::const_int_expr(expr, float_compare(n1, n2));
      default: return unknown(expr);
    }
  }
  if (n == 1 && d(0).kind == DK::Value_boxed_int) return I::simplify_unop(p, d(0).bi, expr, d(0).bival);
  if (n == 2 && d(0).kind == DK::Value_boxed_int && d(1).kind == DK::Value_boxed_int && d(0).bi == d(1).bi)
    return I::simplify_binop(p, d(0).bi, expr, d(0).bival, d(1).bival);
  if (n == 2 && d(0).kind == DK::Value_boxed_int && d(1).kind == DK::Value_int)
    return I::simplify_binop_int(p, d(0).bi, expr, d(0).bival, d(1).i, size_int);
  if (n == 1 && d(0).kind == DK::Value_block && p.kind == K::Pisint) return S::const_bool_expr(expr, false);
  if (n == 1 && d(0).kind == DK::Value_string && (p.kind == K::Pstringlength || p.kind == K::Pbyteslength))
    return S::const_int_expr(expr, d(0).str.size);
  if (n == 2 && d(0).kind == DK::Value_string && d(1).kind == DK::Value_int) {
    long size = d(0).str.size, x = d(1).i;
    if (d(0).str.contents && x >= 0 && x < size) {
      if (p.kind == K::Pstringrefu || p.kind == K::Pstringrefs || p.kind == K::Pbytesrefu || p.kind == K::Pbytesrefs)
        return S::const_char_expr(flambda::n_prim(clambda::prim(K::Pstringrefu), args, dbg),
                                  static_cast<unsigned char>((*d(0).str.contents)[static_cast<std::size_t>(x)]));
      return unknown(expr);
    }
    if (!d(0).str.contents && x >= 0 && x < size && p.kind == K::Pstringrefs)
      // we improved it, but there is no way to account for that:
      return {flambda::n_prim(clambda::prim(K::Pstringrefu), args, dbg), A::value_unknown(A::other()),
              C::benefit::zero()};
    if (!d(0).str.contents && x >= 0 && x < size && p.kind == K::Pbytesrefs)
      return {flambda::n_prim(clambda::prim(K::Pbytesrefu), args, dbg), A::value_unknown(A::other()),
              C::benefit::zero()};
  }
  if (n == 1 && d(0).kind == DK::Value_float_array) {
    const A::ValueFloatArray& fa = d(0).float_array;
    if (p.kind == K::Parraylength) return S::const_int_expr(expr, fa.size);
    if (p.kind == K::Pfloatfield) {
      long i = p.n;
      if (fa.contents_known && i >= 0 && i < fa.size) {
        A::t a = fa.contents[static_cast<std::size_t>(i)];
        std::optional<double> v = A::check_approx_for_float(a);
        if (!v) return {expr, a, C::benefit::zero()};
        return S::const_float_expr(expr, *v);
      }
      return unknown(expr);
    }
    return unknown(expr);
  }
  if (semantics_of_primitives::return_type_of_primitive(p) == semantics_of_primitives::ReturnType::Float)
    return {expr, A::value_any_float(), C::benefit::zero()};
  return unknown(expr);
}

}  // namespace cppcaml::typing::simplify_primitives
