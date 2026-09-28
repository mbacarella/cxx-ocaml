// A lean Marshal decoder for .cmi reading (cmi_format.cpp's Reader), in
// place of marshal.hpp's generic Arena: the decoded graph as 16-byte nodes,
// immediate integers encoded in the ids themselves (as OCaml does: an odd
// id is the integer `id >> 1`, an even id the node `id >> 1`), strings as
// spans of the input buffer, everything reserved once from the Marshal
// header's object count.  Same wire format and sharing as marshal.cpp;
// internal to cmi_format.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cppcaml/marshal.hpp"

namespace cppcaml::typing::cmi_marshal {

enum class Kind : std::uint8_t { Int, String, Double, Block, DoubleArray };

struct Node {
  Kind kind;
  std::uint8_t tag;   // Block
  std::uint32_t n;    // Block: fields; String: bytes; DoubleArray: elements
  std::uint64_t v;    // Int: value (a boxed-integer custom); Block: first field in the pool;
                      // String / DoubleArray: offset in the input (the top byte: 0,
                      // or k for the k-th decompressed value's buffer); Double: bits
};
static_assert(sizeof(Node) == 16);

using Id = std::uint64_t;  // odd: immediate integer; even: node index * 2
inline bool is_imm(Id id) { return id & 1; }
inline long imm(Id id) { return static_cast<long>(static_cast<std::int64_t>(id) >> 1); }
inline std::size_t node_index(Id id) { return static_cast<std::size_t>(id >> 1); }

class Graph {
 public:
  const std::uint8_t* data = nullptr;   // the input the strings point into
  // the payloads of compressed values (Compression.output_value on an OCaml
  // with zstd), decompressed: their strings' offsets carry the buffer's index
  std::vector<std::vector<std::uint8_t>> owned;
  static constexpr int kBufShift = 56;
  const std::uint8_t* base(std::uint64_t v) const {
    std::uint64_t k = v >> kBufShift;
    return k ? owned[k - 1].data() : data;
  }
  std::vector<Node> nodes;
  // the blocks' fields, contiguous per block (uninitialized storage, grown
  // before each value by the value's size: every field takes a byte)
  std::unique_ptr<Id[]> pool;
  std::size_t pool_n = 0, pool_cap = 0;
  Id field(std::size_t i) const { return pool[i]; }
  void reserve_fields(std::size_t more) {
    if (pool_n + more <= pool_cap) return;
    std::size_t cap = pool_n + more;
    std::unique_ptr<Id[]> q(new Id[cap]);
    if (pool_n) std::memcpy(q.get(), pool.get(), pool_n * sizeof(Id));
    pool = std::move(q);
    pool_cap = cap;
  }
  const Node& node(Id id) const { return nodes[node_index(id)]; }
  std::string_view string(const Node& x) const {
    return {reinterpret_cast<const char*>(base(x.v)) + (x.v & ((std::uint64_t{1} << kBufShift) - 1)), x.n};
  }
  double dbl(const Node& x) const {
    double d;
    std::memcpy(&d, &x.v, 8);
    return d;
  }
};

// Decode the Marshal value at data[off] (its magic) into g; its id.  Values
// read in sequence share the graph.  Throws marshal::Error.
Id read_value(const std::uint8_t* data, std::size_t len, std::size_t& off, Graph& g);

}  // namespace cppcaml::typing::cmi_marshal
