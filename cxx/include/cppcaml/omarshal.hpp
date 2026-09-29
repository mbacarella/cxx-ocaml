// A minimal OCaml Marshal *writer* (the inverse of marshal.cpp's reader): build a
// value tree and serialize it in the intext.h wire format.  Shared by the .cmo
// emitter (compilation_unit descriptor) and the linker (DATA global table).
//
// Values are bump-allocated in a process-long arena and never freed one by
// one: a .cmo's -g debug section builds hundreds of thousands of them, and a
// reference-counted node with a heap vector of fields cost ~3x the memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace cppcaml::omarshal {

// storage from the values' arena (never freed)
void* arena_alloc(std::size_t n, std::size_t align);

// A growable array in the arena, with value semantics: assigning copies the
// elements into fresh storage (the source is left alone).
template <class T>
class ArenaVec {
 public:
  ArenaVec() = default;
  ArenaVec(std::initializer_list<T> l) { assign(l.begin(), l.size()); }
  ArenaVec(const std::vector<T>& v) { assign(v.data(), v.size()); }
  ArenaVec(const ArenaVec& o) { assign(o.p_, o.n_); }
  ArenaVec& operator=(const ArenaVec& o) {
    if (this != &o) assign(o.p_, o.n_);
    return *this;
  }
  ArenaVec& operator=(std::initializer_list<T> l) {
    assign(l.begin(), l.size());
    return *this;
  }
  ArenaVec& operator=(const std::vector<T>& v) {
    assign(v.data(), v.size());
    return *this;
  }
  std::size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  T& operator[](std::size_t k) { return p_[k]; }
  const T& operator[](std::size_t k) const { return p_[k]; }
  T* begin() { return p_; }
  T* end() { return p_ + n_; }
  const T* begin() const { return p_; }
  const T* end() const { return p_ + n_; }
  T& front() { return p_[0]; }
  T& back() { return p_[n_ - 1]; }
  const T* data() const { return p_; }
  void push_back(const T& x) {
    if (n_ == cap_) {
      std::uint32_t c = cap_ ? 2 * cap_ : 4;
      T* q = static_cast<T*>(arena_alloc(sizeof(T) * c, alignof(T)));
      for (std::uint32_t k = 0; k < n_; ++k) new (q + k) T(p_[k]);
      p_ = q;
      cap_ = c;
    }
    new (p_ + n_) T(x);
    ++n_;
  }

 private:
  static_assert(std::is_trivially_destructible_v<T>, "arena elements are never destroyed");
  void assign(const T* src, std::size_t k) {
    T* q = k ? static_cast<T*>(arena_alloc(sizeof(T) * k, alignof(T))) : nullptr;
    for (std::size_t j = 0; j < k; ++j) new (q + j) T(src[j]);
    p_ = q;
    n_ = cap_ = static_cast<std::uint32_t>(k);
  }
  T* p_ = nullptr;
  std::uint32_t n_ = 0, cap_ = 0;
};

struct Value;

// A value: an OCaml int as an immediate (tagged, like OCaml's own: an int
// takes no object in Marshal's output, so none here), else a pointer to a
// Value in the arena.  (The handful of shared_ptr operations the writers
// use.)
class ValPtr {
 public:
  ValPtr() = default;
  ValPtr(std::nullptr_t) {}
  explicit ValPtr(Value* p) : b_(reinterpret_cast<std::uintptr_t>(p)) {}
  static ValPtr immediate(long long n) {
    ValPtr v;
    v.b_ = (static_cast<std::uintptr_t>(n) << 1) | 1;
    return v;
  }
  bool is_int() const { return (b_ & 1) != 0; }
  long long int_value() const { return static_cast<long long>(static_cast<std::intptr_t>(b_) >> 1); }
  // the value's kind (Int for an immediate)
  inline int kind() const;
  // the Value of a non-int
  Value* get() const { return is_int() ? nullptr : reinterpret_cast<Value*>(b_); }
  Value* operator->() const { return get(); }
  Value& operator*() const { return *get(); }
  explicit operator bool() const { return b_ != 0; }
  std::uintptr_t bits() const { return b_; }
  friend bool operator==(const ValPtr& a, const ValPtr& b) { return a.b_ == b.b_; }
  friend bool operator!=(const ValPtr& a, const ValPtr& b) { return a.b_ != b.b_; }
  friend bool operator<(const ValPtr& a, const ValPtr& b) { return a.b_ < b.b_; }

 private:
  std::uintptr_t b_ = 0;
};

struct Value {
  enum K : std::uint8_t { Int, Str, Dbl, Block, DblArr, Custom } k_ = Int;  // never Int: an immediate
  int tag = 0;
  double d = 0;
  // Custom: the DATA size in bytes, as the serializer wrote it.  extern.c sizes
  // a custom block as `2 + ((sz + wordsize - 1) / wordsize)` -- header + ops +
  // data -- and the 32- and 64-bit counts therefore differ for the same block,
  // which a word count taken on one of them cannot express.
  long long custom_bytes = 0;
  // the 32-bit data size when it differs from the 64-bit one (nativeint:
  // 4 / 8, runtime/ints.c); -1 = custom_bytes
  long long custom_bytes32 = -1;
  std::string s;  // Str data / Custom raw on-disk bytes (from the code byte)
  ArenaVec<ValPtr> fields;
  ArenaVec<double> darr;
  // the marshaling session that last serialized this object, and its index
  // in that session's object table (a repeat is a back-reference)
  std::uint32_t seen_session = 0;
  long long seen_index = 0;
};
inline int ValPtr::kind() const { return is_int() ? Value::Int : get()->k_; }

ValPtr vint(long long n);
ValPtr vstr(std::string s);
ValPtr vdbl(double d);
ValPtr vblock(int tag, std::vector<ValPtr> f);
ValPtr vblock(int tag, std::initializer_list<ValPtr> f);
ValPtr vdblarr(std::vector<double> ds);
ValPtr vcustom(std::string raw, long long data_bytes);  // verbatim custom bytes
// ... with the serializer's 32- and 64-bit data sizes (extern.c sz_32 / sz_64)
ValPtr vcustom2(std::string raw, long long bytes32, long long bytes64);
ValPtr vlist(const std::vector<ValPtr>& xs);  // OCaml list (cons / [])

// Serialize `root` to a complete marshaled blob (20-byte small header + body).
// compressed: extern.c's COMPRESSED flag (Compression.output_value on an OCaml
// with zstd): absolute shared references, the body through ZSTD streaming,
// the compressed header (magic, header length, VLQ lengths / counts).
std::vector<std::uint8_t> marshal(const ValPtr& root, bool compressed = false);
// whether this c++ocamlc was built with libzstd (CPPCAML_ZSTD)
bool zstd_available();

}  // namespace cppcaml::omarshal

template <>
struct std::hash<cppcaml::omarshal::ValPtr> {
  std::size_t operator()(const cppcaml::omarshal::ValPtr& v) const noexcept {
    return std::hash<std::uintptr_t>()(v.bits());
  }
};
