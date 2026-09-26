// The Clflags settings the typer reads (utils/clflags.ml), with ocamlc's
// defaults.  The c++ocamlc driver sets them from the command line.
#pragma once

namespace cppcaml::typing::clflags {
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
inline bool link_everything = false;       // -linkall
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
inline bool safer_matching = false;       // -safer-matching
}  // namespace cppcaml::typing::clflags
