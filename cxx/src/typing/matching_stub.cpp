// TEMPORARY link stub until the Matching port lands; delete with it.
#include "cppcaml/typing/matching.hpp"
#include "cppcaml/typing/typeopt.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::matching {
[[noreturn]] static void stub() { throw std::runtime_error("matching: not yet ported"); }
lambda::lambda for_function(scopes, const Location&, lambda::IntRef*, lambda::lambda, Slice<PatAction>,
                            typedtree::Partial) { stub(); }
lambda::lambda for_trywith(scopes, const Location&, lambda::lambda, Slice<PatAction>) { stub(); }
lambda::lambda for_handler(scopes, const Location&, lambda::lambda, lambda::lambda, Slice<PatAction>) { stub(); }
lambda::lambda for_let(scopes, const Location&, lambda::lambda param, const typedtree::Pattern* pat,
                       lambda::lambda body) {
  if (typedtree::as<typedtree::Tpat_any>(pat->pat_desc)) return lambda::lsequence(param, body);
  if (auto* v = typedtree::as<typedtree::Tpat_var>(pat->pat_desc))
    return lambda::llet(lambda::LetKind::Strict, typeopt::value_kind(pat->pat_env, pat->pat_type), v->id, param,
                        body);
  stub();
}
lambda::lambda for_multiple_match(scopes, const Location&, Slice<lambda::lambda>, Slice<PatAction>,
                                  typedtree::Partial) { stub(); }
lambda::lambda for_tupled_function(scopes, const Location&, Slice<Ident::t>, Slice<PatsAction>,
                                   typedtree::Partial) { stub(); }
lambda::lambda for_optional_arg_default(scopes, const Location&, const typedtree::Pattern*, lambda::lambda,
                                        Ident::t, lambda::lambda) { stub(); }
std::vector<const typedtree::Pattern*> flatten_pattern(long, const typedtree::Pattern*) { stub(); }
lambda::lambda inline_lazy_force(lambda::lambda, const lambda::ScopedLocation&) { stub(); }
}  // namespace cppcaml::typing::matching
