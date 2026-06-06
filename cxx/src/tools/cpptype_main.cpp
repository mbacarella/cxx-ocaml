// c++type — type-check an OCaml source file and print the typedtree in
// -dtypedtree format, byte-comparable (after stamp normalization) with
// `ocamlc -dtypedtree -stop-after typing`.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/infer_check.hpp"
#include "cppcaml/parser.hpp"
#include "cppcaml/typer.hpp"

int main(int argc, char** argv) {
  bool infer_mode = false;
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--infer") infer_mode = true;
    else path = argv[i];
  }
  if (!path) {
    std::cerr << "usage: c++type [--infer] <file.ml>\n";
    return 2;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "c++type: cannot open " << path << '\n';
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(src, dirfiles);
    if (infer_mode) {  // Slice 2: show inferred top-level value types
      for (auto& [name, ty] : cppcaml::infer_structure_types(structure))
        std::cout << "val " << name << " : " << ty << '\n';
      return 0;
    }
    auto typed = cppcaml::type_structure(structure);
    cppcaml::typedtree::print_dtypedtree(typed, path, std::cout, dirfiles);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  } catch (const cppcaml::TypeError& e) {
    std::cout << "TYPE_ERROR\t" << e.what() << '\n';
    return 1;
  }
  return 0;
}
