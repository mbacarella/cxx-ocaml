// Port of bytecomp/emitcode.mli (cxx/PORTING.md stage 10): the instruction
// list to relocatable bytecode, and the .cmo file (magic, code, debug info,
// optimization hints, the marshaled Cmo_format.compilation_unit).
#pragma once

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/omarshal.hpp"

#include "cppcaml/typing/instruct.hpp"
#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::emitcode {

// to_file outchan artifact_info ~required_globals code: `filename` is the
// .cmo's path as Unit_info.Artifact.filename gives it, `modname` its
// Unit_info.Artifact.modname.  Reads Env.imports(),
// Translmod.primitive_declarations, Clflags.debug / link_everything.
void to_file(std::FILE* outchan, std::string_view filename, std::string_view modname,
             const lambda::IdentSet& required_globals, instruct::code code);

// ---- to_packed_file (Bytepackager's) ----
// OCaml marshals the packed unit's descriptor from values it holds: the
// member units' descriptors as read back, and what to_packed_file returns.
// Here those values are omarshal values built in the caller's
// ValueContext, so that a string OCaml shares stays one value (a member's
// renamed Reloc_setcompunit and the package's Reloc_getcompunit share the
// packed name's string).
class ValueContext {
 public:
  ValueContext();
  ~ValueContext();
  ValueContext(const ValueContext&) = delete;
  ValueContext& operator=(const ValueContext&) = delete;
  omarshal::ValPtr str(std::string_view s);  // one value per storage
  struct Impl;
  std::unique_ptr<Impl> impl;
};

struct PackedFile {
  long size;                                                    // !out_position
  std::vector<std::pair<omarshal::ValPtr, long>> relocs;        // List.rev !reloc_info: (reloc_info, pos)
  std::vector<instruct::DebugEvent*> events;                    // !events (newest first)
  std::set<std::string> debug_dirs;                             // !debug_dirs
  std::vector<std::pair<long, omarshal::ValPtr>> hints;         // !hints (newest first)
};
// to_packed_file outchan code: the code is appended to [out]; the relocs
// are built in [w], the hints in [hw].  (to_memory, the toplevel's, is not
// ported.)
PackedFile to_packed_file(std::string& out, instruct::code code, ValueContext& w, ValueContext& hw);

}  // namespace cppcaml::typing::emitcode
