// Port of utils/config.mli, the part the driver reads: the configuration
// variables ocamlc was built with (config_table.inc, generated from
// ocamlc's Config by cxx/harness/gen_driver_tables.sh) and the standard
// library directory.
//
// c++ocamlc is not installed: its standard_library_default is the stdlib
// it finds at run time (driver: $OCAMLLIB / $CAMLLIB / ./stdlib / next to
// the executable), where ocamlc's is the configured install directory.
#pragma once

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

// configuration_variables (), in print_config's order
std::vector<std::pair<std::string, std::string>> configuration_variables();
// print_config (on stdout) / config_var x
void print_config();
std::optional<std::string> config_var(const std::string& x);

}  // namespace cppcaml::typing::config
