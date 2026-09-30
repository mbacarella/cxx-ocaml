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

namespace cppcaml::typing {

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
  // transparent huge pages (Free unmaps it)
  static char* huge_block(std::size_t sz);

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
    char* blk = mapped ? huge_block(sz) : static_cast<char*>(std::malloc(sz));
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
  // Rebuild a marshaled node exactly (the cmi decoder).
  static const Node* node(const Node* l, const K& x, const V& d, const Node* r, int h) {
    return make<Node>(l, x, d, r, h);
  }

 private:
  static int height(const Node* n) { return n ? n->h : 0; }
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
