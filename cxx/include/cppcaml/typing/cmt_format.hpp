// Port of file_formats/cmt_format.ml (cxx/PORTING.md, driver options:
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

// binary_part: a piece of typed tree the typer saved (add_saved_type)
struct BinaryPart {
  enum class Kind : std::uint8_t {
    Partial_structure, Partial_structure_item, Partial_expression, Partial_pattern, Partial_class_expr,
    Partial_signature, Partial_signature_item, Partial_module_type
  };
  Kind kind;
  bool computation = false;  // Partial_pattern's category: Value | Computation
  const void* node;
};
// the saved types, an immutable list as Cmt_format's (head: the latest)
struct SavedTypes {
  BinaryPart part;
  const SavedTypes* next;
};
using saved_types_t = const SavedTypes*;
saved_types_t get_saved_types();
void set_saved_types(saved_types_t l);
void add_saved_type(BinaryPart p);
saved_types_t cons_saved_type(BinaryPart p, saved_types_t l);
// Typing_recovery_state.with_saved_types ~save_part f (without
// -typing-recovery): on success the parts [f] saved give way to the one
// [save_part] makes of its result; an exception leaves [f]'s own list
// (the outer one is lost)
template <class F, class S>
auto with_saved_types(F&& f, S&& save_part) {
  saved_types_t saved = get_saved_types();
  set_saved_types(nullptr);
  auto result = f();
  set_saved_types(cons_saved_type(save_part(result), saved));
  return result;
}

// binary_annots (Partial_interface is never built: an interface's type
// error writes no .cmti)
struct BinaryAnnots {
  enum class Kind : std::uint8_t {
    Packed, Implementation, Interface, Partial_implementation, Partial_interface
  };
  Kind kind;
  const typedtree::Structure* structure = nullptr;  // Implementation
  const typedtree::Signature* signature = nullptr;  // Interface
  Signature packed_sg;                              // Packed
  std::vector<std::string> packed_files;            // Packed
  saved_types_t parts = nullptr;                    // Partial_implementation (Array.of_list)
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

// Typemod.gen_annot: Cmt2annot's walk over an implementation's annots (its
// Env lookups; the .annot file itself, -annot, is not ported)
void gen_annot(const BinaryAnnots& annots);

}  // namespace cppcaml::typing::cmt_format
