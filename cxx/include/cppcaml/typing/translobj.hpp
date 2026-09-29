// Port of lambda/translobj.mli (cxx/PORTING.md stage 10): helpers for the
// object and class translation (CamlinternalOO primitives, label caches).
#pragma once

#include <functional>
#include <utility>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translobj {

lambda::lambda oo_prim(std::string_view name);
lambda::lambda share(const lambda::StructuredConstant* c);
std::pair<lambda::lambda, Slice<lambda::lambda>> meth(lambda::lambda obj, std::string_view lab);

void reset_labels();
lambda::lambda transl_label_init(const std::function<lambda::lambda()>& f);
// transl_store_label_init glob size f arg (the 'a is erased into the closure)
std::pair<long, lambda::lambda> transl_store_label_init(Ident::t glob, long size,
                                                        const std::function<lambda::lambda()>& f);

extern lambda::IdentSet method_ids;  // reset when starting a new wrapper

lambda::lambda oo_wrap(env::t env, bool req, const std::function<lambda::lambda()>& f);
// oo_wrap_gen, at its one instance ('b = recursive_binding_kind)
std::pair<lambda::lambda, typedtree::RecursiveBindingKind> oo_wrap_gen(
    env::t env, bool req, const std::function<std::pair<lambda::lambda, typedtree::RecursiveBindingKind>()>& f);
std::pair<env::t, bool> oo_add_class(Ident::t id);

void reset();

}  // namespace cppcaml::typing::translobj
