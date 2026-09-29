// BLAKE2b message digest — a faithful port of runtime/blake2.c, used to compute
// the .cmi self-CRC (Digest.BLAKE128 = BLAKE2b with a 16-byte digest, no key).
// output_cmi computes crc = BLAKE128(magic ++ marshaled (name,sign)).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cppcaml::blake2 {

struct Context {
  std::uint64_t h[8];
  std::uint64_t len[2];
  std::size_t numbytes;
  unsigned char buffer[128];  // BLAKE2_BLOCKSIZE
};

void init(Context& s, std::size_t hashlen, std::size_t keylen = 0,
          const unsigned char* key = nullptr);
void update(Context& s, const unsigned char* data, std::size_t len);
void final(Context& s, std::size_t hashlen, unsigned char* hash);

// One-shot: raw digest bytes (length hashlen) for the given buffer.
std::string digest(const unsigned char* data, std::size_t len, std::size_t hashlen);
// Digest.BLAKE128 of a buffer: 16 raw bytes.
std::string blake128(const unsigned char* data, std::size_t len);
std::string to_hex(const std::string& raw);

}  // namespace cppcaml::blake2
