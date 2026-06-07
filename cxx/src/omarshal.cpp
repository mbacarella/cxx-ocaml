#include "cppcaml/omarshal.hpp"

#include <cstring>

namespace cppcaml::omarshal {

ValPtr vint(long long n) { auto v = std::make_shared<Value>(); v->k = Value::Int; v->i = n; return v; }
ValPtr vstr(std::string s) { auto v = std::make_shared<Value>(); v->k = Value::Str; v->s = std::move(s); return v; }
ValPtr vdbl(double d) { auto v = std::make_shared<Value>(); v->k = Value::Dbl; v->d = d; return v; }
ValPtr vblock(int tag, std::vector<ValPtr> f) {
  auto v = std::make_shared<Value>(); v->k = Value::Block; v->tag = tag; v->fields = std::move(f); return v;
}
ValPtr vdblarr(std::vector<double> ds) {
  auto v = std::make_shared<Value>(); v->k = Value::DblArr; v->darr = std::move(ds); return v;
}
ValPtr vcustom(std::string raw, long long words) {
  auto v = std::make_shared<Value>(); v->k = Value::Custom; v->s = std::move(raw); v->custom_words = words; return v;
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
  void byte(int b) { body.push_back((std::uint8_t)b); }
  void be32(std::uint32_t n) { byte(n >> 24); byte(n >> 16); byte(n >> 8); byte(n); }
  void emit_int(long long n) {
    if (n >= 0 && n < 0x40) byte(0x40 | (int)n);
    else if (n >= -128 && n < 128) { byte(0x0); byte((int)n & 0xFF); }
    else if (n >= -32768 && n < 32768) { byte(0x1); byte(n >> 8); byte(n); }
    else if (n >= -(1LL << 31) && n < (1LL << 31)) { byte(0x2); be32((std::uint32_t)n); }
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
        if (size == 0) { byte(0x80 | v->tag); return; }
        if (v->tag < 16 && size < 8) byte(0x80 | v->tag | (size << 4));
        else { byte(0x8); be32(((std::uint32_t)size << 10) | (std::uint32_t)v->tag); }
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
        nobjs++; w64 += 1 + v->custom_words; w32 += 1 + v->custom_words;
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
