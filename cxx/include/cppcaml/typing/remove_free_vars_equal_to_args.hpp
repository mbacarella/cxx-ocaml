// Port of middle_end/flambda/remove_free_vars_equal_to_args.ml and
// remove_unused_arguments.ml.
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::remove_free_vars_equal_to_args {
// run ~ppf_dump set_of_closures (null: None)
const flambda::SetOfClosures* run(format::Formatter& ppf_dump, const flambda::SetOfClosures* set_of_closures);
}  // namespace cppcaml::typing::remove_free_vars_equal_to_args

namespace cppcaml::typing::remove_unused_arguments {
// separate_unused_arguments_in_set_of_closures set_of_closures ~backend
// (null: None)
const flambda::SetOfClosures* separate_unused_arguments_in_set_of_closures(
    const flambda::SetOfClosures* set_of_closures);
flambda::Program separate_unused_arguments_in_closures(const flambda::Program& program);
}  // namespace cppcaml::typing::remove_unused_arguments
