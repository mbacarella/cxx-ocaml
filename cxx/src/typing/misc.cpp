// See misc.hpp.
#include "cppcaml/typing/misc.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "cppcaml/typing/clflags.hpp"

#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/filename.hpp"

#include <filesystem>
#include <stdexcept>

namespace cppcaml::typing::misc {

namespace style {
void inline_code(fd::Formatter& ppf, std::string_view s) {
  fd::pp_open_stag(ppf, "inline_code");
  fd::pp_print_string(ppf, s);
  fd::pp_close_stag(ppf);
}
void hint(fd::Formatter& ppf) { fd::fprintf(ppf, "@{<hint>Hint@}"); }

namespace {
bool g_setup_done = false;
bool g_color_enabled = false;
// style_of_tag: the ANSI codes of the known tags (Style.default_styles);
// "ralign" has no style (ansi_of_style_l [] is the Reset code)
std::optional<std::string> style_codes(const std::string& tag) {
  if (tag == "error") return "1;31";
  if (tag == "warning") return "1;35";
  if (tag == "loc") return "1";
  if (tag == "hint") return "1;34";
  if (tag == "inline_code") return "1";
  if (tag == "ralign") return "0";
  if (tag.rfind("@style:", 0) == 0) return tag.substr(7);
  return std::nullopt;
}
}  // namespace

void setup() {
  if (g_setup_done) return;
  g_setup_done = true;
  namespace cf = clflags;
  cf::Color c = cf::color ? *cf::color : cf::Color::Auto;
  if (c == cf::Color::Always) g_color_enabled = true;
  else if (c == cf::Color::Never) g_color_enabled = false;
  else {
    const char* term = std::getenv("TERM");
    std::string t = term ? term : "";
    g_color_enabled = t != "dumb" && !t.empty() && ::isatty(2);
  }
}

bool marks_enabled() { return g_setup_done; }

std::string mark_open_tag(const std::string& tag) {
  if (std::optional<std::string> codes = style_codes(tag)) return g_color_enabled ? "\x1b[" + *codes + "m" : "";
  return "<" + tag + ">";
}

std::string mark_close_tag(const std::string& tag) {
  if (style_codes(tag)) return g_color_enabled ? "\x1b[0m" : "";
  return "</" + tag + ">";
}

}  // namespace style

std::optional<long> edit_distance(std::string_view a, std::string_view b, long cutoff) {
  long la = static_cast<long>(a.size()), lb = static_cast<long>(b.size());
  cutoff = std::min(std::max(la, lb), cutoff);
  if (std::labs(la - lb) > cutoff) return std::nullopt;
  std::vector<std::vector<long>> m(la + 1, std::vector<long>(lb + 1, cutoff + 1));
  m[0][0] = 0;
  for (long i = 1; i <= la; ++i) m[i][0] = i;
  for (long j = 1; j <= lb; ++j) m[0][j] = j;
  for (long i = 1; i <= la; ++i)
    for (long j = std::max(1L, i - cutoff - 1); j <= std::min(lb, i + cutoff + 1); ++j) {
      long cost = a[i - 1] == b[j - 1] ? 0 : 1;
      long best = std::min(1 + std::min(m[i - 1][j], m[i][j - 1]), m[i - 1][j - 1] + cost);
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) best = std::min(best, m[i - 2][j - 2] + cost);
      m[i][j] = best;
    }
  long result = m[la][lb];
  if (result > cutoff) return std::nullopt;
  return result;
}

std::vector<std::string> spellcheck(const std::vector<std::string>& env0, std::string_view name) {
  long cutoff;
  switch (name.size()) {
    case 1: case 2: cutoff = 0; break;
    case 3: case 4: cutoff = 1; break;
    case 5: case 6: cutoff = 2; break;
    default: cutoff = 3; break;
  }
  // List.sort_uniq (fun s1 s2 -> String.compare s2 s1) env: descending
  std::vector<std::string> env = env0;
  std::sort(env.begin(), env.end(), [](const std::string& a, const std::string& b) { return b < a; });
  env.erase(std::unique(env.begin(), env.end()), env.end());
  // fold_left: best_choice is consed, so later heads come first
  std::vector<std::string> best;  // in reverse (consing) order
  long best_dist = -1;            // max_int
  for (const std::string& head : env) {
    std::optional<long> d = edit_distance(name, head, cutoff);
    if (!d) continue;
    if (best_dist < 0 || *d < best_dist) {
      best = {head};
      best_dist = *d;
    } else if (*d == best_dist) {
      best.push_back(head);
    }
  }
  std::reverse(best.begin(), best.end());
  return best;
}

std::optional<fd::Doc> did_you_mean(const std::vector<std::string>& choices,
                                    const std::function<void(fd::Formatter&, const std::string&)>& pp0) {
  if (choices.empty()) return std::nullopt;
  std::function<void(fd::Formatter&, const std::string&)> pp =
      pp0 ? pp0 : [](fd::Formatter& ppf, const std::string& s) { style::inline_code(ppf, s); };
  std::vector<std::string> rest(choices.begin(), choices.end() - 1);
  const std::string& last = choices.back();
  return fd::doc_printf(
      "@[@{<hint>Hint@}: @{<ralign>Did you mean @}%a%s%a?@]",
      [&](fd::Formatter& ppf) { fd::pp_print_list(ppf, pp, rest, fd::comma); }, rest.empty() ? "" : " or ",
      [&](fd::Formatter& ppf) { pp(ppf, last); });
}

std::pair<fd::Doc, fd::Doc> align_hint(std::string_view prefix, const fd::Doc& main, const fd::Doc& hint) {
  return fd::align_prefix2({main, static_cast<long>(prefix.size())}, {hint, 0});
}
std::pair<fd::Doc, fd::Doc> align_error_hint(const fd::Doc& main, const fd::Doc& hint) {
  return align_hint("Error: ", main, hint);
}

const char* ordinal_suffix(long n) {
  bool teen = (n % 100) / 10 == 1;
  switch (n % 10) {
    case 1: if (!teen) return "st"; break;
    case 2: if (!teen) return "nd"; break;
    case 3: if (!teen) return "rd"; break;
    default: break;
  }
  return "th";
}

void print_manual_section(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "manual section %a", [&](fd::Formatter& f) {
    bool first = true;
    for (long n : section) {
      if (!first) fd::pp_print_char(f, '.');
      first = false;
      fd::pp_print_int(f, n);
    }
  });
}
void print_see_manual(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "(see %a)", [&](fd::Formatter& f) { print_manual_section(f, section); });
}
void print_manual_hint(fd::Formatter& ppf, const std::vector<long>& section) {
  fd::fprintf(ppf, "%t (%a):", [](fd::Formatter& f) { style::hint(f); },
              [&](fd::Formatter& f) { print_manual_section(f, section); });
}

void fatal_error(const std::string& msg) {
  // Format.kfprintf ... Format.err_formatter ("@?>> Fatal error: " ^^ fmt ^^ "@.")
  std::cout.flush();
  std::cerr << ">> Fatal error: " << msg << std::endl;
  throw FatalError{};
}

const build_path_prefix_map::Map* get_build_path_prefix_map() {
  static bool init = false;
  static std::optional<build_path_prefix_map::Map> map_cache;
  if (!init) {
    init = true;
    if (const char* encoded_map = std::getenv("BUILD_PATH_PREFIX_MAP")) {
      build_path_prefix_map::Result<build_path_prefix_map::Map> r = build_path_prefix_map::decode_map(encoded_map);
      if (!r.ok) fatal_error("Invalid value for the environment variable BUILD_PATH_PREFIX_MAP: " + r.error);
      map_cache = std::move(*r.ok);
    }
  }
  return map_cache ? &*map_cache : nullptr;
}

std::vector<std::string> invert_build_path_prefix_map(const std::string& path) {
  const build_path_prefix_map::Map* prefix_map = get_build_path_prefix_map();
  if (!prefix_map) return {path};
  std::vector<std::string> matches = build_path_prefix_map::invert_all(*prefix_map, path);
  if (matches.empty()) return {path};
  return matches;
}

// ---- files and paths ----

std::string find_in_path(const std::vector<std::string>& path, const std::string& name) {
  std::error_code ec;
  if (!filename::is_implicit(name)) {
    if (std::filesystem::exists(name, ec)) return name;
    throw NotFound{};
  }
  for (const std::string& dir : path) {
    std::string fullname = filename::concat(dir, name);
    if (std::filesystem::exists(fullname, ec)) return fullname;
  }
  throw NotFound{};
}

void remove_file(const std::string& f) {
  std::error_code ec;
  if (std::filesystem::is_regular_file(f, ec)) std::filesystem::remove(f, ec);
}

std::vector<std::string> split_path_contents(const std::string& s, char sep) {
  std::vector<std::string> r;
  if (s.empty()) return r;
  std::size_t b = 0;
  for (;;) {
    std::size_t e = s.find(sep, b);
    r.push_back(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
    if (e == std::string::npos) return r;
    b = e + 1;
  }
}

std::string concat_null_terminated(const std::vector<std::string>& l) {
  std::string r;
  for (const std::string& x : l) {
    r += x;
    r += '\0';
  }
  return r;
}

std::string replace_substring(const std::string& before, const std::string& after, const std::string& str) {
  std::string r;
  std::size_t curr = 0;
  for (;;) {
    std::size_t next = before.empty() ? std::string::npos : str.find(before, curr);
    if (next == std::string::npos) return r + str.substr(curr);
    r += str.substr(curr, next - curr) + after;
    curr = next + before.size();
  }
}

// ---- Misc.RuntimeID ----

namespace {
// make fn ?(dev = not Config.is_official_release) ... ()
RuntimeID make_default() {
  RuntimeID t;
  t.dev = !config::is_official_release;
  t.release = static_cast<int>(config::release_number);
  t.reserved = static_cast<int>(config::reserved_header_bits);
  t.no_flat_float_array = !config::flat_float_array;
  t.fp = config::with_frame_pointers;
  t.tsan = config::tsan;
  t.int31 = config::int_size == 31;
  t.is_static = !config::supports_shared_libraries;
  t.no_compression = config::compression_c_libraries.empty();
  t.ansi = config::target_win32 && !config::windows_unicode;
  return t;
}
}  // namespace

RuntimeID RuntimeID::make_zinc() {
  RuntimeID t = make_default();
  t.reserved = 0;
  t.fp = false;
  t.tsan = false;
  t.ansi = false;
  return t;
}

RuntimeID RuntimeID::make_bytecode() {
  RuntimeID t = make_default();
  t.fp = false;
  t.tsan = false;
  return t;
}

RuntimeID RuntimeID::make_native() { return make_default(); }

bool RuntimeID::is_zinc() const { return reserved == 0 && !fp && !tsan && !ansi; }
bool RuntimeID::is_bytecode() const { return !fp && !tsan; }

std::string RuntimeID::to_string() const {
  static const char alpha[] = "0123456789abcdefghijklmnopqrstuv";
  auto bit = [](int b, bool cond) { return cond ? 1 << b : 0; };
  int q0 = bit(0, dev) | ((release << 1) & 0b11110);
  int q1 = (release >> 4) | ((reserved << 2) & 0b11100);
  int q2 = (reserved >> 3) | bit(2, no_flat_float_array) | bit(3, fp) | bit(4, tsan);
  int q3 = bit(0, int31) | bit(1, is_static) | bit(2, no_compression) | bit(3, ansi);
  return {alpha[q0], alpha[q1], alpha[q2], alpha[q3]};
}

std::optional<RuntimeID> RuntimeID::of_string(const std::string& s) {
  if (s.size() != 4) return std::nullopt;
  auto convert = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'v') return c - 'a' + 10;
    return -1000;  // min_int: the sum stays negative
  };
  int q0 = convert(s[0]), q1 = convert(s[1]), q2 = convert(s[2]), q3 = convert(s[3]);
  if (q0 + q1 + q2 + q3 < 0) return std::nullopt;
  auto set = [](int b, int q) { return (q & (1 << b)) != 0; };
  RuntimeID t;
  t.dev = set(0, q0);
  t.release = ((q1 & 0b11) << 4) | (q0 >> 1);
  t.reserved = ((q2 & 0b11) << 2) | (q1 >> 2);
  t.no_flat_float_array = set(2, q2);
  t.fp = set(3, q2);
  t.tsan = set(4, q2);
  t.int31 = set(0, q3);
  t.is_static = set(1, q3);
  t.no_compression = set(2, q3);
  t.ansi = set(3, q3);
  return t;
}

std::string RuntimeID::ocamlrun(const std::string& variant) const {
  if (!is_zinc()) throw std::invalid_argument("Misc.RuntimeID.ocamlrun");
  return "ocamlrun" + variant + "-" + to_string();
}

std::string shared_runtime_bytecode() {
  return "-lcamlrun-" + config::target + "-" + RuntimeID::make_bytecode().to_string();
}

std::string shared_runtime_native() {
  return "-lasmrun-" + config::target + "-" + RuntimeID::make_native().to_string();
}

std::string stubslib(const std::string& name) {
  return name + "-" + config::target + "-" + RuntimeID::make_bytecode().to_string();
}

}  // namespace cppcaml::typing::misc
