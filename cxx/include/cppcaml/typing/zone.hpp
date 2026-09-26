// Arena memory for the typing/ port (see TYPECHECKER.md, "Memory").
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

#include <cstddef>
#include <cstring>
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
  Zone(const Zone&) = delete;
  Zone& operator=(const Zone&) = delete;
  ~Zone() {
    for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) it->second(it->first);
  }

  void* alloc(std::size_t n, std::size_t align) {
    // new char[] storage is aligned for any fundamental type, so a fresh
    // block starts at offset 0 for every alignment we allocate.
    static_assert(alignof(std::max_align_t) >= 8);
    std::size_t off = (off_ + align - 1) & ~(align - 1);
    if (!cur_ || off + n > cap_) {
      std::size_t sz = n > kBlock ? n : kBlock;
      blocks_.emplace_back(new char[sz]);
      cur_ = blocks_.back().get();
      cap_ = sz;
      off = 0;
    }
    void* p = cur_ + off;
    off_ = off + n;
    return p;
  }

  template <class T, class... A>
  T* make(A&&... a) {
    void* p = alloc(sizeof(T), alignof(T));
    T* t = new (p) T{std::forward<A>(a)...};
    if constexpr (!std::is_trivially_destructible_v<T>)
      dtors_.emplace_back(t, [](void* q) { static_cast<T*>(q)->~T(); });
    return t;
  }

  std::string_view str(std::string_view s) {
    if (s.empty()) return {};
    char* p = static_cast<char*>(alloc(s.size(), 1));
    std::memcpy(p, s.data(), s.size());
    return {p, s.size()};
  }

 private:
  static constexpr std::size_t kBlock = 1 << 20;
  std::vector<std::unique_ptr<char[]>> blocks_;
  char* cur_ = nullptr;
  std::size_t cap_ = 0, off_ = 0;
  std::vector<std::pair<void*, void (*)(void*)>> dtors_;
};

// The zone new typing objects are allocated in.  Loading a cmi and typing a
// unit each install their own (ZoneScope).
Zone& zone();
void set_zone(Zone* z);
struct ZoneScope {
  Zone* saved;
  explicit ZoneScope(Zone& z);
  ~ZoneScope();
};

template <class T, class... A>
T* make(A&&... a) {
  return zone().make<T>(std::forward<A>(a)...);
}

// An immutable zone-owned array: an OCaml list inside an immutable value.
template <class T>
struct Slice {
  static_assert(std::is_trivially_copyable_v<T>, "Slice holds plain values");
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
  std::memcpy(static_cast<void*>(p), v.data(), sizeof(T) * v.size());
  return {p, v.size()};
}
template <class T>
Slice<T> slice(std::initializer_list<T> l) {
  return slice(std::vector<T>(l));
}

inline std::string_view zstr(std::string_view s) { return zone().str(s); }

// ---------------------------------------------------------------------------
// A persistent balanced map with string keys: stdlib map.ml's AVL, ported
// (Misc.Stdlib.String.Map -- Types.Meths / Types.Vars, Env's tables).
// Nodes are zone-allocated and immutable; an empty map is nullptr.

template <class V>
struct StrMapNode {
  const StrMapNode* l;
  std::string_view v;
  V d;
  const StrMapNode* r;
  int h;
};

template <class V>
class StrMap {
 public:
  using Node = StrMapNode<V>;
  StrMap() = default;
  explicit StrMap(const Node* t) : t_(t) {}
  const Node* root() const { return t_; }
  bool is_empty() const { return t_ == nullptr; }

  const V* find_opt(std::string_view k) const {
    for (const Node* n = t_; n;) {
      int c = k.compare(n->v);
      if (c == 0) return &n->d;
      n = c < 0 ? n->l : n->r;
    }
    return nullptr;
  }
  bool mem(std::string_view k) const { return find_opt(k) != nullptr; }
  StrMap add(std::string_view k, const V& d) const { return StrMap(add_(k, d, t_)); }

  template <class F>
  void iter(F&& f) const { iter_(t_, f); }
  std::vector<std::pair<std::string_view, V>> bindings() const {
    std::vector<std::pair<std::string_view, V>> out;
    iter([&](std::string_view k, const V& d) { out.emplace_back(k, d); });
    return out;
  }

  // map.ml `create` / `bal`, used by the cmi decoder to rebuild a marshaled
  // tree node by node (the shape is kept exactly).
  static const Node* create(const Node* l, std::string_view x, const V& d,
                            const Node* r) {
    int hl = height(l), hr = height(r);
    return make<Node>(l, x, d, r, hl >= hr ? hl + 1 : hr + 1);
  }
  static const Node* node(const Node* l, std::string_view x, const V& d,
                          const Node* r, int h) {
    return make<Node>(l, x, d, r, h);
  }

 private:
  static int height(const Node* n) { return n ? n->h : 0; }
  static const Node* bal(const Node* l, std::string_view x, const V& d,
                         const Node* r) {
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
  static const Node* add_(std::string_view x, const V& data, const Node* m) {
    if (!m) return make<Node>(nullptr, x, data, nullptr, 1);
    int c = x.compare(m->v);
    if (c == 0) return make<Node>(m->l, x, data, m->r, m->h);
    if (c < 0) return bal(add_(x, data, m->l), m->v, m->d, m->r);
    return bal(m->l, m->v, m->d, add_(x, data, m->r));
  }
  template <class F>
  static void iter_(const Node* n, F& f) {
    if (!n) return;
    iter_(n->l, f);
    f(n->v, n->d);
    iter_(n->r, f);
  }
  const Node* t_ = nullptr;
};

}  // namespace cppcaml::typing
