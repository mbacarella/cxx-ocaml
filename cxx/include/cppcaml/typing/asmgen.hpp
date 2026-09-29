// Port of asmcomp/asmgen.ml: a function's way from Cmm through the Mach
// passes, register allocation and Linearize to Emit; compile_unit writes
// and assembles the assembly file.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cppcaml/typing/closure.hpp"
#include "cppcaml/typing/cmm.hpp"
#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::asmgen {

struct Error {  // Assembler_error of string
  std::string file;
};

// should_emit (): not stopping after scheduling
bool should_emit();
// compile_phrases ~ppf_dump ps: the -d dumps go to dump (raises
// Polling's PollError)
void compile_phrases(format::Formatter& dump, const std::vector<cmm::Phrase>& ps);
void compile_phrase(format::Formatter& dump, const cmm::Phrase& p);
// compile_unit ~asm_filename ~keep_asm ~obj_filename gen: gen returns the
// assembly text (Emit.end_assembly's); it is written to asm_filename and
// assembled into obj_filename (raises Error)
void compile_unit(const std::string& asm_filename, bool keep_asm, const std::string& obj_filename,
                  const std::function<std::string()>& gen);
// end_gen_implementation ~ppf_dump clambda: Cmmgen.compunit, the phrases
// compiled, then the references to the external primitives' symbols
// (Translmod.primitive_declarations); the assembly text
std::string end_gen_implementation(format::Formatter& dump, const closure_middle_end::WithConstants& clambda);
// asm_filename output_prefix: prefix.s with -S, else a temporary file
std::string asm_filename(const std::string& output_prefix);

}  // namespace cppcaml::typing::asmgen
