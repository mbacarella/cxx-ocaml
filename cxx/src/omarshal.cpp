#include <cstdlib>
#include "cppcaml/omarshal.hpp"

#include <cstring>
#include <unordered_map>

namespace cppcaml::omarshal {

//   A marshalled block header carries the source object's COLOUR bits as well
// as its size and tag: extern.c builds it with `Make_header(sz, tag,
// NOT_MARKABLE)` (extern.c:610), and NOT_MARKABLE is `3 << HEADER_COLOR_SHIFT`
// = 0x300 (shared_heap.h:75).  Writing 0 there is read back identically -- the
// intern side masks the colour off -- but it is not the same BYTES, which is
// what a linked executable is compared on.
// NOMARSHALHDR reverts both this and the custom-block word accounting below.
inline std::uint32_t hdr_color() {
  static const std::uint32_t c = std::getenv("NOMARSHALHDR") ? 0u : 3u << 8;
  return c;
}

ValPtr vint(long long n) { auto v = std::make_shared<Value>(); v->k = Value::Int; v->i = n; return v; }
ValPtr vstr(std::string s) { auto v = std::make_shared<Value>(); v->k = Value::Str; v->s = std::move(s); return v; }
ValPtr vdbl(double d) { auto v = std::make_shared<Value>(); v->k = Value::Dbl; v->d = d; return v; }
ValPtr vblock(int tag, std::vector<ValPtr> f) {
  auto v = std::make_shared<Value>(); v->k = Value::Block; v->tag = tag; v->fields = std::move(f); return v;
}
ValPtr vdblarr(std::vector<double> ds) {
  auto v = std::make_shared<Value>(); v->k = Value::DblArr; v->darr = std::move(ds); return v;
}
ValPtr vcustom(std::string raw, long long data_bytes) {
  auto v = std::make_shared<Value>(); v->k = Value::Custom; v->s = std::move(raw);
  v->custom_bytes = data_bytes; return v;
}
ValPtr vcustom2(std::string raw, long long bytes32, long long bytes64) {
  auto v = std::make_shared<Value>(); v->k = Value::Custom; v->s = std::move(raw);
  v->custom_bytes = bytes64; v->custom_bytes32 = bytes32; return v;
}
ValPtr vlist(const std::vector<ValPtr>& xs) {
  ValPtr acc = vint(0);  // []
  for (auto it = xs.rbegin(); it != xs.rend(); ++it) acc = vblock(0, {*it, acc});  // hd :: tl
  return acc;
}

namespace {
struct Marshaler {
  std::vector<std::uint8_t> body;
  long long nobjs = 0, w32 = 0, w64 = 0;
  // Each sharable object (block size>0 / string / double / dblarr / custom) is
  // assigned its emit-order index in `seen` so a repeat emits a CODE_SHARED
  // back-reference instead of re-serializing -- required for the shared / cyclic
  // type_expr graphs in a signature (otherwise type vars duplicate or recursive
  // types loop forever).  Registration happens BEFORE a block's fields so a
  // field may point back at the block itself (cycles).
  std::unordered_map<const Value*, long long> seen;
  void byte(int b) { body.push_back((std::uint8_t)b); }
  void be32(std::uint32_t n) { byte(n >> 24); byte(n >> 16); byte(n >> 8); byte(n); }
  void emit_shared(long long dist) {  // back-distance to the target object
    if (dist < 0x100) { byte(0x4); byte((int)dist); }                  // CODE_SHARED8
    else if (dist < 0x10000) { byte(0x5); byte(dist >> 8); byte(dist); }  // SHARED16
    else { byte(0x6); be32((std::uint32_t)dist); }                     // SHARED32
  }
  void emit_int(long long n) {
    if (n >= 0 && n < 0x40) byte(0x40 | (int)n);
    else if (n >= -128 && n < 128) { byte(0x0); byte((int)n & 0xFF); }
    else if (n >= -32768 && n < 32768) { byte(0x1); byte(n >> 8); byte(n); }
    // extern.c: CODE_INT32 only for a 31-bit int (the 32-bit platforms' range)
    else if (n >= -(1LL << 30) && n < (1LL << 30)) { byte(0x2); be32((std::uint32_t)n); }
    else { byte(0x3); for (int s = 56; s >= 0; s -= 8) byte(n >> s); }
  }
  void emit_str(const std::string& s) {
    size_t len = s.size();
    if (len < 0x20) byte(0x20 | (int)len);
    else if (len < 256) { byte(0x9); byte((int)len); }
    else { byte(0xA); be32((std::uint32_t)len); }
    for (char c : s) byte((std::uint8_t)c);
    nobjs++; w64 += 1 + (len / 8 + 1); w32 += 1 + (len / 4 + 1);
  }
  void emit(const ValPtr& v) {
    // immediates are never registered for sharing
    if (v->k == Value::Int) { emit_int(v->i); return; }
    // A zero-size block is an atom: never registered for sharing.  Tags < 16 use
    // the packed small-block byte; tag >= 16 must use CODE_BLOCK32, else the tag
    // bits spill into the size nibble and the reader sees a non-empty block.
    if (v->k == Value::Block && v->fields.empty()) {
      if (v->tag < 16) byte(0x80 | v->tag);
      else { byte(0x8); be32(hdr_color() | (std::uint32_t)v->tag); }   // size 0
      return;
    }
    // a sharable object already serialized -> a back-reference (objs[nobjs-dist])
    if (auto it = seen.find(v.get()); it != seen.end()) { emit_shared(nobjs - it->second); return; }
    // An Int is an IMMEDIATE: it takes no object slot, so registering it would
    // file the NEXT object's index under it and a second use of the same
    // ValPtr would emit a back-reference to the wrong object.
    if (v->k != Value::Int)
      seen[v.get()] = nobjs;  // index assigned before this object's own nobjs++
    switch (v->k) {
      case Value::Int: emit_int(v->i); return;
      case Value::Str: emit_str(v->s); return;
      case Value::Dbl: {
        byte(0xC);  // CODE_DOUBLE_LITTLE
        std::uint64_t bits; std::memcpy(&bits, &v->d, 8);
        for (int s = 0; s < 64; s += 8) byte(bits >> s);
        nobjs++; w64 += 2; w32 += 3;
        return;
      }
      case Value::Block: {
        int size = (int)v->fields.size();
        if (v->tag < 16 && size < 8) byte(0x80 | v->tag | (size << 4));
        else { byte(0x8);
               be32(((std::uint32_t)size << 10) | hdr_color() |
                    (std::uint32_t)v->tag); }
        nobjs++; w64 += 1 + size; w32 += 1 + size;
        for (auto& f : v->fields) emit(f);
        return;
      }
      case Value::DblArr: {
        std::size_t n = v->darr.size();
        if (n < 0x100) { byte(0xE); byte((int)n); }       // DOUBLE_ARRAY8_LITTLE
        else { byte(0x7); be32((std::uint32_t)n); }       // DOUBLE_ARRAY32_LITTLE
        for (double dd : v->darr) {
          std::uint64_t bits; std::memcpy(&bits, &dd, 8);
          for (int s = 0; s < 64; s += 8) byte(bits >> s);
        }
        nobjs++; w64 += 1 + n; w32 += 1 + 2 * n;
        return;
      }
      case Value::Custom: {  // verbatim on-disk custom bytes (incl. its code byte)
        for (char c : v->s) byte((std::uint8_t)c);
        nobjs++;                                  // extern.c:858 (header + ops)
        if (std::getenv("NOMARSHALHDR")) {
          long long words = 1 + (v->custom_bytes + 7) / 8;
          w32 += 1 + words; w64 += 1 + words;
        } else {
          w32 += 2 + (((v->custom_bytes32 >= 0 ? v->custom_bytes32 : v->custom_bytes) + 3) >> 2);
          w64 += 2 + ((v->custom_bytes + 7) >> 3);
        }
        return;
      }
    }
  }
};
}  // namespace

std::vector<std::uint8_t> marshal(const ValPtr& root) {
  Marshaler m;
  m.emit(root);
  std::vector<std::uint8_t> out;
  auto be = [&](std::uint32_t n) { out.push_back(n >> 24); out.push_back(n >> 16); out.push_back(n >> 8); out.push_back(n); };
  be(0x8495A6BE);
  be((std::uint32_t)m.body.size());
  be((std::uint32_t)m.nobjs);
  be((std::uint32_t)m.w32);
  be((std::uint32_t)m.w64);
  out.insert(out.end(), m.body.begin(), m.body.end());
  return out;
}

}  // namespace cppcaml::omarshal
