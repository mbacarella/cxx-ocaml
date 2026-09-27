// Port of bytecomp/bytelink.ml: linking .cmo / .cma files into a bytecode
// executable (the launcher header, the sections), a custom runtime system
// (-custom, -make-runtime), or a C object / file / complete executable
// (-output-obj, -output-complete-obj, -output-complete-exe).
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/cmo_format.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/linkdeps.hpp"
#include "cppcaml/typing/symtable.hpp"

namespace cppcaml::typing::bytelink {

struct Error {
  enum class Kind {
    File_not_found, Not_an_object_file, Wrong_object_name, Symbol_error, Inconsistent_import, Custom_runtime,
    File_exists, Cannot_open_dll, Camlheader, Link_error, Needs_custom_runtime
  } kind;
  std::string a, b, c;           // the file / modname and files / message and header
  symtable::Error symbol{};      // Symbol_error
  linkdeps::Error link{};        // Link_error
};

// link objfiles output_name
void link(const std::vector<std::string>& objfiles, const std::string& output_name);

// check_consistency file_name cu (Bytelibrarian's too)
void check_consistency(const std::string& file_name, const cmo_format::CompUnit& cu);
// linkdeps_unit ldeps ~filename compunit
void linkdeps_unit(linkdeps::T& ldeps, const std::string& filename, const cmo_format::CompUnit& cu);

void report_error_doc(format_doc::Formatter& ppf, const Error& e);
void reset();

}  // namespace cppcaml::typing::bytelink
