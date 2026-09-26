// Port of lambda/printlambda.mli (TYPECHECKER.md stage 10): the -drawlambda
// / -dlambda printer, on the Format engine port (format.hpp).
#pragma once

#include <string>

#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::printlambda {

void integer_comparison(format::Formatter& ppf, lambda::IntegerComparison c);
void float_comparison(format::Formatter& ppf, lambda::FloatComparison c);
void structured_constant(format::Formatter& ppf, const lambda::StructuredConstant* c);
void lambda(format::Formatter& ppf, lambda::lambda l);
void program(format::Formatter& ppf, const lambda::Program& p);
void primitive(format::Formatter& ppf, const lambda::Primitive& p);
std::string name_of_primitive(const lambda::Primitive& p);
void value_kind(format::Formatter& ppf, const lambda::ValueKind& k);
void block_shape(format::Formatter& ppf, const lambda::BlockShape& s);
void record_rep(format::Formatter& ppf, const RecordRepresentation& r);

// what ocamlc prints for -drawlambda / -dlambda: `Format.fprintf ppf_dump
// "%a@." Printlambda.lambda lam` on stderr's formatter (margin 78)
std::string dump(lambda::lambda l);

}  // namespace cppcaml::typing::printlambda
