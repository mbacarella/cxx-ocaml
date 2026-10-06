// Ports of utils/load_path.ml, utils/consistbl.ml and typing/persistent_env.ml.
// See persistent_env.hpp.
#include <dirent.h>
#include "cppcaml/typing/persistent_env.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "cppcaml/os.hpp"
#include "cppcaml/typing/utf8_lexeme.hpp"

namespace cppcaml::typing {

namespace fs = std::filesystem;

namespace load_path {

// Misc.normalized_unit_filename = Utf8_lexeme.uncapitalize (Error: nullopt)
std::optional<std::string> normalized_unit_filename(const std::string& fn) {
  utf8_lexeme::Result r = utf8_lexeme::uncapitalize(fn);
  if (!r.ok) return std::nullopt;
  return std::move(r.s);
}

struct Dir {
  std::string path;
  std::vector<std::string> files;
  bool hidden;
};

// A directory's listing, cached on disk across compilations: every compiler
// of a parallel build lists the same load path, and concurrent getdents on
// one directory contend in the kernel (in a 32-way dune build, most of the
// compilers' system time).  An entry is keyed by the directory's device and
// inode and valid while its mtime and ctime are the ones recorded; it is
// recorded only for a directory unchanged for two seconds, so that any later
// change moves a timestamp.  The names keep readdir's order.
// CPPCAML_DIR_CACHE=0 disables it, =<dir> puts it in <dir> (default
// $XDG_CACHE_HOME/c++ocamlc/dirs, else ~/.cache/c++ocamlc/dirs).
namespace dir_cache {
const std::string& dir() {
  static const std::string d = [] {
    const char* e = std::getenv("CPPCAML_DIR_CACHE");
    if (e && std::strcmp(e, "0") == 0) return std::string();
    std::string r;
    if (e && *e) r = e;
    else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) r = std::string(x) + "/c++ocamlc/dirs";
    else if (const char* h = std::getenv("HOME"); h && *h) r = std::string(h) + "/.cache/c++ocamlc/dirs";
    else return std::string();
    std::error_code ec;
    fs::create_directories(r, ec);
    return ec ? std::string() : r;
  }();
  return d;
}
struct Header {
  std::uint64_t magic, dev, ino, mtime_s, mtime_ns, ctime_s, ctime_ns, count;
};
constexpr std::uint64_t kMagic = 0x6370706461697231ULL;  // "cppdair1"
std::string file_of(const struct stat& st) {
  char b[64];
  std::snprintf(b, sizeof b, "/%llx-%llx", static_cast<unsigned long long>(st.st_dev),
                static_cast<unsigned long long>(st.st_ino));
  return dir() + b;
}
Header header_of(const struct stat& st, std::uint64_t count) {
  return Header{kMagic,
                static_cast<std::uint64_t>(st.st_dev),
                static_cast<std::uint64_t>(st.st_ino),
                static_cast<std::uint64_t>(os::mtime(st).tv_sec),
                static_cast<std::uint64_t>(os::mtime(st).tv_nsec),
                static_cast<std::uint64_t>(os::ctime(st).tv_sec),
                static_cast<std::uint64_t>(os::ctime(st).tv_nsec),
                count};
}
bool load(const struct stat& st, std::vector<std::string>& files) {
  int fd = ::open(file_of(st).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::string buf;
  char chunk[65536];
  for (;;) {
    ssize_t n = ::read(fd, chunk, sizeof chunk);
    if (n <= 0) break;
    buf.append(chunk, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (buf.size() < sizeof(Header)) return false;
  Header h, want = header_of(st, 0);
  std::memcpy(&h, buf.data(), sizeof h);
  if (h.magic != kMagic || h.dev != want.dev || h.ino != want.ino || h.mtime_s != want.mtime_s ||
      h.mtime_ns != want.mtime_ns || h.ctime_s != want.ctime_s || h.ctime_ns != want.ctime_ns)
    return false;
  std::vector<std::string> out;
  out.reserve(h.count);
  std::size_t p = sizeof h;
  for (std::uint64_t k = 0; k < h.count; ++k) {
    std::size_t e = buf.find('\0', p);
    if (e == std::string::npos) return false;
    out.emplace_back(buf, p, e - p);
    p = e + 1;
  }
  if (p != buf.size()) return false;
  files = std::move(out);
  return true;
}
void record(const struct stat& st, const std::vector<std::string>& files) {
  struct timespec now;
  ::clock_gettime(CLOCK_REALTIME, &now);
  auto settled = [&](const struct timespec& t) { return t.tv_sec + 2 < now.tv_sec; };
  if (!settled(os::mtime(st)) || !settled(os::ctime(st))) return;
  std::string buf(sizeof(Header), '\0');
  Header h = header_of(st, files.size());
  std::memcpy(buf.data(), &h, sizeof h);
  for (const std::string& f : files) {
    buf += f;
    buf += '\0';
  }
  std::string target = file_of(st);
  std::string tmp = target + ".tmp." + std::to_string(::getpid());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  bool ok = ::write(fd, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), target.c_str()) != 0) ::unlink(tmp.c_str());
}
}  // namespace dir_cache

static Dir dir_create(bool hidden, const std::string& path) {
  // readdir_compat: a missing directory has no files; "" is the current one
  // (readdir's order, without "." and "..", as Sys.readdir)
  Dir d{path, {}, hidden};
  const char* p = path.empty() ? "." : path.c_str();
  struct stat st;
  bool cacheable = !dir_cache::dir().empty() && ::stat(p, &st) == 0 && S_ISDIR(st.st_mode);
  if (cacheable && dir_cache::load(st, d.files)) return d;
  if (DIR* dp = ::opendir(p)) {
    while (const struct dirent* e = ::readdir(dp)) {
      const char* n = e->d_name;
      if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
      d.files.emplace_back(n);
    }
    ::closedir(dp);
    // the listing belongs to the directory [st] describes only if nothing
    // changed it meanwhile
    struct stat after;
    if (cacheable && ::stat(p, &after) == 0 && after.st_ino == st.st_ino && after.st_dev == st.st_dev &&
        os::mtime(after).tv_sec == os::mtime(st).tv_sec && os::mtime(after).tv_nsec == os::mtime(st).tv_nsec &&
        os::ctime(after).tv_sec == os::ctime(st).tv_sec && os::ctime(after).tv_nsec == os::ctime(st).tv_nsec)
      dir_cache::record(st, d.files);
  }
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

std::vector<std::vector<std::string>> visible_dir_files() {
  // visible_dirs is kept latest-first: List.rev gives the path's order
  std::vector<std::vector<std::string>> r;
  for (auto it = g_visible_dirs.rbegin(); it != g_visible_dirs.rend(); ++it) r.push_back(it->files);
  return r;
}

std::pair<std::vector<std::string>, std::vector<std::string>> get_paths() {
  return {rev_paths(g_visible_dirs), rev_paths(g_hidden_dirs)};
}

std::vector<std::string> get_path_list() {
  auto v = rev_paths(g_visible_dirs);
  auto h = rev_paths(g_hidden_dirs);
  v.insert(v.end(), h.begin(), h.end());
  return v;
}

// Filename.basename fn = fn (Unix: a non-empty name without a '/')
static bool is_basename(const std::string& fn) { return !fn.empty() && fn.find('/') == std::string::npos; }

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
