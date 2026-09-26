// c++ocamlc — the drop-in driver: a command-line-compatible bytecode `ocamlc`.
//
//   c++ocamlc -c foo.ml                    # foo.ml -> foo.cmo (+ foo.cmi)
//   c++ocamlc a.cmo b.cmo -o prog          # link into a runnable bytecode exe
//   c++ocamlc -I src -w +a-4 -c src/x.ml   # accepts ocamlc's flag vocabulary
//
// Runs the whole c++caml pipeline -- parse, then the ports of ocamlc's
// typing/ (Typemod), lambda/ (Translmod, Simplif) and bytecomp/ (Bytegen,
// Emitcode) as driver/compile.ml sequences them -- then links against the stdlib and writes a `#!ocamlrun` launcher so
// the result is directly executable.  No ocamlc involved; only ocamlrun (the C
// VM) and the prebuilt stdlib objects are reused.
//
// CLI policy: flags whose ARGUMENT we must consume (else they would be mistaken
// for a source file) and meaning-preserving toggles are accepted and ignored;
// flags that would silently change the meaning of the output if ignored (-pp,
// -ppx, -pack, -a, -open, ...) are reported as unsupported rather than dropped.
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/cmi.hpp"
#include "cppcaml/dbgenv.hpp"
#include "cppcaml/link.hpp"
#include "cppcaml/parser.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/error_report.hpp"
#include "cppcaml/typing/parsetree.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typemod.hpp"
#include "cppcaml/typing/printlambda.hpp"
#include "cppcaml/typing/bytegen.hpp"
#include "cppcaml/typing/bytepackager.hpp"
#include "cppcaml/typing/emitcode.hpp"
#include "cppcaml/typing/printinstr.hpp"
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/warnings.hpp"

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
      "A drop-in bytecode compiler: parses, type-checks, and compiles OCaml\n"
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
      "  -drawlambda     Dump the Lambda IR before Simplif to stderr\n"
      "  -dlambda        Dump the Lambda IR to stderr, then keep compiling\n"
      "  -dinstr         Dump the bytecode instructions to stderr, then keep going\n"
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
    "-color", "-error-style", "-cclib", "-ccopt",
    "-dllib", "-dllpath", "-intf-suffix", "-intf_suffix",
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
static const std::set<std::string> kUnsupportedArg = {"-ppx"};
static const std::set<std::string> kUnsupportedBool = {"-i", "-output-obj"};
// -labels/-nolabels affect typing but not our (untyped-after-infer) output.
static const std::set<std::string> kBoolIgnore2 = {"-labels", "-nolabels"};


// Compile a single .ml -> .cmo (+ .cmi unless a hand-written .mli exists).
// Returns 0 on success.  `cmo_out` is where the .cmo is written.
// -d* debug dumps (like ocamlc's): print an intermediate representation to
// stdout during compilation and keep going.  Byte-comparable (after the usual
// label/stamp normalization) with the matching `ocamlc -d*` and with the
// standalone c++parse / c++lambda / c++instr tools.
struct DumpFlags { bool parsetree = false, lambda = false, instr = false, rawlambda = false; };
static DumpFlags g_dump;
static bool g_nopervasives = false;  // -nopervasives: no implicit Stdlib import

// Clflags the ported type checker reads (the code generator ignores them)
static void set_typing_flag(const std::string& a) {
  namespace cf = cppcaml::typing::clflags;
  if (a == "-principal") cf::principal = true;
  else if (a == "-no-principal") cf::principal = false;
  else if (a == "-rectypes") cf::recursive_types = true;
  else if (a == "-no-rectypes") cf::recursive_types = false;
  else if (a == "-strict-sequence") cf::strict_sequence = true;
  else if (a == "-no-strict-sequence") cf::strict_sequence = false;
  else if (a == "-strict-formats") cf::strict_formats = true;
  else if (a == "-no-strict-formats") cf::strict_formats = false;
  else if (a == "-app-funct") cf::applicative_functors = true;
  else if (a == "-no-app-funct") cf::applicative_functors = false;
  else if (a == "-no-alias-deps") cf::no_alias_deps = true;
  else if (a == "-alias-deps") cf::no_alias_deps = false;
  else if (a == "-nolabels") cf::classic = true;
  else if (a == "-labels") cf::classic = false;
  else if (a == "-unsafe") cf::unsafe = true;
  else if (a == "-noassert") cf::noassert = true;
  else if (a == "-g") cf::debug = true;
  else if (a == "-no-g") cf::debug = false;
  else if (a == "-keep-locs") cf::keep_locs = true;
  else if (a == "-no-keep-locs") cf::keep_locs = false;
  else if (a == "-keep-docs") cf::keep_docs = true;
  else if (a == "-no-keep-docs") cf::keep_docs = false;
  else if (a == "-opaque") cf::opaque = true;
}

// -stop-after parsing / typing
enum class StopAfter { None, Parsing, Typing, Lambda };
static StopAfter g_stop_after = StopAfter::None;

// -I directories (resolved) and -nostdlib, for the ported type checker
static std::vector<std::string> g_incdirs;
// -open M (Clflags.open_modules, in command-line order)
static std::vector<std::string> g_open_modules;
// -pp command (Clflags.preprocessor)
static std::optional<std::string> g_preprocessor;

// Filename.quote (Unix): the string in single quotes, each ' as '\''
static std::string filename_quote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

// The source text Pparse parses: the file itself, or with -pp the output of
// `pp 'file' > tmpfile` (call_external_preprocessor); positions still name
// the source file (Location.init lexbuf sourcefile).  False on an error,
// reported as ocamlc does.
static bool read_source(const std::string& path, std::string& text) {
  std::string input = path;
  std::string tmp;
  if (g_preprocessor) {
    char tmpl[] = "/tmp/ocamlppXXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
      std::cerr << "c++ocamlc: cannot create a temporary file\n";
      return false;
    }
    ::close(fd);
    tmp = tmpl;
    std::string comm = *g_preprocessor + " " + filename_quote(path) + " > " + tmp;
    if (std::system(comm.c_str()) != 0) {
      std::remove(tmp.c_str());
      std::cerr << "File \"" << path << "\", line 1:\nError: Error while running external preprocessor\n"
                << "Command line: " << comm << '\n';
      return false;
    }
    input = tmp;
  }
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    std::cerr << "c++ocamlc: cannot open " << input << '\n';
    if (!tmp.empty()) std::remove(tmp.c_str());
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  if (!tmp.empty()) std::remove(tmp.c_str());  // remove_preprocessed
  return true;
}
static std::string g_stdlib_dir;
static bool g_nostdlib = false;

// The typing port's forward references and module-init state (the idents
// its modules create at startup, in link order), once per process
static void install_typing() {
  static bool installed = false;
  if (!installed) {
    cppcaml::typing::typemod::install_forward_refs();
    installed = true;
  }
}

// Compmisc.init_path: the cwd, the -I directories in command-line order,
// then the stdlib
static void init_path(const std::string& stdlib_dir) {
  namespace ty = cppcaml::typing;
  std::vector<std::string> visible{""};
  for (auto& d : g_incdirs) visible.push_back(d);
  // c++ocamlc finds its stdlib through -I when given one (ocamlc has
  // Config.standard_library); that directory is not added twice, as the
  // units of a later duplicate would shadow the opened Stdlib's
  bool listed = false;
  for (auto& d : g_incdirs) {
    std::error_code ec;
    if (fs::equivalent(d, stdlib_dir, ec)) listed = true;
  }
  if (!g_nostdlib && !listed) visible.push_back(stdlib_dir);
  ty::load_path::init(visible, {});
  ty::env::reset_cache();
}

// Compmisc.initial_env
static cppcaml::typing::env::t initial_env() {
  namespace ty = cppcaml::typing;
  ty::ident::reinit();
  ty::uid::reinit();
  ty::types::reset();
  ty::ctype::reset();
  ty::Location cmdline = ty::location::none();
  cmdline.loc_start.pos_fname = cmdline.loc_end.pos_fname = "command line";
  return ty::typemod::initial_env(
      cmdline, ty::clflags::nopervasives ? std::nullopt : std::optional<std::string>("Stdlib"), g_open_modules);
}

// The type checker (the typing/ port, TYPECHECKER.md): Compmisc.init_path +
// initial_env, then Typemod.type_implementation / type_interface, before
// code generation as in ocamlc.  A type error is reported as ocamlc's
// location line and the error's constructor until Printtyp is ported.  An
// internal failure is reported as one (CPPCAML_TYPECHECK_DEBUG adds the
// error's details).
using PortBody = std::function<void(cppcaml::typing::env::t, const cppcaml::typing::typemod::UnitInfo&)>;
// Typed: the port typed the unit (and, for an .ml without .mli, wrote its
// .cmi); Failed: an internal failure (reported); Rejected: a type error was
// reported.
enum class PortResult { Typed, Failed, Rejected };
static PortResult port_typecheck(const std::string& in_path, const std::string& mod, const std::string& stdlib_dir,
                           const std::string& out, bool intf, const PortBody& body) {
  namespace ty = cppcaml::typing;
  install_typing();
  const bool debug = cppcaml::dbg_env("CPPCAML_TYPECHECK_DEBUG");
  try {
    init_path(stdlib_dir);
    // Compile_common: Env.set_current_unit; Compmisc.initial_env
    ty::env::set_current_unit(ty::UnitInfo{mod, intf ? ty::Uid::From::Intf : ty::Uid::From::Impl});
    ty::env::t env0 = initial_env();
    ty::typemod::UnitInfo target;
    target.source_file = in_path;
    target.modname = mod;
    target.prefix = fs::path(out).replace_extension("").string();
    target.has_mli = !intf && fs::exists(fs::path(in_path).replace_extension(".mli"));
    target.cmi_file = target.prefix + ".cmi";
    body(env0, target);
    return PortResult::Typed;
  } catch (const std::bad_function_call&) {
    std::cerr << "c++ocamlc: " << in_path << ": internal error: an unported part of typing/\n";
    return PortResult::Failed;
  } catch (...) {
    std::optional<ty::error_report::Report> r = ty::error_report::classify(std::current_exception());
    if (!r) {
      try {
        throw;
      } catch (const std::exception& e) {
        std::cerr << "c++ocamlc: " << in_path << ": internal error in the type checker: " << e.what() << '\n';
      } catch (...) {
        std::cerr << "c++ocamlc: " << in_path << ": internal error in the type checker\n";
      }
      return PortResult::Failed;
    }
    // an error without a location here: Location.in_file !input_name
    ty::Location l = ty::location::none();
    if (r->printed_loc) l = *r->printed_loc;
    else if (r->loc) l = *r->loc;
    else l.loc_start.pos_fname = l.loc_end.pos_fname = "";
    std::cerr << ty::error_report::format_loc(l, in_path);
    std::cerr << ":\nError: " << r->name << '\n';
    if (debug && !r->detail.empty()) std::cerr << "  (" << r->detail << ")\n";
    return PortResult::Rejected;
  }
}

static int compile_ml(const std::string& in_path, const std::string& cmo_out,
                      const std::string& stdlib_dir, bool prof) {
  namespace ty = cppcaml::typing;
  std::string src;  // Pparse.parse_file: the (preprocessed) source text
  if (!read_source(in_path, src)) return 2;
  // Unit_info.modname: from the output prefix (-o stdlib__Arg.cmo -> Stdlib__Arg)
  std::string mod = module_name(cmo_out);
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
    auto structure = cppcaml::parse_structure(src, dirfiles);
    lap("parse", tp);
    if (g_dump.parsetree)
      cppcaml::ast::print_dparsetree(structure, in_path, std::cout, dirfiles);
    if (g_stop_after == StopAfter::Parsing) return 0;
    // Compile_common.implementation: typecheck_impl
    std::optional<ty::typedtree::Implementation> impl;
    PortResult port = port_typecheck(in_path, mod, stdlib_dir, cmo_out, /*intf=*/false,
                                     [&](ty::env::t env0, const ty::typemod::UnitInfo& target) {
                                       ty::parsetree::Structure st = ty::parsetree::of_ast(structure, in_path, dirfiles);
                                       // an .ml without .mli: its .cmi is written here (Typemod)
                                       impl = ty::typemod::type_implementation(target, env0, st);
                                     });
    if (port != PortResult::Typed) return 2;
    lap("typecheck", tp);
    if (g_stop_after == StopAfter::Typing) return 0;
    // Compile.to_bytecode: Translmod.transl_implementation, -drawlambda,
    // Simplif.simplify_lambda, -dlambda, Bytegen.compile_implementation,
    // -dinstr (the dumps on stderr, as ocamlc's ppf_dump)
    ty::translmod::install_forward_refs();
    ty::lambda::Program prog = ty::translmod::transl_implementation(mod, impl->structure, impl->coercion);
    if (g_dump.rawlambda) std::cerr << ty::printlambda::dump(prog.code);
    ty::lambda::lambda lam = ty::simplif::simplify_lambda(prog.code);
    if (g_dump.lambda) std::cerr << ty::printlambda::dump(lam);
    lap("lambda", tp);
    if (g_stop_after == StopAfter::Lambda) return 0;
    ty::instruct::code bytecode = ty::bytegen::compile_implementation(mod, lam);
    if (g_dump.instr) std::cerr << ty::printinstr::dump(bytecode);
    lap("bytegen", tp);
    // Compile.emit_bytecode: Emitcode.to_file, the .cmo removed on failure
    std::FILE* oc = std::fopen(cmo_out.c_str(), "wb");
    if (!oc) {
      std::cerr << "c++ocamlc: cannot open " << cmo_out << "\n";
      return 2;
    }
    try {
      ty::emitcode::to_file(oc, cmo_out, mod, prog.required_globals, bytecode);
    } catch (...) {
      std::fclose(oc);
      std::remove(cmo_out.c_str());
      throw;
    }
    std::fclose(oc);
    lap("emitcode", tp);
    if (prof)
      std::cerr << "  TOTAL compile " << in_path << ": "
                << std::chrono::duration<double, std::milli>(clk::now() - t0).count() << " ms\n";
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
  std::string src;  // Pparse.parse_file: the (preprocessed) source text
  if (!read_source(in_path, src)) return 2;
  try {
    auto sig = cppcaml::parse_signature(src);
    if (g_stop_after == StopAfter::Parsing) return 0;
    PortResult port = port_typecheck(
        in_path, module_name(cmi_out), g_stdlib_dir, cmi_out, /*intf=*/true,
        [&](cppcaml::typing::env::t env0, const cppcaml::typing::typemod::UnitInfo& target) {
          namespace ty = cppcaml::typing;
          ty::parsetree::Signature sg = ty::parsetree::of_ast_signature(sig, in_path, {});
          // Compile_common.typecheck_intf
          const ty::typedtree::Signature* tsg = ty::typemod::type_interface(target, env0, sg);
          (void)ty::includemod::signatures(env0, true, tsg->sig_type, tsg->sig_type);
          ty::typecore::force_delayed_checks();
          if (g_stop_after == StopAfter::Typing) return;
          // Compile_common.emit_signature
          ty::env::save_signature(ty::builtin_attributes::alerts_of_sig(sg), tsg->sig_type, target.modname,
                                  target.prefix + ".cmi");
        });
    if (port != PortResult::Typed) return 2;
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++ocamlc: " << in_path << ": parse error at " << e.pos << ": " << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "c++ocamlc: " << in_path << ": " << e.what() << '\n';
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
    else if (a == "-dlambda") g_dump.lambda = cppcaml::typing::clflags::dump_lambda = true;         // dump Lambda IR
    else if (a == "-drawlambda") g_dump.rawlambda = true;   // dump Lambda before Simplif (new path)
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
    else if (a == "-w" || a == "-warn-error" || a == "-alert") {
      // Warnings.parse_options / parse_alert_option, in command-line order
      std::string v = need_arg(a.c_str());
      namespace w = cppcaml::typing::warnings;
      try {
        if (a == "-alert") w::parse_alert_option(v);
        else w::parse_options(a == "-warn-error", v);
      } catch (const w::Bad& e) {
        std::cerr << "c++ocamlc: bad argument '" << v << "' to option '" << a << "': " << e.what() << '\n';
        return 2;
      }
    }
    else if (a == "-stop-after") {
      // `typing`: type-check with the ported checker, write nothing
      std::string pass = need_arg("-stop-after");
      if (pass == "parsing") g_stop_after = StopAfter::Parsing;
      else if (pass == "typing") g_stop_after = StopAfter::Typing;
      else if (pass == "lambda") g_stop_after = StopAfter::Lambda;
    }
    else if (a == "-open") g_open_modules.push_back(need_arg("-open"));
    else if (a == "-pp") g_preprocessor = need_arg("-pp");
    else if (a == "-for-pack") cppcaml::typing::clflags::for_package = std::string(need_arg("-for-pack"));
    else if (kArgIgnore.count(a)) {
      if (strict_flags) reject_ignored(a);
      (void)need_arg(a.c_str());
    }
    else if (kBoolIgnore.count(a) || kBoolIgnore2.count(a)) {
      if (strict_flags) reject_ignored(a);
      /* else accept; only the ported type checker reads these */
      set_typing_flag(a);
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
  g_nopervasives = nopervasives;
  g_incdirs = incdirs;
  g_stdlib_dir = stdlib_dir;
  g_nostdlib = nostdlib;
  cppcaml::typing::clflags::nopervasives = nopervasives;
  cppcaml::typing::clflags::no_std_include = nostdlib;
  cppcaml::cmi::cmiw::set_module_dirs(stdlib_dir, incdirs);

  if (runtime.empty()) {
    fs::path r = fs::absolute(fs::path(stdlib_dir)).parent_path() / "runtime" / "ocamlrun";
    if (fs::exists(r)) runtime = r.string();
  }

  // Compile every source input; collect the resulting (and pre-built) objects
  // for a possible link step.
  std::vector<std::string> link_objs;
  std::vector<std::string> objfiles;  // Compenv's objfiles, as named (-pack)
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
  // Compenv.output_prefix: under -c, -o gives the first unit's output prefix
  // (whatever its extension) and is then forgotten; else the source's own
  auto remove_extension = [](const std::string& n) {
    fs::path p(n);
    return p.has_extension() ? (p.parent_path() / p.stem()).string() : n;
  };
  bool out_used = false;
  auto output_prefix = [&](const std::string& f) {
    if (compile_only && !out_path.empty() && !out_used) {
      out_used = true;
      return remove_extension(out_path);
    }
    return remove_extension(f);
  };
  for (const std::string& f : inputs) {
    if (ends_with(f, ".mli")) {
      std::string cmi_out = output_prefix(f) + ".cmi";
      if (int rc = compile_mli(f, cmi_out)) return rc;
    } else if (ends_with(f, ".ml")) {
      std::string cmo_out = output_prefix(f) + ".cmo";
      if (int rc = compile_ml(f, cmo_out, stdlib_dir, prof)) return rc;
      link_objs.push_back(cmo_out);
      objfiles.push_back(cmo_out);
    } else if (ends_with(f, ".cmo") || ends_with(f, ".cma")) {
      link_objs.push_back(resolve_obj(f));  // a pre-compiled object/library to link
      objfiles.push_back(f);
    } else if (ends_with(f, ".cmi") && !pack_name.empty()) {
      objfiles.push_back(f);  // an interface-only member of a pack
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
  if (!pack_name.empty()) {  // -pack -o Pack.cmo : Bytepackager.package_files
    if (out_path.empty()) { std::cerr << "c++ocamlc: -pack needs -o <Pack>.cmo\n"; return 2; }
    namespace ty = cppcaml::typing;
    try {
      install_typing();
      init_path(stdlib_dir);
      ty::bytepackager::package_files(initial_env(), objfiles, out_path);
    } catch (const ty::bytepackager::Error& e) {
      std::cerr << "Error: " << ty::bytepackager::report_error(e) << '\n';
      return 2;
    } catch (...) {
      std::optional<ty::error_report::Report> r = ty::error_report::classify(std::current_exception());
      if (r) {
        std::cerr << "Error: " << r->name << '\n';
        return 2;
      }
      try {
        throw;
      } catch (const std::exception& e) {
        std::cerr << "c++ocamlc: -pack: " << e.what() << '\n';
      }
      return 2;
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
