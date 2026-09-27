// Port of driver/compenv.ml (see compenv.hpp).
#include "cppcaml/typing/compenv.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::compenv {

namespace fs = std::filesystem;
using clflags::Pass;

// Filename.check_suffix / remove_extension
namespace {
bool check_suffix(const std::string& name, const std::string& suff) {
  return name.size() >= suff.size() && name.compare(name.size() - suff.size(), suff.size(), suff) == 0;
}
// Filename.extension_len (Unix): the extension starts at the last '.' of the
// basename, not at its first character
std::string remove_extension(const std::string& name) {
  std::size_t i = name.size();
  while (i > 0) {
    char c = name[i - 1];
    if (c == '/') return name;
    if (c == '.') {
      // a leading dot of the basename (".foo") is not an extension
      std::size_t j = i - 1;
      bool only_dots_before = true;
      for (std::size_t k = j; k > 0; --k) {
        if (name[k - 1] == '/') break;
        if (name[k - 1] != '.') {
          only_dots_before = false;
          break;
        }
      }
      if (only_dots_before) return name;
      return name.substr(0, j);
    }
    --i;
  }
  return name;
}
}  // namespace

void fatal(const std::string& err) {
  std::cout.flush();
  std::cerr << err << '\n';
  std::cerr.flush();
  throw ExitWithStatus{2};
}

std::string output_prefix(const std::string& name) {
  std::string oname = name;
  if (clflags::output_name && clflags::compile_only) {
    oname = *clflags::output_name;
    clflags::output_name.reset();
  }
  return remove_extension(oname);
}

void print_version_and_library(const std::string& compiler) {
  std::cout << "The OCaml " << compiler << ", version " << config::version() << '\n';
  std::cout << "Standard library directory: " << config::standard_library << '\n';
  std::cout.flush();
  throw ExitWithStatus{0};
}

void print_version_string() {
  std::cout << config::version() << '\n';
  std::cout.flush();
  throw ExitWithStatus{0};
}

void print_standard_library() {
  std::cout << config::standard_library << '\n';
  std::cout.flush();
  throw ExitWithStatus{0};
}

std::string extract_output(const std::optional<std::string>& o) {
  if (o) return *o;
  fatal("Please specify the name of the output file, using option -o");
}

std::string default_output(const std::optional<std::string>& o) { return o ? *o : config::default_executable_name; }

std::vector<std::string> first_include_dirs, last_include_dirs;
std::vector<std::string> first_ccopts, last_ccopts;
std::vector<std::string> first_ppx, last_ppx;
std::vector<std::string> first_objfiles, last_objfiles;
bool stop_early = false;

std::vector<std::string> rev_split_words(const std::string& s) {
  std::vector<std::string> res;  // (built in the order OCaml conses: reversed)
  std::size_t i = 0, n = s.size();
  auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (i < n) {
    if (sp(s[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && !sp(s[j])) ++j;
    res.push_back(s.substr(i, j - i));
    i = j;
  }
  return std::vector<std::string>(res.rbegin(), res.rend());
}

std::string expand_directory(const std::string& alt, const std::string& s) {
  if (!s.empty() && s[0] == '+') {
    // Filename.concat alt (String.sub s 1 ...)
    std::string rest = s.substr(1);
    if (alt.empty() || alt.back() == '/') return alt + rest;
    return alt + "/" + rest;
  }
  return s;
}

// ---- OCAMLPARAM -------------------------------------------------------------------

namespace {

struct SyntaxError {
  std::string msg;
};

// print_error ppf msg: the Bad_env_variable warning
void print_error(const std::string& msg) {
  warnings::Warning w = warnings::Warning::make(warnings::Warning::K::Bad_env_variable);
  w.s = "OCAMLPARAM";
  w.s2 = msg;
  location::print_warning(location::none(), location::err_formatter(), w);
  location::err_flush();
}

std::string quote_S(const std::string& s) { return "\"" + format::string_escaped(s) + "\""; }

std::vector<std::string> split_on_char(char c, const std::string& s) {
  std::vector<std::string> r;
  std::string cur;
  for (char x : s) {
    if (x == c) {
      r.push_back(cur);
      cur.clear();
    } else {
      cur += x;
    }
  }
  r.push_back(cur);
  return r;
}

using Binding = std::pair<std::string, std::string>;

std::pair<std::vector<Binding>, std::vector<Binding>> parse_args(const std::string& s) {
  std::vector<std::string> args;
  if (!s.empty()) {
    char c = s[0];
    if (c == ':' || c == '|' || c == ';' || c == ' ' || c == ',') {
      args = split_on_char(c, s);
      args.erase(args.begin());
    } else {
      args = split_on_char(',', s);
    }
  }
  bool is_after = false;
  std::vector<Binding> before, after;
  for (const std::string& a : args) {
    if (a.empty()) continue;
    if (a == "_") {
      if (is_after) throw SyntaxError{"too many '_' separators"};
      is_after = true;
      continue;
    }
    std::size_t eq = a.find('=');
    if (eq == std::string::npos) throw SyntaxError{"missing '=' in " + a};
    Binding b{a.substr(0, eq), a.substr(eq + 1)};
    (is_after ? after : before).push_back(b);
  }
  if (!is_after) throw SyntaxError{"no '_' separator found"};
  return {before, after};
}

// setter ppf f name options s
void setter(bool negate, const std::string& name, std::vector<bool*> options, const std::string& s) {
  bool b;
  if (s == "0") b = false;
  else if (s == "1") b = true;
  else {
    print_error("bad value " + s + " for " + name);
    return;
  }
  for (bool* o : options) *o = negate ? !b : b;
}

bool check_bool(const std::string& name, const std::string& s) {
  if (s == "0") return false;
  if (s == "1") return true;
  print_error("bad value " + s + " for " + name);
  return false;
}

std::optional<Pass> decode_compiler_pass(const std::string& v, const std::string& name) {
  static const std::vector<std::string> passes = {"parsing", "typing", "lambda"};
  if (v == "parsing") return Pass::Parsing;
  if (v == "typing") return Pass::Typing;
  if (v == "lambda") return Pass::Lambda;
  std::string l;
  for (std::size_t i = 0; i < passes.size(); ++i) l += (i ? ", " : "") + passes[i];
  print_error("bad value " + v + " for option \"" + name + "\" (expected one of: " + l + ")");
  return std::nullopt;
}

std::vector<std::string> can_discard;

// the native / flambda settings OCAMLPARAM may carry: parsed (and their
// values checked) as ocamlc does, then unused by a bytecode compiler
struct NativeOnly {
  bool afl_instrument = false, clambda_checks = false, function_sections = false, keep_asm_file = false,
       keep_startup_file = false, optimize_for_speed = true, dlcode = true, force_slash = false,
       classic_inlining = false, unbox_closures = false, remove_unused_arguments = false, inlining_report = false,
       dump_flambda_verbose = false, flambda_invariant_checks = false, cmm_invariants = false, use_linscan = false,
       insn_sched = false, pic_code = false;
} g_native;

void parse_warnings(bool error, const std::string& v) {
  if (std::optional<warnings::Alert> a = warnings::parse_options(error, v))
    location::prerr_alert(location::none(), *a);
}

void read_one_param(Position position, const std::string& name, const std::string& v) {
  auto set = [&](const std::string& n, std::vector<bool*> o) { setter(false, n, std::move(o), v); };
  auto clear = [&](const std::string& n, std::vector<bool*> o) { setter(true, n, std::move(o), v); };
  auto int_setter = [&](const std::string& n, long* o) {
    long x;
    if (arg::int_of_string_opt(v, x)) *o = x;
    else print_error("non-integer parameter " + v + " for " + quote_S(n));
  };
  auto compat = [&](const std::string& n) {
    bool dummy = true;
    if (v == "0") print_error("Unsetting " + n + " is not supported anymore");
    else if (v != "1") print_error("bad value " + v + " for " + n);
    (void)dummy;
  };
  namespace cf = clflags;
  static long afl_inst_ratio = 100, unbox_closures_factor = 10;
  static std::optional<long> simplify_rounds;
  if (name == "g") set("g", {&cf::debug});
  else if (name == "bin-annot") set("bin-annot", {&cf::binary_annotations});
  else if (name == "afl-instrument") set("afl-instrument", {&g_native.afl_instrument});
  else if (name == "afl-inst-ratio") int_setter("afl-inst-ratio", &afl_inst_ratio);
  else if (name == "annot") set("annot", {&cf::annotations});
  else if (name == "absname") set("absname", {&cf::absname});
  else if (name == "compat-32") set("compat-32", {&cf::bytecode_compatible_32});
  else if (name == "noassert") set("noassert", {&cf::noassert});
  else if (name == "noautolink") set("noautolink", {&cf::no_auto_link});
  else if (name == "nostdlib") set("nostdlib", {&cf::no_std_include});
  else if (name == "nocwd") set("nocwd", {&cf::no_cwd});
  else if (name == "linkall") set("linkall", {&cf::link_everything});
  else if (name == "nolabels") set("nolabels", {&cf::classic});
  else if (name == "principal") set("principal", {&cf::principal});
  else if (name == "rectypes") set("rectypes", {&cf::recursive_types});
  else if (name == "safe-string") compat("safe-string");
  else if (name == "strict-sequence") set("strict-sequence", {&cf::strict_sequence});
  else if (name == "strict-formats") set("strict-formats", {&cf::strict_formats});
  else if (name == "thread") set("thread", {&cf::use_threads});
  else if (name == "unboxed-types") set("unboxed-types", {&cf::unboxed_types});
  else if (name == "unsafe") set("unsafe", {&cf::unsafe});
  else if (name == "verbose") set("verbose", {&cf::verbose});
  else if (name == "nopervasives") set("nopervasives", {&cf::nopervasives});
  else if (name == "slash") set("slash", {&g_native.force_slash});
  else if (name == "no-slash") clear("no-slash", {&g_native.force_slash});
  else if (name == "keep-docs") set("keep-docs", {&cf::keep_docs});
  else if (name == "keep-locs") set("keep-locs", {&cf::keep_locs});
  else if (name == "compact") clear("compact", {&g_native.optimize_for_speed});
  else if (name == "no-app-funct") clear("no-app-funct", {&cf::applicative_functors});
  else if (name == "nodynlink") clear("nodynlink", {&g_native.dlcode});
  else if (name == "short-paths") clear("short-paths", {&cf::real_paths});
  else if (name == "typing-recovery") set("typing-recovery", {&cf::typing_recovery});
  else if (name == "no-alias-deps") set("no-alias-deps", {&cf::no_alias_deps});
  else if (name == "opaque") set("opaque", {&cf::opaque});
  else if (name == "pp") cf::preprocessor = v;
  else if (name == "runtime-variant") cf::runtime_variant = v;
  else if (name == "with-runtime") set("with-runtime", {&cf::with_runtime});
  else if (name == "open") {
    for (const std::string& m : split_on_char(',', v)) cf::open_modules.insert(cf::open_modules.begin(), m);
  } else if (name == "cc") cf::c_compiler = v;
  else if (name == "clambda-checks") set("clambda-checks", {&g_native.clambda_checks});
  else if (name == "function-sections") set("function-sections", {&g_native.function_sections});
  else if (name == "s") set("s", {&g_native.keep_asm_file, &g_native.keep_startup_file});
  else if (name == "S") set("S", {&g_native.keep_asm_file});
  else if (name == "dstartup") set("dstartup", {&g_native.keep_startup_file});
  else if (name == "we" || name == "warn-error") parse_warnings(true, v);
  else if (name == "w") parse_warnings(false, v);
  else if (name == "wwe") parse_warnings(false, v);
  else if (name == "alert") warnings::parse_alert_option(v);
  // inlining: native settings (their Arg_helper syntax is not checked here)
  else if (name == "inline" || name == "inline-toplevel" || name == "inline-max-unroll" ||
           name == "inline-call-cost" || name == "inline-alloc-cost" || name == "inline-prim-cost" ||
           name == "inline-branch-cost" || name == "inline-indirect-cost" || name == "inline-lifting-benefit" ||
           name == "inline-branch-factor" || name == "inline-max-depth") {
  } else if (name == "rounds") {
    long x;
    if (arg::int_of_string_opt(v, x)) simplify_rounds = x;
    else print_error("non-integer parameter " + v + " for " + quote_S("rounds"));
  } else if (name == "Oclassic") set("Oclassic", {&g_native.classic_inlining});
  else if (name == "O2") (void)check_bool("O2", v);
  else if (name == "O3") (void)check_bool("O3", v);
  else if (name == "unbox-closures") set("unbox-closures", {&g_native.unbox_closures});
  else if (name == "unbox-closures-factor") int_setter("unbox-closures-factor", &unbox_closures_factor);
  else if (name == "remove-unused-arguments") set("remove-unused-arguments", {&g_native.remove_unused_arguments});
  else if (name == "inlining-report") {
    if (cf::native_code) set("inlining-report", {&g_native.inlining_report});
  } else if (name == "flambda-verbose") set("flambda-verbose", {&g_native.dump_flambda_verbose});
  else if (name == "flambda-invariants") set("flambda-invariants", {&g_native.flambda_invariant_checks});
  else if (name == "cmm-invariants") set("cmm-invariants", {&g_native.cmm_invariants});
  else if (name == "linscan") set("linscan", {&g_native.use_linscan});
  else if (name == "insn-sched") set("insn-sched", {&g_native.insn_sched});
  else if (name == "no-insn-sched") clear("insn-sched", {&g_native.insn_sched});
  else if (name == "color") {
    if (v == "auto") cf::color = cf::Color::Auto;
    else if (v == "always") cf::color = cf::Color::Always;
    else if (v == "never") cf::color = cf::Color::Never;
    else print_error("bad value " + v + " for \"color\", (expected \"auto\", \"always\" or \"never\")");
  } else if (name == "error-style") {
    if (v == "contextual") cf::error_style = cf::ErrorStyle::Contextual;
    else if (v == "short") cf::error_style = cf::ErrorStyle::Short;
    else print_error("bad value " + v + " for \"error-style\", (expected \"contextual\" or \"short\")");
  } else if (name == "intf-suffix") config::interface_suffix = v;
  else if (name == "I") {
    if (position == Position::Before_args) first_include_dirs.insert(first_include_dirs.begin(), v);
    else last_include_dirs.insert(last_include_dirs.begin(), v);
  } else if (name == "cclib") {
    if (position != Position::Before_compile) {
      std::vector<std::string> w = rev_split_words(v);
      cf::ccobjs.insert(cf::ccobjs.begin(), w.begin(), w.end());
    }
  } else if (name == "ccopt" || name == "ccopts") {
    if (position == Position::Before_args) first_ccopts.insert(first_ccopts.begin(), v);
    else last_ccopts.insert(last_ccopts.begin(), v);
  } else if (name == "ppx") {
    if (position == Position::Before_args) first_ppx.insert(first_ppx.begin(), v);
    else last_ppx.insert(last_ppx.begin(), v);
  } else if (name == "cmo" || name == "cma") {
    if (!cf::native_code) {
      if (position == Position::Before_args) first_objfiles.insert(first_objfiles.begin(), v);
      else last_objfiles.insert(last_objfiles.begin(), v);
    }
  } else if (name == "cmx" || name == "cmxa") {
    if (cf::native_code) {
      if (position == Position::Before_args) first_objfiles.insert(first_objfiles.begin(), v);
      else last_objfiles.insert(last_objfiles.begin(), v);
    }
  } else if (name == "pic") {
    if (cf::native_code) set("pic", {&g_native.pic_code});
  } else if (name == "can-discard") {
    can_discard.insert(can_discard.begin(), v);
  } else if (name == "timings" || name == "profile") {
    cf::profile = check_bool(name, v);
  } else if (name == "stop-after") {
    if (std::optional<Pass> pass = decode_compiler_pass(v, name)) {
      if (!cf::stop_after) cf::stop_after = *pass;
      else if (*cf::stop_after != *pass) print_error("Please specify at most one " + name + " <pass>.");
    }
  } else if (name == "save-ir-after") {
    // native only
  } else if (name == "dump-into-file") {
    cf::dump_into_file = true;
  } else if (name == "dump-dir") {
    cf::dump_dir = v;
  } else if (name == "dump") {
    // Clflags.Dump_option: raw_lambda, lambda, instr and the others
    bool value = true;
    std::string key = v;
    if (!v.empty() && v[0] == '-') value = false, key = v.substr(1);
    else if (!v.empty() && v[0] == '+') key = v.substr(1);
    bool* flag = nullptr;
    bool native_only = false;
    if (key == "source") flag = &cf::dump_source;
    else if (key == "parsetree") flag = &cf::dump_parsetree;
    else if (key == "typedtree") flag = &cf::dump_typedtree;
    else if (key == "shape") flag = &cf::dump_shape;
    else if (key == "match_comp") flag = &cf::dump_matchcomp;
    else if (key == "raw_lambda") flag = &cf::dump_rawlambda;
    else if (key == "lambda") flag = &cf::dump_lambda;
    else if (key == "instr") flag = &cf::dump_instr;
    else if (key == "raw_clambda" || key == "clambda" || key == "raw_flambda" || key == "flambda" ||
             key == "cmm" || key == "selection" || key == "combine" || key == "cse" || key == "live" ||
             key == "spill" || key == "split" || key == "interf" || key == "prefer" || key == "regalloc" ||
             key == "scheduling" || key == "linear" || key == "interval")
      native_only = true;
    if (!flag && !native_only) print_error("bad value " + key + " for option \"dump\".");
    else if (native_only) print_error("dump=" + key + ": this option is only available in native code.");
    else *flag = value;
  } else if (name == "keywords") {
    cf::keyword_edition = v;
  } else {
    bool known = false;
    for (const std::string& d : can_discard) known = known || d == name;
    if (!known) {
      can_discard.insert(can_discard.begin(), name);
      print_error("Warning: discarding value of variable " + quote_S(name) + " in OCAMLPARAM\n");
    }
  }
}

void read_OCAMLPARAM(Position position) {
  const char* e = std::getenv("OCAMLPARAM");
  if (!e) return;
  std::string s = e;
  if (s.empty()) return;
  std::vector<Binding> before, after;
  try {
    std::tie(before, after) = parse_args(s);
  } catch (const SyntaxError& err) {
    print_error(err.msg);
  }
  for (auto& [name, v] : position == Position::Before_args ? before : after) read_one_param(position, name, v);
}

// OCAMLPARAM passed as file: <stdlib>/ocaml_compiler_internal_params, lines
// `pattern : name = value` (Scanf "%[0-9a-zA-Z_.*/] : %[a-zA-Z_-] = %s ")
struct FileOption {
  bool any;
  std::string pattern, name, value;
};

std::vector<FileOption> load_config(const std::string& filename) {
  std::ifstream ic(filename, std::ios::binary);
  if (!ic) {
    location::Report r = location::errorf(location::in_file(filename), "Cannot open file %s", "Sys_error");
    location::print_report(location::err_formatter(), r);
    location::err_flush();
    throw ExitWithStatus{2};
  }
  std::string text((std::istreambuf_iterator<char>(ic)), std::istreambuf_iterator<char>());
  std::vector<FileOption> lines;
  std::size_t i = 0, n = text.size();
  auto ws = [&] {
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
  };
  auto run = [&](auto pred) {
    std::size_t j = i;
    while (i < n && pred(text[i])) ++i;
    return text.substr(j, i - j);
  };
  long line_number = 0;
  std::size_t line_start = 0;
  while (i < n) {
    std::string pattern = run([](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '*' || c == '/';
    });
    bool ok = true;
    ws();
    if (i < n && text[i] == ':') ++i;
    else ok = false;
    std::string name, value;
    if (ok) {
      ws();
      name = run([](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
      ws();
      if (i < n && text[i] == '=') ++i;
      else ok = false;
    }
    if (ok) {
      ws();
      value = run([](char c) { return !(c == ' ' || c == '\t' || c == '\n' || c == '\r'); });
      ws();
    }
    if (!ok) {
      Location loc = location::in_file(filename);
      loc.loc_start.pos_lnum = loc.loc_end.pos_lnum = line_number;
      loc.loc_start.pos_bol = loc.loc_end.pos_bol = static_cast<long>(line_start);
      loc.loc_start.pos_cnum = loc.loc_end.pos_cnum = static_cast<long>(i);
      loc.loc_ghost = false;
      location::Report r = location::errorf(loc, "Configuration file error %s", "scanf: bad input");
      location::print_report(location::err_formatter(), r);
      location::err_flush();
      throw ExitWithStatus{2};
    }
    lines.push_back({pattern == "*", pattern, name, value});
    ++line_number;
    line_start = i;
  }
  return std::vector<FileOption>(lines.rbegin(), lines.rend());  // (read conses)
}

std::string lowercase(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void apply_config_file(Position position, const std::string& filename) {
  std::string config_file = config::standard_library + "/ocaml_compiler_internal_params";
  std::vector<FileOption> config;
  std::error_code ec;
  if (fs::exists(config_file, ec)) config = load_config(config_file);
  for (const FileOption& o : config) {
    bool keep = position == Position::Before_compile ? (o.any || lowercase(filename) == lowercase(o.pattern))
                                                      : o.any;
    if (keep) read_one_param(position, o.name, o.value);
  }
}

}  // namespace

void readenv(Position position, const std::string& filename) {
  last_include_dirs.clear();
  last_ccopts.clear();
  last_ppx.clear();
  last_objfiles.clear();
  apply_config_file(position, filename);
  read_OCAMLPARAM(position);
  clflags::all_ccopts = last_ccopts;
  clflags::all_ccopts.insert(clflags::all_ccopts.end(), first_ccopts.begin(), first_ccopts.end());
  clflags::all_ppx = last_ppx;
  clflags::all_ppx.insert(clflags::all_ppx.end(), first_ppx.begin(), first_ppx.end());
}

std::vector<std::string> get_objfiles(bool with_ocamlparam) {
  std::vector<std::string> l;
  if (with_ocamlparam) {
    l = last_objfiles;
    l.insert(l.end(), clflags::objfiles.begin(), clflags::objfiles.end());
    l.insert(l.end(), first_objfiles.begin(), first_objfiles.end());
  } else {
    l = clflags::objfiles;
  }
  return std::vector<std::string>(l.rbegin(), l.rend());
}

// ---- deferred actions ---------------------------------------------------------------

namespace {
std::vector<DeferredAction> deferred_actions;  // (latest first, as the OCaml list)

std::string c_object_of_filename(const std::string& name) {
  std::string base = fs::path(name).filename().string();
  return base.substr(0, base.size() - 2) + config::ext_obj;
}

DeferredAction action_of_file(const std::string& name) {
  using K = DeferredAction::K;
  if (check_suffix(name, ".ml") || check_suffix(name, ".mlt")) return {K::ProcessImplementation, name, {}, false};
  if (check_suffix(name, config::interface_suffix)) return {K::ProcessInterface, name, {}, false};
  if (check_suffix(name, ".c")) return {K::ProcessCFile, name, {}, false};
  return {K::ProcessOtherFile, name, {}, false};
}

void process_action(const ActionContext& ctx, const DeferredAction& action) {
  namespace cf = clflags;
  using K = DeferredAction::K;
  auto impl_ = [&](Pass start_from, const std::string& name) {
    readenv(Position::Before_compile, name);
    std::string opref = output_prefix(name);
    if (int rc = ctx.compile_implementation(start_from, name, opref)) throw ExitWithStatus{rc};
    cf::objfiles.insert(cf::objfiles.begin(), opref + ctx.ocaml_mod_ext);
  };
  switch (action.k) {
    case K::ProcessImplementation: impl_(Pass::Parsing, action.name); break;
    case K::ProcessInterface: {
      readenv(Position::Before_compile, action.name);
      std::string opref = output_prefix(action.name);
      if (int rc = ctx.compile_interface(action.name, opref)) throw ExitWithStatus{rc};
      if (cf::make_package) cf::objfiles.insert(cf::objfiles.begin(), opref + ".cmi");
      break;
    }
    case K::ProcessCFile: {
      readenv(Position::Before_compile, action.name);
      location::input_name = action.name;
      // Ccomp.compile_file: c++ocamlc does not drive a C compiler
      fatal("c++ocamlc: compiling C files (" + action.name + ") is not supported yet");
    }
    case K::ProcessObjects:
      cf::ccobjs.insert(cf::ccobjs.begin(), action.names.begin(), action.names.end());
      break;
    case K::ProcessDLLs: {
      std::vector<std::pair<bool, std::string>> l;
      for (const std::string& n : action.names) l.emplace_back(action.suffixed, n);
      cf::dllibs.insert(cf::dllibs.begin(), l.begin(), l.end());
      break;
    }
    case K::ProcessOtherFile: {
      const std::string& name = action.name;
      if (check_suffix(name, ctx.ocaml_mod_ext) || check_suffix(name, ctx.ocaml_lib_ext)) {
        cf::objfiles.insert(cf::objfiles.begin(), name);
      } else if (check_suffix(name, ".cmi") && cf::make_package) {
        cf::objfiles.insert(cf::objfiles.begin(), name);
      } else if (check_suffix(name, config::ext_obj) || check_suffix(name, config::ext_lib)) {
        cf::ccobjs.insert(cf::ccobjs.begin(), name);
      } else if (!cf::native_code && check_suffix(name, config::ext_dll)) {
        cf::dllibs.insert(cf::dllibs.begin(), {false, name});
      } else if (check_suffix(name, ".cmir-linear")) {
        // Compiler_pass.of_input_filename: start from Emit, which bytecode
        // cannot (Compile.implementation's fatal error)
        location::input_name = name;
        readenv(Position::Before_compile, name);
        (void)output_prefix(name);
        std::cout.flush();
        std::cerr << ">> Fatal error: Cannot start from emit\n";
        std::cerr.flush();
        throw ExitWithStatus{2};
      } else {
        throw arg::Bad("Don't know what to do with " + name);
      }
      break;
    }
  }
}
}  // namespace

void defer(DeferredAction a) { deferred_actions.insert(deferred_actions.begin(), std::move(a)); }
void anonymous(const std::string& filename) { defer(action_of_file(filename)); }
void impl(const std::string& filename) {
  defer({DeferredAction::K::ProcessImplementation, filename, {}, false});
}
void intf(const std::string& filename) { defer({DeferredAction::K::ProcessInterface, filename, {}, false}); }

void process_deferred_actions(const ActionContext& env) {
  namespace cf = clflags;
  using K = DeferredAction::K;
  std::optional<std::string> final_output_name = cf::output_name;
  if (!cf::compile_only) cf::output_name.reset();
  if (final_output_name && cf::compile_only) {
    long n = 0;
    for (const DeferredAction& a : deferred_actions)
      if (a.k == K::ProcessCFile || a.k == K::ProcessImplementation || a.k == K::ProcessInterface) ++n;
    if (n > 1) fatal("Options -c -o are incompatible with compiling multiple files");
  }
  if (cf::make_archive) {
    for (const DeferredAction& a : deferred_actions)
      if (a.k == K::ProcessOtherFile && check_suffix(a.name, ".cmxa"))
        fatal("Option -a cannot be used with .cmxa input files.");
  } else if (deferred_actions.empty()) {
    fatal("No input files");
  }
  for (auto it = deferred_actions.rbegin(); it != deferred_actions.rend(); ++it) process_action(env, *it);
  cf::output_name = final_output_name;
  stop_early = cf::compile_only || cf::print_types || cf::stop_after.has_value();
}

// ---- the argument list and its reports ----------------------------------------------

std::vector<arg::Option>& arg_spec() {
  static std::vector<arg::Option> spec;
  return spec;
}

void add_arguments(const std::vector<arg::Option>& args) {
  for (const arg::Option& a : args) {
    bool dup = false;
    for (const arg::Option& o : arg_spec()) dup = dup || o.key == a.key;
    if (dup) {
      std::cerr << "Warning: compiler argument " << a.key << " is already defined:\n";
      continue;
    }
    arg_spec().push_back(a);
  }
}

std::string create_usage_msg(const std::string& program) {
  return "Usage: " + program + " <options> <files>\nTry '" + program + " --help' for more information.";
}

void print_arguments(const std::string& program) { arg::usage(arg_spec(), create_usage_msg(program)); }

namespace {
std::string trim(const std::string& s) {
  auto is_space = [](char c) { return c == ' ' || c == '\012' || c == '\n' || c == '\r' || c == '\t'; };
  std::size_t i = 0, j = s.size();
  while (i < j && is_space(s[i])) ++i;
  while (j > i && is_space(s[j - 1])) --j;
  return s.substr(i, j - i);
}
}  // namespace

void parse_arguments(std::vector<std::string>& argv, const arg::AnonFun& f, const std::string& program) {
  long current = 0;
  try {
    arg::parse_and_expand_argv_dynamic(current, argv, arg_spec(), f, std::string(1, '\0'));
  } catch (const arg::Bad& e) {
    std::string usage_msg = create_usage_msg(program);
    std::string err_msg = e.msg;
    err_msg = trim(err_msg.substr(0, err_msg.find('\0')));
    std::cout.flush();
    std::cerr << err_msg << '\n' << usage_msg << '\n';
    std::cerr.flush();
    throw ExitWithStatus{2};
  } catch (const arg::Help& h) {
    std::string msg;
    for (char c : h.msg)
      if (c != '\0') msg += c;
    std::cout << "Usage: " << program << " <options> <files>\nOptions are:" << '\n' << msg;
    std::cout.flush();
    throw ExitWithStatus{0};
  }
}

void parse_runtime_parameter(const std::string& opt) {
  std::size_t eq = opt.find('=');
  if (eq == std::string::npos)
    fatal("-set-runtime-default: invalid runtime parameter '" + opt + "'. Expected <name>=<value>.");
  std::string k = opt.substr(0, eq), setting = opt.substr(eq + 1);
  if (k == "standard_library_default") clflags::standard_library_default_override = setting;
  else fatal("-set-runtime-default: unrecognized runtime parameter " + k + ".");
}

}  // namespace cppcaml::typing::compenv
