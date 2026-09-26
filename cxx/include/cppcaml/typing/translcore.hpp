// Port of lambda/translcore.mli (TYPECHECKER.md stage 10): the
// translation of the core language (expressions, functions, lets).
#pragma once

#include <functional>
#include <stdexcept>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translcore {

using debuginfo::scopes;

lambda::LetKind pure_module(const typedtree::ModuleExpr* m);

lambda::lambda transl_exp(scopes sc, const typedtree::Expression* e);
lambda::lambda transl_apply(scopes sc, lambda::TailcallAttribute tailcall, lambda::InlineAttribute inlined,
                            lambda::SpecialiseAttribute specialised, lambda::lambda lam,
                            Slice<typedtree::LabeledArg> sargs, const lambda::ScopedLocation& loc);
// transl_let ~scopes ?in_structure rec_flag pat_expr_list body
lambda::lambda transl_let(scopes sc, bool in_structure, RecFlag rec_flag,
                          Slice<const typedtree::ValueBinding*> pat_expr_list, lambda::lambda body);
lambda::lambda transl_extension_constructor(scopes sc, env::t env, Path::t path,  // nullptr = None
                                            const typedtree::TExtensionConstructor* ext);
lambda::lambda transl_scoped_exp(scopes sc, const typedtree::Expression* e);

struct Error : std::runtime_error {
  enum class Kind { Free_super_var, Unreachable_reached };
  Location loc;
  Kind kind;
  Error(const Location& l, Kind k) : std::runtime_error("Translcore.Error"), loc(l), kind(k) {}
};

// Forward declarations, filled by Translmod / Translclass
extern std::function<lambda::lambda(scopes, const typedtree::ModuleCoercion*, Path::t, const typedtree::ModuleExpr*)>
    transl_module;
extern std::function<lambda::lambda(scopes, Slice<Ident::t>, Path::t, const typedtree::StructureItem*,
                                    const std::function<lambda::lambda(Slice<Ident::t>)>&)>
    transl_struct_item;
extern std::function<lambda::lambda(scopes, Ident::t, Slice<std::string_view>, const typedtree::ClassExpr*)>
    transl_object;

}  // namespace cppcaml::typing::translcore
