// Port of lambda/value_rec_compiler.mli (TYPECHECKER.md stage 10): the
// compilation of `let rec` over values (static pre-allocation or dynamic).
#pragma once

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::value_rec_compiler {

struct RecBinding {  // Ident.t * Value_rec_types.recursive_binding_kind * Lambda.lambda
  Ident::t id;
  typedtree::RecursiveBindingKind kind;
  lambda::lambda def;
};
lambda::lambda compile_letrec(Slice<RecBinding> bindings, lambda::lambda body);

}  // namespace cppcaml::typing::value_rec_compiler
