// Port of middle_end/flambda/alias_analysis.ml: which allocation point (a
// symbol, or a variable bound to an allocation) each constant variable
// aliases, for Lift_constants.
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::alias_analysis {

// allocation_point = Symbol of Symbol.t | Variable of Variable.t
struct AllocationPoint {
  symbol::t sym = nullptr;     // Symbol
  variable::t var = nullptr;   // Variable
};

// allocated_const = Normal of Allocated_const.t
//   | Array of array_kind * mutable_flag * Variable.t list
//   | Duplicate_array of array_kind * mutable_flag * Variable.t
struct AllocatedConst {
  enum class Kind : unsigned char { Normal, Array, Duplicate_array } kind;
  allocated_const::t c = nullptr;
  lambda::ArrayKind array_kind = lambda::ArrayKind::Pgenarray;
  MutableFlag mut = MutableFlag::Immutable;
  Slice<variable::t> vars;  // Array
  variable::t var = nullptr;  // Duplicate_array
};

struct ConstantDefiningValue {
  enum class Kind : unsigned char {
    Allocated_const, Block, Set_of_closures, Project_closure, Move_within_set_of_closures, Project_var, Field,
    Symbol_field, Const, Symbol, Variable
  } kind;
  AllocatedConst allocated{};               // Allocated_const
  tag::t tag = 0;                           // Block
  Slice<variable::t> fields;                // Block
  const flambda::SetOfClosures* set = nullptr;  // Set_of_closures
  projection::ProjectClosure project_closure{};
  projection::MoveWithinSetOfClosures move{};
  projection::ProjectVar project_var{};
  variable::t var = nullptr;                // Field, Variable
  long field = 0;                           // Field, Symbol_field
  symbol::t sym = nullptr;                  // Symbol_field, Symbol
  flambda::Const c{};                       // Const
};
using constant_defining_value = const ConstantDefiningValue*;

void print_constant_defining_value(format::Formatter& ppf, constant_defining_value d);

// run variable initialize_symbol symbol ~the_dead_constant
variable::Map<AllocationPoint> run(const variable::Tbl<constant_defining_value>& variable,
                                   const symbol::Tbl<std::vector<variable::t>>& initialize_symbol,
                                   const symbol::Tbl<flambda::constant_defining_value>& symbol,
                                   symbol::t the_dead_constant);

}  // namespace cppcaml::typing::alias_analysis
