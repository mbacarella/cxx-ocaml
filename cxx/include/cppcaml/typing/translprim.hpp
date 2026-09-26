// Port of lambda/translprim.mli (TYPECHECKER.md stage 10): the translation
// of primitives (`external` declarations and the %-builtins).
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translprim {

lambda::lambda event_before(const lambda::ScopedLocation& loc, const typedtree::Expression* exp, lambda::lambda lam);
lambda::lambda event_after(const lambda::ScopedLocation& loc, const typedtree::Expression* exp, lambda::lambda lam);

void add_exception_ident(Ident::t id);
void remove_exception_ident(Ident::t id);

void clear_used_primitives();
std::vector<Path::t> get_used_primitives();

void check_primitive_arity(const Location& loc, const PrimitiveDescription* p);

lambda::lambda transl_primitive(const lambda::ScopedLocation& loc, const PrimitiveDescription* p, env::t env,
                                TypeExpr* ty, Path::t path);  // path: nullptr = None

lambda::lambda transl_primitive_application(const lambda::ScopedLocation& loc, const PrimitiveDescription* p,
                                            env::t env, TypeExpr* ty, Path::t path,
                                            const typedtree::Expression* exp,  // nullptr = None
                                            Slice<lambda::lambda> args,
                                            Slice<const typedtree::Expression*> arg_exps);

struct Error : std::runtime_error {
  enum class Kind { Unknown_builtin_primitive, Wrong_arity_builtin_primitive };
  Location loc;
  Kind kind;
  std::string name;
  Error(const Location& l, Kind k, std::string n)
      : std::runtime_error("Translprim.Error"), loc(l), kind(k), name(std::move(n)) {}
};

}  // namespace cppcaml::typing::translprim
