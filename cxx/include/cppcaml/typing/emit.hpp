// Ports of asmcomp/amd64/emit.mlp, asmcomp/emitaux.ml and the x86 assembly
// layer (x86_ast.mli, x86_dsl.ml, x86_proc.ml, x86_gas.ml): amd64 assembly
// text for GNU as, on Linux.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/cmm.hpp"
#include "cppcaml/typing/linear.hpp"

namespace cppcaml::typing::emit {

void begin_assembly();
void fundecl(const linear::Fundecl& f);
void data(const std::vector<cmm::DataItem>& l);
// end_assembly: the whole assembly file's text (X86_gas.generate_asm)
std::string end_assembly();

}  // namespace cppcaml::typing::emit
