// Port of typing/typetexp.ml (TYPECHECKER.md, stage 4): typechecking of type
// expressions for the core language.  Errors are `typetexp::Error` (the
// OCaml `Error.In_context`); reporting them comes with Printtyp.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typetexp {

namespace tt = typedtree;
namespace pt = parsetree;

struct AlreadyBound {};

struct Error : std::runtime_error {
  enum class Kind {
    Unbound_type_variable, No_type_wildcards, Undefined_type_constructor, Type_arity_mismatch,
    Bound_type_variable, Recursive_type, Type_mismatch, Alias_type_mismatch,
    Present_has_conjunction, Present_has_no_type, Constructor_mismatch, Not_a_variant,
    Variant_tags, Invalid_variable_name, Cannot_quantify, Multiple_constraints_on_type,
    Method_mismatch, Opened_object, Not_an_object, Repeated_tuple_label,
    Polymorphic_optional_param, Functor_optional_param
  };
  Location loc;
  env::t env;
  Kind kind;
  std::string name;                  // variable / label / tag names
  std::string name2;                 // Variant_tags
  std::vector<std::string> names;    // Unbound_type_variable: in-scope names
  Path::t path = nullptr;            // Undefined_type_constructor / Opened_object (option)
  Longident::t lid = nullptr;        // Type_arity_mismatch / Multiple_constraints_on_type
  long expected = 0, provided = 0;   // Type_arity_mismatch
  ctype::et::UnificationError trace; // Type_mismatch / Alias_type_mismatch
  TypeExpr* ty1 = nullptr;
  TypeExpr* ty2 = nullptr;
  Error(const Location& l, env::t e, Kind k) : std::runtime_error("Typetexp.Error"), loc(l), env(e), kind(k) {}
};
// Error_forward of Location.error: an uninterpreted extension node.
struct ErrorForward : std::runtime_error {
  const pt::Extension* ext;
  explicit ErrorForward(const pt::Extension* e) : std::runtime_error("Typetexp.Error_forward"), ext(e) {}
};

namespace ty_var_env {
void reset();
bool is_in_scope(std::string_view name);
template <class F>
auto with_local_scope(F&& f) -> decltype(f());
struct PendingUnivar {
  TypeExpr* univar;
  std::vector<TyOptRef*> associated;  // type_expr option ref list
};
using PolyUnivars = std::vector<std::pair<std::string_view, PendingUnivar*>>;
PolyUnivars make_poly_univars(const std::vector<std::string_view>& vars);
std::vector<TypeExpr*> check_poly_univars(env::t env, const Location& loc, const PolyUnivars& vars);
std::vector<TypeExpr*> instance_poly_univars(env::t env, const Location& loc,
                                             const PolyUnivars& vars);
// narrow / widen (with_local_scope)
struct Context {
  long gl;
  StrMap<std::pair<TypeExpr*, bool*>> tv;
};
Context narrow();
void widen(const Context& c);
template <class F>
auto with_local_scope(F&& f) -> decltype(f()) {
  Context c = narrow();
  struct W {
    Context c;
    ~W() { widen(c); }
  } w{c};
  return f();
}
}  // namespace ty_var_env

// Forward declarations (set by Typemod)
extern std::function<std::pair<Path::t, env::t>(std::shared_ptr<bool> used_slot, OverrideFlag, env::t,
                                                const Location&, const pt::LidLoc&)>
    type_open;
extern std::function<Path::t(const Location&, env::t, Longident::t)> transl_modtype_longident;
extern std::function<const tt::ModuleType*(env::t, const pt::ModuleType*)> transl_modtype;
// check_package_with_type_constraints loc env mty maybe ptys (ComputeMType
// returns the module type; NoMType returns nullptr)
extern std::function<const ModuleType*(const Location&, env::t, const ModuleType*, bool compute,
                                       Slice<std::pair<pt::LidLoc, const tt::CoreType*>>)>
    check_package_with_type_constraints;

bool valid_tyvar_name(std::string_view name);
const tt::CoreType* transl_simple_type(env::t env, const ty_var_env::PolyUnivars* univars,
                                       bool closed, const pt::CoreType* styp);
const tt::CoreType* transl_simple_type_univars(env::t env, const pt::CoreType* styp);
struct Delayed {
  const tt::CoreType* cty;
  TypeExpr* ty;
  std::function<void()> force;
};
Delayed transl_simple_type_delayed(env::t env, const pt::CoreType* styp);
const tt::CoreType* transl_type_scheme(env::t env, const pt::CoreType* styp);
const tt::CoreType* transl_type_param(env::t env, const pt::CoreType* styp);

// Pprintast.tyvar_of_name
std::string tyvar_of_name(std::string_view s);

}  // namespace cppcaml::typing::typetexp
