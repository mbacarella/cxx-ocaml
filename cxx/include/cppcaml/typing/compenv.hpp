// Port of driver/compenv.ml: the compiler's environment -- OCAMLPARAM and
// the configuration file (readenv), the deferred per-file actions and their
// processing (output prefixes, objfiles), and the argument parser's error
// and help reports (parse_arguments).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::compenv {

// exception Exit_with_status of int
struct ExitWithStatus {
  int status;
};

// fatal err: prerr_endline err; raise (Exit_with_status 2)
[[noreturn]] void fatal(const std::string& err);

std::string output_prefix(const std::string& name);
[[noreturn]] void print_version_and_library(const std::string& compiler);
[[noreturn]] void print_version_string();
[[noreturn]] void print_standard_library();
std::string extract_output(const std::optional<std::string>& o);
std::string default_output(const std::optional<std::string>& o);

extern std::vector<std::string> first_include_dirs, last_include_dirs;
extern std::vector<std::string> first_ccopts, last_ccopts;
extern std::vector<std::string> first_ppx, last_ppx;
extern std::vector<std::string> first_objfiles, last_objfiles;
extern bool stop_early;

enum class Position { Before_args, Before_compile, Before_link };
// readenv ppf position (the formatter is Format.err_formatter)
void readenv(Position position, const std::string& filename = "");
std::vector<std::string> get_objfiles(bool with_ocamlparam);

// the deferred actions
struct DeferredAction {
  enum class K { ProcessImplementation, ProcessInterface, ProcessCFile, ProcessOtherFile, ProcessObjects, ProcessDLLs };
  K k;
  std::string name;                 // the file
  std::vector<std::string> names;   // ProcessObjects / ProcessDLLs
  bool suffixed = false;            // ProcessDLLs
};
void defer(DeferredAction a);
void anonymous(const std::string& filename);
void impl(const std::string& filename);
void intf(const std::string& filename);

// action_context: the compile functions return 0, or the exit status of a
// failed compilation (they report it themselves)
struct ActionContext {
  std::function<int(clflags::Pass start_from, const std::string& source_file, const std::string& output_prefix)>
      compile_implementation;
  std::function<int(const std::string& source_file, const std::string& output_prefix)> compile_interface;
  std::string ocaml_mod_ext;
  std::string ocaml_lib_ext;
};
void process_deferred_actions(const ActionContext& env);

// Clflags.arg_spec (Clflags.add_arguments / print_arguments)
std::vector<arg::Option>& arg_spec();
void add_arguments(const std::vector<arg::Option>& args);
std::string create_usage_msg(const std::string& program);
void print_arguments(const std::string& program);

// parse_arguments argv anonymous program
void parse_arguments(std::vector<std::string>& argv, const arg::AnonFun& f, const std::string& program);

// parse_runtime_parameter (-set-runtime-default)
void parse_runtime_parameter(const std::string& opt);

// Misc.rev_split_words
std::vector<std::string> rev_split_words(const std::string& s);
// Misc.expand_directory alt s
std::string expand_directory(const std::string& alt, const std::string& s);

}  // namespace cppcaml::typing::compenv
