// Port of stdlib/filename.ml, the Unix sysdeps (Sys.os_type is "Unix" for
// the configurations c++ocamlc targets): the functions the driver, Ccomp
// and Bytelink use.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cppcaml::typing::filename {

inline const char* current_dir_name = ".";
inline const char* parent_dir_name = "..";
inline const char* dir_sep = "/";

bool is_relative(const std::string& n);
bool is_implicit(const std::string& n);
bool check_suffix(const std::string& name, const std::string& suff);
std::string concat(const std::string& dirname, const std::string& filename);
std::string basename(const std::string& name);
std::string dirname(const std::string& name);
// raises std::invalid_argument
std::string chop_suffix(const std::string& name, const std::string& suff);
std::string extension(const std::string& name);
// raises std::invalid_argument ("Filename.chop_extension")
std::string chop_extension(const std::string& name);
std::string remove_extension(const std::string& name);
std::string quote(const std::string& s);
// quote_command cmd ?stdin ?stdout ?stderr args
std::string quote_command(const std::string& cmd, const std::vector<std::string>& args,
                          const std::optional<std::string>& stdin_ = std::nullopt,
                          const std::optional<std::string>& stdout_ = std::nullopt,
                          const std::optional<std::string>& stderr_ = std::nullopt);
// Filename.get_temp_dir_name () ($TMPDIR, else /tmp)
std::string get_temp_dir_name();
// temp_file prefix suffix: creates the file (O_EXCL, 0600); raises
// std::runtime_error (Sys_error) after 20 attempts
std::string temp_file(const std::string& prefix, const std::string& suffix);

}  // namespace cppcaml::typing::filename
