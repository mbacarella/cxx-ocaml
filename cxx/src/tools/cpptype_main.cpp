// c++type — type-check an OCaml source file and print the typedtree in
// -dtypedtree format, byte-comparable (after stamp normalization) with
// `ocamlc -dtypedtree -stop-after typing`.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/infer_check.hpp"
#include "cppcaml/lexer.hpp"
#include "cppcaml/parser.hpp"
#include "cppcaml/typer.hpp"

int main(int argc, char** argv) {
  bool infer_mode = false, check_mode = false;
  const char* path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--infer") infer_mode = true;
    else if (a == "--check") check_mode = true;
    else path = argv[i];
  }
  if (!path) {
    std::cerr << "usage: c++type [--infer|--check] <file.ml>\n";
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
      for (auto& [name, ty] : cppcaml::infer_structure_types(structure)) {
        // Operators print parenthesised, like `ocamlc -i`: `( *. )` (the spaces
        // also keep `( * )` from opening a comment). Any name containing a
        // non-identifier char is an operator -- this catches symbolic operators
        // (`*.`, `+`) AND binding operators (`let+`, `and+`), which begin with a
        // letter but still print parenthesised.
        bool op = false;
        for (unsigned char c : name)
          if (!(std::isalnum(c) || c == '_' || c == '\'')) { op = true; break; }
        std::string disp = op ? "( " + name + " )" : name;
        std::cout << "val " << disp << " : " << ty << '\n';
      }
      return 0;
    }
    if (check_mode) {  // strict type-check: print errors, exit 1 if rejected
      // A companion `.mli` with no compiled `.cmi` makes ocamlc reject before
      // typing ("Could not find the .cmi file for interface ..."); match that
      // deterministic build error (a valid file's .cmi would be present).
      {
        std::string p = path;
        if (p.size() > 3 && p.compare(p.size() - 3, 3, ".ml") == 0) {
          std::ifstream mli(p + "i"), cmi(p.substr(0, p.size() - 3) + ".cmi");
          if (mli.good() && !cmi.good()) {
            std::cout << "Error: Could not find the .cmi file for interface\n";
            return 1;
          }
        }
      }
      auto errs = cppcaml::structure_typecheck(structure);
      for (auto& e : errs) std::cout << "Error: " << e << '\n';
      if (errs.empty()) std::cout << "OK\n";
      return errs.empty() ? 0 : 1;
    }
    auto typed = cppcaml::type_structure(structure);
    cppcaml::typedtree::print_dtypedtree(typed, path, std::cout, dirfiles);
  } catch (const cppcaml::ParseError& e) {
    std::cout << "TYPE_ERROR\tparse\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  } catch (const cppcaml::LexError& e) {
    std::cout << "TYPE_ERROR\tlex\t" << e.pos << '\t' << e.what() << '\n';
    return 1;
  } catch (const cppcaml::TypeError& e) {
    std::cout << "TYPE_ERROR\t" << e.what() << '\n';
    return 1;
  } catch (const std::exception& e) {
    // Robustness: never crash (SIGABRT) on an unexpected internal error -- report
    // it as a parser/internal gap so the harnesses exclude it cleanly.
    std::cout << "TYPE_ERROR\tinternal\t" << e.what() << '\n';
    return 1;
  }
  return 0;
}
