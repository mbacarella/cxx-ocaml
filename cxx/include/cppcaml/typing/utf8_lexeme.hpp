// Port of utils/utf8_lexeme.ml: minimal support for Unicode characters in
// identifiers (ASCII and Latin-9 letters, the latter in NFC), used by
// Unit_info (module names of source files), Load_path
// (Misc.normalized_unit_filename), Oprint and Env.  The C++ lexer carries
// its own copy of the same tables (lexer.cpp).
#pragma once

#include <string>
#include <string_view>

namespace cppcaml::typing::utf8_lexeme {

// (t, t) Result.t: [ok] is false for Error (invalid UTF-8, replaced by U+FFFD)
struct Result {
  bool ok;
  std::string s;
};

Result normalize(std::string_view s);
Result capitalize(std::string_view s);
Result uncapitalize(std::string_view s);
bool is_capitalized(std::string_view s);
bool is_valid_identifier(std::string_view s);
bool is_lowercase(std::string_view s);

struct Validation {
  enum class Kind { Valid, Invalid_character, Invalid_beginning };
  Kind kind;
  int u = 0;  // the offending Uchar
};
Validation validate_identifier(std::string_view s, bool with_dot = false);
bool starts_like_a_valid_identifier(std::string_view s);

}  // namespace cppcaml::typing::utf8_lexeme
