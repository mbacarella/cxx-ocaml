// gen_config: c++ocamlc's configuration -- the values ocamlc's Config module
// holds -- from the configured tree, as cxx/Makefile builds it:
//
//   gen_config ROOT OUTDIR
//
// reads what ./configure wrote (utils/config.generated.ml,
// utils/config.common.ml, Makefile.build_config, runtime/caml/s.h and m.h)
// and VERSION, evaluates them as config.common.ml does, and writes
// OUTDIR/config_table.inc
// (Config.print_config's variables, in its order) and
// OUTDIR/cppcaml/typing/config_link.inc (the typed values the C++ port
// reads; config.hpp includes it).  A file is rewritten only when its
// contents change.  No OCaml compiler is involved.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void die(const std::string& msg) {
  std::fprintf(stderr, "gen_config: %s\n", msg.c_str());
  std::exit(2);
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) die("cannot read " + p.string() + " (configure the tree first: ./configure)");
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// ---- utils/config.generated.ml: [let name = expr] bindings ----------------------------------------
// expr ::= term ('^' term)* ; term ::= {|..|} | {id|..|id} | "..." | name | true | false
//        | INT '!=' INT | INT | Some INT | None | '[' (expr (';' expr)*)? ']'
struct Value {
  std::variant<std::string, bool, long, std::optional<long>, std::vector<std::string>> v;
  const std::string& str() const {
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    die("a string was expected");
  }
  bool boolean() const {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    die("a boolean was expected");
  }
};

class Parser {
 public:
  Parser(const std::string& src, std::map<std::string, Value>& env) : s_(src), env_(env) {}

  void bindings() {
    for (;;) {
      skip();
      if (pos_ >= s_.size()) return;
      std::string kw = ident();
      if (kw != "let") die("config.generated.ml: 'let' expected, got '" + kw + "'");
      std::string name = ident();
      skip();
      expect('=');
      env_[name] = expr();
    }
  }

 private:
  void skip() {
    for (;;) {
      while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
      if (pos_ < s_.size() && s_[pos_] == '#' && (pos_ == 0 || s_[pos_ - 1] == '\n')) {  // # line directive
        while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
        continue;
      }
      if (s_.compare(pos_, 2, "(*") == 0) {
        int depth = 0;
        do {
          if (s_.compare(pos_, 2, "(*") == 0) {
            ++depth;
            pos_ += 2;
          } else if (s_.compare(pos_, 2, "*)") == 0) {
            --depth;
            pos_ += 2;
          } else if (pos_ < s_.size()) {
            ++pos_;
          } else {
            die("config.generated.ml: unterminated comment");
          }
        } while (depth > 0);
        continue;
      }
      return;
    }
  }
  void expect(char c) {
    skip();
    if (pos_ >= s_.size() || s_[pos_] != c) die(std::string("config.generated.ml: '") + c + "' expected");
    ++pos_;
  }
  bool peek(char c) {
    skip();
    return pos_ < s_.size() && s_[pos_] == c;
  }
  std::string ident() {
    skip();
    std::size_t b = pos_;
    while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
    if (b == pos_) die("config.generated.ml: identifier expected at offset " + std::to_string(b));
    return s_.substr(b, pos_ - b);
  }
  Value expr() {
    Value v = term();
    while (peek('^')) {
      ++pos_;
      v = Value{v.str() + term().str()};
    }
    return v;
  }
  Value term() {
    skip();
    if (pos_ >= s_.size()) die("config.generated.ml: expression expected");
    char c = s_[pos_];
    if (c == '{') {  // {id|...|id}
      std::size_t bar = s_.find('|', pos_);
      std::string id = s_.substr(pos_ + 1, bar - pos_ - 1);
      std::string close = "|" + id + "}";
      std::size_t e = s_.find(close, bar + 1);
      if (bar == std::string::npos || e == std::string::npos) die("config.generated.ml: bad quoted string");
      std::string r = s_.substr(bar + 1, e - bar - 1);
      pos_ = e + close.size();
      return Value{r};
    }
    if (c == '"') {
      std::string r;
      for (++pos_; pos_ < s_.size() && s_[pos_] != '"'; ++pos_) {
        if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
          char n = s_[++pos_];
          r += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        } else {
          r += s_[pos_];
        }
      }
      ++pos_;
      return Value{r};
    }
    if (c == '[') {
      ++pos_;
      std::vector<std::string> l;
      while (!peek(']')) {
        l.push_back(expr().str());
        if (peek(';')) ++pos_;
      }
      ++pos_;
      return Value{l};
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
      long n = number();
      if (peek('!') && s_.compare(pos_, 2, "!=") == 0) {  // INT != INT
        pos_ += 2;
        return Value{n != number()};
      }
      return Value{n};
    }
    std::string id = ident();
    if (id == "true") return Value{true};
    if (id == "false") return Value{false};
    if (id == "None") return Value{std::optional<long>{}};
    if (id == "Some") return Value{std::optional<long>{number()}};
    auto it = env_.find(id);
    if (it == env_.end()) die("config.generated.ml: unbound " + id);
    return it->second;
  }
  long number() {
    skip();
    std::size_t b = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    return std::stol(s_.substr(b, pos_ - b));
  }

  const std::string& s_;
  std::size_t pos_ = 0;
  std::map<std::string, Value>& env_;
};

// ---- Makefile.build_config: NAME=value lines -----------------------------------------------
std::map<std::string, std::string> make_vars(const std::string& src) {
  std::map<std::string, std::string> m;
  std::istringstream in(src);
  for (std::string line; std::getline(in, line);) {
    std::size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    m[name] = value;
  }
  return m;
}

bool defines(const std::string& header, const std::string& macro) {
  return header.find("#define " + macro + " ") != std::string::npos ||
         header.find("#define " + macro + "\n") != std::string::npos;
}

// config.common.ml's literals: [let name = value], [and name = {magic|...|magic}]
std::string common_literal(const std::string& src, const std::string& name) {
  for (const char* kw : {"let ", "and "}) {
    std::string key = std::string(kw) + name + " = ";
    std::size_t p = src.find(key);
    if (p == std::string::npos) continue;
    p += key.size();
    if (src.compare(p, 7, "{magic|") == 0) {
      std::size_t e = src.find("|magic}", p + 7);
      return src.substr(p + 7, e - p - 7);
    }
    std::size_t e = src.find_first_of("\n ", p);
    return src.substr(p, e - p);
  }
  die("config.common.ml: no " + name);
}

// C++ string literal contents (gen_driver_tables.ml's esc)
std::string esc(const std::string& s) {
  std::string r;
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      default:
        if (c < 32 || c >= 127) {
          char b[8];
          std::snprintf(b, sizeof b, "\\%03o", c);
          r += b;
        } else {
          r += static_cast<char>(c);
        }
    }
  }
  return r;
}

void write_if_changed(const fs::path& p, const std::string& text) {
  std::error_code ec;
  if (fs::exists(p, ec)) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream old;
    old << in.rdbuf();
    if (old.str() == text) return;
  }
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) die("cannot write " + p.string());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) die("usage: gen_config ROOT OUTDIR");
  fs::path root = argv[1], outdir = argv[2];

  std::map<std::string, Value> g;
  std::string generated = read_file(root / "utils/config.generated.ml");
  Parser(generated, g).bindings();
  auto S = [&](const char* n) -> const std::string& {
    auto it = g.find(n);
    if (it == g.end()) die(std::string("config.generated.ml: no ") + n);
    return it->second.str();
  };
  auto B = [&](const char* n) {
    auto it = g.find(n);
    if (it == g.end()) die(std::string("config.generated.ml: no ") + n);
    return it->second.boolean();
  };
  std::string common = read_file(root / "utils/config.common.ml");
  auto build = make_vars(read_file(root / "Makefile.build_config"));
  std::string s_h = read_file(root / "runtime/caml/s.h");
  std::string m_h = read_file(root / "runtime/caml/m.h");

  // Sys.ocaml_version, Sys.ocaml_release
  std::string version = read_file(root / "VERSION");
  version = version.substr(0, version.find('\n'));
  long major = std::stol(version.substr(0, version.find('.')));
  long minor = std::stol(version.substr(version.find('.') + 1));
  // Sys.word_size, Sys.int_size (the target's: the compiler runs on it)
  bool sixtyfour = defines(m_h, "ARCH_SIXTYFOUR");
  long word_size = sixtyfour ? 64 : 32, int_size = word_size - 1;

  // %standard_library_default (runtime/build_config.h's OCAML_STDLIB_DIR)
  // and stdlib_dirs: a relative one is resolved by the running compiler
  std::string stdlib_raw = build["TARGET_LIBDIR"];
  bool relative = build["TARGET_LIBDIR_IS_RELATIVE"] == "true";
  std::string standard_library_relative = relative ? stdlib_raw : "";
  std::string standard_library_default = stdlib_raw;

  std::string target_os_type = S("target_os_type");
  bool target_unix = target_os_type == "Unix", target_win32 = target_os_type == "Win32",
       target_cygwin = target_os_type == "Cygwin";
  std::string default_executable_name = target_unix                    ? "a.out"
                                        : target_win32 || target_cygwin ? "camlprog.exe"
                                                                        : "camlprog";
  std::string launch_method = S("launch_method");
  std::string launch_method_raw = launch_method == "exe" ? "exe" : launch_method == "sh" ? "sh" : launch_method;
  std::string search_method = S("search_method");
  std::string search_method_raw =
      search_method == "enable" ? "enable" : search_method == "fallback" ? "fallback" : "disable";
  auto magic = [&](const char* n) { return common_literal(common, n); };
  bool is_official_release = common_literal(common, "is_official_release") == "true";
  long release_number = std::stol(common_literal(common, "release_number"));
  // Compression.compression_supported: the runtime has zstd
  bool compression_supported = defines(s_h, "HAS_ZSTD");

  // ---- Config.print_config's variables (configuration_variables ()) ----
  std::ostringstream t;
  t << "// Generated by cxx/src/tools/gen_config.cpp from the configured tree: do not edit.\n";
  auto p = [&](const char* n, const std::string& v) { t << "    {\"" << n << "\", \"" << esc(v) << "\"},\n"; };
  auto pb = [&](const char* n, bool v) { p(n, v ? "true" : "false"); };
  auto pi = [&](const char* n, long v) { p(n, std::to_string(v)); };
  p("version", version);
  p("standard_library_default", standard_library_default);
  p("standard_library_relative", standard_library_relative);
  p("standard_library", standard_library_default);
  p("ccomp_type", S("ccomp_type"));
  p("c_compiler", S("c_compiler"));
  p("bytecode_cflags", S("bytecode_cflags"));
  p("ocamlc_cflags", S("bytecode_cflags"));
  p("bytecode_cppflags", S("bytecode_cppflags"));
  p("ocamlc_cppflags", S("bytecode_cppflags"));
  p("native_cflags", S("native_cflags"));
  p("ocamlopt_cflags", S("native_cflags"));
  p("native_cppflags", S("native_cppflags"));
  p("ocamlopt_cppflags", S("native_cppflags"));
  p("bytecomp_c_compiler", S("bytecomp_c_compiler"));
  p("native_c_compiler", S("native_c_compiler"));
  p("bytecomp_c_libraries", S("bytecomp_c_libraries"));
  p("native_c_libraries", S("native_c_libraries"));
  p("compression_c_libraries", S("compression_c_libraries"));
  p("native_ldflags", S("native_ldflags"));
  p("native_pack_linker", S("native_pack_linker"));
  pb("native_compiler", B("native_compiler"));
  p("architecture", S("architecture"));
  p("model", S("model"));
  pi("int_size", int_size);
  pi("word_size", word_size);
  p("system", S("system"));
  p("asm", S("asm"));
  pb("asm_cfi_supported", B("asm_cfi_supported"));
  pb("asm_size_type_directives", B("asm_size_type_directives"));
  pb("with_frame_pointers", B("with_frame_pointers"));
  pb("with_nonexecstack_note", B("with_nonexecstack_note"));
  p("ext_exe", S("ext_exe"));
  p("ext_obj", S("ext_obj"));
  p("ext_asm", S("ext_asm"));
  p("ext_lib", S("ext_lib"));
  p("ext_dll", S("ext_dll"));
  p("os_type", target_os_type);
  p("default_executable_name", default_executable_name);
  pb("systhread_supported", B("systhread_supported"));
  p("host", S("host"));
  p("target", S("target"));
  p("bytecode_runtime_id", S("bytecode_runtime_id"));
  p("native_runtime_id", S("native_runtime_id"));
  pb("flambda", B("flambda"));
  pb("safe_string", true);
  pb("default_safe_string", true);
  pb("flat_float_array", B("flat_float_array"));
  pb("align_double", B("align_double"));
  pb("align_int64", B("align_int64"));
  pb("function_sections", B("function_sections"));
  pb("afl_instrument", B("afl_instrument"));
  pb("tsan", B("tsan"));
  pb("windows_unicode", B("windows_unicode"));
  pb("supports_shared_libraries", B("supports_shared_libraries"));
  pb("native_dynlink", B("native_dynlink"));
  pb("naked_pointers", false);
  pb("with_codegen_invariants", B("with_codegen_invariants"));
  {
    auto it = g.find("reserved_header_bits");
    pi("reserved_header_bits", it == g.end() ? 0 : std::get<long>(it->second.v));
  }
  for (const char* m : {"exec_magic_number", "cmi_magic_number", "cmo_magic_number", "cma_magic_number",
                        "cmx_magic_number", "cmxa_magic_number", "ast_impl_magic_number", "ast_intf_magic_number",
                        "cmxs_magic_number", "cmt_magic_number", "linear_magic_number"})
    p(m, magic(m));

  // ---- the typed values the port reads (config.hpp) ----
  std::ostringstream l;
  l << "// Generated by cxx/src/tools/gen_config.cpp from the configured tree: do not edit.\n";
  auto str = [&](const char* n, const std::string& v) {
    l << "inline const std::string " << n << " = \"" << esc(v) << "\";\n";
  };
  auto cstr = [&](const char* n, const std::string& v) {
    l << "inline const char* const " << n << " = \"" << esc(v) << "\";\n";
  };
  auto boo = [&](const char* n, bool v) { l << "inline constexpr bool " << n << " = " << (v ? "true" : "false") << ";\n"; };
  auto num = [&](const char* n, long v) { l << "inline constexpr long " << n << " = " << v << ";\n"; };
  // configured --with-relative-libdir, Config.bindir is the directory the
  // running compiler resolved the stdlib from (relative_root_dir):
  // config.cpp recomputes it ("" here)
  str("bindir", relative ? "" : S("bindir"));
  // config.common.ml's "." rule (Filename.dirname Sys.executable_name) is
  // applied by config.cpp
  str("target_bindir_raw", S("target_bindir"));
  str("ccomp_type", S("ccomp_type"));
  str("c_compiler", S("c_compiler"));
  str("c_output_obj", S("c_output_obj"));
  boo("c_has_debug_prefix_map", B("c_has_debug_prefix_map"));
  boo("as_has_debug_prefix_map", B("as_has_debug_prefix_map"));
  str("bytecode_cflags", S("bytecode_cflags"));
  str("bytecode_cppflags", S("bytecode_cppflags"));
  str("native_cflags", S("native_cflags"));
  str("native_cppflags", S("native_cppflags"));
  str("bytecomp_c_libraries", S("bytecomp_c_libraries"));
  str("native_pack_linker", S("native_pack_linker"));
  str("ar", S("ar"));
  boo("ar_supports_response_files", B("ar_supports_response_files"));
  str("mkdll", S("mkdll"));
  str("mkexe", S("mkexe"));
  str("mkmaindll", S("mkmaindll"));
  str("system", S("system"));
  str("host", S("host"));
  str("target", S("target"));
  boo("target_win32", target_win32);
  boo("windows_unicode", B("windows_unicode"));
  boo("supports_shared_libraries", B("supports_shared_libraries"));
  str("compression_c_libraries", S("compression_c_libraries"));
  boo("compression_supported", compression_supported);
  boo("suffixing", B("suffixing"));
  boo("shebangscripts", B("shebangscripts"));
  boo("flat_float_array", B("flat_float_array"));
  boo("with_frame_pointers", B("with_frame_pointers"));
  boo("tsan", B("tsan"));
  boo("is_official_release", is_official_release);
  num("release_number", release_number);
  {
    auto it = g.find("reserved_header_bits");
    num("reserved_header_bits", it == g.end() ? 0 : std::get<long>(it->second.v));
  }
  num("int_size", int_size);
  num("ocaml_release_major", major);
  num("ocaml_release_minor", minor);
  for (const char* m : {"exec_magic_number", "cmo_magic_number", "cma_magic_number", "cmi_magic_number",
                        "cmt_magic_number"})
    str(m, magic(m));
  boo("flambda", B("flambda"));
  boo("with_cmm_invariants", B("with_cmm_invariants"));
  boo("with_codegen_invariants", B("with_codegen_invariants"));
  boo("with_flambda_invariants", B("with_flambda_invariants"));
  boo("function_sections", B("function_sections"));
  boo("afl_instrument", B("afl_instrument"));
  str("architecture", S("architecture"));
  str("model", S("model"));
  str("asm_", S("asm"));  // (asm: a C++ keyword)
  boo("asm_cfi_supported", B("asm_cfi_supported"));
  str("cmx_magic_number", magic("cmx_magic_number"));
  str("cmxa_magic_number", magic("cmxa_magic_number"));
  str("launch_method_raw", launch_method_raw);
  str("search_method_raw", search_method_raw);
  {
    auto it = g.find("flexdll_dirs");
    std::string dirs;
    if (it != g.end())
      for (const std::string& d : std::get<std::vector<std::string>>(it->second.v))
        dirs += (dirs.empty() ? "\"" : ", \"") + esc(d) + "\"";
    l << "inline const std::vector<std::string> flexdll_dirs = {" << dirs << "};\n";
  }
  // file names
  cstr("default_executable_name", default_executable_name);
  cstr("ext_exe", S("ext_exe"));
  cstr("ext_obj", S("ext_obj"));
  cstr("ext_asm", S("ext_asm"));
  cstr("ext_lib", S("ext_lib"));
  cstr("ext_dll", S("ext_dll"));

  write_if_changed(outdir / "config_table.inc", t.str());
  write_if_changed(outdir / "cppcaml/typing/config_link.inc", l.str());
  return 0;
}
