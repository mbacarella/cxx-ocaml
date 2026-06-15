// c++ocamlc — the drop-in driver: compile an OCaml source file and link it into
// a runnable bytecode executable in one step.
//
//   c++ocamlc hello.ml -o hello && ./hello
//
// Runs the whole c++caml pipeline (parse -> infer -> Lambda -> Bytegen ->
// emitcode) then links against the stdlib and writes a `#!ocamlrun` launcher so
// the result is directly executable.  No ocamlc involved; only ocamlrun (the C
// VM) and the prebuilt stdlib objects are reused.
#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/bytecode.hpp"
#include "cppcaml/cmo.hpp"
#include "cppcaml/lambda.hpp"
#include "cppcaml/link.hpp"
#include "cppcaml/parser.hpp"

namespace fs = std::filesystem;

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

static std::string module_name(const std::string& path) {
  std::string base = fs::path(path).filename().string();
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty()) base[0] = (char)std::toupper((unsigned char)base[0]);
  return base;
}

int main(int argc, char** argv) {
  std::string in_path, out_path, stdlib_dir, runtime;  // stdlib_dir: -I, else discovered
  bool compile_only = false;  // -c : stop at the .cmo
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) out_path = argv[++i];
    else if (a == "-I" && i + 1 < argc) stdlib_dir = argv[++i];
    else if (a == "-runtime" && i + 1 < argc) runtime = argv[++i];
    else if (a == "-c") compile_only = true;
    else in_path = a;
  }
  if (in_path.empty()) {
    std::cerr << "usage: c++ocamlc [-c] [-I <stdlibdir>] [-runtime <ocamlrun>] "
                 "<file.ml> [-o <out>]\n";
    return 2;
  }
  std::string mod = module_name(in_path);
  stdlib_dir = discover_stdlib(stdlib_dir);
  if (out_path.empty()) {
    fs::path p(in_path);
    out_path = compile_only ? (p.parent_path() / (p.stem().string() + ".cmo")).string()
                            : p.stem().string();
  }
  // Default the launcher path to an absolute ocamlrun next to the stdlib dir.
  if (runtime.empty()) {
    fs::path r = fs::absolute(fs::path(stdlib_dir)).parent_path() / "runtime" / "ocamlrun";
    if (fs::exists(r)) runtime = r.string();
  }

  std::ifstream in(in_path, std::ios::binary);
  if (!in) { std::cerr << "c++ocamlc: cannot open " << in_path << '\n'; return 2; }
  std::ostringstream ss; ss << in.rdbuf();

  // 1. compile source -> .cmo
  std::string cmo = compile_only ? out_path : (fs::temp_directory_path() / (mod + ".cmo")).string();
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    std::vector<std::string> required_globals;
    auto code = cppcaml::lambda::translate_implementation(structure, mod, stdlib_dir, in_path,
                                                          &required_globals);
    auto instrs = cppcaml::bytecode::compile_implementation(code, mod);
    cppcaml::cmo::write_cmo(instrs, mod, cmo, required_globals);
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++ocamlc: " << in_path << ": parse error at " << e.pos << ": " << e.what() << '\n';
    return 1;
  }
  if (compile_only) return 0;

  // 2. link: stdlib + this unit + std_exit (flushes stdout at exit)
  std::vector<std::string> inputs = {stdlib_dir + "/stdlib.cma", cmo, stdlib_dir + "/std_exit.cmo"};
  try {
    cppcaml::link::link_executable(inputs, out_path, runtime);
  } catch (const std::exception& e) {
    std::cerr << "c++ocamlc: link error: " << e.what() << '\n';
    return 1;
  }
  // 3. make the launcher executable
  chmod(out_path.c_str(), 0755);
  return 0;
}
