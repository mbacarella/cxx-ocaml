// Port of middle_end/flambda/simple_value_approx.ml: approximations of
// values (what is known at compile time about the result of a
// computation), as Inline_and_simplify, Import_approx and the export info
// use them.
//
// `t` is a pointer to an immutable approximation in the current zone.
// OCaml's lazy fields (a set of closures' invariant parameters, recursive
// functions and sizes) are `Lazy` cells: forced at most once.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "cppcaml/typing/flambda.hpp"
#include "cppcaml/typing/freshening.hpp"

namespace cppcaml::typing::simple_value_approx {

// Lazy.t: a value, or a computation run on first force
template <class T>
class Lazy {
 public:
  Lazy() = default;
  static Lazy from_val(T v) {
    Lazy l;
    l.cell_ = new Cell{true, std::move(v), {}};
    return l;
  }
  static Lazy of_fun(std::function<T()> f) {
    Lazy l;
    l.cell_ = new Cell{false, T{}, std::move(f)};
    return l;
  }
  const T& force() const {
    if (!cell_->forced) {
      cell_->value = cell_->thunk();
      cell_->forced = true;
      cell_->thunk = nullptr;
    }
    return cell_->value;
  }

 private:
  struct Cell {
    bool forced;
    T value;
    std::function<T()> thunk;
  };
  Cell* cell_ = nullptr;  // (never freed: as the zone's)
};

enum class BoxedInt : std::uint8_t { Int32, Int64, Nativeint };

struct ValueString {
  std::optional<std::string_view> contents;  // None if unknown or mutable
  long size;
};

// unresolved_value = Set_of_closures_id of ... | Symbol of Symbol.t
struct UnresolvedValue {
  set_of_closures_id::t set_of_closures_id = nullptr;  // null: Symbol
  symbol::t sym = nullptr;
};

// unknown_because_of = Unresolved_value of unresolved_value | Other
struct UnknownBecauseOf {
  bool unresolved = false;  // false: Other
  UnresolvedValue value;
};
inline UnknownBecauseOf other() { return {}; }

struct Approx;
using t = const Approx*;

struct FunctionBody {
  variable::Set free_variables;
  symbol::Set free_symbols;
  bool stub;
  debuginfo::t dbg;
  lambda::InlineAttribute inline_;
  lambda::SpecialiseAttribute specialise;
  bool is_a_functor;
  flambda::t body;
  lambda::PollAttribute poll;
};

struct FunctionDeclaration {
  variable::t closure_origin;
  Slice<Parameter> params;
  const FunctionBody* function_body;  // option
};

struct FunctionDeclarations {
  bool is_classic_mode;
  set_of_closures_id::t set_of_closures_id;
  set_of_closures_id::t set_of_closures_origin;
  variable::Map<const FunctionDeclaration*> funs;
};

struct ValueSetOfClosures {
  const FunctionDeclarations* function_decls;
  variable::Map<t> bound_vars;  // Var_within_closure.Map
  variable::Map<flambda::SpecialisedTo> free_vars;
  Lazy<variable::Map<variable::Set>> invariant_params;
  Lazy<variable::Set> recursive;
  Lazy<variable::Map<std::optional<long>>> size;
  variable::Map<flambda::SpecialisedTo> specialised_args;
  freshening::project_var::T freshening;
  variable::Map<variable::t> direct_call_surrogates;  // Closure_id.Map
};

struct ValueClosure {
  t set_of_closures;
  variable::t closure_id;
};

struct ValueFloatArray {
  bool contents_known = false;  // Contents of t array | Unknown_or_mutable
  Slice<t> contents;
  long size = 0;
};

enum class DK : std::uint8_t {
  Value_block, Value_int, Value_char, Value_float, Value_boxed_int, Value_set_of_closures, Value_closure,
  Value_string, Value_float_array, Value_unknown, Value_bottom, Value_extern, Value_symbol, Value_unresolved
};

struct Descr {
  DK kind;
  tag::t tag = 0;               // Value_block
  Slice<t> fields;              // Value_block
  long i = 0;                   // Value_int, Value_char (the code)
  std::optional<double> f;      // Value_float
  BoxedInt bi = BoxedInt::Int32;  // Value_boxed_int
  std::int64_t bival = 0;
  const ValueSetOfClosures* set = nullptr;  // Value_set_of_closures
  ValueClosure closure{};                   // Value_closure
  ValueString str{};                        // Value_string
  ValueFloatArray float_array{};            // Value_float_array
  UnknownBecauseOf unknown{};               // Value_unknown
  export_id::t ex = nullptr;                // Value_extern
  symbol::t sym = nullptr;                  // Value_symbol
  UnresolvedValue unresolved{};             // Value_unresolved
};

struct SymbolRef {  // Symbol.t * int option
  symbol::t sym;
  std::optional<long> field;
};

struct Approx {
  Descr descr;
  variable::t var = nullptr;  // option
  std::optional<SymbolRef> symbol;
};

inline const Descr& descr(t a) { return a->descr; }

void print(format::Formatter& ppf, t a);
void print_descr(format::Formatter& ppf, const Descr& d);
void print_value_set_of_closures(format::Formatter& ppf, const ValueSetOfClosures* s);
void print_function_declarations(format::Formatter& ppf, const FunctionDeclarations* fd);

const FunctionDeclarations* function_declarations_approx(
    FnRef<bool(variable::t, const flambda::FunctionDeclaration*)> keep_body, const flambda::FunctionDeclarations* fd);

const ValueSetOfClosures* create_value_set_of_closures(
    const FunctionDeclarations* function_decls, variable::Map<t> bound_vars,
    variable::Map<flambda::SpecialisedTo> free_vars, Lazy<variable::Map<variable::Set>> invariant_params,
    Lazy<variable::Set> recursive, variable::Map<flambda::SpecialisedTo> specialised_args,
    freshening::project_var::T freshening, variable::Map<variable::t> direct_call_surrogates);
const ValueSetOfClosures* update_freshening_of_value_set_of_closures(const ValueSetOfClosures* s,
                                                                     freshening::project_var::T freshening);

t approx(const Descr& d);
t augment_with_variable(t a, variable::t var);
t augment_with_symbol(t a, symbol::t sym);
t augment_with_symbol_field(t a, symbol::t sym, long field);
t replace_description(t a, const Descr& d);
t augment_with_kind(t a, const lambda::ValueKind& kind);
lambda::ValueKind augment_kind_with_approx(t a, const lambda::ValueKind& kind);

t value_unknown(UnknownBecauseOf reason);
t value_int(long i);
t value_char(long c);
t value_float(double f);
t value_any_float();
t value_mutable_float_array(long size);
t value_immutable_float_array(Slice<t> contents);
t value_string(long size, std::optional<std::string_view> contents);
t value_boxed_int(BoxedInt bi, std::int64_t i);
t value_block(tag::t tag, Slice<t> fields);
t value_extern(export_id::t ex);
t value_symbol(symbol::t sym);
t value_bottom();
t value_unresolved(UnresolvedValue value);
// value_closure ?closure_var ?set_of_closures_var ?set_of_closures_symbol
t value_closure(const ValueSetOfClosures* s, variable::t closure_id, variable::t closure_var = nullptr,
                variable::t set_of_closures_var = nullptr, symbol::t set_of_closures_symbol = nullptr);
t value_set_of_closures(const ValueSetOfClosures* s, variable::t set_of_closures_var = nullptr);

std::pair<flambda::t, t> make_const_int(long n);
std::pair<flambda::t, t> make_const_char(long c);
std::pair<flambda::t, t> make_const_bool(bool b);
std::pair<flambda::t, t> make_const_float(double f);
std::pair<flambda::t, t> make_const_boxed_int(BoxedInt bi, std::int64_t i);
std::pair<flambda::named, t> make_const_int_named(long n);
std::pair<flambda::named, t> make_const_char_named(long c);
std::pair<flambda::named, t> make_const_bool_named(bool b);
std::pair<flambda::named, t> make_const_float_named(double f);
std::pair<flambda::named, t> make_const_boxed_int_named(BoxedInt bi, std::int64_t i);

enum class SimplificationSummary : std::uint8_t { Nothing_done, Replaced_term };
struct SimplificationResult {
  flambda::t expr;
  SimplificationSummary summary;
  t approx;
};
struct SimplificationResultNamed {
  flambda::named named;
  SimplificationSummary summary;
  t approx;
};
SimplificationResult simplify(t a, flambda::t lam);
SimplificationResultNamed simplify_named(t a, flambda::named named);
// (nullopt: None)
std::optional<std::pair<flambda::named, t>> simplify_var(t a);
SimplificationResult simplify_using_env(t a, FnRef<bool(variable::t)> is_present_in_env, flambda::t flam);
SimplificationResultNamed simplify_named_using_env(t a, FnRef<bool(variable::t)> is_present_in_env,
                                                   flambda::named named);
variable::t simplify_var_to_var_using_env(t a, FnRef<bool(variable::t)> is_present_in_env);

bool known(t a);
bool useful(t a);
bool all_not_useful(Slice<t> ts);
bool warn_on_mutation(t a);

// get_field t ~field_index (null: Unreachable)
t get_field(t a, long field_index);
// check_approx_for_block (nullopt: Wrong)
struct BlockApprox {
  tag::t tag;
  Slice<t> fields;
};
std::optional<BlockApprox> check_approx_for_block(t a);

bool equal_boxed_int(BoxedInt bi1, std::int64_t i1, BoxedInt bi2, std::int64_t i2);
// meet ~really_import_approx
t meet(FnRef<t(t)> really_import_approx, t a1, t a2);

variable::t freshen_and_check_closure_id(const ValueSetOfClosures* s, variable::t closure_id);

struct CheckedSetOfClosures {
  enum class Kind : std::uint8_t { Wrong, Unresolved, Unknown, Unknown_because_of_unresolved_value, Ok } kind;
  UnresolvedValue value{};
  variable::t var = nullptr;
  const ValueSetOfClosures* set = nullptr;
};
CheckedSetOfClosures check_approx_for_set_of_closures(t a);
// strict_check_approx_for_set_of_closures (Wrong: kind Wrong)
CheckedSetOfClosures strict_check_approx_for_set_of_closures(t a);

struct CheckedClosure {
  enum class Kind : std::uint8_t { Wrong, Unresolved, Unknown, Unknown_because_of_unresolved_value, Ok } kind;
  UnresolvedValue value{};
  const ValueClosure* closure = nullptr;
  variable::t set_of_closures_var = nullptr;
  symbol::t set_of_closures_symbol = nullptr;
  const ValueSetOfClosures* set = nullptr;
};
CheckedClosure check_approx_for_closure_allowing_unresolved(t a);
CheckedClosure check_approx_for_closure(t a);

t approx_for_bound_var(const ValueSetOfClosures* s, variable::t var);
std::optional<double> check_approx_for_float(t a);
// float_array_as_constant (nullopt: None)
std::optional<std::vector<double>> float_array_as_constant(const ValueFloatArray& fa);
std::optional<std::string_view> check_approx_for_string(t a);

enum class SwitchBranchSelection : std::uint8_t { Cannot_be_taken, Can_be_taken, Must_be_taken };
SwitchBranchSelection potentially_taken_const_switch_branch(t a, long branch);
SwitchBranchSelection potentially_taken_block_switch_branch(t a, long tag);

inline long function_arity(const FunctionDeclaration* d) { return static_cast<long>(d->params.size()); }

const FunctionDeclarations* import_function_declarations_for_pack(
    const FunctionDeclarations* fd, FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_id,
    FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_origin);
const FunctionDeclarations* update_function_declarations(const FunctionDeclarations* fd,
                                                         variable::Map<const FunctionDeclaration*> funs);
const FunctionDeclarations* clear_function_bodies(const FunctionDeclarations* fd);
const FunctionDeclaration* update_function_declaration_body(const FunctionDeclaration* d,
                                                            FnRef<flambda::t(flambda::t)> f);
// make_closure_map: Closure_id -> the function declarations defining it
variable::Map<const FunctionDeclarations*> make_closure_map(
    const set_of_closures_id::Map<const FunctionDeclarations*>& input);

}  // namespace cppcaml::typing::simple_value_approx
