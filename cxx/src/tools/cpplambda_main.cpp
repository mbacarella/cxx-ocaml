// c++lambda — translate an OCaml source file to Lambda and print it in
// -dlambda format, byte-comparable (after stamp normalization) with
// `ocamlc -dlambda -stop-after lambda`.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cppcaml/infer_check.hpp"
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
  namespace fs = std::filesystem;
  std::string file, stdlib_dir = "stdlib";
  std::vector<std::string> incdirs;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-I" && i + 1 < argc) incdirs.push_back(argv[++i]);
    else file = a;
  }
  if (file.empty()) { std::cerr << "usage: c++lambda [-I <dir>]... <file.ml>\n"; return 2; }
  for (const std::string& d : incdirs)
    if (fs::exists(fs::path(d) / "stdlib.cmi")) { stdlib_dir = d; break; }
  cppcaml::lambda::set_module_dirs(incdirs);
  cppcaml::set_infer_module_dirs(incdirs);
  std::ifstream in(file, std::ios::binary);
  if (!in) { std::cerr << "c++lambda: cannot open " << file << '\n'; return 2; }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    std::vector<std::string> dirfiles;
    auto structure = cppcaml::parse_structure(ss.str(), dirfiles);
    auto code = cppcaml::lambda::translate_implementation(structure, module_name(file),
                                                          stdlib_dir, file);
    cppcaml::lambda::print_dlambda(code, std::cout);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  }
  return 0;
}
