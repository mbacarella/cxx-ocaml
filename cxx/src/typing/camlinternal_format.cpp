// Port of stdlib/camlinternalFormat.ml's fmt_ebb_of_string (see the header).
// The parser follows the OCaml function by function; where OCaml evaluates
// several effectful expressions in one application or constructor (right to
// left), the order here is the same, since it decides which Failure is raised
// first.
#include "cppcaml/typing/camlinternal_format.hpp"

#include <optional>
#include <vector>

namespace cppcaml::typing::camlinternal_format {

namespace {

using V = FmtValue;
using VK = FmtValue::Kind;

// ---- value builders (typecore.ml's mk_* functions) ----------------------------------------
const V* constr(std::string_view name, std::vector<const V*> args = {}) {
  return make<V>(V{VK::Constr, name, slice(args)});
}
const V* tuple(std::vector<const V*> args) { return make<V>(V{VK::Tuple, {}, slice(args)}); }
const V* vint(long n) {
  V v{VK::Int};
  v.i = n;
  return make<V>(v);
}
const V* vstring(std::string_view s) {
  V v{VK::String};
  v.s = zone().str(s);
  return make<V>(v);
}
const V* vchar(char c) {
  V v{VK::Char};
  v.c = c;
  return make<V>(v);
}
const V* int_opt(std::optional<long> n) {
  if (!n) return make<V>(V{VK::None});
  return make<V>(V{VK::Some, {}, slice(std::vector<const V*>{vint(*n)})});
}

// ---- CamlinternalFormatBasics.padding / precision ----------------------------------------
enum class Side { Left, Right, Zeros };
struct Pad {
  enum class Kind { No_padding, Lit_padding, Arg_padding };
  Kind kind;
  Side side = Side::Right;
  long width = 0;
};
struct Prec {
  enum class Kind { No_precision, Lit_precision, Arg_precision };
  Kind kind;
  long n = 0;
};

const V* mk_side(Side s) {
  switch (s) {
    case Side::Left: return constr("Left");
    case Side::Right: return constr("Right");
    case Side::Zeros: return constr("Zeros");
  }
  return nullptr;
}
const V* mk_padding(const Pad& p) {
  switch (p.kind) {
    case Pad::Kind::No_padding: return constr("No_padding");
    case Pad::Kind::Lit_padding: return constr("Lit_padding", {mk_side(p.side), vint(p.width)});
    case Pad::Kind::Arg_padding: return constr("Arg_padding", {mk_side(p.side)});
  }
  return nullptr;
}
const V* mk_precision(const Prec& p) {
  switch (p.kind) {
    case Prec::Kind::No_precision: return constr("No_precision");
    case Prec::Kind::Lit_precision: return constr("Lit_precision", {vint(p.n)});
    case Prec::Kind::Arg_precision: return constr("Arg_precision");
  }
  return nullptr;
}

// ---- fmtty_of_fmt, over the value tree ----------------------------------------------------
const V* arg(const V* v, std::size_t k) { return v->args[k]; }
const V* last(const V* v) { return v->args.back(); }
bool is(const V* v, std::string_view name) { return v->kind == VK::Constr && v->name == name; }

// CamlinternalFormatBasics.concat_fmtty: the rest is always the last argument
const V* concat_fmtty(const V* fmtty1, const V* fmtty2) {
  if (is(fmtty1, "End_of_fmtty")) return fmtty2;
  std::vector<const V*> args(fmtty1->args.begin(), fmtty1->args.end());
  args.back() = concat_fmtty(args.back(), fmtty2);
  return constr(fmtty1->name, args);
}

const V* fmtty_of_fmt(const V* fmt);

const V* fmtty_of_padding_fmtty(const V* pad, const V* fmtty) {
  if (is(pad, "Arg_padding")) return constr("Int_ty", {fmtty});
  return fmtty;
}
const V* fmtty_of_precision_fmtty(const V* prec, const V* fmtty) {
  if (is(prec, "Arg_precision")) return constr("Int_ty", {fmtty});
  return fmtty;
}
const V* fmtty_of_formatting_gen(const V* fg) {
  // Open_tag (Format (fmt, _)) | Open_box (Format (fmt, _))
  return fmtty_of_fmt(arg(arg(fg, 0), 0));
}
const V* fmtty_of_ignored_format(const V* ign, const V* fmt) {
  if (is(ign, "Ignored_format_subst")) return concat_fmtty(arg(ign, 1), fmtty_of_fmt(fmt));
  if (is(ign, "Ignored_reader")) return constr("Ignored_reader_ty", {fmtty_of_fmt(fmt)});
  return fmtty_of_fmt(fmt);
}

const V* fmtty_of_fmt(const V* f) {
  std::string_view n = f->name;
  if (n == "String" || n == "Caml_string")
    return fmtty_of_padding_fmtty(arg(f, 0), constr("String_ty", {fmtty_of_fmt(arg(f, 1))}));
  auto int_like = [&](std::string_view ty) {
    const V* ty_rest = fmtty_of_fmt(arg(f, 3));
    const V* prec_ty = fmtty_of_precision_fmtty(arg(f, 2), constr(ty, {ty_rest}));
    return fmtty_of_padding_fmtty(arg(f, 1), prec_ty);
  };
  if (n == "Int") return int_like("Int_ty");
  if (n == "Int32") return int_like("Int32_ty");
  if (n == "Nativeint") return int_like("Nativeint_ty");
  if (n == "Int64") return int_like("Int64_ty");
  if (n == "Float") return int_like("Float_ty");
  if (n == "Char" || n == "Caml_char") return constr("Char_ty", {fmtty_of_fmt(arg(f, 0))});
  if (n == "Bool") return fmtty_of_padding_fmtty(arg(f, 0), constr("Bool_ty", {fmtty_of_fmt(arg(f, 1))}));
  if (n == "Alpha") return constr("Alpha_ty", {fmtty_of_fmt(arg(f, 0))});
  if (n == "Theta") return constr("Theta_ty", {fmtty_of_fmt(arg(f, 0))});
  if (n == "Reader") return constr("Reader_ty", {fmtty_of_fmt(arg(f, 0))});
  if (n == "Format_arg") return constr("Format_arg_ty", {arg(f, 1), fmtty_of_fmt(arg(f, 2))});
  if (n == "Format_subst") return constr("Format_subst_ty", {arg(f, 1), arg(f, 1), fmtty_of_fmt(arg(f, 2))});
  if (n == "Flush" || n == "String_literal" || n == "Char_literal" || n == "Formatting_lit")
    return fmtty_of_fmt(last(f));
  if (n == "Scan_char_set") return constr("String_ty", {fmtty_of_fmt(arg(f, 2))});
  if (n == "Scan_get_counter") return constr("Int_ty", {fmtty_of_fmt(arg(f, 1))});
  if (n == "Scan_next_char") return constr("Char_ty", {fmtty_of_fmt(arg(f, 0))});
  if (n == "Ignored_param") return fmtty_of_ignored_format(arg(f, 0), arg(f, 1));
  if (n == "Formatting_gen")
    return concat_fmtty(fmtty_of_formatting_gen(arg(f, 0)), fmtty_of_fmt(arg(f, 1)));
  // End_of_format (Custom never comes out of the parser)
  return constr("End_of_fmtty");
}

// ---- failwith_message's conversions -----------------------------------------------------
// %S: string_to_caml_string (String.escaped, in double quotes)
std::string caml_string(std::string_view s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\b': r += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') {
          r += static_cast<char>(c);
        } else {
          r += '\\';
          r += static_cast<char>('0' + c / 100);
          r += static_cast<char>('0' + (c / 10) % 10);
          r += static_cast<char>('0' + c % 10);
        }
    }
  }
  return r + "\"";
}
// %C: format_caml_char (Char.escaped, in single quotes)
std::string caml_char(char ch) {
  unsigned char c = static_cast<unsigned char>(ch);
  std::string r = "'";
  switch (c) {
    case '\'': r += "\\'"; break;
    case '\\': r += "\\\\"; break;
    case '\n': r += "\\n"; break;
    case '\t': r += "\\t"; break;
    case '\r': r += "\\r"; break;
    case '\b': r += "\\b"; break;
    default:
      if (c >= ' ' && c <= '~') {
        r += static_cast<char>(c);
      } else {
        r += '\\';
        r += static_cast<char>('0' + c / 100);
        r += static_cast<char>('0' + (c / 10) % 10);
        r += static_cast<char>('0' + c % 10);
      }
  }
  return r + "'";
}

// Sys.max_string_length on a 64-bit host: word_size / 8 * max_array_length - 1
constexpr long max_string_length = 8 * ((1L << 54) - 1) - 1;

struct NotFound {};

class Parser {
 public:
  Parser(bool legacy, std::string_view str) : legacy_behavior(legacy), str(str) {}

  const V* run() { return parse(0, static_cast<long>(str.size())); }

 private:
  bool legacy_behavior;
  std::string_view str;

  char at(long i) const { return str[static_cast<std::size_t>(i)]; }
  std::string sub(long pos, long len) const {
    return std::string(str.substr(static_cast<std::size_t>(pos), static_cast<std::size_t>(len)));
  }

  // ---- messages ----
  [[noreturn]] void failwith(const std::string& msg) const { throw Failure(msg); }
  std::string head() const { return "invalid format " + caml_string(str); }
  [[noreturn]] void invalid_format_message(long str_ind, const std::string& msg) const {
    failwith(head() + ": at character number " + std::to_string(str_ind) + ", " + msg);
  }
  [[noreturn]] void unexpected_end_of_format(long end_ind) const {
    invalid_format_message(end_ind, "unexpected end of format");
  }
  [[noreturn]] void invalid_nonnull_char_width(long str_ind) const {
    invalid_format_message(str_ind, "non-zero widths are unsupported for %c conversions");
  }
  [[noreturn]] void invalid_format_without(long str_ind, char c, const std::string& s) const {
    failwith(head() + ": at character number " + std::to_string(str_ind) + ", '" + std::string(1, c) +
             "' without " + s);
  }
  [[noreturn]] void expected_character(long str_ind, const std::string& expected, char read) const {
    failwith(head() + ": at character number " + std::to_string(str_ind) + ", " + expected +
             " expected, read " + caml_char(read));
  }
  [[noreturn]] void incompatible_flag(long pct_ind, long str_ind, char symb, const std::string& option) const {
    std::string subfmt = sub(pct_ind, str_ind - pct_ind);
    failwith(head() + ": at character number " + std::to_string(pct_ind) + ", " + option +
             " is incompatible with '" + std::string(1, symb) + "' in sub-format " + caml_string(subfmt));
  }

  // ---- parsing ----
  const V* parse(long beg_ind, long end_ind) { return parse_literal(beg_ind, beg_ind, end_ind); }

  const V* parse_literal(long lit_start, long str_ind, long end_ind) {
    for (;;) {
      if (str_ind == end_ind) return add_literal(lit_start, str_ind, constr("End_of_format"));
      char c = at(str_ind);
      if (c == '%') {
        const V* fmt_rest = parse_format(str_ind, end_ind);
        return add_literal(lit_start, str_ind, fmt_rest);
      }
      if (c == '@') {
        const V* fmt_rest = parse_after_at(str_ind + 1, end_ind);
        return add_literal(lit_start, str_ind, fmt_rest);
      }
      str_ind++;
    }
  }

  const V* parse_format(long pct_ind, long end_ind) { return parse_ign(pct_ind, pct_ind + 1, end_ind); }

  const V* parse_ign(long pct_ind, long str_ind, long end_ind) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    if (at(str_ind) == '_') return parse_flags(pct_ind, str_ind + 1, end_ind, true);
    return parse_flags(pct_ind, str_ind, end_ind, false);
  }

  const V* parse_flags(long pct_ind, long str_ind, long end_ind, bool ign) {
    bool zero = false, minus = false, plus = false, space = false, hash = false;
    auto set_flag = [&](long i, bool& flag) {
      // in legacy mode, duplicate flags are accepted
      if (flag && !legacy_behavior)
        failwith(head() + ": at character number " + std::to_string(i) + ", duplicate flag " + caml_char(at(i)));
      flag = true;
    };
    for (;;) {
      if (str_ind == end_ind) unexpected_end_of_format(end_ind);
      switch (at(str_ind)) {
        case '0': set_flag(str_ind, zero); break;
        case '-': set_flag(str_ind, minus); break;
        case '+': set_flag(str_ind, plus); break;
        case '#': set_flag(str_ind, hash); break;
        case ' ': set_flag(str_ind, space); break;
        default: return parse_padding(pct_ind, str_ind, end_ind, zero, minus, plus, hash, space, ign);
      }
      str_ind++;
    }
  }

  const V* parse_padding(long pct_ind, long str_ind, long end_ind, bool zero, bool minus, bool plus,
                         bool hash, bool space, bool ign) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    Side padty;
    if (!zero && !minus) padty = Side::Right;
    else if (!zero && minus) padty = Side::Left;
    else if (zero && !minus) padty = Side::Zeros;
    else if (legacy_behavior) padty = Side::Left;
    else incompatible_flag(pct_ind, str_ind, '-', "0");
    char c = at(str_ind);
    if (c >= '0' && c <= '9') {
      auto [new_ind, width] = parse_positive(str_ind, end_ind, 0);
      return parse_after_padding(pct_ind, new_ind, end_ind, minus, plus, hash, space, ign,
                                 Pad{Pad::Kind::Lit_padding, padty, width});
    }
    if (c == '*')
      return parse_after_padding(pct_ind, str_ind + 1, end_ind, minus, plus, hash, space, ign,
                                 Pad{Pad::Kind::Arg_padding, padty});
    switch (padty) {
      case Side::Left:
        if (!legacy_behavior) invalid_format_without(str_ind - 1, '-', "padding");
        return parse_after_padding(pct_ind, str_ind, end_ind, minus, plus, hash, space, ign,
                                   Pad{Pad::Kind::No_padding});
      case Side::Zeros:
        // a '0' padding indication not followed by anything should be
        // interpreted as a Right padding of width 0 (%0s and %0c)
        return parse_after_padding(pct_ind, str_ind, end_ind, minus, plus, hash, space, ign,
                                   Pad{Pad::Kind::Lit_padding, Side::Right, 0});
      case Side::Right:
        return parse_after_padding(pct_ind, str_ind, end_ind, minus, plus, hash, space, ign,
                                   Pad{Pad::Kind::No_padding});
    }
    return nullptr;
  }

  const V* parse_after_padding(long pct_ind, long str_ind, long end_ind, bool minus, bool plus, bool hash,
                               bool space, bool ign, Pad pad) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    char symb = at(str_ind);
    if (symb == '.') return parse_precision(pct_ind, str_ind + 1, end_ind, minus, plus, hash, space, ign, pad);
    return parse_conversion(pct_ind, str_ind + 1, end_ind, plus, hash, space, ign, pad,
                            Prec{Prec::Kind::No_precision}, pad, symb);
  }

  const V* parse_precision(long pct_ind, long str_ind, long end_ind, bool minus, bool plus, bool hash,
                           bool space, bool ign, Pad pad) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    auto parse_lit = [&](bool minus2, long i) {
      auto [new_ind, prec] = parse_positive(i, end_ind, 0);
      return parse_after_precision(pct_ind, new_ind, end_ind, minus2, plus, hash, space, ign, pad,
                                   Prec{Prec::Kind::Lit_precision, prec});
    };
    char c = at(str_ind);
    if (c >= '0' && c <= '9') return parse_lit(minus, str_ind);
    if ((c == '+' || c == '-') && legacy_behavior) return parse_lit(minus || c == '-', str_ind + 1);
    if (c == '*')
      return parse_after_precision(pct_ind, str_ind + 1, end_ind, minus, plus, hash, space, ign, pad,
                                   Prec{Prec::Kind::Arg_precision});
    if (legacy_behavior)
      return parse_after_precision(pct_ind, str_ind, end_ind, minus, plus, hash, space, ign, pad,
                                   Prec{Prec::Kind::Lit_precision, 0});
    invalid_format_without(str_ind - 1, '.', "precision");
  }

  const V* parse_after_precision(long pct_ind, long str_ind, long end_ind, bool minus, bool plus, bool hash,
                                 bool space, bool ign, Pad pad, Prec prec) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    auto parse_conv = [&](Pad padprec) {
      return parse_conversion(pct_ind, str_ind + 1, end_ind, plus, hash, space, ign, pad, prec, padprec,
                              at(str_ind));
    };
    // in legacy mode, %s and %S merge precision into padding
    if (pad.kind == Pad::Kind::No_padding) {
      switch (prec.kind) {
        case Prec::Kind::No_precision: return parse_conv(Pad{Pad::Kind::No_padding});
        case Prec::Kind::Lit_precision:
          return parse_conv(Pad{Pad::Kind::Lit_padding, minus ? Side::Left : Side::Right, prec.n});
        case Prec::Kind::Arg_precision:
          return parse_conv(Pad{Pad::Kind::Arg_padding, minus ? Side::Left : Side::Right});
      }
    }
    return parse_conv(pad);
  }

  const V* parse_conversion(long pct_ind, long str_ind, long end_ind, bool plus, bool hash, bool space,
                            bool ign, Pad pad, Prec prec, Pad padprec, char symb) {
    // Flags used to check option usages/compatibilities.
    bool plus_used = false, hash_used = false, space_used = false, ign_used = false, pad_used = false,
         prec_used = false;
    auto get_plus = [&] { plus_used = true; return plus; };
    auto get_hash = [&] { hash_used = true; return hash; };
    auto get_space = [&] { space_used = true; return space; };
    auto get_ign = [&] { ign_used = true; return ign; };
    auto get_pad = [&] { pad_used = true; return pad; };
    auto get_prec = [&] { prec_used = true; return prec; };
    auto get_padprec = [&] { pad_used = true; return padprec; };

    auto get_int_pad = [&]() -> Pad {
      // match get_pad (), get_prec () with ... (a tuple: get_prec first)
      Prec pr = get_prec();
      Pad p = get_pad();
      if (pr.kind == Prec::Kind::No_precision) return p;
      if (p.kind == Pad::Kind::No_padding) return p;
      if (p.side == Side::Zeros) {
        if (legacy_behavior) return Pad{p.kind, Side::Right, p.width};
        incompatible_flag(pct_ind, str_ind, '0', "precision");
      }
      return p;
    };
    // Check that padty <> Zeros.
    auto check_no_0 = [&](char s, Pad p) -> Pad {
      if (p.kind == Pad::Kind::No_padding || p.side != Side::Zeros) return p;
      if (legacy_behavior) return Pad{p.kind, Side::Right, p.width};
      incompatible_flag(pct_ind, str_ind, s, "0");
    };
    auto opt_of_pad = [&](char c, Pad p) -> std::optional<long> {
      switch (p.kind) {
        case Pad::Kind::No_padding: return std::nullopt;
        case Pad::Kind::Lit_padding:
          switch (p.side) {
            case Side::Right: return p.width;
            case Side::Zeros:
              if (legacy_behavior) return p.width;
              incompatible_flag(pct_ind, str_ind, c, "'0'");
            case Side::Left:
              if (legacy_behavior) return p.width;
              incompatible_flag(pct_ind, str_ind, c, "'-'");
          }
          break;
        case Pad::Kind::Arg_padding: incompatible_flag(pct_ind, str_ind, c, "'*'");
      }
      return std::nullopt;
    };
    auto get_pad_opt = [&](char c) { return opt_of_pad(c, get_pad()); };
    auto get_padprec_opt = [&](char c) { return opt_of_pad(c, get_padprec()); };
    auto get_prec_opt = [&]() -> std::optional<long> {
      Prec p = get_prec();
      switch (p.kind) {
        case Prec::Kind::No_precision: return std::nullopt;
        case Prec::Kind::Lit_precision: return p.n;
        case Prec::Kind::Arg_precision: incompatible_flag(pct_ind, str_ind, '_', "'*'");
      }
      return std::nullopt;
    };
    auto ignored_param = [](const V* ignored, const V* rest) { return constr("Ignored_param", {ignored, rest}); };
    // compute_int_conv pct_ind str_ind (get_plus ()) (get_hash ()) (get_space ()) symb
    auto int_conv = [&](long si, char s) {
      bool sp = get_space(), h = get_hash(), p = get_plus();
      return compute_int_conv(pct_ind, si, p, h, sp, s);
    };

    const V* fmt_result = nullptr;
    auto int_like = [&](std::string_view ctor, std::string_view ign_ctor, const V* iconv, const V* fmt_rest) {
      if (get_ign()) return ignored_param(constr(ign_ctor, {iconv, int_opt(get_pad_opt('_'))}), fmt_rest);
      Prec pr = get_prec();
      Pad p = get_int_pad();
      return constr(ctor, {iconv, mk_padding(p), mk_precision(pr), fmt_rest});
    };
    switch (symb) {
      case ',': fmt_result = parse(str_ind, end_ind); break;
      case 'c': {
        auto char_format = [&](const V* fmt_rest) {
          if (get_ign()) return ignored_param(constr("Ignored_char"), fmt_rest);
          return constr("Char", {fmt_rest});
        };
        auto scan_format = [&](const V* fmt_rest) {
          if (get_ign()) return ignored_param(constr("Ignored_scan_next_char"), fmt_rest);
          return constr("Scan_next_char", {fmt_rest});
        };
        const V* fmt_rest = parse(str_ind, end_ind);
        std::optional<long> po = get_pad_opt('c');
        if (!po) fmt_result = char_format(fmt_rest);
        else if (*po == 0) fmt_result = scan_format(fmt_rest);
        else if (!legacy_behavior) invalid_nonnull_char_width(str_ind);
        else fmt_result = char_format(fmt_rest);  // legacy ignores %c widths
        break;
      }
      case 'C': {
        const V* fmt_rest = parse(str_ind, end_ind);
        fmt_result = get_ign() ? ignored_param(constr("Ignored_caml_char"), fmt_rest) : constr("Caml_char", {fmt_rest});
        break;
      }
      case 's':
      case 'S': {
        Pad p = check_no_0(symb, get_padprec());
        const V* fmt_rest = parse(str_ind, end_ind);
        if (get_ign())
          fmt_result = ignored_param(
              constr(symb == 's' ? "Ignored_string" : "Ignored_caml_string", {int_opt(get_padprec_opt('_'))}),
              fmt_rest);
        else
          fmt_result = constr(symb == 's' ? "String" : "Caml_string", {mk_padding(p), fmt_rest});
        break;
      }
      case 'd': case 'i': case 'x': case 'X': case 'o': case 'u': {
        const V* iconv = int_conv(str_ind, symb);
        const V* fmt_rest = parse(str_ind, end_ind);
        fmt_result = int_like("Int", "Ignored_int", iconv, fmt_rest);
        break;
      }
      case 'N': {
        const V* fmt_rest = parse(str_ind, end_ind);
        const V* counter = constr("Token_counter");
        fmt_result = get_ign() ? ignored_param(constr("Ignored_scan_get_counter", {counter}), fmt_rest)
                               : constr("Scan_get_counter", {counter, fmt_rest});
        break;
      }
      case 'l': case 'n': case 'L': {
        if (str_ind == end_ind || !is_int_base(at(str_ind))) {
          const V* fmt_rest = parse(str_ind, end_ind);
          const V* counter = counter_of_char(symb);
          fmt_result = get_ign() ? ignored_param(constr("Ignored_scan_get_counter", {counter}), fmt_rest)
                                 : constr("Scan_get_counter", {counter, fmt_rest});
          break;
        }
        const V* iconv = int_conv(str_ind + 1, at(str_ind));
        const V* fmt_rest = parse(str_ind + 1, end_ind);
        if (symb == 'l') fmt_result = int_like("Int32", "Ignored_int32", iconv, fmt_rest);
        else if (symb == 'n') fmt_result = int_like("Nativeint", "Ignored_nativeint", iconv, fmt_rest);
        else fmt_result = int_like("Int64", "Ignored_int64", iconv, fmt_rest);
        break;
      }
      case 'f': case 'e': case 'E': case 'g': case 'G': case 'F': case 'h': case 'H': {
        bool sp = get_space(), h = get_hash(), p = get_plus();
        const V* fconv = compute_float_conv(pct_ind, str_ind, p, h, sp, symb);
        const V* fmt_rest = parse(str_ind, end_ind);
        if (get_ign()) {
          // Ignored_float (get_pad_opt '_', get_prec_opt ()): right to left
          std::optional<long> prec_opt = get_prec_opt();
          std::optional<long> pad_opt = get_pad_opt('_');
          fmt_result = ignored_param(constr("Ignored_float", {int_opt(pad_opt), int_opt(prec_opt)}), fmt_rest);
        } else {
          Prec pr = get_prec();
          Pad pd = get_pad();
          fmt_result = constr("Float", {fconv, mk_padding(pd), mk_precision(pr), fmt_rest});
        }
        break;
      }
      case 'b': case 'B': {
        Pad p = check_no_0(symb, get_padprec());
        const V* fmt_rest = parse(str_ind, end_ind);
        if (get_ign())
          fmt_result = ignored_param(constr("Ignored_bool", {int_opt(get_padprec_opt('_'))}), fmt_rest);
        else
          fmt_result = constr("Bool", {mk_padding(p), fmt_rest});
        break;
      }
      case 'a': fmt_result = constr("Alpha", {parse(str_ind, end_ind)}); break;
      case 't': fmt_result = constr("Theta", {parse(str_ind, end_ind)}); break;
      case 'r': {
        const V* fmt_rest = parse(str_ind, end_ind);
        fmt_result = get_ign() ? ignored_param(constr("Ignored_reader"), fmt_rest) : constr("Reader", {fmt_rest});
        break;
      }
      case '!': fmt_result = constr("Flush", {parse(str_ind, end_ind)}); break;
      case '%': case '@': fmt_result = constr("Char_literal", {vchar(symb), parse(str_ind, end_ind)}); break;
      case '{': {
        long sub_end = search_subformat_end(str_ind, end_ind, '}');
        const V* sub_fmt = parse(str_ind, sub_end);
        const V* fmt_rest = parse(sub_end + 2, end_ind);
        const V* sub_fmtty = fmtty_of_fmt(sub_fmt);
        if (get_ign())
          fmt_result = ignored_param(constr("Ignored_format_arg", {int_opt(get_pad_opt('_')), sub_fmtty}), fmt_rest);
        else
          fmt_result = constr("Format_arg", {int_opt(get_pad_opt('{')), sub_fmtty, fmt_rest});
        break;
      }
      case '(': {
        long sub_end = search_subformat_end(str_ind, end_ind, ')');
        const V* fmt_rest = parse(sub_end + 2, end_ind);
        const V* sub_fmt = parse(str_ind, sub_end);
        const V* sub_fmtty = fmtty_of_fmt(sub_fmt);
        if (get_ign())
          fmt_result = ignored_param(constr("Ignored_format_subst", {int_opt(get_pad_opt('_')), sub_fmtty}), fmt_rest);
        else
          fmt_result = constr("Format_subst", {int_opt(get_pad_opt('(')), sub_fmtty, fmt_rest});
        break;
      }
      case '[': {
        auto [next_ind, char_set] = parse_char_set(str_ind, end_ind);
        const V* fmt_rest = parse(next_ind, end_ind);
        if (get_ign())
          fmt_result = ignored_param(
              constr("Ignored_scan_char_set", {int_opt(get_pad_opt('_')), vstring(char_set)}), fmt_rest);
        else
          fmt_result = constr("Scan_char_set", {int_opt(get_pad_opt('[')), vstring(char_set), fmt_rest});
        break;
      }
      case '-': case '+': case '#': case ' ': case '_':
        failwith(head() + ": at character number " + std::to_string(pct_ind) + ", flag " + caml_char(symb) +
                 " is only allowed after the '%', before padding and precision");
      default:
        failwith(head() + ": at character number " + std::to_string(str_ind - 1) +
                 ", invalid conversion \"%" + std::string(1, symb) + "\"");
    }
    // Check for unused options, and reject them as incompatible (not in
    // legacy mode, as the legacy parser silently ignored them).
    if (!legacy_behavior) {
      if (!plus_used && plus) incompatible_flag(pct_ind, str_ind, symb, "'+'");
      if (!hash_used && hash) incompatible_flag(pct_ind, str_ind, symb, "'#'");
      if (!space_used && space) incompatible_flag(pct_ind, str_ind, symb, "' '");
      if (!pad_used && pad.kind != Pad::Kind::No_padding) incompatible_flag(pct_ind, str_ind, symb, "`padding'");
      if (!prec_used && prec.kind != Prec::Kind::No_precision)
        incompatible_flag(pct_ind, str_ind, ign ? '_' : symb, "`precision'");
      if (ign && plus) incompatible_flag(pct_ind, str_ind, '_', "'+'");
    }
    // this last test must not be disabled in legacy mode
    if (!ign_used && ign) {
      bool argless = symb == '@' || symb == '%' || symb == '!' || symb == ',';
      if (!(argless && legacy_behavior)) incompatible_flag(pct_ind, str_ind, symb, "'_'");
    }
    return fmt_result;
  }

  // Parse formatting information (after '@').
  const V* parse_after_at(long str_ind, long end_ind) {
    if (str_ind == end_ind) return constr("Char_literal", {vchar('@'), constr("End_of_format")});
    auto lit = [&](const V* formatting_lit, long next) {
      const V* fmt_rest = parse(next, end_ind);
      return constr("Formatting_lit", {formatting_lit, fmt_rest});
    };
    char c = at(str_ind);
    switch (c) {
      case '[': return parse_tag(false, str_ind + 1, end_ind);
      case ']': return lit(constr("Close_box"), str_ind + 1);
      case '{': return parse_tag(true, str_ind + 1, end_ind);
      case '}': return lit(constr("Close_tag"), str_ind + 1);
      case ',': {
        const V* fmt_rest = parse(str_ind + 1, end_ind);
        return constr("Formatting_lit", {constr("Break", {vstring("@,"), vint(0), vint(0)}), fmt_rest});
      }
      case ' ': {
        const V* fmt_rest = parse(str_ind + 1, end_ind);
        return constr("Formatting_lit", {constr("Break", {vstring("@ "), vint(1), vint(0)}), fmt_rest});
      }
      case ';': return parse_good_break(str_ind + 1, end_ind);
      case '?': return lit(constr("FFlush"), str_ind + 1);
      case '\n': return lit(constr("Force_newline"), str_ind + 1);
      case '.': return lit(constr("Flush_newline"), str_ind + 1);
      case '<': return parse_magic_size(str_ind + 1, end_ind);
      case '@': return lit(constr("Escaped_at"), str_ind + 1);
      case '%':
        if (str_ind + 1 < end_ind && at(str_ind + 1) == '%') return lit(constr("Escaped_percent"), str_ind + 2);
        return constr("Char_literal", {vchar('@'), parse(str_ind, end_ind)});
      default: {
        const V* fmt_rest = parse(str_ind + 1, end_ind);
        return constr("Formatting_lit", {constr("Scan_indic", {vchar(c)}), fmt_rest});
      }
    }
  }

  // Try to read the optional <name> after "@{" or "@[".
  const V* parse_tag(bool is_open_tag, long str_ind, long end_ind) {
    std::string_view gen = is_open_tag ? "Open_tag" : "Open_box";
    if (str_ind != end_ind && at(str_ind) == '<') {
      std::size_t found = str.find('>', static_cast<std::size_t>(str_ind + 1));
      if (found != std::string_view::npos && static_cast<long>(found) < end_ind) {
        long ind = static_cast<long>(found);
        std::string sub_str = sub(str_ind, ind - str_ind + 1);
        const V* fmt_rest = parse(ind + 1, end_ind);
        const V* sub_fmt = parse(str_ind, ind + 1);
        const V* sub_format = constr("Format", {sub_fmt, vstring(sub_str)});
        return constr("Formatting_gen", {constr(gen, {sub_format}), fmt_rest});
      }
    }
    // with Not_found
    const V* fmt_rest = parse(str_ind, end_ind);
    const V* sub_format = constr("Format", {constr("End_of_format"), vstring("")});
    return constr("Formatting_gen", {constr(gen, {sub_format}), fmt_rest});
  }

  // Try to read the optional <width offset> after "@;".
  const V* parse_good_break(long str_ind, long end_ind) {
    long next_ind;
    const V* formatting_lit;
    try {
      if (str_ind == end_ind || at(str_ind) != '<') throw NotFound{};
      long str_ind_1 = parse_spaces(str_ind + 1, end_ind);
      char c1 = at(str_ind_1);
      if (!((c1 >= '0' && c1 <= '9') || c1 == '-')) throw NotFound{};
      auto [str_ind_2, width] = parse_integer(str_ind_1, end_ind);
      long str_ind_3 = parse_spaces(str_ind_2, end_ind);
      char c3 = at(str_ind_3);
      if (c3 == '>') {
        std::string s = sub(str_ind - 2, str_ind_3 - str_ind + 3);
        next_ind = str_ind_3 + 1;
        formatting_lit = constr("Break", {vstring(s), vint(width), vint(0)});
      } else if ((c3 >= '0' && c3 <= '9') || c3 == '-') {
        auto [str_ind_4, offset] = parse_integer(str_ind_3, end_ind);
        long str_ind_5 = parse_spaces(str_ind_4, end_ind);
        if (at(str_ind_5) != '>') throw NotFound{};
        std::string s = sub(str_ind - 2, str_ind_5 - str_ind + 3);
        next_ind = str_ind_5 + 1;
        formatting_lit = constr("Break", {vstring(s), vint(width), vint(offset)});
      } else {
        throw NotFound{};
      }
    } catch (const NotFound&) {
      next_ind = str_ind;
      formatting_lit = constr("Break", {vstring("@;"), vint(1), vint(0)});
    } catch (const Failure&) {
      next_ind = str_ind;
      formatting_lit = constr("Break", {vstring("@;"), vint(1), vint(0)});
    }
    const V* fmt_rest = parse(next_ind, end_ind);
    return constr("Formatting_lit", {formatting_lit, fmt_rest});
  }

  // Parse the size in a <n>.
  const V* parse_magic_size(long str_ind, long end_ind) {
    std::optional<std::pair<long, const V*>> r;
    try {
      long str_ind_1 = parse_spaces(str_ind, end_ind);
      char c1 = at(str_ind_1);
      if ((c1 >= '0' && c1 <= '9') || c1 == '-') {
        auto [str_ind_2, size] = parse_integer(str_ind_1, end_ind);
        long str_ind_3 = parse_spaces(str_ind_2, end_ind);
        if (at(str_ind_3) != '>') throw NotFound{};
        std::string s = sub(str_ind - 2, str_ind_3 - str_ind + 3);
        r.emplace(str_ind_3 + 1, constr("Magic_size", {vstring(s), vint(size)}));
      }
    } catch (const NotFound&) {
      r.reset();
    } catch (const Failure&) {
      r.reset();
    }
    if (r) {
      const V* fmt_rest = parse(r->first, end_ind);
      return constr("Formatting_lit", {r->second, fmt_rest});
    }
    const V* fmt_rest = parse(str_ind, end_ind);
    return constr("Formatting_lit", {constr("Scan_indic", {vchar('<')}), fmt_rest});
  }

  // Parse and construct a char set.
  std::pair<long, std::string> parse_char_set(long str_ind, long end_ind) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    std::string char_set(32, '\0');
    auto add_char = [&](char ch) {
      unsigned ind = static_cast<unsigned char>(ch);
      char_set[ind >> 3] = static_cast<char>(static_cast<unsigned char>(char_set[ind >> 3]) | (1u << (ind & 7)));
    };
    auto add_range = [&](char c, char c2) {
      for (int i = static_cast<unsigned char>(c); i <= static_cast<unsigned char>(c2); i++)
        add_char(static_cast<char>(i));
    };
    auto fail_single_percent = [&](long i) {
      failwith(head() + ": '%' alone is not accepted in character sets, use %% instead at position " +
               std::to_string(i) + ".");
    };
    // parse_char_set_content / parse_char_set_after_char / _after_minus as a
    // state machine; `c` is the pending character of the after_* states
    enum class St { Content, AfterChar, AfterMinus } st;
    char c = 0;
    long i = str_ind;
    bool reverse = false;
    if (at(i) == '^') {
      reverse = true;
      i++;
    }
    // parse_char_set_start
    if (i == end_ind) unexpected_end_of_format(end_ind);
    c = at(i);
    i++;
    st = St::AfterChar;
    long next_ind;
    for (;;) {
      if (i == end_ind) unexpected_end_of_format(end_ind);
      char x = at(i);
      if (st == St::Content) {
        if (x == ']') {
          next_ind = i + 1;
          break;
        }
        if (x == '-') {
          add_char('-');
          i++;
          continue;
        }
        c = x;
        i++;
        st = St::AfterChar;
      } else if (st == St::AfterChar) {
        if (x == ']') {
          add_char(c);
          next_ind = i + 1;
          break;
        }
        if (x == '-') {
          i++;
          st = St::AfterMinus;
          continue;
        }
        if ((x == '%' || x == '@') && c == '%') {
          add_char(x);
          i++;
          st = St::Content;
          continue;
        }
        if (c == '%') fail_single_percent(i);
        add_char(c);
        c = x;
        i++;
      } else {  // AfterMinus
        if (x == ']') {
          add_char(c);
          add_char('-');
          next_ind = i + 1;
          break;
        }
        if (x == '%') {
          if (i + 1 == end_ind) unexpected_end_of_format(end_ind);
          char y = at(i + 1);
          if (y == '%' || y == '@') {
            add_range(c, y);
            i += 2;
            st = St::Content;
            continue;
          }
          fail_single_percent(i);
        }
        add_range(c, x);
        i++;
        st = St::Content;
      }
    }
    if (reverse)
      for (auto& b : char_set) b = static_cast<char>(static_cast<unsigned char>(b) ^ 0xFF);
    return {next_ind, char_set};
  }

  // Consume all next spaces, raise a Failure if end_ind is reached.
  long parse_spaces(long str_ind, long end_ind) {
    for (;;) {
      if (str_ind == end_ind) unexpected_end_of_format(end_ind);
      if (at(str_ind) != ' ') return str_ind;
      str_ind++;
    }
  }

  std::pair<long, long> parse_positive(long str_ind, long end_ind, long acc) {
    for (;;) {
      if (str_ind == end_ind) unexpected_end_of_format(end_ind);
      char c = at(str_ind);
      if (!(c >= '0' && c <= '9')) return {str_ind, acc};
      long new_acc = acc * 10 + (c - '0');
      if (new_acc > max_string_length)
        failwith(head() + ": integer " + std::to_string(new_acc) + " is greater than the limit " +
                 std::to_string(max_string_length));
      acc = new_acc;
      str_ind++;
    }
  }

  std::pair<long, long> parse_integer(long str_ind, long end_ind) {
    if (str_ind == end_ind) unexpected_end_of_format(end_ind);
    char c = at(str_ind);
    if (c >= '0' && c <= '9') return parse_positive(str_ind, end_ind, 0);
    // '-' (the callers checked)
    if (str_ind + 1 == end_ind) unexpected_end_of_format(end_ind);
    char d = at(str_ind + 1);
    if (d >= '0' && d <= '9') {
      auto [next_ind, n] = parse_positive(str_ind + 1, end_ind, 0);
      return {next_ind, -n};
    }
    expected_character(str_ind + 1, "digit", d);
  }

  const V* add_literal(long lit_start, long str_ind, const V* fmt) {
    long size = str_ind - lit_start;
    if (size == 0) return fmt;
    if (size == 1) return constr("Char_literal", {vchar(at(lit_start)), fmt});
    return constr("String_literal", {vstring(sub(lit_start, size)), fmt});
  }

  // Search the end of the current sub-format (the "%}" or "%)")
  long search_subformat_end(long str_ind, long end_ind, char c) {
    for (;;) {
      if (str_ind == end_ind)
        failwith(head() + ": unclosed sub-format, expected \"%" + std::string(1, c) +
                 "\" at character number " + std::to_string(end_ind));
      if (at(str_ind) != '%') {
        str_ind++;
        continue;
      }
      if (str_ind + 1 == end_ind) unexpected_end_of_format(end_ind);
      char n = at(str_ind + 1);
      if (n == c) return str_ind;  // End of format found
      switch (n) {
        case '_': {
          // Search for "%_(" or "%_{".
          if (str_ind + 2 == end_ind) unexpected_end_of_format(end_ind);
          char m = at(str_ind + 2);
          if (m == '{') str_ind = search_subformat_end(str_ind + 3, end_ind, '}') + 2;
          else if (m == '(') str_ind = search_subformat_end(str_ind + 3, end_ind, ')') + 2;
          else str_ind = str_ind + 3;
          break;
        }
        case '{': str_ind = search_subformat_end(str_ind + 2, end_ind, '}') + 2; break;
        case '(': str_ind = search_subformat_end(str_ind + 2, end_ind, ')') + 2; break;
        case '}': expected_character(str_ind + 1, "character ')'", '}');
        case ')': expected_character(str_ind + 1, "character '}'", ')');
        default: str_ind = str_ind + 2;
      }
    }
  }

  static bool is_int_base(char symb) {
    return symb == 'd' || symb == 'i' || symb == 'x' || symb == 'X' || symb == 'o' || symb == 'u';
  }
  static const V* counter_of_char(char symb) {
    switch (symb) {
      case 'l': return constr("Line_counter");
      case 'n': return constr("Char_counter");
      default: return constr("Token_counter");  // 'L'
    }
  }

  // Convert (plus, symb) to its associated int_conv.
  const V* compute_int_conv(long pct_ind, long str_ind, bool plus, bool hash, bool space, char symb) {
    for (;;) {
      std::string_view r;
      if (!plus && !hash && !space) {
        switch (symb) {
          case 'd': r = "Int_d"; break;
          case 'i': r = "Int_i"; break;
          case 'x': r = "Int_x"; break;
          case 'X': r = "Int_X"; break;
          case 'o': r = "Int_o"; break;
          case 'u': r = "Int_u"; break;
        }
      } else if (!plus && !hash && space) {
        if (symb == 'd') r = "Int_sd";
        else if (symb == 'i') r = "Int_si";
      } else if (plus && !hash && !space) {
        if (symb == 'd') r = "Int_pd";
        else if (symb == 'i') r = "Int_pi";
      } else if (!plus && hash && !space) {
        switch (symb) {
          case 'x': r = "Int_Cx"; break;
          case 'X': r = "Int_CX"; break;
          case 'o': r = "Int_Co"; break;
          case 'd': r = "Int_Cd"; break;
          case 'i': r = "Int_Ci"; break;
          case 'u': r = "Int_Cu"; break;
        }
      }
      if (!r.empty()) return constr(r);
      if (hash && symb == 'x' && legacy_behavior) return constr("Int_Cx");
      if (hash && symb == 'X' && legacy_behavior) return constr("Int_CX");
      if (hash && symb == 'o' && legacy_behavior) return constr("Int_Co");
      if (hash && (symb == 'd' || symb == 'i' || symb == 'u')) {
        if (legacy_behavior) {  // ignore
          hash = false;
          continue;
        }
        incompatible_flag(pct_ind, str_ind, symb, "'#'");
      }
      if (plus && space) {
        // plus and space: legacy implementation prefers plus
        if (legacy_behavior) {
          space = false;
          continue;
        }
        incompatible_flag(pct_ind, str_ind, ' ', "'+'");
      }
      if (!plus && space) {
        if (legacy_behavior) {
          space = false;
          continue;
        }
        incompatible_flag(pct_ind, str_ind, symb, "' '");
      }
      if (plus && !space) {
        if (legacy_behavior) {
          plus = false;
          continue;
        }
        incompatible_flag(pct_ind, str_ind, symb, "'+'");
      }
      throw std::logic_error("compute_int_conv");  // assert false
    }
  }

  // Convert (plus, space, symb) to its associated float_conv.
  const V* compute_float_conv(long pct_ind, long str_ind, bool plus, bool hash, bool space, char symb) {
    std::string_view flag;
    if (!plus && !space) flag = "Float_flag_";
    else if (!plus && space) flag = "Float_flag_s";
    else if (plus && !space) flag = "Float_flag_p";
    else if (legacy_behavior) flag = "Float_flag_p";
    else incompatible_flag(pct_ind, str_ind, ' ', "'+'");
    std::string_view kind;
    switch (symb) {
      case 'f': kind = "Float_f"; break;
      case 'e': kind = "Float_e"; break;
      case 'E': kind = "Float_E"; break;
      case 'g': kind = "Float_g"; break;
      case 'G': kind = "Float_G"; break;
      case 'h': kind = "Float_h"; break;
      case 'H': kind = "Float_H"; break;
      default: kind = hash ? "Float_CF" : "Float_F"; break;  // 'F'
    }
    return tuple({constr(flag), constr(kind)});
  }
};

}  // namespace

const FmtValue* fmt_ebb_of_string(bool legacy_behavior, std::string_view str) {
  return Parser(legacy_behavior, str).run();
}

}  // namespace cppcaml::typing::camlinternal_format
