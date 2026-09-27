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
  // The record's identity where it has one the writers must keep: the
  // Reader's (one per marshaled record of a .cmi -- input_value's sharing).
  // It points at the record's contents as read; a copy whose contents were
  // changed since is a different record (same_record says whether it holds).
  const Position* obj = nullptr;
};
struct Location {
  Position loc_start;
  Position loc_end;
  bool loc_ghost = false;
  const Location* obj = nullptr;  // as Position::obj
  // [obj] is a parser record distinct from the equal-valued ones: the
  // parser's second `make_loc` of one span (location::distinct_record)
  bool distinct = false;
};
// [x] is still the record [x.obj] identifies
bool same_record(const Position& x);
bool same_record(const Location& x);
namespace location {
// Location.none
Location none();
// [l] as a record of its own: the writers keep it apart from equal-valued
// locations (which they merge, as typing shares parsed locations by
// reference) -- for a span the parser gives two `make_loc` records
Location distinct_record(Location l);
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
  static ArgLabel labelled(std::string_view s) { return {Kind::Labelled, zborrow(s)}; }
  static ArgLabel optional(std::string_view s) { return {Kind::Optional, zborrow(s)}; }
  bool operator==(const ArgLabel& o) const { return kind == o.kind && name == o.name; }
  bool operator!=(const ArgLabel& o) const { return !(*this == o); }
};

// Asttypes.string_of_label
inline std::string string_of_label(const ArgLabel& l) {
  switch (l.kind) {
    case ArgLabel::Kind::Nolabel: return "";
    case ArgLabel::Kind::Labelled: return std::string(l.name);
    case ArgLabel::Kind::Optional: return "?" + std::string(l.name);
  }
  return "";
}

// An optional string (`string option`), trivially copyable for Slices.
// A fresh identity token (a zone byte): an OCaml allocation's identity where
// the port keeps a value (the .cmi writer shares by it, as Marshal does).
const void* fresh_identity();

struct OptStr {
  bool some = false;
  std::string_view v;
  // the `Some` block's identity: of() is an allocation (`~name` at a call
  // site), copies keep it; nullptr = none recorded.  Not part of equality.
  const void* obj = nullptr;
  static OptStr none() { return {}; }
  static OptStr of(std::string_view s) { return {true, zborrow(s), fresh_identity()}; }
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
  // The record's identity: OCaml passes a uid by reference, so copies of
  // one uid are one object (the .cmi writer shares by it); nullptr for the
  // immediate Internal.  Not part of equality.
  const void* obj = nullptr;
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
  // Unboxed_integer's block identity: typedecl's `Unboxed_integer Pint64`
  // (...) are static constants, a cmi's one per marshaled block.  Not part
  // of equality.
  const void* obj = nullptr;
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
  // a Lexing.position block read from a .cmi: the Reader's record for the
  // same marshaled block (one object, whichever way it is decoded)
  const Position* pos = nullptr;
};

// ---- Parsetree.attribute ------------------------------------------------
// As Types records hold them: decoded from a cmi (`attr_payload`, a generic
// value) or taken from the parsed source (`ast`, parsetree.hpp).
namespace parsetree {
struct Attribute;
}
struct Attribute {
  std::string_view attr_name;
  Location attr_name_loc;
  const OValue* attr_payload = nullptr;
  Location attr_loc;
  const parsetree::Attribute* ast = nullptr;
  // the {txt; loc} name record's identity where it is one object: a doc
  // attribute's is Docstrings's doc_loc / text_loc (nullptr: its own)
  const void* name_obj = nullptr;
};
using Attributes = Slice<const Attribute*>;

}  // namespace cppcaml::typing
