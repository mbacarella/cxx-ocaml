// Port of asmcomp/asmlibrarian.ml: building a .cmxa library and its .a
// archive of object files (ocamlopt -a).
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/linkdeps.hpp"

namespace cppcaml::typing::asmlibrarian {

struct Error {
  enum class Kind { File_not_found, Archiver_error, Link_error } kind;
  std::string name;
  linkdeps::Error link{};
};

// create_archive file_list lib_name
void create_archive(const std::vector<std::string>& file_list, const std::string& lib_name);
void report_error_doc(format_doc::Formatter& ppf, const Error& e);

}  // namespace cppcaml::typing::asmlibrarian
