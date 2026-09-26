// Port of lambda/tmc.mli (TYPECHECKER.md stage 10): the
// tail-modulo-constructor transformation of [@tail_mod_cons] functions.
#pragma once

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::tmc {

lambda::lambda rewrite(lambda::lambda lam);

}  // namespace cppcaml::typing::tmc
