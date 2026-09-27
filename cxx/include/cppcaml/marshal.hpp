// Reader for OCaml's "external" Marshal format (the wire format produced by
// output_value / input_value).  See runtime/{intern.c,extern.c} and
// runtime/caml/intext.h in this tree for the authoritative spec.
//
// This is the bottom plumbing layer for loading .cmi files: a .cmi is just the
// cmi magic string followed by three back-to-back Marshal values
// ((name, signature), crcs, flags).  We decode the wire format into a generic
// value arena that faithfully reconstructs sharing and cycles, which
// typing/cmi_format.cpp's Reader interprets (and link.cpp / Bytepackager read
// .cmo descriptors with).
//
// Scope: small + big headers, uncompressed.  The cmi read path is always plain
// Marshal (utils/compression.ml: input_value = Stdlib.input_value), and our
// stdlib .cmi files are emitted uncompressed, so zstd is deferred until a
// compressed magic is actually encountered.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cppcaml::marshal {

// A read-only view of one Block node's field ids: a (pointer, count) slice into
// the arena's single contiguous fields pool.  Exposes just the vector subset the
// readers use (size/empty/[]/at/begin/end) so call sites are unchanged, but the
// per-node std::vector<size_t> -- one heap allocation per Block, ~500k/compile --
// is gone: the pool is filled once (finalize) or bulk-loaded once (cache hit).
struct Fields {
  const std::size_t* p = nullptr;
  std::size_t n = 0;
  std::size_t size() const { return n; }
  bool empty() const { return n == 0; }
  std::size_t operator[](std::size_t i) const { return p[i]; }
  std::size_t at(std::size_t i) const {
    if (i >= n) throw std::out_of_range("marshal::Fields::at");
    return p[i];
  }
  const std::size_t* begin() const { return p; }
  const std::size_t* end() const { return p + n; }
};

// A decoded OCaml value.  Heap blocks reference their fields by arena index so
// that shared substructure and cyclic graphs (recursive type_expr) round-trip
// exactly as the runtime intended.
struct Value {
  enum class Kind { Int, String, Double, Block, DoubleArray };

  // Cold payload for the rare node kinds (String / Double / DoubleArray and the
  // scalar boxed-int customs).  ~90% of the ~500k nodes decoded per compile are
  // Int or Block and touch only i/tag/fields, so keeping these five members
  // out-of-line shrinks Value from ~152 to ~48 bytes -- a large cut in the
  // per-node construct/move/destruct churn and memory traffic over the arena.
  struct Extra {
    std::string str;             // String (raw bytes, may contain NULs)
    double d = 0.0;              // Double
    std::vector<double> darr;    // DoubleArray
    // A scalar boxed-int custom (int32/int64/nativeint) decodes to an Int for
    // the type reader, but also keeps its verbatim on-disk bytes so the linker
    // can round-trip the boxed value into the DATA section unchanged.
    std::string custom_raw;
    int custom_bsize = 0;        // in-memory data size in bytes
  };

  Kind kind = Kind::Int;
  long long i = 0;                  // Int (scalar boxed ints decode to Int too)
  unsigned tag = 0;                 // Block tag
  Fields fields;                    // Block: slice of arena ids (see Arena pool)
  // Allocated only for String/Double/DoubleArray/custom nodes; null otherwise.
  std::unique_ptr<Extra> extra;

  // Lazily materialise the cold payload (write path, marshal decoder only).
  Extra& ensure_extra() {
    if (!extra) extra = std::make_unique<Extra>();
    return *extra;
  }
  // Read accessors that degrade gracefully when the node has no Extra: an Int or
  // Block reads an empty string / zero / empty array, matching the old inline
  // default-constructed members.
  const std::string& str() const {
    static const std::string kEmpty;
    return extra ? extra->str : kEmpty;
  }
  double d() const { return extra ? extra->d : 0.0; }
  const std::vector<double>& darr() const {
    static const std::vector<double> kEmpty;
    return extra ? extra->darr : kEmpty;
  }
  const std::string& custom_raw() const {
    static const std::string kEmpty;
    return extra ? extra->custom_raw : kEmpty;
  }
  int custom_bsize() const { return extra ? extra->custom_bsize : 0; }
};

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Owns the decoded value graph; nodes are referred to by index (id).
//
// Every Block's field ids live in one contiguous pool (fpool_).  A block reserves
// its slice at allocation time -- BEFORE descending into its children -- so the
// slice stays contiguous despite the pre-order recursion (a child's slice is
// reserved further along, never inside its parent's).  Because fpool_ may realloc
// as later blocks reserve, field writes go through the stable index foff_[id]+k,
// and finalize() seals each node's span pointer once the pool has stopped
// growing.  Result: zero per-Block heap allocation -- the old std::vector<size_t>
// per node (~500k/compile) is gone from both the decode and the cache-load path.
class Arena {
public:
  const Value& operator[](std::size_t id) const { return nodes_[id]; }
  std::size_t size() const { return nodes_.size(); }

  // Construct a fresh (default) node and return its id; the reader fills it via
  // at(id).  A reference from at() is invalidated by a later new_node() (vector
  // realloc), so callers re-fetch by id; field writes use set_field (by index).
  std::size_t new_node() {
    nodes_.emplace_back();
    foff_.push_back(0);
    return nodes_.size() - 1;
  }
  Value& at(std::size_t id) { return nodes_[id]; }
  void reserve(std::size_t n) {
    nodes_.reserve(n);
    foff_.reserve(n);
    fpool_.reserve(n);
  }

  // Reserve a contiguous `n`-id slice for block `id`; call before reading its
  // children.  The span pointer is sealed later (finalize); only the length is
  // recorded now so an in-progress arena is still describable.
  std::size_t begin_block(std::size_t id, std::size_t n) {
    std::size_t off = fpool_.size();
    fpool_.resize(off + n);
    foff_[id] = off;
    nodes_[id].fields.n = n;
    return off;
  }
  void set_field(std::size_t id, std::size_t k, std::size_t child) {
    fpool_[foff_[id] + k] = child;
  }

  // Seal every block's field span to point into the now-stable pool.  Call once
  // after all reads on this arena complete and before any field is read.
  void finalize() {
    const std::size_t* base = fpool_.data();
    for (std::size_t id = 0; id < nodes_.size(); ++id)
      if (nodes_[id].fields.n) nodes_[id].fields.p = base + foff_[id];
  }

  // ---- cache-hit loader ----
  // Size the node array and reserve the exact pool, then the caller fills each
  // node via at(id) + begin_block/set_field, and finalize() seals the spans --
  // the same primitives as decode, just fed from the flat blob instead of wire.
  void load_reserve(std::size_t nnodes, std::size_t nfields) {
    nodes_.resize(nnodes);
    foff_.assign(nnodes, 0);
    fpool_.reserve(nfields);
  }

private:
  std::vector<Value> nodes_;
  std::vector<std::size_t> foff_;   // per-node offset of its field slice in fpool_
  std::vector<std::size_t> fpool_;  // all blocks' field ids, contiguous per block
};

// Decode one Marshal value beginning at `data[off]` (off should point at the
// 4-byte Marshal magic).  Returns the arena id of the decoded root and advances
// `off` past the consumed value.  Multiple values can be read in sequence from
// the same buffer by threading `off`.
std::size_t read_value(const std::uint8_t* data, std::size_t len,
                       std::size_t& off, Arena& arena);

// Advance `off` past one Marshal value (which must begin at its 4-byte magic)
// WITHOUT decoding its body -- reads only the header's declared data length and
// jumps to the value's end.  Used to skip over a value we don't need (e.g. a
// cmi's signature) to reach the next one (its crc table) without materialising
// the whole node graph.  Throws marshal::Error on a bad/short header.
void skip_value(const std::uint8_t* data, std::size_t len, std::size_t& off);

}  // namespace cppcaml::marshal
