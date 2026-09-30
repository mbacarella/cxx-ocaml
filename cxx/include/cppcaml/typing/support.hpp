// Small modules Types depends on, ported as far as the typing/ port needs them
// (cxx/PORTING.md): Lexing.position / Location.t, Shape.Uid.t,
// Primitive.description, the Asttypes flags, Parsetree attributes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

// ---- Lexing.position / Location.t ----------------------------------------
// A position's file name: the string (its identity kept: the writers share
// equal names as the OCaml heap does) through an 8-byte handle, interned by
// that identity -- a location is in every node of the trees, and a
// string_view twice per location cost a sixth of their memory.  The handle
// is permanent, or in the .cmi image being recorded.
class Fname {
 public:
  Fname() = default;
  Fname(std::string_view s) : p_(intern(s)) {}
  Fname(const char* s) : Fname(std::string_view(s)) {}
  operator std::string_view() const { return p_ ? *p_ : std::string_view(); }
  std::string_view view() const { return *this; }
  const char* data() const { return p_ ? p_->data() : nullptr; }
  std::size_t size() const { return p_ ? p_->size() : 0; }
  bool empty() const { return size() == 0; }
  int compare(std::string_view o) const { return view().compare(o); }
  // the interned handle: one per string identity (data and size)
  const void* handle() const { return p_; }
  friend bool operator<(const Fname& a, const Fname& b) { return a.view() < b.view(); }
  friend bool operator==(const Fname& a, const Fname& b) { return a.view() == b.view(); }
  friend bool operator==(const Fname& a, std::string_view b) { return a.view() == b; }
  friend bool operator==(const Fname& a, const char* b) { return a.view() == std::string_view(b); }
  // the handles whose string [dying] holds point to [copy]'s copy of it
  // from now on (the zone is being dropped: evacuate.hpp -- the evacuator's
  // copy, which the other references to the string share)
  static void relocate(const Zone& dying, std::string_view (*copy)(void*, std::string_view), void* ctx);

 private:
  static const std::string_view* intern(std::string_view s);
  const std::string_view* p_ = nullptr;
};

struct Position {
  Fname pos_fname;
  // (32-bit: a source is smaller than 2 GiB)
  std::int32_t pos_lnum = 0;
  std::int32_t pos_bol = 0;
  std::int32_t pos_cnum = 0;
  // The record's identity where it has one the writers must keep: the
  // Reader's (one per marshaled record of a .cmi -- input_value's sharing).
  // It points at the record's contents as read; a copy whose contents were
  // changed since is a different record (same_record says whether it holds).
  const Position* obj = nullptr;
};
// Position{fname, lnum, bol, cnum} from wider integers
inline Position mkpos(Fname f, long lnum, long bol, long cnum) {
  return Position{f, static_cast<std::int32_t>(lnum), static_cast<std::int32_t>(bol), static_cast<std::int32_t>(cnum)};
}
// A location record's identity (Location::obj): what same_record needs to
// tell a copy still equal to the record from a changed one -- the character
// offsets and a fingerprint of the rest -- in 16 bytes (every parsed location
// has one: a copy of the whole record was a tenth of the typing memory)
struct LocRecord {
  std::int32_t start_cnum = 0;
  std::int32_t end_cnum = 0;
  std::uint64_t fp = 0;
};
struct Location;
// a new identity for [l]'s record, as its contents are now
const LocRecord* loc_record(const Location& l);

struct Location {
  Position loc_start;
  Position loc_end;
  bool loc_ghost = false;
  // [obj] is a parser record distinct from the equal-valued ones: the
  // parser's second `make_loc` of one span (location::distinct_record)
  bool distinct = false;
  const LocRecord* obj = nullptr;  // as Position::obj
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

// A tree node's location (the parse tree's and the typed tree's nodes): the
// record shared, as OCaml's nodes share the parser's Location.t -- a copy of
// the 80-byte record in every node was a sixth of the typing memory.  Made
// from a location in the current zone's storage it points at it (a zone's
// objects never move: the parser's record, another node's); from another (a
// temporary) it holds a copy.  Its fields are read through ->; a default
// one is Location.none.
class LocPtr {
 public:
  LocPtr() = default;
  LocPtr(const Location& l) : p_(share(l)) {}
  const Location& get() const { return p_ ? *p_ : none_record(); }
  operator const Location&() const { return get(); }
  const Location* operator->() const { return &get(); }

 private:
  static const Location* share(const Location& l);
  static const Location& none_record();
  const Location* p_ = nullptr;
};

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
  // the block's identity where it was read back (a .cmi, a binary AST); 0:
  // the parser's label, which the writers share by its name's string
  std::uint64_t obj = 0;
  static std::uint64_t fresh_obj() {
    static std::uint64_t n = 0;
    return ++n;
  }
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
std::string_view unit_name_string(std::string_view modname);
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
// primitive.ml's "" literal: the one native name of every description built
// there without one (Primitive.simple, parse_declaration), shared in a .cmx
inline std::string_view empty_native_name() {
  static constexpr char storage[1] = "";
  return {storage, 0};
}

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
  // a Location.t block converted from a parsetree location record (the
  // record, with its identity Location::obj): the writers make it that
  // record's one value
  const Location* loc_rec = nullptr;
  // a Location.t block of a whole parsetree being written (Pparse.write_ast):
  // the location itself, which the writer gives the value it gives the
  // same location of a typed tree (by value, or by its record's identity)
  const Location* loc_val = nullptr;
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
