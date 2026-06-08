// c++lambda — translate an OCaml source file to Lambda and print it in
// -dlambda format, byte-comparable (after stamp normalization) with
// `ocamlc -dlambda -stop-after lambda`.
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/lambda.hpp"
#include "cppcaml/parser.hpp"

// OCaml's module name: the file basename without extension, first letter
// capitalized (e.g. "l0.ml" -> "L0").
static std::string module_name(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = base.find('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  if (!base.empty()) base[0] = (char)std::toupper((unsigned char)base[0]);
  return base;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: c++lambda <file.ml>\n"; return 2; }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { std::cerr << "c++lambda: cannot open " << argv[1] << '\n'; return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    auto code = cppcaml::lambda::translate_implementation(structure, module_name(argv[1]),
                                                          "stdlib", argv[1]);
    cppcaml::lambda::print_dlambda(code, std::cout);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  }
  return 0;
}
