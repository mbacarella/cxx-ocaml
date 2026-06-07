// c++instr — translate an OCaml source file to bytecode instructions and print
// them in -dinstr format, byte-comparable (after label normalization) with
// `ocamlc -dinstr -c`.
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/bytecode.hpp"
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
  if (argc < 2) { std::cerr << "usage: c++instr <file.ml>\n"; return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { std::cerr << "c++instr: cannot open " << argv[1] << '\n'; return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    std::string mod = module_name(argv[1]);
    auto code = cppcaml::lambda::translate_implementation(structure, mod);
    auto instrs = cppcaml::bytecode::compile_implementation(code, mod);
    cppcaml::bytecode::print_dinstr(instrs, std::cout);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  }
  return 0;
}
