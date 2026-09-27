// Port of bytecomp/bytelibrarian.ml: building a .cma library (ocamlc -a)
// from .cmo / .cma files, with the C objects, options and DLLs it records.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/linkdeps.hpp"

namespace cppcaml::typing::bytelibrarian {

struct Error {
  enum class Kind { File_not_found, Not_an_object_file, Link_error } kind;
  std::string name;
  linkdeps::Error link{};
};

// create_archive file_list lib_name
void create_archive(const std::vector<std::string>& file_list, const std::string& lib_name);

void report_error_doc(format_doc::Formatter& ppf, const Error& e);
void reset();

}  // namespace cppcaml::typing::bytelibrarian
