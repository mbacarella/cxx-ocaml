// Port of middle_end/flambda/unbox_closures.ml, unbox_specialised_args.ml
// and unbox_free_vars_of_closures.ml: the unboxing passes Inline_and_simplify
// runs on sets of closures.
#pragma once

#include "cppcaml/typing/augment_specialised_args.hpp"

namespace cppcaml::typing::unbox_closures {
// include ASA.Make (Transform)
const augment_specialised_args::Make& pass();
}  // namespace cppcaml::typing::unbox_closures

namespace cppcaml::typing::unbox_specialised_args {
const augment_specialised_args::Make& pass();
}  // namespace cppcaml::typing::unbox_specialised_args

namespace cppcaml::typing::unbox_free_vars_of_closures {
// run ~env ~set_of_closures
std::optional<std::pair<flambda::t, inlining_cost::Benefit>> run(const inline_and_simplify_aux::Env& env,
                                                                 const flambda::SetOfClosures* set_of_closures);
}  // namespace cppcaml::typing::unbox_free_vars_of_closures
