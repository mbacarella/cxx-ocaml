// Port of typing/primitive.ml (parse_description) and parsing/attr_helper.ml
// (get_no_payload_attribute), as Typedecl uses them.
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing {

namespace attr_helper {
struct Error : std::runtime_error {  // Multiple_attributes | No_payload_expected
  enum class Kind { Multiple_attributes, No_payload_expected };
  Location loc;
  Kind kind;
  std::string_view name;
  Error(const Location& l, Kind k, std::string_view n) : std::runtime_error("Attr_helper.Error"), loc(l), kind(k), name(n) {}
};
// Some name (the attribute's name, with its location) | None
std::optional<parsetree::StrLoc> get_no_payload_attribute(std::string_view nm, const parsetree::Attributes& attrs);
bool has_no_payload_attribute(std::string_view nm, const parsetree::Attributes& attrs);
}  // namespace attr_helper

namespace primitive {
struct Error : std::runtime_error {
  enum class Kind {
    Old_style_float_with_native_repr_attribute, Old_style_noalloc_with_noalloc_attribute,
    No_native_primitive_with_repr_attribute
  };
  Location loc;
  Kind kind;
  Error(const Location& l, Kind k) : std::runtime_error("Primitive.Error"), loc(l), kind(k) {}
};
const PrimitiveDescription* parse_description(const std::vector<NativeRepr>& native_repr_args,
                                              NativeRepr native_repr_res, Slice<std::string_view> prim,
                                              const parsetree::Attributes& attrs, const Location& loc);
}  // namespace primitive

}  // namespace cppcaml::typing
