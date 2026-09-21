// c++ocamlc — the drop-in driver: a command-line-compatible bytecode `ocamlc`.
//
//   c++ocamlc -c foo.ml                    # foo.ml -> foo.cmo (+ foo.cmi)
//   c++ocamlc a.cmo b.cmo -o prog          # link into a runnable bytecode exe
//   c++ocamlc -I src -w +a-4 -c src/x.ml   # accepts ocamlc's flag vocabulary
//
// Runs the whole c++caml pipeline (parse -> infer -> Lambda -> Bytegen ->
// emitcode) then links against the stdlib and writes a `#!ocamlrun` launcher so
// the result is directly executable.  No ocamlc involved; only ocamlrun (the C
// VM) and the prebuilt stdlib objects are reused.
//
// CLI policy: flags whose ARGUMENT we must consume (else they would be mistaken
// for a source file) and meaning-preserving toggles are accepted and ignored;
// flags that would silently change the meaning of the output if ignored (-pp,
// -ppx, -pack, -a, -open, ...) are reported as unsupported rather than dropped.
#include <sys/stat.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/bytecode.hpp"
#include "cppcaml/cmi.hpp"
#include "cppcaml/cmo.hpp"
#include "cppcaml/dbgenv.hpp"
#include "cppcaml/infer_check.hpp"
#include "cppcaml/lambda.hpp"
#include "cppcaml/link.hpp"
#include "cppcaml/parser.hpp"

namespace fs = std::filesystem;

static const char* kVersion = "5.6.0+dev0-2026-01-26";

// Locate the stdlib directory (containing stdlib.cmi / stdlib.cma): an explicit
// -I, else $OCAMLLIB/$CAMLLIB, else "stdlib" relative to the CWD, else relative
// to this executable (<repo>/cxx/build/c++ocamlc -> <repo>/stdlib).
static std::string discover_stdlib(const std::string& flag) {
  if (!flag.empty()) return flag;
  if (const char* e = std::getenv("OCAMLLIB"); e && *e) return e;
  if (const char* e = std::getenv("CAMLLIB"); e && *e) return e;
  if (fs::exists("stdlib/stdlib.cmi")) return "stdlib";
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path cand = exe.parent_path().parent_path().parent_path() / "stdlib";
    if (fs::exists(cand / "stdlib.cmi")) return cand.string();
  }
  return "stdlib";
}

// Print the full command-line help.  Mirrors `ocamlc -help` in spirit but only
// documents what c++ocamlc actually implements; the large remainder of ocamlc's
// vocabulary is accepted-and-ignored (or rejected, see -strict-flags) for
// drop-in compatibility rather than honoured.
static void print_help(std::ostream& os) {
  os <<
      "Usage: c++ocamlc [options] <files>\n"
      "\n"
      "A drop-in bytecode compiler: parses, type-infers, and compiles OCaml\n"
      "source to .cmo/.cmi, and links .cmo/.cma objects into a runnable\n"
      "#!ocamlrun bytecode executable.  Accepts ocamlc's flag vocabulary.\n"
      "\n"
      "  c++ocamlc -c foo.ml              # foo.ml -> foo.cmo (+ foo.cmi)\n"
      "  c++ocamlc a.cmo b.cmo -o prog    # link objects into an executable\n"
      "\n"
      "Options:\n"
      "  -a              Build a .cma library from the given .cmo files\n"
      "  -c              Compile only (do not link); stop at .cmo/.cmi\n"
      "  -I <dir>        Add <dir> to the list of include directories\n"
      "                  (a `+dir' is taken relative to the stdlib directory)\n"
      "  -impl <file>    Compile <file> as a .ml regardless of its extension\n"
      "  -intf <file>    Compile <file> as a .mli regardless of its extension\n"
      "  -nocwd          Do not implicitly search the current directory for\n"
      "                  compiled interfaces\n"
      "  -nostdlib       Do not add the stdlib directory to the include path,\n"
      "                  (stdlib.cma is still linked unless -nopervasives)\n"
      "  -o <file>       Set the output file name\n"
      "  -pack           Package the given .cmo files into one unit (needs -o)\n"
      "  -runtime <file> Use <file> as the ocamlrun launched by the output exe\n"
      "  -strict-flags   Turn accepted-but-ignored and unknown options into\n"
      "                  errors instead of silently dropping them\n"
      "  -dparsetree     Dump the parsed AST to stdout, then keep compiling\n"
      "  -dlambda        Dump the Lambda IR to stdout, then keep compiling\n"
      "  -dinstr         Dump the bytecode instructions to stdout, then keep going\n"
      "  -config         Print the compiler configuration and exit\n"
      "  -version        Print the compiler version and exit\n"
      "  -vnum           Print the compiler version number and exit\n"
      "  -where          Print the standard library directory and exit\n"
      "  -help, --help   Print this help and exit\n"
      "\n"
      "Many other ocamlc options (-w, -g, -bin-annot, ...) are accepted for\n"
      "compatibility and ignored; a few that would silently change the output\n"
      "(-pp, -ppx, -open, -for-pack, -i, -output-obj) are rejected.  Use\n"
      "-strict-flags to also reject the ignored ones.\n";
}

static std::string module_name(const std::string& path) {
  std::string base = fs::path(path).filename().string();
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty()) base[0] = (char)std::toupper((unsigned char)base[0]);
  return base;
}

// ocamlc spells a stdlib-relative include dir as `+unix` (= <stdlib>/unix in an
// install).  In this repo's dev layout the otherlibs live at <stdlib>/../otherlibs
// instead, so fall back there when <stdlib>/<x> is absent.
static std::string resolve_incdir(const std::string& d, const std::string& stdlib_dir) {
  if (d.empty() || d[0] != '+') return d;
  fs::path inst = fs::path(stdlib_dir) / d.substr(1);
  if (fs::exists(inst)) return inst.string();
  fs::path dev = fs::path(stdlib_dir).parent_path() / "otherlibs" / d.substr(1);
  if (fs::exists(dev)) return dev.string();
  return inst.string();
}

static bool ends_with(const std::string& s, const char* suf) {
  size_t n = std::string(suf).size();
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// Flags taking ONE argument that we accept and ignore (must consume the arg so
// it is not mistaken for a source file).
static const std::set<std::string> kArgIgnore = {
    "-w", "-warn-error", "-alert", "-color", "-error-style", "-cclib", "-ccopt",
    "-dllib", "-dllpath", "-stop-after", "-intf-suffix", "-intf_suffix",
    "-cmi-file", "-dump-dir", "-inline", "-afl-inst-ratio", "-function-sections",
    "-match-context-rows", "-runtime-variant"};
// Boolean flags we accept and ignore (meaning-preserving for our bytecode output).
static const std::set<std::string> kBoolIgnore = {
    "-safe-string", "-unsafe-string", "-strict-sequence", "-no-strict-sequence",
    "-strict-formats", "-no-strict-formats", "-bin-annot", "-bin-annot-occurrences",
    "-annot", "-g", "-no-g", "-opaque", "-principal", "-no-principal", "-rectypes",
    "-no-rectypes", "-short-paths", "-keep-locs", "-no-keep-locs", "-keep-docs",
    "-no-keep-docs", "-absname", "-no-absname", "-noassert", "-unsafe",
    "-no-alias-deps", "-alias-deps", "-app-funct", "-no-app-funct", "-compat-32",
    "-noautolink", "-linkall", "-custom", "-no-check-prims", "-bytecode",
    "-make-runtime", "-make_runtime", "-with-runtime", "-without-runtime",
    "-warn-help", "-warn-error-help",
    // typing/dump switches with no effect on our .cmo/.cmi output
    "-typing-recovery", "-dno-unique-ids", "-dunique-ids", "-dno-locations", "-dlocations"};
// Flags that would silently change the output if dropped -> reported unsupported.
static const std::set<std::string> kUnsupportedArg = {"-pp", "-ppx", "-open",
                                                      "-for-pack"};
static const std::set<std::string> kUnsupportedBool = {"-i", "-output-obj"};
// -labels/-nolabels affect typing but not our (untyped-after-infer) output.
static const std::set<std::string> kBoolIgnore2 = {"-labels", "-nolabels"};

// ocamlc's exact "Unbound module" report: location line, source excerpt with
// carets under the offending name, then the error -- e.g.
//   File "a.ml", line 1, characters 5-14:
//   1 | open Nosuchmod
//            ^^^^^^^^^
//   Error: Unbound module Nosuchmod
static void report_unbound_module(const std::string& in_path, const std::string& src,
                                  const cppcaml::lambda::UnboundModuleError& e) {
  std::cerr << "File \"" << in_path << "\", line " << e.line
            << ", characters " << e.col_start << '-' << e.col_end << ":\n";
  // find the source line (1-based)
  size_t pos = 0;
  for (int l = 1; l < e.line && pos != std::string::npos; ++l)
    pos = src.find('\n', pos) == std::string::npos ? std::string::npos
                                                   : src.find('\n', pos) + 1;
  if (pos != std::string::npos) {
    size_t eol = src.find('\n', pos);
    std::string text = src.substr(pos, eol == std::string::npos ? std::string::npos
                                                                : eol - pos);
    std::string num = std::to_string(e.line);
    std::cerr << num << " | " << text << '\n';
    std::cerr << std::string(num.size() + 3 + e.col_start, ' ')
              << std::string(std::max(1, e.col_end - e.col_start), '^') << '\n';
  }
  std::cerr << "Error: Unbound module " << e.head << '\n';
}

// Compile a single .ml -> .cmo (+ .cmi unless a hand-written .mli exists).
// Returns 0 on success.  `cmo_out` is where the .cmo is written.
// -d* debug dumps (like ocamlc's): print an intermediate representation to
// stdout during compilation and keep going.  Byte-comparable (after the usual
// label/stamp normalization) with the matching `ocamlc -d*` and with the
// standalone c++parse / c++lambda / c++instr tools.
struct DumpFlags { bool parsetree = false, lambda = false, instr = false; };
static DumpFlags g_dump;
static bool g_nopervasives = false;  // -nopervasives: no implicit Stdlib import

static int compile_ml(const std::string& in_path, const std::string& cmo_out,
                      const std::string& stdlib_dir, bool prof) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) { std::cerr << "c++ocamlc: cannot open " << in_path << '\n'; return 2; }
  std::ostringstream ss; ss << in.rdbuf();
  std::string mod = module_name(in_path);
  cppcaml::clear_head_cmi_cache();  // a prior unit's fresh .cmi must be visible
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  auto lap = [&](const char* what, clk::time_point& prev) {
    auto now = clk::now();
    if (prof)
      std::cerr << "  " << what << ": "
                << std::chrono::duration<double, std::milli>(now - prev).count() << " ms\n";
    prev = now;
  };
  try {
    auto tp = t0;
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    lap("parse", tp);
    if (g_dump.parsetree)
      cppcaml::ast::print_dparsetree(structure, in_path, std::cout, dirfiles);
    std::vector<std::string> required_globals;
    std::size_t eta_sites = 0, pv_reify = 0;
    auto code = cppcaml::lambda::translate_implementation(structure, mod, stdlib_dir, in_path,
                                                          &required_globals, &dirfiles,
                                                          &eta_sites, &pv_reify);
    lap("translate (infer+lambda)", tp);
    if (g_dump.lambda) cppcaml::lambda::print_dlambda(code, std::cout);
    auto instrs = cppcaml::bytecode::compile_implementation(code, mod);
    lap("bytegen", tp);
    if (g_dump.instr) cppcaml::bytecode::print_dinstr(instrs, std::cout);
    // Write the .cmi BEFORE the .cmo so write_cmo can read the interface CRCs it
    // records (a hand-written .mli's .cmi already exists on disk from earlier).
    try {  // best-effort .cmi from inference (unless a hand-written .mli owns it)
      fs::path cmi_path = fs::path(cmo_out).replace_extension(".cmi");
      bool has_mli = fs::exists(fs::path(in_path).replace_extension(".mli"));
      if (!has_mli)  // inferred from the .ml -> Impl provenance in the uids
        // The source path is the file_id-0 entry of the location table: without
        // it every declaration's pos_fname came out "" (NOMLLOC reverts).
        // The saved stamps continue ocamlc's global ident counter: 273 after
        // the initial environment, plus what typing allocated (NOSTAMPBASE
        // reverts to the flat 300).
      {
        auto sig = cppcaml::infer_signature(structure);
        const std::set<std::string> fexp = cppcaml::fexp_paths(sig);
        // The crc list is what typing imported: every unit whose .cmi the
        // stamp count read, each with its own imports (NOCMIIMPORTS reverts
        // to the signature's citations).
        std::set<std::string> loaded;
        const bool imports = !cppcaml::dbg_env("NOCMIIMPORTS");
        // The count at each constructor's / label's ident: their saved
        // stamps, Subst keeping those idents (S557; NOLDSTAMP reverts).
        std::map<std::string, long long> at;
        int base = cppcaml::dbg_env("NOSTAMPBASE")
                       ? 300
                       : 274 + cppcaml::typing_ident_count(
                                   structure, eta_sites,
                                   cppcaml::package_sig_idents(sig), &fexp,
                                   pv_reify, &loaded, &at);
        std::map<std::string, int> stamps;
        if (!cppcaml::dbg_env("NOSTAMPBASE") && !cppcaml::dbg_env("NOLDSTAMP"))
          for (auto& e : at) stamps[e.first] = 274 + (int)e.second;
        // The uid typing gave each declaration -- the local binders on the
        // way included (NOUIDWALK reverts to numbering the signature).
        cppcaml::UidMap um;
        if (!cppcaml::dbg_env("NOUIDWALK")) um = cppcaml::typing_uid_map(structure);
        cppcaml::cmi::cmiw::write_cmi(
            cmi_path.string(), mod, sig,
            imports ? cppcaml::cmi::cmiw::cmi_imports(loaded, mod,
                                                      !g_nopervasives)
                    : std::vector<cppcaml::cmi::cmiw::Import>{},
            /*intf=*/false,
            cppcaml::dbg_env("NOMLLOC") ? std::vector<std::string>{}
                                        : std::vector<std::string>{in_path},
            base, /*cite=*/!imports, um.complete ? &um.ids : nullptr,
            stamps.empty() ? nullptr : &stamps);
      }
    } catch (const std::exception& e) {
      if (prof) std::cerr << "  (.cmi emission skipped: " << e.what() << ")\n";
    }
    cppcaml::cmo::write_cmo(instrs, mod, cmo_out, required_globals);
    lap("write_cmo", tp);
    if (prof)
      std::cerr << "  TOTAL compile " << in_path << ": "
                << std::chrono::duration<double, std::milli>(clk::now() - t0).count() << " ms\n";
  } catch (const cppcaml::lambda::UnboundModuleError& e) {
    report_unbound_module(in_path, ss.str(), e);
    return 2;  // ocamlc's exit code for a type error
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++ocamlc: " << in_path << ": parse error at " << e.pos << ": " << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "c++ocamlc: " << in_path << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}

// Compile a .mli -> .cmi.
static int compile_mli(const std::string& in_path, const std::string& cmi_out) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) { std::cerr << "c++ocamlc: cannot open " << in_path << '\n'; return 2; }
  std::ostringstream ss; ss << in.rdbuf();
  cppcaml::clear_head_cmi_cache();  // a prior unit's fresh .cmi must be visible
  try {
    auto sig = cppcaml::parse_signature(ss.str());
    cppcaml::cmi::cmiw::write_cmi(cmi_out, module_name(in_path), cppcaml::signature_to_cmi(sig),
                                  {}, /*intf=*/true, /*src_files=*/{in_path},
                                  cppcaml::dbg_env("NOSTAMPBASE")
                                      ? 300
                                      : 274 + cppcaml::typing_ident_count(sig));
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++ocamlc: " << in_path << ": parse error at " << e.pos << ": " << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "c++ocamlc: " << in_path << ": .cmi write failed: " << e.what() << '\n';
    return 1;
  }
  return 0;
}

static int run_main(int argc, char** argv) {
  std::string out_path, stdlib_flag, runtime;
  std::vector<std::string> incdirs_raw;  // -I dirs (may be `+unix`), in order
  std::vector<std::string> inputs;       // positional files (.ml/.mli/.cmo/.cma)
  bool compile_only = false;             // -c : stop at the .cmo / .cmi
  bool make_lib = false;                 // -a : build a .cma archive
  std::string pack_name;                 // -pack : build a packed unit (name from -o)
  bool nostdlib = false;                 // -nostdlib : no default stdlib search dir
  bool nocwd = false;                    // -nocwd : no implicit cwd cmi search
  bool nopervasives = false;             // -nopervasives : Stdlib not implicitly opened
  bool prof = std::getenv("CPPCAML_PROFILE") != nullptr;

  // -strict-flags makes accepted-but-ignored / unknown options hard errors.
  // Detect it up front so it governs options appearing before it on the line.
  bool strict_flags = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "-strict-flags") strict_flags = true;
  auto reject_ignored = [&](const std::string& a) {
    std::cerr << "c++ocamlc: option " << a
              << " is accepted-but-ignored and -strict-flags is set\n";
    std::exit(2);
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need_arg = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "c++ocamlc: option " << what << " needs an argument\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-help" || a == "--help" || a == "-h") { print_help(std::cout); return 0; }
    else if (a == "-strict-flags") { /* handled in the pre-scan above */ }
    else if (a == "-o") out_path = need_arg("-o");
    else if (a == "-I") incdirs_raw.push_back(need_arg("-I"));
    // -H <dir>: APPROXIMATION -- searched like -I.  ocamlc lets a hidden dir
    // satisfy dependencies only, never a direct reference; the hidden_includes
    // tests that expect "Unbound module" therefore wrongly pass here.
    else if (a == "-H") incdirs_raw.push_back(need_arg("-H"));
    else if (a == "-runtime") runtime = need_arg("-runtime");
    else if (a == "-use-runtime" || a == "-use_runtime")  // ocamlc's spelling of -runtime
      runtime = need_arg(a.c_str());
    else if (a == "-c") compile_only = true;
    else if (a == "-dparsetree") g_dump.parsetree = true;   // dump AST, keep going
    else if (a == "-dlambda") g_dump.lambda = true;         // dump Lambda IR
    else if (a == "-dinstr") g_dump.instr = true;           // dump bytecode instrs
    else if (a == "-a") make_lib = true;
    else if (a == "-pack") pack_name = "?";  // resolved from -o once known
    else if (a == "-nostdlib") nostdlib = true;
    else if (a == "-nocwd") nocwd = true;
    else if (a == "-nopervasives") nopervasives = true;
    else if (a == "-stdlib") stdlib_flag = need_arg("-stdlib");
    else if (a == "-version") { std::cout << kVersion << '\n'; return 0; }
    else if (a == "-vnum") { std::cout << kVersion << '\n'; return 0; }
    else if (a == "-where") { std::cout << discover_stdlib(stdlib_flag) << '\n'; return 0; }
    else if (a == "-config") {
      std::cout << "version: " << kVersion << "\nstandard_library: "
                << fs::absolute(discover_stdlib(stdlib_flag)).string()
                << "\next_obj: .o\next_lib: .a\next_dll: .so\nos_type: Unix\n";
      return 0;
    } else if (a == "-impl") inputs.push_back(need_arg("-impl"));   // force .ml kind
    else if (a == "-intf") inputs.push_back(need_arg("-intf"));     // force .mli kind
    else if (kArgIgnore.count(a)) {
      if (strict_flags) reject_ignored(a);
      (void)need_arg(a.c_str());
    }
    else if (kBoolIgnore.count(a) || kBoolIgnore2.count(a)) {
      if (strict_flags) reject_ignored(a);
      /* else accept, ignore */
    }
    else if (kUnsupportedArg.count(a)) {
      std::cerr << "c++ocamlc: " << a << " is not supported yet\n";
      (void)need_arg(a.c_str());
      return 2;
    } else if (kUnsupportedBool.count(a)) {
      std::cerr << "c++ocamlc: " << a << " is not supported yet\n";
      return 2;
    } else if (!a.empty() && a[0] == '-') {
      // An unrecognised flag: error under -strict-flags, else warn and keep
      // going (be lenient for drop-in use).
      if (strict_flags) {
        std::cerr << "c++ocamlc: unknown option " << a << " (-strict-flags)\n";
        return 2;
      }
      std::cerr << "c++ocamlc: warning: ignoring unknown option " << a << '\n';
    } else {
      inputs.push_back(a);  // a source/object file
    }
  }

  if (inputs.empty()) {
    std::cerr << "usage: c++ocamlc [-c] [-I <dir>]... <files...> [-o <out>]\n"
                 "       c++ocamlc -help   for the full list of options\n";
    return 2;
  }

  // stdlib_dir: an explicit -stdlib wins (pins every unit to one stdlib so their
  // interface CRCs stay consistent -- vital when bootstrapping against a stdlib
  // that is still being built and whose stdlib.cmi may not exist yet); else the
  // -I dir that actually holds stdlib.cmi; else discovery.
  std::string stdlib_dir = stdlib_flag;
  if (stdlib_dir.empty())
    for (const std::string& d : incdirs_raw)
      if (d.empty() || d[0] != '+')
        if (fs::exists(fs::path(d) / "stdlib.cmi")) { stdlib_dir = d; break; }
  stdlib_dir = discover_stdlib(stdlib_dir);

  std::vector<std::string> incdirs;  // resolved (+unix -> <stdlib>/unix)
  for (const std::string& d : incdirs_raw) incdirs.push_back(resolve_incdir(d, stdlib_dir));
  // ocamlc searches the current directory FIRST (before the -I dirs) unless
  // -nocwd is given, so a sibling unit's .cmi is found without an explicit -I.
  static const bool no_cwd_search = cppcaml::dbg_env("NOCWDSEARCH") != nullptr;
  if (!nocwd && !no_cwd_search) incdirs.insert(incdirs.begin(), ".");
  cppcaml::lambda::set_module_dirs(incdirs);
  cppcaml::lambda::set_nopervasives(nopervasives);
  g_nopervasives = nopervasives;
  cppcaml::set_infer_module_dirs(incdirs);
  cppcaml::set_infer_stdlib_dir(stdlib_dir);
  cppcaml::cmi::cmiw::set_module_dirs(stdlib_dir, incdirs);

  if (runtime.empty()) {
    fs::path r = fs::absolute(fs::path(stdlib_dir)).parent_path() / "runtime" / "ocamlrun";
    if (fs::exists(r)) runtime = r.string();
  }

  // Compile every source input; collect the resulting (and pre-built) objects
  // for a possible link step.
  std::vector<std::string> link_objs;
  // A bare object name (`unix.cma`) on the link line is resolved against the
  // -I include path, like ocamlc -- otherwise only a cwd-relative path works.
  auto resolve_obj = [&](const std::string& f) -> std::string {
    if (fs::exists(f)) return f;
    for (const std::string& d : incdirs) {
      fs::path cand = fs::path(d) / f;
      if (fs::exists(cand)) return cand.string();
    }
    return f;  // leave as-is; the linker reports the open failure
  };
  for (const std::string& f : inputs) {
    if (ends_with(f, ".mli")) {
      std::string cmi_out =
          (compile_only && !out_path.empty() && ends_with(out_path, ".cmi"))
              ? out_path
              : (fs::path(f).parent_path() / (fs::path(f).stem().string() + ".cmi")).string();
      if (int rc = compile_mli(f, cmi_out)) return rc;
    } else if (ends_with(f, ".ml")) {
      std::string cmo_out =
          (compile_only && !out_path.empty() && ends_with(out_path, ".cmo"))
              ? out_path
              : (fs::path(f).parent_path() / (fs::path(f).stem().string() + ".cmo")).string();
      if (int rc = compile_ml(f, cmo_out, stdlib_dir, prof)) return rc;
      link_objs.push_back(cmo_out);
    } else if (ends_with(f, ".cmo") || ends_with(f, ".cma")) {
      link_objs.push_back(resolve_obj(f));  // a pre-compiled object/library to link
    } else if (ends_with(f, ".cmi")) {
      link_objs.push_back(resolve_obj(f));  // interface-only member (meaningful under -pack)
    } else {
      std::cerr << "c++ocamlc: don't know what to do with " << f << '\n';
      return 2;
    }
  }

  if (compile_only) return 0;       // -c : no link

  if (make_lib) {  // -a : bundle the .cmo objects into a .cma
    if (out_path.empty()) out_path = "a.cma";
    try {
      cppcaml::link::archive(link_objs, out_path);
    } catch (const std::exception& e) {
      std::cerr << "c++ocamlc: -a: " << e.what() << '\n';
      return 1;
    }
    return 0;
  }
  if (!pack_name.empty()) {  // -pack -o Pack.cmo : consolidate into one unit
    if (out_path.empty()) { std::cerr << "c++ocamlc: -pack needs -o <Pack>.cmo\n"; return 2; }
    try {
      cppcaml::link::pack(link_objs, module_name(out_path), out_path);
    } catch (const std::exception& e) {
      std::cerr << "c++ocamlc: -pack: " << e.what() << '\n';
      return 1;
    }
    return 0;
  }

  if (link_objs.empty()) return 0;  // only .mli inputs

  // Link: [stdlib.cma] + objects + [std_exit.cmo] -> runnable bytecode launcher.
  // Like Bytelink.link, only -nopervasives drops the implicit stdlib.cma /
  // std_exit.cmo; -nostdlib merely removes the default dir from the search
  // path, and stdlib_dir above was already resolved from -I in that case.
  if (out_path.empty()) out_path = "a.out";
  std::vector<std::string> linkin;
  if (!nopervasives) linkin.push_back(stdlib_dir + "/stdlib.cma");
  for (const std::string& o : link_objs) linkin.push_back(o);
  if (!nopervasives) linkin.push_back(stdlib_dir + "/std_exit.cmo");
  try {
    cppcaml::link::link_executable(linkin, out_path, runtime);
  } catch (const std::exception& e) {
    std::cerr << "c++ocamlc: link error: " << e.what() << '\n';
    return 1;
  }
  chmod(out_path.c_str(), 0755);
  return 0;
}

int main(int argc, char** argv) {
  // Every .cmo/.cmi/executable is written through a local, RAII std::ofstream
  // that has already flushed and closed by the time run_main returns; the only
  // process-lifetime streams are std::cout/std::cerr (dumps, diagnostics).  So
  // once those are flushed there is nothing left to do but free memory the OS
  // is about to reclaim anyway -- and that teardown is not cheap: the never-
  // erased cmi cache (g_load_cache) held a large shared_ptr<TypeExpr> graph
  // whose recursive destruction was the single hottest function at exit (~6%
  // of a warm compile).  Skip all static destructors and atexit handlers with
  // _Exit; the output bytes are identical, we just stop paying to unbuild the
  // in-memory graphs on the way out.
  int rc = run_main(argc, argv);
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  // Leak-checkers (asan/valgrind) and -pg profiling need the normal exit path
  // (static destructors, atexit-registered gmon writer) to run; opt out there.
  if (std::getenv("CPPCAML_NO_FASTEXIT")) return rc;
  std::_Exit(rc);
}
