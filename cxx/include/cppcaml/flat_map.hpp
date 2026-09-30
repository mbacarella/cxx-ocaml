// A hash map for the writers' memo tables: open addressing (linear probing,
// power-of-two capacity), the entries in the slots themselves, a byte per
// slot beside them (occupied + 7 bits of the hash) so that a probe reads the
// dense control bytes and one entry.  An insertion may move every entry: a
// writer must not hold an entry across a call that inserts into the same
// map.  The subset of std::map / std::unordered_map they use: find / end /
// try_emplace / emplace / operator[] / count / size / clear.  No erasure,
// no iteration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace cppcaml {

// the hash of a memo key: pointers, integers, strings, pairs and tuples of them
struct MemoHash {
  static std::uint64_t mix(std::uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
  }
  static std::uint64_t combine(std::uint64_t a, std::uint64_t b) { return mix(a * 31 + b + 0x9e3779b97f4a7c15ULL); }
  template <class T>
  static std::uint64_t h(const T* p) { return mix(reinterpret_cast<std::uintptr_t>(p)); }
  static std::uint64_t h(std::nullptr_t) { return 0; }
  template <class T>
    requires std::is_integral_v<T> || std::is_enum_v<T>
  static std::uint64_t h(T x) { return mix(static_cast<std::uint64_t>(x)); }
  static std::uint64_t h(std::string_view s) { return std::hash<std::string_view>{}(s); }
  static std::uint64_t h(const std::string& s) { return h(std::string_view(s)); }
  template <class A, class B>
  static std::uint64_t h(const std::pair<A, B>& p) { return combine(h(p.first), h(p.second)); }
  template <class... T>
  static std::uint64_t h(const std::tuple<T...>& t) {
    std::uint64_t r = 0x12345;
    std::apply([&](const T&... x) { ((r = combine(r, h(x))), ...); }, t);
    return r;
  }
  template <class T>
  static std::uint64_t h(const std::vector<T>& v) {
    std::uint64_t r = v.size();
    for (const T& x : v) r = combine(r, h(x));
    return r;
  }
  template <class K>
  std::uint64_t operator()(const K& k) const { return h(k); }
};

template <class K, class V>
class FlatMap {
 public:
  using value_type = std::pair<const K, V>;
  using iterator = value_type*;  // nullptr = end()

  FlatMap() = default;
  FlatMap(const FlatMap&) = delete;
  FlatMap& operator=(const FlatMap&) = delete;
  FlatMap(FlatMap&& o) noexcept { take(o); }
  FlatMap& operator=(FlatMap&& o) noexcept {
    if (this != &o) {
      destroy();
      take(o);
    }
    return *this;
  }
  ~FlatMap() { destroy(); }

  iterator end() const { return nullptr; }
  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  void clear() { destroy(); }

  template <class Q>
  iterator find(const Q& k) const {
    if (!cap_) return nullptr;
    std::uint64_t h = MemoHash{}(k);
    std::uint8_t tag = tag_of(h);
    for (std::size_t i = h & (cap_ - 1);; i = (i + 1) & (cap_ - 1)) {
      std::uint8_t c = ctrl_[i];
      if (!c) return nullptr;
      if (c == tag && slots_[i].first == k) return &slots_[i];
    }
  }
  template <class Q>
  std::size_t count(const Q& k) const { return find(k) ? 1 : 0; }

  template <class Q, class... A>
  std::pair<iterator, bool> try_emplace(Q&& k, A&&... a) {
    if (4 * (size_ + 1) > 3 * cap_) grow();
    std::uint64_t h = MemoHash{}(k);
    std::uint8_t tag = tag_of(h);
    std::size_t i = h & (cap_ - 1);
    for (;; i = (i + 1) & (cap_ - 1)) {
      std::uint8_t c = ctrl_[i];
      if (!c) break;
      if (c == tag && slots_[i].first == k) return {&slots_[i], false};
    }
    new (&slots_[i]) value_type(std::piecewise_construct, std::forward_as_tuple(std::forward<Q>(k)),
                                std::forward_as_tuple(std::forward<A>(a)...));
    ctrl_[i] = tag;
    ++size_;
    return {&slots_[i], true};
  }
  template <class Q, class W>
  std::pair<iterator, bool> emplace(Q&& k, W&& v) {
    return try_emplace(std::forward<Q>(k), std::forward<W>(v));
  }
  template <class Q>
  V& operator[](Q&& k) { return try_emplace(std::forward<Q>(k)).first->second; }

 private:
  static std::uint8_t tag_of(std::uint64_t h) { return static_cast<std::uint8_t>(0x80 | (h >> 57)); }
  static value_type* alloc(std::size_t n) {
    return static_cast<value_type*>(::operator new(n * sizeof(value_type), std::align_val_t{alignof(value_type)}));
  }
  static void dealloc(value_type* p) { ::operator delete(p, std::align_val_t{alignof(value_type)}); }
  void grow() {
    std::size_t ncap = cap_ ? 2 * cap_ : 64;
    value_type* ns = alloc(ncap);
    std::uint8_t* nc = new std::uint8_t[ncap]();
    for (std::size_t j = 0; j < cap_; ++j) {
      if (!ctrl_[j]) continue;
      std::uint64_t h = MemoHash{}(slots_[j].first);
      std::size_t i = h & (ncap - 1);
      while (nc[i]) i = (i + 1) & (ncap - 1);
      new (&ns[i]) value_type(std::move(const_cast<K&>(slots_[j].first)), std::move(slots_[j].second));
      nc[i] = ctrl_[j];
      slots_[j].~value_type();
    }
    if (slots_) dealloc(slots_);
    delete[] ctrl_;
    slots_ = ns;
    ctrl_ = nc;
    cap_ = ncap;
  }
  void destroy() {
    if (!std::is_trivially_destructible_v<value_type>)
      for (std::size_t j = 0; j < cap_; ++j)
        if (ctrl_[j]) slots_[j].~value_type();
    if (slots_) dealloc(slots_);
    delete[] ctrl_;
    slots_ = nullptr;
    ctrl_ = nullptr;
    cap_ = size_ = 0;
  }
  void take(FlatMap& o) {
    slots_ = o.slots_;
    ctrl_ = o.ctrl_;
    cap_ = o.cap_;
    size_ = o.size_;
    o.slots_ = nullptr;
    o.ctrl_ = nullptr;
    o.cap_ = o.size_ = 0;
  }
  value_type* slots_ = nullptr;
  std::uint8_t* ctrl_ = nullptr;
  std::size_t cap_ = 0, size_ = 0;
};

}  // namespace cppcaml
