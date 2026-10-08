// An in-process assembler for the GNU as text c++ocamlopt's amd64 emitter
// writes (and Asmlink's startup file): the subset of AT&T syntax, the
// directives and the relocations ocamlopt uses, turned into the ELF object
// GNU as (binutils 2.42, x86-64) makes of the same text, byte for byte --
// its instruction encodings, its branch relaxation and NOP padding, its
// .eh_frame from the .cfi_* directives, its .debug_line from .file / .loc,
// its section, symbol and relocation order.
//
// It is not a general assembler: text outside that subset is an error
// (Unsupported), and the caller falls back to the system assembler.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cppcaml::typing::x86_assembler {

// Text this assembler does not handle (the caller runs the system's `as`)
struct Unsupported : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The ELF relocatable object for [text]; throws Unsupported.
// [debug_prefix_map]: as's --debug-prefix-map OLD=NEW pairs (DW_AT_comp_dir).
std::vector<std::uint8_t> assemble(std::string_view text,
                                   const std::vector<std::pair<std::string, std::string>>& debug_prefix_map = {});

}  // namespace cppcaml::typing::x86_assembler
