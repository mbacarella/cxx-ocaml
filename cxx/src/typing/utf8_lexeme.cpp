// Port of utils/utf8_lexeme.ml.
#include "cppcaml/typing/utf8_lexeme.hpp"

namespace cppcaml::typing::utf8_lexeme {

namespace {

constexpr int rep = 0xFFFD;  // Uchar.rep

// Non-ASCII letters that are allowed in identifiers (currently: Latin-9).
// get_known_char: the other case of [c], positive for an uppercase [c]
// (Upper), negative for a lowercase one (Lower), 0 for None.
int get_known_char(int c) {
  if ((c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xde)) return c + 0x20;
  if ((c >= 0xe0 && c <= 0xf6) || (c >= 0xf8 && c <= 0xfe)) return -(c - 0x20);
  switch (c) {
    case 0x160: return 0x161;   // Š
    case 0x161: return -0x160;  // š
    case 0x17d: return 0x17e;   // Ž
    case 0x17e: return -0x17d;  // ž
    case 0x152: return 0x153;   // Œ
    case 0x153: return -0x152;  // œ
    case 0x178: return 0xff;    // Ÿ
    case 0xff: return -0x178;   // ÿ
    case 0x1e9e: return 0xdf;   // ẞ
    case 0xdf: return -0x1e9e;  // ß
    default: return 0;
  }
}

// NFD to NFC conversion table for the letters above (0: None)
int get_known_pair(int c1, int n2) {
  struct P { int c; int n; int r; };
  static constexpr P table[] = {
      {'A', 0x300, 0xc0}, {'A', 0x301, 0xc1}, {'A', 0x302, 0xc2}, {'A', 0x303, 0xc3},
      {'A', 0x308, 0xc4}, {'A', 0x30a, 0xc5}, {'C', 0x327, 0xc7}, {'E', 0x300, 0xc8},
      {'E', 0x301, 0xc9}, {'E', 0x302, 0xca}, {'E', 0x308, 0xcb}, {'I', 0x300, 0xcc},
      {'I', 0x301, 0xcd}, {'I', 0x302, 0xce}, {'I', 0x308, 0xcf}, {'N', 0x303, 0xd1},
      {'O', 0x300, 0xd2}, {'O', 0x301, 0xd3}, {'O', 0x302, 0xd4}, {'O', 0x303, 0xd5},
      {'O', 0x308, 0xd6}, {'U', 0x300, 0xd9}, {'U', 0x301, 0xda}, {'U', 0x302, 0xdb},
      {'U', 0x308, 0xdc}, {'Y', 0x301, 0xdd}, {'Y', 0x308, 0x178}, {'S', 0x30c, 0x160},
      {'Z', 0x30c, 0x17d}, {'a', 0x300, 0xe0}, {'a', 0x301, 0xe1}, {'a', 0x302, 0xe2},
      {'a', 0x303, 0xe3}, {'a', 0x308, 0xe4}, {'a', 0x30a, 0xe5}, {'c', 0x327, 0xe7},
      {'e', 0x300, 0xe8}, {'e', 0x301, 0xe9}, {'e', 0x302, 0xea}, {'e', 0x308, 0xeb},
      {'i', 0x300, 0xec}, {'i', 0x301, 0xed}, {'i', 0x302, 0xee}, {'i', 0x308, 0xef},
      {'n', 0x303, 0xf1}, {'o', 0x300, 0xf2}, {'o', 0x301, 0xf3}, {'o', 0x302, 0xf4},
      {'o', 0x303, 0xf5}, {'o', 0x308, 0xf6}, {'u', 0x300, 0xf9}, {'u', 0x301, 0xfa},
      {'u', 0x302, 0xfb}, {'u', 0x308, 0xfc}, {'y', 0x301, 0xfd}, {'y', 0x308, 0xff},
      {'s', 0x30c, 0x161}, {'z', 0x30c, 0x17e}};
  for (const P& p : table)
    if (p.c == c1 && p.n == n2) return p.r;
  return 0;
}

// String.get_utf_8_uchar: the decoded scalar (Uchar.rep if invalid), its
// length (for an invalid sequence, its maximal invalid subpart) and validity
struct Decode {
  int u;
  std::size_t len;
  bool valid;
};

Decode get_utf_8_uchar(std::string_view s, std::size_t i) {
  auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  std::size_t max = s.size() - 1;
  unsigned b0 = b(i);
  auto invalid = [](std::size_t n) { return Decode{rep, n, false}; };
  auto in = [](unsigned x, unsigned lo, unsigned hi) { return x >= lo && x <= hi; };
  if (b0 <= 0x7f) return {static_cast<int>(b0), 1, true};
  if (in(b0, 0xc2, 0xdf)) {
    if (i + 1 > max) return invalid(1);
    unsigned b1 = b(i + 1);
    if (!in(b1, 0x80, 0xbf)) return invalid(1);
    return {static_cast<int>(((b0 & 0x1f) << 6) | (b1 & 0x3f)), 2, true};
  }
  unsigned lo1 = 0x80, hi1 = 0xbf;
  std::size_t n;
  if (b0 == 0xe0) { lo1 = 0xa0; n = 3; }
  else if (in(b0, 0xe1, 0xec) || in(b0, 0xee, 0xef)) n = 3;
  else if (b0 == 0xed) { hi1 = 0x9f; n = 3; }
  else if (b0 == 0xf0) { lo1 = 0x90; n = 4; }
  else if (in(b0, 0xf1, 0xf3)) n = 4;
  else if (b0 == 0xf4) { hi1 = 0x8f; n = 4; }
  else return invalid(1);
  if (i + 1 > max) return invalid(1);
  unsigned b1 = b(i + 1);
  if (!in(b1, lo1, hi1)) return invalid(1);
  if (i + 2 > max) return invalid(2);
  unsigned b2 = b(i + 2);
  if (!in(b2, 0x80, 0xbf)) return invalid(2);
  if (n == 3)
    return {static_cast<int>(((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f)), 3, true};
  if (i + 3 > max) return invalid(3);
  unsigned b3 = b(i + 3);
  if (!in(b3, 0x80, 0xbf)) return invalid(3);
  return {static_cast<int>(((b0 & 0x07) << 18) | ((b1 & 0x3f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f)),
          4, true};
}

// Buffer.add_utf_8_uchar
void add_utf_8_uchar(std::string& buf, int u) {
  if (u <= 0x7f) {
    buf += static_cast<char>(u);
  } else if (u <= 0x7ff) {
    buf += static_cast<char>(0xc0 | (u >> 6));
    buf += static_cast<char>(0x80 | (u & 0x3f));
  } else if (u <= 0xffff) {
    buf += static_cast<char>(0xe0 | (u >> 12));
    buf += static_cast<char>(0x80 | ((u >> 6) & 0x3f));
    buf += static_cast<char>(0x80 | (u & 0x3f));
  } else {
    buf += static_cast<char>(0xf0 | (u >> 18));
    buf += static_cast<char>(0x80 | ((u >> 12) & 0x3f));
    buf += static_cast<char>(0x80 | ((u >> 6) & 0x3f));
    buf += static_cast<char>(0x80 | (u & 0x3f));
  }
}

bool is_valid_decode_and_char(const Decode& d) { return d.valid && d.u != rep; }

bool pair_normalize(std::string& buf, bool valid, std::string_view s, int prev, std::size_t i) {
  while (i < s.size()) {
    Decode d = get_utf_8_uchar(s, i);
    valid = valid && is_valid_decode_and_char(d);
    std::size_t i2 = i + d.len;
    if (int u2 = get_known_pair(prev, d.u)) {
      prev = u2;
    } else {
      add_utf_8_uchar(buf, prev);
      prev = d.u;
    }
    i = i2;
  }
  add_utf_8_uchar(buf, prev);
  return valid;
}

// [first] is applied to the first character of [s] only (nullptr: none)
Result normalize_map_first(std::string_view s, int (*first)(int)) {
  if (s.empty()) return {true, ""};
  bool only_ascii = true;
  for (char c : s)
    if (static_cast<unsigned char>(c) >= 0x80) only_ascii = false;
  Decode d = get_utf_8_uchar(s, 0);
  int u0 = d.u;
  std::size_t i0 = d.len;
  int u0b = first ? first(u0) : u0;
  if (u0b == u0 && only_ascii) return {true, std::string(s)};
  if (only_ascii) {
    std::string res;
    add_utf_8_uchar(res, u0b);
    res.append(s.substr(i0));
    return {true, res};
  }
  std::string buf;
  bool valid = is_valid_decode_and_char(d);
  valid = pair_normalize(buf, valid, s, u0b, i0);
  return {valid, buf};
}

bool uchar_is_uppercase(int c) {
  if (c < 0x80) return c >= 65 && c <= 90;
  return get_known_char(c) > 0;
}

int uchar_lowercase(int c) {
  if (c < 0x80) return c >= 65 && c <= 90 ? c + 32 : c;
  int k = get_known_char(c);
  return k > 0 ? k : c;
}

int uchar_uppercase(int c) {
  if (c < 0x80) return c >= 97 && c <= 122 ? c - 32 : c;
  int k = get_known_char(c);
  return k < 0 ? -k : c;
}

bool uchar_valid_in_identifier(bool with_dot, int c) {
  if (c < 0x80)
    return (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57) || c == 95 || c == 39 ||
           (with_dot && c == 46);
  return get_known_char(c) != 0;
}

bool uchar_not_identifier_start(int c) { return (c >= 48 && c <= 57) || c == 39; }

}  // namespace

Result normalize(std::string_view s) { return normalize_map_first(s, nullptr); }
Result capitalize(std::string_view s) { return normalize_map_first(s, uchar_uppercase); }
Result uncapitalize(std::string_view s) { return normalize_map_first(s, uchar_lowercase); }

bool is_capitalized(std::string_view s) { return !s.empty() && uchar_is_uppercase(get_utf_8_uchar(s, 0).u); }

Validation validate_identifier(std::string_view s, bool with_dot) {
  std::size_t i = 0;
  while (i < s.size()) {
    Decode d = get_utf_8_uchar(s, i);
    if (!uchar_valid_in_identifier(with_dot, d.u)) return {Validation::Kind::Invalid_character, d.u};
    if (i == 0 && uchar_not_identifier_start(d.u)) return {Validation::Kind::Invalid_beginning, d.u};
    i += d.len;
  }
  return {Validation::Kind::Valid};
}

bool is_valid_identifier(std::string_view s) { return validate_identifier(s).kind == Validation::Kind::Valid; }

bool starts_like_a_valid_identifier(std::string_view s) {
  if (s.empty()) return false;
  int u = get_utf_8_uchar(s, 0).u;
  return uchar_valid_in_identifier(false, u) && !uchar_not_identifier_start(u);
}

bool is_lowercase(std::string_view s) {
  std::size_t n = 0;
  while (n < s.size()) {
    Decode d = get_utf_8_uchar(s, n);
    if (!(uchar_valid_in_identifier(false, d.u) && !uchar_is_uppercase(d.u))) return false;
    n += d.len;
  }
  return true;
}

}  // namespace cppcaml::typing::utf8_lexeme
