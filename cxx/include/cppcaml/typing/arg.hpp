// Port of stdlib/arg.ml (the part ocamlc's driver uses): the option specs,
// parse_and_expand_argv_dynamic with its error messages (Unknown / Wrong /
// Missing / Message) and the usage text, and read_arg / read_arg0 (-args /
// -args0).
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cppcaml::typing::arg {

struct Spec {
  enum class K {
    Unit, Bool, Set, Clear, String, Set_string, Int, Set_int, Float, Set_float, Tuple, Symbol, Rest,
    Rest_all, Expand
  };
  K k = K::Unit;
  std::function<void()> unit;                                            // Unit
  std::function<void(bool)> bool_;                                       // Bool
  bool* ref = nullptr;                                                   // Set / Clear
  std::function<void(const std::string&)> string;                        // String / Symbol / Rest
  std::string* sref = nullptr;                                           // Set_string
  std::function<void(long)> int_;                                        // Int
  long* iref = nullptr;                                                  // Set_int
  std::function<void(double)> float_;                                    // Float
  double* fref = nullptr;                                                // Set_float
  std::vector<Spec> tuple;                                               // Tuple
  std::vector<std::string> symbols;                                      // Symbol
  std::function<void(const std::vector<std::string>&)> rest_all;         // Rest_all
  std::function<std::vector<std::string>(const std::string&)> expand;    // Expand
};

struct Option {  // key * spec * doc
  std::string key;
  Spec spec;
  std::string doc;
};

// Arg.Bad / Arg.Help (msg: the whole message -- it may hold NULs, which
// what() would cut)
struct Bad : std::runtime_error {
  std::string msg;
  explicit Bad(const std::string& m) : std::runtime_error(m), msg(m) {}
};
struct Help : std::runtime_error {
  std::string msg;
  explicit Help(const std::string& m) : std::runtime_error(m), msg(m) {}
};

using AnonFun = std::function<void(const std::string&)>;

// parse_and_expand_argv_dynamic current argv speclist anonfun errmsg
void parse_and_expand_argv_dynamic(long& current, std::vector<std::string>& argv, std::vector<Option>& speclist,
                                   const AnonFun& anonfun, const std::string& errmsg);

// usage_string speclist errmsg / usage (on stderr)
std::string usage_string(const std::vector<Option>& speclist, const std::string& errmsg);
void usage(const std::vector<Option>& speclist, const std::string& errmsg);

// read_arg file (newline-terminated, trailing \r trimmed) / read_arg0 file
// (NUL-terminated); raise Sys_error (a std::runtime_error with OCaml's text)
std::vector<std::string> read_arg(const std::string& file);
std::vector<std::string> read_arg0(const std::string& file);
struct SysError : std::runtime_error {
  explicit SysError(const std::string& m) : std::runtime_error(m) {}
};

// int_of_string_opt / bool_of_string_opt / float_of_string_opt
bool int_of_string_opt(const std::string& s, long& out);

}  // namespace cppcaml::typing::arg
