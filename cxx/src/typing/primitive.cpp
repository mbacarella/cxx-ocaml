// Port of typing/primitive.ml (parse_description) and parsing/attr_helper.ml.
#include "cppcaml/typing/primitive.hpp"

#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/location.hpp"

namespace cppcaml::typing {

namespace attr_helper {
std::optional<parsetree::StrLoc> get_no_payload_attribute(std::string_view nm, const parsetree::Attributes& attrs) {
  std::vector<const parsetree::Attribute*> sel =
      builtin_attributes::select_attributes({{nm, builtin_attributes::AttrAction::Return}}, attrs);
  if (sel.empty()) return std::nullopt;
  if (sel.size() == 1) {
    const parsetree::Payload& p = sel[0]->attr_payload;
    if (p.kind == parsetree::Payload::Kind::PStr && p.str.empty()) return sel[0]->attr_name;
    throw Error(sel[0]->attr_name.loc, Error::Kind::No_payload_expected, sel[0]->attr_name.txt);
  }
  throw Error(sel[1]->attr_name.loc, Error::Kind::Multiple_attributes, sel[1]->attr_name.txt);
}
bool has_no_payload_attribute(std::string_view nm, const parsetree::Attributes& attrs) {
  return get_no_payload_attribute(nm, attrs).has_value();
}
}  // namespace attr_helper

namespace primitive {
static bool is_ocaml_repr(const NativeRepr& r) { return r.kind == NativeRepr::Kind::Same_as_ocaml_repr; }

const PrimitiveDescription* parse_description(const std::vector<NativeRepr>& native_repr_args0,
                                              NativeRepr native_repr_res, Slice<std::string_view> prim,
                                              const parsetree::Attributes& attrs, const Location& loc) {
  long arity = static_cast<long>(native_repr_args0.size());
  if (prim.empty()) throw std::logic_error("Primitive.parse_description");
  // primitive.ml's default native name: one "" literal
  static const std::string_view empty_native_name = [] {
    ZoneScope perm(permanent_zone());
    return zborrow(std::string_view());
  }();
  std::string_view name = prim[0], native_name = empty_native_name;
  bool old_style_noalloc = false, old_style_float = false;
  auto at = [&](std::size_t k) { return k < prim.size() ? prim[k] : std::string_view(); };
  if (prim.size() >= 4 && at(1) == "noalloc" && at(3) == "float") {
    native_name = at(2);
    old_style_noalloc = old_style_float = true;
  } else if (prim.size() >= 3 && at(1) == "noalloc") {
    native_name = at(2);
    old_style_noalloc = true;
  } else if (prim.size() >= 3 && at(2) == "float") {
    native_name = at(1);
    old_style_float = true;
  } else if (prim.size() >= 2 && at(1) == "noalloc") {
    old_style_noalloc = true;
  } else if (prim.size() >= 2) {
    native_name = at(1);
  }
  bool noalloc_attribute = attr_helper::has_no_payload_attribute("noalloc", attrs);
  bool all_ocaml = is_ocaml_repr(native_repr_res);
  for (auto& r : native_repr_args0) all_ocaml = all_ocaml && is_ocaml_repr(r);
  if (old_style_float && !all_ocaml) throw Error(loc, Error::Kind::Old_style_float_with_native_repr_attribute);
  if (old_style_noalloc && noalloc_attribute) throw Error(loc, Error::Kind::Old_style_noalloc_with_noalloc_attribute);
  // The compiler used to assume "noalloc" with "float" (GPR#167)
  old_style_noalloc = old_style_noalloc || old_style_float;
  if (old_style_float)
    location::deprecated(loc, "[@@unboxed] + [@@noalloc] should be used\ninstead of \"float\"");
  else if (old_style_noalloc)
    location::deprecated(loc, "[@@noalloc] should be used instead of \"noalloc\"");
  if (native_name.empty() && !all_ocaml) throw Error(loc, Error::Kind::No_native_primitive_with_repr_attribute);
  bool noalloc = old_style_noalloc || noalloc_attribute;
  std::vector<NativeRepr> native_repr_args = native_repr_args0;
  if (old_style_float) {
    native_repr_args.assign(static_cast<std::size_t>(arity), NativeRepr{NativeRepr::Kind::Unboxed_float});
    native_repr_res = NativeRepr{NativeRepr::Kind::Unboxed_float};
  }
  auto* d = make<PrimitiveDescription>();
  d->prim_name = name;
  d->prim_arity = arity;
  d->prim_alloc = !noalloc;
  d->prim_native_name = native_name;
  d->prim_native_repr_args = slice(native_repr_args);
  d->prim_native_repr_res = native_repr_res;
  return d;
}
}  // namespace primitive

}  // namespace cppcaml::typing
