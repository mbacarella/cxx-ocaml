// Port of asmcomp/printcmm.ml: pretty-printing of C-- code (-dcmm).
#pragma once

#include "cppcaml/typing/cmm.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::printcmm {
void machtype(format::Formatter& ppf, cmm::Machtype mty);
void print_expression(format::Formatter& ppf, cmm::expression e);  // Printcmm.expression
void fundecl(format::Formatter& ppf, const cmm::Fundecl& f);
void data(format::Formatter& ppf, const std::vector<cmm::DataItem>& dl);
void phrase(format::Formatter& ppf, const cmm::Phrase& p);
}  // namespace cppcaml::typing::printcmm
