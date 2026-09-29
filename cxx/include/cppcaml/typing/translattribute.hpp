// Port of lambda/translattribute.mli (cxx/PORTING.md stage 10): the
// [@inline] / [@inlined] / [@specialise] / [@local] / [@tailcall] /
// [@poll] / [@tail_mod_cons] attributes on the Lambda terms they annotate.
#pragma once

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::translattribute {

lambda::lambda add_inline_attribute(lambda::lambda expr, const Location& loc, const parsetree::Attributes& attributes);
lambda::InlineAttribute get_inline_attribute(const parsetree::Attributes& attributes);
lambda::lambda add_specialise_attribute(lambda::lambda expr, const Location& loc,
                                        const parsetree::Attributes& attributes);
lambda::SpecialiseAttribute get_specialise_attribute(const parsetree::Attributes& attributes);
lambda::lambda add_local_attribute(lambda::lambda expr, const Location& loc, const parsetree::Attributes& attributes);
lambda::LocalAttribute get_local_attribute(const parsetree::Attributes& attributes);
lambda::InlineAttribute get_inlined_attribute(const typedtree::Expression* e);
lambda::InlineAttribute get_inlined_attribute_on_module(const typedtree::ModuleExpr* e);
lambda::SpecialiseAttribute get_specialised_attribute(const typedtree::Expression* e);
lambda::TailcallAttribute get_tailcall_attribute(const typedtree::Expression* e);
lambda::lambda add_function_attributes(lambda::lambda lam, const Location& loc,
                                       const parsetree::Attributes& attr);

}  // namespace cppcaml::typing::translattribute
