// Port of bytecomp/emitcode.mli (TYPECHECKER.md stage 10): the instruction
// list to relocatable bytecode, and the .cmo file (magic, code, debug info,
// optimization hints, the marshaled Cmo_format.compilation_unit).
#pragma once

#include <cstdio>
#include <string>

#include "cppcaml/typing/instruct.hpp"
#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::emitcode {

// to_file outchan artifact_info ~required_globals code: `filename` is the
// .cmo's path as Unit_info.Artifact.filename gives it, `modname` its
// Unit_info.Artifact.modname.  Reads Env.imports(),
// Translmod.primitive_declarations, Clflags.debug / link_everything.
void to_file(std::FILE* outchan, std::string_view filename, std::string_view modname,
             const lambda::IdentSet& required_globals, instruct::code code);

// to_memory / to_packed_file (the toplevel's, -pack's) are not ported yet.

}  // namespace cppcaml::typing::emitcode
