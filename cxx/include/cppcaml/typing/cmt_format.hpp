// Port of file_formats/cmt_format.ml (TYPECHECKER.md, driver options:
// -bin-annot): the .cmt / .cmti file -- the typed tree with its
// environments reduced to their summaries (clear_env), the unit's
// comments, command line, load path, digests, imports, the uid ->
// declaration index, the reduced shape -- marshaled with ocamlc's sharing.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/shape.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::cmt_format {

// binary_annots (the partial forms, which only a failed compilation saves,
// are not built)
struct BinaryAnnots {
  enum class Kind : std::uint8_t {
    Packed, Implementation, Interface, Partial_implementation, Partial_interface
  };
  Kind kind;
  const typedtree::Structure* structure = nullptr;  // Implementation
  const typedtree::Signature* signature = nullptr;  // Interface
  Signature packed_sg;                              // Packed
  std::vector<std::string> packed_files;            // Packed
};

// Lexer.comments (): the unit's comments, in source order (set by the
// driver after parsing the unit)
void set_comments(std::vector<std::pair<std::string_view, Location>> comments);
// Sys.argv (cmt_args), set by the driver
void set_argv(std::vector<std::string> argv);
// The source file's name as the unit's one string object: the argv element
// that is also Location.input_name, every position's pos_fname and
// cmt_sourcefile (the driver passes this view to the parsetree conversion)
void set_source_name(std::string_view name);

// Uid.Deps.record_declaration_dependency
enum class DepKind : std::uint8_t { Definition_to_declaration, Declaration_to_declaration };
void record_declaration_dependency(DepKind k, const Uid& uid1, const Uid& uid2);

// Cmt_format.clear (): the saved parts and Uid.Deps
void clear();

// save_cmt target binary_annots initial_env cmi shape, when
// Clflags.binary_annotations and not Clflags.print_types.  [filename] is
// the .cmt / .cmti (Unit_info.Artifact.filename target), [modname] its unit,
// [sourcefile] Unit_info.Artifact.input_source_file target.
void save_cmt(const std::string& filename, std::string_view modname, const std::optional<std::string>& sourcefile,
              const BinaryAnnots& annots, env::t initial_env, const cmi_format::CmiInfos* cmi,
              shape::t shape);

}  // namespace cppcaml::typing::cmt_format
