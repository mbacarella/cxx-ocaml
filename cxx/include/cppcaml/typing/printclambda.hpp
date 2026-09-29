// Port of middle_end/printclambda.ml and printclambda_primitives.ml.
#pragma once

#include <string>

#include "cppcaml/typing/clambda.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::printclambda {
void primitive(format::Formatter& ppf, const clambda::Primitive& p);  // Printclambda_primitives.primitive
void clambda(format::Formatter& ppf, clambda::ulambda l);              // "%a@." of the term
void approx(format::Formatter& ppf, const clambda::ValueApproximation* a);
std::string dump(clambda::ulambda l);  // clambda on a fresh formatter
}  // namespace cppcaml::typing::printclambda
