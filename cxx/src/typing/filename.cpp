// Port of stdlib/filename.ml (Unix sysdeps); see filename.hpp.
#include "cppcaml/typing/filename.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <random>
#include <stdexcept>

namespace cppcaml::typing::filename {

namespace {
bool is_dir_sep(const std::string& s, long i) { return s[static_cast<std::size_t>(i)] == '/'; }
bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// extension_len
std::size_t extension_len(const std::string& name) {
  long n = static_cast<long>(name.size());
  auto check = [&](long i0, long i) -> std::size_t {
    for (;;) {
      if (i < 0 || is_dir_sep(name, i)) return 0;
      if (name[static_cast<std::size_t>(i)] == '.') {
        --i;
        continue;
      }
      return static_cast<std::size_t>(n - i0);
    }
  };
  for (long i = n - 1;; --i) {
    if (i < 0 || is_dir_sep(name, i)) return 0;
    if (name[static_cast<std::size_t>(i)] == '.') return check(i, i - 1);
  }
}

// temp_file_name: Random.State.bits land 0xFFFFFF (a self-initialized PRNG)
std::string temp_file_name(const std::string& temp_dir, const std::string& prefix, const std::string& suffix) {
  static std::mt19937 prng{std::random_device{}()};
  unsigned rnd = static_cast<unsigned>(prng()) & 0xFFFFFFu;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06x", rnd);
  return concat(temp_dir, prefix + buf + suffix);
}
}  // namespace

bool is_relative(const std::string& n) { return n.empty() || n[0] != '/'; }

bool is_implicit(const std::string& n) {
  return is_relative(n) && !starts_with(n, "./") && !starts_with(n, "../");
}

bool check_suffix(const std::string& name, const std::string& suff) {
  return name.size() >= suff.size() && name.compare(name.size() - suff.size(), suff.size(), suff) == 0;
}

std::string concat(const std::string& dirname, const std::string& filename) {
  std::size_t l = dirname.size();
  if (l == 0 || is_dir_sep(dirname, static_cast<long>(l) - 1)) return dirname + filename;
  return dirname + dir_sep + filename;
}

// generic_basename
std::string basename(const std::string& name) {
  if (name.empty()) return current_dir_name;
  long n = static_cast<long>(name.size()) - 1;
  while (n >= 0 && is_dir_sep(name, n)) --n;  // find_end
  if (n < 0) return name.substr(0, 1);
  long p = n + 1;
  while (n >= 0 && !is_dir_sep(name, n)) --n;  // find_beg
  if (n < 0) return name.substr(0, static_cast<std::size_t>(p));
  return name.substr(static_cast<std::size_t>(n + 1), static_cast<std::size_t>(p - n - 1));
}

// generic_dirname
std::string dirname(const std::string& name) {
  if (name.empty()) return current_dir_name;
  long n = static_cast<long>(name.size()) - 1;
  while (n >= 0 && is_dir_sep(name, n)) --n;  // trailing_sep
  if (n < 0) return name.substr(0, 1);
  while (n >= 0 && !is_dir_sep(name, n)) --n;  // base
  if (n < 0) return current_dir_name;
  while (n >= 0 && is_dir_sep(name, n)) --n;  // intermediate_sep
  if (n < 0) return name.substr(0, 1);
  return name.substr(0, static_cast<std::size_t>(n + 1));
}

std::string chop_suffix(const std::string& name, const std::string& suff) {
  if (!check_suffix(name, suff)) throw std::invalid_argument("Filename.chop_suffix");
  return name.substr(0, name.size() - suff.size());
}

std::string extension(const std::string& name) {
  std::size_t l = extension_len(name);
  return l == 0 ? std::string() : name.substr(name.size() - l);
}

std::string chop_extension(const std::string& name) {
  std::size_t l = extension_len(name);
  if (l == 0) throw std::invalid_argument("Filename.chop_extension");
  return name.substr(0, name.size() - l);
}

std::string remove_extension(const std::string& name) {
  std::size_t l = extension_len(name);
  return l == 0 ? name : name.substr(0, name.size() - l);
}

// generic_quote "'\\''"
std::string quote(const std::string& s) {
  std::string b = "'";
  for (char c : s) {
    if (c == '\'') b += "'\\''";
    else b += c;
  }
  b += '\'';
  return b;
}

std::string quote_command(const std::string& cmd, const std::vector<std::string>& args,
                          const std::optional<std::string>& stdin_, const std::optional<std::string>& stdout_,
                          const std::optional<std::string>& stderr_) {
  std::string r = quote(cmd);
  for (const std::string& a : args) r += " " + quote(a);
  if (stdin_) r += " <" + quote(*stdin_);
  if (stdout_) r += " >" + quote(*stdout_);
  if (stderr_) r += stderr_ == stdout_ ? std::string(" 2>&1") : " 2>" + quote(*stderr_);
  return r;
}

std::string get_temp_dir_name() {
  const char* t = std::getenv("TMPDIR");
  return t ? t : "/tmp";
}

std::string temp_file(const std::string& prefix, const std::string& suffix) {
  std::string temp_dir = get_temp_dir_name();
  for (int counter = 0;; ++counter) {
    std::string name = temp_file_name(temp_dir, prefix, suffix);
    int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      ::close(fd);
      return name;
    }
    if (counter >= 20) throw std::runtime_error(name + ": cannot create temporary file");
  }
}

}  // namespace cppcaml::typing::filename
