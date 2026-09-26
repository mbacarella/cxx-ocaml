// Port of stdlib/camlinternalFormat.ml's format-string parser,
// `fmt_ebb_of_string` (with what it needs: fmtty_of_fmt, concat_fmtty, the
// char sets, failwith_message's messages), for Typecore.type_format.
//
// OCaml's result is a GADT value of CamlinternalFormatBasics; here it is an
// untyped value tree shaped exactly as typecore.ml's mk_* functions render
// it into a Parsetree expression:
//   - Constr: a constructor (`Char`, `Lit_padding`, `Int_d`, `End_of_fmtty`,
//     `Format`, ...) and its arguments, as mk_constr receives them (the
//     "0 args / 1 arg / >= 2 args as a tuple" rule is the caller's);
//   - Tuple: mk_fconv's (flag, kind) pair;
//   - None / Some: mk_int_opt;
//   - Int / String / Char: mk_int, mk_string, mk_char.
// `fmt_ebb_of_string` returns `mk_fmt fmt` (not wrapped in `Format`).
// Values are allocated in the current typing zone (`make<T>`).
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::camlinternal_format {

struct FmtValue {
  enum class Kind : std::uint8_t { Constr, Tuple, None, Some, Int, String, Char };
  Kind kind;
  std::string_view name;             // Constr
  Slice<const FmtValue*> args;       // Constr / Tuple; Some: one element
  long i = 0;                        // Int
  std::string_view s;                // String (bytes, may contain NULs)
  char c = 0;                        // Char
};

// Failure msg.  `msg` is the exact message: it may contain NUL bytes (e.g.
// `invalid conversion "%\000"`), which what() would truncate.
struct Failure : std::runtime_error {
  std::string msg;
  explicit Failure(const std::string& m) : std::runtime_error(m), msg(m) {}
};

// fmt_ebb_of_string ~legacy_behavior str; raises Failure
const FmtValue* fmt_ebb_of_string(bool legacy_behavior, std::string_view str);

}  // namespace cppcaml::typing::camlinternal_format
