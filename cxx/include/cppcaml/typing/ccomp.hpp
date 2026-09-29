// Port of utils/ccomp.ml: compiling C files and calling the C linker, as
// the bytecode compiler does it (-custom, -output-obj, -output-complete-*,
// .c inputs).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cppcaml::typing::ccomp {

// command cmdline: Sys.command (echoed with -verbose); raises Sys_error
// (arg::SysError) when the shell cannot run it (127)
int command(const std::string& cmdline);
void run_command(const std::string& cmdline);
// compile_file ?output ?(opt="") ?stable_name name
int compile_file(const std::string& name, const std::optional<std::string>& output = std::nullopt,
                 const std::string& opt = "", const std::optional<std::string>& stable_name = std::nullopt);
// create_archive archive file_list
int create_archive(const std::string& archive, const std::vector<std::string>& file_list);
// quote_files ~response_files lst
std::string quote_files(bool response_files, const std::vector<std::string>& lst);
std::string quote_optfile(const std::optional<std::string>& f);

enum class LinkMode { Exe, Dll, MainDll, Partial };
// call_linker mode output_name files extra
int call_linker(LinkMode mode, const std::string& output_name, const std::vector<std::string>& files,
                const std::string& extra);

}  // namespace cppcaml::typing::ccomp
