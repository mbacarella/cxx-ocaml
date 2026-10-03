// Arena memory for the typing/ port (see cxx/PORTING.md, "Memory").
//
// OCaml's heap becomes zones: typing objects (type_expr nodes, their descs,
// paths, idents, declarations, persistent-map nodes) are allocated in a Zone
// and never freed individually.  A Zone runs the destructors of the
// non-trivially-destructible objects it holds when it is itself destroyed.
//
// Immutable OCaml lists inside immutable values become `Slice<T>` (a
// zone-owned array); OCaml strings become `std::string_view` into zone
// storage (`Zone::str`).
#pragma once

#include <algorithm>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <initializer_list>
#include <stdexcept>
#include <iterator>

namespace cppcaml::typing {

// A vector of trivially copyable values keeping up to N of them inline: the
// backend's register arrays and sets are copied with every instruction, and a
// heap allocation each (std::vector's) was a native compilation's largest
// cost.  The std::vector operations the backend uses.
template <class T, std::size_t N>
class SmallVec {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;
  using size_type = std::size_t;
  SmallVec() = default;
  explicit SmallVec(std::size_t n) { resize(n); }
  SmallVec(std::size_t n, const T& v) { resize(n, v); }
  SmallVec(std::initializer_list<T> l) { assign(l.begin(), l.end()); }
  template <class It, class = decltype(*std::declval<It>())>
  SmallVec(It first, It last) {
    assign(first, last);
  }
  SmallVec(const std::vector<T>& v) { assign(v.begin(), v.end()); }
  SmallVec(const SmallVec& o) { assign(o.begin(), o.end()); }
  SmallVec(SmallVec&& o) noexcept { take(o); }
  ~SmallVec() {
    if (p_ != buf_) std::free(p_);
  }
  SmallVec& operator=(const SmallVec& o) {
    if (this != &o) assign(o.begin(), o.end());
    return *this;
  }
  SmallVec& operator=(SmallVec&& o) noexcept {
    if (this != &o) {
      if (p_ != buf_) std::free(p_);
      p_ = buf_;
      cap_ = N;
      take(o);
    }
    return *this;
  }
  SmallVec& operator=(std::initializer_list<T> l) {
    assign(l.begin(), l.end());
    return *this;
  }
  operator std::vector<T>() const { return std::vector<T>(begin(), end()); }

  template <class It>
  void assign(It first, It last) {
    n_ = 0;
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_pointer_t<It>>, T> ||
                  std::contiguous_iterator<It>) {
      std::size_t m = static_cast<std::size_t>(last - first);
      if (m > cap_) grow(m);
      if (m) std::memcpy(p_, &*first, m * sizeof(T));
      n_ = static_cast<std::uint32_t>(m);
    } else {
      if constexpr (std::is_base_of_v<std::forward_iterator_tag,
                                      typename std::iterator_traits<It>::iterator_category>)
        reserve(static_cast<std::size_t>(std::distance(first, last)));
      for (; first != last; ++first) push_back(*first);
    }
  }
  std::size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  T* begin() { return p_; }
  T* end() { return p_ + n_; }
  const T* begin() const { return p_; }
  const T* end() const { return p_ + n_; }
  const T* cbegin() const { return p_; }
  const T* cend() const { return p_ + n_; }
  std::reverse_iterator<const T*> rbegin() const { return std::reverse_iterator<const T*>(end()); }
  std::reverse_iterator<const T*> rend() const { return std::reverse_iterator<const T*>(begin()); }
  T* data() { return p_; }
  const T* data() const { return p_; }
  T& operator[](std::size_t k) { return p_[k]; }
  const T& operator[](std::size_t k) const { return p_[k]; }
  T& at(std::size_t k) {
    if (k >= n_) throw std::out_of_range("SmallVec::at");
    return p_[k];
  }
  const T& at(std::size_t k) const {
    if (k >= n_) throw std::out_of_range("SmallVec::at");
    return p_[k];
  }
  T& front() { return p_[0]; }
  const T& front() const { return p_[0]; }
  T& back() { return p_[n_ - 1]; }
  const T& back() const { return p_[n_ - 1]; }
  void reserve(std::size_t c) {
    if (c > cap_) grow(c);
  }
  [[gnu::noinline]] void grow(std::size_t c) {
    std::size_t nc = cap_ * 2 > c ? cap_ * 2 : c;
    T* q = static_cast<T*>(std::malloc(nc * sizeof(T)));
    if (!q) throw std::bad_alloc();
    if (n_) std::memcpy(q, p_, n_ * sizeof(T));
    if (p_ != buf_) std::free(p_);
    p_ = q;
    cap_ = static_cast<std::uint32_t>(nc);
  }
  void push_back(const T& v) {
    if (n_ == cap_) [[unlikely]] {
      T copy = v;  // (v may be one of ours)
      grow(n_ + 1);
      p_[n_++] = copy;
      return;
    }
    p_[n_++] = v;
  }
  template <class... A>
  T& emplace_back(A&&... a) {
    push_back(T{std::forward<A>(a)...});
    return back();
  }
  void pop_back() { --n_; }
  void clear() { n_ = 0; }
  void resize(std::size_t n) { resize(n, T{}); }
  void resize(std::size_t n, const T& v) {
    reserve(n);
    for (std::size_t k = n_; k < n; ++k) p_[k] = v;
    n_ = static_cast<std::uint32_t>(n);
  }
  T* insert(const T* pos, const T& v) {
    std::size_t k = static_cast<std::size_t>(pos - p_);
    T copy = v;
    reserve(n_ + 1);
    std::memmove(p_ + k + 1, p_ + k, (n_ - k) * sizeof(T));
    p_[k] = copy;
    ++n_;
    return p_ + k;
  }
  template <class It, class = decltype(*std::declval<It>())>
  T* insert(const T* pos, It first, It last) {
    std::size_t k = static_cast<std::size_t>(pos - p_);
    SmallVec tmp(first, last);  // (the range may be ours)
    std::size_t m = tmp.size();
    reserve(n_ + m);
    std::memmove(p_ + k + m, p_ + k, (n_ - k) * sizeof(T));
    if (m) std::memcpy(p_ + k, tmp.p_, m * sizeof(T));
    n_ += static_cast<std::uint32_t>(m);
    return p_ + k;
  }
  T* erase(const T* pos) { return erase(pos, pos + 1); }
  T* erase(const T* first, const T* last) {
    std::size_t k = static_cast<std::size_t>(first - p_), m = static_cast<std::size_t>(last - first);
    std::memmove(p_ + k, p_ + k + m, (n_ - k - m) * sizeof(T));
    n_ -= static_cast<std::uint32_t>(m);
    return p_ + k;
  }
  friend bool operator==(const SmallVec& a, const SmallVec& b) {
    return a.n_ == b.n_ && std::equal(a.begin(), a.end(), b.begin());
  }
  friend bool operator!=(const SmallVec& a, const SmallVec& b) { return !(a == b); }
  friend bool operator<(const SmallVec& a, const SmallVec& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  }

 private:
  void take(SmallVec& o) {
    if (o.p_ == o.buf_) {
      n_ = o.n_;
      if (n_) std::memcpy(buf_, o.buf_, n_ * sizeof(T));
    } else {
      p_ = o.p_;
      n_ = o.n_;
      cap_ = o.cap_;
      o.p_ = o.buf_;
      o.cap_ = N;
    }
    o.n_ = 0;
  }
  T* p_ = buf_;
  std::uint32_t n_ = 0, cap_ = N;
  T buf_[N];
};
// An OCaml list built by consing (the newest first): push_front appends to
// a vector, iteration runs backwards (a std::deque allocates a chunk and a
// map for each register, even an empty one)
template <class T>
class NewestFirst {
 public:
  void push_front(T x) { v_.push_back(std::move(x)); }
  void clear() { v_.clear(); }
  auto begin() const { return v_.rbegin(); }
  auto end() const { return v_.rend(); }
  std::size_t size() const { return v_.size(); }
  bool empty() const { return v_.empty(); }

 private:
  std::vector<T> v_;
};

class Zone {
 public:
  Zone() = default;
  // mmap_all: every block from the kernel (drop_protected can then make the
  // storage inaccessible)
  explicit Zone(bool mmap_all) : mmap_all_(mmap_all) {}
  // a scratch zone (Matching's, Parmatch's heads): cleared while what is
  // built beside it lives on, so nothing may point into it
  bool scratch = false;
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;
  ~Zone() {
    for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) it->second(it->first);
  }
  // 2 MiB-aligned storage straight from the kernel, advised for
  // transparent huge pages when [advise] (Free unmaps it)
  static char* huge_block(std::size_t sz, bool advise = true);

  // (the bump is inlined at every allocation; a new block is out of line)
  void* alloc(std::size_t n, std::size_t align) {
    std::size_t off = (off_ + align - 1) & ~(align - 1);
    if (cur_ && off + n <= cap_) [[likely]] {
      off_ = off + n;
      return cur_ + off;
    }
    return alloc_block(n);
  }

  template <class T, class... A>
  T* make(A&&... a) {
    void* p = alloc(sizeof(T), alignof(T));
    T* t = new (p) T{std::forward<A>(a)...};
    if constexpr (!std::is_trivially_destructible_v<T>)
      dtors_.emplace_back(t, [](void* q) { static_cast<T*>(q)->~T(); });
    return t;
  }

  // bytes of block storage all zones have allocated (CPPCAML_PROFILE)
  static std::size_t& block_bytes() {
    static std::size_t n = 0;
    return n;
  }

  // Drop everything the zone holds (running the destructors) but keep its
  // first block for the next allocations: a scratch zone reused phase after
  // phase touches the same memory again instead of fresh blocks.
  void clear() {
    for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) it->second(it->first);
    dtors_.clear();
    if (blocks_.empty()) return;
    if (blocks_.size() > 1 || ranges_.size() > 1) {
      blocks_.resize(1);
      cur_ = blocks_[0].get();
      // keep the first block's map node (a scratch zone is cleared per
      // scope: no allocation for it)
      auto first = ranges_.extract(cur_);
      ranges_.clear();
      cap_ = first.mapped();
      ranges_.insert(std::move(first));
    }
    off_ = 0;
  }

  // (a check) drop everything, making the storage inaccessible instead of
  // freeing it: a later use of it faults (the zone must be mmap_all)
  void drop_protected();

  // A zone allocating only from [base, base + cap) (a .cmi image being
  // recorded, cmi_image.hpp): past it, alloc throws RegionFull.
  struct RegionFull {};
  void use_region(char* base, std::size_t cap) {
    cur_ = base;
    cap_ = cap;
    off_ = 0;
    fixed_ = true;
    ranges_.emplace(base, cap);
  }
  std::size_t region_used() const { return off_; }
  // [base, base + n) is zone storage from now on (a mapped .cmi image): its
  // strings are borrowed, not copied (zborrow).
  void adopt(const char* base, std::size_t n) { ranges_.emplace(base, n); }

  // the bytes of block storage this zone holds
  std::size_t bytes() const {
    std::size_t n = 0;
    for (const auto& r : ranges_) n += r.second;
    return n;
  }

  // whether [p] points into this zone's storage
  bool is_fixed() const { return fixed_; }
  bool owns(const char* p) const {
    auto it = ranges_.upper_bound(p);
    if (it == ranges_.begin()) return false;
    --it;
    return p < it->first + it->second;
  }

  // A copy has an identity (its address), "" included: the .cmi writer
  // shares strings by identity, as Marshal does.
  std::string_view str(std::string_view s) {
    if (s.empty()) return {static_cast<char*>(alloc(1, 1)), 0};
    char* p = static_cast<char*>(alloc(s.size(), 1));
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
  }

 private:
  // A fresh block for an allocation of [n] bytes (malloc'd and huge-page
  // storage is aligned for any fundamental type, so it starts at offset 0
  // whatever the alignment); a fixed region has no more.
  [[gnu::noinline]] void* alloc_block(std::size_t n) {
    static_assert(alignof(std::max_align_t) >= 8);
    if (fixed_) throw RegionFull{};
    // a zone that has grown takes big blocks backed by huge pages (the
    // typing data's TLB misses); a small one stays in small blocks
    bool big = blocks_.size() >= kSmallBlocks;
    // (most zones -- a .cmi's, a function's scratch -- stay small, and the
    // allocator keeps a block's pages resident: small blocks are small)
    std::size_t unit = big ? kBigBlock : kBlock;
    std::size_t sz = n > unit ? n : unit;
    block_bytes() += sz;
    bool mapped = big || mmap_all_;
    // huge pages once the compilation is big (fewer TLB misses and page
    // faults); a small one's pages stay small: a huge page's fault zeroes
    // 2 MiB, which many small compilers at once pay for in the kernel
    bool advise = block_bytes() > (std::size_t{64} << 20);
    char* blk = mapped ? huge_block(sz, advise) : static_cast<char*>(std::malloc(sz));
    if (!blk) throw std::bad_alloc();
    blocks_.emplace_back(blk, Free{mapped ? sz : 0});
    cur_ = blk;
    ranges_.emplace(cur_, sz);
    cap_ = sz;
    off_ = n;
    return cur_;
  }
  static constexpr std::size_t kBlock = 64 << 10;
  static constexpr std::size_t kBigBlock = 8 << 20;
  static constexpr std::size_t kSmallBlocks = 8;
  struct Free {  // a huge block (size > 0) is the kernel's mapping
    std::size_t huge = 0;
    void operator()(char* p) const;
  };
  std::vector<std::unique_ptr<char, Free>> blocks_;
  std::map<const char*, std::size_t> ranges_;  // block start -> size
  char* cur_ = nullptr;
  std::size_t cap_ = 0, off_ = 0;
  bool fixed_ = false;
  bool mmap_all_ = false;
  std::vector<std::pair<void*, void (*)(void*)>> dtors_;
};

// The zone new typing objects are allocated in.  Loading a cmi and typing a
// unit each install their own (ZoneScope).
Zone& zone();
void set_zone(Zone* z);
// Never dropped: process-long singletons (Predef's idents and types, the
// shared constant descs) are allocated here, whatever zone is current.
Zone& permanent_zone();
// the transient zones alive (the flambda middle end registers its pass
// zones here while they exist)
std::vector<const Zone*>& transient_zones();
// [s] itself, or a copy in the permanent zone where it is in a transient
// zone (an identifier's name outlives it)
std::string_view keep_str(std::string_view s);
struct ZoneScope {
  Zone* saved;
  explicit ZoneScope(Zone& z);
  ~ZoneScope();
};
// While a scratch zone is current for temporaries (Matching's patterns), the
// zone the types live in: what outlives the scratch -- an abbreviation's
// memorized expansion, Ctype's [abbreviations] -- is allocated there
// (TypesZoneScope, around the expansion of an abbreviation).
extern Zone* g_types_zone;
struct TypesZoneScope {
  Zone* saved = nullptr;
  TypesZoneScope() {
    if (g_types_zone && g_types_zone != &zone()) {
      saved = &zone();
      set_zone(g_types_zone);
    }
  }
  ~TypesZoneScope() {
    if (saved) set_zone(saved);
  }
  TypesZoneScope(const TypesZoneScope&) = delete;
  TypesZoneScope& operator=(const TypesZoneScope&) = delete;
};

template <class T, class... A>
T* make(A&&... a) {
  return zone().make<T>(std::forward<A>(a)...);
}

// An immutable zone-owned array: an OCaml list inside an immutable value.
template <class T>
struct Slice {
  static_assert(std::is_trivially_destructible_v<T>, "Slice holds plain values");
  const T* p = nullptr;
  std::size_t n = 0;
  std::size_t size() const { return n; }
  bool empty() const { return n == 0; }
  const T& operator[](std::size_t i) const { return p[i]; }
  const T* begin() const { return p; }
  const T* end() const { return p + n; }
  const T& front() const { return p[0]; }
  const T& back() const { return p[n - 1]; }
};

template <class T>
Slice<T> slice(const std::vector<T>& v) {
  if (v.empty()) return {};
  T* p = static_cast<T*>(zone().alloc(sizeof(T) * v.size(), alignof(T)));
  std::uninitialized_copy(v.begin(), v.end(), p);
  return {p, v.size()};
}
// ... in the given zone
template <class T>
Slice<T> slice_in(Zone& z, const std::vector<T>& v) {
  if (v.empty()) return {};
  T* p = static_cast<T*>(z.alloc(sizeof(T) * v.size(), alignof(T)));
  std::uninitialized_copy(v.begin(), v.end(), p);
  return {p, v.size()};
}
// a Slice is already the list: the same list (its identity kept)
template <class T>
Slice<T> slice(Slice<T> s) {
  return s;
}
template <class T>
Slice<T> slice(std::initializer_list<T> l) {
  return slice(std::vector<T>(l));
}

inline std::string_view zstr(std::string_view s) { return zone().str(s); }
// zborrow: [s] itself when it already lives in a zone (the zones are
// process-long), else a zone copy.  OCaml passes a string by reference where
// the port takes a string_view, so borrowing keeps one string one object --
// which the .cmi writer turns back into Marshal's sharing.
std::string_view zborrow(std::string_view s);

// An OCaml string literal: one static object, and ocamlopt (which built the
// reference ocamlc.opt) merges the equal immutable string constants of a
// compilation unit -- so one object per (ported unit, content), the unit
// being the C++ file that ports it (OCAML_LIT's __FILE__).  Where the object
// reaches marshaled output (ident names in the -g debug events), its sharing
// is Marshal's.
std::string_view ocaml_literal(const char* unit, std::string_view s);
// (each use site looks its literal up once: the interned object is permanent)
#define OCAML_LIT(s)                                                                  \
  ([]() -> std::string_view {                                                         \
    static const std::string_view ocaml_lit_ = ::cppcaml::typing::ocaml_literal(__FILE__, s); \
    return ocaml_lit_;                                                                \
  }())

// ---------------------------------------------------------------------------
// A persistent balanced map: stdlib map.ml's AVL, ported.  `Cmp` is a
// functor returning <0 / 0 / >0 like OCaml's `compare`.  Nodes are
// zone-allocated and immutable; an empty map is nullptr.  StrMap is
// Misc.Stdlib.String.Map (Types.Meths / Vars); Path.Map lives in path.hpp.

template <class K, class V>
struct PMapNode {
  const PMapNode* l;
  K v;
  V d;
  const PMapNode* r;
  int h;
};

template <class K, class V, class Cmp>
class PMap {
 public:
  using Node = PMapNode<K, V>;
  PMap() = default;
  explicit PMap(const Node* t) : t_(t) {}
  const Node* root() const { return t_; }
  bool is_empty() const { return t_ == nullptr; }
  bool same_as(const PMap& o) const { return t_ == o.t_; }

  const V* find_opt(const K& k) const {
    for (const Node* n = t_; n;) {
      int c = Cmp{}(k, n->v);
      if (c == 0) return &n->d;
      n = c < 0 ? n->l : n->r;
    }
    return nullptr;
  }
  bool mem(const K& k) const { return find_opt(k) != nullptr; }
  PMap add(const K& k, const V& d) const { return PMap(add_(k, d, t_)); }
  PMap remove(const K& k) const { return PMap(remove_(k, t_)); }

  // in increasing key order (Map.iter / Map.fold)
  template <class F>
  void iter(F&& f) const { iter_(t_, f); }
  std::vector<std::pair<K, V>> bindings() const {
    std::vector<std::pair<K, V>> out;
    iter([&](const K& k, const V& d) { out.emplace_back(k, d); });
    return out;
  }
  template <class F>
  PMap map(F&& f) const { return PMap(map_(t_, f)); }

  static const Node* create(const Node* l, const K& x, const V& d, const Node* r) {
    int hl = height(l), hr = height(r);
    return make<Node>(l, x, d, r, hl >= hr ? hl + 1 : hr + 1);
  }
  // A balanced tree of bindings sorted by key, distinct keys (n nodes where
  // n adds make about n log n)
  static PMap of_sorted(const std::vector<std::pair<K, V>>& b) { return PMap(of_sorted_(b, 0, b.size())); }
  int height() const { return height(t_); }
  // Rebuild a marshaled node exactly (the cmi decoder).
  static const Node* node(const Node* l, const K& x, const V& d, const Node* r, int h) {
    return make<Node>(l, x, d, r, h);
  }

 private:
  static int height(const Node* n) { return n ? n->h : 0; }
  static const Node* of_sorted_(const std::vector<std::pair<K, V>>& b, std::size_t lo, std::size_t hi) {
    if (lo >= hi) return nullptr;
    std::size_t mid = lo + (hi - lo) / 2;
    const Node* l = of_sorted_(b, lo, mid);
    const Node* r = of_sorted_(b, mid + 1, hi);
    return create(l, b[mid].first, b[mid].second, r);
  }
  static const Node* bal(const Node* l, const K& x, const V& d, const Node* r) {
    int hl = height(l), hr = height(r);
    if (hl > hr + 2) {
      const Node* ll = l->l; const Node* lr = l->r;
      if (height(ll) >= height(lr)) return create(ll, l->v, l->d, create(lr, x, d, r));
      return create(create(ll, l->v, l->d, lr->l), lr->v, lr->d, create(lr->r, x, d, r));
    }
    if (hr > hl + 2) {
      const Node* rl = r->l; const Node* rr = r->r;
      if (height(rr) >= height(rl)) return create(create(l, x, d, rl), r->v, r->d, rr);
      return create(create(l, x, d, rl->l), rl->v, rl->d, create(rl->r, r->v, r->d, rr));
    }
    return create(l, x, d, r);
  }
  static const Node* add_(const K& x, const V& data, const Node* m) {
    if (!m) return make<Node>(nullptr, x, data, nullptr, 1);
    int c = Cmp{}(x, m->v);
    if (c == 0) return make<Node>(m->l, x, data, m->r, m->h);
    if (c < 0) return bal(add_(x, data, m->l), m->v, m->d, m->r);
    return bal(m->l, m->v, m->d, add_(x, data, m->r));
  }
  static const Node* min_binding(const Node* t) {
    while (t->l) t = t->l;
    return t;
  }
  static const Node* remove_min_binding(const Node* t) {
    if (!t->l) return t->r;
    return bal(remove_min_binding(t->l), t->v, t->d, t->r);
  }
  static const Node* merge(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    const Node* m = min_binding(t2);
    return bal(t1, m->v, m->d, remove_min_binding(t2));
  }
  static const Node* remove_(const K& x, const Node* m) {
    if (!m) return nullptr;
    int c = Cmp{}(x, m->v);
    if (c == 0) return merge(m->l, m->r);
    if (c < 0) {
      const Node* ll = remove_(x, m->l);
      return ll == m->l ? m : bal(ll, m->v, m->d, m->r);
    }
    const Node* rr = remove_(x, m->r);
    return rr == m->r ? m : bal(m->l, m->v, m->d, rr);
  }
  template <class F>
  static void iter_(const Node* n, F& f) {
    if (!n) return;
    iter_(n->l, f);
    f(n->v, n->d);
    iter_(n->r, f);
  }
  template <class F>
  static const Node* map_(const Node* n, F& f) {
    if (!n) return nullptr;
    const Node* l = map_(n->l, f);
    V d = f(n->d);
    const Node* r = map_(n->r, f);
    return make<Node>(l, n->v, d, r, n->h);
  }
  const Node* t_ = nullptr;
};

struct StrCmp {
  int operator()(std::string_view a, std::string_view b) const {
    int c = a.compare(b);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
};
template <class V>
using StrMap = PMap<std::string_view, V, StrCmp>;
template <class V>
using StrMapNode = PMapNode<std::string_view, V>;

}  // namespace cppcaml::typing
