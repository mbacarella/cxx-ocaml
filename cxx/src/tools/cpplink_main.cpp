// c++link — link .cmo/.cma objects into a runnable bytecode executable.
#include <iostream>
#include <string>
#include <vector>

#include "cppcaml/link.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> inputs;
  std::string out = "a.out", runtime;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) out = argv[++i];
    else if (a == "-runtime" && i + 1 < argc) runtime = argv[++i];
    else inputs.push_back(a);
  }
  if (inputs.empty()) {
    std::cerr << "usage: c++link [-runtime <ocamlrun>] <obj.cmo|lib.cma>... -o <exe>\n";
    return 2;
  }
  try {
    cppcaml::link::link_executable(inputs, out, runtime);
  } catch (const std::exception& e) {
    std::cerr << "c++link: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
