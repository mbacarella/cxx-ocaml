// Port of utils/arg_helper.ml's Make functor, as Clflags instantiates it:
// Int_arg_helper (int keys, int values) and Float_arg_helper (int keys, float
// values) -- the "<value> | <key>=<value>[,...]" specifications of the
// inlining parameters, varying by simplification round.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <string>

#include "cppcaml/typing/arg.hpp"

namespace cppcaml::typing::arg_helper {

// Stdlib.float_of_string (caml_float_of_string): '_' ignored, the whole
// string must be a float (strtod's syntax, hexadecimal included)
inline bool float_of_string_opt(const std::string& s, double& out) {
  std::string t;
  for (char c : s)
    if (c != '_') t += c;
  if (t.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double d = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size()) return false;
  out = d;
  return true;
}

template <class V>
struct Parsed {
  V base_default{};
  std::map<long, V> base_override;
  std::optional<V> user_default;
  std::map<long, V> user_override;
};

template <class V>
Parsed<V> default_(V v) {
  Parsed<V> p;
  p.base_default = v;
  return p;
}
template <class V>
Parsed<V> set_base_default(V value, Parsed<V> t) {
  t.base_default = value;
  return t;
}
template <class V>
Parsed<V> add_base_override(long key, V value, Parsed<V> t) {
  t.base_override[key] = value;
  return t;
}
template <class V>
Parsed<V> reset_base_overrides(Parsed<V> t) {
  t.base_override.clear();
  return t;
}

// the value of S.Value.of_string, false when it raises (the exception's
// Printexc.to_string in [exn])
inline bool value_of_string(const std::string& s, long& out, std::string& exn) {
  if (arg::int_of_string_opt(s, out)) return true;
  exn = "Failure(\"int_of_string\")";
  return false;
}
inline bool value_of_string(const std::string& s, double& out, std::string& exn) {
  if (float_of_string_opt(s, out)) return true;
  exn = "Failure(\"float_of_string\")";
  return false;
}

// parse_exn str ~update: the failing exception's Printexc.to_string, or
// nullopt on success
template <class V>
std::optional<std::string> parse_exn(const std::string& str, Parsed<V>& update) {
  Parsed<V> acc = update;
  std::size_t start = 0;
  for (;;) {
    std::size_t comma = str.find(',', start);
    std::string value = str.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!value.empty()) {  // the removal of empty chunks
      std::size_t equals = value.find('=');
      std::string exn;
      if (equals == std::string::npos) {
        V v{};
        if (!value_of_string(value, v, exn)) return exn;
        acc.user_default = v;
      } else {
        if (equals == 0) return std::string("Failure(\"Missing key in argument specification\")");
        long key;
        if (!arg::int_of_string_opt(value.substr(0, equals), key)) return std::string("Failure(\"int_of_string\")");
        V v{};
        if (!value_of_string(value.substr(equals + 1), v, exn)) return exn;
        acc.user_override[key] = v;
      }
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  update = acc;
  return std::nullopt;
}

// parse str help_text update: on a failure, prints
// "<exception>: <help_text>" and exits 2 (Arg_helper.fatal)
template <class V>
void parse(const std::string& str, const std::string& help_text, Parsed<V>& update) {
  if (std::optional<std::string> exn = parse_exn(str, update)) {
    std::cout.flush();
    std::cerr << *exn << ": " << help_text << std::endl;
    std::exit(2);
  }
}

// parse_no_error str update: Ok (nullopt) | Parse_failed exn
template <class V>
std::optional<std::string> parse_no_error(const std::string& str, Parsed<V>& update) {
  return parse_exn(str, update);
}

template <class V>
V get(long key, const Parsed<V>& parsed) {
  if (auto it = parsed.user_override.find(key); it != parsed.user_override.end()) return it->second;
  if (parsed.user_default) return *parsed.user_default;
  if (auto it = parsed.base_override.find(key); it != parsed.base_override.end()) return it->second;
  return parsed.base_default;
}

}  // namespace cppcaml::typing::arg_helper
