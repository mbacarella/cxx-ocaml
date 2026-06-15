// Emitcode: serialize a bytecode instruction stream to a `.cmo` object file
// (bytecomp/emitcode.ml).  The instruction list is encoded to relocatable
// bytecode (with the `emit` peephole fusions), and wrapped in the .cmo container
// (magic + code + a marshaled Cmo_format.compilation_unit descriptor).
#pragma once

#include <string>
#include <vector>

#include "cppcaml/bytecode.hpp"

namespace cppcaml::cmo {

// Write a runnable .cmo for `code` (the module's instruction stream) to `path`.
// `required_compunits` are modules linked even though only referenced via a C
// external (cu_required_compunits), so the linker pulls their dependency chains.
void write_cmo(const bytecode::Code& code, const std::string& module_name,
               const std::string& path,
               const std::vector<std::string>& required_compunits = {});

}  // namespace cppcaml::cmo
