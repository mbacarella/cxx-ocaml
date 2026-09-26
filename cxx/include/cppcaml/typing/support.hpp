// Small modules Types depends on, ported as far as the typing/ port needs them
// (TYPECHECKER.md): Lexing.position / Location.t, Shape.Uid.t,
// Primitive.description, the Asttypes flags, Parsetree attributes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

// ---- Lexing.position / Location.t ----------------------------------------
struct Position {
  std::string_view pos_fname;
  long pos_lnum = 0;
  long pos_bol = 0;
  long pos_cnum = 0;
};
struct Location {
  Position loc_start;
  Position loc_end;
  bool loc_ghost = false;
};
namespace location {
// Location.none
Location none();
}

// ---- Asttypes ------------------------------------------------------------
enum class PrivateFlag : std::uint8_t { Private, Public };
enum class MutableFlag : std::uint8_t { Immutable, Mutable };
enum class AtomicFlag : std::uint8_t { Nonatomic, Atomic };
enum class VirtualFlag : std::uint8_t { Virtual, Concrete };
enum class RecFlag : std::uint8_t { Nonrecursive, Recursive };
enum class OverrideFlag : std::uint8_t { Override, Fresh };
enum class ClosedFlag : std::uint8_t { Closed, Open };

// arg_label = Nolabel | Labelled of string | Optional of string
struct ArgLabel {
  enum class Kind : std::uint8_t { Nolabel, Labelled, Optional };
  Kind kind = Kind::Nolabel;
  std::string_view name;
  static ArgLabel nolabel() { return {}; }
  static ArgLabel labelled(std::string_view s) { return {Kind::Labelled, zstr(s)}; }
  static ArgLabel optional(std::string_view s) { return {Kind::Optional, zstr(s)}; }
  bool operator==(const ArgLabel& o) const { return kind == o.kind && name == o.name; }
  bool operator!=(const ArgLabel& o) const { return !(*this == o); }
};

// An optional string (`string option`), trivially copyable for Slices.
struct OptStr {
  bool some = false;
  std::string_view v;
  static OptStr none() { return {}; }
  static OptStr of(std::string_view s) { return {true, zstr(s)}; }
  bool operator==(const OptStr& o) const { return some == o.some && (!some || v == o.v); }
};

// ---- Shape.Uid.t -----------------------------------------------------------
struct Uid {
  enum class Kind : std::uint8_t {
    Compilation_unit, Item, Local_opaque_item, Internal, Predef
  };
  enum class From : std::uint8_t { Intf, Impl };  // Unit_info.intf_or_impl
  Kind kind = Kind::Internal;
  std::string_view comp_unit;  // Compilation_unit / Item / Local_opaque_item; Predef name
  long id = 0;
  From from = From::Intf;
};

// Unit_info.t, as far as typing needs it (modname and intf/impl kind).
struct UnitInfo {
  std::string modname;
  Uid::From kind = Uid::From::Impl;
};

// Shape.Uid functions (shape.ml).
namespace uid {
Uid mk(const UnitInfo* current_unit);
Uid mk_local_opaque(const UnitInfo* current_unit);
Uid of_compilation_unit_id(std::string_view name);
Uid of_predef_id(std::string_view name);
inline Uid internal_not_actually_unique() { return Uid{}; }
bool for_actual_declaration(const Uid& u);
bool equal(const Uid& a, const Uid& b);
void reinit();
}  // namespace uid

// ---- Primitive.description ---------------------------------------------
enum class BoxedInteger : std::uint8_t { Pnativeint, Pint32, Pint64 };
struct NativeRepr {
  enum class Kind : std::uint8_t {
    Same_as_ocaml_repr, Unboxed_float, Unboxed_integer, Untagged_immediate
  };
  Kind kind = Kind::Same_as_ocaml_repr;
  BoxedInteger bi = BoxedInteger::Pnativeint;
};
struct PrimitiveDescription {
  std::string_view prim_name;
  long prim_arity = 0;
  bool prim_alloc = false;
  std::string_view prim_native_name;
  Slice<NativeRepr> prim_native_repr_args;
  NativeRepr prim_native_repr_res;
};

// ---- a generic OCaml value (Parsetree payloads, until they are needed typed)
// A zone copy of a marshaled value: an immediate, a string, a double or a
// block of fields.  Sharing among blocks is preserved (one OValue per
// marshaled block).
struct OValue {
  enum class Kind : std::uint8_t { Int, String, Double, Block };
  Kind kind = Kind::Int;
  long i = 0;               // Int
  std::string_view s;       // String
  double d = 0;             // Double
  unsigned tag = 0;         // Block
  Slice<const OValue*> fields;
};

// ---- Parsetree.attribute ------------------------------------------------
struct Attribute {
  std::string_view attr_name;
  Location attr_name_loc;
  const OValue* attr_payload = nullptr;
  Location attr_loc;
};
using Attributes = Slice<const Attribute*>;

}  // namespace cppcaml::typing
