// Port of middle_end/flambda/export_info.ml: what a .cmx exports for the
// flambda middle end (the approximations of the unit's symbols, its sets of
// closures and their function bodies, closure offsets).
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/simple_value_approx.hpp"

namespace cppcaml::typing::export_info {

namespace A = simple_value_approx;

// approx = Value_unknown | Value_id of Export_id.t | Value_symbol of Symbol.t
struct Approx {
  enum class Kind : unsigned char { Value_unknown, Value_id, Value_symbol } kind = Kind::Value_unknown;
  export_id::t id = nullptr;
  symbol::t sym = nullptr;
};

struct ValueString {
  std::optional<std::string_view> contents;  // Contents | Unknown_or_mutable
  long size;
};
struct ValueFloatArray {
  bool contents_known;                    // Contents of float option array | Unknown_or_mutable
  Slice<std::optional<double>> contents;
  long size;
};

struct ValueSetOfClosures {
  set_of_closures_id::t set_of_closures_id;
  variable::Map<Approx> bound_vars;  // Var_within_closure.Map
  variable::Map<flambda::SpecialisedTo> free_vars;
  variable::Map<Approx> results;  // Closure_id.Map
  symbol::t aliased_symbol;       // option
};

struct Descr {
  enum class Kind : unsigned char {
    Value_block, Value_mutable_block, Value_int, Value_char, Value_float, Value_float_array, Value_boxed_int,
    Value_string, Value_closure, Value_set_of_closures, Value_unknown_descr
  } kind;
  tag::t tag = 0;                // Value_block, Value_mutable_block
  Slice<Approx> fields;          // Value_block
  long n = 0;                    // Value_mutable_block size, Value_int, Value_char
  double f = 0;                  // Value_float
  ValueFloatArray float_array{};
  A::BoxedInt bi = A::BoxedInt::Int32;  // Value_boxed_int
  std::int64_t bival = 0;
  ValueString str{};
  variable::t closure_id = nullptr;           // Value_closure
  const ValueSetOfClosures* set = nullptr;  // Value_closure, Value_set_of_closures
};

using CUMapCmp = compilation_unit::Cmp;
template <class V>
using CUMap = OMap<compilation_unit::t, V, CUMapCmp>;

struct T {
  set_of_closures_id::Map<const A::FunctionDeclarations*> sets_of_closures;
  CUMap<export_id::Map<const Descr*>> values;
  symbol::Map<export_id::t> symbol_id;
  variable::Map<long> offset_fun;  // Closure_id.Map
  variable::Map<long> offset_fv;   // Var_within_closure.Map
  variable::Set constant_closures;  // Closure_id.Set
  set_of_closures_id::Map<variable::Map<variable::Set>> invariant_params;
  set_of_closures_id::Map<variable::Set> recursive;
};

// transient: what Build_export_info leaves for Flambda_to_clambda to
// complete with the closures' layout (t_of_transient)
struct Transient {
  set_of_closures_id::Map<const A::FunctionDeclarations*> sets_of_closures;
  CUMap<export_id::Map<const Descr*>> values;
  symbol::Map<export_id::t> symbol_id;
  set_of_closures_id::Map<variable::Map<variable::Set>> invariant_params;
  set_of_closures_id::Map<variable::Set> recursive;
  variable::Set relevant_local_closure_ids;             // Closure_id.Set
  variable::Set relevant_imported_closure_ids;          // Closure_id.Set
  variable::Set relevant_local_vars_within_closure;     // Var_within_closure.Set
  variable::Set relevant_imported_vars_within_closure;  // Var_within_closure.Set
};

const T* empty();
Transient opaque_transient(compilation_unit::t compilation_unit, symbol::t root_symbol);
// t_of_transient transient ~program ~local_offset_fun ~local_offset_fv
//   ~imported_offset_fun ~imported_offset_fv ~constant_closures
const T* t_of_transient(const Transient& transient, const variable::Map<long>& local_offset_fun,
                        const variable::Map<long>& local_offset_fv, const variable::Map<long>& imported_offset_fun,
                        const variable::Map<long>& imported_offset_fv, const variable::Set& constant_closures);
bool equal_approx(const Approx& a1, const Approx& a2);
bool equal_descr(const Descr* d1, const Descr* d2);
const T* merge(const T* t1, const T* t2);
// find_description t eid (null: Not_found)
const Descr* find_description(const T* t, export_id::t eid);
CUMap<export_id::Map<const Descr*>> nest_eid_map(const export_id::Map<const Descr*>& map);

// print_approx ppf (t, root_symbols) / print_functions ppf t (ocamlobjinfo's)
void print_approx(format::Formatter& ppf, const T* t, const std::vector<symbol::t>& root_symbols);
void print_functions(format::Formatter& ppf, const T* t);

}  // namespace cppcaml::typing::export_info
