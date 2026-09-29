// c++lex — tokenize an OCaml source file and print the canonical token stream.
// Output is byte-comparable with oracle/dump_tokens.ml run on the trunk lexer.
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "cppcaml/lexer.hpp"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: c++lex <file.ml>\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "c++lex: cannot open " << argv[1] << '\n';
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();

  cppcaml::Lexer lex(src);
  try {
    for (;;) {
      cppcaml::Token t = lex.next();
      cppcaml::print_canonical(t, std::cout);
      if (t.kind == cppcaml::Kind::TEOF) break;
    }
  } catch (const cppcaml::LexError& e) {
    // Message text is not compared against the oracle yet (error-path parity is
    // a later refinement); only the failing position is.
    std::cout << "ERROR\t" << e.pos << '\n';
    return 1;
  }
  return 0;
}
