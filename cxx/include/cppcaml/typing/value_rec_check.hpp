// Port of typing/value_rec_check.ml (cxx/PORTING.md, stage 4c): the static
// check of recursive definitions (`let rec x = e`), which rejects the
// ill-formed ones (Typecore's Illegal_letrec_expr).
#pragma once

#include <optional>
#include <vector>

#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::value_rec_check {

// nullopt = the expression is not a valid recursive definition
std::optional<typedtree::RecursiveBindingKind> is_valid_recursive_expression(
    const std::vector<Ident::t>& idlist, const typedtree::Expression* expr);
bool is_valid_class_expr(const std::vector<Ident::t>& idlist, const typedtree::ClassExpr* ce);

}  // namespace cppcaml::typing::value_rec_check
