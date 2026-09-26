// Port of lambda/tmc.mli (TYPECHECKER.md stage 10): the
// tail-modulo-constructor transformation of [@tail_mod_cons] functions.
#pragma once

#include <vector>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::tmc {

lambda::lambda rewrite(lambda::lambda lam);

// exception Error of Location.t * error (the one error,
// Ambiguous_constructor_arguments); raised by rewrite, not yet reported
// (its register_error_of_exn printer is stage 9).
struct TmcCallInformation {
  lambda::ScopedLocation loc;
  bool explicit_;
};
struct Error {
  Location loc;
  bool explicit_;  // ambiguous_arguments.explicit
  std::vector<std::vector<TmcCallInformation>> arguments;  // subterm_information list
};

}  // namespace cppcaml::typing::tmc
