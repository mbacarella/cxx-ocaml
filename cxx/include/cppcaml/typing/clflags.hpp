// The Clflags settings the typer reads (utils/clflags.ml), with ocamlc's
// defaults.  The c++ocamlc driver sets them from the command line.
#pragma once

#include "cppcaml/typing/arg_helper.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"
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
inline bool afl_instrument = config::afl_instrument;  // -afl-instrument (native)
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
// target_bindir / launch_method / search_method: unset = the Config default
inline std::optional<std::string> target_bindir;                 // -launch-method "<m> <bindir>"
inline std::optional<config::LaunchMethod> launch_method;        // -launch-method
inline std::optional<config::SearchMethod> search_method;        // -runtime-search
inline std::string runtime_variant;        // -runtime-variant
inline std::string use_prims;              // -use-prims
inline std::string use_runtime;            // -use-runtime
enum class Color : std::uint8_t { Auto, Always, Never };
inline std::optional<Color> color;         // -color / OCAML_COLOR
enum class ErrorStyle : std::uint8_t { Contextual, Short };
inline std::optional<ErrorStyle> error_style;  // -error-style / OCAML_ERROR_STYLE
// ---- the native compiler's ----
inline bool keep_asm_file = false;          // -S
inline bool optimize_for_speed = true;      // -compact
inline bool dump_rawclambda = false;        // -drawclambda
inline bool dump_clambda = false;           // -dclambda
inline bool dump_rawflambda = false;        // -drawflambda
inline bool dump_flambda = false;           // -dflambda
inline std::optional<long> dump_flambda_let;  // -dflambda-let=...
inline bool dump_flambda_verbose = false;   // -dflambda-verbose
inline bool dump_cmm = false;               // -dcmm
inline bool dump_selection = false;         // -dsel
inline bool dump_combine = false;           // -dcombine
inline bool dump_cse = false;               // -dcse
inline bool dump_live = false;              // -dlive
inline bool dump_spill = false;             // -dspill
inline bool dump_split = false;             // -dsplit
inline bool dump_interf = false;            // -dinterf
inline bool dump_prefer = false;            // -dprefer
inline bool dump_interval = false;          // -dinterval
inline bool dump_regalloc = false;          // -dalloc
inline bool dump_reload = false;            // -dreload
inline bool dump_scheduling = false;        // -dscheduling
inline bool dump_linear = false;            // -dlinear
inline bool keep_startup_file = false;      // -dstartup
inline bool clambda_checks = false;         // -clambda-checks
inline bool cmm_invariants = config::with_cmm_invariants;  // -dcmm-invariants
inline bool flambda_invariant_checks = config::with_flambda_invariants;  // -dflambda-(no-)invariants
inline bool insn_sched = true;              // -[no-]insn-sched
inline bool use_linscan = false;            // -linscan
inline bool float_const_prop = true;        // -no-float-const-prop
inline bool shared = false;                 // -shared
inline bool dlcode = true;                  // not -nodynlink
inline bool pic_code = config::architecture == "amd64" || config::architecture == "s390x";  // -fPIC
inline long afl_inst_ratio = 100;           // -afl-inst-ratio
inline bool function_sections = false;      // -function-sections
inline std::optional<long> simplify_rounds;  // -rounds
inline long default_simplify_rounds = 1;
inline long rounds() { return simplify_rounds ? *simplify_rounds : default_simplify_rounds; }
inline bool classic_inlining = false;       // -Oclassic
inline bool inlining_report = false;        // -inlining-report
inline bool unbox_specialised_args = true;  // -no-unbox-specialised-args
inline bool unbox_free_vars_of_closures = true;
inline bool unbox_closures = false;         // -unbox-closures
inline long unbox_closures_factor = 10;     // -unbox-closures-factor
inline bool remove_unused_arguments = false;  // -remove-unused-arguments
inline std::vector<std::string> dumped_passes_list;  // -dump-pass (latest first)
inline std::vector<std::string> all_passes;          // registered by the passes

inline const double default_inline_threshold = config::flambda ? 10. : 10. / 8.;
inline const long inline_toplevel_multiplier = 16;
inline const long default_inline_toplevel_threshold =
    static_cast<long>(static_cast<double>(inline_toplevel_multiplier) * default_inline_threshold);
inline const long default_inline_call_cost = 5;
inline const long default_inline_alloc_cost = 7;
inline const long default_inline_prim_cost = 3;
inline const long default_inline_branch_cost = 5;
inline const long default_inline_indirect_cost = 4;
inline const double default_inline_branch_factor = 0.1;
inline const long default_inline_lifting_benefit = 1300;
inline const long default_inline_max_unroll = 0;
inline const long default_inline_max_depth = 1;

using IntArg = arg_helper::Parsed<long>;
using FloatArg = arg_helper::Parsed<double>;
inline FloatArg inline_threshold = arg_helper::default_(default_inline_threshold);
inline IntArg inline_toplevel_threshold = arg_helper::default_(default_inline_toplevel_threshold);
inline IntArg inline_call_cost = arg_helper::default_(default_inline_call_cost);
inline IntArg inline_alloc_cost = arg_helper::default_(default_inline_alloc_cost);
inline IntArg inline_prim_cost = arg_helper::default_(default_inline_prim_cost);
inline IntArg inline_branch_cost = arg_helper::default_(default_inline_branch_cost);
inline IntArg inline_indirect_cost = arg_helper::default_(default_inline_indirect_cost);
inline FloatArg inline_branch_factor = arg_helper::default_(default_inline_branch_factor);
inline IntArg inline_lifting_benefit = arg_helper::default_(default_inline_lifting_benefit);
inline IntArg inline_max_unroll = arg_helper::default_(default_inline_max_unroll);
inline IntArg inline_max_depth = arg_helper::default_(default_inline_max_depth);

struct InliningArguments {  // inlining_arguments (None: the default)
  std::optional<long> inline_call_cost, inline_alloc_cost, inline_prim_cost, inline_branch_cost,
      inline_indirect_cost, inline_lifting_benefit;
  std::optional<double> inline_branch_factor;
  std::optional<long> inline_max_depth, inline_max_unroll;
  std::optional<double> inline_threshold;
  std::optional<long> inline_toplevel_threshold;
};
template <class V>
void set_arg(std::optional<long> round, arg_helper::Parsed<V>& arg, V dflt, std::optional<V> value) {
  V v = value ? *value : dflt;
  if (!round) arg = arg_helper::set_base_default(v, arg_helper::reset_base_overrides(arg));
  else arg = arg_helper::add_base_override(*round, v, arg);
}
inline void use_inlining_arguments_set(const InliningArguments& a, std::optional<long> round = std::nullopt) {
  set_arg(round, inline_call_cost, default_inline_call_cost, a.inline_call_cost);
  set_arg(round, inline_alloc_cost, default_inline_alloc_cost, a.inline_alloc_cost);
  set_arg(round, inline_prim_cost, default_inline_prim_cost, a.inline_prim_cost);
  set_arg(round, inline_branch_cost, default_inline_branch_cost, a.inline_branch_cost);
  set_arg(round, inline_indirect_cost, default_inline_indirect_cost, a.inline_indirect_cost);
  set_arg(round, inline_lifting_benefit, default_inline_lifting_benefit, a.inline_lifting_benefit);
  set_arg(round, inline_branch_factor, default_inline_branch_factor, a.inline_branch_factor);
  set_arg(round, inline_max_depth, default_inline_max_depth, a.inline_max_depth);
  set_arg(round, inline_max_unroll, default_inline_max_unroll, a.inline_max_unroll);
  set_arg(round, inline_threshold, default_inline_threshold, a.inline_threshold);
  set_arg(round, inline_toplevel_threshold, default_inline_toplevel_threshold, a.inline_toplevel_threshold);
}
// o1 is the default
inline const InliningArguments o1_arguments{};
inline const InliningArguments classic_arguments = [] {
  InliningArguments a;
  // [inline_threshold] matches the current compiler's default.  Note that
  // this particular fraction can be expressed exactly in floating point.
  a.inline_threshold = 10. / 8.;
  // [inline_toplevel_threshold] is not used in classic mode.
  a.inline_toplevel_threshold = 1;
  return a;
}();
inline const InliningArguments o2_arguments = [] {
  InliningArguments a;
  a.inline_call_cost = 2 * default_inline_call_cost;
  a.inline_alloc_cost = 2 * default_inline_alloc_cost;
  a.inline_prim_cost = 2 * default_inline_prim_cost;
  a.inline_branch_cost = 2 * default_inline_branch_cost;
  a.inline_indirect_cost = 2 * default_inline_indirect_cost;
  a.inline_max_depth = 2;
  a.inline_threshold = 25.;
  a.inline_toplevel_threshold = 25 * inline_toplevel_multiplier;
  return a;
}();
inline const InliningArguments o3_arguments = [] {
  InliningArguments a;
  a.inline_call_cost = 3 * default_inline_call_cost;
  a.inline_alloc_cost = 3 * default_inline_alloc_cost;
  a.inline_prim_cost = 3 * default_inline_prim_cost;
  a.inline_branch_cost = 3 * default_inline_branch_cost;
  a.inline_indirect_cost = 3 * default_inline_indirect_cost;
  a.inline_branch_factor = 0.;
  a.inline_max_depth = 3;
  a.inline_max_unroll = 1;
  a.inline_threshold = 50.;
  a.inline_toplevel_threshold = 50 * inline_toplevel_multiplier;
  return a;
}();

// Compiler_pass: -stop-after (Scheduling and Emit: the native compiler's)
enum class Pass : std::uint8_t { Parsing, Typing, Lambda, Scheduling, Emit };
inline std::optional<Pass> stop_after;
// Clflags.should_stop_after pass (-i stops after typing too)
inline bool should_stop_after(Pass pass) {
  if (Pass::Typing <= pass && print_types) return true;
  return stop_after && *stop_after <= pass;
}
// std_include_flag prefix
inline std::string std_include_flag(const std::string& prefix) {
  if (no_std_include) return "";
  return prefix + filename::quote(config::standard_library);
}
}  // namespace cppcaml::typing::clflags
