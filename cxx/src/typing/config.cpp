// Port of utils/config.mli's driver-facing part (see config.hpp).
#include "cppcaml/typing/config.hpp"

#include <unistd.h>

#include <iostream>

namespace cppcaml::typing::config {

namespace {
struct Var {
  const char* name;
  const char* value;
};
const Var kVars[] = {
#include "config_table.inc"
};
}  // namespace

std::string standard_library_default;
std::string standard_library;
std::string interface_suffix = ".mli";

std::string target_bindir() {
  if (target_bindir_raw != ".") return target_bindir_raw;
  // Filename.dirname Sys.executable_name
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  std::string exe(buf, static_cast<std::size_t>(n));
  std::size_t slash = exe.rfind('/');
  return slash == std::string::npos ? "." : slash == 0 ? "/" : exe.substr(0, slash);
}

LaunchMethod launch_method() {
  if (launch_method_raw == "exe") return {LaunchMethod::K::Executable, std::nullopt};
  if (launch_method_raw == "sh") return {LaunchMethod::K::Shebang, std::nullopt};
  return {LaunchMethod::K::Shebang, launch_method_raw};
}

SearchMethod search_method() {
  if (search_method_raw == "enable") return SearchMethod::Enable;
  if (search_method_raw == "fallback") return SearchMethod::Fallback;
  return SearchMethod::Disable;
}

const std::string& version() {
  static const std::string v = [] {
    for (const Var& x : kVars)
      if (std::string(x.name) == "version") return std::string(x.value);
    return std::string();
  }();
  return v;
}

std::vector<std::pair<std::string, std::string>> configuration_variables() {
  std::vector<std::pair<std::string, std::string>> r;
  for (const Var& x : kVars) {
    std::string n = x.name;
    if (n == "standard_library_default") r.emplace_back(n, standard_library_default);
    else if (n == "standard_library") r.emplace_back(n, standard_library);
    else r.emplace_back(n, x.value);
  }
  return r;
}

void print_config() {
  for (auto& [x, v] : configuration_variables()) std::cout << x << ": " << v << '\n';
}

std::optional<std::string> config_var(const std::string& x) {
  for (auto& [n, v] : configuration_variables())
    if (n == x) return v;
  return std::nullopt;
}

}  // namespace cppcaml::typing::config
