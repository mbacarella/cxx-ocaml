#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdlib>
#include "cppcaml/omarshal.hpp"

#include <new>
#include <stdexcept>
#ifdef CPPCAML_HAVE_ZSTD
#include <zstd.h>
#endif

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
namespace {
struct Arena {
  char* cur = nullptr;
  std::size_t left = 0;
  std::vector<char*> blocks;
  std::vector<Value::Extra*> extras;  // (their strings own heap storage)
};
Arena& arena() {
  static Arena* a = new Arena;
  return *a;
}
}  // namespace

void* arena_alloc(std::size_t n, std::size_t align) {
  Arena& a = arena();
  constexpr std::size_t kBlock = 1 << 20;
  std::size_t pad = (align - reinterpret_cast<std::uintptr_t>(a.cur) % align) % align;
  if (!a.cur || pad + n > a.left) {
    std::size_t sz = n + align > kBlock ? n + align : kBlock;
    a.cur = static_cast<char*>(std::malloc(sz));
    if (!a.cur) throw std::bad_alloc();
    a.blocks.push_back(a.cur);
    a.left = sz;
    pad = (align - reinterpret_cast<std::uintptr_t>(a.cur) % align) % align;
  }
  char* p = a.cur + pad;
  a.cur = p + n;
  a.left -= pad + n;
  return p;
}

ArenaMark arena_mark() {
  Arena& a = arena();
  return ArenaMark{a.blocks.size(), a.extras.size(), a.cur, a.left};
}
void arena_release(const ArenaMark& m) {
  Arena& a = arena();
  for (std::size_t k = a.extras.size(); k-- > m.extras;) a.extras[k]->~Extra();
  a.extras.resize(m.extras);
  for (std::size_t k = m.blocks; k < a.blocks.size(); ++k) std::free(a.blocks[k]);
  a.blocks.resize(m.blocks);
  a.cur = m.cur;
  a.left = m.left;
}

Value::Extra& Value::extra() {
  if (!x) {
    x = new (arena_alloc(sizeof(Extra), alignof(Extra))) Extra;
    arena().extras.push_back(x);
  }
  return *x;
}
const std::string& Value::empty_str() {
  static const std::string e;
  return e;
}

static Value* new_value(Value::K k) {
  Value* v = new (arena_alloc(sizeof(Value), alignof(Value))) Value;
  v->k_ = k;
  return v;
}

// An int is an immediate (ValPtr), never shared, never allocated.
ValPtr vint(long long n) { return ValPtr::immediate(n); }
ValPtr vstr(std::string s) {
  Value* v = new_value(Value::Str);
  v->extra().s = std::move(s);
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
  v->extra().darr = ds;
  return ValPtr(v);
}
ValPtr vcustom(std::string raw, long long data_bytes) {
  Value* v = new_value(Value::Custom);
  v->extra().s = std::move(raw);
  v->extra().custom_bytes = data_bytes;
  return ValPtr(v);
}
ValPtr vcustom2(std::string raw, long long bytes32, long long bytes64) {
  Value* v = new_value(Value::Custom);
  v->extra().s = std::move(raw);
  v->extra().custom_bytes = bytes64;
  v->extra().custom_bytes32 = bytes32;
  return ValPtr(v);
}
ValPtr vlist(const std::vector<ValPtr>& xs) {
  ValPtr acc = vint(0);  // []
  for (auto it = xs.rbegin(); it != xs.rend(); ++it) acc = vblock(0, {*it, acc});  // hd :: tl
  return acc;
}

namespace {
#ifdef CPPCAML_HAVE_ZSTD
// zstd.c caml_zstd_compress: a default context, the output streamed with
// ZSTD_e_continue, then ZSTD_e_end on no more input -- the frame carries no
// content size.  (zstd's output does not depend on how the input is split
// into extern.c's blocks, or into ours.)
struct ZStream {
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  std::vector<std::uint8_t> body = std::vector<std::uint8_t>(1 << 20);
  ZSTD_outBuffer ob{body.data(), body.size(), 0};
  ZStream() {
    if (!ctx) throw std::bad_alloc();
  }
  ~ZStream() { ZSTD_freeCCtx(ctx); }
  ZStream(const ZStream&) = delete;
  ZStream& operator=(const ZStream&) = delete;
  void grow() {
    body.resize(body.size() * 2);
    ob.dst = body.data();
    ob.size = body.size();
  }
  void feed(const std::uint8_t* p, std::size_t n) {
    ZSTD_inBuffer in{p, n, 0};
    while (in.pos < in.size) {
      std::size_t rc = ZSTD_compressStream2(ctx, &ob, &in, ZSTD_e_continue);
      if (ZSTD_isError(rc)) throw std::runtime_error("output_value: compression error");
      if (ob.pos == ob.size) grow();
    }
  }
  void finish() {
    ZSTD_inBuffer none{nullptr, 0, 0};
    for (;;) {
      std::size_t rc = ZSTD_compressStream2(ctx, &ob, &none, ZSTD_e_end);
      if (ZSTD_isError(rc)) throw std::runtime_error("output_value: compression error");
      if (rc == 0) break;
      grow();
    }
  }
};
#endif

struct Marshaler {
  static constexpr std::size_t kHeader = 20;
  static constexpr std::size_t kChunk = 1 << 20;
  // the output, the 20-byte header's room first (filled in at the end); or,
  // compressing, the body's latest chunk (the whole body is never held)
  std::vector<std::uint8_t> out;
  std::size_t len;
#ifdef CPPCAML_HAVE_ZSTD
  ZStream* z = nullptr;
#endif
  std::size_t flushed = 0;  // (compressing) the body's bytes before `out`'s
  explicit Marshaler(bool compress) : out(compress ? kChunk : kHeader), len(compress ? 0 : kHeader) {}
  long long nobjs = 0, w32 = 0, w64 = 0;
  std::uint8_t* room(std::size_t n) {  // n bytes at the end
    if (len + n > out.size()) {
#ifdef CPPCAML_HAVE_ZSTD
      if (z) {
        flush();
        if (n > out.size()) out.resize(n);
      } else
#endif
        out.resize(std::max(out.size() * 2, len + n));
    }
    std::uint8_t* p = out.data() + len;
    len += n;
    return p;
  }
#ifdef CPPCAML_HAVE_ZSTD
  void flush() {
    z->feed(out.data(), len);
    flushed += len;
    len = 0;
  }
#endif
  // Each sharable object (block size>0 / string / double / dblarr / custom) is
  // assigned its emit-order index in `seen` so a repeat emits a CODE_SHARED
  // back-reference instead of re-serializing -- required for the shared / cyclic
  // type_expr graphs in a signature (otherwise type vars duplicate or recursive
  // types loop forever).  Registration happens BEFORE a block's fields so a
  // field may point back at the block itself (cycles).
  // (the index lives in the object itself, stamped with this session)
  std::uint32_t session;
  bool compressed = false;  // absolute shared references (extern.c #4056)
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
    if (v.is_int()) { emit_int(v.int_value()); return; }
    // A zero-size block is an atom: never registered for sharing.  Tags < 16 use
    // the packed small-block byte; tag >= 16 must use CODE_BLOCK32, else the tag
    // bits spill into the size nibble and the reader sees a non-empty block.
    if (v->k_ == Value::Block && v->fields.empty()) {
      if (v->tag < 16) byte(0x80 | v->tag);
      else { byte(0x8); be32(hdr_color() | (std::uint32_t)v->tag); }   // size 0
      return;
    }
    // a sharable object already serialized -> a back-reference (objs[nobjs-dist])
    if (v->seen_session == session) {
      emit_shared(compressed ? v->seen_index : nobjs - v->seen_index);
      return;
    }
    // (index assigned before this object's own nobjs++; an int returned above)
    v->seen_session = session;
    v->seen_index = nobjs;
    switch (v->k_) {
      case Value::Int: return;  // (an immediate: above)
      case Value::Str: emit_str(v->str()); return;
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
        std::size_t n = v->extra().darr.size();
        if (n < 0x100) { byte(0xE); byte((int)n); }       // DOUBLE_ARRAY8_LITTLE
        else { byte(0x7); be32((std::uint32_t)n); }       // DOUBLE_ARRAY32_LITTLE
        for (double dd : v->extra().darr) {
          std::uint64_t bits; std::memcpy(&bits, &dd, 8);
          for (int s = 0; s < 64; s += 8) byte(bits >> s);
        }
        nobjs++; w64 += 1 + n; w32 += 1 + 2 * n;
        return;
      }
      case Value::Custom: {  // verbatim on-disk custom bytes (incl. its code byte)
        bytes(v->str());
        nobjs++;                                  // extern.c:858 (header + ops)
        w32 += 2 + (((v->extra().custom_bytes32 >= 0 ? v->extra().custom_bytes32 : v->extra().custom_bytes) + 3) >> 2);
        w64 += 2 + ((v->extra().custom_bytes + 7) >> 3);
        return;
      }
    }
  }
};
}  // namespace

namespace {
// extern.c storevlq: base-128 digits, most significant first, all but the
// last with the high bit set
void storevlq(std::vector<std::uint8_t>& out, std::uint64_t n) {
  int ndigits = 1;
  for (std::uint64_t m = n >> 7; m != 0; m >>= 7) ndigits++;
  std::size_t at = out.size();
  out.resize(at + ndigits);
  std::uint8_t* dst = out.data() + at + ndigits - 1;
  *dst = n & 0x7F;
  for (n >>= 7; n != 0; n >>= 7) *--dst = 0x80 | (n & 0x7F);
}
}  // namespace

bool zstd_available() {
#ifdef CPPCAML_HAVE_ZSTD
  return true;
#else
  return false;
#endif
}

std::vector<std::uint8_t> marshal(const ValPtr& root, bool compressed) {
  static std::uint32_t sessions = 0;
  Marshaler m(compressed);
  m.session = ++sessions;
  m.compressed = compressed;
  if (compressed) {
#ifdef CPPCAML_HAVE_ZSTD
    ZStream zs;
    m.z = &zs;
    m.emit(root);
    m.flush();
    zs.finish();
    std::size_t uncompressed_len = m.flushed;
    ZSTD_outBuffer out = zs.ob;
    // the header in compressed format: magic, its own length, then the
    // compressed and uncompressed lengths, the object count, size_32, size_64
    std::vector<std::uint8_t> res = {0x84, 0x95, 0xA6, 0xBD, 0};
    storevlq(res, out.pos);
    storevlq(res, uncompressed_len);
    storevlq(res, m.nobjs);
    storevlq(res, m.w32);
    storevlq(res, m.w64);
    res[4] = static_cast<std::uint8_t>(res.size());
    res.insert(res.end(), zs.body.data(), zs.body.data() + out.pos);
    return res;
#else
    throw std::logic_error("omarshal: compressed output needs a c++ocamlc built with zstd");
#endif
  }
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
