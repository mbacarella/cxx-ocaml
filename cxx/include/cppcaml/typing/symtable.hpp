// Port of bytecomp/symtable.ml, the batch linker's half: numbering the
// globals and the C primitives, patching object code, the initial global
// data and the global map (the toplevel's functions are not ported).
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/cmo_format.hpp"
#include "cppcaml/typing/format_doc.hpp"

namespace cppcaml::typing::symtable {

// Global.t = Glob_compunit of compunit | Glob_predef of predef
struct Global {
  enum class K { Glob_compunit, Glob_predef } k;
  std::string name;
  cmo_format::V obj;  // the name's string object (what the global map marshals)
};

struct Error {
  enum class Kind { Undefined_global, Unavailable_primitive, Wrong_vm, Uninitialized_global } kind;
  Global global;  // Undefined_global / Uninitialized_global
  std::string s;  // Unavailable_primitive / Wrong_vm
};

// Initialization for batch linking: the predefined exceptions, the known
// C primitives (-use-prims, -use-runtime's `-p`, or Runtimedef's)
void init();
// patch_object buff patchlist (raises Error)
void patch_object(std::string& buff, const std::vector<cmo_format::Reloc>& patchlist);
// require_primitive name
void require_primitive(const std::string& name);
// output_primitive_names: the names, NUL-terminated
std::string primitive_names();
// output_primitive_table: the C declarations of -custom's primitive table
std::string primitive_table();
// initial_global_table (): the global data, literals filled in
cmo_format::V initial_global_table();
// output_global_map / data_global_map: the global map, as marshaled
cmo_format::V data_global_map();
// required_compunits patchlist (Reloc_getcompunit's names, the latest first)
std::vector<std::string> required_compunits(const std::vector<cmo_format::Reloc>& patchlist);

void report_error_doc(format_doc::Formatter& ppf, const Error& e);
void reset();

}  // namespace cppcaml::typing::symtable
