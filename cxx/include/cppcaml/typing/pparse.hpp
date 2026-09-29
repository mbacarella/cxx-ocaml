// Port of driver/pparse.ml's binary-AST half (cxx/PORTING.md "Driver
// options"): the AST files Pparse reads in place of a source (a dune ppx
// driver's `-impl foo.pp.ml`), and -ppx rewriters (Pparse.apply_rewriters
// with Ast_mapper's ppx context).  The -pp half (call_external_preprocessor)
// is the driver's read_source.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::pparse {

enum class AstKind : std::uint8_t { Structure, Signature };

// magic_of_kind: Config.ast_impl_magic_number / ast_intf_magic_number
std::string magic_of_kind(AstKind k);

// Pparse.Error
struct Error : std::runtime_error {
  enum class Kind : std::uint8_t { CannotRun, WrongMagic };
  Kind kind;
  std::string cmd;
  Error(Kind k, std::string c) : std::runtime_error("Pparse.Error"), kind(k), cmd(std::move(c)) {}
};

// open_and_check_magic: [contents] (a source file's, after -pp) is a
// binary AST of [kind].  The magic of another version (same first 9
// characters): Misc.fatal_error "OCaml and preprocessor have incompatible
// versions".
bool is_ast_file(const std::string& contents, AstKind kind);

// file_aux's is_ast_file branch: Location.input_name := the recorded name,
// the lexbuf of that file for error excerpts (if it can be read), the
// Unsafe_array_syntax_without_parsing warning under -unsafe, the AST
// (input_value's sharing, parsetree_of_ovalue) and -- when no -ppx is given
// -- Ast_invariants' checks.
parsetree::Structure read_ast_structure(const std::string& contents);
parsetree::Signature read_ast_signature(const std::string& contents);

// apply_rewriters ~restore:false ~tool_name: the identity without -ppx
parsetree::Structure apply_rewriters_str(parsetree::Structure ast, std::string_view tool_name);
parsetree::Signature apply_rewriters_sig(parsetree::Signature ast, std::string_view tool_name);

}  // namespace cppcaml::typing::pparse
