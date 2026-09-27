// Port of utils/config.mli, the part the driver reads: the configuration
// variables ocamlc was built with (config_table.inc, generated from
// ocamlc's Config by cxx/harness/gen_driver_tables.sh) and the standard
// library directory.
//
// c++ocamlc is not installed: its standard_library_default is the stdlib
// it finds at run time (driver: $OCAMLLIB / $CAMLLIB / ./stdlib / next to
// the executable), where ocamlc's is the configured install directory.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cppcaml::typing::config {

// Config.version
const std::string& version();
// Config.standard_library_default / standard_library: set by the driver
// (standard_library is $OCAMLLIB, else $CAMLLIB, else the default)
extern std::string standard_library_default;
extern std::string standard_library;
// Config.interface_suffix (-intf-suffix)
extern std::string interface_suffix;
inline const char* default_executable_name = "a.out";
inline const char* ext_obj = ".o";
inline const char* ext_lib = ".a";
inline const char* ext_dll = ".so";

// ---- the values Bytelink / Symtable / Dll / Ccomp / Misc.RuntimeID read
// (config_link.inc, generated from ocamlc's Config by
// cxx/harness/gen_driver_tables.sh)
#include "cppcaml/typing/config_link.inc"

// Config.target_bindir (config.common.ml: "." is the compiler's own directory)
std::string target_bindir();

// type launch_method = Executable | Shebang of string option
struct LaunchMethod {
  enum class K : std::uint8_t { Executable, Shebang } k;
  std::optional<std::string> sh;  // Shebang
};
// type search_method = Disable | Fallback | Enable
enum class SearchMethod : std::uint8_t { Disable, Fallback, Enable };
// Config.launch_method / Config.search_method (config.common.ml's decoding)
LaunchMethod launch_method();
SearchMethod search_method();

// configuration_variables (), in print_config's order
std::vector<std::pair<std::string, std::string>> configuration_variables();
// print_config (on stdout) / config_var x
void print_config();
std::optional<std::string> config_var(const std::string& x);

}  // namespace cppcaml::typing::config
