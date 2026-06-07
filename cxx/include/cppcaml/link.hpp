// Bytelink: link .cmo / .cma objects into a runnable bytecode executable
// (bytecomp/bytelink.ml + symtable.ml).  Assigns global slots and C-primitive
// numbers, resolves relocations, concatenates the code, and writes the
// sectioned exec image (CODE / PRIM / DATA + trailer) that ocamlrun runs.
#pragma once

#include <string>
#include <vector>

namespace cppcaml::link {

// Link `inputs` (paths to .cmo and .cma files, in order) into an executable at
// `out_path`.  If `runtime_path` is non-empty, a `#!<runtime>` line is prepended
// so the file is directly executable; otherwise run it as `ocamlrun <out>`.
void link_executable(const std::vector<std::string>& inputs,
                     const std::string& out_path,
                     const std::string& runtime_path);

}  // namespace cppcaml::link
