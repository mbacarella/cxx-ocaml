// c++cmo — compile an OCaml source file all the way to a .cmo bytecode object.
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/bytecode.hpp"
#include "cppcaml/cmo.hpp"
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
  std::string in_path, out_path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) out_path = argv[++i];
    else in_path = a;
  }
  if (in_path.empty()) { std::cerr << "usage: c++cmo <file.ml> [-o out.cmo]\n"; return 2; }
  if (out_path.empty()) {
    out_path = in_path;
    size_t dot = out_path.rfind('.');
    if (dot != std::string::npos) out_path = out_path.substr(0, dot);
    out_path += ".cmo";
  }
  std::ifstream in(in_path, std::ios::binary);
  if (!in) { std::cerr << "c++cmo: cannot open " << in_path << '\n'; return 2; }
  std::ostringstream ss; ss << in.rdbuf();
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    std::string mod = module_name(in_path);
    auto code = cppcaml::lambda::translate_implementation(structure, mod, "stdlib", in_path,
                                                          nullptr, &dirfiles);
    auto instrs = cppcaml::bytecode::compile_implementation(code, mod);
    cppcaml::cmo::write_cmo(instrs, mod, out_path);
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++cmo: parse error at " << e.pos << ": " << e.what() << '\n';
    return 1;
  }
  return 0;
}
