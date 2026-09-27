// The Clflags settings the typer reads (utils/clflags.ml), with ocamlc's
// defaults.  The c++ocamlc driver sets them from the command line.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cppcaml::typing::clflags {
inline std::optional<std::string> for_package;  // -for-pack
inline bool dump_lambda = false;             // -dlambda
inline bool principal = false;             // -principal
inline bool recursive_types = false;       // -rectypes
inline bool strict_sequence = false;       // -strict-sequence
inline bool strict_formats = true;         // -strict-formats
inline bool applicative_functors = true;   // -no-app-funct
inline bool no_alias_deps = false;         // -no-alias-deps
inline bool classic = false;               // -nolabels
inline bool nopervasives = false;          // -nopervasives
inline bool no_std_include = false;        // -nostdlib
inline bool unsafe = false;                // -unsafe
inline bool noassert = false;              // -noassert
inline bool debug = false;                 // -g
inline bool absname = false;               // -absname (Location.show_filename)
inline bool link_everything = false;       // -linkall
inline bool print_variance = false;        // -i-variance
inline bool real_paths = true;             // -short-paths clears it
inline bool print_types = false;           // -i
inline bool keep_locs = true;              // -keep-locs
inline bool keep_docs = false;             // -keep-docs
inline bool native_code = false;
inline bool unboxed_types = false;
inline bool typing_recovery = false;      // -typing-recovery
inline bool opaque = false;               // -opaque
inline bool dont_write_files = false;
inline bool locations = true;             // -dno-locations
inline bool unique_ids = true;            // -dno-unique-ids
inline bool annotations = false;          // -annot
inline bool afl_instrument = false;       // -afl-instrument (native)
inline long match_context_rows = 32;      // -match-context-rows
inline long error_size = 500;              // -error-size
inline bool safer_matching = false;       // -safer-matching

// ---- the driver's (Main_args / Compenv / Bytelink) ----
// lists are kept as Clflags keeps them: the latest option first
inline std::vector<std::string> include_dirs;          // -I
inline std::vector<std::string> hidden_include_dirs;   // -H
inline std::vector<std::string> open_modules;          // -open
inline std::vector<std::string> objfiles;              // .cmo / .cma inputs and outputs
inline std::vector<std::string> ccobjs;                // .o / .a inputs, -cclib
inline std::vector<std::pair<bool, std::string>> dllibs;  // -dllib (suffixed, name)
inline std::vector<std::string> dllpaths;              // -dllpath (in order)
inline std::vector<std::string> all_ccopts;            // -ccopt
inline std::vector<std::string> all_ppx;               // -ppx
inline std::optional<std::string> preprocessor;       // -pp
inline std::optional<std::string> output_name;        // -o
inline std::optional<std::string> cmi_file;           // -cmi-file
inline std::optional<std::string> c_compiler;         // -cc
inline std::optional<std::string> keyword_edition;    // -keywords
inline std::optional<std::string> dump_dir;           // -dump-dir
inline std::optional<std::string> standard_library_default_override;  // -set-runtime-default
inline bool make_archive = false;          // -a
inline bool make_package = false;          // -pack
inline bool compile_only = false;          // -c
inline bool no_cwd = false;                // -nocwd
inline bool binary_annotations = false;    // -bin-annot
inline bool store_occurrences = false;     // -bin-annot-occurrences
inline bool dump_parsetree = false;        // -dparsetree
inline bool dump_source = false;           // -dsource
inline bool dump_typedtree = false;        // -dtypedtree
inline bool dump_shape = false;            // -dshape
inline bool dump_matchcomp = false;        // -dmatchcomp
inline bool dump_rawlambda = false;        // -drawlambda
inline bool dump_instr = false;            // -dinstr
inline bool dump_into_file = false;        // -dump-into-file
inline bool parsetree_ghost_loc_invariant = false;  // -dparsetree-loc-ghost-invariants
inline bool canonical_ids = false;         // -dcanonical-ids
inline bool keep_camlprimc_file = false;   // -dcamlprimc
inline bool profile = false;               // -dtimings / -dprofile (profile_columns <> [])
inline bool no_auto_link = false;          // -noautolink
inline bool plugin = false;                // -plugin
inline bool use_threads = false;           // -thread
inline bool verbose = false;               // -verbose
inline bool with_runtime = true;           // -without-runtime
inline bool bytecode_hints = false;        // -bytecode-hints
inline bool bytecode_compatible_32 = false;  // -compat-32
inline bool custom_runtime = false;        // -custom
inline bool make_runtime = false;          // -make-runtime
inline bool no_check_prims = false;        // -no-check-prims
inline bool output_c_object = false;       // -output-obj
inline bool output_complete_object = false;   // -output-complete-obj
inline bool output_complete_executable = false;  // -output-complete-exe
inline bool launch_method_set = false;     // -launch-method
inline bool search_method_set = false;     // -runtime-search (not "disable")
inline std::string runtime_variant;        // -runtime-variant
inline std::string use_prims;              // -use-prims
inline std::string use_runtime;            // -use-runtime
enum class Color : std::uint8_t { Auto, Always, Never };
inline std::optional<Color> color;         // -color / OCAML_COLOR
enum class ErrorStyle : std::uint8_t { Contextual, Short };
inline std::optional<ErrorStyle> error_style;  // -error-style / OCAML_ERROR_STYLE
// Compiler_pass (bytecode's): -stop-after
enum class Pass : std::uint8_t { Parsing, Typing, Lambda };
inline std::optional<Pass> stop_after;
// Clflags.should_stop_after pass (-i stops after typing too)
inline bool should_stop_after(Pass pass) {
  if (Pass::Typing <= pass && print_types) return true;
  return stop_after && *stop_after <= pass;
}
}  // namespace cppcaml::typing::clflags
