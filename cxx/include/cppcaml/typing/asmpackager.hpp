// Port of asmcomp/asmpackager.ml: "package" a set of .cmx/.o files into one
// .cmx/.o file having the original compilation units as sub-modules
// (ocamlopt -pack).
#pragma once

#include <ostream>
#include <string>
#include <vector>

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/format_doc.hpp"

namespace cppcaml::typing::asmpackager {

struct Error {
  enum class Kind {
    Illegal_renaming, Forward_reference, Wrong_for_pack, Linking_error, Assembler_error, File_not_found
  } kind;
  std::string a, b, c;  // Illegal_renaming (name, file, id) / Forward_reference (file, ident) /
                        // Wrong_for_pack (file, path) / Assembler_error, File_not_found (file)
};

// package_files ~ppf_dump initial_env files targetcmx
void package_files(std::ostream& ppf_dump, env::t initial_env, const std::vector<std::string>& files,
                   const std::string& targetcmx);
void report_error_doc(format_doc::Formatter& ppf, const Error& e);

}  // namespace cppcaml::typing::asmpackager
