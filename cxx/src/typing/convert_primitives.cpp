// Port of middle_end/convert_primitives.ml (Lambda primitives to Clambda
// primitives) and middle_end/semantics_of_primitives.ml.
#include "cppcaml/typing/convert_primitives.hpp"

#include <stdexcept>

#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing::convert_primitives {

using CK = clambda::Primitive::K;
using LK = lambda::Primitive::K;

namespace {
// the fields both representations share, copied over
clambda::Primitive with_fields(CK k, const lambda::Primitive& p) {
  clambda::Primitive c{k};
  c.n = p.n;
  c.mut = p.mut;
  c.shape = p.shape;
  c.lazy_tag = p.lazy_tag;
  c.ptr = p.ptr;
  c.init = p.init;
  c.repr = p.repr;
  c.ccall = p.ccall;
  c.raise = p.raise;
  c.safe = p.safe;
  c.icmp = p.icmp;
  c.fcmp = p.fcmp;
  c.array = p.array;
  c.bi = p.bi;
  c.bi2 = p.bi2;
  c.unsafe = p.unsafe;
  c.ba_kind = p.ba_kind;
  c.ba_layout = p.ba_layout;
  return c;
}
// Pstring_load_16 is_unsafe -> Pstring_load (Sixteen, convert_unsafety is_unsafe)
clambda::Primitive sized(CK k, clambda::MemoryAccessSize size, const lambda::Primitive& p) {
  clambda::Primitive c{k};
  c.size = size;
  c.safe = p.unsafe ? lambda::IsSafe::Unsafe : lambda::IsSafe::Safe;
  return c;
}
}  // namespace

clambda::Primitive convert(const lambda::Primitive& prim) {
  using S = clambda::MemoryAccessSize;
  switch (prim.kind) {
#define SAME(N) \
  case LK::N: return with_fields(CK::N, prim);
    SAME(Pmakeblock) SAME(Pmakelazyblock) SAME(Pfield) SAME(Pfield_computed) SAME(Psetfield)
    SAME(Psetfield_computed) SAME(Pfloatfield) SAME(Psetfloatfield) SAME(Pduprecord) SAME(Prunstack)
    SAME(Pperform) SAME(Presume) SAME(Preperform) SAME(Pccall) SAME(Praise) SAME(Psequand) SAME(Psequor)
    SAME(Pnot) SAME(Pnegint) SAME(Paddint) SAME(Psubint) SAME(Pmulint) SAME(Pdivint) SAME(Pmodint)
    SAME(Pandint) SAME(Porint) SAME(Pxorint) SAME(Plslint) SAME(Plsrint) SAME(Pasrint) SAME(Pintcomp)
    SAME(Pcompare_ints) SAME(Pcompare_floats) SAME(Pcompare_bints) SAME(Poffsetint) SAME(Poffsetref)
    SAME(Pintoffloat) SAME(Pfloatofint) SAME(Pnegfloat) SAME(Pabsfloat) SAME(Paddfloat) SAME(Psubfloat)
    SAME(Pmulfloat) SAME(Pdivfloat) SAME(Pfloatcomp) SAME(Pstringlength) SAME(Pstringrefu) SAME(Pstringrefs)
    SAME(Pbyteslength) SAME(Pbytesrefu) SAME(Pbytessetu) SAME(Pbytesrefs) SAME(Pbytessets) SAME(Pmakearray)
    SAME(Pduparray) SAME(Parraylength) SAME(Parrayrefu) SAME(Parraysetu) SAME(Parrayrefs) SAME(Parraysets)
    SAME(Pisint) SAME(Pisout) SAME(Pcvtbint) SAME(Pnegbint) SAME(Paddbint) SAME(Psubbint)
    SAME(Pmulbint) SAME(Pbintofint) SAME(Pintofbint) SAME(Pandbint) SAME(Porbint) SAME(Pxorbint)
    SAME(Plslbint) SAME(Plsrbint) SAME(Pasrbint) SAME(Pbbswap) SAME(Pdivbint) SAME(Pmodbint) SAME(Pbintcomp)
    SAME(Pbigarrayref) SAME(Pbigarrayset) SAME(Pbigarraydim) SAME(Pbswap16) SAME(Pint_as_pointer)
    SAME(Patomic_load) SAME(Popaque) SAME(Pdls_get) SAME(Ppoll)
#undef SAME
    case LK::Pstring_load_16: return sized(CK::Pstring_load, S::Sixteen, prim);
    case LK::Pstring_load_32: return sized(CK::Pstring_load, S::Thirty_two, prim);
    case LK::Pstring_load_64: return sized(CK::Pstring_load, S::Sixty_four, prim);
    case LK::Pbytes_load_16: return sized(CK::Pbytes_load, S::Sixteen, prim);
    case LK::Pbytes_load_32: return sized(CK::Pbytes_load, S::Thirty_two, prim);
    case LK::Pbytes_load_64: return sized(CK::Pbytes_load, S::Sixty_four, prim);
    case LK::Pbytes_set_16: return sized(CK::Pbytes_set, S::Sixteen, prim);
    case LK::Pbytes_set_32: return sized(CK::Pbytes_set, S::Thirty_two, prim);
    case LK::Pbytes_set_64: return sized(CK::Pbytes_set, S::Sixty_four, prim);
    case LK::Pbigstring_load_16: return sized(CK::Pbigstring_load, S::Sixteen, prim);
    case LK::Pbigstring_load_32: return sized(CK::Pbigstring_load, S::Thirty_two, prim);
    case LK::Pbigstring_load_64: return sized(CK::Pbigstring_load, S::Sixty_four, prim);
    case LK::Pbigstring_set_16: return sized(CK::Pbigstring_set, S::Sixteen, prim);
    case LK::Pbigstring_set_32: return sized(CK::Pbigstring_set, S::Thirty_two, prim);
    case LK::Pbigstring_set_64: return sized(CK::Pbigstring_set, S::Sixty_four, prim);
    case LK::Pbytes_to_string:
    case LK::Pbytes_of_string:
    case LK::Pctconst:
    case LK::Pignore:
    case LK::Pgetglobal:
    case LK::Psetglobal:
      break;
  }
  throw std::logic_error("lambda primitive " + printlambda::name_of_primitive(prim) +
                         " can't be converted to clambda primitive");
}

}  // namespace cppcaml::typing::convert_primitives

namespace cppcaml::typing::semantics_of_primitives {

using CK = clambda::Primitive::K;

std::pair<Effects, Coeffects> for_primitive(const clambda::Primitive& prim) {
  using E = Effects;
  using C = Coeffects;
  using lambda::IsSafe;
  switch (prim.kind) {
    case CK::Pmakeblock:
    case CK::Pmakelazyblock:
      return {E::Only_generative_effects, C::No_coeffects};
    case CK::Pmakearray:
      if (prim.mut == MutableFlag::Mutable) return {E::Only_generative_effects, C::No_coeffects};
      return {E::No_effects, C::No_coeffects};
    case CK::Pduparray:
      // Pduparray (_, Immutable) is allowed only on immutable arrays.
      if (prim.mut == MutableFlag::Immutable) return {E::No_effects, C::No_coeffects};
      return {E::Only_generative_effects, C::Has_coeffects};
    case CK::Pduprecord: return {E::Only_generative_effects, C::Has_coeffects};
    case CK::Pccall: {
      std::string_view n = prim.ccall->prim_name;
      if (n == "caml_format_float" || n == "caml_format_int" || n == "caml_int32_format" ||
          n == "caml_nativeint_format" || n == "caml_int64_format")
        return {E::No_effects, C::No_coeffects};
      return {E::Arbitrary_effects, C::Has_coeffects};
    }
    case CK::Praise: return {E::Arbitrary_effects, C::No_coeffects};
    case CK::Prunstack:
    case CK::Pperform:
    case CK::Presume:
    case CK::Preperform: return {E::Arbitrary_effects, C::Has_coeffects};
    case CK::Pnot: case CK::Pnegint: case CK::Paddint: case CK::Psubint: case CK::Pmulint: case CK::Pandint:
    case CK::Porint: case CK::Pxorint: case CK::Plslint: case CK::Plsrint: case CK::Pasrint:
    case CK::Pintcomp:
    case CK::Pcompare_ints: case CK::Pcompare_floats: case CK::Pcompare_bints:
      return {E::No_effects, C::No_coeffects};
    case CK::Pdivbint: case CK::Pmodbint: case CK::Pdivint: case CK::Pmodint:
      // Unsafe: will not raise [Division_by_zero].
      if (prim.safe == IsSafe::Unsafe) return {E::No_effects, C::No_coeffects};
      return {E::Arbitrary_effects, C::No_coeffects};
    case CK::Poffsetint: return {E::No_effects, C::No_coeffects};
    case CK::Poffsetref: return {E::Arbitrary_effects, C::Has_coeffects};
    case CK::Pintoffloat: case CK::Pfloatofint: case CK::Pnegfloat: case CK::Pabsfloat: case CK::Paddfloat:
    case CK::Psubfloat: case CK::Pmulfloat: case CK::Pdivfloat: case CK::Pfloatcomp:
    case CK::Pstringlength: case CK::Pbyteslength: case CK::Parraylength:
    case CK::Pisint: case CK::Pisout: case CK::Pbintofint: case CK::Pintofbint: case CK::Pcvtbint:
    case CK::Pnegbint: case CK::Paddbint: case CK::Psubbint: case CK::Pmulbint: case CK::Pandbint:
    case CK::Porbint: case CK::Pxorbint: case CK::Plslbint: case CK::Plsrbint: case CK::Pasrbint:
    case CK::Pbintcomp:
      return {E::No_effects, C::No_coeffects};
    case CK::Pbigarraydim:
      return {E::No_effects, C::Has_coeffects};  // Some people resize bigarrays in place.
    case CK::Pread_symbol: case CK::Pfield: case CK::Pfield_computed: case CK::Pfloatfield:
    case CK::Parrayrefu: case CK::Pstringrefu: case CK::Pbytesrefu:
      return {E::No_effects, C::Has_coeffects};
    case CK::Pstring_load: case CK::Pbytes_load: case CK::Pbigstring_load:
      if (prim.safe == IsSafe::Unsafe) return {E::No_effects, C::Has_coeffects};
      return {E::Arbitrary_effects, C::Has_coeffects};  // May trigger a bounds check exception.
    case CK::Pbigarrayref:
      if (prim.unsafe) return {E::No_effects, C::Has_coeffects};
      return {E::Arbitrary_effects, C::Has_coeffects};
    case CK::Parrayrefs: case CK::Pstringrefs: case CK::Pbytesrefs:
      return {E::Arbitrary_effects, C::Has_coeffects};
    case CK::Psetfield: case CK::Psetfield_computed: case CK::Psetfloatfield: case CK::Patomic_load:
    case CK::Parraysetu: case CK::Parraysets: case CK::Pbytessetu:
    case CK::Pbytessets: case CK::Pbytes_set: case CK::Pbigarrayset: case CK::Pbigstring_set:
      // Whether or not some of these are "unsafe" is irrelevant; they
      // always have an effect.
      return {E::Arbitrary_effects, C::No_coeffects};
    case CK::Pbswap16: case CK::Pbbswap: case CK::Pint_as_pointer:
      return {E::No_effects, C::No_coeffects};
    case CK::Popaque: case CK::Ppoll: return {E::Arbitrary_effects, C::Has_coeffects};
    case CK::Psequand: case CK::Psequor:
      // Removed by [Closure_conversion] in the flambda pipeline.
      return {E::No_effects, C::No_coeffects};
    case CK::Pdls_get: return {E::No_effects, C::No_coeffects};  // only read
  }
  throw std::logic_error("Semantics_of_primitives.for_primitive");
}

ReturnType return_type_of_primitive(const clambda::Primitive& prim) {
  switch (prim.kind) {
    case CK::Pfloatofint: case CK::Pnegfloat: case CK::Pabsfloat: case CK::Paddfloat: case CK::Psubfloat:
    case CK::Pmulfloat: case CK::Pdivfloat: case CK::Pfloatfield:
      return ReturnType::Float;
    case CK::Parrayrefu:
    case CK::Parrayrefs:
      return prim.array == lambda::ArrayKind::Pfloatarray ? ReturnType::Float : ReturnType::Other;
    default: return ReturnType::Other;
  }
}

}  // namespace cppcaml::typing::semantics_of_primitives
