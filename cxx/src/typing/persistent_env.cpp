// Ports of utils/load_path.ml, utils/consistbl.ml and typing/persistent_env.ml.
// See persistent_env.hpp.
#include "cppcaml/typing/persistent_env.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace cppcaml::typing {

namespace fs = std::filesystem;

namespace load_path {

std::optional<std::string> normalized_unit_filename(const std::string& fn) {
  if (fn.empty()) return fn;
  std::string r = fn;
  if (static_cast<unsigned char>(r[0]) < 0x80) r[0] = static_cast<char>(std::tolower(r[0]));
  return r;
}

struct Dir {
  std::string path;
  std::vector<std::string> files;
  bool hidden;
};

static Dir dir_create(bool hidden, const std::string& path) {
  // readdir_compat: a missing directory has no files; "" is the current one
  Dir d{path, {}, hidden};
  std::error_code ec;
  for (auto& e : fs::directory_iterator(path.empty() ? "." : path, ec))
    d.files.push_back(e.path().filename().string());
  return d;
}

static std::string concat(const std::string& dir, const std::string& base) {
  // Filename.concat
  if (dir.empty() || dir.back() == '/') return dir + base;
  return dir + "/" + base;
}

static std::unordered_map<std::string, std::string> g_visible_files, g_visible_files_uncap,
    g_hidden_files, g_hidden_files_uncap;
static std::vector<Dir> g_visible_dirs, g_hidden_dirs;  // most recent first

void reset() {
  g_hidden_files.clear();
  g_hidden_files_uncap.clear();
  g_visible_files.clear();
  g_visible_files_uncap.clear();
  g_hidden_dirs.clear();
  g_visible_dirs.clear();
}

static void prepend_add(const Dir& dir) {
  for (auto& base : dir.files) {
    auto filename = normalized_unit_filename(base);
    if (!filename) continue;
    std::string fn = concat(dir.path, base);
    if (dir.hidden) {
      g_hidden_files[base] = fn;
      g_hidden_files_uncap[*filename] = fn;
    } else {
      g_visible_files[base] = fn;
      g_visible_files_uncap[*filename] = fn;
    }
  }
}

void init(const std::vector<std::string>& visible, const std::vector<std::string>& hidden) {
  reset();
  // visible_dirs := List.rev_map (Dir.create ~hidden:false) visible
  for (auto& d : visible) g_visible_dirs.insert(g_visible_dirs.begin(), dir_create(false, d));
  for (auto& d : hidden) g_hidden_dirs.insert(g_hidden_dirs.begin(), dir_create(true, d));
  for (auto& d : g_hidden_dirs) prepend_add(d);
  for (auto& d : g_visible_dirs) prepend_add(d);
}

void add_dir(bool hidden, const std::string& path) {
  Dir dir = dir_create(hidden, path);
  auto update = [&](const std::string& base, const std::string& fn,
                    std::unordered_map<std::string, std::string>& visible_files,
                    std::unordered_map<std::string, std::string>& hidden_files) {
    if (dir.hidden) hidden_files.emplace(base, fn);
    else visible_files.emplace(base, fn);
  };
  for (auto& base : dir.files) {
    auto ubase = normalized_unit_filename(base);
    if (!ubase) continue;
    std::string fn = concat(dir.path, base);
    update(base, fn, g_visible_files, g_hidden_files);
    update(*ubase, fn, g_visible_files_uncap, g_hidden_files_uncap);
  }
  if (dir.hidden) g_hidden_dirs.insert(g_hidden_dirs.begin(), std::move(dir));
  else g_visible_dirs.insert(g_visible_dirs.begin(), std::move(dir));
}

static std::vector<std::string> rev_paths(const std::vector<Dir>& ds) {
  std::vector<std::string> v;
  for (auto it = ds.rbegin(); it != ds.rend(); ++it) v.push_back(it->path);
  return v;
}

std::vector<std::string> get_path_list() {
  auto v = rev_paths(g_visible_dirs);
  auto h = rev_paths(g_hidden_dirs);
  v.insert(v.end(), h.begin(), h.end());
  return v;
}

static bool is_basename(const std::string& fn) { return fs::path(fn).filename().string() == fn; }

std::string find(const std::string& fn) {
  if (is_basename(fn)) {
    if (auto it = g_visible_files.find(fn); it != g_visible_files.end()) return it->second;
    if (auto it = g_hidden_files.find(fn); it != g_hidden_files.end()) return it->second;
    throw NotFound{};
  }
  // Misc.find_in_path
  for (auto& d : get_path_list()) {
    std::string f = concat(d, fn);
    if (fs::exists(f)) return f;
  }
  throw NotFound{};
}

static std::string find_in_path_normalized(const std::vector<std::string>& path,
                                           const std::string& name) {
  auto uname = normalized_unit_filename(name);
  if (!uname) throw NotFound{};
  for (auto& dir : path) {
    std::string fullname = concat(dir, name), ufullname = concat(dir, *uname);
    if (fs::exists(ufullname)) return ufullname;
    if (fs::exists(fullname)) return fullname;
  }
  throw NotFound{};
}

std::pair<std::string, Visibility> find_normalized_with_visibility(const std::string& fn) {
  auto fn_uncap = normalized_unit_filename(fn);
  if (!fn_uncap) throw NotFound{};
  if (is_basename(fn)) {
    if (auto it = g_visible_files_uncap.find(*fn_uncap); it != g_visible_files_uncap.end())
      return {it->second, Visibility::Visible};
    if (auto it = g_hidden_files_uncap.find(*fn_uncap); it != g_hidden_files_uncap.end())
      return {it->second, Visibility::Hidden};
    throw NotFound{};
  }
  try {
    return {find_in_path_normalized(rev_paths(g_visible_dirs), fn), Visibility::Visible};
  } catch (const NotFound&) {
    return {find_in_path_normalized(rev_paths(g_hidden_dirs), fn), Visibility::Hidden};
  }
}

std::string find_normalized(const std::string& fn) {
  return find_normalized_with_visibility(fn).first;
}

}  // namespace load_path

// ---- Consistbl --------------------------------------------------------------
void Consistbl::check(const std::string& name, const std::string& crc,
                      const std::string& source) {
  auto it = tbl_.find(name);
  if (it == tbl_.end()) {
    tbl_.emplace(name, std::make_pair(crc, source));
    return;
  }
  if (crc != it->second.first) throw Inconsistency{name, source, it->second.second};
}

void Consistbl::check_noadd(const std::string& name, const std::string& crc,
                            const std::string& source) {
  auto it = tbl_.find(name);
  if (it == tbl_.end()) throw NotAvailable{name};
  if (crc != it->second.first) throw Inconsistency{name, source, it->second.second};
}

std::string Consistbl::source(const std::string& name) const {
  auto it = tbl_.find(name);
  if (it == tbl_.end()) throw load_path::NotFound{};
  return it->second.second;
}

std::vector<std::pair<std::string, std::optional<std::string>>> Consistbl::extract(
    const std::vector<std::string>& l0) const {
  std::vector<std::string> l = l0;
  std::sort(l.begin(), l.end());
  l.erase(std::unique(l.begin(), l.end()), l.end());
  std::vector<std::pair<std::string, std::optional<std::string>>> assc;
  for (auto& name : l) {
    auto it = tbl_.find(name);
    std::optional<std::string> crc;
    if (it != tbl_.end()) crc = it->second.first;
    assc.insert(assc.begin(), {name, crc});
  }
  return assc;
}

// ---- Persistent_env ------------------------------------------------------------
namespace persistent_env {

Error::Error(Kind k, std::string a_, std::string b_, std::string c_)
    : std::runtime_error([&] {
        switch (k) {
          case Kind::Illegal_renaming:
            return "Wrong file naming: " + c_ + " contains the compiled interface for " + b_ +
                   " when " + a_ + " was expected";
          case Kind::Inconsistent_import:
            return "The files " + b_ + " and " + c_ +
                   " make inconsistent assumptions over interface " + a_;
          default:
            return "Invalid import of " + a_ +
                   ", which uses recursive types. The compilation flag -rectypes is required";
        }
      }()),
      kind(k), a(std::move(a_)), b(std::move(b_)), c(std::move(c_)) {}


std::function<std::optional<PersistentSignature>(bool, const std::string&)> load =
    [](bool allow_hidden, const std::string& unit_name) -> std::optional<PersistentSignature> {
  std::pair<std::string, load_path::Visibility> found;
  try {
    found = load_path::find_normalized_with_visibility(unit_name + ".cmi");
  } catch (const load_path::NotFound&) {
    return std::nullopt;
  }
  if (!allow_hidden && found.second == load_path::Visibility::Hidden) return std::nullopt;
  return PersistentSignature{found.first, cmi_format::read_cmi(found.first),
                             allow_hidden ? found.second : load_path::Visibility::Visible};
};

}  // namespace persistent_env

}  // namespace cppcaml::typing
