// Port of driver/main_args.ml's bytecode compiler options:
// Make_bytecomp_options (Default.Main).list -- the keys, Arg kinds, Symbol
// choices and docs come from main_args_table.inc (generated from ocamlc's
// own list by cxx/harness/gen_driver_tables.sh), in ocamlc's order; the
// actions are Default.Main's, ported here.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/arg.hpp"

namespace cppcaml::typing::main_args {

// Make_bytecomp_options (Default.Main).list
std::vector<arg::Option> bytecomp_options();
// Make_optcomp_options (Default.Optmain).list (optmain_args_table.inc)
std::vector<arg::Option> optcomp_options();

// the options c++ocamlc accepts beyond ocamlc's (undocumented: not in
// -help): -stdlib <dir> sets Config.standard_library
std::vector<arg::Option> cppcaml_extensions();

// Default.Main's effects c++ocamlc does not implement: the option that
// asked for one (the first, in ocamlc's list order), or "" -- checked by
// the driver when the effect would take place
std::string unsupported_compile_option();  // after the arguments are parsed
std::string unsupported_link_option();     // when linking an executable
std::string unsupported_archive_option();  // when building a library (-a)

}  // namespace cppcaml::typing::main_args
