// Port of typing/includeclass.mli (cxx/PORTING.md, stage 5): inclusion of
// class types and class declarations.  Reporting comes with Printtyp.
#pragma once

#include <vector>

#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/out_type.hpp"

namespace cppcaml::typing::includeclass {

std::vector<ctype::ClassMatchFailure> class_types(env::t env, const ClassType* cty1, const ClassType* cty2);
std::vector<ctype::ClassMatchFailure> class_type_declarations(const Location& loc, env::t env,
                                                              const ClassTypeDeclaration* decl1,
                                                              const ClassTypeDeclaration* decl2);
std::vector<ctype::ClassMatchFailure> class_declarations(env::t env, const ClassDeclaration* decl1,
                                                         const ClassDeclaration* decl2);

// report_error_doc mode ppf errs
void report_error_doc(out_type::Mode mode, format_doc::Formatter& ppf, const std::vector<ctype::ClassMatchFailure>& errs);

}  // namespace cppcaml::typing::includeclass
