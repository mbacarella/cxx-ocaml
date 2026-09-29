// Port of lambda/printlambda.mli (cxx/PORTING.md stage 10): the -drawlambda
// / -dlambda printer, on the Format engine port (format.hpp).
#pragma once

#include <string>

#include "cppcaml/typing/clflags.hpp"
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
void print_bigarray(std::string_view name, bool unsafe, lambda::BigarrayKind kind, format::Formatter& ppf,
                    lambda::BigarrayLayout layout);

// ---- additions (not in printlambda.mli) ----
// Clflags read by the printer, with ocamlc's defaults (-d(no-)locations,
// -d(no-)unique-ids); they belong in clflags.hpp.  (-dcanonical-ids is not
// ported: Clflags.canonical_ids is taken as false.)
namespace flags {
inline bool& locations = clflags::locations;
inline bool& unique_ids = clflags::unique_ids;
}  // namespace flags
// Ident.print as the compiler's printers call it (Format_doc.compat
// doc_print: ~with_scope:false, honouring flags::unique_ids) -- `name/stamp`
void ident(format::Formatter& ppf, Ident::t id);
// Printtyp.path, used by record_rep's `ext(...)`; defaults to Path.name
// until Printtyp is ported (stage 9), which should install its printer here.
extern void (*print_path)(format::Formatter& ppf, Path::t p);

// what ocamlc prints for -drawlambda / -dlambda: `Format.fprintf ppf_dump
// "%a@." Printlambda.lambda lam` on stderr's formatter (margin 78)
std::string dump(lambda::lambda l);

}  // namespace cppcaml::typing::printlambda
