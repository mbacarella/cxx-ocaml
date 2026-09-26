// Port of typing/predef.ml (TYPECHECKER.md): the predefined types and
// exceptions.  The idents are created on first use in predef.ml's order, so
// their stamps (int = 1 ... Some) equal ocamlc's -- paths compare predef
// idents by stamp.
#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/btype.hpp"

namespace cppcaml::typing::predef {

// type_constr, in all_type_constrs order
enum class TypeConstr {
  Int, Char, String, Bytes, Float, Bool, Unit, Exn, Eff, Continuation, Array,
  List, Option, Nativeint, Int32, Int64, Lazy_t, Extension_constructor,
  Floatarray, Iarray, Atomic_loc, Todo_info
};
const std::vector<TypeConstr>& all_type_constrs();
bool is_abstract_type_constr(TypeConstr c);

Ident::t ident_of_type_constr(TypeConstr c);
std::string_view name_of_type_constr(TypeConstr c);
Path::t path_of_type_constr(TypeConstr c);
std::optional<TypeConstr> find_type_constr(Path::t p);

struct Idents {
  Ident::t int_, char_, bytes, float_, bool_, unit, exn, eff, continuation, array, list,
      option, nativeint, int32, int64, lazy_t, string, extension_constructor, floatarray,
      iarray, atomic_loc, todo_info;
  Ident::t match_failure, out_of_memory, invalid_argument, failure, not_found, sys_error,
      end_of_file, division_by_zero, stack_overflow, sys_blocked_io, assert_failure,
      undefined_recursive_module, continuation_already_taken, todo;
  Ident::t false_, true_, void_, nil, cons, none, some;
};
const Idents& idents();

struct Paths {
  Path::t int_, char_, bytes, float_, bool_, unit, exn, eff, continuation, array, list,
      option, nativeint, int32, int64, lazy_t, string, extension_constructor, floatarray,
      iarray, atomic_loc, todo_info;
  Path::t match_failure, assert_failure, undefined_recursive_module, todo;
};
const Paths& paths();

// type_int, type_char, ... (created once, generic)
TypeExpr* type_int();
TypeExpr* type_char();
TypeExpr* type_bytes();
TypeExpr* type_float();
TypeExpr* type_bool();
TypeExpr* type_unit();
TypeExpr* type_exn();
TypeExpr* type_nativeint();
TypeExpr* type_int32();
TypeExpr* type_int64();
TypeExpr* type_string();
TypeExpr* type_extension_constructor();
TypeExpr* type_floatarray();
TypeExpr* type_todo_info();
// parameterised ones build a fresh node per call
TypeExpr* type_eff(TypeExpr* t);
TypeExpr* type_continuation(TypeExpr* t1, TypeExpr* t2);
TypeExpr* type_array(TypeExpr* t);
TypeExpr* type_list(TypeExpr* t);
TypeExpr* type_option(TypeExpr* t);
TypeExpr* type_lazy_t(TypeExpr* t);
TypeExpr* type_iarray(TypeExpr* t);
TypeExpr* type_atomic_loc(TypeExpr* t);

const std::vector<Ident::t>& all_predef_exns();

const TypeDeclaration* decl_of_type_constr(TypeConstr c);

// build_initial_env add_type add_extension empty_env
template <class Env>
Env build_initial_env(const std::function<Env(Ident::t, const TypeDeclaration*, Env)>& add_type,
                      const std::function<Env(Ident::t, const ExtensionConstructor*, Env)>& add_extension,
                      Env empty_env);

// the predefined exception's declaration, as build_initial_env adds it
const ExtensionConstructor* predef_extension(Ident::t id, Slice<TypeExpr*> args);
// build_initial_env's (exception ident, argument types) list, in its order.
// The argument types are made on demand: `env |> add_extension id [args]` is
// the application `add_extension id [args] env`, whose arguments evaluate
// right to left -- the chain so far first, then this step's types.
std::vector<std::pair<Ident::t, std::function<Slice<TypeExpr*>()>>> initial_extensions();

// Predef's module initialization (idents, paths, the shared nullary types),
// which in ocamlc runs before any typing and so takes the lowest ids.
void init();

std::vector<std::pair<std::string_view, Ident::t>> builtin_values();
std::vector<std::pair<std::string_view, Ident::t>> builtin_idents();

template <class Env>
Env build_initial_env(const std::function<Env(Ident::t, const TypeDeclaration*, Env)>& add_type,
                      const std::function<Env(Ident::t, const ExtensionConstructor*, Env)>& add_extension,
                      Env env) {
  for (TypeConstr c : all_type_constrs())
    env = add_type(ident_of_type_constr(c), decl_of_type_constr(c), env);
  for (auto& [id, args] : initial_extensions())
    env = add_extension(id, predef_extension(id, args()), env);
  return env;
}

}  // namespace cppcaml::typing::predef
