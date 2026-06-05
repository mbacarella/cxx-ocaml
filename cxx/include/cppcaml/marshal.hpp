// Reader for OCaml's "external" Marshal format (the wire format produced by
// output_value / input_value).  See runtime/{intern.c,extern.c} and
// runtime/caml/intext.h in this tree for the authoritative spec.
//
// This is the bottom plumbing layer for loading .cmi files: a .cmi is just the
// cmi magic string followed by three back-to-back Marshal values
// ((name, signature), crcs, flags).  We decode the wire format into a generic
// value arena that faithfully reconstructs sharing and cycles, then a higher
// layer (cmi.cpp) interprets that arena as Types.signature.
//
// Scope: small + big headers, uncompressed.  The cmi read path is always plain
// Marshal (utils/compression.ml: input_value = Stdlib.input_value), and our
// stdlib .cmi files are emitted uncompressed, so zstd is deferred until a
// compressed magic is actually encountered.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cppcaml::marshal {

// A decoded OCaml value.  Heap blocks reference their fields by arena index so
// that shared substructure and cyclic graphs (recursive type_expr) round-trip
// exactly as the runtime intended.
struct Value {
  enum class Kind { Int, String, Double, Block, DoubleArray };
  Kind kind = Kind::Int;

  long long i = 0;              // Int
  std::string str;             // String (raw bytes, may contain NULs)
  double d = 0.0;              // Double
  unsigned tag = 0;            // Block tag
  std::vector<std::size_t> fields;  // Block: arena ids of fields, in order
  std::vector<double> darr;    // DoubleArray
};

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Owns the decoded value graph; nodes are referred to by index (id).
class Arena {
public:
  const Value& operator[](std::size_t id) const { return nodes_[id]; }
  std::size_t size() const { return nodes_.size(); }

  std::size_t push(Value v) {
    nodes_.push_back(std::move(v));
    return nodes_.size() - 1;
  }
  Value& at(std::size_t id) { return nodes_[id]; }

private:
  std::vector<Value> nodes_;
};

// Decode one Marshal value beginning at `data[off]` (off should point at the
// 4-byte Marshal magic).  Returns the arena id of the decoded root and advances
// `off` past the consumed value.  Multiple values can be read in sequence from
// the same buffer by threading `off`.
std::size_t read_value(const std::uint8_t* data, std::size_t len,
                       std::size_t& off, Arena& arena);

}  // namespace cppcaml::marshal
