#include <algorithm>
#include <cstdlib>
#include "cppcaml/omarshal.hpp"

#include <cstdint>
#include <cstring>
#include <new>

namespace cppcaml::omarshal {

//   A marshalled block header carries the source object's COLOUR bits as well
// as its size and tag: extern.c builds it with `Make_header(sz, tag,
// NOT_MARKABLE)` (extern.c:610), and NOT_MARKABLE is `3 << HEADER_COLOR_SHIFT`
// = 0x300 (shared_heap.h:75).  Writing 0 there is read back identically -- the
// intern side masks the colour off -- but it is not the same BYTES, which is
// what a linked executable is compared on.
constexpr std::uint32_t hdr_color() { return 3u << 8; }

// ---- the values' arena ------------------------------------------------------
void* arena_alloc(std::size_t n, std::size_t align) {
  static char* cur = nullptr;
  static std::size_t left = 0;
  constexpr std::size_t kBlock = 1 << 20;
  std::size_t pad = (align - reinterpret_cast<std::uintptr_t>(cur) % align) % align;
  if (!cur || pad + n > left) {
    std::size_t sz = n + align > kBlock ? n + align : kBlock;
    cur = static_cast<char*>(std::malloc(sz));
    if (!cur) throw std::bad_alloc();
    left = sz;
    pad = (align - reinterpret_cast<std::uintptr_t>(cur) % align) % align;
  }
  char* p = cur + pad;
  cur = p + n;
  left -= pad + n;
  return p;
}

static Value* new_value(Value::K k) {
  Value* v = new (arena_alloc(sizeof(Value), alignof(Value))) Value;
  v->k = k;
  return v;
}

// An int is an immediate, never shared: the small ones are allocated once.
ValPtr vint(long long n) {
  constexpr long long lo = -256, hi = 4096;
  static const std::vector<ValPtr> small = [] {
    std::vector<ValPtr> v;
    v.reserve(hi - lo);
    for (long long k = lo; k < hi; ++k) {
      Value* x = new_value(Value::Int);
      x->i = k;
      v.push_back(ValPtr(x));
    }
    return v;
  }();
  if (n >= lo && n < hi) return small[static_cast<std::size_t>(n - lo)];
  Value* v = new_value(Value::Int);
  v->i = n;
  return ValPtr(v);
}
ValPtr vstr(std::string s) {
  Value* v = new_value(Value::Str);
  v->s = std::move(s);
  return ValPtr(v);
}
ValPtr vdbl(double d) {
  Value* v = new_value(Value::Dbl);
  v->d = d;
  return ValPtr(v);
}
ValPtr vblock(int tag, std::vector<ValPtr> f) {
  Value* v = new_value(Value::Block);
  v->tag = tag;
  v->fields = f;
  return ValPtr(v);
}
ValPtr vblock(int tag, std::initializer_list<ValPtr> f) {
  Value* v = new_value(Value::Block);
  v->tag = tag;
  v->fields = f;
  return ValPtr(v);
}
ValPtr vdblarr(std::vector<double> ds) {
  Value* v = new_value(Value::DblArr);
  v->darr = ds;
  return ValPtr(v);
}
ValPtr vcustom(std::string raw, long long data_bytes) {
  Value* v = new_value(Value::Custom);
  v->s = std::move(raw);
  v->custom_bytes = data_bytes;
  return ValPtr(v);
}
ValPtr vcustom2(std::string raw, long long bytes32, long long bytes64) {
  Value* v = new_value(Value::Custom);
  v->s = std::move(raw);
  v->custom_bytes = bytes64;
  v->custom_bytes32 = bytes32;
  return ValPtr(v);
}
ValPtr vlist(const std::vector<ValPtr>& xs) {
  ValPtr acc = vint(0);  // []
  for (auto it = xs.rbegin(); it != xs.rend(); ++it) acc = vblock(0, {*it, acc});  // hd :: tl
  return acc;
}

namespace {
struct Marshaler {
  // the output, the 20-byte header's room first (filled in at the end)
  std::vector<std::uint8_t> out = std::vector<std::uint8_t>(kHeader);
  std::size_t len = kHeader;
  static constexpr std::size_t kHeader = 20;
  long long nobjs = 0, w32 = 0, w64 = 0;
  std::uint8_t* room(std::size_t n) {  // n bytes at the end
    if (len + n > out.size()) out.resize(std::max(out.size() * 2, len + n));
    std::uint8_t* p = out.data() + len;
    len += n;
    return p;
  }
  // Each sharable object (block size>0 / string / double / dblarr / custom) is
  // assigned its emit-order index in `seen` so a repeat emits a CODE_SHARED
  // back-reference instead of re-serializing -- required for the shared / cyclic
  // type_expr graphs in a signature (otherwise type vars duplicate or recursive
  // types loop forever).  Registration happens BEFORE a block's fields so a
  // field may point back at the block itself (cycles).
  // (the index lives in the object itself, stamped with this session)
  std::uint32_t session;
  void byte(int b) { *room(1) = static_cast<std::uint8_t>(b); }
  void bytes(const std::string& s) {
    if (!s.empty()) std::memcpy(room(s.size()), s.data(), s.size());
  }
  void be32(std::uint32_t n) {
    std::uint8_t* p = room(4);
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
  }
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
    bytes(s);
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
    if (v->seen_session == session) { emit_shared(nobjs - v->seen_index); return; }
    // An Int is an IMMEDIATE: it takes no object slot, so registering it would
    // file the NEXT object's index under it and a second use of the same
    // ValPtr would emit a back-reference to the wrong object.
    if (v->k != Value::Int) {  // index assigned before this object's own nobjs++
      v->seen_session = session;
      v->seen_index = nobjs;
    }
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
        bytes(v->s);
        nobjs++;                                  // extern.c:858 (header + ops)
        w32 += 2 + (((v->custom_bytes32 >= 0 ? v->custom_bytes32 : v->custom_bytes) + 3) >> 2);
        w64 += 2 + ((v->custom_bytes + 7) >> 3);
        return;
      }
    }
  }
};
}  // namespace

std::vector<std::uint8_t> marshal(const ValPtr& root) {
  static std::uint32_t sessions = 0;
  Marshaler m;
  m.session = ++sessions;
  m.emit(root);
  std::vector<std::uint8_t> out = std::move(m.out);
  out.resize(m.len);
  std::uint8_t* h = out.data();
  auto be = [&](std::uint32_t n) { h[0] = n >> 24; h[1] = n >> 16; h[2] = n >> 8; h[3] = n; h += 4; };
  be(0x8495A6BE);
  be(static_cast<std::uint32_t>(m.len - Marshaler::kHeader));
  be(static_cast<std::uint32_t>(m.nobjs));
  be(static_cast<std::uint32_t>(m.w32));
  be(static_cast<std::uint32_t>(m.w64));
  return out;
}

}  // namespace cppcaml::omarshal
