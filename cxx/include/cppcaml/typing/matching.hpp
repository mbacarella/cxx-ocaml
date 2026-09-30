// Port of lambda/matching.mli (cxx/PORTING.md stage 10): the compilation of
// pattern matching (with lambda/switch.ml, whose functor Matching
// instantiates -- switch.hpp).
#pragma once

#include <stdexcept>
#include <vector>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::matching {

using debuginfo::scopes;

struct PatAction {  // pattern * lambda
  const typedtree::Pattern* pat;
  lambda::lambda action;
};
struct PatsAction {  // pattern list * lambda
  Slice<const typedtree::Pattern*> pats;
  lambda::lambda action;
};

// for_function ~scopes loc repr param pat_act_list partial
lambda::lambda for_function(scopes sc, const Location& loc, lambda::IntRef* repr, lambda::lambda param,
                            Slice<PatAction> pat_act_list, typedtree::Partial partial);
lambda::lambda for_trywith(scopes sc, const Location& loc, lambda::lambda param, Slice<PatAction> pat_act_list);
lambda::lambda for_handler(scopes sc, const Location& loc, lambda::lambda param, lambda::lambda cont,
                           lambda::lambda cont_tail, Slice<PatAction> pat_act_list);
lambda::lambda for_let(scopes sc, const Location& loc, lambda::lambda param, const typedtree::Pattern* pat,
                       lambda::lambda body);
lambda::lambda for_multiple_match(scopes sc, const Location& loc, Slice<lambda::lambda> paraml,
                                  Slice<PatAction> pat_act_list, typedtree::Partial partial);
lambda::lambda for_tupled_function(scopes sc, const Location& loc, Slice<Ident::t> paraml,
                                   Slice<PatsAction> pats_act_list, typedtree::Partial partial);
lambda::lambda for_optional_arg_default(scopes sc, const Location& loc, const typedtree::Pattern* pat,
                                        lambda::lambda default_arg, Ident::t param, lambda::lambda body);

struct CannotFlatten {};
std::vector<const typedtree::Pattern*> flatten_pattern(long size, const typedtree::Pattern* p);  // raises CannotFlatten

lambda::lambda expand_stringswitch(const lambda::ScopedLocation& loc, lambda::lambda arg,
                                   Slice<lambda::StringCase> sw, lambda::lambda d);  // d: nullptr = None
lambda::lambda inline_lazy_force(lambda::lambda arg, const lambda::ScopedLocation& loc);

}  // namespace cppcaml::typing::matching
