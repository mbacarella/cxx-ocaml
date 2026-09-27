// Port of typing/includemod_errorprinter.mli (TYPECHECKER.md stage 9): the
// messages of Includemod.Error / Apply_error and of first-class module
// coercions.
#pragma once

#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/location.hpp"

namespace cppcaml::typing::includemod_errorprinter {

// err_msgs ppf (env, err)
void err_msgs(format_doc::Formatter& ppf, const includemod::Explanation& e);
format_doc::Doc coercion_in_package_subtype(env::t env, const ModuleType* mty, const typedtree::ModuleCoercion* c);
location::Report report_error_doc(const includemod::Explanation& err);
location::Report report_apply_error_doc(const Location& loc, env::t env, const includemod::ApplicationName& app_name,
                                        const ModuleType* mty_f, const std::vector<includemod::AppArg>& args);

}  // namespace cppcaml::typing::includemod_errorprinter
