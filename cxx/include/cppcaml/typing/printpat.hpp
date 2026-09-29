// Port of typing/printpat.mli (cxx/PORTING.md stage 9): patterns (the
// counter-examples of the exhaustiveness warnings) printed as values.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::printpat {

std::string pretty_const(const typedtree::Constant& c);
void pretty_val(format_doc::Formatter& ppf, const typedtree::Pattern* v);
void top_pretty(format_doc::Formatter& ppf, const typedtree::Pattern* v);
void pretty_pat(format_doc::Formatter& ppf, const typedtree::Pattern* p);
void pretty_line(format_doc::Formatter& ppf, const std::vector<const typedtree::Pattern*>& line);
void pretty_matrix(format_doc::Formatter& ppf, const std::vector<std::vector<const typedtree::Pattern*>>& pss);

}  // namespace cppcaml::typing::printpat
