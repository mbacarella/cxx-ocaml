// Port of middle_end/flambda/inlining_transforms.ml and
// inlining_decision.ml: whether and how a direct call is inlined or
// specialised, and the copying transformations themselves.
#pragma once

#include <functional>
#include <optional>

#include "cppcaml/typing/inline_and_simplify_aux.hpp"

namespace cppcaml::typing::inlining_decision {

namespace A = simple_value_approx;
using inline_and_simplify_aux::Env;
using inline_and_simplify_aux::Result;

// Inlining_decision_intf.simplify: env -> r -> expr -> expr * r
using Simplify = std::function<std::pair<flambda::t, Result>(const Env&, const Result&, flambda::t)>;

// for_call_site ~env ~r ~function_decls ~lhs_of_application
//   ~closure_id_being_applied ~function_decl ~value_set_of_closures ~args
//   ~args_approxs ~dbg ~simplify ~inline_requested ~specialise_requested
std::pair<flambda::t, Result> for_call_site(const Env& env, const Result& r, const A::FunctionDeclarations* function_decls,
                                            variable::t lhs_of_application, variable::t closure_id_being_applied,
                                            const A::FunctionDeclaration* function_decl,
                                            const A::ValueSetOfClosures* value_set_of_closures, Slice<variable::t> args,
                                            const std::vector<A::t>& args_approxs, const debuginfo::t& dbg,
                                            const Simplify& simplify, lambda::InlineAttribute inline_requested,
                                            lambda::SpecialiseAttribute specialise_requested);

// We do not inline inside stubs, which are always inlined at their call
// site.  (inlining_decision.ml)
inline bool should_inline_inside_declaration(const flambda::FunctionDeclaration* decl) { return !decl->stub; }

}  // namespace cppcaml::typing::inlining_decision
