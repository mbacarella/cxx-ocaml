// Port of lambda/translclass.mli (cxx/PORTING.md stage 10): the
// translation of class definitions.
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translclass {

// transl_class ~scopes ids cl_id pub_meths cl vflag
std::pair<lambda::lambda, typedtree::RecursiveBindingKind> transl_class(
    debuginfo::scopes sc, Slice<Ident::t> ids, Ident::t cl_id, Slice<std::string_view> pub_meths,
    const typedtree::ClassExpr* cl, VirtualFlag vflag);

struct Error : std::runtime_error {  // Tags of string * string
  Location loc;
  std::string a, b;
  Error(const Location& l, std::string x, std::string y)
      : std::runtime_error("Translclass.Error"), loc(l), a(std::move(x)), b(std::move(y)) {}
};

}  // namespace cppcaml::typing::translclass
