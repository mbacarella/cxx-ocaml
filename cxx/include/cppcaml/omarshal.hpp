// A minimal OCaml Marshal *writer* (the inverse of marshal.cpp's reader): build a
// value tree and serialize it in the intext.h wire format.  Shared by the .cmo
// emitter (compilation_unit descriptor) and the linker (DATA global table).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cppcaml::omarshal {

struct Value;
using ValPtr = std::shared_ptr<Value>;
struct Value {
  enum K { Int, Str, Dbl, Block, DblArr, Custom } k;
  long long i = 0;
  std::string s;          // Str data / Custom raw on-disk bytes (from the code byte)
  double d = 0;
  int tag = 0;
  std::vector<ValPtr> fields;
  std::vector<double> darr;
  long long custom_words = 0;  // Custom: in-memory size in words (for the header)
};

ValPtr vint(long long n);
ValPtr vstr(std::string s);
ValPtr vdbl(double d);
ValPtr vblock(int tag, std::vector<ValPtr> f);
ValPtr vdblarr(std::vector<double> ds);
ValPtr vcustom(std::string raw, long long words);  // verbatim custom block bytes
ValPtr vlist(const std::vector<ValPtr>& xs);  // OCaml list (cons / [])

// Serialize `root` to a complete marshaled blob (20-byte small header + body).
std::vector<std::uint8_t> marshal(const ValPtr& root);

}  // namespace cppcaml::omarshal
