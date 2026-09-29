#include "cppcaml/marshal.hpp"

#ifdef CPPCAML_HAVE_ZSTD
#include <zstd.h>
#endif
#include <cstdio>
#include <cstdlib>

#include <cstring>

namespace cppcaml::marshal {

namespace {

// Opcodes, mirroring runtime/caml/intext.h.
constexpr std::uint32_t MAGIC_SMALL = 0x8495A6BE;
constexpr std::uint32_t MAGIC_BIG = 0x8495A6BF;
constexpr std::uint32_t MAGIC_COMPRESSED = 0x8495A6BD;

constexpr int PREFIX_SMALL_BLOCK = 0x80;
constexpr int PREFIX_SMALL_INT = 0x40;
constexpr int PREFIX_SMALL_STRING = 0x20;
constexpr int CODE_INT8 = 0x0;
constexpr int CODE_INT16 = 0x1;
constexpr int CODE_INT32 = 0x2;
constexpr int CODE_INT64 = 0x3;
constexpr int CODE_SHARED8 = 0x4;
constexpr int CODE_SHARED16 = 0x5;
constexpr int CODE_SHARED32 = 0x6;
constexpr int CODE_SHARED64 = 0x14;
constexpr int CODE_BLOCK32 = 0x8;
constexpr int CODE_BLOCK64 = 0x13;
constexpr int CODE_STRING8 = 0x9;
constexpr int CODE_STRING32 = 0xA;
constexpr int CODE_STRING64 = 0x15;
constexpr int CODE_DOUBLE_BIG = 0xB;
constexpr int CODE_DOUBLE_LITTLE = 0xC;
constexpr int CODE_DOUBLE_ARRAY8_BIG = 0xD;
constexpr int CODE_DOUBLE_ARRAY8_LITTLE = 0xE;
constexpr int CODE_DOUBLE_ARRAY32_BIG = 0xF;
constexpr int CODE_DOUBLE_ARRAY32_LITTLE = 0x7;
constexpr int CODE_DOUBLE_ARRAY64_BIG = 0x16;
constexpr int CODE_DOUBLE_ARRAY64_LITTLE = 0x17;
constexpr int CODE_CODEPOINTER = 0x10;
constexpr int CODE_INFIXPOINTER = 0x11;
constexpr int OLD_CODE_CUSTOM = 0x12;
constexpr int CODE_CUSTOM_LEN = 0x18;
constexpr int CODE_CUSTOM_FIXED = 0x19;

// A cursor over the byte buffer, reading the big-endian integers the Marshal
// wire format uses, plus the running object table needed to resolve sharing.
class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t len, std::size_t off, Arena& arena)
      : data_(data), len_(len), pos_(off), arena_(arena) {}

  std::size_t read_root() {
    CompressedHeader ch;
    if (compressed_header(data_, len_, pos_, ch)) {
      // intern.c: decompress, then read with absolute shared references
      std::vector<std::uint8_t> buf = decompress(data_, len_, pos_, ch);
      std::size_t end = pos_ + ch.header_len + ch.data_len;
      const std::uint8_t* data = data_;
      std::size_t len = len_;
      data_ = buf.data(); len_ = buf.size(); pos_ = 0; data_end_ = buf.size();
      compressed_ = true;
      std::size_t root = read_value();
      data_ = data; len_ = len; pos_ = end; compressed_ = false;
      return root;
    }
    parse_header();
    std::size_t root = read_value();
    pos_ = data_end_;  // skip any trailing padding the header accounted for
    return root;
  }

  // Parse only the header and jump to the value's end, decoding nothing.
  std::size_t skip_root() {
    CompressedHeader ch;
    if (compressed_header(data_, len_, pos_, ch)) return pos_ = pos_ + ch.header_len + ch.data_len;
    parse_header();
    pos_ = data_end_;
    return pos_;
  }

  std::size_t pos() const { return pos_; }

private:
  // ---- raw byte access (big-endian) ----
  std::uint8_t u8() {
    if (pos_ >= len_) throw Error("marshal: unexpected end of input");
    return data_[pos_++];
  }
  std::uint32_t u16() {
    std::uint32_t a = u8(), b = u8();
    return (a << 8) | b;
  }
  std::uint32_t u32() {
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v = (v << 8) | u8();
    return v;
  }
  std::uint64_t u64() {
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v = (v << 8) | u8();
    return v;
  }

  void parse_header() {
    std::uint32_t magic = u32();
    std::uint64_t data_len;
    if (magic == MAGIC_SMALL) {
      data_len = u32();
      (void)u32();  // num_objects
      (void)u32();  // size_32
      (void)u32();  // size_64
    } else if (magic == MAGIC_BIG) {
      data_len = u64();
      (void)u64();  // num_objects
      (void)u64();  // size_64
    } else {
      throw Error("marshal: bad magic number");
    }
    data_end_ = pos_ + data_len;
  }

  // The object table records, in creation order, every heap-allocated object
  // (blocks of size>0, strings, doubles, double arrays, custom).  CODE_SHARED
  // back-references an earlier entry by relative distance.
  std::size_t mk_int(long long v) {
    std::size_t id = arena_.new_node();  // ints are not registered for sharing
    Value& val = arena_.at(id);          // Kind defaults to Int
    val.i = v;
    return id;
  }

  std::size_t mk_string(std::uint64_t n) {
    std::size_t id = arena_.new_node();
    Value& val = arena_.at(id);
    val.kind = Value::Kind::String;
    auto& s = val.ensure_extra().str;
    s.resize(n);
    for (std::uint64_t k = 0; k < n; ++k) s[k] = static_cast<char>(u8());
    objs_.push_back(id);
    return id;
  }

  double read_f64(bool little) {
    std::uint8_t bytes[8];
    for (int k = 0; k < 8; ++k) bytes[k] = u8();
    if (!little) {
      for (int k = 0; k < 4; ++k) std::swap(bytes[k], bytes[7 - k]);
    }
    double d;
    std::memcpy(&d, bytes, 8);
    return d;
  }

  std::size_t mk_double(bool little) {
    double d = read_f64(little);  // read before allocating (no ordering dep)
    std::size_t id = arena_.new_node();
    Value& val = arena_.at(id);
    val.kind = Value::Kind::Double;
    val.ensure_extra().d = d;
    objs_.push_back(id);
    return id;
  }

  std::size_t mk_double_array(std::uint64_t n, bool little) {
    // Reserve the arena slot/registration before reading elements, matching the
    // runtime's allocate-then-fill order.  read_f64 does not touch the arena, so
    // holding the node reference across the fill loop is safe.
    std::size_t id = arena_.new_node();
    Value& val = arena_.at(id);
    val.kind = Value::Kind::DoubleArray;
    auto& da = val.ensure_extra().darr;
    da.reserve(n);
    objs_.push_back(id);
    for (std::uint64_t k = 0; k < n; ++k) da.push_back(read_f64(little));
    return id;
  }

  std::size_t mk_block(unsigned tag, std::uint64_t size) {
    std::size_t id = arena_.new_node();
    {
      Value& val = arena_.at(id);
      val.kind = Value::Kind::Block;
      val.tag = tag;
      if (size == 0)
        return id;  // Atom: a zero-size block is an immediate, not registered.
    }
    // Reserve this block's contiguous field slice in the pool BEFORE reading its
    // children (whose slices are reserved further along, keeping ours intact).
    arena_.begin_block(id, size);
    // Register the block BEFORE reading fields so a field may reference the
    // block itself (cyclic graphs, e.g. recursive type_expr).
    objs_.push_back(id);
    for (std::uint64_t k = 0; k < size; ++k) {
      std::size_t child = read_value();
      arena_.set_field(id, k, child);  // by stable index; fpool_ may have realloc'd
    }
    return id;
  }

  std::size_t shared(std::uint64_t dist) {
    if (compressed_) {  // intern.c: an absolute reference in the compressed format
      if (dist >= objs_.size()) throw Error("marshal: shared back-reference out of range");
      return objs_[dist];
    }
    if (dist == 0 || dist > objs_.size())
      throw Error("marshal: shared back-reference out of range");
    return objs_[objs_.size() - dist];
  }

  std::size_t read_custom(int code, std::size_t start) {
    // Custom blocks begin with a NUL-terminated identifier ("_i" int32, "_j"
    // int64, "_n" nativeint, BLAKE128 digests, ...).  CODE_CUSTOM_LEN carries
    // the serialized data length (sz_32 + sz_64) before the payload.  We keep
    // the verbatim on-disk bytes (from the code byte) so the value round-trips
    // through the linker's DATA section unchanged.
    std::string id;
    for (;;) {
      char c = static_cast<char>(u8());
      if (c == '\0') break;
      id.push_back(c);
    }
    long long bsize = 0;       // in-memory data size in bytes
    long long val = 0;         // decoded scalar value (int32/64/nativeint)
    bool scalar = true;
    if (code == CODE_CUSTOM_LEN) { u32(); bsize = (long long)u64(); }
    if (id == "_i") { val = (std::int32_t)u32(); bsize = 4; }   // int32 (FIXED)
    else if (id == "_j") { val = (std::int64_t)u64(); bsize = 8; }  // int64 (FIXED)
    else if (id == "_n") {                           // nativeint (LEN): tag + value
      int t = u8(); val = (t == 1) ? (long long)(std::int32_t)u32() : (long long)(std::int64_t)u64();
      bsize = 8;
    } else if (code == CODE_CUSTOM_LEN) {            // unknown (e.g. a digest)
      scalar = false;
      for (long long k = 0; k < bsize; ++k) u8();
    } else {
      throw Error("marshal: unsupported custom block '" + id + "'");
    }
    std::size_t r = arena_.new_node();
    Value& v = arena_.at(r);
    if (scalar) {  // int32/int64/nativeint: an Int for the reader, raw for the linker
      v.kind = Value::Kind::Int;
      v.i = val;
      auto& ex = v.ensure_extra();
      ex.custom_raw.assign(reinterpret_cast<const char*>(data_ + start), pos_ - start);
      ex.custom_bsize = (int)bsize;
    } else {       // a digest etc.: opaque payload bytes
      v.kind = Value::Kind::String;
      v.ensure_extra().str.assign(reinterpret_cast<const char*>(data_ + start), pos_ - start);
    }
    objs_.push_back(r);
    return r;
  }

  std::size_t read_value() {
    int code = u8();
    if (code >= PREFIX_SMALL_INT) {
      if (code >= PREFIX_SMALL_BLOCK) {
        unsigned tag = code & 0xF;
        unsigned size = (code >> 4) & 0x7;
        return mk_block(tag, size);
      }
      return mk_int(code & 0x3F);
    }
    if (code >= PREFIX_SMALL_STRING) {
      return mk_string(code & 0x1F);
    }
    switch (code) {
      case CODE_INT8: return mk_int(static_cast<std::int8_t>(u8()));
      case CODE_INT16: return mk_int(static_cast<std::int16_t>(u16()));
      case CODE_INT32: return mk_int(static_cast<std::int32_t>(u32()));
      case CODE_INT64: return mk_int(static_cast<std::int64_t>(u64()));
      case CODE_SHARED8: return shared(u8());
      case CODE_SHARED16: return shared(u16());
      case CODE_SHARED32: return shared(u32());
      case CODE_SHARED64: return shared(u64());
      case CODE_BLOCK32: {
        std::uint32_t h = u32();
        return mk_block(h & 0xFF, h >> 10);
      }
      case CODE_BLOCK64: {
        std::uint64_t h = u64();
        return mk_block(h & 0xFF, h >> 10);
      }
      case CODE_STRING8: return mk_string(u8());
      case CODE_STRING32: return mk_string(u32());
      case CODE_STRING64: return mk_string(u64());
      case CODE_DOUBLE_BIG: return mk_double(false);
      case CODE_DOUBLE_LITTLE: return mk_double(true);
      case CODE_DOUBLE_ARRAY8_BIG: return mk_double_array(u8(), false);
      case CODE_DOUBLE_ARRAY8_LITTLE: return mk_double_array(u8(), true);
      case CODE_DOUBLE_ARRAY32_BIG: return mk_double_array(u32(), false);
      case CODE_DOUBLE_ARRAY32_LITTLE: return mk_double_array(u32(), true);
      case CODE_DOUBLE_ARRAY64_BIG: return mk_double_array(u64(), false);
      case CODE_DOUBLE_ARRAY64_LITTLE: return mk_double_array(u64(), true);
      case CODE_CUSTOM_LEN:
      case CODE_CUSTOM_FIXED:
      case OLD_CODE_CUSTOM:
        return read_custom(code, pos_ - 1);
      case CODE_CODEPOINTER:
      case CODE_INFIXPOINTER:
        throw Error("marshal: code/infix pointer unsupported");
      default:
        throw Error("marshal: unknown code " + std::to_string(code));
    }
  }

  const std::uint8_t* data_;
  std::size_t len_;
  std::size_t pos_;
  std::size_t data_end_ = 0;
  bool compressed_ = false;
  Arena& arena_;
  std::vector<std::size_t> objs_;
};

}  // namespace

bool compressed_header(const std::uint8_t* data, std::size_t len, std::size_t off, CompressedHeader& h) {
  if (off + 5 > len) return false;
  std::uint32_t magic = (std::uint32_t(data[off]) << 24) | (std::uint32_t(data[off + 1]) << 16) |
                        (std::uint32_t(data[off + 2]) << 8) | data[off + 3];
  if (magic != MAGIC_COMPRESSED) return false;
  std::size_t p = off + 4;
  h.header_len = data[p++] & 0x3F;
  auto vlq = [&]() -> std::uint64_t {  // intern.c readvlq
    if (p >= len) throw Error("marshal: truncated compressed header");
    std::uint8_t c = data[p++];
    std::uint64_t n = c & 0x7F;
    while (c & 0x80) {
      if (p >= len) throw Error("marshal: truncated compressed header");
      c = data[p++];
      n = (n << 7) | (c & 0x7F);
    }
    return n;
  };
  h.data_len = vlq();
  h.uncompressed_len = vlq();
  h.num_objects = vlq();
  (void)vlq();  // size_32
  (void)vlq();  // size_64
  if (off + h.header_len + h.data_len > len) throw Error("marshal: truncated compressed value");
  return true;
}

std::vector<std::uint8_t> decompress(const std::uint8_t* data, std::size_t len, std::size_t off,
                                     const CompressedHeader& h) {
#ifdef CPPCAML_HAVE_ZSTD
  (void)len;
  std::vector<std::uint8_t> out(h.uncompressed_len);
  std::size_t res = ZSTD_decompress(out.data(), out.size(), data + off + h.header_len, h.data_len);
  if (ZSTD_isError(res) || res != h.uncompressed_len) throw Error("input_value: decompression error");
  return out;
#else
  (void)data; (void)len; (void)off; (void)h;
  throw Error("input_value: compressed object, cannot decompress");
#endif
}

std::size_t read_value(const std::uint8_t* data, std::size_t len,
                       std::size_t& off, Arena& arena) {
  Reader r(data, len, off, arena);
  std::size_t root = r.read_root();
  off = r.pos();
  return root;
}

void skip_value(const std::uint8_t* data, std::size_t len, std::size_t& off) {
  Arena unused;  // never touched by skip_root
  Reader r(data, len, off, unused);
  off = r.skip_root();
}

}  // namespace cppcaml::marshal
