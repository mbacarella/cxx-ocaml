// The identifiers of the flambda middle end: middle_end/compilation_unit.ml,
// linkage_name.ml, variable.ml, symbol.ml, and middle_end/flambda/
// base_types/ (closure_element, closure_id, var_within_closure,
// closure_origin, mutable_variable, set_of_closures_id / _origin,
// export_id, static_exception, tag) -- each with its Identifiable maps and
// sets (ocaml_map.hpp: the stdlib's trees, exactly).
//
// Closure_id, Var_within_closure, Closure_origin and Mutable_variable are
// Variable (`include Variable`): one type, one stamp counter.
// Set_of_closures_id / _origin and Export_id are Id_types.UnitId: a counter
// each.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/ident.hpp"
#include "cppcaml/typing/ocaml_map.hpp"

namespace cppcaml::typing {

// ---- Linkage_name ---------------------------------------------------------
namespace linkage_name {
using t = std::string_view;  // (a zone string)
struct Cmp {
  int operator()(t a, t b) const { return a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0; }
};
long hash(t s);  // Hashtbl.hash
}  // namespace linkage_name

// ---- Compilation_unit -----------------------------------------------------
struct CompilationUnit {
  Ident::t id;
  linkage_name::t linkage_name;
  long hash;
};
namespace compilation_unit {
using t = const CompilationUnit*;
// create id linkage_name (id persistent)
t create(Ident::t id, linkage_name::t linkage_name);
int compare(t a, t b);
inline bool equal(t a, t b) { return a == b || compare(a, b) == 0; }
inline std::string_view string_for_printing(t c) { return ident::name(c->id); }
void print(format::Formatter& ppf, t c);
// the current compilation unit (null: not set)
void set_current(t c);
t get_current();
t get_current_exn();
bool is_current(t c);
struct Cmp {
  int operator()(t a, t b) const { return compare(a, b); }
};
}  // namespace compilation_unit

// ---- Variable (and Closure_id, Var_within_closure, Closure_origin,
// Mutable_variable) ---------------------------------------------------------
struct VariableDesc {
  compilation_unit::t compilation_unit;
  std::string_view name;
  long name_stamp;
};
namespace variable {
using t = const VariableDesc*;
inline int compare(t a, t b) {
  if (a == b) return 0;
  long c = a->name_stamp - b->name_stamp;
  if (c != 0) return c < 0 ? -1 : 1;
  return compilation_unit::compare(a->compilation_unit, b->compilation_unit);
}
inline bool equal(t a, t b) {
  return a == b || (a->name_stamp == b->name_stamp && compilation_unit::equal(a->compilation_unit, b->compilation_unit));
}
inline long hash(t v) { return v->name_stamp ^ v->compilation_unit->hash; }
struct Cmp {
  int operator()(t a, t b) const { return compare(a, b); }
};
using Set = OSet<t, Cmp>;
template <class V>
using Map = OMap<t, V, Cmp>;

// create_with_name_string ?current_compilation_unit name
t create_with_name_string(std::string_view name, compilation_unit::t cu = nullptr);
// create ?current_compilation_unit name (an Internal_variable_names.t)
inline t create(std::string_view name, compilation_unit::t cu = nullptr) { return create_with_name_string(name, cu); }
t create_with_same_name_as_ident(Ident::t id);
inline t rename(t v, compilation_unit::t cu = nullptr) { return create_with_name_string(v->name, cu); }
inline bool in_compilation_unit(t v, compilation_unit::t cu) { return compilation_unit::equal(cu, v->compilation_unit); }
inline compilation_unit::t get_compilation_unit(t v) { return v->compilation_unit; }
inline std::string_view name(t v) { return v->name; }
std::string unique_name(t v);  // name ^ "_" ^ stamp
void print(format::Formatter& ppf, t v);
void print_opt(format::Formatter& ppf, t v);  // (null: <no var>)
// Make_set.print / Make_map.print
void print_set(format::Formatter& ppf, const Set& s);
}  // namespace variable

// ---- Linkage-name-based or variable-based symbols -------------------------
struct SymbolDesc {
  enum class Kind : unsigned char { Linkage, Variable } kind;
  compilation_unit::t compilation_unit;
  linkage_name::t label;  // Linkage
  long hash;              // Linkage
  variable::t variable;   // Variable
};
namespace symbol {
using t = const SymbolDesc*;
linkage_name::t label(t s);
int compare(t a, t b);
inline bool equal(t a, t b) { return a == b || compare(a, b) == 0; }
long hash(t s);
t of_global_linkage(compilation_unit::t cu, linkage_name::t label);
t of_variable(variable::t v);
t import_for_pack(compilation_unit::t pack, t s);
inline compilation_unit::t compilation_unit(t s) { return s->compilation_unit; }
void print(format::Formatter& ppf, t s);
void print_opt(format::Formatter& ppf, t s);  // (null: <no symbol>)
struct Cmp {
  int operator()(t a, t b) const { return compare(a, b); }
};
using Set = OSet<t, Cmp>;
template <class V>
using Map = OMap<t, V, Cmp>;
}  // namespace symbol

// ---- Id_types.UnitId (Set_of_closures_id, Export_id) ----------------------
struct UnitIdDesc {
  long id;
  std::optional<std::string_view> name;  // (None: Id_types' empty_string)
  compilation_unit::t unit;
};
namespace unit_id {
using t = const UnitIdDesc*;
inline int compare(t a, t b) {
  long c = a->id - b->id;
  if (c != 0) return c < 0 ? -1 : 1;
  return compilation_unit::compare(a->unit, b->unit);
}
std::string to_string(t x);  // unit.id (or unit.name_id)
void print(format::Formatter& ppf, t x);
struct Cmp {
  int operator()(t a, t b) const { return compare(a, b); }
};
}  // namespace unit_id
namespace set_of_closures_id {
using t = unit_id::t;
t create(compilation_unit::t cu, std::optional<std::string_view> name = std::nullopt);
using Set = OSet<t, unit_id::Cmp>;
template <class V>
using Map = OMap<t, V, unit_id::Cmp>;
}  // namespace set_of_closures_id
namespace export_id {
using t = unit_id::t;
t create(compilation_unit::t cu, std::optional<std::string_view> name = std::nullopt);
using Set = OSet<t, unit_id::Cmp>;
template <class V>
using Map = OMap<t, V, unit_id::Cmp>;
}  // namespace export_id

// ---- Static_exception (Numbers.Int), Tag ----------------------------------
struct IntCmp {
  int operator()(long a, long b) const { return a < b ? -1 : a > b ? 1 : 0; }
};
namespace static_exception {
using t = long;
long create();  // Lambda.next_raise_count ()
using Set = OSet<t, IntCmp>;
template <class V>
using Map = OMap<t, V, IntCmp>;
}  // namespace static_exception
namespace tag {
using t = long;
t create_exn(long tag);  // 0 .. 255
}  // namespace tag

}  // namespace cppcaml::typing
