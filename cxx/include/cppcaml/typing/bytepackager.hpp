// Port of bytecomp/bytepackager.mli (cxx/PORTING.md stage 10): -pack, a
// set of .cmo (and .cmi) files as one .cmo having the original
// compilation units as sub-modules.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "cppcaml/typing/env.hpp"

namespace cppcaml::typing::bytepackager {

// package_files ~ppf_dump initial_env files targetfile
void package_files(env::t initial_env, const std::vector<std::string>& files, const std::string& targetfile);

struct Error : std::runtime_error {
  enum class Kind {
    Forward_reference,     // file, compunit
    Multiple_definition,   // file, compunit
    Not_an_object_file,    // file
    Illegal_renaming,      // compunit (name), file, compunit (id)
    File_not_found,        // file
    Inconsistent_import,   // Bytelink's: name, user, auth
  };
  Kind kind;
  std::string file, name, id, auth;
  Error(Kind k) : std::runtime_error("Bytepackager.Error"), kind(k) {}
};
// ocamlc's report ("Error: ..." text of report_error_doc)
std::string report_error(const Error& e);

}  // namespace cppcaml::typing::bytepackager
