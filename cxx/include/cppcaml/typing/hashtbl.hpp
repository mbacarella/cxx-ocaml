// OCaml's polymorphic Hashtbl (stdlib/hashtbl.ml, not randomized: seed 0)
// with Hashtbl.hash (runtime/hash.c: caml_hash 10 100 0), for the tables
// whose iteration order reaches output (Linkdeps' error reports): the
// buckets, their growth and the order within a bucket are OCaml's.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cppcaml::typing::hashtbl {

// ---- caml_hash, on the values the tables use -----------------------------------

namespace detail {
inline std::uint32_t rotl(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline std::uint32_t mix_uint32(std::uint32_t h, std::uint32_t d) {
  d *= 0xcc9e2d51U;
  d = rotl(d, 15);
  d *= 0x1b873593U;
  h ^= d;
  h = rotl(h, 13);
  return h * 5 + 0xe6546b64U;
}
inline std::uint32_t mix_string(std::uint32_t h, const std::string& s) {
  std::size_t len = s.size(), i = 0;
  auto byte = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[k])); };
  for (; i + 4 <= len; i += 4) h = mix_uint32(h, byte(i) | (byte(i + 1) << 8) | (byte(i + 2) << 16) | (byte(i + 3) << 24));
  std::uint32_t w = 0;
  switch (len & 3) {
    case 3: w = byte(i + 2) << 16; [[fallthrough]];
    case 2: w |= byte(i + 1) << 8; [[fallthrough]];
    case 1:
      w |= byte(i);
      h = mix_uint32(h, w);
      break;
    default: break;
  }
  return h ^ static_cast<std::uint32_t>(len);
}
// caml_hash_mix_intnat on a 64-bit value (the tagged representation)
inline std::uint32_t mix_intnat(std::uint32_t h, std::int64_t d) {
  std::int64_t n = (d >> 32) ^ (d >> 63) ^ d;
  return mix_uint32(h, static_cast<std::uint32_t>(n));
}
inline long final_mix(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return static_cast<long>(h & 0x3FFFFFFFU);
}
}  // namespace detail

// Hashtbl.hash on a string
inline long hash_string(const std::string& s) { return detail::final_mix(detail::mix_string(0, s)); }

// Hashtbl.hash on a record (tag 0 block) of strings, e.g. { compunit;
// filename }: the header (size lsl 10 lor tag, not counted), then the
// fields in order
inline long hash_string_record(const std::vector<const std::string*>& fields) {
  std::uint32_t h = detail::mix_uint32(0, static_cast<std::uint32_t>(fields.size() << 10));
  for (const std::string* f : fields) h = detail::mix_string(h, *f);
  return detail::final_mix(h);
}

// ---- Hashtbl.t ---------------------------------------------------------------------

// Hash: long operator()(const K&) (Hashtbl.hash); keys compare with ==
// (compare k k' = 0).
template <class K, class V, class Hash>
class Hashtbl {
 public:
  struct Cell {
    K key;
    V data;
    Cell* next;
  };

  // create n: power_2_above 16 n buckets
  explicit Hashtbl(long n) {
    long s = 16;
    while (s < n) s *= 2;
    data_.assign(static_cast<std::size_t>(s), nullptr);
  }
  Hashtbl(const Hashtbl&) = delete;
  Hashtbl& operator=(const Hashtbl&) = delete;

  long length() const { return size_; }
  // clear: the bindings dropped, the bucket array kept (not reset)
  void clear() {
    if (size_ > 0) {
      size_ = 0;
      for (Cell*& c : data_) c = nullptr;
      cells_.clear();
    }
  }

  void add(const K& key, const V& data) {
    std::size_t i = index(key);
    data_[i] = alloc(key, data, data_[i]);
    grow();
  }
  void remove(const K& key) {
    std::size_t i = index(key);
    Cell* prec = nullptr;
    for (Cell* c = data_[i]; c; prec = c, c = c->next)
      if (c->key == key) {
        --size_;
        (prec ? prec->next : data_[i]) = c->next;
        return;
      }
  }
  const V* find_opt(const K& key) const {
    for (Cell* c = data_[index(key)]; c; c = c->next)
      if (c->key == key) return &c->data;
    return nullptr;
  }
  bool mem(const K& key) const { return find_opt(key) != nullptr; }
  // replace h key data: the first binding of key rebound in place, else a
  // new binding at the head of its bucket
  void replace(const K& key, const V& data) {
    std::size_t i = index(key);
    for (Cell* c = data_[i]; c; c = c->next)
      if (c->key == key) {
        c->key = key;
        c->data = data;
        return;
      }
    data_[i] = alloc(key, data, data_[i]);
    grow();
  }
  // to_seq: the buckets from index 0, each bucket's cells in order
  std::vector<std::pair<K, V>> to_seq() const {
    std::vector<std::pair<K, V>> r;
    for (Cell* c : data_)
      for (; c; c = c->next) r.emplace_back(c->key, c->data);
    return r;
  }

 private:
  std::size_t index(const K& key) const { return static_cast<std::size_t>(Hash{}(key)) & (data_.size() - 1); }
  Cell* alloc(const K& key, const V& data, Cell* next) {
    cells_.push_back(std::unique_ptr<Cell>(new Cell{key, data, next}));
    ++size_;
    return cells_.back().get();
  }
  void grow() {
    if (size_ > static_cast<long>(data_.size() * 2)) resize();
  }
  // resize / insert_all_buckets ~inplace:true: the cells keep their order
  // within each new bucket
  void resize() {
    std::size_t nsize = data_.size() * 2;
    std::vector<Cell*> ndata(nsize, nullptr), tail(nsize, nullptr);
    std::vector<Cell*> odata = std::move(data_);
    data_ = std::move(ndata);  // (so that index sees the new bucket count)
    for (Cell* c : odata) {
      while (c) {
        Cell* next = c->next;
        std::size_t k = index(c->key);
        (tail[k] ? tail[k]->next : data_[k]) = c;
        tail[k] = c;
        c = next;
      }
    }
    for (Cell* t : tail)
      if (t) t->next = nullptr;
  }

  std::vector<Cell*> data_;
  long size_ = 0;
  std::vector<std::unique_ptr<Cell>> cells_;
};

}  // namespace cppcaml::typing::hashtbl
