// Port of middle_end/flambda/augment_specialised_args.ml: new specialised
// arguments added to the functions of a set of closures (a wrapper keeping
// the old calling convention), the generic part of the unboxing passes.
#pragma once

#include <functional>
#include <optional>

#include "cppcaml/typing/inline_and_simplify_aux.hpp"

namespace cppcaml::typing::augment_specialised_args {

// Definition.t = Existing_inner_free_var of Variable.t
//   | Projection_from_existing_specialised_arg of Projection.t
struct Definition {
  variable::t existing_inner_free_var = nullptr;  // null: a projection
  projection::t projection = nullptr;
};
int compare_definition(const Definition& a, const Definition& b);

using PairKey = std::pair<variable::t, variable::t>;  // (fun_var, group)
struct PairCmp {
  int operator()(const PairKey& a, const PairKey& b) const {
    int c = variable::compare(a.first, b.first);
    return c != 0 ? c : variable::compare(a.second, b.second);
  }
};

// What_to_specialise.t
struct WhatToSpecialise {
  // [definitions] is indexed by (fun_var, group)
  OMap<PairKey, Slice<Definition>, PairCmp> definitions;
  const flambda::SetOfClosures* set_of_closures;
  variable::Set make_direct_call_surrogates_for;

  static WhatToSpecialise create(const flambda::SetOfClosures* set_of_closures);
  WhatToSpecialise new_specialised_arg(variable::t fun_var, variable::t group, const Definition& definition) const;
  WhatToSpecialise make_direct_call_surrogate_for(variable::t fun_var) const;
};

// duplicate_function ~env ~set_of_closures ~fun_var ~new_fun_var
using DuplicateFunction = std::function<std::pair<const flambda::FunctionDeclaration*,
                                                  variable::Map<flambda::SpecialisedTo>>(
    const inline_and_simplify_aux::Env& env, const flambda::SetOfClosures* set_of_closures, variable::t fun_var,
    variable::t new_fun_var)>;

// Make (T): T.pass_name and T.what_to_specialise
struct Make {
  const char* pass_name;
  std::function<WhatToSpecialise(const inline_and_simplify_aux::Env&, const flambda::SetOfClosures*)>
      what_to_specialise;

  // rewrite_set_of_closures ~env ~duplicate_function ~set_of_closures
  std::optional<std::pair<flambda::t, inlining_cost::Benefit>> rewrite_set_of_closures(
      const inline_and_simplify_aux::Env& env, const DuplicateFunction& duplicate_function,
      const flambda::SetOfClosures* set_of_closures) const;
};

}  // namespace cppcaml::typing::augment_specialised_args
