// Port of utils/binutils.ml: the dynamic symbols of a shared library (ELF,
// Mach-O, FlexDLL's PE export table), for Dll's link-time check that the
// primitives a program uses exist.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace cppcaml::typing::binutils {

struct Error {
  enum class Kind { Truncated_file, Unrecognized, Unsupported, Out_of_range } kind;
  std::string s;        // Unrecognized magic / Unsupported / Out_of_range
  std::int64_t n = 0;   // Unsupported
};
std::string error_to_string(const Error& e);

struct T {
  std::function<bool(const std::string&)> defines_symbol;
  std::function<std::optional<std::int64_t>(const std::string&)> symbol_offset;
};

// read filename: Ok t | Error err
std::variant<T, Error> read(const std::string& filename);

}  // namespace cppcaml::typing::binutils
