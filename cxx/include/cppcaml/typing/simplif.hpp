// Port of lambda/simplif.mli (TYPECHECKER.md stage 10): the Lambda
// simplification pass (let-bound references to variables, static
// raise/catch, local functions, function arity fusion, default-argument
// wrappers).
#pragma once

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::simplif {

lambda::lambda simplify_lambda(lambda::lambda lam);

Slice<lambda::RecBinding> split_default_wrapper(Ident::t id, lambda::FunctionKind kind, Slice<lambda::Param> params,
                                                lambda::ValueKind return_, lambda::lambda body,
                                                const lambda::FunctionAttribute& attr,
                                                const lambda::ScopedLocation& loc);

}  // namespace cppcaml::typing::simplif
