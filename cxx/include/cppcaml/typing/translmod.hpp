// Port of lambda/translmod.mli (cxx/PORTING.md stage 10): the translation
// of the module language, and of a compilation unit.  (The native-code
// entry points -- transl_store_*, *_flambda -- and the toplevel's are not
// ported: c++ocamlc is a bytecode compiler.)
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translmod {

// transl_implementation module_name (structure, coercion)
lambda::Program transl_implementation(std::string_view module_name, const typedtree::Structure* str,
                                      const typedtree::ModuleCoercion* cc);
// transl_store_implementation module_name (structure, coercion): the native
// compiler's (the defined values stored in the global block as they are
// defined; Clflags.native_code)
lambda::Program transl_store_implementation(std::string_view module_name, const typedtree::Structure* str,
                                            const typedtree::ModuleCoercion* restr);
lambda::lambda transl_package(Slice<Ident::t> component_names,  // Ident.t option list: nullptr = None
                              Ident::t target_name, const typedtree::ModuleCoercion* coercion);
// transl_store_package component_names target_name coercion (native -pack)
std::pair<long, lambda::lambda> transl_store_package(Slice<Ident::t> component_names, Ident::t target_name,
                                                     const typedtree::ModuleCoercion* coercion);
std::string toplevel_name(Ident::t id);

extern std::vector<const PrimitiveDescription*> primitive_declarations;

struct UnsafeInfo {  // Unsafe of {reason; loc; path} | Unnamed
  enum class Reason { Unsafe_module_binding, Unsafe_functor, Unsafe_non_function, Unsafe_typext };
  bool unnamed = false;
  Reason reason = Reason::Unsafe_module_binding;
  Location loc{};
  Path::t path = nullptr;
};
struct Error : std::runtime_error {
  enum class Kind { Circular_dependency, Conflicting_inline_attributes };
  Location loc;
  Kind kind;
  std::vector<std::pair<Ident::t, UnsafeInfo>> cycle;  // Circular_dependency
  Error(const Location& l, Kind k) : std::runtime_error("Translmod.Error"), loc(l), kind(k) {}
};

void reset();

// sets Translcore's forward references (transl_module, transl_struct_item,
// transl_object) to Translmod's and Translclass's functions
void install_forward_refs();

}  // namespace cppcaml::typing::translmod
