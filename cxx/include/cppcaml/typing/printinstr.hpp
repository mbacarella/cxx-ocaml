// Port of bytecomp/printinstr.mli (TYPECHECKER.md stage 10): the -dinstr
// printer, on the Format engine port.
#pragma once

#include <string>

#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/instruct.hpp"

namespace cppcaml::typing::printinstr {

void instruction(format::Formatter& ppf, const instruct::Instruction& i);
void instrlist(format::Formatter& ppf, instruct::code c);

// what ocamlc prints for -dinstr: `Format.fprintf ppf_dump "%a@."
// Printinstr.instrlist code` on stderr's formatter (as printlambda::dump)
std::string dump(instruct::code c);

}  // namespace cppcaml::typing::printinstr
