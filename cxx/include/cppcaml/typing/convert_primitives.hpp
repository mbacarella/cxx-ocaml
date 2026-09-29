// Port of middle_end/convert_primitives.ml and
// middle_end/semantics_of_primitives.ml.
#pragma once

#include <utility>

#include "cppcaml/typing/clambda.hpp"

namespace cppcaml::typing::convert_primitives {
// convert prim (a fatal error on the primitives Closure never sees:
// Pbytes_to/of_string, Pctconst, Pignore, Pgetglobal, Psetglobal)
clambda::Primitive convert(const lambda::Primitive& prim);
}  // namespace cppcaml::typing::convert_primitives

namespace cppcaml::typing::semantics_of_primitives {
enum class Effects : std::uint8_t { No_effects, Only_generative_effects, Arbitrary_effects };
enum class Coeffects : std::uint8_t { No_coeffects, Has_coeffects };
std::pair<Effects, Coeffects> for_primitive(const clambda::Primitive& prim);
enum class ReturnType : std::uint8_t { Float, Other };
ReturnType return_type_of_primitive(const clambda::Primitive& prim);
}  // namespace cppcaml::typing::semantics_of_primitives
