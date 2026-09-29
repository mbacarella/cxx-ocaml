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
// The command line is ocamlc's: driver/main_args.ml's option table,
// stdlib Arg's parser and messages, Compenv's deferred actions and
// Maindriver's sequence (typing/{main_args,arg,compenv}).  An option whose
// effect c++ocamlc does not implement is refused, when that effect would
// take place, with "option -X is not supported yet" (cxx/PORTING.md,
// "Driver options").
#include <ucontext.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/parser.hpp"
#include "cppcaml/lexer.hpp"
#include "cppcaml/typing/closure.hpp"
#include "cppcaml/typing/cmmgen.hpp"
#include "cppcaml/typing/printcmm.hpp"
#include "cppcaml/typing/selection.hpp"
#include "cppcaml/typing/mach_passes.hpp"
#include "cppcaml/typing/linear.hpp"
#include "cppcaml/typing/emit.hpp"
#include "cppcaml/typing/asmgen.hpp"
#include "cppcaml/typing/asmlibrarian.hpp"
#include "cppcaml/typing/asmlink.hpp"
#include "cppcaml/typing/asmpackager.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pparse.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/error_report.hpp"
#include "cppcaml/typing/parsetree.hpp"
#include "cppcaml/typing/persistent_env.hpp"
#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typemod.hpp"
#include "cppcaml/typing/printlambda.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/out_type.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/bytegen.hpp"
#include "cppcaml/typing/bytelibrarian.hpp"
#include "cppcaml/typing/bytelink.hpp"
#include "cppcaml/typing/bytepackager.hpp"
#include "cppcaml/typing/emitcode.hpp"
#include "cppcaml/typing/cmt_format.hpp"
#include "cppcaml/typing/printinstr.hpp"
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/translmod.hpp"
#include "cppcaml/typing/warnings.hpp"
#include "cppcaml/typing/utf8_lexeme.hpp"
#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/compenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/main_args.hpp"

namespace fs = std::filesystem;

// Return the allocator's free memory to the system: the parser's tree and
// tokens, freed before typing, would otherwise stay resident for the whole
// compilation (the zones grow in blocks of their own).  mimalloc's
// mi_collect when it is linked in (weak: absent otherwise), else glibc's
// malloc_trim.
extern "C" void mi_collect(bool force) __attribute__((weak));
extern "C" int malloc_trim(size_t pad) __attribute__((weak));
static void release_free_memory() {
  if (mi_collect) mi_collect(true);
  else if (malloc_trim) malloc_trim(0);
}

// The same driver builds c++ocamlc (Maindriver, Compile) and, with
// CPPCAML_OCAMLOPT, c++ocamlopt (Optmaindriver, Optcompile).
#ifdef CPPCAML_OCAMLOPT
constexpr bool kNative = true;
#define CPPCAML_SELF "c++ocamlopt"
#else
constexpr bool kNative = false;
#define CPPCAML_SELF "c++ocamlc"
#endif
constexpr const char* kTool = kNative ? "ocamlopt" : "ocamlc";  // Compile_common's tool_name



// Unit_info.strict_modname_from_source: the stem, Utf8_lexeme.capitalize'd;
// an invalid encoding is Unit_info's Invalid_encoding error (exit 2)
static std::string module_name(const std::string& path) {
  namespace ty = cppcaml::typing;
  std::string base = fs::path(path).filename().string();
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  ty::utf8_lexeme::Result r = ty::utf8_lexeme::capitalize(base);
  if (!r.ok) {
    ty::location::print_report(ty::location::err_formatter(),
                               ty::location::errorf(ty::location::none(), "Invalid encoding of output name: %s.", base));
    ty::location::err_flush();
    std::exit(2);
  }
  return std::move(r.s);
}

// Unit_info.make's check_unit_name: Bad_module_name on the source file
static void check_unit_name(const std::string& source_file, const std::string& modname) {
  namespace ty = cppcaml::typing;
  if (!ty::oprint::is_valid_identifier(modname))
    ty::location::prerr_warning(ty::location::in_file(source_file),
                                ty::warnings::Warning::with_s(ty::warnings::Warning::K::Bad_module_name, modname));
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

// Filename.remove_extension (Unix)
static std::string remove_extension(const std::string& name) {
  std::size_t slash = name.rfind('/');
  std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return name;
  std::size_t base = slash == std::string::npos ? 0 : slash + 1;
  bool only_dots = true;  // a basename of leading dots only has no extension
  for (std::size_t k = base; k < dot; ++k) only_dots = only_dots && name[k] == '.';
  if (only_dots) return name;
  return name.substr(0, dot);
}

static bool ends_with(const std::string& s, const char* suf) {
  size_t n = std::string(suf).size();
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}



// Compile a single .ml -> .cmo (+ .cmi unless a hand-written .mli exists).
// Returns 0 on success.  `cmo_out` is where the .cmo is written.
// -d* debug dumps: ocamlc's, on its ppf_dump (stderr, or a file with
// -dump-into-file / -dump-dir), byte for byte.




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
  namespace cf = cppcaml::typing::clflags;
  if (cf::preprocessor) {
    char tmpl[] = "/tmp/ocamlppXXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
      std::cerr << CPPCAML_SELF ": cannot create a temporary file\n";
      return false;
    }
    ::close(fd);
    tmp = tmpl;
    std::string comm = *cf::preprocessor + " " + filename_quote(path) + " > " + tmp;
    // Ccomp.command: -verbose echoes the command line
    if (cf::verbose) {
      std::cout.flush();
      std::cerr << "+ " << comm << '\n';
      std::cerr.flush();
    }
    if (std::system(comm.c_str()) != 0) {
      std::remove(tmp.c_str());
      // Pparse.Error (CannotRun comm): Location.error_of_printer_file
      namespace ty = cppcaml::typing;
      ty::location::input_name = path;
      ty::location::input_source.reset();
      ty::location::print_report(
          ty::location::err_formatter(),
          ty::location::error_of_printer_file([&](ty::format_doc::Formatter& f) {
            ty::format_doc::fprintf(f, "Error while running external preprocessor@.Command line: %s@.", comm);
          }));
      ty::location::err_flush();
      return false;
    }
    input = tmp;
  }
  std::ifstream in(input, std::ios::binary);
  if (!in) {
    // Sys_error, reported by Location: "I/O error: <file>: <strerror>"
    int errnum = errno;
    if (!tmp.empty()) std::remove(tmp.c_str());
    namespace ty = cppcaml::typing;
    ty::location::input_name = path;
    ty::location::input_source.reset();
    std::string msg = input + ": " + std::strerror(errnum);
    ty::location::print_report(ty::location::err_formatter(),
                               ty::location::errorf(ty::location::in_file(path), "I/O error: %s", msg));
    ty::location::err_flush();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  if (!tmp.empty()) std::remove(tmp.c_str());  // remove_preprocessed
  return true;
}

// The typing port's forward references and module-init state (the idents
// its modules create at startup, in link order), once per process
static void install_typing() {
  static bool installed = false;
  if (!installed) {
    cppcaml::typing::typemod::install_forward_refs();
    cppcaml::typing::printtyp::install_hooks();
    installed = true;
  }
}

// Compmisc.init_path: [dir] (the cwd) unless -nocwd, the -I directories
// in command-line order (with OCAMLPARAM's first / last ones, and +threads
// under -thread), the standard library unless -nostdlib; the -H ones hidden
static void init_path() {
  namespace ty = cppcaml::typing;
  namespace cf = ty::clflags;
  namespace ce = ty::compenv;
  const std::string& stdlib = ty::config::standard_library;
  std::vector<std::string> visible = cf::include_dirs;
  if (cf::use_threads) visible.insert(visible.begin(), "+threads");
  // last_include_dirs @ visible @ first_include_dirs (flexdll_dirs is [])
  std::vector<std::string> l = ce::last_include_dirs;
  l.insert(l.end(), visible.begin(), visible.end());
  l.insert(l.end(), ce::first_include_dirs.begin(), ce::first_include_dirs.end());
  for (std::string& d : l) d = resolve_incdir(d, stdlib);  // Misc.expand_directory
  // (if no_cwd then [] else [dir]) @ List.rev_append visible std_include
  std::vector<std::string> path;
  if (!cf::no_cwd) path.push_back("");
  path.insert(path.end(), l.rbegin(), l.rend());
  // c++ocamlc finds its stdlib through -I when given one; that directory
  // is not added twice (Load_path would ignore the duplicate's units)
  bool listed = false;
  for (auto& d : l) {
    std::error_code ec;
    if (fs::equivalent(d, stdlib, ec)) listed = true;
  }
  if (!cf::no_std_include && !listed) path.push_back(stdlib);
  std::vector<std::string> hidden;
  for (auto it = cf::hidden_include_dirs.rbegin(); it != cf::hidden_include_dirs.rend(); ++it)
    hidden.push_back(resolve_incdir(*it, stdlib));
  ty::load_path::init(path, hidden);
  ty::env::reset_cache();
}

// Compmisc.initial_env
static cppcaml::typing::env::t initial_env() {
  namespace ty = cppcaml::typing;
  ty::ident::reinit();
  ty::uid::reinit();
  ty::types::reset();
  ty::ctype::reset();
  ty::parsetree::reset_types_attributes();
  ty::out_type::reset_short_paths_cache();
  ty::Location cmdline = ty::location::none();
  cmdline.loc_start.pos_fname = cmdline.loc_end.pos_fname = "command line";
  // ~open_implicit_modules:(List.rev !Clflags.open_modules)
  std::vector<std::string> opens(ty::clflags::open_modules.rbegin(), ty::clflags::open_modules.rend());
  return ty::typemod::initial_env(
      cmdline, ty::clflags::nopervasives ? std::nullopt : std::optional<std::string>("Stdlib"), opens);
}

// The type checker (the typing/ port, cxx/PORTING.md): Compmisc.init_path +
// initial_env, then Typemod.type_implementation / type_interface, before
// code generation as in ocamlc.  A type error is reported as ocamlc's
// location line and the error's constructor until Printtyp is ported.  An
// internal failure is reported as one (CPPCAML_TYPECHECK_DEBUG adds the
// error's details).
using PortBody = std::function<void(cppcaml::typing::env::t, cppcaml::typing::typemod::UnitInfo&)>;
// Typed: the port typed the unit (and, for an .ml without .mli, wrote its
// .cmi); Failed: an internal failure (reported); Rejected: a type error was
// reported.
enum class PortResult { Typed, Failed, Rejected };

// Misc.Fatal_error: fatal_errorf has printed ">> Fatal error: ..."; nothing
// reports the exception, so OCaml's uncaught exception handler prints it
// (exit 2).  True when [ep] is one.
static bool uncaught_fatal_error(std::exception_ptr ep) {
  try {
    std::rethrow_exception(ep);
  } catch (const cppcaml::typing::misc::FatalError&) {
    std::cout.flush();
    std::cerr << "Fatal error: exception Misc.Fatal_error\n";
    std::cerr.flush();
    return true;
  } catch (...) {
  }
  return false;
}
static PortResult port_typecheck(const std::string& in_path, const std::string& mod, const std::string& out, bool intf,
                                 const PortBody& body) {
  namespace ty = cppcaml::typing;
  install_typing();
  const bool debug = std::getenv("CPPCAML_TYPECHECK_DEBUG") != nullptr;
  try {
    init_path();
    // Compile_common: Env.set_current_unit; Compmisc.initial_env
    ty::env::set_current_unit(ty::UnitInfo{mod, intf ? ty::Uid::From::Intf : ty::Uid::From::Impl});
    ty::env::t env0 = initial_env();
    ty::typemod::UnitInfo target;
    target.source_file = in_path;
    target.modname = mod;
    target.prefix = fs::path(out).replace_extension("").string();
    // Unit_info.mli_from_source: the source's prefix and -intf-suffix
    target.cmi_file = target.prefix + ".cmi";
    body(env0, target);
    return PortResult::Typed;
  } catch (const std::bad_function_call&) {
    std::cerr << CPPCAML_SELF ": " << in_path << ": internal error: an unported part of typing/\n";
    return PortResult::Failed;
  } catch (...) {
    // Location.report_exception Format.err_formatter exn (Maindriver: exit 2)
    ty::reporters::install();
    std::exception_ptr ep = std::current_exception();
    if (uncaught_fatal_error(ep)) return PortResult::Failed;
    bool reported = false;
    try {
      reported = ty::location::report_exception(ty::location::err_formatter(), ep);
    } catch (...) {
      ep = std::current_exception();
    }
    ty::location::err_flush();
    if (reported) return PortResult::Rejected;
    std::optional<ty::error_report::Report> r = ty::error_report::classify(ep);
    if (!r) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        std::cerr << CPPCAML_SELF ": " << in_path << ": internal error in the type checker: " << e.what() << '\n';
      } catch (...) {
        std::cerr << CPPCAML_SELF ": " << in_path << ": internal error in the type checker\n";
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

// The location of a byte span of the source (Location.curr lexbuf; the
// `# N "file"` directives are not taken into account here)
static cppcaml::typing::Location span_loc(const std::string& path, const std::string& src, size_t a, size_t b) {
  namespace ty = cppcaml::typing;
  auto pos = [&](size_t off) {
    long lnum = 1, bol = 0;
    for (size_t i = 0; i < off && i < src.size(); ++i)
      if (src[i] == '\n') {
        ++lnum;
        bol = static_cast<long>(i + 1);
      }
    return ty::mkpos(ty::zborrow(path), lnum, bol, static_cast<long>(off));
  };
  return ty::Location{pos(a), pos(b), false};
}

static void emit_report(const cppcaml::typing::location::Report& r) {
  namespace ty = cppcaml::typing;
  ty::location::print_report(ty::location::err_formatter(), r);
  ty::location::err_flush();
}

// Syntaxerr.prepare_error (parse.ml)
// The lexer's warnings (lexer.mll prints them as the parser pulls tokens):
// those before [limit], in source order
static void emit_lex_warnings(const std::string& path, const std::string& src, size_t limit) {
  namespace ty = cppcaml::typing;
  using WK = ty::warnings::Warning::K;
  for (const cppcaml::LexWarning& w : cppcaml::lex_warnings()) {
    if (w.start >= limit) continue;
    WK k = w.number == 1 ? WK::Comment_start : w.number == 2 ? WK::Comment_not_end
         : w.number == 50 ? WK::Unexpected_docstring : WK::Illegal_backslash;
    ty::warnings::Warning wn = ty::warnings::Warning::make(k);
    if (w.number == 50) wn.b = w.flag;
    ty::location::prerr_warning(span_loc(path, src, w.start, w.end), wn);
  }
  cppcaml::lex_warnings().clear();
}

static void report_syntax_error(const std::string& path, const std::string& src, const cppcaml::ParseError& e) {
  namespace ty = cppcaml::typing;
  namespace fd = ty::format_doc;
  using ty::misc::style::code_str;
  ty::Location loc = span_loc(path, src, e.pos, e.end);
  switch (e.kind) {
    case cppcaml::ParseError::Kind::Unclosed: {
      ty::Location oloc = span_loc(path, src, e.open_pos, e.open_end);
      emit_report(ty::location::errorf_sub(
          loc, {ty::location::msg(oloc, "This %a might be unmatched", code_str(e.opening))},
          "Syntax error: %a expected", code_str(e.what_)));
      return;
    }
    case cppcaml::ParseError::Kind::Expecting:
      emit_report(ty::location::errorf(loc, "Syntax error: %a expected.", code_str(e.what_)));
      return;
    case cppcaml::ParseError::Kind::Not_expecting:
      emit_report(ty::location::errorf(loc, "Syntax error: %a not expected.", code_str(e.what_)));
      return;
    case cppcaml::ParseError::Kind::Other: emit_report(ty::location::errorf(loc, "Syntax error")); return;
  }
}

// Lexer.prepare_error (lexer.mll)
static void report_lexer_error(const std::string& path, const std::string& src, const cppcaml::LexError& e) {
  namespace ty = cppcaml::typing;
  namespace fd = ty::format_doc;
  using ty::misc::style::code_str;
  using K = cppcaml::LexError::Kind;
  ty::Location loc = span_loc(path, src, e.pos, e.end);
  std::string expl = e.expl ? *e.expl : std::string();
  bool has_expl = e.expl.has_value();
  switch (e.kind) {
    case K::Illegal_character:
      emit_report(ty::location::errorf(loc, "Illegal character (%s)", ty::format::char_escaped(e.arg[0])));
      return;
    case K::Illegal_escape:
      emit_report(ty::location::errorf(loc, "Illegal backslash escape in string or character (%s)%t", e.arg,
                                       [&](fd::Formatter& f) {
                                         if (has_expl) fd::fprintf(f, ": %s", expl);
                                       }));
      return;
    case K::Reserved_sequence:
      emit_report(ty::location::errorf(loc, "Reserved character sequence: %s%t", e.arg, [&](fd::Formatter& f) {
        if (has_expl) fd::fprintf(f, " %s", expl);
      }));
      return;
    case K::Unterminated_comment: emit_report(ty::location::errorf(loc, "Comment not terminated")); return;
    case K::Unterminated_string: emit_report(ty::location::errorf(loc, "String literal not terminated")); return;
    case K::Unterminated_string_in_comment:
      emit_report(ty::location::errorf_sub(
          loc, {ty::location::msg(span_loc(path, src, e.pos2, e.pos2 + 1), "String literal begins here")},
          "This comment contains an unterminated string literal"));
      return;
    case K::Empty_character_literal:
      emit_report(ty::location::error(
          loc, "Illegal empty character literal ''",
          {ty::location::msg_noloc("@{<hint>Hint@}: Did you mean %a or a type variable %a?", code_str("' '"),
                                   code_str("'a"))}));
      return;
    case K::Invalid_literal: emit_report(ty::location::errorf(loc, "Invalid literal %s", e.arg)); return;
    case K::Invalid_directive:
      emit_report(ty::location::errorf(loc, "Invalid lexer directive %S%t", e.arg, [&](fd::Formatter& f) {
        if (has_expl) fd::fprintf(f, ": %s", expl);
      }));
      return;
    case K::Invalid_encoding:
      emit_report(ty::location::errorf(loc, "Invalid encoding of identifier %s.", e.arg));
      return;
    case K::Invalid_char_in_ident: {
      char b[32];
      std::snprintf(b, sizeof b, "%04lX", std::stol(e.arg));
      emit_report(ty::location::errorf(loc, "Invalid character U+%s in identifier", std::string(b)));
      return;
    }
    case K::Non_lowercase_delimiter:
      emit_report(ty::location::errorf(
          loc, "%a cannot be used as a quoted string delimiter,@ it must contain only lowercase letters.",
          code_str(e.arg)));
      return;
    case K::Capitalized_raw_identifier:
      emit_report(ty::location::errorf(
          loc, "%a cannot be used as a raw identifier, it must start with a lowercase letter", code_str(e.arg)));
      return;
    case K::Unknown_keyword:
      emit_report(ty::location::errorf(
          loc, "%a has been defined as an additional keyword.@ This version of OCaml does not support this keyword.",
          code_str(e.arg)));
      return;
    case K::Other: break;
  }
  std::cerr << CPPCAML_SELF ": " << path << ": " << e.what() << '\n';
}

// Compmisc.with_ppf_dump ~file_prefix: where the -d* dumps go -- stderr,
// or <file_prefix>.dump under -dump-into-file, or a fresh file in -dump-dir
struct PpfDump {
  std::ofstream file;
  bool to_file = false;
  std::ostream& out() { return to_file ? static_cast<std::ostream&>(file) : std::cerr; }
};
static void open_ppf_dump(PpfDump& d, const std::string& file_prefix) {
  namespace cf = cppcaml::typing::clflags;
  if (cf::dump_dir) {
    std::error_code ec;
    fs::create_directories(fs::path(*cf::dump_dir + "/" + file_prefix).parent_path(), ec);
    std::string tmpl = *cf::dump_dir + "/" + file_prefix + ".XXXXXX.dump";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), 5);
    if (fd >= 0) {
      ::close(fd);
      d.file.open(buf.data(), std::ios::binary | std::ios::trunc);
      d.to_file = true;
    }
  } else if (cf::dump_into_file) {
    d.file.open(file_prefix + ".dump", std::ios::binary | std::ios::trunc);
    d.to_file = true;
  }
}

// the options whose effect the compilers here lack, when a unit is compiled
static bool refuse_unsupported_compile_option() {
  std::string o = cppcaml::typing::main_args::unsupported_compile_option();
  if (o.empty()) return false;
  std::cout.flush();
  std::cerr << CPPCAML_SELF ": option " << o << " is not supported yet\n";
  return true;
}

// Parse.wrap: Lexer.init ~keyword_edition (Clflags.parse_keyword_edition
// of -keywords, whose Arg.Bad reaches Maindriver)
static void init_lexer() {
  namespace cf = cppcaml::typing::clflags;
  if (!cf::keyword_edition) {
    cppcaml::set_keyword_edition(std::nullopt, {});
    return;
  }
  auto bad_version = [] {
    throw cppcaml::typing::arg::Bad(
        "Ill-formed version in keywords flag,\nthe supported format is <major>.<minor>, for example 5.2 .");
  };
  auto parse_version = [&](const std::string& v) -> std::optional<std::pair<int, int>> {
    if (v.empty()) return std::nullopt;
    std::size_t dot = v.find('.');
    if (dot == std::string::npos || v.find('.', dot + 1) != std::string::npos) bad_version();
    long major, minor;
    if (!cppcaml::typing::arg::int_of_string_opt(v.substr(0, dot), major) ||
        !cppcaml::typing::arg::int_of_string_opt(v.substr(dot + 1), minor))
      bad_version();
    return std::pair<int, int>{static_cast<int>(major), static_cast<int>(minor)};
  };
  // String.split_on_char '+'
  std::vector<std::string> parts;
  std::string cur;
  for (char c : *cf::keyword_edition) {
    if (c == '+') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  parts.push_back(cur);
  std::optional<std::pair<int, int>> version = parse_version(parts[0]);
  cppcaml::set_keyword_edition(version, std::vector<std::string>(parts.begin() + 1, parts.end()));
}

static int compile_ml_(const std::string& in_path, const std::string& cmo_out, bool prof);
static int compile_ml(const std::string& in_path, const std::string& cmo_out, bool prof) {
  if (refuse_unsupported_compile_option()) return 2;
  check_unit_name(in_path, module_name(cmo_out));
  return compile_ml_(in_path, cmo_out, prof);
}
static int compile_ml_(const std::string& in_path, const std::string& cmo_out, bool prof) {
  namespace cf = cppcaml::typing::clflags;
  PpfDump ppf_dump;
  open_ppf_dump(ppf_dump, remove_extension(cmo_out) + (kNative ? ".cmx" : ".cmo"));
  namespace ty = cppcaml::typing;
  std::string src;  // Pparse.parse_file: the (preprocessed) source text
  if (!read_source(in_path, src)) return 2;
  // Pparse: Location.input_name; a source's lexbuf holds the whole source
  // (a binary AST's, the file it names: Pparse.read_ast_structure)
  cppcaml::typing::location::input_name = in_path;
  bool ast_file = false;
  try {
    ast_file = cppcaml::typing::pparse::is_ast_file(src, cppcaml::typing::pparse::AstKind::Structure);
  } catch (...) {
    if (uncaught_fatal_error(std::current_exception())) return 2;
    throw;
  }
  if (!ast_file) cppcaml::typing::location::input_source = src;
  // Unit_info.modname: from the output prefix (-o stdlib__Arg.cmo -> Stdlib__Arg)
  std::string mod = module_name(cmo_out);
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  auto lap = [&](const char* what, clk::time_point& prev) {
    auto now = clk::now();
    if (prof) {
      std::cerr << "  " << what << ": "
                << std::chrono::duration<double, std::milli>(now - prev).count() << " ms\n";
      // resident set and zone storage after the phase (not an "ms" line:
      // bench.sh's phase sums skip it)
      long pages = 0, resident = 0;
      if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
      }
      std::cerr << "  mem after " << what << ": rss " << resident * ::sysconf(_SC_PAGESIZE) / (1 << 20)
                << " MB, zones " << cppcaml::typing::Zone::block_bytes() / (1 << 20) << " MB\n";
      // the phase's end on CLOCK_MONOTONIC (steady_clock's), to cut a
      // `perf record -k CLOCK_MONOTONIC` profile into phases
      char at[32];
      std::snprintf(at, sizeof at, "%.6f",
                    std::chrono::duration<double>(now.time_since_epoch()).count());
      std::cerr << "  end of " << what << " at " << at << " s\n";
    }
    prev = now;
  };
  // Compile_common.implementation's end: Builtin_attributes.warn_unused ();
  // Warnings.check_fatal () (Warnings.Errors: Already_displayed_error, the
  // .cmo removed, exit 2)
  auto finish = [&]() -> int {
    ty::builtin_attributes::warn_unused();
    try {
      ty::warnings::check_fatal();
    } catch (const ty::location::AlreadyDisplayed&) {
      std::remove(cmo_out.c_str());
      return 2;
    }
    return 0;
  };
  try {
    auto tp = t0;
    std::vector<std::string> dirfiles;
    // Pparse.file_aux: a source is parsed (a binary AST is read in the
    // typing body, after Compmisc.initial_env as in ocamlc)
    cppcaml::ast::Structure structure;
    if (!ast_file) {
      init_lexer();
      structure = cppcaml::parse_structure(src, dirfiles);
      emit_lex_warnings(in_path, src, static_cast<size_t>(-1));
    }
    lap("parse", tp);
    // -dparsetree prints the parser's tree; a binary AST's or a rewritten
    // one has no printer yet (Printast on the typing port's parsetree)
    const bool rewritten = ast_file || !cf::all_ppx.empty();
    if (cf::dump_parsetree && rewritten) {
      std::cout.flush();
      std::cerr << CPPCAML_SELF ": option -dparsetree is not supported yet with -ppx or a binary AST input\n";
      return 2;
    }
    if (cf::dump_parsetree) cppcaml::ast::print_dparsetree(structure, in_path, ppf_dump.out(), dirfiles);
    if (cf::should_stop_after(cf::Pass::Parsing) && !rewritten) return finish();
    // Compile_common.implementation: typecheck_impl
    std::optional<ty::typedtree::Implementation> impl;
    bool stopped = false;
    PortResult port = port_typecheck(in_path, mod, cmo_out, /*intf=*/false,
                                     [&](ty::env::t env0, ty::typemod::UnitInfo& target) {
                                       // the source's name: one string object (cmt_format.hpp)
                                       std::string_view src_name = ty::zborrow(in_path);
                                       ty::cmt_format::set_source_name(src_name);
                                       ty::parsetree::Structure st;
                                       if (ast_file) {
                                         st = ty::pparse::read_ast_structure(src);
                                         ty::cmt_format::set_comments({});  // no lexing: Lexer.comments () = []
                                       } else {
                                         // with -pp the lexer's positions name the file through a
                                         // string of their own (Pparse's preprocessed input)
                                         std::string_view pos_name = cf::preprocessor ? ty::zstr(in_path) : src_name;
                                         st = ty::parsetree::of_ast(structure, pos_name, dirfiles);
                                         // the C++ parser's tree is not read again (Parsetree's is a
                                         // copy in the zone): free it before typing
                                         decltype(structure)().swap(structure);
                                         release_free_memory();
                                         ty::cmt_format::set_comments(ty::parsetree::comments_of_ast(
                                             cppcaml::ast::last_comments(), pos_name, dirfiles));
                                       }
                                       st = ty::pparse::apply_rewriters_str(st, kTool);
                                       // Compile_common.Parse_result.update_unit_info
                                       target.human_source_file = ty::location::input_name;
                                       if (cf::should_stop_after(cf::Pass::Parsing)) {
                                         stopped = true;
                                         return;
                                       }
                                       // an .ml without .mli: its .cmi is written here (Typemod)
                                       impl = ty::typemod::type_implementation(target, env0, st);
                                     });
    if (port != PortResult::Typed) return 2;
    if (stopped) return finish();
    lap("typecheck", tp);
    // Clflags.should_stop_after Typing (-i prints the signature, writes nothing)
    if (cf::should_stop_after(cf::Pass::Typing)) return finish();
    // Compile.to_bytecode: Translmod.transl_implementation, -drawlambda,
    // Simplif.simplify_lambda, -dlambda, Bytegen.compile_implementation,
    // -dinstr (the dumps on stderr, as ocamlc's ppf_dump)
    ty::translmod::install_forward_refs();
    // Unit_info.modname: one string, which Translmod's module ident,
    // Bytegen's events and Emitcode's cu_name all share
    std::string_view modname = ty::zborrow(mod);
    if (kNative) {
      // Optcompile.clambda: Clflags.use_inlining_arguments_set
      // classic_arguments; Translmod.transl_store_implementation,
      // -drawlambda, Simplif.simplify_lambda, -dlambda; the back end unless
      // -stop-after lambda; Compilenv.save_unit_info (the .cmx)
      // Unit_info.modname: the one string (the module ident's name, the
      // unit infos' ui_name, __MODULE__)
      std::string_view umod = ty::uid::unit_name_string(modname);
      ty::compilenv::reset(cf::for_package, umod);
      cf::use_inlining_arguments_set(cf::classic_arguments);
      // Lambda's nodes in a zone of their own, dropped once Closure has
      // turned them into Clambda
      static ty::Zone lambda_nodes;
      ty::lambda::set_node_zone(&lambda_nodes);
      ty::lambda::Program prog = ty::translmod::transl_store_implementation(umod, impl->structure, impl->coercion);
      if (cf::dump_rawlambda) ppf_dump.out() << ty::printlambda::dump(prog.code);
      ty::lambda::lambda lam = ty::simplif::simplify_lambda(prog.code);
      if (cf::dump_lambda) ppf_dump.out() << ty::printlambda::dump(lam);
      ppf_dump.out().flush();
      lap("lambda", tp);
      if (!cf::should_stop_after(cf::Pass::Lambda)) {
        // Asmgen.compile_implementation: Compilenv.require_global on the
        // required globals, the middle end (Closure_middle_end)
        for (ty::Ident::t id : prog.required_globals) ty::compilenv::require_global(id);
        ty::format::Formatter dump;
        ty::closure_middle_end::WithConstants clambda = ty::closure_middle_end::lambda_to_clambda(dump, prog, lam);
        ty::lambda::set_node_zone(nullptr);
        lambda_nodes.clear();
        lap("clambda", tp);
        // Asmgen.compile_unit: end_gen_implementation (Cmmgen.compunit,
        // compile_phrases with the -d dumps, then the references to the
        // external primitives' symbols) into the assembly file (kept with
        // -S, else a temporary file), assembled into the object file
        std::string prefix = remove_extension(cmo_out);
        try {
          ty::asmgen::compile_unit(ty::asmgen::asm_filename(prefix), cf::keep_asm_file, prefix + ".o",
                                   [&] { return ty::asmgen::end_gen_implementation(dump, clambda); });
        } catch (const ty::polling::PollError& e) {
          // Location.error_of_printer_file Polling.report_error
          ppf_dump.out() << dump.contents();
          ppf_dump.out().flush();
          ty::location::print_report(ty::location::err_formatter(),
                                     ty::location::error_of_printer_file(
                                         [&](ty::format_doc::Formatter& f) { ty::polling::report_error(f, e); }));
          ty::location::err_flush();
          std::remove(cmo_out.c_str());
          return 2;
        } catch (const ty::asmgen::Error& e) {
          ppf_dump.out() << dump.contents();
          ppf_dump.out().flush();
          std::cerr << "Error: Assembler error, input left in file " << e.file << "\n";
          return 2;
        }
        ppf_dump.out() << dump.contents();
        ppf_dump.out().flush();
        lap("emit", tp);
        // Compilenv.save_unit_info (the .cmx)
        {
          ty::cmx_format::UnitInfos& cu = ty::compilenv::current_unit();
          cu.ui_imports_cmi.clear();
          // the current unit's entry holds its modname string (ui_name's) --
          // unless -cmi-file named the interface (Unit_info.Artifact.
          // from_filename makes a fresh modname, which Env.read_signature
          // added), as Emitcode's
          for (auto& [name, crc] : ty::env::imports())
          {
            std::string_view n =
                name == cu.ui_name && !cf::cmi_file ? cu.ui_name : ty::env::import_name(name);
            cu.ui_imports_cmi.push_back({n.data() ? n : ty::zstr(name), crc});
          }
          std::string bytes = ty::cmx_format::write_unit_info(cu);
          std::ofstream os(cmo_out, std::ios::binary);
          os << bytes;
        }
        return finish();
      }
      return finish();
    }
    // Lambda's nodes in a zone of their own, dropped once Bytegen has turned
    // them into instructions
    static ty::Zone lambda_nodes;
    ty::lambda::set_node_zone(&lambda_nodes);
    ty::lambda::Program prog = ty::translmod::transl_implementation(modname, impl->structure, impl->coercion);
    if (cf::dump_rawlambda) ppf_dump.out() << ty::printlambda::dump(prog.code);
    ty::lambda::lambda lam = ty::simplif::simplify_lambda(prog.code);
    if (cf::dump_lambda) ppf_dump.out() << ty::printlambda::dump(lam);
    lap("lambda", tp);
    ty::instruct::code bytecode = ty::bytegen::compile_implementation(modname, lam);
    ty::lambda::set_node_zone(nullptr);
    lambda_nodes.clear();
    if (cf::dump_instr) ppf_dump.out() << ty::printinstr::dump(bytecode);
    ppf_dump.out().flush();
    lap("bytegen", tp);
    // Compile.emit_bytecode: Emitcode.to_file, the .cmo removed on failure
    std::FILE* oc = std::fopen(cmo_out.c_str(), "wb");
    if (!oc) {
      std::cerr << CPPCAML_SELF ": cannot open " << cmo_out << "\n";
      return 2;
    }
    try {
      ty::emitcode::to_file(oc, cmo_out, modname, prog.required_globals, bytecode);
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
    return finish();
  } catch (const cppcaml::typing::arg::Bad&) {
    throw;  // (Arg.Bad escapes to Maindriver: -keywords' version)
  } catch (const cppcaml::ParseError& e) {
    emit_lex_warnings(in_path, src, e.pos);
    report_syntax_error(in_path, src, e);
    return 2;
  } catch (const cppcaml::LexError& e) {
    emit_lex_warnings(in_path, src, e.pos);
    report_lexer_error(in_path, src, e);
    return 2;
  } catch (...) {
    // Location.report_exception (Maindriver: exit 2): the translators' errors
    namespace ty = cppcaml::typing;
    ty::reporters::install();
    std::exception_ptr ep = std::current_exception();
    if (uncaught_fatal_error(ep)) return 2;
    bool reported = false;
    try {
      reported = ty::location::report_exception(ty::location::err_formatter(), ep);
    } catch (...) {
      ep = std::current_exception();
    }
    ty::location::err_flush();
    if (reported) return 2;
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::cerr << CPPCAML_SELF ": " << in_path << ": " << e.what() << '\n';
    } catch (...) {
      std::cerr << CPPCAML_SELF ": " << in_path << ": internal error\n";
    }
    return 1;
  }
  return 0;
}

// Compile a .mli -> .cmi.
static int compile_mli(const std::string& in_path, const std::string& cmi_out) {
  namespace cf = cppcaml::typing::clflags;
  if (refuse_unsupported_compile_option()) return 2;
  check_unit_name(in_path, module_name(cmi_out));
  std::string src;  // Pparse.parse_file: the (preprocessed) source text
  if (!read_source(in_path, src)) return 2;
  // Pparse: Location.input_name; a source's lexbuf holds the whole source
  // (a binary AST's, the file it names: Pparse.read_ast_signature)
  cppcaml::typing::location::input_name = in_path;
  bool ast_file = false;
  try {
    ast_file = cppcaml::typing::pparse::is_ast_file(src, cppcaml::typing::pparse::AstKind::Signature);
  } catch (...) {
    if (uncaught_fatal_error(std::current_exception())) return 2;
    throw;
  }
  if (!ast_file) cppcaml::typing::location::input_source = src;
  try {
    std::vector<std::string> dirfiles;  // the `# N "file"` directives' names
    cppcaml::ast::Signature sig;
    if (!ast_file) {
      init_lexer();
      sig = cppcaml::parse_signature(src, dirfiles);
      emit_lex_warnings(in_path, src, static_cast<size_t>(-1));
    }
    const bool rewritten = ast_file || !cf::all_ppx.empty();
    if (cf::dump_parsetree && rewritten) {
      std::cout.flush();
      std::cerr << CPPCAML_SELF ": option -dparsetree is not supported yet with -ppx or a binary AST input\n";
      return 2;
    }
    // Compile.interface: with_info ~dump_ext:"cmi" (Compmisc.with_ppf_dump)
    PpfDump ppf_dump;
    open_ppf_dump(ppf_dump, remove_extension(cmi_out) + ".cmi");
    if (cf::dump_parsetree) cppcaml::ast::print_dparsetree(sig, in_path, ppf_dump.out(), dirfiles);
    if (cf::should_stop_after(cf::Pass::Parsing) && !rewritten) return 0;
    PortResult port = port_typecheck(
        in_path, module_name(cmi_out), cmi_out, /*intf=*/true,
        [&](cppcaml::typing::env::t env0, cppcaml::typing::typemod::UnitInfo& target) {
          namespace ty = cppcaml::typing;
          std::string_view src_name = ty::zborrow(in_path);  // the source's name: one string object
          ty::cmt_format::set_source_name(src_name);
          ty::parsetree::Signature sg;
          if (ast_file) {
            sg = ty::pparse::read_ast_signature(src);
            ty::cmt_format::set_comments({});  // no lexing: Lexer.comments () = []
          } else {
            std::string_view pos_name = cf::preprocessor ? ty::zstr(in_path) : src_name;  // as compile_ml's
            sg = ty::parsetree::of_ast_signature(sig, pos_name, dirfiles);
            decltype(sig)().swap(sig);  // (not read again: see the implementation's)
            release_free_memory();
            ty::cmt_format::set_comments(
                ty::parsetree::comments_of_ast(cppcaml::ast::last_comments(), pos_name, dirfiles));
          }
          sg = ty::pparse::apply_rewriters_sig(sg, kTool);
          target.human_source_file = ty::location::input_name;  // update_unit_info
          if (cf::should_stop_after(cf::Pass::Parsing)) return;
          // Compile_common.typecheck_intf
          const ty::typedtree::Signature* tsg = ty::typemod::type_interface(target, env0, sg);
          ty::StrMap<std::string_view> alerts = ty::builtin_attributes::alerts_of_sig(true, sg);
          if (ty::clflags::print_types) {
            ty::printtyp::wrap_printing_env(false, env0, [&] {
              ty::format_doc::Formatter d;
              ty::printtyp::printed_signature(target.human(), d, tsg->sig_type);
              ty::format::Formatter out;
              ty::format_doc::format(out, d.doc);
              out.print_newline();
              std::fwrite(out.contents().data(), 1, out.contents().size(), stdout);
              std::fflush(stdout);
            });
          }
          (void)ty::includemod::signatures(env0, true, tsg->sig_type, tsg->sig_type);
          ty::typecore::force_delayed_checks();
          ty::builtin_attributes::warn_unused();
          ty::warnings::check_fatal();
          // Compile_common.interface: the .cmi unless -i
          if (ty::clflags::print_types) return;
          // Compile_common.emit_signature: the .cmi, then Typemod.save_signature's .cmti
          ty::cmi_format::CmiInfos cmi =
              ty::env::save_signature(alerts, tsg->sig_type, target.modname, target.prefix + ".cmi");
          ty::cmt_format::BinaryAnnots annots{ty::cmt_format::BinaryAnnots::Kind::Interface};
          annots.signature = tsg;
          ty::cmt_format::save_cmt(target.prefix + ".cmti", target.modname, in_path, annots, env0, &cmi, nullptr);
        });
    if (port != PortResult::Typed) return 2;
  } catch (const cppcaml::typing::arg::Bad&) {
    throw;  // (Arg.Bad escapes to Maindriver: -keywords' version)
  } catch (const cppcaml::ParseError& e) {
    emit_lex_warnings(in_path, src, e.pos);
    report_syntax_error(in_path, src, e);
    return 2;
  } catch (const cppcaml::LexError& e) {
    emit_lex_warnings(in_path, src, e.pos);
    report_lexer_error(in_path, src, e);
    return 2;
  } catch (const std::exception& e) {
    std::cerr << CPPCAML_SELF ": " << in_path << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}

// The default standard library: next to the executable (<repo>/cxx/build/
// c++ocamlc -> <repo>/stdlib), as c++ocamlc is not installed
// Config.standard_library without $OCAMLLIB / $CAMLLIB: the configured
// default, or -- c++ocamlc running from its build tree, the configured
// directory holding no stdlib -- the tree's stdlib next to the executable
static std::string default_standard_library(const std::string& configured) {
  std::error_code ec;
  if (fs::exists(fs::path(configured) / "stdlib.cmi", ec)) return configured;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path cand = exe.parent_path().parent_path().parent_path() / "stdlib";
    if (fs::exists(cand / "stdlib.cmi")) return cand.string();
  }
  return configured;
}

// Location.report_exception on Format.err_formatter; false when no reporter
// knows the exception
static bool report_exception(std::exception_ptr ep) {
  namespace ty = cppcaml::typing;
  ty::reporters::install();
  bool reported = false;
  try {
    reported = ty::location::report_exception(ty::location::err_formatter(), ep);
  } catch (...) {
  }
  ty::location::err_flush();
  return reported;
}

// Bytelink.link objfiles output_name (its errors reach Location's report)
static int link(const std::vector<std::string>& objfiles, const std::string& output_name) {
  cppcaml::typing::bytelink::link(objfiles, output_name);
  return 0;
}

// Maindriver.main's last step: Compmisc.with_ppf_dump ~file_prefix:"profile"
// (fun ppf -> Profile.print ppf !profile_columns) -- nothing to print (no
// -dtimings / -dprofile), but -dump-into-file / -dump-dir make the file
static void profile_dump() {
  PpfDump d;
  open_ppf_dump(d, "profile");
}

// Maindriver.main
static int run_main(int argc, char** argv) {
  cppcaml::typing::cmt_format::set_argv(std::vector<std::string>(argv, argv + argc));  // Sys.argv (cmt_args)
  namespace ty = cppcaml::typing;
  namespace cf = ty::clflags;
  namespace ce = ty::compenv;
  const std::string program = kTool;
  // Optmaindriver.main: native_code := true, before the arguments
  if (kNative) cf::native_code = true;
  if (kNative) {
    // the native back end ported: non-flambda amd64 on Linux, as this
    // configuration's Config says (the code generator reads these values
    // as constants)
    static const std::pair<const char*, const char*> supported[] = {
        {"architecture", "amd64"}, {"system", "linux"},       {"flambda", "false"},
        {"with_frame_pointers", "false"}, {"asm_cfi_supported", "true"}, {"tsan", "false"}};
    for (auto& [k, v] : supported) {
      std::optional<std::string> x = ty::config::config_var(k);
      if (!x || *x != v) {
        std::cerr << CPPCAML_SELF ": this OCaml's configuration is not supported (" << k << ": "
                  << (x ? *x : "?") << ", only " << v << ")\n";
        return 2;
      }
    }
  }
  const bool prof = std::getenv("CPPCAML_PROFILE") != nullptr;
  std::vector<std::string> args(argv, argv + argc);
  if (kNative) {
    // Arch.command_line_options (amd64's) @ Options.list
    ty::arg::Option fpic{"-fPIC", ty::arg::Spec{}, " Generate position-independent machine code (default)"};
    fpic.spec.k = ty::arg::Spec::K::Unit;
    fpic.spec.unit = [] { cf::pic_code = true; };
    ty::arg::Option fnopic{"-fno-PIC", ty::arg::Spec{}, " Generate position-dependent machine code"};
    fnopic.spec.k = ty::arg::Spec::K::Unit;
    fnopic.spec.unit = [] { cf::pic_code = false; };
    ce::add_arguments({fpic, fnopic});
    ce::add_arguments(ty::main_args::optcomp_options());
  } else {
    ce::add_arguments(ty::main_args::bytecomp_options());
  }
  {
    ty::arg::Option depend;
    depend.key = "-depend";
    depend.spec.k = ty::arg::Spec::K::Unit;
    depend.spec.unit = [] { ce::fatal(CPPCAML_SELF ": option -depend is not supported yet"); };
    depend.doc = kNative ? "<options> Compute dependencies (use 'ocamlopt -depend -help' for details)"
                         : "<options> Compute dependencies (use 'ocamlc -depend -help' for details)";
    ce::add_arguments({depend});
  }
  ce::add_arguments(ty::main_args::cppcaml_extensions());
  {
    // -cxx-version (undocumented: not in -help): identifies the C++ port --
    // the stock compilers reject it as an unknown option (exit 2)
    ty::arg::Option cxxv;
    cxxv.key = "-cxx-version";
    cxxv.spec.k = ty::arg::Spec::K::Unit;
    cxxv.spec.unit = [] {
      std::cout << CPPCAML_SELF " (the C++ port of " << kTool << "), OCaml " << *ty::config::config_var("version") << std::endl;
      throw ce::ExitWithStatus{0};
    };
    cxxv.doc = "";
    ce::add_arguments({cxxv});
  }
  // Config.standard_library: $OCAMLLIB, else $CAMLLIB, else the default
  ty::config::standard_library_default = ty::config::configured_standard_library_default();
  if (const char* e = std::getenv("OCAMLLIB")) ty::config::standard_library = e;
  else if (const char* e2 = std::getenv("CAMLLIB")) ty::config::standard_library = e2;
  else ty::config::standard_library = default_standard_library(ty::config::standard_library_default);
  try {
    ce::readenv(ce::Position::Before_args);
    ce::parse_arguments(args, ce::anonymous, program);
    // Compmisc.read_clflags_from_env
    auto from_env = [](const char* var, auto parse, auto& flag, const char* usage) {
      const char* v = std::getenv(var);
      if (!v) return;
      auto x = parse(std::string(v));
      if (!x) {
        ty::warnings::Warning w = ty::warnings::Warning::make(ty::warnings::Warning::K::Bad_env_variable);
        w.s = var;
        w.s2 = usage;
        ty::location::prerr_warning(ty::location::none(), w);
      } else if (!flag) {
        flag = *x;
      }
    };
    from_env("OCAML_COLOR",
             [](const std::string& s) -> std::optional<cf::Color> {
               if (s == "auto") return cf::Color::Auto;
               if (s == "always") return cf::Color::Always;
               if (s == "never") return cf::Color::Never;
               return std::nullopt;
             },
             cf::color, "expected \"auto\", \"always\" or \"never\"");
    if (!cf::color) {
      const char* nc = std::getenv("NO_COLOR");
      if (nc && *nc) cf::color = cf::Color::Never;
    }
    from_env("OCAML_ERROR_STYLE",
             [](const std::string& s) -> std::optional<cf::ErrorStyle> {
               if (s == "contextual") return cf::ErrorStyle::Contextual;
               if (s == "short") return cf::ErrorStyle::Short;
               return std::nullopt;
             },
             cf::error_style, "expected \"contextual\" or \"short\"");
    if (cf::plugin) ce::fatal("-plugin is only supported up to OCaml 4.08.0");
    try {
      ce::ActionContext ctx;
      ctx.compile_implementation = [&](cf::Pass start_from, const std::string& source_file,
                                       const std::string& output_prefix) -> int {
        (void)start_from;  // (only Parsing reaches here: .cmir-linear is refused)
        return compile_ml(source_file, output_prefix + (kNative ? ".cmx" : ".cmo"), prof);
      };
      ctx.compile_interface = [](const std::string& source_file, const std::string& output_prefix) {
        return compile_mli(source_file, output_prefix + ".cmi");
      };
      ctx.ocaml_mod_ext = kNative ? ".cmx" : ".cmo";
      ctx.ocaml_lib_ext = kNative ? ".cmxa" : ".cma";
      ce::process_deferred_actions(ctx);
    } catch (const ty::arg::Bad& b) {
      std::cout.flush();
      std::cerr << b.msg << '\n';
      std::cerr.flush();
      ce::print_arguments(program);
      return 2;
    }
    if (!kNative && cf::should_stop_after(cf::Pass::Lambda)) {  // Continue
      profile_dump();
      return 0;
    }
    ce::readenv(ce::Position::Before_link);
    if (kNative) {
      if ((cf::make_package ? 1 : 0) + (cf::make_archive ? 1 : 0) + (cf::shared ? 1 : 0) +
              (ce::stop_early ? 1 : 0) + (cf::output_c_object ? 1 : 0) >
          1) {
        if (!cf::stop_after) ce::fatal("Please specify at most one of -pack, -a, -shared, -c, -output-obj");
        ce::fatal("Options -i and -stop-after (parsing|typing|lambda|scheduling|emit) are  incompatible with -pack, "
                  "-a, -shared, -output-obj");
      }
      if (cf::make_archive) {
        init_path();
        std::string target = ce::extract_output(cf::output_name);
        ty::asmlibrarian::create_archive(ce::get_objfiles(false), target);
        ty::warnings::check_fatal();
      } else if (cf::make_package) {
        init_path();
        std::string target = ce::extract_output(cf::output_name);
        install_typing();
        PpfDump d;
        open_ppf_dump(d, target);
        ty::asmpackager::package_files(d.out(), initial_env(), ce::get_objfiles(false), target);
        ty::warnings::check_fatal();
      } else if (cf::shared) {
        init_path();
        std::string target = ce::extract_output(cf::output_name);
        install_typing();
        PpfDump d;
        open_ppf_dump(d, target);
        ty::asmlink::link_shared(d.out(), ce::get_objfiles(false), target);
        ty::warnings::check_fatal();
      } else if (!ce::stop_early && (!cf::objfiles.empty() || ce::has_linker_inputs)) {
        std::string target;
        if (cf::output_c_object) {
          std::string s = ce::extract_output(cf::output_name);
          if (!(ends_with(s, ty::config::ext_obj) || ends_with(s, ty::config::ext_dll)))
            ce::fatal(std::string("The extension of the output file must be ") + ty::config::ext_obj + " or " +
                      ty::config::ext_dll);
          target = s;
        } else {
          target = ce::default_output(cf::output_name);
        }
        init_path();
        // (the compiler's modules initialized: their idents' stamps)
        install_typing();
        PpfDump d;
        open_ppf_dump(d, target);
        std::vector<std::string> objs = ce::get_objfiles(true);
        ty::asmlink::link(d.out(), objs, target);
        ty::warnings::check_fatal();
      }
      profile_dump();
      return 0;
    }
    if ((cf::make_archive ? 1 : 0) + (cf::make_package ? 1 : 0) + (ce::stop_early ? 1 : 0) +
            (cf::output_c_object ? 1 : 0) >
        1) {
      if (!cf::stop_after) ce::fatal("Please specify at most one of -pack, -a, -c, -output-obj");
      ce::fatal("Options -i and -stop-after (parsing|typing|lambda) are  incompatible with -pack, -a, -output-obj");
    }
    if (cf::make_archive) {
      init_path();
      std::string out = ce::extract_output(cf::output_name);
      std::string o = ty::main_args::unsupported_archive_option();
      if (!o.empty()) ce::fatal(CPPCAML_SELF ": option " + o + " is not supported yet");
      ty::bytelibrarian::create_archive(ce::get_objfiles(false), out);
      ty::warnings::check_fatal();
    } else if (cf::make_package) {
      init_path();
      std::string extracted_output = ce::extract_output(cf::output_name);
      std::vector<std::string> revd = ce::get_objfiles(false);
      install_typing();
      ty::bytepackager::package_files(initial_env(), revd, extracted_output);
      ty::warnings::check_fatal();
    } else if (!ce::stop_early && !cf::objfiles.empty()) {
      std::string target;
      if (cf::output_c_object && !cf::output_complete_executable) {
        std::string s = ce::extract_output(cf::output_name);
        if (!(ends_with(s, ty::config::ext_obj) || ends_with(s, ty::config::ext_dll) || ends_with(s, ".c")))
          ce::fatal(std::string("The extension of the output file must be .c, ") + ty::config::ext_obj + " or " +
                    ty::config::ext_dll);
        target = s;
      } else {
        target = ce::default_output(cf::output_name);
      }
      init_path();
      std::string o = ty::main_args::unsupported_link_option();
      if (!o.empty()) ce::fatal(CPPCAML_SELF ": option " + o + " is not supported yet");
      if (int rc = link(ce::get_objfiles(true), target)) return rc;
      ty::warnings::check_fatal();
    }
    profile_dump();
    return 0;
  } catch (const ce::ExitWithStatus& e) {
    return e.status;
  } catch (const ty::arg::SysError& e) {
    // Sys_error (from -args): Location's "I/O error" report
    ty::location::print_report(ty::location::err_formatter(),
                               ty::location::errorf(ty::location::in_file(ty::location::input_name),
                                                    "I/O error: %s", std::string(e.what())));
    ty::location::err_flush();
    return 2;
  } catch (...) {
    std::exception_ptr ep = std::current_exception();
    if (uncaught_fatal_error(ep)) return 2;
    if (report_exception(ep)) return 2;
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::cout.flush();
      std::cerr << CPPCAML_SELF ": " << e.what() << '\n';
    } catch (...) {
      std::cerr << CPPCAML_SELF ": internal error\n";
    }
    return 2;
  }
}

// The compiler runs on a large stack.  OCaml's native frames
// are several times smaller than the port's, so a recursion ocamlc.opt
// takes in its 8 MiB system stack (a deep type graph in Ctype, a long
// expression) needs more here; the reservation is virtual, pages are
// committed as the recursion touches them.  Where the port does run out,
// it does what ocamlc does when OCaml raises Stack_overflow past
// Location.report_exception: the uncaught exception handler's "Fatal
// error: exception Stack overflow" (Printexc's spelling), exit 2 (a guard
// region below the stack, caught on an alternate signal stack).  ocamlc.opt
// reaches it once its fiber stack hits OCAMLRUNPARAM's l (1 GiB by default,
// typing-misc/conjunctive_types.ml's unbounded update_level_abbrev
// recursion): the port's larger frames get there sooner, with the same
// result.
namespace {
constexpr std::size_t kStackSize = std::size_t{1} << 30;   // 1 GiB
constexpr std::size_t kGuardSize = std::size_t{1} << 20;   // 1 MiB
char* g_guard_lo = nullptr;
char* g_guard_hi = nullptr;

void on_segv(int sig, siginfo_t* info, void*) {
  char* a = static_cast<char*>(info->si_addr);
  if (a >= g_guard_lo && a < g_guard_hi) {
    static const char msg[] = "Fatal error: exception Stack overflow\n";
    std::fflush(nullptr);
    (void)!::write(2, msg, sizeof msg - 1);
    ::_exit(2);
  }
  // any other fault: the default action (a core dump)
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

struct MainArgs {
  int argc;
  char** argv;
  int rc;
};
MainArgs g_args;
// The main thread switches to the big stack (ucontext) rather than handing
// the work to a second thread: a thread's creation and join cost two trips
// through the scheduler, measurable on a small unit's compile.
ucontext_t g_main_ctx, g_big_ctx;

void big_stack_entry() {
  g_args.rc = run_main(g_args.argc, g_args.argv);
  // returning resumes g_main_ctx (uc_link)
}

// run_main on a kStackSize stack; falls back to the normal stack when the
// reservation fails (e.g. a tight ulimit -v)
int run_main_big_stack(int argc, char** argv) {
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
  // AddressSanitizer does not follow a swapcontext'd stack (it reports the
  // deep recursions there as stack errors): run on the thread's own stack
  // (`ulimit -s unlimited` for the deep ones)
  return run_main(argc, argv);
#endif
#endif
  g_args = MainArgs{argc, argv, 0};
  void* mem = ::mmap(nullptr, kGuardSize + kStackSize, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (mem == MAP_FAILED) return run_main(argc, argv);
  g_guard_lo = static_cast<char*>(mem);
  g_guard_hi = g_guard_lo + kGuardSize;
  ::mprotect(mem, kGuardSize, PROT_NONE);  // the stack grows down into it
  struct sigaction sa{};
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&sa.sa_mask);
  ::sigaction(SIGSEGV, &sa, nullptr);
  // the SIGSEGV handler runs on an alternate stack (the big one is full)
  static char altstack[64 * 1024];
  stack_t ss{};
  ss.ss_sp = altstack;
  ss.ss_size = sizeof altstack;
  ::sigaltstack(&ss, nullptr);
  if (::getcontext(&g_big_ctx) != 0) return run_main(argc, argv);
  g_big_ctx.uc_stack.ss_sp = g_guard_hi;
  g_big_ctx.uc_stack.ss_size = kStackSize;
  g_big_ctx.uc_link = &g_main_ctx;
  ::makecontext(&g_big_ctx, big_stack_entry, 0);
  if (::swapcontext(&g_main_ctx, &g_big_ctx) != 0) return run_main(argc, argv);
  return g_args.rc;
}
}  // namespace

int main(int argc, char** argv) {
  cppcaml::typing::typemod::report_lexer_error_hook = report_lexer_error;
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
  int rc = run_main_big_stack(argc, argv);
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  // Leak-checkers (asan/valgrind) and -pg profiling need the normal exit path
  // (static destructors, atexit-registered gmon writer) to run; opt out there.
  if (std::getenv("CPPCAML_NO_FASTEXIT")) return rc;
  std::_Exit(rc);
}
