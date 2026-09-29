// Port of asmcomp/asmlink.ml: linking .cmx / .cmxa files and their object
// files into an executable (or -output-obj / -output-complete-obj), with
// the startup module (the generic functions, caml_program, the global
// tables) compiled on the fly.
#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "cppcaml/typing/cmx_format.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/linkdeps.hpp"

namespace cppcaml::typing::asmlink {

struct Error {
  enum class Kind {
    File_not_found, Not_an_object_file, Inconsistent_interface, Inconsistent_implementation, Assembler_error,
    Linking_error, Missing_cmx, Link_error
  } kind;
  std::string a, b, c;       // the file / modname and files
  int exitcode = 0;          // Linking_error
  linkdeps::Error link{};    // Link_error
};

// link ~ppf_dump objfiles output_name (the -dcmm... dumps of the startup
// module on ppf_dump)
void link(std::ostream& ppf_dump, const std::vector<std::string>& objfiles, const std::string& output_name);
// link_shared ~ppf_dump objfiles output_name (-shared: a .cmxs plugin)
void link_shared(std::ostream& ppf_dump, const std::vector<std::string>& objfiles, const std::string& output_name);
// check_consistency file_name unit crc (Asmlibrarian's too)
void check_consistency(const std::string& file_name, const cmx_format::UnitInfos& unit, const std::string& crc);
// extract_crc_interfaces () / extract_crc_implementations () (Asmpackager)
cmx_format::Crcs extract_crc_interfaces();
cmx_format::Crcs extract_crc_implementations();
void report_error_doc(format_doc::Formatter& ppf, const Error& e);
void reset();

}  // namespace cppcaml::typing::asmlink
