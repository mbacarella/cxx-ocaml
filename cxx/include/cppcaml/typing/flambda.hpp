// The flambda term language: middle_end/flambda/flambda.ml (with
// allocated_const.ml, parameter.ml and projection.ml, the modules its types
// are made of).
//
// Terms are immutable tagged nodes in the current zone, like Lambda's
// (lambda.hpp): `flambda::t` is an expression, `flambda::named` a let-bound
// defining expression, one struct per constructor, `kind` to switch on.  A
// pointer is OCaml's physical identity (map_lets & co. compare with ==).
// Lists are Slices, OCaml's options null pointers; maps and sets are the
// stdlib's trees (ocaml_map.hpp).  A Let caches the free variables of its
// defining expression and body, as Flambda.create_let computes them.
#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/flambda_ids.hpp"
#include "cppcaml/typing/fn_ref.hpp"
#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing {

// ---- Allocated_const ------------------------------------------------------
struct AllocatedConst {
  enum class Kind : std::uint8_t {
    Float, Int32, Int64, Nativeint, Float_array, Immutable_float_array, String, Immutable_string
  };
  Kind kind;
  double f = 0;            // Float
  std::int64_t i = 0;      // Int32 / Int64 / Nativeint
  Slice<double> floats;    // (Immutable_)float_array
  std::string_view s;      // (Immutable_)string
};
namespace allocated_const {
using t = const AllocatedConst*;
t float_(double f);
t int32(std::int32_t n);
t int64(std::int64_t n);
t nativeint(std::int64_t n);
t immutable_float_array(Slice<double> fl);
t immutable_string(std::string_view s);
int compare(t x, t y);
void print(format::Formatter& ppf, t c);
}  // namespace allocated_const

// ---- Parameter --------------------------------------------------------------
struct Parameter {
  variable::t var;
};
namespace parameter {
inline Parameter wrap(variable::t v) { return {v}; }
inline variable::t var(const Parameter& p) { return p.var; }
// Parameter.Set.vars
variable::Set set_vars(Slice<Parameter> ps);
}  // namespace parameter

// ---- Projection -------------------------------------------------------------
namespace projection {
struct ProjectClosure {
  variable::t set_of_closures;  // must yield a set of closures
  variable::t closure_id;       // Closure_id.t
};
struct MoveWithinSetOfClosures {
  variable::t closure;     // must yield a closure
  variable::t start_from;  // Closure_id.t
  variable::t move_to;     // Closure_id.t
};
struct ProjectVar {
  variable::t closure;     // must yield a closure
  variable::t closure_id;  // Closure_id.t
  variable::t var;         // Var_within_closure.t
};
struct T {
  enum class Kind : std::uint8_t { Project_var, Project_closure, Move_within_set_of_closures, Field };
  Kind kind;
  ProjectVar project_var;
  ProjectClosure project_closure;
  MoveWithinSetOfClosures move;
  long field_index = 0;  // Field
  variable::t field_var = nullptr;
};
using t = const T*;
int compare_project_var(const ProjectVar& a, const ProjectVar& b);
int compare_move_within_set_of_closures(const MoveWithinSetOfClosures& a, const MoveWithinSetOfClosures& b);
int compare_project_closure(const ProjectClosure& a, const ProjectClosure& b);
int compare(t a, t b);
inline bool equal(t a, t b) { return compare(a, b) == 0; }
variable::t projecting_from(t p);
t map_projecting_from(t p, FnRef<variable::t(variable::t)> f);
void print_project_closure(format::Formatter& ppf, const ProjectClosure& p);
void print_move_within_set_of_closures(format::Formatter& ppf, const MoveWithinSetOfClosures& m);
void print_project_var(format::Formatter& ppf, const ProjectVar& p);
void print(format::Formatter& ppf, t p);
}  // namespace projection

namespace flambda {

struct Expr;
struct Named;
using t = const Expr*;
using named = const Named*;

// call_kind = Indirect | Direct of Closure_id.t
struct CallKind {
  variable::t direct = nullptr;  // null: Indirect
};

// const = Int of int | Char of char
struct Const {
  enum class Kind : std::uint8_t { Int, Char };
  Kind kind;
  long n;  // the int, or the char's code
  // the block's identity: one per construction, kept by copies (the .cmx
  // writer shares it as ocamlopt's values are shared)
  std::uint64_t obj = next_value_identity();
};
inline Const const_int(long n) { return {Const::Kind::Int, n}; }
// a literal [Int n] of an OCaml source file: one block per file and value
// (ocamlopt shares a unit's equal constants), the file being the C++ one
// that ports it
Const const_int_literal(const char* unit, long n);
#define FLAMBDA_INT_LITERAL(n) ::cppcaml::typing::flambda::const_int_literal(__FILE__, n)

struct SpecialisedTo {
  variable::t var;
  projection::t projection = nullptr;  // option
};

struct FunctionDeclaration;
struct FunctionDeclarations {
  bool is_classic_mode;
  set_of_closures_id::t set_of_closures_id;
  set_of_closures_id::t set_of_closures_origin;  // Set_of_closures_origin.t
  variable::Map<const FunctionDeclaration*> funs;
};
struct FunctionDeclaration {
  variable::t closure_origin;  // Closure_origin.t
  Slice<Parameter> params;
  t body;
  variable::Set free_variables;
  symbol::Set free_symbols;
  bool stub;
  debuginfo::t dbg;
  lambda::InlineAttribute inline_;
  lambda::SpecialiseAttribute specialise;
  bool is_a_functor;
  lambda::PollAttribute poll;
};
struct SetOfClosures {
  const FunctionDeclarations* function_decls;
  variable::Map<SpecialisedTo> free_vars;
  variable::Map<SpecialisedTo> specialised_args;
  variable::Map<variable::t> direct_call_surrogates;
};

// ---- t ----------------------------------------------------------------------
enum class EK : std::uint8_t {
  Var, Let, Let_mutable, Apply, Send, Assign, If_then_else, Switch, String_switch, Static_raise, Static_catch,
  Try_with, While, For, Proved_unreachable
};
struct Expr {
  EK kind;
};
template <class T>
const T* as(t e) {
  return e && e->kind == T::K ? static_cast<const T*>(e) : nullptr;
}
using IntSet = OSet<long, IntCmp>;  // Numbers.Int.Set
struct SwitchCase {
  long key;
  t action;
};
struct StringCase {
  std::string_view s;
  t action;
};
struct CatchVar {  // Variable.t * Lambda.value_kind
  variable::t var;
  lambda::ValueKind kind;
};
#define FLAMBDA_EXPR(Name) \
  struct Name : Expr {     \
    static constexpr EK K = EK::Name;
FLAMBDA_EXPR(Var) variable::t var; };
FLAMBDA_EXPR(Let)
  variable::t var;
  named defining_expr;
  t body;
  variable::Set free_vars_of_defining_expr;
  variable::Set free_vars_of_body;
};
FLAMBDA_EXPR(Let_mutable)
  variable::t var;  // Mutable_variable.t
  variable::t initial_value;
  lambda::ValueKind contents_kind;
  t body;
};
FLAMBDA_EXPR(Apply)
  variable::t func;
  Slice<variable::t> args;
  CallKind call_kind;
  debuginfo::t dbg;
  lambda::InlineAttribute inline_;
  lambda::SpecialiseAttribute specialise;
};
FLAMBDA_EXPR(Send)
  lambda::MethKind meth_kind;
  variable::t meth;
  variable::t obj;
  Slice<variable::t> args;
  debuginfo::t dbg;
};
FLAMBDA_EXPR(Assign)
  variable::t being_assigned;  // Mutable_variable.t
  variable::t new_value;
};
FLAMBDA_EXPR(If_then_else) variable::t cond; t ifso; t ifnot; };
FLAMBDA_EXPR(Switch)
  variable::t scrutinee;
  IntSet numconsts;
  Slice<SwitchCase> consts;
  IntSet numblocks;
  Slice<SwitchCase> blocks;
  t failaction;  // option
};
FLAMBDA_EXPR(String_switch) variable::t scrutinee; Slice<StringCase> cases; t def; };
FLAMBDA_EXPR(Static_raise) static_exception::t exn; Slice<variable::t> args; };
FLAMBDA_EXPR(Static_catch) static_exception::t exn; Slice<CatchVar> vars; t body; t handler; };
FLAMBDA_EXPR(Try_with) t body; variable::t var; t handler; };
FLAMBDA_EXPR(While) t cond; t body; };
FLAMBDA_EXPR(For)
  variable::t bound_var;
  variable::t from_value;
  variable::t to_value;
  parsetree::DirectionFlag direction;
  t body;
};
FLAMBDA_EXPR(Proved_unreachable) };
#undef FLAMBDA_EXPR

// ---- named ------------------------------------------------------------------
enum class NK : std::uint8_t {
  Symbol, Const, Allocated_const, Read_mutable, Read_symbol_field, Set_of_closures, Project_closure,
  Move_within_set_of_closures, Project_var, Prim, Expr
};
struct Named {
  NK kind;
};
template <class T>
const T* as(named n) {
  return n && n->kind == T::K ? static_cast<const T*>(n) : nullptr;
}
#define FLAMBDA_NAMED(Name) \
  struct N##Name : Named {  \
    static constexpr NK K = NK::Name;
FLAMBDA_NAMED(Symbol) symbol::t sym; };
FLAMBDA_NAMED(Const) Const c; };
FLAMBDA_NAMED(Allocated_const) allocated_const::t c; };
FLAMBDA_NAMED(Read_mutable) variable::t var; };
FLAMBDA_NAMED(Read_symbol_field) symbol::t sym; long field; };
FLAMBDA_NAMED(Set_of_closures) const SetOfClosures* set; };
FLAMBDA_NAMED(Project_closure) projection::ProjectClosure p; };
FLAMBDA_NAMED(Move_within_set_of_closures) projection::MoveWithinSetOfClosures m; };
FLAMBDA_NAMED(Project_var) projection::ProjectVar p; };
FLAMBDA_NAMED(Prim) const clambda::Primitive* prim; Slice<variable::t> args; debuginfo::t dbg; };
FLAMBDA_NAMED(Expr) t expr; };
#undef FLAMBDA_NAMED

// ---- constant_defining_value ------------------------------------------------
struct BlockField {  // constant_defining_value_block_field = Symbol of Symbol.t | Const of const
  symbol::t sym = nullptr;  // null: Const
  Const c{Const::Kind::Int, 0};
};
struct ConstantDefiningValue {
  enum class Kind : std::uint8_t { Allocated_const, Block, Set_of_closures, Project_closure };
  Kind kind;
  allocated_const::t c = nullptr;    // Allocated_const
  tag::t tag = 0;                    // Block
  Slice<BlockField> fields;          // Block
  const SetOfClosures* set = nullptr;  // Set_of_closures ([free_vars] must be empty)
  symbol::t sym = nullptr;           // Project_closure
  variable::t closure_id = nullptr;  // Project_closure
};
using constant_defining_value = const ConstantDefiningValue*;

// ---- program ------------------------------------------------------------------
struct SymbolBinding {
  symbol::t sym;
  constant_defining_value def;
};
struct ProgramBody {
  enum class Kind : std::uint8_t { Let_symbol, Let_rec_symbol, Initialize_symbol, Effect, End };
  Kind kind;
  symbol::t sym = nullptr;                     // Let_symbol, Initialize_symbol, End
  constant_defining_value def = nullptr;       // Let_symbol
  Slice<SymbolBinding> defs;                   // Let_rec_symbol
  tag::t tag = 0;                              // Initialize_symbol
  Slice<t> fields;                             // Initialize_symbol
  t expr = nullptr;                            // Effect
  const ProgramBody* body = nullptr;           // all but End
};
using program_body = const ProgramBody*;
struct Program {
  symbol::Set imported_symbols;
  const ProgramBody* program_body;
};

// ---- constructors (fresh nodes) ----------------------------------------------
t var(variable::t v);
t let_mutable(variable::t var, variable::t initial_value, lambda::ValueKind contents_kind, t body);
t apply(variable::t func, Slice<variable::t> args, CallKind kind, debuginfo::t dbg,
        lambda::InlineAttribute inline_, lambda::SpecialiseAttribute specialise);
t send(lambda::MethKind kind, variable::t meth, variable::t obj, Slice<variable::t> args, debuginfo::t dbg);
t assign(variable::t being_assigned, variable::t new_value);
t if_then_else(variable::t cond, t ifso, t ifnot);
t switch_(variable::t scrutinee, IntSet numconsts, Slice<SwitchCase> consts, IntSet numblocks,
          Slice<SwitchCase> blocks, t failaction);
t string_switch(variable::t scrutinee, Slice<StringCase> cases, t def);
t static_raise(static_exception::t exn, Slice<variable::t> args);
t static_catch(static_exception::t exn, Slice<CatchVar> vars, t body, t handler);
t try_with(t body, variable::t var, t handler);
t while_(t cond, t body);
t for_(variable::t bound_var, variable::t from_value, variable::t to_value, parsetree::DirectionFlag direction,
       t body);
t proved_unreachable();

named n_symbol(symbol::t s);
named n_const(Const c);
named n_allocated_const(allocated_const::t c);
named n_read_mutable(variable::t v);
named n_read_symbol_field(symbol::t s, long field);
named n_set_of_closures(const SetOfClosures* set);
named n_project_closure(const projection::ProjectClosure& p);
named n_move_within_set_of_closures(const projection::MoveWithinSetOfClosures& m);
named n_project_var(const projection::ProjectVar& p);
named n_prim(const clambda::Primitive& prim, Slice<variable::t> args, debuginfo::t dbg);
named n_expr(t e);
// the literal named values of an OCaml source file ([Const (Int n)],
// [Allocated_const (Int32 0l)] ...): one block per file and value, in the
// permanent zone
named n_const_literal(const char* unit, long n);
named n_allocated_const_literal(const char* unit, allocated_const::t (*make)(), const char* key);
#define FLAMBDA_NAMED_INT_LITERAL(n) ::cppcaml::typing::flambda::n_const_literal(__FILE__, n)

program_body let_symbol(symbol::t s, constant_defining_value def, program_body body);
program_body let_rec_symbol(Slice<SymbolBinding> defs, program_body body);
program_body initialize_symbol(symbol::t s, tag::t tag, Slice<t> fields, program_body body);
program_body effect(t e, program_body body);
program_body end(symbol::t root);

// ---- flambda.mli ----------------------------------------------------------------
// free_variables ?ignore_uses_as_callee ?ignore_uses_as_argument
//   ?ignore_uses_in_project_var
struct FvOpts {
  bool ignore_uses_as_callee = false;
  bool ignore_uses_as_argument = false;
  bool ignore_uses_in_project_var = false;
};
variable::Set free_variables(t tree, FvOpts o = {});
variable::Set free_variables_named(named n, bool ignore_uses_in_project_var = false);
variable::Set used_variables(t tree, FvOpts o = {});
variable::Set used_variables_named(named n, bool ignore_uses_in_project_var = false);

t create_let(variable::t var, named defining_expr, t body);

symbol::Set free_symbols(t expr);
symbol::Set free_symbols_named(named n);
symbol::Set free_symbols_program(const Program& program);

const FunctionDeclaration* create_function_declaration(Slice<Parameter> params, t body, bool stub, debuginfo::t dbg,
                                                       lambda::InlineAttribute inline_,
                                                       lambda::SpecialiseAttribute specialise, bool is_a_functor,
                                                       variable::t closure_origin, lambda::PollAttribute poll);
const FunctionDeclarations* create_function_declarations(bool is_classic_mode,
                                                         variable::Map<const FunctionDeclaration*> funs);
const SetOfClosures* create_set_of_closures(const FunctionDeclarations* function_decls,
                                            variable::Map<SpecialisedTo> free_vars,
                                            variable::Map<SpecialisedTo> specialised_args,
                                            variable::Map<variable::t> direct_call_surrogates);

// Flambda.With_free_variables: a term with its free variables
struct WithFvExpr {
  t expr;
  variable::Set free_vars;
};
struct WithFvNamed {
  named n;
  variable::Set free_vars;
};
namespace with_free_variables {
inline WithFvNamed of_defining_expr_of_let(const Let* l) { return {l->defining_expr, l->free_vars_of_defining_expr}; }
inline WithFvExpr of_body_of_let(const Let* l) { return {l->body, l->free_vars_of_body}; }
inline WithFvExpr of_expr(t e) { return {e, free_variables(e)}; }
inline WithFvNamed of_named(named n) { return {n, free_variables_named(n)}; }
t create_let_reusing_defining_expr(variable::t var, const WithFvNamed& def, t body);
t create_let_reusing_body(variable::t var, named defining_expr, const WithFvExpr& body);
t create_let_reusing_both(variable::t var, const WithFvNamed& def, const WithFvExpr& body);
inline WithFvNamed expr(const WithFvExpr& e) { return {n_expr(e.expr), e.free_vars}; }
}  // namespace with_free_variables

// iter_general ~toplevel f f_named (Is_expr expr / Is_named named)
void iter_general(bool toplevel, FnRef<void(t)> f, FnRef<void(named)> f_named, t expr);
void iter_general_named(bool toplevel, FnRef<void(t)> f, FnRef<void(named)> f_named, named n);
// iter_lets t ~for_defining_expr ~for_last_body ~for_each_let
void iter_lets(t e, FnRef<void(variable::t, named)> for_defining_expr, FnRef<void(t)> for_last_body,
               FnRef<void(t)> for_each_let);
// map_lets t ~for_defining_expr ~for_last_body ~after_rebuild
t map_lets(t e, FnRef<named(variable::t, named)> for_defining_expr, FnRef<t(t)> for_last_body,
           FnRef<t(t)> after_rebuild);
t map_defining_expr_of_let(const Let* let_expr, FnRef<named(named)> f);
// fold_lets_option t ~init ~for_defining_expr ~for_last_body
//   ~filter_defining_expr
template <class A, class B, class FD, class FL, class FF>
std::pair<t, B> fold_lets_option(t e, A init, FD&& for_defining_expr, FL&& for_last_body,
                                 FF&& filter_defining_expr);

// update_body_of_function_declaration / update_function_decl's_params_and_body
const FunctionDeclaration* update_body_of_function_declaration(const FunctionDeclaration* d, t body);
const FunctionDeclaration* update_function_decl_params_and_body(const FunctionDeclaration* d, Slice<Parameter> params,
                                                                t body);
const FunctionDeclaration* update_function_declaration(const FunctionDeclaration* d, Slice<Parameter> params, t body);
const FunctionDeclarations* create_function_declarations_with_origin(bool is_classic_mode,
                                                                     variable::Map<const FunctionDeclaration*> funs,
                                                                     set_of_closures_id::t set_of_closures_origin);
inline const FunctionDeclarations* create_function_declarations_with_closures_origin(
    bool is_classic_mode, variable::Map<const FunctionDeclaration*> funs, set_of_closures_id::t origin) {
  return create_function_declarations_with_origin(is_classic_mode, funs, origin);
}
const FunctionDeclarations* update_function_declarations(const FunctionDeclarations* fds,
                                                         variable::Map<const FunctionDeclaration*> funs);
const FunctionDeclarations* import_function_declarations_for_pack(
    const FunctionDeclarations* fds, FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_set_of_closures_id,
    FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_set_of_closures_origin);
variable::Set used_params(const FunctionDeclaration* d);

int compare_const(const Const& a, const Const& b);
int compare_block_field(const BlockField& a, const BlockField& b);
// Constant_defining_value.compare / equal
int compare_constant_defining_value(constant_defining_value a, constant_defining_value b);
inline bool equal_constant_defining_value(constant_defining_value a, constant_defining_value b) {
  return a == b || compare_constant_defining_value(a, b) == 0;
}
bool equal_call_kind(const CallKind& a, const CallKind& b);
bool equal_specialised_to(const SpecialisedTo& a, const SpecialisedTo& b);

// printers
void print_const(format::Formatter& ppf, const Const& c);
void print(format::Formatter& ppf, t flam);  // "%a@." lam
void print_expr(format::Formatter& ppf, t flam);  // lam
void print_named(format::Formatter& ppf, named n);
void print_set_of_closures(format::Formatter& ppf, const SetOfClosures* set);
// print_function_declaration ppf (var, decl)
void print_function_declaration_pub(format::Formatter& ppf, variable::t var, const FunctionDeclaration* f);
void print_function_declarations(format::Formatter& ppf, const FunctionDeclarations* fd);
void print_specialised_to(format::Formatter& ppf, const SpecialisedTo& s);
void print_constant_defining_value(format::Formatter& ppf, constant_defining_value c);
void print_program(format::Formatter& ppf, const Program& program);

template <class A, class B, class FD, class FL, class FF>
std::pair<t, B> fold_lets_option(t e, A init, FD&& for_defining_expr, FL&& for_last_body,
                                 FF&& filter_defining_expr) {
  struct Def {
    variable::t var;
    named defining_expr;
  };
  std::vector<Def> lets;  // rev_lets, oldest first
  A acc = std::move(init);
  while (auto* l = as<Let>(e)) {
    auto [acc2, var, defining_expr] = for_defining_expr(std::move(acc), l->var, l->defining_expr);
    acc = std::move(acc2);
    lets.push_back({var, defining_expr});
    e = l->body;
  }
  auto [last_body, acc_b] = for_last_body(std::move(acc), e);
  // finish: List.fold_left over rev_lets, the innermost let first
  B accb = std::move(acc_b);
  WithFvExpr w = with_free_variables::of_expr(last_body);
  for (std::size_t k = lets.size(); k-- > 0;) {
    auto [accb2, var, defining_expr] = filter_defining_expr(std::move(accb), lets[k].var, lets[k].defining_expr,
                                                            w.free_vars);
    accb = std::move(accb2);
    if (!defining_expr) continue;
    t let_expr = with_free_variables::create_let_reusing_body(var, defining_expr, w);
    w = with_free_variables::of_expr(let_expr);
  }
  return {w.expr, std::move(accb)};
}

}  // namespace flambda
}  // namespace cppcaml::typing
