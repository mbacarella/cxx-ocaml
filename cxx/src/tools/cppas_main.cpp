// c++as: the in-process assembler as a command (the oracle: GNU as 2.42,
// `as -o foo.o foo.s` and this tool's object are compared byte for byte).
//   c++as [--debug-prefix-map OLD=NEW]... [-o OUT] IN.s
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "cppcaml/typing/amd64/x86_assembler.hpp"

int main(int argc, char** argv) {
  std::string out = "a.out", in;
  std::vector<std::pair<std::string, std::string>> prefix_map;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) out = argv[++i];
    else if (a == "--debug-prefix-map" && i + 1 < argc) {
      std::string m = argv[++i];
      std::size_t eq = m.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "c++as: bad --debug-prefix-map %s\n", m.c_str());
        return 2;
      }
      prefix_map.emplace_back(m.substr(0, eq), m.substr(eq + 1));
    } else if (in.empty() && (a.empty() || a[0] != '-'))
      in = a;
    else {
      std::fprintf(stderr, "c++as: usage: c++as [--debug-prefix-map OLD=NEW] [-o OUT] IN.s\n");
      return 2;
    }
  }
  std::ifstream is(in, std::ios::binary);
  if (!is) {
    std::fprintf(stderr, "c++as: cannot open %s\n", in.c_str());
    return 2;
  }
  std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  std::vector<std::uint8_t> obj;
  try {
    obj = cppcaml::typing::x86_assembler::assemble(text, prefix_map);
  } catch (const cppcaml::typing::x86_assembler::Unsupported& e) {
    std::fprintf(stderr, "c++as: %s: unsupported: %s\n", in.c_str(), e.what());
    return 3;
  }
  std::FILE* fp = std::fopen(out.c_str(), "wb");
  if (!fp || std::fwrite(obj.data(), 1, obj.size(), fp) != obj.size() || std::fclose(fp) != 0) {
    std::fprintf(stderr, "c++as: cannot write %s\n", out.c_str());
    return 2;
  }
  return 0;
}
