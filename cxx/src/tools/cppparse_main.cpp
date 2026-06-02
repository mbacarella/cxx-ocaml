// c++parse — parse an OCaml source file and print the AST in -dparsetree format,
// byte-comparable with `ocamlc -stop-after parsing -dparsetree`.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/parser.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: c++parse <file.ml>\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "c++parse: cannot open " << argv[1] << '\n';
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  try {
    auto structure = cppcaml::parse_structure(src);
    cppcaml::ast::print_dparsetree(structure, argv[1], std::cout);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "PARSE_ERROR\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  }
  return 0;
}
