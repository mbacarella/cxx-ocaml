// c++instr — translate an OCaml source file to bytecode instructions and print
// them in -dinstr format, byte-comparable (after label normalization) with
// `ocamlc -dinstr -c`.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/bytecode.hpp"
#include "cppcaml/cmi.hpp"
#include "cppcaml/infer_check.hpp"
#include "cppcaml/lambda.hpp"
#include "cppcaml/parser.hpp"

static std::string module_name(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty()) base[0] = (char)std::toupper((unsigned char)base[0]);
  return base;
}

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  std::string file, stdlib_dir = "stdlib";
  std::vector<std::string> incdirs;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-I" && i + 1 < argc) {
      std::string d = argv[++i];
      if (d.rfind("+", 0) == 0) {  // `+unix` -> <stdlib>/../otherlibs/unix dev fallback
        std::string x = d.substr(1);
        fs::path cand = fs::path(stdlib_dir) / x;
        if (!fs::exists(cand)) cand = fs::path(stdlib_dir) / ".." / "otherlibs" / x;
        d = cand.string();
      }
      incdirs.push_back(d);
      if (fs::exists(fs::path(d) / "stdlib.cmi")) stdlib_dir = d;
    } else if (a == "-stdlib" && i + 1 < argc) {
      stdlib_dir = argv[++i];
    } else if (a[0] != '-') {
      file = a;
    }
  }
  if (file.empty()) { std::cerr << "usage: c++instr [-I dir]... <file.ml>\n"; return 2; }
  std::ifstream in(file, std::ios::binary);
  if (!in) { std::cerr << "c++instr: cannot open " << file << '\n'; return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  cppcaml::lambda::set_module_dirs(incdirs);
  cppcaml::set_infer_module_dirs(incdirs);
  cppcaml::set_infer_stdlib_dir(stdlib_dir);
  cppcaml::cmi::cmiw::set_module_dirs(stdlib_dir, incdirs);
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    std::string mod = module_name(file);
    auto code = cppcaml::lambda::translate_implementation(structure, mod, stdlib_dir, file);
    auto instrs = cppcaml::bytecode::compile_implementation(code, mod);
    cppcaml::bytecode::print_dinstr(instrs, std::cout);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  }
  return 0;
}
