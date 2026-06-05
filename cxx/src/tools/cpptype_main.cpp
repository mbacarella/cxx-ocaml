// c++type — type-check an OCaml source file and print the typedtree in
// -dtypedtree format, byte-comparable (after stamp normalization) with
// `ocamlc -dtypedtree -stop-after typing`.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/parser.hpp"
#include "cppcaml/typer.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: c++type <file.ml>\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "c++type: cannot open " << argv[1] << '\n';
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(src, dirfiles);
    auto typed = cppcaml::type_structure(structure);
    cppcaml::typedtree::print_dtypedtree(typed, argv[1], std::cout, dirfiles);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  } catch (const cppcaml::TypeError& e) {
    std::cout << "TYPE_ERROR\t" << e.what() << '\n';
    return 1;
  }
  return 0;
}
