// A hash map for the writers' memo tables: open addressing (linear probing,
// power-of-two capacity) over an index, the entries themselves in a deque so
// their addresses stay put while the table grows -- the writers register an
// entry, then recurse and insert more before filling it in.  The subset of
// std::map / std::unordered_map they use: find / end / try_emplace /
// emplace / operator[] / count / size / clear.  No ordered iteration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
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
  template <class K>
  std::uint64_t operator()(const K& k) const { return h(k); }
};

template <class K, class V>
class FlatMap {
 public:
  using value_type = std::pair<const K, V>;
  using iterator = value_type*;  // nullptr = end()

  iterator end() const { return nullptr; }
  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  void clear() {
    entries_.clear();
    slots_.clear();
  }

  template <class Q>
  iterator find(const Q& k) const {
    if (slots_.empty()) return nullptr;
    std::size_t mask = slots_.size() - 1;
    for (std::size_t i = MemoHash{}(k) & mask;; i = (i + 1) & mask) {
      value_type* e = slots_[i];
      if (!e) return nullptr;
      if (e->first == k) return e;
    }
  }
  template <class Q>
  std::size_t count(const Q& k) const { return find(k) ? 1 : 0; }

  template <class Q, class... A>
  std::pair<iterator, bool> try_emplace(Q&& k, A&&... a) {
    if (iterator e = find(k)) return {e, false};
    if (4 * (entries_.size() + 1) > 3 * slots_.size()) grow();
    entries_.emplace_back(std::piecewise_construct, std::forward_as_tuple(std::forward<Q>(k)),
                          std::forward_as_tuple(std::forward<A>(a)...));
    value_type* e = &entries_.back();
    place(e);
    return {e, true};
  }
  template <class Q, class W>
  std::pair<iterator, bool> emplace(Q&& k, W&& v) {
    return try_emplace(std::forward<Q>(k), std::forward<W>(v));
  }
  template <class Q>
  V& operator[](Q&& k) { return try_emplace(std::forward<Q>(k)).first->second; }

 private:
  void place(value_type* e) {
    std::size_t mask = slots_.size() - 1;
    std::size_t i = MemoHash{}(e->first) & mask;
    while (slots_[i]) i = (i + 1) & mask;
    slots_[i] = e;
  }
  void grow() {
    slots_.assign(slots_.empty() ? 64 : 2 * slots_.size(), nullptr);
    for (value_type& e : entries_) place(&e);
  }
  std::deque<value_type> entries_;
  std::vector<value_type*> slots_;
};

}  // namespace cppcaml
