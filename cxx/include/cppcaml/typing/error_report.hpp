// The typing error an exception carries, as a constructor name and a
// location (the typing_dump oracles' `ERR` lines; c++ocamlc's error report
// until Printtyp is ported).
#pragma once

#include <exception>
#include <optional>
#include <string>

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/typetexp.hpp"

namespace cppcaml::typing::error_report {

struct Report {
  std::string name;             // e.g. "Typecore.Expr_type_clash"
  std::optional<Location> loc;  // none for the errors that carry no location here
  std::string detail;           // a debugging hint (payload kinds), not ocamlc's message
  // where ocamlc's report_error prints the error when it is not [loc]:
  // Location.none for Typedecl's Duplicate_label (no ~loc), the whole
  // application for Typecore's Apply_non_function of a function
  std::optional<Location> printed_loc;
};
// nullopt: not a typing error
std::optional<Report> classify(std::exception_ptr ep);
// Location.Doc.loc without styling: `File "f", line 3, characters 4-7`;
// input_name stands for an empty pos_fname (Location.input_name)
std::string format_loc(const Location& loc, const std::string& input_name);

const char* texp_error_name(typetexp::Error::Kind k);
const char* lookup_error_name(env::LookupError::Kind k);
extern const char* const tc_error_names[];
extern const char* const td_error_names[];
extern const char* const tm_error_names[];
extern const char* const tcl_error_names[];

}  // namespace cppcaml::typing::error_report
