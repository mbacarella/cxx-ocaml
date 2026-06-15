// Faithful port of runtime/blake2.c (BLAKE2b).  Kept byte-for-byte equivalent to
// the runtime so the .cmi self-CRC (Digest.BLAKE128) matches the oracle's.
#include "cppcaml/blake2.hpp"

#include <cstring>

namespace cppcaml::blake2 {

namespace {

inline std::uint64_t u8to64le(const unsigned char* p) {
  return (std::uint64_t)p[0] | ((std::uint64_t)p[1] << 8) | ((std::uint64_t)p[2] << 16) |
         ((std::uint64_t)p[3] << 24) | ((std::uint64_t)p[4] << 32) | ((std::uint64_t)p[5] << 40) |
         ((std::uint64_t)p[6] << 48) | ((std::uint64_t)p[7] << 56);
}
inline std::uint64_t rotr64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

const std::uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

const std::uint8_t SIGMA[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

#define MIX2B(a, b, c, d, x, y)  \
  do {                           \
    a += b + x;                  \
    d = rotr64(d ^ a, 32);       \
    c += d;                      \
    b = rotr64(b ^ c, 24);       \
    a += b + y;                  \
    d = rotr64(d ^ a, 16);       \
    c += d;                      \
    b = rotr64(b ^ c, 63);       \
  } while (0)

void compress(Context& s, const unsigned char* data, std::size_t numbytes, int is_last) {
  s.len[0] += numbytes;
  if (s.len[0] < numbytes) s.len[1]++;  // carry
  std::uint64_t v0 = s.h[0], v1 = s.h[1], v2 = s.h[2], v3 = s.h[3];
  std::uint64_t v4 = s.h[4], v5 = s.h[5], v6 = s.h[6], v7 = s.h[7];
  std::uint64_t v8 = IV[0], v9 = IV[1], v10 = IV[2], v11 = IV[3];
  std::uint64_t v12 = IV[4] ^ s.len[0], v13 = IV[5] ^ s.len[1];
  std::uint64_t v14 = is_last ? ~IV[6] : IV[6];
  std::uint64_t v15 = IV[7];
  std::uint64_t m[16];
  for (int i = 0; i < 16; i++) m[i] = u8to64le(data + i * 8);
  for (int i = 0; i < 12; i++) {
    const std::uint8_t* sg = SIGMA[i];
    MIX2B(v0, v4, v8, v12, m[sg[0]], m[sg[1]]);
    MIX2B(v1, v5, v9, v13, m[sg[2]], m[sg[3]]);
    MIX2B(v2, v6, v10, v14, m[sg[4]], m[sg[5]]);
    MIX2B(v3, v7, v11, v15, m[sg[6]], m[sg[7]]);
    MIX2B(v0, v5, v10, v15, m[sg[8]], m[sg[9]]);
    MIX2B(v1, v6, v11, v12, m[sg[10]], m[sg[11]]);
    MIX2B(v2, v7, v8, v13, m[sg[12]], m[sg[13]]);
    MIX2B(v3, v4, v9, v14, m[sg[14]], m[sg[15]]);
  }
  s.h[0] ^= v0 ^ v8;  s.h[1] ^= v1 ^ v9;
  s.h[2] ^= v2 ^ v10; s.h[3] ^= v3 ^ v11;
  s.h[4] ^= v4 ^ v12; s.h[5] ^= v5 ^ v13;
  s.h[6] ^= v6 ^ v14; s.h[7] ^= v7 ^ v15;
}

}  // namespace

void init(Context& s, std::size_t hashlen, std::size_t keylen, const unsigned char* key) {
  for (int i = 0; i < 8; i++) s.h[i] = IV[i];
  s.h[0] ^= 0x01010000 | (keylen << 8) | hashlen;
  s.len[0] = s.len[1] = 0;
  s.numbytes = 0;
  if (keylen > 0) {
    if (keylen > 64) keylen = 64;
    std::memcpy(s.buffer, key, keylen);
    std::memset(s.buffer + keylen, 0, 128 - keylen);
    s.numbytes = 128;
  }
}

void update(Context& s, const unsigned char* data, std::size_t len) {
  if (s.numbytes > 0) {
    std::size_t n = 128 - s.numbytes;
    if (len <= n) {  // not enough fresh data to compress -- buffer it
      std::memcpy(s.buffer + s.numbytes, data, len);
      s.numbytes += len;
      return;
    }
    std::memcpy(s.buffer + s.numbytes, data, n);
    compress(s, s.buffer, 128, 0);
    data += n; len -= n;
  }
  while (len > 128) {  // strictly > : a full final block stays buffered for Final
    compress(s, data, 128, 0);
    data += 128; len -= 128;
  }
  std::memcpy(s.buffer, data, len);
  s.numbytes = len;
}

void final(Context& s, std::size_t hashlen, unsigned char* hash) {
  std::memset(s.buffer + s.numbytes, 0, 128 - s.numbytes);
  compress(s, s.buffer, s.numbytes, 1);
  for (unsigned i = 0; i < hashlen; i++) hash[i] = (unsigned char)(s.h[i / 8] >> (8 * (i % 8)));
}

std::string digest(const unsigned char* data, std::size_t len, std::size_t hashlen) {
  Context s;
  init(s, hashlen);
  update(s, data, len);
  std::string out(hashlen, '\0');
  final(s, hashlen, reinterpret_cast<unsigned char*>(out.data()));
  return out;
}

std::string blake128(const unsigned char* data, std::size_t len) { return digest(data, len, 16); }

std::string to_hex(const std::string& raw) {
  static const char* hx = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) { out.push_back(hx[c >> 4]); out.push_back(hx[c & 0xF]); }
  return out;
}

}  // namespace cppcaml::blake2
