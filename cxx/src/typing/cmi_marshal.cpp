// See cmi_marshal.hpp.  The wire format: runtime/{intern.c,extern.c},
// runtime/caml/intext.h (as marshal.cpp, whose decoding this mirrors case
// by case).
#include "cmi_marshal.hpp"

namespace cppcaml::typing::cmi_marshal {

namespace {

using marshal::Error;

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

Id imm_id(long long v) { return (static_cast<std::uint64_t>(v) << 1) | 1; }

class Decoder {
 public:
  Decoder(const std::uint8_t* data, std::size_t len, std::size_t pos, Graph& g)
      : d_(data), len_(len), pos_(pos), g_(g) {}

  Id read_root() {
    marshal::CompressedHeader ch;
    if (marshal::compressed_header(d_, len_, pos_, ch)) {
      // intern.c: the payload decompressed, then read with absolute shared
      // references; its strings point into the graph's owned buffer
      g_.owned.push_back(marshal::decompress(d_, len_, pos_, ch));
      const std::vector<std::uint8_t>& buf = g_.owned.back();
      std::size_t end = pos_ + ch.header_len + ch.data_len;
      const std::uint8_t* d = d_;
      std::size_t len = len_;
      d_ = buf.data(); len_ = buf.size(); pos_ = 0;
      compressed_ = true;
      buf_tag_ = static_cast<std::uint64_t>(g_.owned.size()) << Graph::kBufShift;
      if (ch.num_objects <= buf.size()) {
        g_.nodes.reserve(g_.nodes.size() + ch.num_objects + 16);
        objs_.reserve(ch.num_objects);
      }
      g_.reserve_fields(buf.size());
      Id root = value();
      d_ = d; len_ = len; pos_ = end; compressed_ = false; buf_tag_ = 0;
      return root;
    }
    std::uint32_t magic = u32();
    std::uint64_t data_len, num_objects;
    if (magic == MAGIC_SMALL) {
      data_len = u32();
      num_objects = u32();
      (void)u32();
      (void)u32();
    } else if (magic == MAGIC_BIG) {
      data_len = u64();
      num_objects = u64();
      (void)u64();
    } else {
      throw Error("marshal: bad magic number");
    }
    if (data_len > len_ - pos_) throw Error("marshal: unexpected end of input");
    std::size_t end = pos_ + data_len;
    // every heap object is a node (plus the atoms); every field takes a byte
    if (num_objects <= data_len) {
      g_.nodes.reserve(g_.nodes.size() + num_objects + 16);
      objs_.reserve(num_objects);
    }
    g_.reserve_fields(data_len);
    Id root = value();
    pos_ = end;
    return root;
  }
  std::size_t pos() const { return pos_; }

 private:
  void need(std::size_t n) {
    if (len_ - pos_ < n) throw Error("marshal: unexpected end of input");
  }
  std::uint8_t u8() {
    need(1);
    return d_[pos_++];
  }
  std::uint32_t u16() {
    need(2);
    std::uint32_t v = (std::uint32_t{d_[pos_]} << 8) | d_[pos_ + 1];
    pos_ += 2;
    return v;
  }
  std::uint32_t u32() {
    need(4);
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v = (v << 8) | d_[pos_ + k];
    pos_ += 4;
    return v;
  }
  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) v = (v << 8) | d_[pos_ + k];
    pos_ += 8;
    return v;
  }

  Id new_node(Kind k, std::uint8_t tag, std::uint32_t n, std::uint64_t v) {
    g_.nodes.push_back(Node{k, tag, n, v});
    return static_cast<Id>(g_.nodes.size() - 1) << 1;
  }
  Id registered(Id id) {
    objs_.push_back(id);
    return id;
  }

  Id string(std::uint64_t n) {
    need(n);
    Id id = new_node(Kind::String, 0, static_cast<std::uint32_t>(n), pos_ | buf_tag_);
    pos_ += n;
    return registered(id);
  }
  Id dbl(bool little) {
    need(8);
    std::uint8_t b[8];
    std::memcpy(b, d_ + pos_, 8);
    pos_ += 8;
    if (!little)
      for (int k = 0; k < 4; ++k) std::swap(b[k], b[7 - k]);
    std::uint64_t bits;
    std::memcpy(&bits, b, 8);
    return registered(new_node(Kind::Double, 0, 0, bits));
  }
  Id dbl_array(std::uint64_t n, bool little) {
    // registered before its elements, as the runtime allocates then fills
    Id id = registered(new_node(Kind::DoubleArray, little ? 1 : 0, static_cast<std::uint32_t>(n), pos_ | buf_tag_));
    need(n * 8);
    pos_ += n * 8;
    return id;
  }
  Id block(unsigned tag, std::uint64_t size) {
    if (size == 0) return new_node(Kind::Block, static_cast<std::uint8_t>(tag), 0, 0);  // an atom
    std::size_t first = g_.pool_n;
    if (size > g_.pool_cap - first) throw Error("marshal: block larger than its value");
    g_.pool_n = first + size;
    // registered before its fields: a field may be the block itself
    Id id = registered(new_node(Kind::Block, static_cast<std::uint8_t>(tag), static_cast<std::uint32_t>(size), first));
    for (std::uint64_t k = 0; k < size; ++k) {
      Id child = value();
      g_.pool[first + k] = child;
    }
    return id;
  }
  Id shared(std::uint64_t dist) {
    if (compressed_) {  // intern.c: an absolute reference in the compressed format
      if (dist >= objs_.size()) throw Error("marshal: shared back-reference out of range");
      return objs_[dist];
    }
    if (dist == 0 || dist > objs_.size()) throw Error("marshal: shared back-reference out of range");
    return objs_[objs_.size() - dist];
  }
  Id custom(int code, std::size_t start) {
    std::string ident;
    for (;;) {
      char c = static_cast<char>(u8());
      if (c == '\0') break;
      ident.push_back(c);
    }
    long long bsize = 0, val = 0;
    bool scalar = true;
    if (code == CODE_CUSTOM_LEN) {
      u32();
      bsize = static_cast<long long>(u64());
    }
    if (ident == "_i") {
      val = static_cast<std::int32_t>(u32());
    } else if (ident == "_j") {
      val = static_cast<std::int64_t>(u64());
    } else if (ident == "_n") {
      int t = u8();
      val = (t == 1) ? static_cast<long long>(static_cast<std::int32_t>(u32()))
                     : static_cast<long long>(static_cast<std::int64_t>(u64()));
    } else if (code == CODE_CUSTOM_LEN) {
      scalar = false;
      need(static_cast<std::size_t>(bsize));
      pos_ += static_cast<std::size_t>(bsize);
    } else {
      throw Error("marshal: unsupported custom block '" + ident + "'");
    }
    // a boxed integer reads as an integer; anything else as its raw bytes
    if (scalar) return registered(new_node(Kind::Int, 0, 0, static_cast<std::uint64_t>(val)));
    return registered(new_node(Kind::String, 0, static_cast<std::uint32_t>(pos_ - start), start | buf_tag_));
  }

  Id value() {
    int code = u8();
    if (code >= PREFIX_SMALL_INT) {
      if (code >= PREFIX_SMALL_BLOCK) return block(code & 0xF, (code >> 4) & 0x7);
      return imm_id(code & 0x3F);
    }
    if (code >= PREFIX_SMALL_STRING) return string(code & 0x1F);
    switch (code) {
      case CODE_INT8: return imm_id(static_cast<std::int8_t>(u8()));
      case CODE_INT16: return imm_id(static_cast<std::int16_t>(u16()));
      case CODE_INT32: return imm_id(static_cast<std::int32_t>(u32()));
      case CODE_INT64: return imm_id(static_cast<std::int64_t>(u64()));
      case CODE_SHARED8: return shared(u8());
      case CODE_SHARED16: return shared(u16());
      case CODE_SHARED32: return shared(u32());
      case CODE_SHARED64: return shared(u64());
      case CODE_BLOCK32: {
        std::uint32_t h = u32();
        return block(h & 0xFF, h >> 10);
      }
      case CODE_BLOCK64: {
        std::uint64_t h = u64();
        return block(h & 0xFF, h >> 10);
      }
      case CODE_STRING8: return string(u8());
      case CODE_STRING32: return string(u32());
      case CODE_STRING64: return string(u64());
      case CODE_DOUBLE_BIG: return dbl(false);
      case CODE_DOUBLE_LITTLE: return dbl(true);
      case CODE_DOUBLE_ARRAY8_BIG: return dbl_array(u8(), false);
      case CODE_DOUBLE_ARRAY8_LITTLE: return dbl_array(u8(), true);
      case CODE_DOUBLE_ARRAY32_BIG: return dbl_array(u32(), false);
      case CODE_DOUBLE_ARRAY32_LITTLE: return dbl_array(u32(), true);
      case CODE_DOUBLE_ARRAY64_BIG: return dbl_array(u64(), false);
      case CODE_DOUBLE_ARRAY64_LITTLE: return dbl_array(u64(), true);
      case CODE_CUSTOM_LEN:
      case CODE_CUSTOM_FIXED:
      case OLD_CODE_CUSTOM:
        return custom(code, pos_ - 1);
      case CODE_CODEPOINTER:
      case CODE_INFIXPOINTER:
        throw Error("marshal: code/infix pointer unsupported");
      default:
        throw Error("marshal: unknown code " + std::to_string(code));
    }
  }

  const std::uint8_t* d_;
  std::size_t len_;
  std::size_t pos_;
  Graph& g_;
  std::vector<Id> objs_;
  bool compressed_ = false;
  std::uint64_t buf_tag_ = 0;  // the current buffer's index, in a String's offset
};

}  // namespace

Id read_value(const std::uint8_t* data, std::size_t len, std::size_t& off, Graph& g) {
  if (off > len) throw Error("marshal: unexpected end of input");
  g.data = data;
  Decoder d(data, len, off, g);
  Id root = d.read_root();
  off = d.pos();
  return root;
}

}  // namespace cppcaml::typing::cmi_marshal
