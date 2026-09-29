// Port of bytecomp/dll.ml (For_checking); see dll.hpp.
#include "cppcaml/typing/dll.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "cppcaml/typing/binutils.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::dll {

namespace {
std::vector<std::string> g_search_path;
// opened_dlls: (fullname, Checking t), the latest first
std::vector<std::pair<std::string, std::shared_ptr<binutils::T>>> g_opened;

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// runtime/dynlink.c: make_relative_path_absolute path root
std::string make_relative_path_absolute(const std::string& path, const std::string& root) {
  if (!path.empty() && path[0] == '.') {
    if (path.size() == 1) return root;
    if (path[1] == '/') return root + path.substr(1);
    if (path[1] == '.' && (path.size() == 2 || path[2] == '/')) return root + "/" + path;
  }
  return path;
}

// ld_conf_contents Config.standard_library_default
// (runtime/dynlink.c: caml_parse_ld_conf, the $OCAMLLIB, $CAMLLIB and the
// stdlib's ld.conf, each in turn)
std::vector<std::string> ld_conf_contents(const std::string& stdlib) {
  std::vector<std::string> entries;
  const char* locations[3] = {std::getenv("OCAMLLIB"), std::getenv("CAMLLIB"), stdlib.c_str()};
  for (const char* loc : locations) {
    if (!loc) continue;
    std::string libroot = loc;
    while (!libroot.empty() && libroot.back() == '/') libroot.pop_back();
    std::string ldconfname = libroot + "/ld.conf";
    struct stat st;
    if (::stat(ldconfname.c_str(), &st) == -1) continue;
    std::ifstream in(ldconfname, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string config = ss.str();
    config = config.substr(0, config.find('\0'));
    std::size_t p = 0;
    while (p < config.size()) {
      std::size_t q = config.find('\n', p);
      std::size_t r = q;
      if (q == std::string::npos) {
        q = r = config.size();
      } else {
        ++r;
        // CR*LF is a single LF
        while (q > p && config[q - 1] == '\r') --q;
      }
      entries.push_back(make_relative_path_absolute(config.substr(p, q - p), libroot));
      p = r;
    }
  }
  return entries;
}

// ld_library_path_contents: $CAML_LD_LIBRARY_PATH
std::vector<std::string> ld_library_path_contents() {
  const char* s = std::getenv("CAML_LD_LIBRARY_PATH");
  if (!s) return {};
  return misc::split_path_contents(s);
}

// open_dll For_checking name
void open_dll(const std::string& name0) {
  std::string name = name0 + config::ext_dll;
  std::string fullname;
  try {
    fullname = misc::find_in_path(g_search_path, name);
    if (filename::is_implicit(fullname)) fullname = filename::concat(filename::current_dir_name, fullname);
  } catch (const misc::NotFound&) {
    fullname = name;
  }
  for (auto& [n, _] : g_opened)
    if (n == fullname) return;  // Some (Checking _), For_checking
  auto r = binutils::read(fullname);
  if (auto* e = std::get_if<binutils::Error>(&r))
    throw Failure(fullname + ": " + binutils::error_to_string(*e));  // failwith
  g_opened.insert(g_opened.begin(), {fullname, std::make_shared<binutils::T>(std::get<binutils::T>(std::move(r)))});
}
}  // namespace

std::string extract_dll_name(const std::pair<bool, std::string>& dllib) {
  auto [suffixed, file0] = dllib;
  if (!suffixed && filename::check_suffix(file0, config::ext_dll)) return filename::chop_suffix(file0, config::ext_dll);
  std::string file = starts_with(file0, "-l") ? "dll" + file0.substr(2) : file0;
  return suffixed ? misc::stubslib(file) : file;
}

void init_compile(bool nostdlib) {
  g_search_path = ld_library_path_contents();
  if (!nostdlib) {
    std::vector<std::string> c = ld_conf_contents(config::standard_library_default);
    g_search_path.insert(g_search_path.end(), c.begin(), c.end());
  }
}

void add_path(const std::vector<std::string>& dirs) {
  std::vector<std::string> p = dirs;
  p.insert(p.end(), g_search_path.begin(), g_search_path.end());
  g_search_path = std::move(p);
}

void open_dlls_for_checking(const std::vector<std::string>& names) {
  for (const std::string& n : names) open_dll(n);
}

void close_all_dlls() { g_opened.clear(); }

bool find_primitive(const std::string& prim_name) {
  for (auto& [_, t] : g_opened)
    if (t->defines_symbol(prim_name)) return true;
  return false;
}

void reset() {
  g_search_path.clear();
  g_opened.clear();
}

std::vector<std::string> search_path() { return g_search_path; }

}  // namespace cppcaml::typing::dll
