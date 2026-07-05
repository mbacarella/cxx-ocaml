#include "cppcaml/typer.hpp"

#include <unordered_map>
#include <unordered_set>

#include "cppcaml/cmi.hpp"
#include "cppcaml/infer_check.hpp"

namespace cppcaml {
namespace {

namespace tt = typedtree;
using namespace ast;

// The set of value names the implicit `open Stdlib` brings into scope, loaded
// once from stdlib.cmi (relative to the repo root, where the harness runs).
const std::unordered_set<std::string>& stdlib_values() {
  static const std::unordered_set<std::string> s = [] {
    std::unordered_set<std::string> out;
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib.cmi");
      for (auto& v : cmi.values()) out.insert(v.name);
    } catch (...) {
      // No stdlib found: only locals will resolve.
    }
    return out;
  }();
  return s;
}

// Type names Stdlib itself defines (ref, in_channel, format6, ...): an
// unqualified use resolves to Stdlib!.name, like stdlib_values for values.
const std::unordered_set<std::string>& stdlib_types() {
  static const std::unordered_set<std::string> s = [] {
    std::unordered_set<std::string> out;
    try {
      auto cmi = cmi::CmiFile::load("stdlib/stdlib.cmi");
      for (auto& t : cmi.types()) out.insert(t.name);
    } catch (...) {
    }
    return out;
  }();
  return s;
}

// Predefined type constructors (Predef idents, printed name/stamp!).  Stamp
// values are arbitrary post-normalization; only distinctness matters.
const std::unordered_map<std::string, long long>& predef_types() {
  static const std::unordered_map<std::string, long long> m = {
      {"int", 1},      {"char", 2},    {"bytes", 3},   {"float", 4},
      {"bool", 5},     {"unit", 6},    {"exn", 7},     {"array", 8},
      {"list", 9},     {"option", 10}, {"nativeint", 11}, {"int32", 12},
      {"int64", 13},   {"lazy_t", 14}, {"string", 17}, {"floatarray", 16},
      {"extension_constructor", 15},
      {"eff", 18},     {"continuation", 19}, {"iarray", 20},
      {"atomic_loc", 21},
  };
  return m;
}

// ---- format-string desugaring -------------------------------------------
// DRAFT (written 2026-06-05, NOT yet built/verified — pending machine recovery).
// A string literal in a format-typed position becomes
//   CamlinternalFormatBasics.Format(fmt_tree, original_string)
// where fmt_tree is a chain of CamlinternalFormatBasics constructors, all ghost
// at the string's location.  Structure confirmed from `ocamlc -dtypedtree` on
// `Printf.printf "%d\n" 1`.  Conservative: covers literal runs + the common
// plain directives (No_padding/No_precision); ANYTHING with flags/width/
// precision (e.g. %5d, %.3f, %ld, %a) makes `make` return null so the caller
// leaves the argument as a plain string (the file stays a safe DIFF, no
// regression).  TODO before relying on it: verify against the oracle, then add
// padding/precision/%a/%t and unqualified (open Printf) detection.
namespace fmtlib {
inline std::string ns(const std::string& n) { return "CamlinternalFormatBasics." + n; }

inline tt::ExprBox gctor(const std::string& n, std::vector<tt::ExprBox> args,
                         const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  e->desc = tt::Texp_construct{ns(n), std::move(args)};
  return e;
}
inline tt::ExprBox gchar(unsigned char c, const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  ast::Constant k;
  k.loc = g;
  ast::Pconst_char pc;
  pc.code = c;
  k.desc = pc;
  e->desc = tt::Texp_constant{std::move(k)};
  return e;
}
inline tt::ExprBox gstr(const std::string& s, const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  ast::Constant k;
  k.loc = g;
  ast::Pconst_string ps;
  ps.s = s;
  ps.strloc = g;
  k.desc = ps;
  e->desc = tt::Texp_constant{std::move(k)};
  return e;
}
inline tt::ExprBox gint(long n, const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  ast::Constant k;
  k.loc = g;
  ast::Pconst_integer pi;
  pi.value = std::to_string(n);
  k.desc = pi;
  e->desc = tt::Texp_constant{std::move(k)};
  return e;
}

// Build the `padding` sub-tree (No_padding / Lit_padding(padty,w) /
// Arg_padding(padty)).  padty: 0=Right, 1=Left, 2=Zeros.
inline tt::ExprBox padding_node(int padty, bool has_w, bool arg_w, long w,
                                const Location& g) {
  if (!has_w && !arg_w) return gctor("No_padding", {}, g);
  const char* pty = padty == 1 ? "Left" : padty == 2 ? "Zeros" : "Right";
  std::vector<tt::ExprBox> a;
  a.push_back(gctor(pty, {}, g));
  if (arg_w) return gctor("Arg_padding", std::move(a), g);
  a.push_back(gint(w, g));
  return gctor("Lit_padding", std::move(a), g);
}
// Build the `precision` sub-tree.
inline tt::ExprBox precision_node(bool has_p, bool arg_p, long p,
                                  const Location& g) {
  if (!has_p) return gctor("No_precision", {}, g);
  if (arg_p) return gctor("Arg_precision", {}, g);
  std::vector<tt::ExprBox> a;
  a.push_back(gint(p, g));
  return gctor("Lit_precision", std::move(a), g);
}

inline tt::ExprBox parse(const std::string& s, size_t i, const Location& g);

// A plain (unqualified) constructor node -- `None` / `Some` for the pad_opt of
// a sub-format spec (the oracle's mk_int_opt uses Lident, not the
// CamlinternalFormatBasics path).
inline tt::ExprBox pctor(const std::string& n, std::vector<tt::ExprBox> args,
                         const Location& g) {
  auto e = std::make_unique<tt::Expression>();
  e->loc = g;
  e->desc = tt::Texp_construct{n, std::move(args)};
  return e;
}

// fmtty_of_fmt over an already-built fmt tree: the `fmtty` of a `%(...%)`
// sub-format (Int_ty/String_ty/... chain ending in End_of_fmtty).  Conservative
// subset -- pad/prec must be absent (Padding_ty/Precision_ty wrappers and
// Formatting_gen concat are not modelled); null = unsupported (caller bails).
inline tt::ExprBox fmtty_from_tree(const tt::Expression* e, const Location& g) {
  auto wrap1 = [&](const char* ty, const tt::Expression* rest) -> tt::ExprBox {
    auto r = fmtty_from_tree(rest, g);
    if (!r) return nullptr;
    std::vector<tt::ExprBox> a;
    a.push_back(std::move(r));
    return gctor(ty, std::move(a), g);
  };
  auto is0 = [&](const tt::Expression* n, const char* want) {
    auto* c = std::get_if<tt::Texp_construct>(&n->desc);
    return c && c->name == ns(want);
  };
  while (e) {
    auto* c = std::get_if<tt::Texp_construct>(&e->desc);
    if (!c || c->name.rfind("CamlinternalFormatBasics.", 0) != 0) return nullptr;
    std::string n = c->name.substr(25);
    if (n == "End_of_format") return gctor("End_of_fmtty", {}, g);
    // transparent nodes: type flows through to the tail (last arg)
    if (n == "Flush" || n == "String_literal" || n == "Char_literal" ||
        n == "Formatting_lit") { e = c->args.back().get(); continue; }
    if (n == "Char" || n == "Caml_char") return wrap1("Char_ty", c->args[0].get());
    if (n == "String" || n == "Caml_string") {
      if (!is0(c->args[0].get(), "No_padding")) return nullptr;
      return wrap1("String_ty", c->args[1].get());
    }
    if (n == "Int" || n == "Int32" || n == "Nativeint" || n == "Int64" ||
        n == "Float") {
      if (!is0(c->args[1].get(), "No_padding") ||
          !is0(c->args[2].get(), "No_precision")) return nullptr;
      const char* ty = n == "Int" ? "Int_ty" : n == "Int32" ? "Int32_ty"
                     : n == "Nativeint" ? "Nativeint_ty"
                     : n == "Int64" ? "Int64_ty" : "Float_ty";
      return wrap1(ty, c->args[3].get());
    }
    if (n == "Bool") {
      if (!is0(c->args[0].get(), "No_padding")) return nullptr;
      return wrap1("Bool_ty", c->args[1].get());
    }
    if (n == "Alpha") return wrap1("Alpha_ty", c->args[0].get());
    if (n == "Theta") return wrap1("Theta_ty", c->args[0].get());
    if (n == "Reader") return wrap1("Reader_ty", c->args[0].get());
    if (n == "Scan_get_counter") return wrap1("Int_ty", c->args[1].get());
    if (n == "Scan_next_char") return wrap1("Char_ty", c->args[0].get());
    if (n == "Scan_char_set") return wrap1("String_ty", c->args[2].get());
    return nullptr;  // Formatting_gen (needs concat), Format_arg/subst, Ignored_*
  }
  return nullptr;
}

// Find the index of the '%' of the "%<c>" closing a sub-format opened by
// "%(" / "%{" (npos on failure).  Mirrors search_subformat_end, including
// nested sub-formats.
inline size_t subformat_end(const std::string& s, size_t i, char close) {
  while (i < s.size()) {
    if (s[i] != '%') { ++i; continue; }
    if (i + 1 >= s.size()) return std::string::npos;
    char c = s[i + 1];
    if (c == close) return i;
    size_t k = i + 2;                 // index after "%<c>"
    if (c == '_' && k < s.size()) { c = s[k]; ++k; }  // "%_(" / "%_{"
    if (c == '{' || c == '(') {
      size_t sub = subformat_end(s, k, c == '{' ? '}' : ')');
      if (sub == std::string::npos) return std::string::npos;
      i = sub + 2;
    } else if (c == '}' || c == ')') {
      return std::string::npos;       // mismatched closer
    } else {
      i = k;
    }
  }
  return std::string::npos;
}

// Parse a full `%`-spec at s[i] (s[i]=='%'): %[_][flags][width][.prec][length]conv.
// On success returns the fmt node (tail = rest) and sets consumed = #chars used;
// ok=false for any spec we don't desugar (caller bails, leaving a plain string).
inline tt::ExprBox pct_directive(const std::string& s, size_t i, tt::ExprBox rest,
                                 const Location& g, bool& ok, size_t& consumed) {
  ok = false;
  size_t j = i + 1;
  bool ign = false;
  if (j < s.size() && s[j] == '_') { ign = true; ++j; }
  bool f_minus = false, f_zero = false, f_plus = false, f_space = false,
       f_hash = false;
  for (; j < s.size(); ++j) {
    char c = s[j];
    if (c == '-') f_minus = true;
    else if (c == '0') f_zero = true;
    else if (c == '+') f_plus = true;
    else if (c == ' ') f_space = true;
    else if (c == '#') f_hash = true;
    else break;
  }
  bool w_has = false, w_arg = false; long w_val = 0;
  if (j < s.size() && s[j] == '*') { w_arg = true; ++j; }
  else { size_t st = j;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
    if (j > st) { w_has = true; w_val = std::stol(s.substr(st, j - st)); } }
  bool p_has = false, p_arg = false; long p_val = 0;
  if (j < s.size() && s[j] == '.') {
    ++j; p_has = true;
    if (j < s.size() && s[j] == '*') { p_arg = true; ++j; }
    else { size_t st = j;
      while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
      p_val = (j > st) ? std::stol(s.substr(st, j - st)) : 0; }
  }
  // l/L/n are length modifiers only when an int-base conversion follows;
  // otherwise they ARE the conversion (a scan counter, like %N).
  auto int_base = [](char c) {
    return c == 'd' || c == 'i' || c == 'x' || c == 'X' || c == 'o' || c == 'u';
  };
  char len = 0;
  if (j < s.size() && (s[j] == 'l' || s[j] == 'L' || s[j] == 'n') &&
      j + 1 < s.size() && int_base(s[j + 1])) {
    len = s[j]; ++j;
  }
  if (j >= s.size()) return rest;
  char d = s[j];
  consumed = (j - i) + 1;
  // Scan counters (%n %l %N %L, and their %_ ignored forms).
  if (d == 'n' || d == 'l' || d == 'L' || d == 'N') {
    if (len || w_has || w_arg || p_has || f_minus || f_zero || f_plus ||
        f_space || f_hash) return rest;
    const char* cnt = d == 'l' ? "Line_counter"
                    : d == 'n' ? "Char_counter" : "Token_counter";
    std::vector<tt::ExprBox> a;
    if (ign) {
      std::vector<tt::ExprBox> ia;
      ia.push_back(gctor(cnt, {}, g));
      a.push_back(gctor("Ignored_scan_get_counter", std::move(ia), g));
      a.push_back(std::move(rest));
      ok = true;
      return gctor("Ignored_param", std::move(a), g);
    }
    a.push_back(gctor(cnt, {}, g));
    a.push_back(std::move(rest));
    ok = true;
    return gctor("Scan_get_counter", std::move(a), g);
  }
  // Ignored (%_) forms other than counters are not desugared yet.
  if (ign) return rest;
  // Zeros padding needs a width; bail on a bare `0` flag.
  if (f_zero && !w_has && !w_arg) return rest;
  int padty = f_minus ? 1 : f_zero ? 2 : 0;
  auto pad = [&] { return padding_node(padty, w_has, w_arg, w_val, g); };
  auto prec = [&] { return precision_node(p_has, p_arg, p_val, g); };

  switch (d) {
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
      const char* conv = nullptr;
      if (d == 'd') conv = f_plus ? "Int_pd" : f_space ? "Int_sd" : "Int_d";
      else if (d == 'i') conv = f_plus ? "Int_pi" : f_space ? "Int_si" : "Int_i";
      else if (d == 'u') { if (f_plus || f_space || f_hash) return rest; conv = "Int_u"; }
      else if (d == 'x') { if (f_plus || f_space) return rest; conv = f_hash ? "Int_Cx" : "Int_x"; }
      else if (d == 'X') { if (f_plus || f_space) return rest; conv = f_hash ? "Int_CX" : "Int_X"; }
      else { if (f_plus || f_space) return rest; conv = f_hash ? "Int_Co" : "Int_o"; }
      if ((d == 'd' || d == 'i') && f_hash) return rest;
      const char* fam = len == 'l' ? "Int32" : len == 'L' ? "Int64"
                      : len == 'n' ? "Nativeint" : "Int";
      std::vector<tt::ExprBox> a;
      a.push_back(gctor(conv, {}, g));
      a.push_back(pad());
      a.push_back(prec());
      a.push_back(std::move(rest));
      ok = true;
      return gctor(fam, std::move(a), g);
    }
    case 'f': case 'e': case 'g': case 'E': case 'F': case 'h': case 'H': {
      if (len || f_hash) return rest;
      const char* fc = d == 'f' ? "Float_f" : d == 'e' ? "Float_e"
                     : d == 'g' ? "Float_g" : d == 'E' ? "Float_E"
                     : d == 'F' ? "Float_F" : d == 'h' ? "Float_h" : "Float_H";
      const char* fl = f_plus ? "Float_flag_p" : f_space ? "Float_flag_s"
                                                         : "Float_flag_";
      auto fconv = std::make_unique<tt::Expression>();
      fconv->loc = g;
      tt::Texp_tuple ft;
      ft.elems.emplace_back(std::nullopt, gctor(fl, {}, g));
      ft.elems.emplace_back(std::nullopt, gctor(fc, {}, g));
      fconv->desc = std::move(ft);
      std::vector<tt::ExprBox> a;
      a.push_back(std::move(fconv));
      a.push_back(pad());
      a.push_back(prec());
      a.push_back(std::move(rest));
      ok = true;
      return gctor("Float", std::move(a), g);
    }
    case 's': case 'S': {
      if (len || p_has || f_plus || f_space || f_hash || f_zero) return rest;
      std::vector<tt::ExprBox> a;
      a.push_back(pad());
      a.push_back(std::move(rest));
      ok = true;
      return gctor(d == 's' ? "String" : "Caml_string", std::move(a), g);
    }
    case 'c': case 'C': {
      if (len || p_has || w_has || w_arg || f_minus || f_zero || f_plus ||
          f_space || f_hash) return rest;
      std::vector<tt::ExprBox> a;
      a.push_back(std::move(rest));
      ok = true;
      return gctor(d == 'c' ? "Char" : "Caml_char", std::move(a), g);
    }
    case 'b': case 'B': {
      if (len || p_has || f_plus || f_space || f_hash || f_zero) return rest;
      std::vector<tt::ExprBox> a;
      a.push_back(pad());
      a.push_back(std::move(rest));
      ok = true;
      return gctor("Bool", std::move(a), g);
    }
    case 'a': case 't': {  // %a printer / %t thunk
      if (len || p_has || w_has || w_arg || f_minus || f_zero || f_plus ||
          f_space || f_hash) return rest;
      std::vector<tt::ExprBox> a;
      a.push_back(std::move(rest));
      ok = true;
      return gctor(d == 'a' ? "Alpha" : "Theta", std::move(a), g);
    }
    case '@': {  // "%@" -- a literal at sign
      if (len || p_has || w_has || w_arg || f_minus || f_zero || f_plus ||
          f_space || f_hash) return rest;
      std::vector<tt::ExprBox> a;
      a.push_back(gchar('@', g));
      a.push_back(std::move(rest));
      ok = true;
      return gctor("Char_literal", std::move(a), g);
    }
    case '(': {  // "%(...%)" -- format substitution
      // Only a literal right-padding width maps to a pad_opt; anything else
      // (flags, precision, '*') is out of scope.
      if (len || p_has || w_arg || f_minus || f_zero || f_plus || f_space ||
          f_hash) return rest;
      size_t sub_end = subformat_end(s, j + 1, ')');
      if (sub_end == std::string::npos) return rest;
      auto sub = parse(s.substr(j + 1, sub_end - (j + 1)), 0, g);
      if (!sub) return rest;
      auto fmtty = fmtty_from_tree(sub.get(), g);
      if (!fmtty) return rest;
      tt::ExprBox pad_opt;
      if (w_has) {
        std::vector<tt::ExprBox> sa;
        sa.push_back(gint(w_val, g));
        pad_opt = pctor("Some", std::move(sa), g);
      } else {
        pad_opt = pctor("None", {}, g);
      }
      consumed = (sub_end + 2) - i;  // through the closing "%)"
      std::vector<tt::ExprBox> a;
      a.push_back(std::move(pad_opt));
      a.push_back(std::move(fmtty));
      a.push_back(std::move(rest));
      ok = true;
      return gctor("Format_subst", std::move(a), g);
    }
    default: return rest;
  }
}

// Parse s[i..] into the fmt tree; null if it contains anything unsupported.
inline tt::ExprBox parse(const std::string& s, size_t i, const Location& g) {
  if (i >= s.size()) return gctor("End_of_format", {}, g);
  if (s[i] == '%') {
    if (i + 1 >= s.size()) return nullptr;
    char d = s[i + 1];
    if (d == '%') {
      auto r = parse(s, i + 2, g);
      if (!r) return nullptr;
      return gctor("Char_literal", [&] { std::vector<tt::ExprBox> a; a.push_back(gchar('%', g)); a.push_back(std::move(r)); return a; }(), g);
    }
    if (d == '!') {
      auto r = parse(s, i + 2, g);
      if (!r) return nullptr;
      std::vector<tt::ExprBox> a; a.push_back(std::move(r));
      return gctor("Flush", std::move(a), g);
    }
    bool ok; size_t consumed = 0;
    // peek: build with a placeholder rest only after we know the spec parses.
    // We parse the spec first to learn its length, then recurse for the tail.
    {
      // Trial parse with an empty tail just to measure `consumed` and validity.
      bool tok; size_t tlen = 0;
      auto probe = pct_directive(s, i, gctor("End_of_format", {}, g), g, tok, tlen);
      (void)probe;
      if (!tok) return nullptr;
      consumed = tlen;
    }
    auto r = parse(s, i + consumed, g);
    if (!r) return nullptr;
    auto e = pct_directive(s, i, std::move(r), g, ok, consumed);
    return ok ? std::move(e) : nullptr;
  }
  if (s[i] == '@') {
    // A Format formatting directive; mirrors camlinternalFormat's
    // parse_after_at / parse_tag / parse_good_break / parse_magic_size.
    auto lit_then = [&](tt::ExprBox lit, size_t next) -> tt::ExprBox {
      auto r = parse(s, next, g);
      if (!r) return nullptr;
      std::vector<tt::ExprBox> args;
      args.push_back(std::move(lit));
      args.push_back(std::move(r));
      return gctor("Formatting_lit", std::move(args), g);
    };
    auto break_node = [&](const std::string& org, long ns_, long ni) {
      std::vector<tt::ExprBox> b;
      b.push_back(gstr(org, g));
      b.push_back(gint(ns_, g));
      b.push_back(gint(ni, g));
      return gctor("Break", std::move(b), g);
    };
    // "<w [o]>" scanner shared by @; and @< (spaces allowed, ints may be
    // negative).  On success sets the out-params and returns true.
    auto angle_ints = [&](size_t j, long& v1, bool& has2, long& v2,
                          size_t& after) -> bool {
      auto spaces = [&](size_t k) { while (k < s.size() && s[k] == ' ') ++k; return k; };
      auto integer = [&](size_t k, long& out) -> size_t {  // 0 = fail
        size_t st = k;
        if (k < s.size() && s[k] == '-') ++k;
        size_t d0 = k;
        while (k < s.size() && std::isdigit((unsigned char)s[k])) ++k;
        if (k == d0) return 0;
        out = std::stol(s.substr(st, k - st));
        return k;
      };
      j = spaces(j);
      j = integer(j, v1);
      if (!j) return false;
      j = spaces(j);
      if (j < s.size() && s[j] == '>') { has2 = false; after = j + 1; return true; }
      j = integer(j, v2);
      if (!j) return false;
      j = spaces(j);
      if (j >= s.size() || s[j] != '>') return false;
      has2 = true; after = j + 1;
      return true;
    };
    if (i + 1 >= s.size()) {  // lone trailing '@' is a literal
      std::vector<tt::ExprBox> a;
      a.push_back(gchar('@', g));
      a.push_back(gctor("End_of_format", {}, g));
      return gctor("Char_literal", std::move(a), g);
    }
    char c = s[i + 1];
    switch (c) {
      case '[': case '{': {  // open box / open tag, optional <name> sub-format
        std::string sub_str;
        tt::ExprBox sub_fmt;
        size_t next = i + 2;
        if (next < s.size() && s[next] == '<') {
          size_t gt = s.find('>', next + 1);
          if (gt != std::string::npos) {
            sub_str = s.substr(next, gt - next + 1);
            sub_fmt = parse(sub_str, 0, g);
            if (!sub_fmt) return nullptr;
            next = gt + 1;
          }
        }
        if (!sub_fmt) sub_fmt = gctor("End_of_format", {}, g);
        std::vector<tt::ExprBox> fa;
        fa.push_back(std::move(sub_fmt));
        fa.push_back(gstr(sub_str, g));
        std::vector<tt::ExprBox> ga;
        ga.push_back(gctor("Format", std::move(fa), g));
        auto gen = gctor(c == '{' ? "Open_tag" : "Open_box", std::move(ga), g);
        auto r = parse(s, next, g);
        if (!r) return nullptr;
        std::vector<tt::ExprBox> args;
        args.push_back(std::move(gen));
        args.push_back(std::move(r));
        return gctor("Formatting_gen", std::move(args), g);
      }
      case ']': return lit_then(gctor("Close_box", {}, g), i + 2);
      case '}': return lit_then(gctor("Close_tag", {}, g), i + 2);
      case ',': return lit_then(break_node("@,", 0, 0), i + 2);
      case ' ': return lit_then(break_node("@ ", 1, 0), i + 2);
      case ';': {  // "@;" or "@;<width [offset]>"
        long w = 0, off = 0; bool has2 = false; size_t after = 0;
        if (i + 2 < s.size() && s[i + 2] == '<' &&
            angle_ints(i + 3, w, has2, off, after))
          return lit_then(
              break_node(s.substr(i, after - i), w, has2 ? off : 0), after);
        return lit_then(break_node("@;", 1, 0), i + 2);
      }
      case '?': return lit_then(gctor("FFlush", {}, g), i + 2);
      case '\n': return lit_then(gctor("Force_newline", {}, g), i + 2);
      case '.': return lit_then(gctor("Flush_newline", {}, g), i + 2);
      case '<': {  // "@<size>" magic size, else a '<' scan indication
        long sz = 0, dummy = 0; bool has2 = false; size_t after = 0;
        if (angle_ints(i + 2, sz, has2, dummy, after) && !has2) {
          std::vector<tt::ExprBox> ma;
          ma.push_back(gstr(s.substr(i, after - i), g));
          ma.push_back(gint(sz, g));
          return lit_then(gctor("Magic_size", std::move(ma), g), after);
        }
        std::vector<tt::ExprBox> sa;
        sa.push_back(gchar('<', g));
        return lit_then(gctor("Scan_indic", std::move(sa), g), i + 2);
      }
      case '@': return lit_then(gctor("Escaped_at", {}, g), i + 2);
      case '%':
        if (i + 2 < s.size() && s[i + 2] == '%')
          return lit_then(gctor("Escaped_percent", {}, g), i + 3);
        else {  // "@%<conv>": the '@' is a plain char; reparse from the '%'
          auto r = parse(s, i + 1, g);
          if (!r) return nullptr;
          std::vector<tt::ExprBox> a;
          a.push_back(gchar('@', g));
          a.push_back(std::move(r));
          return gctor("Char_literal", std::move(a), g);
        }
      default: {  // any other char is a scan indication
        std::vector<tt::ExprBox> sa;
        sa.push_back(gchar(c, g));
        return lit_then(gctor("Scan_indic", std::move(sa), g), i + 2);
      }
    }
  }
  size_t k = i;
  while (k < s.size() && s[k] != '%' && s[k] != '@') ++k;
  auto r = parse(s, k, g);
  if (!r) return nullptr;
  std::vector<tt::ExprBox> a;
  if (k - i == 1) {
    a.push_back(gchar(static_cast<unsigned char>(s[i]), g));
    a.push_back(std::move(r));
    return gctor("Char_literal", std::move(a), g);
  }
  a.push_back(gstr(s.substr(i, k - i), g));
  a.push_back(std::move(r));
  return gctor("String_literal", std::move(a), g);
}

// Build the CamlinternalFormatBasics.Format(...) wrapper, or null if the format
// uses anything we don't yet desugar.
inline tt::ExprBox make(const std::string& s, const Location& sloc) {
  Location g = sloc;
  g.ghost = true;
  auto tree = parse(s, 0, g);
  if (!tree) return nullptr;
  std::vector<tt::ExprBox> a;
  a.push_back(std::move(tree));
  a.push_back(gstr(s, g));
  return gctor("Format", std::move(a), sloc);  // outer wrapper keeps the real loc
}
}  // namespace fmtlib

// True when a path's root identifier is a global (cmi-loaded) module, e.g.
// `Stdlib!.Set.Make` -- but not a locally-bound `Mods/279.F`.
bool path_root_global(const tt::Path& p) {
  if (auto* pi = std::get_if<tt::Pident>(&p.v))
    return pi->id.kind == tt::Ident::Global;
  if (auto* pd = std::get_if<tt::Pdot>(&p.v))
    return path_root_global(*pd->prefix);
  return false;
}

// Build the Stdlib path Stdlib!.name (Pdot over a global Stdlib ident).
tt::Path stdlib_path(const std::string& name) {
  auto pre = std::make_shared<tt::Path>();
  pre->v = tt::Pident{tt::Ident{"Stdlib", 0, tt::Ident::Global}};
  tt::Path p;
  p.v = tt::Pdot{pre, name};
  return p;
}

struct Typer {
  // Side-table from the inference pass: match nodes that are non-exhaustive.
  const std::unordered_map<const ast::Expression*, bool>* partiality = nullptr;
  // Bare `function ..` nodes that are non-exhaustive (keyed by Pfunction_cases).
  const std::unordered_map<const void*, bool>* function_cases_partial = nullptr;
  // Param patterns' exhaustiveness (keyed by the AST pattern node).
  const std::unordered_map<const void*, bool>* param_partial = nullptr;
  // Slice 3: per Pexp_apply, the reconstructed argument slots (consumed at
  // Pexp_apply in step 3).
  const std::unordered_map<const ast::Expression*, std::vector<applymatch::Slot>>*
      apply_plans = nullptr;
  // Construct nodes whose argument tuple flattens (resolved arity>1, incl. cmi
  // constructors that the local ctor_arity_ registry can't see).
  const std::unordered_set<const void*>* flatten_construct = nullptr;
  // Functional record-update nodes -> the EXTERNAL record type's ordered field
  // list (for <kept> fields; local records use field_registry).
  const std::unordered_map<const ast::Expression*, std::vector<std::string>>*
      record_fields = nullptr;
  // Parallel to record_fields: an external record node's non-default repr
  // (Record_float); absent => Record_regular.
  const std::unordered_map<const ast::Expression*, std::string>*
      record_reprs = nullptr;
  // String-literal nodes inferred at a format type: desugared to the
  // CamlinternalFormatBasics.Format(...) tree in the dump (type-directed, so it
  // catches unqualified/open'd/let-bound formats the syntactic path can't).
  const std::set<const ast::Expression*>* format_lits = nullptr;
  long long next_stamp = 274;  // arbitrary base; the harness normalizes stamps
  // Scope frames mapping value name -> local ident; innermost last.
  std::vector<std::unordered_map<std::string, tt::Ident>> scopes{{}};

  // Type constructors, submodules, and module types (own namespaces).
  std::unordered_map<std::string, tt::Ident> type_scope;
  std::unordered_map<std::string, tt::Ident> module_scope;
  std::unordered_map<std::string, tt::Ident> modtype_scope;

  // Names a locally-declared module type exports (one level deep), keyed by the
  // modtype ident's stamp.  `S with type t = ..` prints the constrained item
  // with the SIGNATURE's own ident (t/274 in both places), so the with-clause
  // needs to look idents up inside S rather than mint new ones.
  struct SigExports {
    std::unordered_map<std::string, tt::Ident> types, modules;
  };
  std::unordered_map<long long, SigExports> modtype_exports_;

  // Names a locally-defined module exports: for `open M` of a local module
  // (resolution through the open's path, like "Std/1.Hash") and for
  // `open F(X)` / `open struct .. end` (fresh-ident instantiation).
  // Submodules link by stamp so dotted opens can walk down.
  struct ModExports {
    std::unordered_set<std::string> values, types;
    std::unordered_map<std::string, long long> submodule_stamps;
  };
  std::unordered_map<long long, ModExports> module_exports_;
  // Virtual stamps for module-type submodules whose members we track but which
  // never appear in the typed tree (so they must not consume next_stamp and
  // shift real idents).  Kept in a high range that can't collide with the small
  // per-file real stamps; only ever used as module_exports_ keys, never printed.
  long long virt_next_ = (1LL << 30);
  long long register_virtual_module(ModExports ex) {
    long long vs = virt_next_++;
    module_exports_[vs] = std::move(ex);
    return vs;
  }
  // What each `include ME` contributed to its enclosing structure (keyed by the
  // Tstr_include's ModuleExpr): lets collect_module_exports see path/cmi
  // includes, whose members aren't recoverable from the typed items alone.
  std::unordered_map<const tt::ModuleExpr*, ModExports> include_exports_;
  // Local module stamps defined as an alias to a module path -- `module S = P`
  // (for any P, global or local).  Used as a functor argument, or as the root
  // of a functor path (`S.Make(..)`), such an alias is strengthened just like a
  // direct global path (a transparent coercion layer in the typed tree).
  std::set<long long> alias_module_stamps_;
  // Module idents bound as a functor PARAMETER: aliasing one (`module M = X`)
  // stays Mp_present (the parameter has no static address to strengthen away),
  // unlike a plain `module M = P` alias which is Mp_absent.
  std::set<long long> functor_param_stamps_;
  // Named module types by name -> their AST body (for resolving `X : T` params
  // when T is a named `module type T = sig .. end`).
  std::unordered_map<std::string, const ast::ModuleType*> modtype_ast_;

  static void pat_var_names(const tt::Pattern& p,
                            std::unordered_set<std::string>& out) {
    if (auto* v = std::get_if<tt::Tpat_var>(&p.desc)) {
      out.insert(v->id.name);
    } else if (auto* a = std::get_if<tt::Tpat_alias>(&p.desc)) {
      out.insert(a->id.name);
      pat_var_names(*a->inner, out);
    } else if (auto* t = std::get_if<tt::Tpat_tuple>(&p.desc)) {
      for (auto& [l, e] : t->elems) pat_var_names(*e, out);
    } else if (auto* c = std::get_if<tt::Tpat_construct>(&p.desc)) {
      for (auto& e : c->args) pat_var_names(*e, out);
    } else if (auto* r = std::get_if<tt::Tpat_record>(&p.desc)) {
      for (auto& [n, e] : r->fields) pat_var_names(*e, out);
    } else if (auto* ar = std::get_if<tt::Tpat_array>(&p.desc)) {
      for (auto& e : ar->elems) pat_var_names(*e, out);
    } else if (auto* o = std::get_if<tt::Tpat_or>(&p.desc)) {
      pat_var_names(*o->left, out);
    } else if (auto* lz = std::get_if<tt::Tpat_lazy>(&p.desc)) {
      pat_var_names(*lz->inner, out);
    } else if (auto* vv = std::get_if<tt::Tpat_variant>(&p.desc)) {
      if (vv->arg) pat_var_names(*vv->arg, out);
    }
  }

  void collect_module_exports(const std::vector<tt::StructureItem>& items,
                              ModExports& ex) {
    for (auto& it : items) {
      if (auto* v = std::get_if<tt::Tstr_value>(&it.desc)) {
        for (auto& b : v->bindings) pat_var_names(b.pat, ex.values);
      } else if (auto* p = std::get_if<tt::Tstr_primitive>(&it.desc)) {
        ex.values.insert(p->id.name);
      } else if (auto* t = std::get_if<tt::Tstr_type>(&it.desc)) {
        for (auto& d : t->decls) ex.types.insert(d.id.name);
      } else if (auto* m = std::get_if<tt::Tstr_module>(&it.desc)) {
        ex.submodule_stamps[m->id.name] = m->id.stamp;
      } else if (auto* inc = std::get_if<tt::Tstr_include>(&it.desc)) {
        auto f = include_exports_.find(inc->expr.get());
        if (f != include_exports_.end()) {
          for (auto& n : f->second.values) ex.values.insert(n);
          for (auto& n : f->second.types) ex.types.insert(n);
          for (auto& [n, st] : f->second.submodule_stamps)
            ex.submodule_stamps[n] = st;
        } else if (auto* body = module_body(*inc->expr)) {
          collect_module_exports(*body, ex);
        }
      }
    }
  }

  // The structure a module expression evaluates to, syntactically: through
  // functor bodies (their RESULT) and constraints.  Null when opaque.
  static const std::vector<tt::StructureItem>* module_body(const tt::ModuleExpr& me) {
    if (auto* s = std::get_if<tt::Tmod_structure>(&me.desc)) return &s->items;
    if (auto* f = std::get_if<tt::Tmod_functor>(&me.desc)) return module_body(*f->body);
    if (auto* c = std::get_if<tt::Tmod_constraint>(&me.desc)) return module_body(*c->expr);
    return nullptr;
  }

  // Exports of the local module a (possibly dotted) resolved path denotes.
  const ModExports* exports_by_path(const tt::Path& p) {
    if (auto* pi = std::get_if<tt::Pident>(&p.v)) {
      auto f = module_exports_.find(pi->id.stamp);
      return f != module_exports_.end() ? &f->second : nullptr;
    }
    if (auto* d = std::get_if<tt::Pdot>(&p.v)) {
      const ModExports* pre = exports_by_path(*d->prefix);
      if (!pre) return nullptr;
      auto s = pre->submodule_stamps.find(d->name);
      if (s == pre->submodule_stamps.end()) return nullptr;
      auto f = module_exports_.find(s->second);
      return f != module_exports_.end() ? &f->second : nullptr;
    }
    return nullptr;
  }

  // Exports of an elaborated module expression (for generalized opens).
  const ModExports* exports_of_modexpr(const tt::ModuleExpr& me, ModExports& tmp) {
    if (auto* i = std::get_if<tt::Tmod_ident>(&me.desc)) {
      if (auto* pi = std::get_if<tt::Pident>(&i->path.v)) {
        auto f = module_exports_.find(pi->id.stamp);
        if (f != module_exports_.end()) return &f->second;
      }
      return nullptr;
    }
    if (auto* a = std::get_if<tt::Tmod_apply>(&me.desc)) {
      // A cmi functor's application: the members come from the functor's
      // RESULT signature (Set.Make -> Set.S with elt fixed).  Peel the
      // strengthening constraint the apply path was wrapped in.
      const tt::ModuleExpr* fn = &*a->fn;
      while (auto* c = std::get_if<tt::Tmod_constraint>(&fn->desc))
        fn = &*c->expr;
      if (auto* i = std::get_if<tt::Tmod_ident>(&fn->desc))
        if (path_root_global(i->path)) {
          std::vector<std::string> comps;
          if (tt_path_comps(i->path, comps) && comps.size() >= 2) {
            std::string fname = std::move(comps.back());
            comps.pop_back();
            if (const cmi::Signature* pre = cmi_sig_of_comps(comps))
              for (auto& m : pre->modules)
                if (m.name == fname) {
                  const cmi::ModuleType* ft = m.type.get();
                  while (ft && ft->kind == cmi::ModuleType::Functor)
                    ft = ft->functor_body.get();  // curried: peel to the result
                  if (const cmi::Signature* rs = cmi_resolve_mt(ft, pre, 0)) {
                    modexports_of_cmisig(*rs, tmp);
                    return &tmp;
                  }
                }
          }
        }
      return exports_of_modexpr(*a->fn, tmp);
    }
    if (auto* c = std::get_if<tt::Tmod_constraint>(&me.desc))
      return exports_of_modexpr(*c->expr, tmp);
    if (auto* body = module_body(me)) {
      collect_module_exports(*body, tmp);
      return &tmp;
    }
    return nullptr;
  }

  static void collect_exports(const tt::ModuleType& mt, SigExports& ex) {
    auto* sg = std::get_if<tt::Tmty_signature>(&mt.desc);
    if (!sg) return;
    for (auto& it : sg->items) {
      if (auto* t = std::get_if<tt::Tsig_type>(&it.desc)) {
        for (auto& d : t->decls) ex.types.emplace(d.id.name, d.id);
      } else if (auto* m = std::get_if<tt::Tsig_module>(&it.desc)) {
        ex.modules.emplace(m->md.id.name, m->md.id);
      }
    }
  }

  // Exports of an elaborated module type: a local Tmty_ident goes through the
  // recorded table; an inline signature is walked directly.
  SigExports exports_of(const tt::ModuleType& mt) {
    if (auto* bi = std::get_if<tt::Tmty_ident>(&mt.desc)) {
      if (auto* pi = std::get_if<tt::Pident>(&bi->path.v)) {
        auto f = modtype_exports_.find(pi->id.stamp);
        if (f != modtype_exports_.end()) return f->second;
      }
      return {};
    }
    SigExports ex;
    collect_exports(mt, ex);
    return ex;
  }

  // Record fields: name -> the record type's full field list (decl order) +
  // representation.  Drives Texp_record's decl-order emission and <kept> fields.
  struct RecordInfo {
    std::vector<std::string> decl_fields;
    std::string repr = "Record_regular";
  };
  std::unordered_map<std::string, RecordInfo> field_registry;
  // Local type names whose manifest resolves to `float` (`type t = [private]
  // float`, transitively): a record all of whose fields have such a type gets
  // the Record_float representation, like a bare-`float` record.
  std::set<std::string> float_abbrevs_;

  // Locally-declared constructor -> number of constructors in its variant
  // (drives param-pattern partiality: the sole ctor is irrefutable).
  std::unordered_map<std::string, size_t> ctor_siblings_;
  // Constructor -> a per-variant-declaration group id, so an or-pattern can be
  // checked for covering ALL siblings of ONE type (`A|B` over `type t = A | B`
  // is exhaustive, hence irrefutable).
  std::unordered_map<std::string, int> ctor_group_;
  int ctor_group_seq_ = 0;

  // Locally-declared constructors of arity > 1 (`C of t1 * t2`): name -> arity.
  // A constructor of arity n applied to an n-tuple flattens its argument in the
  // typedtree (`C (a, b)` -> args [a; b]).  Keyed by simple name; external
  // (cmi) constructors aren't here, so they keep the single (tuple) argument.
  std::unordered_map<std::string, int> ctor_arity_;
  // Locally-declared type constructors: simple name -> parameter count.  Drives
  // the `_ t` expansion (a lone `_` argument fills every arity slot).
  std::unordered_map<std::string, int> type_arity_;

  static std::string lid_last(const Longident& x) {
    if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&x.v)) return p->name;
    return "?";
  }

  // Index of the format-string argument for a qualified Printf/Format/Scanf
  // call (draft: qualified only, to avoid mis-converting shadowed names), or -1.
  static int format_arg_index(const Longident& fn) {
    auto* d = std::get_if<Ldot>(&fn.v);
    if (!d) return -1;
    std::string mod = lid_last(*d->prefix);
    if (mod != "Printf" && mod != "Format" && mod != "Scanf") return -1;
    const std::string& n = d->name;
    if (n == "printf" || n == "eprintf" || n == "sprintf" || n == "asprintf" ||
        n == "dprintf" || n == "scanf")
      return 0;
    if (n == "fprintf" || n == "ifprintf" || n == "bprintf" || n == "ksprintf" ||
        n == "kprintf" || n == "kasprintf" || n == "sscanf" || n == "bscanf")
      return 1;
    if (n == "kfprintf" || n == "kbprintf") return 2;
    return -1;
  }

  // Module-level `open M`: names exported by M resolve through M's path.
  struct OpenEntry {
    tt::Path path;
    std::unordered_set<std::string> values;
    std::unordered_set<std::string> types;
    std::unordered_set<std::string> submodules;  // for `open M; Sub.x` -> M.Sub.x
  };
  std::vector<OpenEntry> opens;

  // `open M.Sub[..]`: walk the cmi signature graph (aliases, named module
  // types, an explicit Stdlib root all resolve — see the walker below).
  void load_open_names_lid(const Longident& lid, OpenEntry& oe) {
    std::vector<std::string> comps;
    if (!lid_comps(lid, comps)) return;
    const cmi::Signature* sig = cmi_sig_of_comps(comps);
    if (!sig) return;
    for (auto& v : sig->values) oe.values.insert(v.name);
    for (auto& t : sig->types) oe.types.insert(t.name);
    for (auto& m : sig->modules) oe.submodules.insert(m.name);
  }

  // Build an OpenEntry for a module path (stdlib cmi names + local exports),
  // resolving its path.  Used by local opens in expressions, patterns and types.
  OpenEntry make_open_entry(const Longident& lid) {
    OpenEntry oe;
    oe.path = resolve_module(lid);
    load_open_names_lid(lid, oe);
    if (const ModExports* ex = exports_by_path(oe.path)) {
      for (auto& n : ex->values) oe.values.insert(n);
      for (auto& n : ex->types) oe.types.insert(n);
      for (auto& [n, st] : ex->submodule_stamps) oe.submodules.insert(n);
    }
    return oe;
  }

  // --- cmi name-level module resolution ------------------------------------
  // General walker over cmi signatures: dotted module paths (Stdlib-rooted or
  // unit-rooted), module ALIASES (stdlib.cmi's Float -> Stdlib__Float), named
  // MODULE TYPES (Digest.S), and functor RESULTS (Set.Make's sig).  Name-level
  // only: it answers "what members does this thing have".
  std::unordered_map<std::string, cmi::CmiFile> cmi_units_;  // keeps sigs alive
  const cmi::CmiFile* cmi_unit(const std::string& unit) {
    auto it = cmi_units_.find(unit);
    if (it != cmi_units_.end()) return &it->second;
    try {
      std::string path = unit == "Stdlib" ? "stdlib/stdlib.cmi"
                                          : "stdlib/stdlib__" + unit + ".cmi";
      return &cmi_units_.emplace(unit, cmi::CmiFile::load(path)).first->second;
    } catch (...) {
      return nullptr;
    }
  }
  static bool tt_path_comps(const tt::Path& p, std::vector<std::string>& out) {
    if (auto* pi = std::get_if<tt::Pident>(&p.v)) {
      out.push_back(pi->id.name);
      return true;
    }
    if (auto* d = std::get_if<tt::Pdot>(&p.v)) {
      if (!tt_path_comps(*d->prefix, out)) return false;
      out.push_back(d->name);
      return true;
    }
    return false;
  }
  static bool lid_comps(const Longident& lid, std::vector<std::string>& out) {
    if (auto* l = std::get_if<Lident>(&lid.v)) { out.push_back(l->name); return true; }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      if (!lid_comps(*d->prefix, out)) return false;
      out.push_back(d->name);
      return true;
    }
    return false;  // Lapply
  }
  // Resolve a cmi module type to a concrete signature.  `home` is the
  // signature whose modtypes a bare Ident path (same-unit module type) names.
  const cmi::Signature* cmi_resolve_mt(const cmi::ModuleType* mt,
                                       const cmi::Signature* home, int depth) {
    if (!mt || depth > 8) return nullptr;
    switch (mt->kind) {
      case cmi::ModuleType::Sig:
        return mt->sig.get();
      case cmi::ModuleType::Alias:
        return cmi_sig_of_cmipath(mt->path.get(), depth + 1);
      case cmi::ModuleType::Ident: {
        const cmi::Path* p = mt->path.get();
        if (!p) return nullptr;
        if (p->kind == cmi::Path::Pident) {
          if (!home) return nullptr;
          for (auto& d : home->modtypes)
            if (d.name == p->id.name)
              return cmi_resolve_mt(d.type.get(), home, depth + 1);
          return nullptr;
        }
        if (p->kind == cmi::Path::Pdot) {
          const cmi::Signature* pre = cmi_sig_of_cmipath(p->a.get(), depth + 1);
          if (!pre) return nullptr;
          for (auto& d : pre->modtypes)
            if (d.name == p->s) return cmi_resolve_mt(d.type.get(), pre, depth + 1);
          return nullptr;
        }
        return nullptr;
      }
      case cmi::ModuleType::Functor:
        return nullptr;  // callers peel functor_body explicitly
    }
    return nullptr;
  }
  // Signature of the global module a cmi Path denotes ("Stdlib__Float",
  // Pdot(Stdlib, "Sys"), ...).
  const cmi::Signature* cmi_sig_of_cmipath(const cmi::Path* p, int depth) {
    if (!p || depth > 8) return nullptr;
    if (p->kind == cmi::Path::Pident) {
      std::string n = p->id.name;
      if (n.rfind("Stdlib__", 0) == 0) n = n.substr(8);
      const cmi::CmiFile* f = cmi_unit(n);
      return f ? &f->sig() : nullptr;
    }
    if (p->kind == cmi::Path::Pdot) {
      const cmi::Signature* pre = cmi_sig_of_cmipath(p->a.get(), depth + 1);
      if (!pre) return nullptr;
      for (auto& m : pre->modules)
        if (m.name == p->s) return cmi_resolve_mt(m.type.get(), pre, depth + 1);
      return nullptr;
    }
    return nullptr;
  }
  // Signature of the module a dotted source path denotes, rooted at a stdlib
  // unit name or an explicit "Stdlib".
  const cmi::Signature* cmi_sig_of_comps(const std::vector<std::string>& comps) {
    if (comps.empty()) return nullptr;
    const cmi::CmiFile* f = cmi_unit(comps[0]);
    if (!f) return nullptr;
    const cmi::Signature* sig = &f->sig();
    for (size_t i = 1; i < comps.size() && sig; ++i) {
      const cmi::ModuleDecl* md = nullptr;
      for (auto& m : sig->modules)
        if (m.name == comps[i]) { md = &m; break; }
      sig = md ? cmi_resolve_mt(md->type.get(), sig, 0) : nullptr;
    }
    return sig;
  }
  void modexports_of_cmisig(const cmi::Signature& sig, ModExports& ex,
                            int depth = 0) {
    for (auto& v : sig.values) ex.values.insert(v.name);
    for (auto& t : sig.types) ex.types.insert(t.name);
    for (auto& m : sig.modules) {
      ModExports sub;
      if (depth < 6)
        if (const cmi::Signature* s = cmi_resolve_mt(m.type.get(), &sig, 0))
          modexports_of_cmisig(*s, sub, depth + 1);
      ex.submodule_stamps.emplace(m.name, register_virtual_module(std::move(sub)));
    }
  }

  // --- name-level members of a module TYPE ----------------------------------
  // For binding sites whose members come from a signature rather than a
  // structure: functor parameters, module-rec constraints, first-class
  // package unpacks.  Local named module types are recorded here as they are
  // declared; cmi-side ones resolve through the walker above.
  std::unordered_map<long long, ModExports> modtype_members_;
  void members_of_modtype_lid(const Longident& lid, ModExports& ex, int depth) {
    if (depth > 8) return;
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto m = modtype_scope.find(l->name);
      if (m != modtype_scope.end()) {
        auto f = modtype_members_.find(m->second.stamp);
        if (f == modtype_members_.end()) return;
        for (auto& n : f->second.values) ex.values.insert(n);
        for (auto& n : f->second.types) ex.types.insert(n);
        for (auto& [n, st] : f->second.submodule_stamps)
          ex.submodule_stamps.emplace(n, st);
        return;
      }
      return;
    }
    std::vector<std::string> comps;  // dotted: a cmi module type
    if (!lid_comps(lid, comps) || comps.size() < 2) return;
    std::string last = std::move(comps.back());
    comps.pop_back();
    if (const cmi::Signature* pre = cmi_sig_of_comps(comps))
      for (auto& d : pre->modtypes)
        if (d.name == last)
          if (const cmi::Signature* s = cmi_resolve_mt(d.type.get(), pre, depth))
            modexports_of_cmisig(*s, ex);
  }
  // Register the inline-record constructors of a signature's type declarations
  // into field_registry (Record_inlined <block-tag>), mirroring type_kind but
  // persisting past the signature's own scope -- so a functor body referencing
  // `X.B { .. }` (B an inline-record ctor from param X's sig) gets the repr.
  void register_sig_inline_records(const ast::ModuleType& mt) {
    if (auto* id = std::get_if<Pmty_ident>(&mt.desc)) {  // a named `module type T`
      if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
        auto f = modtype_ast_.find(l->name);
        if (f != modtype_ast_.end()) register_sig_inline_records(*f->second);
      }
      return;
    }
    auto* sg = std::get_if<Pmty_signature>(&mt.desc);
    if (!sg) return;
    for (auto& it : sg->items) {
      auto* t = std::get_if<Psig_type>(&it.desc);
      if (!t) continue;
      for (auto& d : t->decls) {
        auto* v = std::get_if<Ptype_variant>(&d.kind);
        if (!v) continue;
        int block_idx = 0;
        for (auto& c : v->ctors) {
          if (auto* r = std::get_if<Pcstr_record>(&c.args)) {
            RecordInfo info;
            for (auto& f : r->fields) info.decl_fields.push_back(f.name.txt);
            info.repr = "Record_inlined " + std::to_string(block_idx);
            for (auto& f : r->fields) field_registry[f.name.txt] = info;
          }
          auto* tp = std::get_if<Pcstr_tuple>(&c.args);
          if (!(tp && tp->elems.empty())) ++block_idx;
        }
      }
    }
  }
  void members_of_ast_modtype(const ast::ModuleType& mt, ModExports& ex,
                              int depth) {
    if (depth > 8) return;
    if (auto* sg = std::get_if<Pmty_signature>(&mt.desc)) {
      for (auto& it : sg->items) {
        if (auto* v = std::get_if<Psig_value>(&it.desc)) {
          ex.values.insert(v->vd.name.txt);
        } else if (auto* t = std::get_if<Psig_type>(&it.desc)) {
          for (auto& d : t->decls) ex.types.insert(d.name.txt);
        } else if (auto* m = std::get_if<Psig_module>(&it.desc)) {
          if (m->md.name.txt) {
            ModExports sub;
            members_of_ast_modtype(*m->md.type, sub, depth + 1);
            ex.submodule_stamps.emplace(*m->md.name.txt,
                                        register_virtual_module(std::move(sub)));
          }
        } else if (auto* inc = std::get_if<Psig_include>(&it.desc)) {
          members_of_ast_modtype(inc->mt, ex, depth + 1);
        }
      }
    } else if (auto* id = std::get_if<Pmty_ident>(&mt.desc)) {
      members_of_modtype_lid(id->id.txt, ex, depth + 1);
    } else if (auto* w = std::get_if<Pmty_with>(&mt.desc)) {
      members_of_ast_modtype(*w->mt, ex, depth + 1);  // constraints keep names
    }
  }

  // When typing a single pattern tree, a variable NAME resolves to one Ident.
  // The only way a name recurs in a well-formed pattern is across the branches
  // of an or-pattern, where OCaml gives both occurrences the SAME stamp.  So
  // while this map is active (set for the duration of one case/binding lhs),
  // fresh_local reuses the first ident minted for a name -- reproducing OCaml's
  // or-pattern variable sharing, including nested ors.
  std::unordered_map<std::string, tt::Ident>* pat_vars_ = nullptr;

  tt::Ident fresh_local(const std::string& name) {
    if (pat_vars_) {
      auto it = pat_vars_->find(name);
      if (it != pat_vars_->end()) {
        scopes.back()[name] = it->second;
        return it->second;
      }
    }
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    scopes.back()[name] = id;
    if (pat_vars_) (*pat_vars_)[name] = id;
    return id;
  }
  tt::Ident fresh_anon(const std::string& name) {  // stamped, not scoped
    return tt::Ident{name, next_stamp++, tt::Ident::Local};
  }
  // Instance variables of the class currently being transcribed: a bare name in
  // a method body that is one of these (and not shadowed by a local) is an
  // instance-variable reference (Texp_instvar), not an ordinary identifier.
  std::unordered_map<std::string, tt::Ident> instvars_;
  std::unordered_map<std::string, tt::Ident> class_scope_;  // class names -> Ident
  // Method self-sends: each class' methods get a shared ident (the "meths" table).
  // A send whose object is the enclosing self resolves the method to its ident
  // (Tmeth_val); every other send prints the name only (Tmeth_name).
  using MethsMap = std::unordered_map<std::string, tt::Ident>;
  std::unordered_map<int, std::shared_ptr<MethsMap>> self_meths_;  // self-ident stamp -> class' meths
  // Per-class (by class-ident stamp) instance-variable and method names in
  // declaration order, INCLUDING inherited ones -- so `inherit P` can bring P's
  // vals/methods into a subclass (fresh instvar idents; ancestor method idents).
  std::unordered_map<int, std::vector<std::string>> class_vals_;
  std::unordered_map<int, std::vector<std::string>> class_methods_;
  // Local polymorphic-variant abbreviations (`type t = [ `A | `B of u | .. ]`):
  // a `#t` pattern expands to the or-pattern of these tags.
  struct PolyTag { std::string name; bool has_arg; };
  std::unordered_map<int, std::vector<PolyTag>> polyvar_tags_;  // type-ident stamp -> tags
  // OCaml's polymorphic-variant tag hash (btype.ml hash_variant): rows sort by it.
  static int hash_variant(const std::string& s) {
    unsigned accu = 0;
    for (char c : s) accu = 223 * accu + static_cast<unsigned char>(c);
    accu &= (1u << 31) - 1;
    return accu > 0x3FFFFFFF ? static_cast<int>(accu) - (1 << 31)
                             : static_cast<int>(accu);
  }
  tt::Path resolve_class(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto it = class_scope_.find(l->name);
      if (it != class_scope_.end()) {
        tt::Path p;
        p.v = tt::Pident{it->second};
        return p;
      }
      // A class from an opened module resolves through the open's path.
      for (auto rit = opens.rbegin(); rit != opens.rend(); ++rit)
        if (rit->types.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(rit->path), l->name};
          return p;
        }
      throw TypeError("Unbound class " + l->name);
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {  // M.c class path
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("qualified class path");
  }
  bool is_local(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
      if (it->count(name)) return true;
    return false;
  }
  const tt::Ident* local_ident(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  // Local `external` bound to the %apply / %revapply primitive (stamp -> kind:
  // 2 = %apply like `@@`, 1 = %revapply like `|>`).  Applications through such
  // an external are rewritten to a plain Texp_apply, same as Stdlib's operators.
  std::unordered_map<long long, int> prim_apply_stamp_;
  // Mirrors typecore's `turn_let_into_match`: a `let pat = e in body` is typed as
  // `match e with pat -> body` (and dumped as Texp_match) when the pattern
  // contains a constructor, an open tuple, or a labelled tuple.
  bool turn_let_into_match(const ast::Pattern& p) {
    if (std::holds_alternative<Ppat_construct>(p.desc)) return true;
    if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) {
      if (t->closed == ClosedFlag::Open) return true;
      for (auto& l : t->labels) if (l.has_value()) return true;
      for (auto& e : t->elems) if (turn_let_into_match(*e)) return true;
    } else if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      for (auto& e : a->elems) if (turn_let_into_match(*e)) return true;
    } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      return turn_let_into_match(*o->l) || turn_let_into_match(*o->r);
    } else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      return turn_let_into_match(*al->p);
    } else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) {
      return turn_let_into_match(*c->p);
    } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      for (auto& f : r->fields) if (turn_let_into_match(*f.second)) return true;
    } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
      return turn_let_into_match(*lz->p);
    } else if (auto* op = std::get_if<Ppat_open>(&p.desc)) {
      return turn_let_into_match(*op->p);
    } else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
      if (v->arg) return turn_let_into_match(**v->arg);
    }
    return false;
  }
  // Conservative syntactic irrefutability (drives the synthetic match's Total/
  // Partial flag): only true when CERTAIN -- so an uncertain single-constructor
  // type yields Partial (a safe over-approximation that can't falsely accept).
  bool pat_irrefutable(const ast::Pattern& p) {
    if (std::holds_alternative<Ppat_any>(p.desc) ||
        std::holds_alternative<Ppat_var>(p.desc)) return true;
    if (auto* c = std::get_if<Ppat_construct>(&p.desc)) {
      if (std::holds_alternative<Lident>(c->id.txt.v) &&
          lid_last(c->id.txt) == "()" && !c->arg)
        return true;  // unit is irrefutable
      // Sole ctor of a local variant (bare or module-qualified, `Stdlib.B`).
      auto s = ctor_siblings_.find(lid_last(c->id.txt));
      if (s != ctor_siblings_.end() && s->second == 1)
        return !c->arg || pat_irrefutable(**c->arg);
      return false;
    }
    if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) {
      if (t->closed == ClosedFlag::Open) return false;
      for (auto& e : t->elems) if (!pat_irrefutable(*e)) return false;
      return true;
    }
    if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      for (auto& f : r->fields) if (!pat_irrefutable(*f.second)) return false;
      return true;
    }
    if (auto* al = std::get_if<Ppat_alias>(&p.desc)) return pat_irrefutable(*al->p);
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return pat_irrefutable(*c->p);
    if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) return pat_irrefutable(*lz->p);
    if (auto* op = std::get_if<Ppat_open>(&p.desc)) return pat_irrefutable(*op->p);
    if (std::holds_alternative<Ppat_unpack>(p.desc)) return true;  // (module M)
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      // An or-pattern with an irrefutable branch (`M.Ex | _`) always matches.
      if (pat_irrefutable(*o->l) || pat_irrefutable(*o->r)) return true;
      // `A | B` over `type t = A | B` is exhaustive -> irrefutable.  Collect the
      // distinct constructors named in the or-tree; they are irrefutable iff they
      // all belong to ONE variant and together cover every sibling (each with an
      // irrefutable argument).
      std::set<std::string> names;
      int group = -1;
      if (or_ctor_cover(p, names, group) && group >= 0 &&
          !names.empty() && names.size() == ctor_siblings_[*names.begin()])
        return true;
      return false;
    }
    return false;
  }

  // Walk an or-pattern tree; every leaf must be a bare-Lident constructor of the
  // same variant `group`, with an irrefutable argument.  Records their names.
  bool or_ctor_cover(const ast::Pattern& p, std::set<std::string>& names,
                     int& group) {
    if (auto* o = std::get_if<Ppat_or>(&p.desc))
      return or_ctor_cover(*o->l, names, group) &&
             or_ctor_cover(*o->r, names, group);
    if (auto* op = std::get_if<Ppat_open>(&p.desc))
      return or_ctor_cover(*op->p, names, group);
    if (auto* c = std::get_if<Ppat_construct>(&p.desc)) {
      auto* l = std::get_if<Lident>(&c->id.txt.v);
      if (!l) return false;
      auto g = ctor_group_.find(l->name);
      if (g == ctor_group_.end()) return false;
      if (group == -1) group = g->second;
      else if (group != g->second) return false;
      if (c->arg && !pat_irrefutable(**c->arg)) return false;
      names.insert(l->name);
      return true;
    }
    return false;
  }

  // `a |> b` (%revapply) and `b @@ a` (%apply) are rewritten by the typer to the
  // application `b a`.  Returns 1 for an unshadowed Stdlib `|>`, 2 for `@@`, else
  // 0 -- gated on the operator resolving to Stdlib (not a local/opened rebinding).
  int revapply_kind(const ast::Expression& fn) {
    auto* id = std::get_if<Pexp_ident>(&fn.desc);
    if (!id) return 0;
    auto* l = std::get_if<Lident>(&id->id.txt.v);
    if (!l) return 0;
    // A local external redefining the operator via %apply/%revapply (apply.ml).
    if (const tt::Ident* li = local_ident(l->name)) {
      auto it = prim_apply_stamp_.find(li->stamp);
      return it != prim_apply_stamp_.end() ? it->second : 0;
    }
    int kind = l->name == "|>" ? 1 : l->name == "@@" ? 2 : 0;
    if (!kind) return 0;
    tt::Path p = resolve_value(id->id.txt, fn.loc.start.cnum);
    auto* d = std::get_if<tt::Pdot>(&p.v);
    if (!d || d->name != l->name) return 0;
    auto* pi = std::get_if<tt::Pident>(&d->prefix->v);
    return (pi && pi->id.name == "Stdlib") ? kind : 0;
  }
  tt::Ident fresh_type(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    type_scope[name] = id;
    return id;
  }
  tt::Ident fresh_module(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    module_scope[name] = id;
    return id;
  }
  tt::Ident fresh_modtype(const std::string& name) {
    tt::Ident id{name, next_stamp++, tt::Ident::Local};
    modtype_scope[name] = id;
    return id;
  }
  void push() { scopes.emplace_back(); }
  void pop() { scopes.pop_back(); }

  // Resolve a type constructor: local type, else predef, else qualified path.
  tt::Path resolve_type(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto t = type_scope.find(l->name);
      if (t != type_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{t->second};
        return p;
      }
      // Opens shadow predefs (e.g. Effect.Deep.continuation over the predef
      // continuation), so they resolve first.
      for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
        if (it->types.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(it->path), l->name};
          return p;
        }
      }
      auto pd = predef_types().find(l->name);
      if (pd != predef_types().end()) {
        tt::Path p;
        p.v = tt::Pident{tt::Ident{l->name, pd->second, tt::Ident::Predef}};
        return p;
      }
      if (stdlib_types().count(l->name)) return stdlib_path(l->name);
      throw TypeError("Unbound type constructor " + l->name);
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported type path");
  }

  tt::PackageType package_type(const Ptyp_package& p) {
    tt::PackageType out;
    out.path = resolve_modtype(p.path.txt);
    for (auto& [lid, ct] : p.constraints)
      out.constraints.emplace_back(
          lid_str(lid.txt), std::make_unique<tt::CoreType>(core_type(*ct)));
    return out;
  }

  tt::CoreType core_type(const CoreType& t) {
    tt::CoreType out;
    out.loc = t.loc;
    if (!t.attrs.empty()) out.attrs = &t.attrs;
    if (std::holds_alternative<Ptyp_any>(t.desc)) {
      out.desc = tt::Ttyp_any{};
    } else if (auto* v = std::get_if<Ptyp_var>(&t.desc)) {
      out.desc = tt::Ttyp_var{v->name};
    } else if (auto* a = std::get_if<Ptyp_arrow>(&t.desc)) {
      out.desc = tt::Ttyp_arrow{a->label,
                                std::make_unique<tt::CoreType>(core_type(*a->dom)),
                                std::make_unique<tt::CoreType>(core_type(*a->cod))};
    } else if (auto* tu = std::get_if<Ptyp_tuple>(&t.desc)) {
      tt::Ttyp_tuple tup;
      for (size_t k = 0; k < tu->elems.size(); ++k) {
        std::optional<std::string> label;
        if (k < tu->labels.size()) label = tu->labels[k];
        tup.elems.emplace_back(
            label, std::make_unique<tt::CoreType>(core_type(*tu->elems[k])));
      }
      out.desc = std::move(tup);
    } else if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      tt::Ttyp_constr tc;
      tc.path = resolve_type(c->id.txt);
      // A lone `_` argument to an N-ary constructor (`_ t`, t of arity N) fills
      // every slot: N copies of Ttyp_any, all at the written `_`'s location.
      int expand = 0;
      if (c->args.size() == 1 &&
          std::holds_alternative<Ptyp_any>(c->args[0]->desc)) {
        auto ar = type_arity_.find(lid_last(c->id.txt));
        if (ar != type_arity_.end() && ar->second != 1) expand = ar->second;
      }
      if (expand)
        for (int k = 0; k < expand; ++k)
          tc.args.push_back(std::make_unique<tt::CoreType>(core_type(*c->args[0])));
      else
        for (auto& arg : c->args)
          tc.args.push_back(std::make_unique<tt::CoreType>(core_type(*arg)));
      out.desc = std::move(tc);
    } else if (auto* cl = std::get_if<Ptyp_class>(&t.desc)) {  // `[args] #class`
      tt::Ttyp_class tc;
      tc.path = resolve_class(cl->id.txt);
      for (auto& arg : cl->args)
        tc.args.push_back(std::make_unique<tt::CoreType>(core_type(*arg)));
      out.desc = std::move(tc);
    } else if (auto* al = std::get_if<Ptyp_alias>(&t.desc)) {
      out.desc = tt::Ttyp_alias{al->name,
                                std::make_unique<tt::CoreType>(core_type(*al->type))};
    } else if (auto* po = std::get_if<Ptyp_poly>(&t.desc)) {
      out.desc = tt::Ttyp_poly{po->vars,
                               std::make_unique<tt::CoreType>(core_type(*po->type))};
    } else if (auto* pv = std::get_if<Ptyp_variant>(&t.desc)) {
      tt::Ttyp_variant tv;
      tv.closed = pv->closed == ClosedFlag::Closed;
      for (auto& row : pv->rows) {
        if (auto* rt = std::get_if<Rtag>(&row)) {
          tt::Ttag tag{rt->name, rt->constant, {}};
          for (auto& ty : rt->types)
            tag.types.push_back(std::make_unique<tt::CoreType>(core_type(*ty)));
          tv.rows.push_back({std::move(tag)});
        } else {  // Rinherit: a `[ t | ... ]` inheritance row
          auto& ri = std::get<Rinherit>(row);
          tv.rows.push_back(
              {tt::Tinherit{std::make_unique<tt::CoreType>(core_type(*ri.ct))}});
        }
      }
      if (pv->labels) tv.labels = *pv->labels;
      out.desc = std::move(tv);
    } else if (auto* ob = std::get_if<Ptyp_object>(&t.desc)) {
      tt::Ttyp_object to;
      to.closed = ob->closed == ClosedFlag::Closed;
      for (auto& f : ob->fields) {
        if (auto* ot = std::get_if<Otag>(&f))
          to.fields.push_back({tt::OTmethod{
              ot->name.txt, std::make_unique<tt::CoreType>(poly_wrap(*ot->type))}});
        else
          to.fields.push_back({tt::OTinherit{std::make_unique<tt::CoreType>(
              core_type(*std::get<Oinherit>(f).type))}});
      }
      out.desc = std::move(to);
    } else if (auto* pk = std::get_if<Ptyp_package>(&t.desc)) {
      out.desc = tt::Ttyp_package{package_type(*pk)};
    } else if (auto* op = std::get_if<Ptyp_open>(&t.desc)) {  // M.(t)
      tt::Ttyp_open to;
      to.path = resolve_module(op->mod_.txt);
      auto saved = opens;
      OpenEntry oe = make_open_entry(op->mod_.txt);
      // The local open shadows outer type bindings of the same name, so `N.(t)`
      // resolves `t` to `N.t` even when an outer `type t` exists.  Temporarily
      // remove the shadowed names so resolve_type falls through to the open.
      std::vector<std::pair<std::string, tt::Ident>> shadowed;
      for (auto& n : oe.types) {
        auto it = type_scope.find(n);
        if (it != type_scope.end()) {
          shadowed.emplace_back(n, it->second);
          type_scope.erase(it);
        }
      }
      opens.push_back(std::move(oe));
      to.type = std::make_unique<tt::CoreType>(core_type(*op->type));
      opens = std::move(saved);
      for (auto& [n, id] : shadowed) type_scope[n] = id;
      out.desc = std::move(to);
    } else {
      throw TypeError("coretype#" + std::to_string(t.desc.index()));
    }
    return out;
  }

  // The typedtree analogue of Typ.varify_constructors: replace every nullary
  // Ttyp_constr on a locally-abstract type (a `type a` newtype ident) with the
  // corresponding Ttyp_var.  Used for the `: type a. t` pattern annotation.
  void varify_tt(tt::CoreType& t, const std::unordered_map<int, std::string>& nt) {
    if (auto* c = std::get_if<tt::Ttyp_constr>(&t.desc)) {
      if (c->args.empty())
        if (auto* pid = std::get_if<tt::Pident>(&c->path.v)) {
          auto f = nt.find(pid->id.stamp);
          if (f != nt.end()) { t.desc = tt::Ttyp_var{f->second}; return; }
        }
      for (auto& a : c->args) varify_tt(*a, nt);
    } else if (auto* a = std::get_if<tt::Ttyp_arrow>(&t.desc)) {
      varify_tt(*a->dom, nt);
      varify_tt(*a->cod, nt);
    } else if (auto* tu = std::get_if<tt::Ttyp_tuple>(&t.desc)) {
      for (auto& [l, e] : tu->elems) varify_tt(*e, nt);
    } else if (auto* al = std::get_if<tt::Ttyp_alias>(&t.desc)) {
      varify_tt(*al->type, nt);
    } else if (auto* po = std::get_if<tt::Ttyp_poly>(&t.desc)) {
      varify_tt(*po->type, nt);
    }
  }

  // Record fields (and object methods) wrap their type in Ttyp_poly([], inner) --
  // unless it is already an explicit `'a. t` polymorphic type, which stays as-is.
  tt::CoreType poly_wrap(const CoreType& t) {
    if (std::holds_alternative<Ptyp_poly>(t.desc)) return core_type(t);
    tt::CoreType inner = core_type(t);
    tt::CoreType poly;
    poly.loc = inner.loc;
    poly.desc = tt::Ttyp_poly{{}, std::make_unique<tt::CoreType>(std::move(inner))};
    return poly;
  }

  std::vector<tt::CoreTypeBox> ctor_args(const ConstructorArguments& a) {
    std::vector<tt::CoreTypeBox> out;
    if (auto* t = std::get_if<Pcstr_tuple>(&a)) {
      for (auto& el : t->elems)
        out.push_back(std::make_unique<tt::CoreType>(core_type(*el)));
    } else {
      throw TypeError("inline record constructor");
    }
    return out;
  }

  // `external f [: t] = g`: the alias g resolves as a value.
  tt::Path resolve_prim_alias(const StringLoc& alias) {
    if (alias.txt.find('.') != std::string::npos)
      throw TypeError("dotted primitive alias");
    Longident lid;
    lid.v = Lident{alias.txt};
    return resolve_value(lid, alias.loc.start.cnum);
  }

  // Extension-constructor idents by name (`exception E = F` resolves F here).
  std::unordered_map<std::string, tt::Ident> extctor_scope_;

  tt::ExtCtor ext_ctor(const ExtensionConstructor& c) {
    tt::ExtCtor out;
    out.loc = c.loc;
    out.id = fresh_anon(c.name.txt);
    if (auto* decl = std::get_if<Pext_decl>(&c.kind)) {
      if (auto* r = std::get_if<Pcstr_record>(&decl->args)) {
        RecordInfo info;  // `exception E of {..}` / `t += E of {..}` -> extension
        info.repr = "Record_extension \"" + out.id.name + "/" +
                    std::to_string(out.id.stamp) + "\"";
        for (auto& f : r->fields) {
          out.labels.push_back(label_decl(f));
          info.decl_fields.push_back(f.name.txt);
        }
        for (auto& f : r->fields) field_registry[f.name.txt] = info;
      } else {
        out.args = ctor_args(decl->args);
      }
      if (decl->res)
        out.res = std::make_unique<tt::CoreType>(core_type(**decl->res));
    } else {
      auto& rb = std::get<Pext_rebind>(c.kind);
      tt::Path p;
      if (auto* l = std::get_if<Lident>(&rb.id.txt.v)) {  // `E = F` (local)
        auto f = extctor_scope_.find(l->name);
        if (f == extctor_scope_.end()) throw TypeError("extension rebind unknown");
        p.v = tt::Pident{f->second};
      } else if (auto* d = std::get_if<Ldot>(&rb.id.txt.v)) {  // `E = M.F`
        tt::Path prefix = resolve_module(*d->prefix);
        p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      } else {
        throw TypeError("extension rebind path");
      }
      out.rebind = std::move(p);
    }
    extctor_scope_[c.name.txt] = out.id;
    if (!c.attrs.empty()) out.attrs = &c.attrs;
    return out;
  }

  tt::ConstructorDecl constructor_decl(const ConstructorDecl& c) {
    tt::ConstructorDecl out;
    out.loc = c.loc;
    out.id = fresh_anon(c.name.txt);
    if (auto* r = std::get_if<Pcstr_record>(&c.args)) {
      for (auto& f : r->fields) out.labels.push_back(label_decl(f));
    } else {
      out.args = ctor_args(c.args);
    }
    if (c.res) out.res = std::make_unique<tt::CoreType>(core_type(**c.res));
    if (auto* t = std::get_if<Pcstr_tuple>(&c.args))
      if (t->elems.size() > 1) ctor_arity_[c.name.txt] = (int)t->elems.size();
    if (!c.attrs.empty()) out.attrs = &c.attrs;
    return out;
  }

  tt::LabelDecl label_decl(const LabelDecl& f) {
    tt::LabelDecl out;
    out.loc = f.loc;
    out.mutable_ = f.mut == MutableFlag::Mutable;
    for (auto& a : f.attrs)  // `mutable x : t [@atomic]` (fork)
      if (a.name == "atomic") out.atomic = true;
    out.id = fresh_anon(f.name.txt);
    out.type = poly_wrap(*f.type);
    if (!f.attrs.empty()) out.attrs = &f.attrs;
    return out;
  }

  tt::TypeKind type_kind(const TypeKind& k) {
    tt::TypeKind out;
    if (std::holds_alternative<Ptype_abstract>(k)) {
      out.v = tt::Ttype_abstract{};
    } else if (auto* v = std::get_if<Ptype_variant>(&k)) {
      tt::Ttype_variant tv;
      int block_idx = 0;  // index among non-constant ctors (the inlined tag)
      // GADT variants (any ctor with a return type) refine at use sites, so a
      // single-ctor pattern can be total: count them as 1 (never Partial).
      bool gadt = false;
      for (auto& c : v->ctors) gadt = gadt || c.res.has_value();
      int group = ctor_group_seq_++;
      for (auto& c : v->ctors) {
        ctor_siblings_[c.name.txt] = gadt ? 1 : v->ctors.size();
        ctor_group_[c.name.txt] = group;
      }
      for (auto& c : v->ctors) {
        if (auto* r = std::get_if<Pcstr_record>(&c.args)) {
          RecordInfo info;
          for (auto& f : r->fields) info.decl_fields.push_back(f.name.txt);
          info.repr = "Record_inlined " + std::to_string(block_idx);
          for (auto& f : r->fields) field_registry[f.name.txt] = info;
        }
        auto* t = std::get_if<Pcstr_tuple>(&c.args);
        if (!(t && t->elems.empty())) ++block_idx;
        tv.ctors.push_back(constructor_decl(c));
      }
      out.v = std::move(tv);
    } else if (auto* r = std::get_if<Ptype_record>(&k)) {
      tt::Ttype_record tr;
      for (auto& f : r->fields) tr.labels.push_back(label_decl(f));
      out.v = std::move(tr);
    } else if (std::holds_alternative<Ptype_open>(k)) {
      out.v = tt::Ttype_open{};
    } else {
      throw TypeError("type_external kind");
    }
    return out;
  }

  tt::TypeDeclaration type_declaration(
      const TypeDeclaration& d,
      std::optional<tt::Ident> id = std::nullopt) {
    tt::TypeDeclaration td;
    // For a `nonrec` group the declaration's own ident is passed explicitly (the
    // name has been rebound to the OUTER type in type_scope so the body resolves
    // there); otherwise the name maps to this declaration.
    td.id = id ? *id : type_scope.at(d.name.txt);
    td.loc = d.loc;
    td.attrs = &d.attrs;
    for (auto& p : d.params)
      td.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
    td.kind = type_kind(d.kind);
    td.private_ = d.priv == PrivateFlag::Private;
    for (auto& c : d.constraints)
      td.constraints.push_back({std::make_unique<tt::CoreType>(core_type(*c.t1)),
                                std::make_unique<tt::CoreType>(core_type(*c.t2)),
                                c.loc});
    if (d.manifest) {
      td.manifest = std::make_unique<tt::CoreType>(core_type(**d.manifest));
      // Record a polymorphic-variant abbreviation's tag set (for `#t` patterns).
      if (auto* pv = std::get_if<Ptyp_variant>(&(*d.manifest)->desc)) {
        std::vector<PolyTag> tags;
        collect_polyvar_tags(*pv, tags);
        polyvar_tags_[td.id.stamp] = std::move(tags);
      }
    }
    return td;
  }
  // Flatten a polyvariant manifest's tags (Rtag direct, Rinherit via the
  // referenced local abbreviation).
  void collect_polyvar_tags(const Ptyp_variant& pv, std::vector<PolyTag>& out) {
    for (auto& row : pv.rows) {
      if (auto* rt = std::get_if<Rtag>(&row)) {
        out.push_back({rt->name, !rt->constant});
      } else if (auto* ri = std::get_if<Rinherit>(&row)) {
        if (auto* c = std::get_if<Ptyp_constr>(&ri->ct->desc))
          if (auto* l = std::get_if<Lident>(&c->id.txt.v)) {
            auto t = type_scope.find(l->name);
            if (t != type_scope.end()) {
              auto f = polyvar_tags_.find(t->second.stamp);
              if (f != polyvar_tags_.end())
                for (auto& pt : f->second) out.push_back(pt);
            }
          }
      }
    }
  }

  // A `private` type whose manifest is an OPEN row -- an open polymorphic
  // variant (`[> ]` / `[< ]`) or an open object (`< ..; .. >`) -- gets a hidden
  // companion `t#row` type_declaration (abstract, private, no manifest), emitted
  // (and stamped) BEFORE the real `t`.  Matches OCaml's fixed-row abbreviations.
  bool needs_row_type(const TypeDeclaration& d) {
    if (d.priv != PrivateFlag::Private || !d.manifest) return false;
    auto& m = **d.manifest;
    if (auto* pv = std::get_if<Ptyp_variant>(&m.desc))
      return pv->closed == ClosedFlag::Open || pv->labels.has_value();
    if (auto* ob = std::get_if<Ptyp_object>(&m.desc))
      return ob->closed == ClosedFlag::Open;
    return false;
  }

  // Transcribe a (recursive) type-declaration group: pre-bind names, register
  // record fields, then transcribe bodies.  Shared by Pstr_type and Psig_type.
  std::vector<tt::TypeDeclaration> type_decls(
      const std::vector<TypeDeclaration>& decls, RecFlag rf = RecFlag::Recursive) {
    bool nonrec = rf == RecFlag::Nonrecursive;
    // `#row` companions are stamped first (in decl order), before the real types.
    std::vector<std::pair<const TypeDeclaration*, tt::Ident>> rows;
    for (auto& d : decls)
      if (needs_row_type(d))
        rows.emplace_back(&d, tt::Ident{d.name.txt + "#row", next_stamp++,
                                        tt::Ident::Local});
    // `type nonrec t = .. t ..`: the body's `t` refers to the OUTER (pre-existing)
    // t, not the one being declared.  Save the outer type-scope bindings so we
    // can restore them for body transcription, while the declaration's own ident
    // is the fresh one (passed explicitly to type_declaration).
    std::vector<std::optional<tt::Ident>> outer;   // per decl (nonrec only)
    std::vector<tt::Ident> new_ids;                // the fresh declaration idents
    if (nonrec)
      for (auto& d : decls) {
        auto it = type_scope.find(d.name.txt);
        outer.push_back(it != type_scope.end() ? std::optional(it->second)
                                               : std::nullopt);
      }
    for (auto& d : decls) new_ids.push_back(fresh_type(d.name.txt));
    for (auto& d : decls) type_arity_[d.name.txt] = (int)d.params.size();
    // Register float abbreviations first (`type t = [private] float`, or an alias
    // of an already-known float type), so a same-group float record sees them.
    auto is_float_ty = [&](const CoreType& t) {
      auto* c = std::get_if<Ptyp_constr>(&t.desc);
      return c && (lid_last(c->id.txt) == "float" || float_abbrevs_.count(lid_last(c->id.txt)));
    };
    for (auto& d : decls)
      if (d.manifest && is_float_ty(*d.manifest->get())) float_abbrevs_.insert(d.name.txt);
    for (auto& d : decls) {
      if (auto* r = std::get_if<Ptype_record>(&d.kind)) {
        RecordInfo info;
        bool all_float = !r->fields.empty();
        for (auto& f : r->fields) {
          info.decl_fields.push_back(f.name.txt);
          if (!is_float_ty(*f.type)) all_float = false;
        }
        bool unboxed = false;
        for (auto& a : d.attrs)
          if (a.name == "unboxed" || a.name == "ocaml.unboxed") unboxed = true;
        if (all_float) info.repr = "Record_float";
        else if (unboxed) info.repr = "Record_unboxed false";
        for (auto& f : r->fields) field_registry[f.name.txt] = info;
      }
    }
    std::vector<tt::TypeDeclaration> out;
    // All `#row` companions first (ghost, abstract, private, params mirror the
    // real type, no constraints/manifest), then the real declarations.
    for (auto& [dp, rid] : rows) {
      tt::TypeDeclaration rd;
      rd.id = rid;
      rd.loc = dp->loc;
      rd.loc.ghost = true;
      for (auto& p : dp->params)
        rd.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
      rd.kind.v = tt::Ttype_abstract{};
      rd.private_ = true;
      out.push_back(std::move(rd));
    }
    if (nonrec)  // bodies resolve names to the outer types, not this group
      for (size_t i = 0; i < decls.size(); ++i) {
        if (outer[i]) type_scope[decls[i].name.txt] = *outer[i];
        else type_scope.erase(decls[i].name.txt);
      }
    for (size_t i = 0; i < decls.size(); ++i)
      out.push_back(type_declaration(
          decls[i], nonrec ? std::optional(new_ids[i]) : std::nullopt));
    if (nonrec)  // after the group, the new types ARE in scope for later items
      for (size_t i = 0; i < decls.size(); ++i)
        type_scope[decls[i].name.txt] = new_ids[i];
    return out;
  }

  // Resolve an unqualified value name: locals (innermost first), else Stdlib.
  tt::Path resolve_value(const Longident& lid, size_t err_pos) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->find(l->name);
        if (f != it->end()) {
          tt::Path p;
          p.v = tt::Pident{f->second};
          return p;
        }
      }
      for (auto it = opens.rbegin(); it != opens.rend(); ++it) {
        if (it->values.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(it->path), l->name};
          return p;
        }
      }
      if (stdlib_values().count(l->name)) return stdlib_path(l->name);
      throw TypeError("Unbound value " + l->name);
    }
    // Qualified M.x: resolve through Stdlib's implicit open for the head, then
    // append the field path.  (Only the common M.x shape for now.)
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported longident");
  }

  // Resolve a module path head: local submodule, else Stdlib (implicit open).
  tt::Path resolve_module(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto m = module_scope.find(l->name);
      if (m != module_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{m->second};
        return p;
      }
      // A submodule brought into scope by `open M` resolves through M's path.
      for (auto it = opens.rbegin(); it != opens.rend(); ++it)
        if (it->submodules.count(l->name)) {
          tt::Path p;
          p.v = tt::Pdot{std::make_shared<tt::Path>(it->path), l->name};
          return p;
        }
      // `Stdlib` itself is the global root, not a submodule of Stdlib.
      if (l->name == "Stdlib") {
        tt::Path p;
        p.v = tt::Pident{tt::Ident{"Stdlib", 0, tt::Ident::Global}};
        return p;
      }
      // Other bare names are auto-opened Stdlib submodules (`List` -> Stdlib.List).
      auto pre = std::make_shared<tt::Path>();
      pre->v = tt::Pident{tt::Ident{"Stdlib", 0, tt::Ident::Global}};
      tt::Path p;
      p.v = tt::Pdot{pre, l->name};
      return p;
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    if (auto* ap = std::get_if<Lapply>(&lid.v)) {  // F(Arg) in a path
      tt::Path p;
      p.v = tt::Papply{std::make_shared<tt::Path>(resolve_module(*ap->f)),
                       std::make_shared<tt::Path>(resolve_module(*ap->x))};
      return p;
    }
    throw TypeError("unsupported module path");
  }

  // Resolve a module-type path: local module type, else qualified.
  tt::Path resolve_modtype(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) {
      auto m = modtype_scope.find(l->name);
      if (m != modtype_scope.end()) {
        tt::Path p;
        p.v = tt::Pident{m->second};
        return p;
      }
      throw TypeError("Unbound module type " + l->name);
    }
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      tt::Path prefix = resolve_module(*d->prefix);
      tt::Path p;
      p.v = tt::Pdot{std::make_shared<tt::Path>(std::move(prefix)), d->name};
      return p;
    }
    throw TypeError("unsupported module-type path");
  }

  static std::string lid_str(const Longident& x) {
    if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&x.v)) return lid_str(*p->prefix) + '.' + p->name;
    auto& a = std::get<Lapply>(x.v);
    return lid_str(*a.f) + '(' + lid_str(*a.x) + ')';
  }

  // A constructor of arity>1 applied to an n-tuple flattens its arguments in the
  // typedtree.  `::` is always arity-2; other multi-arg constructors are looked
  // up in ctor_arity_ (local declarations) by simple name, requiring the tuple
  // size to match the arity.  Unknown (external) constructors keep one argument.
  bool flattens(const std::string& name, size_t tuple_n, const void* node) {
    if (name == "::") return true;
    // The inference side-table flattens by resolved arity (covers cmi ctors).
    if (flatten_construct && flatten_construct->count(node)) return true;
    size_t dot = name.rfind('.');
    auto it = ctor_arity_.find(dot == std::string::npos ? name
                                                        : name.substr(dot + 1));
    return it != ctor_arity_.end() && it->second > 1 &&
           (size_t)it->second == tuple_n;
  }

  // Top-level entry: the FIRST pattern() (or to_computation()) on a case/binding
  // lhs installs a name->ident map so or-pattern branches share variable stamps;
  // recursive calls inherit it.  See pat_vars_.
  tt::Pattern pattern(const Pattern& p) {
    if (pat_vars_) return pattern_impl(p);
    std::unordered_map<std::string, tt::Ident> vars;
    pat_vars_ = &vars;
    tt::Pattern r = pattern_impl(p);
    pat_vars_ = nullptr;
    return r;
  }

  tt::Pattern pattern_impl(const Pattern& p) {
    tt::Pattern out;
    out.loc = p.loc;
    out.attrs = &p.attrs;
    if (std::holds_alternative<Ppat_any>(p.desc)) {
      out.desc = tt::Tpat_any{};
    } else if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
      out.desc = tt::Tpat_var{fresh_local(v->name.txt)};
    } else if (auto* c = std::get_if<Ppat_constant>(&p.desc)) {
      out.desc = tt::Tpat_constant{c->c};
    } else if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      tt::Tpat_construct tc;
      tc.name = lid_str(k->id.txt);
      if (k->arg) {
        auto* tup = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
        bool plain = tup && tup->closed == ClosedFlag::Closed &&
                     std::none_of(tup->labels.begin(), tup->labels.end(),
                                  [](auto& l) { return l.has_value(); });
        size_t dot = tc.name.rfind('.');
        auto ar = ctor_arity_.find(dot == std::string::npos ? tc.name
                                                            : tc.name.substr(dot + 1));
        int arity = ar != ctor_arity_.end() ? ar->second : 0;
        if (plain && flattens(tc.name, tup->elems.size(), &p)) {
          for (auto& el : tup->elems)
            tc.args.push_back(std::make_unique<tt::Pattern>(pattern(*el)));
        } else if (arity > 1 &&
                   std::holds_alternative<Ppat_any>((*k->arg)->desc)) {
          // `C _` for an arity-N ctor: the lone `_` fills every slot (N Tpat_any).
          for (int i = 0; i < arity; ++i)
            tc.args.push_back(std::make_unique<tt::Pattern>(pattern(**k->arg)));
        } else {
          tc.args.push_back(std::make_unique<tt::Pattern>(pattern(**k->arg)));
        }
      }
      out.desc = std::move(tc);
    } else if (auto* tu = std::get_if<Ppat_tuple>(&p.desc)) {
      tt::Tpat_tuple tup;
      for (auto& el : tu->elems)
        tup.elems.emplace_back(std::nullopt,
                               std::make_unique<tt::Pattern>(pattern(*el)));
      out.desc = std::move(tup);
    } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      out.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(pattern(*o->l)),
                             std::make_unique<tt::Pattern>(pattern(*o->r))};
    } else if (auto* al = std::get_if<Ppat_alias>(&p.desc)) {
      auto inner = std::make_unique<tt::Pattern>(pattern(*al->p));
      tt::Tpat_alias ta;
      ta.id = fresh_local(al->name.txt);
      ta.inner = std::move(inner);
      out.desc = std::move(ta);
    } else if (auto* ct = std::get_if<Ppat_constraint>(&p.desc)) {
      out = pattern(*ct->p);  // become inner pattern; record constraint as extra
      tt::PatExtra ex;
      ex.ctype = core_type(*ct->t);
      ex.loc = p.loc;
      // OCaml conses pat_extra, so the OUTER constraint prints before any extra
      // the inner pattern already carries (e.g. `((module X) : pkg)` prints the
      // constraint before the unpack).
      out.extras.insert(out.extras.begin(), std::move(ex));
    } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      tt::Tpat_record tr;  // only the written fields, in order (label = last comp)
      for (auto& [lid, sub] : r->fields)
        tr.fields.emplace_back(lid_last(lid.txt),
                               std::make_unique<tt::Pattern>(pattern(*sub)));
      out.desc = std::move(tr);
    } else if (auto* a = std::get_if<Ppat_array>(&p.desc)) {
      tt::Tpat_array ta;
      for (auto& el : a->elems) ta.elems.push_back(std::make_unique<tt::Pattern>(pattern(*el)));
      out.desc = std::move(ta);
    } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
      out.desc = tt::Tpat_lazy{std::make_unique<tt::Pattern>(pattern(*lz->p))};
    } else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
      tt::Tpat_variant tv;
      tv.label = v->label;
      if (v->arg) tv.arg = std::make_unique<tt::Pattern>(pattern(**v->arg));
      out.desc = std::move(tv);
    } else if (auto* up = std::get_if<Ppat_unpack>(&p.desc)) {
      tt::PatExtra ex;
      ex.kind = tt::PatExtra::Kind::Unpack;
      if (up->pkg) ex.pkg = package_type(*up->pkg);
      // OCaml's asymmetry: a named `(module M : S)` extra spans the whole pattern;
      // anonymous `(module _ : S)` (Tpat_any) spans just the `_`.
      ex.loc = up->name.txt ? p.loc : up->name.loc;
      out.extras.push_back(std::move(ex));
      if (up->name.txt) {
        tt::Tpat_var tv;
        tv.id = fresh_module(*up->name.txt);  // binds a module for the body
        out.desc = std::move(tv);
      } else {
        out.desc = tt::Tpat_any{};
      }
    } else if (auto* iv = std::get_if<Ppat_interval>(&p.desc)) {  // 'a'..'z'
      // An interval expands to a right-nested or-pattern of every value in range.
      auto* a = std::get_if<Pconst_char>(&iv->c1.desc);
      auto* b = std::get_if<Pconst_char>(&iv->c2.desc);
      if (!a || !b) throw TypeError("pat#interval non-char");
      Location gloc = p.loc;
      gloc.ghost = true;  // every expansion node is ghost; only the top isn't
      auto mk_char = [&](int ch) {
        tt::Pattern cp;
        cp.loc = gloc;
        ast::Constant k;
        k.desc = ast::Pconst_char{ch};
        cp.desc = tt::Tpat_constant{k};
        return cp;
      };
      int lo = a->code, hi = b->code;
      tt::Pattern acc = mk_char(hi);
      for (int ch = hi - 1; ch >= lo; --ch) {
        tt::Pattern orp;
        orp.loc = gloc;
        orp.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(mk_char(ch)),
                               std::make_unique<tt::Pattern>(std::move(acc))};
        acc = std::move(orp);
      }
      out.desc = std::move(acc.desc);  // out keeps the non-ghost top loc
    } else if (auto* op = std::get_if<Ppat_open>(&p.desc)) {  // M.(P)
      auto saved = opens;
      opens.push_back(make_open_entry(op->mod_.txt));
      out = pattern(*op->p);  // the open only affects resolution of P
      opens = std::move(saved);
      tt::PatExtra pe;  // ..but records a Tpat_extra_open with the module path
      pe.kind = tt::PatExtra::Kind::Open;
      pe.type_path = resolve_module(op->mod_.txt);
      pe.loc = p.loc;
      out.extras.insert(out.extras.begin(), std::move(pe));
    } else if (auto* ty = std::get_if<Ppat_type>(&p.desc)) {  // #t
      // `#t` expands to the or-pattern of t's polyvariant tags (in decl order,
      // seeded from the first: A | B | C  ->  Or(C, Or(B, A))), with a
      // Tpat_extra_type recording the type path.  All nodes are ghost at #t's loc.
      tt::Path tp = resolve_type(ty->id.txt);
      const std::vector<PolyTag>* tags = nullptr;
      if (auto* pid = std::get_if<tt::Pident>(&tp.v)) {
        auto f = polyvar_tags_.find(pid->id.stamp);
        if (f != polyvar_tags_.end()) tags = &f->second;
      }
      if (!tags || tags->empty()) throw TypeError("pat#type unknown tags");
      // Polyvariant rows are ordered by tag hash; the or-pattern is seeded from
      // the first and wraps each later tag, so it prints in reverse-hash order.
      std::vector<PolyTag> ordered(*tags);
      std::stable_sort(ordered.begin(), ordered.end(),
                       [](const PolyTag& a, const PolyTag& b) {
                         return hash_variant(a.name) < hash_variant(b.name);
                       });
      tags = &ordered;
      Location gloc = p.loc;
      gloc.ghost = true;
      auto mk_variant = [&](const PolyTag& t) {
        tt::Pattern v;
        v.loc = gloc;
        tt::Tpat_variant tv;
        tv.label = t.name;
        if (t.has_arg) {
          auto arg = std::make_unique<tt::Pattern>();
          arg->loc = none_loc();
          arg->loc.ghost = true;
          arg->desc = tt::Tpat_any{};
          tv.arg = std::move(arg);
        }
        v.desc = std::move(tv);
        return v;
      };
      tt::Pattern acc = mk_variant((*tags)[0]);
      for (size_t k = 1; k < tags->size(); ++k) {
        tt::Pattern orp;
        orp.loc = gloc;
        orp.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(mk_variant((*tags)[k])),
                               std::make_unique<tt::Pattern>(std::move(acc))};
        acc = std::move(orp);
      }
      out.desc = std::move(acc.desc);  // top node is non-ghost, carries the extra
      tt::PatExtra ex;
      ex.kind = tt::PatExtra::Kind::Type;
      ex.type_path = std::move(tp);
      ex.loc = p.loc;
      out.extras.push_back(std::move(ex));
    } else {
      throw TypeError("pat#" + std::to_string(p.desc.index()));
    }
    return out;
  }

  // Build a computation pattern (match case lhs): or distributes, `exception P`
  // becomes Tpat_exception, and any other (value) pattern is wrapped Tpat_value.
  tt::Pattern to_computation(const Pattern& p) {
    if (pat_vars_) return to_computation_impl(p);
    std::unordered_map<std::string, tt::Ident> vars;
    pat_vars_ = &vars;
    tt::Pattern r = to_computation_impl(p);
    pat_vars_ = nullptr;
    return r;
  }

  tt::Pattern to_computation_impl(const Pattern& p) {
    tt::Pattern out;
    out.loc = p.loc;
    // `exception P [@attr]` / or-pattern attrs live on the computation node;
    // the value branch delegates to pattern(), which carries them itself.
    if (!p.attrs.empty() && (std::holds_alternative<Ppat_or>(p.desc) ||
                             std::holds_alternative<Ppat_exception>(p.desc)))
      out.attrs = &p.attrs;
    if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      out.desc = tt::Tpat_or{std::make_unique<tt::Pattern>(to_computation(*o->l)),
                             std::make_unique<tt::Pattern>(to_computation(*o->r))};
    } else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) {
      out.desc = tt::Tpat_exception{std::make_unique<tt::Pattern>(pattern(*ex->p))};
    } else {
      tt::Pattern inner = pattern(p);
      out.loc = inner.loc;
      // A top-level type CONSTRAINT on a match-case pattern (`(_ : t)`) sits on
      // the COMPUTATION pattern in OCaml; the other extras (#type, unpack, open)
      // stay on the wrapped value pattern.
      std::vector<tt::PatExtra> keep;
      for (auto& e : inner.extras) {
        if (e.kind == tt::PatExtra::Kind::Constraint)
          out.extras.push_back(std::move(e));
        else
          keep.push_back(std::move(e));
      }
      inner.extras = std::move(keep);
      out.desc = tt::Tpat_value{std::make_unique<tt::Pattern>(std::move(inner))};
    }
    return out;
  }

  tt::Expression expr(const Expression& e) {
    tt::Expression out;
    out.loc = e.loc;
    out.attrs = &e.attrs;
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      // A string literal inferred at a format type desugars to the
      // CamlinternalFormatBasics.Format(...) tree (type-directed, via the
      // inference side-table -- catches unqualified/open'd/let-bound formats
      // the Pexp_apply syntactic path misses).
      if (format_lits && format_lits->count(&e))
        if (auto* ps = std::get_if<ast::Pconst_string>(&c->c.desc))
          if (tt::ExprBox fmt = fmtlib::make(ps->s, e.loc)) return std::move(*fmt);
      out.desc = tt::Texp_constant{c->c};
    } else if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      const std::string* ivn = nullptr;
      if (auto* l = std::get_if<Lident>(&id->id.txt.v))
        if (instvars_.count(l->name) && !is_local(l->name)) ivn = &l->name;
      if (ivn) out.desc = tt::Texp_instvar{instvars_.at(*ivn)};
      else out.desc = tt::Texp_ident{resolve_value(id->id.txt, e.loc.start.cnum)};
    } else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      tt::Texp_tuple tup;
      for (size_t k = 0; k < t->elems.size(); ++k) {
        std::optional<std::string> label;
        if (k < t->labels.size()) label = t->labels[k];
        tup.elems.emplace_back(
            label, std::make_unique<tt::Expression>(expr(*t->elems[k])));
      }
      out.desc = std::move(tup);
    } else if (auto* a = std::get_if<Pexp_apply>(&e.desc)) {
      // `a |> b` / `b @@ a` rewrite to the application `b a` (matching the typer's
      // %revapply/%apply handling), but only for a 2-arg unlabelled application.
      int rev = 0;
      if (a->args.size() == 2 &&
          std::holds_alternative<Nolabel>(a->args[0].first) &&
          std::holds_alternative<Nolabel>(a->args[1].first))
        rev = revapply_kind(*a->fn);
      if (rev) {
        auto& fexpr = rev == 1 ? a->args[1].second : a->args[0].second;
        auto& aexpr = rev == 1 ? a->args[0].second : a->args[1].second;
        tt::Texp_apply ap;
        ap.fn = std::make_unique<tt::Expression>(expr(*fexpr));
        ap.args.emplace_back(ArgLabel{Nolabel{}},
                             std::make_unique<tt::Expression>(expr(*aexpr)));
        out.desc = std::move(ap);
        return out;
      }
      tt::Texp_apply ap;
      ap.fn = std::make_unique<tt::Expression>(expr(*a->fn));
      // Detect a format-string argument to a qualified printf/scanf-family call
      // and desugar it to CamlinternalFormatBasics.Format(...).
      int fmtidx = -1;
      if (auto* fid = std::get_if<Pexp_ident>(&a->fn->desc))
        fmtidx = format_arg_index(fid->id.txt);
      auto written_arg = [&](size_t k) -> tt::ExprBox {  // the k-th source arg
        auto& arg = a->args[k].second;
        if (static_cast<int>(k) == fmtidx)
          if (auto* cst = std::get_if<Pexp_constant>(&arg->desc))
            if (auto* ps = std::get_if<ast::Pconst_string>(&cst->c.desc))
              if (tt::ExprBox fmt = fmtlib::make(ps->s, arg->loc))  // null if unparseable
                return fmt;
        return std::make_unique<tt::Expression>(expr(*arg));
      };
      // Slice 3: when inference reconstructed the call's arguments (omitted
      // optionals filled with None, labelled args reordered to parameter order,
      // `~l:e` on an optional Some-wrapped), emit that; else source order.
      const std::vector<applymatch::Slot>* plan = nullptr;
      if (apply_plans) { auto it = apply_plans->find(&e); if (it != apply_plans->end()) plan = &it->second; }
      if (plan) {
        std::vector<bool> used(a->args.size(), false);
        for (auto& s : *plan) {
          ArgLabel label = Nolabel{};
          if (s.param_label == 1) label = Labelled{s.param_name};
          else if (s.param_label == 2) label = Optional{s.param_name};
          tt::ExprBox av;
          if (s.omitted) {
            if (s.none_fill) {  // omitted optional defaulted -> ghost None
              av = std::make_unique<tt::Expression>();
              av->loc = none_loc();
              av->desc = tt::Texp_construct{"None", {}};
            }
            // else an eta/Omitted argument: the label with no expression (av null)
          } else {
            av = written_arg(s.arg_index);
            used[s.arg_index] = true;
            if (s.some_wrap) {  // ~l:e on an optional param -> Some e
              auto some = std::make_unique<tt::Expression>();
              some->loc = av->loc;
              std::vector<tt::ExprBox> sa;
              sa.push_back(std::move(av));
              some->desc = tt::Texp_construct{"Some", std::move(sa)};
              av = std::move(some);
            }
          }
          ap.args.emplace_back(std::move(label), std::move(av));
        }
        for (size_t k = 0; k < a->args.size(); ++k)  // over-application leftover
          if (!used[k]) {
            tt::ExprBox av = written_arg(k);
            ap.args.emplace_back(a->args[k].first, std::move(av));
          }
      } else {
        for (size_t k = 0; k < a->args.size(); ++k) {
          tt::ExprBox av = written_arg(k);  // evaluate the arg before emplace_back
          ap.args.emplace_back(a->args[k].first, std::move(av));
        }
      }
      out.desc = std::move(ap);
    } else if (auto* f = std::get_if<Pexp_function>(&e.desc)) {
      out.desc = function(*f, out.extras);
    } else if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      // `let pat = e in body` with a constructor/open-tuple/labelled-tuple
      // pattern is typed as `match e with pat -> body` (single non-rec binding,
      // no attributes/constraint).
      if (le->rf == RecFlag::Nonrecursive && le->bindings.size() == 1 &&
          le->bindings[0].attrs.empty() && !le->bindings[0].constraint_ &&
          turn_let_into_match(le->bindings[0].pat)) {
        auto& vb = le->bindings[0];
        tt::Texp_match tm;
        tm.scrut = std::make_unique<tt::Expression>(expr(*vb.expr));
        push();
        tt::Case cs;
        cs.lhs = to_computation(vb.pat);
        cs.rhs = std::make_unique<tt::Expression>(expr(*le->body));
        tm.cases.push_back(std::move(cs));
        pop();
        tm.partial = !pat_irrefutable(vb.pat);
        out.desc = std::move(tm);
        return out;
      }
      tt::Texp_let tl;
      tl.rf = le->rf;
      push();
      tl.bindings = value_bindings(le->rf, le->bindings);
      tl.body = std::make_unique<tt::Expression>(expr(*le->body));
      pop();
      out.desc = std::move(tl);
    } else if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      tt::Texp_ifthenelse ti;
      ti.cond = std::make_unique<tt::Expression>(expr(*it->cond));
      ti.then_ = std::make_unique<tt::Expression>(expr(*it->then_));
      if (it->else_)
        ti.else_ = std::make_unique<tt::Expression>(expr(**it->else_));
      out.desc = std::move(ti);
    } else if (auto* s = std::get_if<Pexp_sequence>(&e.desc)) {
      tt::Texp_sequence ts;
      ts.e1 = std::make_unique<tt::Expression>(expr(*s->e1));
      ts.e2 = std::make_unique<tt::Expression>(expr(*s->e2));
      out.desc = std::move(ts);
    } else if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      tt::Texp_match tm;
      tm.scrut = std::make_unique<tt::Expression>(expr(*m->e));
      for (auto& c : m->cases) {
        if (auto* pe = std::get_if<Ppat_effect>(&c.lhs.desc)) {
          // `| effect P, k ->`: the case's printed lhs is the effect pattern;
          // the continuation variable binds for the rhs but is not dumped.
          push();
          tt::Case cs;
          cs.lhs = pattern(*pe->eff);
          if (auto* kv = std::get_if<Ppat_var>(&pe->cont->desc))
            fresh_local(kv->name.txt);
          if (c.guard) cs.guard = std::make_unique<tt::Expression>(expr(**c.guard));
          cs.rhs = std::make_unique<tt::Expression>(expr(*c.rhs));
          pop();
          tm.eff_cases.push_back(std::move(cs));
          continue;
        }
        tm.cases.push_back(case_(c, /*computation=*/true));
      }
      if (partiality) {
        auto it = partiality->find(&e);
        tm.partial = it != partiality->end() && it->second;
      }
      out.desc = std::move(tm);
    } else if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      tt::Texp_try tt2;
      tt2.body = std::make_unique<tt::Expression>(expr(*tr->e));
      for (auto& c : tr->cases) tt2.cases.push_back(case_(c, /*computation=*/false));
      out.desc = std::move(tt2);
    } else if (std::holds_alternative<Pexp_unreachable>(e.desc)) {
      out.desc = tt::Texp_unreachable{};
    } else if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) {
      tt::Texp_letop tl;
      auto bop = [&](const BindingOp& b) {
        tt::BindingOp bo;
        Longident lid;
        lid.v = Lident{b.op.txt};
        bo.path = resolve_value(lid, b.op.loc.start.cnum);
        bo.loc = b.loc;
        bo.exp = std::make_unique<tt::Expression>(expr(*b.exp));
        return bo;
      };
      tl.let_ = bop(lo->let_);
      for (auto& a : lo->ands) tl.ands.push_back(bop(a));
      bool irr = pat_irrefutable(lo->let_.pat);
      for (auto& a : lo->ands) irr = irr && pat_irrefutable(a.pat);
      tl.partial = !irr;
      push();
      tt::Case cs;
      // Joined pattern: each `and+` pairs with the accumulated pattern in a
      // LEFT-nested ghost 2-tuple at the leading operator's location.
      tt::Pattern acc = pattern(lo->let_.pat);
      for (auto& a : lo->ands) {
        tt::Tpat_tuple tup;
        tup.elems.emplace_back(std::nullopt,
                               std::make_unique<tt::Pattern>(std::move(acc)));
        tup.elems.emplace_back(std::nullopt,
                               std::make_unique<tt::Pattern>(pattern(a.pat)));
        acc = tt::Pattern{};
        acc.loc = lo->let_.op.loc;
        acc.loc.ghost = true;
        acc.desc = std::move(tup);
      }
      cs.lhs = std::move(acc);
      cs.rhs = std::make_unique<tt::Expression>(expr(*lo->body));
      pop();
      tl.body = std::make_unique<tt::Case>(std::move(cs));
      out.desc = std::move(tl);
    } else if (auto* pk = std::get_if<Pexp_pack>(&e.desc)) {
      auto me = std::make_unique<tt::ModuleExpr>(module_expr(*pk->me));
      if (!pk->pkg) {
        // An unannotated `(module ME)` is still coerced to the expected package
        // signature: one implicit Tmod_constraint layer (each an extra
        // module_expr line), plus one more for a global path (also strengthened).
        int layers = 1;
        if (auto* mi = std::get_if<tt::Tmod_ident>(&me->desc))
          if (path_global_or_alias(mi->path)) layers = 2;  // strengthened
        for (int k = 0; k < layers; ++k) {
          auto wrap = std::make_unique<tt::ModuleExpr>();
          wrap->loc = me->loc;
          wrap->desc = tt::Tmod_constraint{std::move(me), nullptr, true};
          me = std::move(wrap);
        }
      }
      if (pk->pkg) {
        // `(module M : S)`: the ascription coerces the ident behind a
        // transparent constraint and prints as a Ttyp_package Texp_constraint.
        // A global path is additionally strengthened (a second implicit layer).
        if (auto* mi = std::get_if<tt::Tmod_ident>(&me->desc)) {
          int layers = path_global_or_alias(mi->path) ? 2 : 1;  // strengthened
          for (int k = 0; k < layers; ++k) {
            auto wrap = std::make_unique<tt::ModuleExpr>();
            wrap->loc = me->loc;
            wrap->desc = tt::Tmod_constraint{std::move(me), nullptr, true};
            me = std::move(wrap);
          }
        }
        tt::ExprExtra ex;
        ex.kind = tt::ExprExtra::Kind::Constraint;
        tt::CoreType ct;
        ct.loc = pk->pkg->path.loc;
        // The package core_type spans `S with type .. = u` -- extend past the
        // path to the end of the last `with type` constraint.
        if (!pk->pkg->constraints.empty())
          ct.loc.end = pk->pkg->constraints.back().second->loc.end;
        ct.desc = tt::Ttyp_package{package_type(*pk->pkg)};
        ex.ctype = std::move(ct);
        ex.loc = e.loc;
        out.extras.push_back(std::move(ex));
      }
      out.desc = tt::Texp_pack{std::move(me)};
    } else if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      tt::Texp_construct tc;
      tc.name = lid_str(k->id.txt);
      if (k->arg) {
        auto* tup = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
        bool plain = tup &&
                     std::none_of(tup->labels.begin(), tup->labels.end(),
                                  [](auto& l) { return l.has_value(); });
        if (plain && flattens(tc.name, tup->elems.size(), &e)) {
          for (auto& el : tup->elems)
            tc.args.push_back(std::make_unique<tt::Expression>(expr(*el)));
        } else {
          tc.args.push_back(std::make_unique<tt::Expression>(expr(**k->arg)));
        }
      }
      out.desc = std::move(tc);
    } else if (auto* ar = std::get_if<Pexp_array>(&e.desc)) {
      tt::Texp_array ta;
      for (auto& el : ar->elems)
        ta.elems.push_back(std::make_unique<tt::Expression>(expr(*el)));
      out.desc = std::move(ta);
    } else if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      out.desc = tt::Texp_assert{std::make_unique<tt::Expression>(expr(*as->e))};
    } else if (auto* fo = std::get_if<Pexp_for>(&e.desc)) {
      tt::Texp_for tf;
      tf.lo = std::make_unique<tt::Expression>(expr(*fo->lo));
      tf.hi = std::make_unique<tt::Expression>(expr(*fo->hi));
      tf.dir = fo->dir == DirectionFlag::Upto ? tt::Direction::Up
                                              : tt::Direction::Down;
      push();
      if (auto* pv = std::get_if<Ppat_var>(&fo->var.desc)) {
        tf.var = fresh_local(pv->name.txt);
      } else if (std::holds_alternative<Ppat_any>(fo->var.desc)) {
        tf.var = fresh_anon("_for");  // `for _ = ..`: typecore's dummy ident
      } else {
        pop();
        throw TypeError("for-var not a variable");
      }
      tf.body = std::make_unique<tt::Expression>(expr(*fo->body));
      pop();
      out.desc = std::move(tf);
    } else if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) {
      out.desc = tt::Texp_lazy{std::make_unique<tt::Expression>(expr(*lz->e))};
    } else if (auto* wh = std::get_if<Pexp_while>(&e.desc)) {
      out.desc = tt::Texp_while{std::make_unique<tt::Expression>(expr(*wh->cond)),
                                std::make_unique<tt::Expression>(expr(*wh->body))};
    } else if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) {
      out = expr(*ct->e);  // become the inner expr; record the constraint as extra
      tt::ExprExtra ex;
      ex.kind = tt::ExprExtra::Kind::Constraint;
      ex.ctype = core_type(*ct->t);
      ex.loc = e.loc;
      out.extras.push_back(std::move(ex));
    } else if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) {
      out = expr(*co->e);  // become the inner expr; record the coercion as extra
      tt::ExprExtra ex;
      ex.kind = tt::ExprExtra::Kind::Coerce;
      ex.ctype = core_type(*co->to_);
      if (co->from) ex.from = core_type(**co->from);
      ex.loc = e.loc;
      out.extras.push_back(std::move(ex));
    } else if (auto* fd = std::get_if<Pexp_field>(&e.desc)) {
      out.desc = tt::Texp_field{std::make_unique<tt::Expression>(expr(*fd->e)),
                                lid_str(fd->field.txt)};
    } else if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      out.desc = tt::Texp_setfield{std::make_unique<tt::Expression>(expr(*sf->obj)),
                                   lid_str(sf->field.txt),
                                   std::make_unique<tt::Expression>(expr(*sf->value))};
    } else if (auto* vr = std::get_if<Pexp_variant>(&e.desc)) {
      tt::Texp_variant tv;
      tv.label = vr->label;
      if (vr->arg) tv.arg = std::make_unique<tt::Expression>(expr(**vr->arg));
      out.desc = std::move(tv);
    } else if (auto* sd = std::get_if<Pexp_send>(&e.desc)) {
      auto objexpr = std::make_unique<tt::Expression>(expr(*sd->obj));
      // A self-send (object is the enclosing self ident, method is one of its
      // methods) resolves the method to its ident (Tmeth_val); else Tmeth_name.
      std::optional<tt::Ident> mid;
      if (auto* oi = std::get_if<tt::Texp_ident>(&objexpr->desc))
        if (auto* pid = std::get_if<tt::Pident>(&oi->path.v)) {
          auto it = self_meths_.find(pid->id.stamp);
          if (it != self_meths_.end()) {
            auto mit = it->second->find(sd->meth.txt);
            if (mit != it->second->end()) mid = mit->second;
          }
        }
      out.desc = tt::Texp_send{std::move(objexpr), sd->meth.txt, mid};
    } else if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) {  // fun (type a) -> e
      auto saved = type_scope;
      fresh_type(nt->name.txt);
      out = expr(*nt->body);
      type_scope = std::move(saved);
      out.loc = e.loc;  // keep the newtype expression's outer location
      tt::ExprExtra ex;
      ex.kind = tt::ExprExtra::Kind::Newtype;
      ex.newtype = nt->name.txt;
      ex.loc = nt->name.loc;
      out.extras.insert(out.extras.begin(), std::move(ex));
    } else if (auto* nw = std::get_if<Pexp_new>(&e.desc)) {
      out.desc = tt::Texp_new{resolve_class(nw->id.txt)};
    } else if (auto* sv = std::get_if<Pexp_setinstvar>(&e.desc)) {  // x <- e
      auto iv = instvars_.find(sv->name.txt);
      if (iv == instvars_.end()) throw TypeError("Unbound instance variable " + sv->name.txt);
      out.desc = tt::Texp_setinstvar{iv->second,
                                     std::make_unique<tt::Expression>(expr(*sv->value))};
    } else if (auto* ov = std::get_if<Pexp_override>(&e.desc)) {  // {< x = e >}
      tt::Texp_override to;
      for (auto& [nm, ev] : ov->fields) {
        auto iv = instvars_.find(nm.txt);
        if (iv == instvars_.end()) throw TypeError("Unbound instance variable " + nm.txt);
        to.fields.emplace_back(iv->second, std::make_unique<tt::Expression>(expr(*ev)));
      }
      out.desc = std::move(to);
    } else if (auto* ob = std::get_if<Pexp_object>(&e.desc)) {
      out.desc = tt::Texp_object{std::make_unique<tt::ClassStructure>(build_class_structure(*ob->cs))};
    } else if (auto* rec = std::get_if<Pexp_record>(&e.desc)) {
      tt::Texp_record tr;
      if (rec->base)
        tr.extended = std::make_unique<tt::Expression>(expr(**rec->base));
      const RecordInfo* info = nullptr;
      if (!rec->fields.empty()) {
        auto it = field_registry.find(lid_last(rec->fields[0].first.txt));
        if (it != field_registry.end()) info = &it->second;
      }
      // Pick the decl-order field list + representation: the local field registry,
      // else (an external record update) the inference side-table.
      const std::vector<std::string>* decl_fields = nullptr;
      std::string repr = "Record_regular";
      if (info) { decl_fields = &info->decl_fields; repr = info->repr; }
      else if (record_fields) {
        auto it = record_fields->find(&e);
        if (it != record_fields->end()) decl_fields = &it->second;
        if (record_reprs) {
          auto ir = record_reprs->find(&e);
          if (ir != record_reprs->end()) repr = ir->second;
        }
      }
      if (decl_fields) {  // emit all fields in declaration order, <kept> for omitted
        std::unordered_map<std::string, std::pair<std::string, const ExprBox*>> prov;
        for (auto& [lid, ev] : rec->fields)
          prov[lid_last(lid.txt)] = {lid_str(lid.txt), &ev};
        tr.representation = repr;
        for (auto& fname : *decl_fields) {
          tt::RecordField rf;
          auto p = prov.find(fname);
          if (p != prov.end()) {
            rf.name = p->second.first;
            rf.value = std::make_unique<tt::Expression>(expr(**p->second.second));
          } else {
            rf.kept = true;
          }
          tr.fields.push_back(std::move(rf));
        }
      } else {  // unknown record type: source order, all overridden
        for (auto& [lid, ev] : rec->fields) {
          tt::RecordField rf;
          rf.name = lid_str(lid.txt);
          rf.value = std::make_unique<tt::Expression>(expr(*ev));
          tr.fields.push_back(std::move(rf));
        }
      }
      out.desc = std::move(tr);
    } else if (auto* sti = std::get_if<Pexp_struct_item>(&e.desc)) {
      // let module/open/exception … in e: the item's bindings scope to the body.
      auto st = type_scope; auto md = module_scope; auto mt = modtype_scope;
      auto fr = field_registry; auto op = opens;
      push();
      auto item = std::make_unique<tt::StructureItem>(structure_item(*sti->item));
      auto body = std::make_unique<tt::Expression>(expr(*sti->body));
      pop();
      type_scope = std::move(st); module_scope = std::move(md);
      modtype_scope = std::move(mt); field_registry = std::move(fr);
      opens = std::move(op);
      out.desc = tt::Texp_struct_item{std::move(item), std::move(body)};
    } else if (auto* xt = std::get_if<Pexp_extension>(&e.desc)) {
      // Extension nodes the compiler types natively (no ppx):
      // [%extension_constructor P] and the fork's [%atomic.loc r.f].
      const Expression* payload = nullptr;
      if (xt->payload.str.size() == 1)
        if (auto* ev = std::get_if<Pstr_eval>(&xt->payload.str[0].desc))
          payload = &*ev->e;
      if (xt->name == "extension_constructor" && payload) {
        if (auto* c = std::get_if<Pexp_construct>(&payload->desc))
          out.desc = tt::Texp_extension_ctor{lid_str(c->id.txt)};
        else
          throw TypeError("extension_constructor payload");
      } else if (xt->name == "atomic.loc" && payload) {
        if (auto* fd = std::get_if<Pexp_field>(&payload->desc))
          out.desc = tt::Texp_atomic_loc{
              std::make_unique<tt::Expression>(expr(*fd->e)),
              lid_str(fd->field.txt)};
        else
          throw TypeError("atomic.loc payload");
      } else {
        throw TypeError("expr extension [%" + xt->name + "]");
      }
    } else {
      throw TypeError("expr#" + std::to_string(e.desc.index()));
    }
    return out;
  }

  // Transcribe a match/try case.  Match cases are computation patterns and so
  // wrap the value pattern in a Tpat_value layer (same loc); try cases are value
  // patterns and don't.
  tt::Case case_(const ast::Case& c, bool computation) {
    push();
    tt::Case out;
    out.lhs = computation ? to_computation(c.lhs) : pattern(c.lhs);
    if (c.guard)
      out.guard = std::make_unique<tt::Expression>(expr(**c.guard));
    out.rhs = std::make_unique<tt::Expression>(expr(*c.rhs));
    pop();
    return out;
  }

  // `extras` is the enclosing expression's extra list: `(type a)` params
  // become Texp_newtype extras there (with `a` bound as a local type for the
  // rest of the function), not Param_* entries.
  tt::Texp_function function(const Pexp_function& f,
                             std::vector<tt::ExprExtra>& extras) {
    tt::Texp_function fn;
    push();
    auto saved_types = type_scope;
    auto saved_modules = module_scope;  // (module M) params end with the body
    for (auto& param : f.params) {
      if (auto* nt = std::get_if<Pparam_newtype>(&param.desc)) {
        fresh_type(nt->name.txt);
        tt::ExprExtra ex;
        ex.kind = tt::ExprExtra::Kind::Newtype;
        ex.newtype = nt->name.txt;
        ex.loc = nt->name.loc;
        extras.push_back(std::move(ex));
        continue;
      }
      auto& pv = std::get<Pparam_val>(param.desc);
      tt::FunctionParam fp;
      fp.label = pv.label;
      // The default expression sees the OUTER scope (not this param's binding).
      if (pv.default_)
        fp.default_ = std::make_unique<tt::Expression>(expr(**pv.default_));
      // The syntactic check is authoritative for Total (it handles GADT
      // refinement etc.); the inference-computed verdict only DOWNGRADES a
      // syntactic Partial to Total when the resolved type proves exhaustive
      // (e.g. a `` `Var s `` param over a closed polyvariant row).
      fp.partial = !pat_irrefutable(pv.pat);
      if (fp.partial && param_partial) {
        auto it = param_partial->find(&pv.pat);
        if (it != param_partial->end() && !it->second) fp.partial = false;
      }
      fp.pat = std::make_unique<tt::Pattern>(pattern(pv.pat));
      fn.params.push_back(std::move(fp));
    }
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      fn.is_cases = false;
      auto body = std::make_unique<tt::Expression>(expr(*fb->e));
      // A return-type annotation (`let f .. : t = e` / `fun .. : t -> e`) wraps
      // the body in a Texp_constraint / Texp_coerce extra.
      if (f.constraint_) {
        tt::ExprExtra ex;
        ex.loc = fb->e->loc;
        if (auto* pc = std::get_if<Pconstraint>(&*f.constraint_)) {
          ex.kind = tt::ExprExtra::Kind::Constraint;
          ex.ctype = core_type(*pc->type);
        } else {
          auto& cc = std::get<Pcoerce>(*f.constraint_);
          ex.kind = tt::ExprExtra::Kind::Coerce;
          ex.ctype = core_type(*cc.to_);
          if (cc.from) ex.from = core_type(**cc.from);
        }
        body->extras.push_back(std::move(ex));
      }
      fn.body = std::move(body);
    } else {
      auto& fc = std::get<Pfunction_cases>(f.body->v);
      fn.is_cases = true;
      fn.cases_loc = fc.loc;
      fn.cases_attrs = &fc.attrs;
      if (function_cases_partial) {
        auto it = function_cases_partial->find(&fc);
        fn.cases_partial = it != function_cases_partial->end() && it->second;
      }
      // A `function .. : t` return-type annotation wraps the cases node in a
      // Texp_constraint / Texp_coerce extra (at the cases location).
      if (f.constraint_) {
        tt::ExprExtra ex;
        ex.loc = fc.loc;
        if (auto* pc = std::get_if<Pconstraint>(&*f.constraint_)) {
          ex.kind = tt::ExprExtra::Kind::Constraint;
          ex.ctype = core_type(*pc->type);
        } else {
          auto& cc = std::get<Pcoerce>(*f.constraint_);
          ex.kind = tt::ExprExtra::Kind::Coerce;
          ex.ctype = core_type(*cc.to_);
          if (cc.from) ex.from = core_type(**cc.from);
        }
        fn.cases_extras.push_back(std::move(ex));
      }
      for (auto& c : fc.cases)
        fn.cases.push_back(case_(c, /*computation=*/false));  // value patterns
    }
    pop();
    type_scope = std::move(saved_types);  // (type a) bindings end here
    module_scope = std::move(saved_modules);
    return fn;
  }

  // `let p : t = e` (a Pvc_constraint binding) wraps the pattern in a
  // Tpat_extra_constraint and the RHS in a Texp_constraint, both at the type t.
  // (The `: type a. t` polymorphic form involves newtypes -- left unhandled.)
  void apply_value_constraint(tt::ValueBinding& out, const ValueBinding& vb) {
    if (!vb.constraint_) return;
    auto* vc = std::get_if<Pvc_constraint>(&*vb.constraint_);
    if (!vc || !vc->univars.empty()) return;
    // core_type may throw on a construct we don't transcribe; if so leave the
    // binding unconstrained (a DIFF) rather than failing the whole file.
    tt::CoreType ct1, ct2;
    try {
      ct1 = core_type(*vc->typ);
      ct2 = core_type(*vc->typ);
    } catch (const TypeError&) { return; }
    tt::PatExtra pe;
    pe.ctype = std::move(ct1);
    pe.loc = vb.pat.loc;  // the WHOLE binding pattern span (incl. an inner `(p:t)`)
    pe.loc.ghost = true;
    // OCaml conses pat_extra, so the outer binding constraint (added after the
    // pattern's own inner constraints) prints FIRST: `let (_ : t1) : t2 = e`
    // dumps the ghost outer t2 extra before the inner t1 extra.
    out.pat.extras.insert(out.pat.extras.begin(), std::move(pe));
    // A polymorphic annotation (`: 'a. t`) constrains only the pattern -- the RHS
    // stays un-annotated (it can't carry a polymorphic Texp_constraint).
    if (std::holds_alternative<Ptyp_poly>(vc->typ->desc)) return;
    tt::ExprExtra ee;
    ee.kind = tt::ExprExtra::Kind::Constraint;
    ee.ctype = std::move(ct2);
    ee.loc = out.expr.loc;
    ee.loc.ghost = true;
    out.expr.extras.push_back(std::move(ee));
  }

  // `let f : type a b. t = e` (wrap_type_annotation): the newtypes scope the RHS,
  // which gets Texp_newtype + Texp_constraint(t) extras; the pattern gets a
  // Tpat_extra_constraint whose type is Ttyp_poly(['a;'b], varify(t)).  Returns
  // false (leaving `out` unconstrained) if the type can't be transcribed.
  bool apply_newtype_constraint(tt::ValueBinding& out, const ValueBinding& vb,
                                const Pvc_constraint& vc,
                                const std::unordered_map<int, std::string>& nt,
                                const std::vector<std::string>& names) {
    tt::CoreType cexpr, cpat;  // built while the newtypes are still in scope
    try {
      cexpr = core_type(*vc.typ);   // newtype refs stay Ttyp_constr "a"
      cpat = core_type(*vc.typ);
    } catch (const TypeError&) { return false; }
    varify_tt(cpat, nt);            // pattern side: newtype refs -> Ttyp_var
    Location tyloc = vc.typ->loc;
    // --- expression: Texp_newtype* then Texp_constraint, on the binding span ---
    Location espan{vb.pat.loc.start, vb.expr->loc.end, true};
    out.expr.loc = espan;
    tt::ExprExtra ce;
    ce.kind = tt::ExprExtra::Kind::Constraint;
    ce.ctype = std::move(cexpr);
    ce.loc = espan;
    out.expr.extras.insert(out.expr.extras.begin(), std::move(ce));
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
      tt::ExprExtra ne;
      ne.kind = tt::ExprExtra::Kind::Newtype;
      ne.newtype = *it;
      ne.loc = espan;
      out.expr.extras.insert(out.expr.extras.begin(), std::move(ne));
    }
    // --- pattern: Tpat_extra_constraint of Ttyp_poly(vars, varified type) ---
    tt::CoreType poly;
    poly.loc = tyloc;
    poly.desc = tt::Ttyp_poly{names, std::make_unique<tt::CoreType>(std::move(cpat))};
    tt::PatExtra pe;
    pe.ctype = std::move(poly);
    pe.loc = Location{vb.pat.loc.start, tyloc.end, true};
    out.pat.extras.push_back(std::move(pe));
    return true;
  }

  tt::ValueBinding value_binding(const ValueBinding& vb) {
    tt::ValueBinding out;
    const Pvc_constraint* nt_vc = nullptr;
    if (vb.constraint_)
      if (auto* vc = std::get_if<Pvc_constraint>(&*vb.constraint_))
        if (!vc->univars.empty()) nt_vc = vc;
    if (nt_vc) {  // `: type a. t` scopes the newtypes over the RHS and constraints
      auto saved = type_scope;
      std::unordered_map<int, std::string> nt;  // newtype-ident stamp -> var name
      std::vector<std::string> names;
      for (auto& u : nt_vc->univars) {
        tt::Ident id = fresh_type(u.txt);
        nt[id.stamp] = u.txt;
        names.push_back(u.txt);
      }
      out.expr = expr(*vb.expr);   // body sees the newtypes
      out.pat = pattern(vb.pat);
      out.attrs = &vb.attrs;
      apply_newtype_constraint(out, vb, *nt_vc, nt, names);
      type_scope = std::move(saved);
      return out;
    }
    out.expr = expr(*vb.expr);  // RHS typed before the pattern is bound (non-rec)
    out.pat = pattern(vb.pat);
    out.attrs = &vb.attrs;
    apply_value_constraint(out, vb);
    return out;
  }

  // Value_rec_check.classify_expression: a `let rec` binding's RHS has a
  // statically-known size (Static -> <def_rec>) or not (Dynamic ->
  // <def_rec_dynamic>).  `env` maps a locally-let-bound ident stamp to its own
  // classification (nested lets refine what a bare variable resolves to); a
  // variable not in env (bound outside this size analysis) is Dynamic.
  enum class Sd { Static, Dynamic };
  Sd classify_rec_expr(const tt::Expression& e,
                       const std::unordered_map<long long, Sd>& env) {
    using namespace tt;
    if (std::holds_alternative<Texp_function>(e.desc)) return Sd::Static;
    if (auto* id = std::get_if<Texp_ident>(&e.desc)) {
      if (auto* pi = std::get_if<Pident>(&id->path.v)) {
        auto f = env.find(pi->id.stamp);
        return f != env.end() ? f->second : Sd::Dynamic;
      }
      return Sd::Dynamic;  // Pdot / Papply
    }
    if (auto* lt = std::get_if<Texp_let>(&e.desc)) {
      auto env2 = env;  // classify each binding in the OLD env, then the body
      for (auto& b : lt->bindings)
        if (auto* v = std::get_if<Tpat_var>(&b.pat.desc))
          env2[v->id.stamp] = classify_rec_expr(b.expr, env);
      return classify_rec_expr(*lt->body, env2);
    }
    if (auto* sq = std::get_if<Texp_sequence>(&e.desc))
      return classify_rec_expr(*sq->e2, env);
    if (auto* si = std::get_if<Texp_struct_item>(&e.desc))
      return classify_rec_expr(*si->body, env);
    if (auto* pk = std::get_if<Texp_pack>(&e.desc))
      return classify_rec_modexpr(*pk->me, env);
    if (auto* ap = std::get_if<Texp_apply>(&e.desc)) {
      // `ref e` (%makemutable) is a statically-sized allocation.
      if (auto* cid = std::get_if<Texp_ident>(&ap->fn->desc)) {
        std::string last;
        if (auto* pd = std::get_if<Pdot>(&cid->path.v)) last = pd->name;
        if (last == "ref" && path_root_global(cid->path)) return Sd::Static;
      }
      // A partial application (some argument omitted/abstracted -> a null arg
      // expr) builds a closure, which has a statically-known size.
      for (auto& a : ap->args)
        if (!a.second) return Sd::Static;
      return Sd::Dynamic;
    }
    // Statically-sized forms (constructors, records, tuples, functions,
    // arrays, unit-returning writes, constants, ...).
    if (std::holds_alternative<Texp_construct>(e.desc) ||
        std::holds_alternative<Texp_record>(e.desc) ||
        std::holds_alternative<Texp_variant>(e.desc) ||
        std::holds_alternative<Texp_tuple>(e.desc) ||
        std::holds_alternative<Texp_constant>(e.desc) ||
        std::holds_alternative<Texp_array>(e.desc) ||
        std::holds_alternative<Texp_for>(e.desc) ||
        std::holds_alternative<Texp_setfield>(e.desc) ||
        std::holds_alternative<Texp_while>(e.desc) ||
        std::holds_alternative<Texp_setinstvar>(e.desc) ||
        std::holds_alternative<Texp_lazy>(e.desc) ||
        std::holds_alternative<Texp_atomic_loc>(e.desc) ||
        std::holds_alternative<Texp_extension_ctor>(e.desc) ||
        std::holds_alternative<Texp_unreachable>(e.desc))
      return Sd::Static;
    // Everything else (apply, match, if, field, assert, try, send, new,
    // object, instvar, override, letop) has no statically-known size.
    return Sd::Dynamic;
  }
  Sd classify_rec_modexpr(const tt::ModuleExpr& me,
                          const std::unordered_map<long long, Sd>& env) {
    using namespace tt;
    if (auto* mi = std::get_if<Tmod_ident>(&me.desc)) {
      if (auto* pi = std::get_if<Pident>(&mi->path.v)) {
        auto f = env.find(pi->id.stamp);
        return f != env.end() ? f->second : Sd::Dynamic;
      }
      return Sd::Dynamic;
    }
    if (std::holds_alternative<Tmod_structure>(me.desc) ||
        std::holds_alternative<Tmod_functor>(me.desc))
      return Sd::Static;
    if (auto* c = std::get_if<Tmod_constraint>(&me.desc))
      return classify_rec_modexpr(*c->expr, env);  // approx: Tcoerce_none
    if (auto* u = std::get_if<Tmod_unpack>(&me.desc))
      return classify_rec_expr(*u->e, env);
    return Sd::Dynamic;  // apply / apply_unit
  }

  // For `let rec`, bind all pattern names before typing any RHS so the names are
  // in scope in their own and siblings' bodies.
  std::vector<tt::ValueBinding> value_bindings(RecFlag rf,
                                               const std::vector<ValueBinding>& vbs) {
    std::vector<tt::ValueBinding> out;
    if (rf == RecFlag::Recursive) {
      std::vector<tt::Pattern> pats;
      pats.reserve(vbs.size());
      for (auto& vb : vbs) pats.push_back(pattern(vb.pat));
      for (size_t i = 0; i < vbs.size(); ++i) {
        tt::ValueBinding b;
        b.pat = std::move(pats[i]);
        const Pvc_constraint* nt_vc = nullptr;
        if (vbs[i].constraint_)
          if (auto* vc = std::get_if<Pvc_constraint>(&*vbs[i].constraint_))
            if (!vc->univars.empty()) nt_vc = vc;
        if (nt_vc) {  // `let rec f : type a. t = e`: newtypes scope the RHS
          auto saved = type_scope;
          std::unordered_map<int, std::string> nt;
          std::vector<std::string> names;
          for (auto& u : nt_vc->univars) {
            tt::Ident id = fresh_type(u.txt);
            nt[id.stamp] = u.txt;
            names.push_back(u.txt);
          }
          b.expr = expr(*vbs[i].expr);
          b.attrs = &vbs[i].attrs;
          apply_newtype_constraint(b, vbs[i], *nt_vc, nt, names);
          type_scope = std::move(saved);
        } else {
          b.expr = expr(*vbs[i].expr);
          b.attrs = &vbs[i].attrs;
          apply_value_constraint(b, vbs[i]);
        }
        // Value_rec_check size classification for <def_rec[_dynamic]>.
        b.rec_dynamic = classify_rec_expr(b.expr, {}) == Sd::Dynamic;
        out.push_back(std::move(b));
      }
    } else {
      for (auto& vb : vbs) out.push_back(value_binding(vb));
    }
    return out;
  }

  // A nested structure (module body) gets its own scopes; outer type/module/open
  // bindings are visible inside but inner ones don't leak out.
  std::vector<tt::StructureItem> nested_structure(const ast::Structure& s) {
    auto st = type_scope;
    auto md = module_scope;
    auto op = opens;
    push();
    std::vector<tt::StructureItem> out;
    for (auto& it : s) out.push_back(structure_item(it));
    pop();
    type_scope = std::move(st);
    module_scope = std::move(md);
    opens = std::move(op);
    return out;
  }

  // A signature gets its own scopes (its types/etc. don't leak out).
  std::vector<tt::SignatureItem> signature(const Signature& items) {
    auto st = type_scope; auto md = module_scope; auto mt = modtype_scope;
    auto fr = field_registry; auto op = opens;
    push();
    std::vector<tt::SignatureItem> out;
    for (auto& it : items) {
      tt::SignatureItem si;
      si.loc = it.loc;
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        tt::ValueDesc vd;
        vd.id = fresh_anon(v->vd.name.txt);
        vd.type = core_type(*v->vd.type);
        vd.loc = v->vd.loc;
        vd.attrs = &v->vd.attrs;
        si.desc = tt::Tsig_value{std::move(vd)};
      } else if (auto* t = std::get_if<Psig_type>(&it.desc)) {
        tt::Tsig_type ts;
        ts.rf = t->rf;
        ts.decls = type_decls(t->decls, t->rf);
        si.desc = std::move(ts);
      } else if (auto* m = std::get_if<Psig_module>(&it.desc)) {
        tt::Tsig_module out;
        // `module M = Path` in a signature is an alias: Mp_absent.
        out.md.present = !std::holds_alternative<Pmty_alias>(m->md.type->desc);
        out.md.type = std::make_unique<tt::ModuleType>(module_type_t(*m->md.type));
        if (m->md.name.txt) out.md.id = fresh_module(*m->md.name.txt);
        else out.md.id.stamp = -1;  // anonymous `module _ : S`, prints as `_`
        out.md.attrs = &m->md.attrs;
        si.desc = std::move(out);
      } else if (auto* rm = std::get_if<Psig_recmodule>(&it.desc)) {
        tt::Tsig_recmodule out;
        std::vector<tt::Ident> ids;  // pre-bind all names (mutual refs)
        for (auto& d : rm->decls)
          ids.push_back(fresh_module(d.name.txt ? *d.name.txt : "_"));
        for (size_t k = 0; k < rm->decls.size(); ++k) {
          tt::ModuleDecl md;
          md.id = ids[k];
          md.attrs = &rm->decls[k].attrs;
          md.type = std::make_unique<tt::ModuleType>(module_type_t(*rm->decls[k].type));
          out.decls.push_back(std::move(md));
        }
        si.desc = std::move(out);
      } else if (auto* mt = std::get_if<Psig_modtype>(&it.desc)) {
        tt::Tsig_modtype out;
        out.id = fresh_modtype(mt->name.txt);
        out.attrs = &mt->attrs;
        if (mt->type) {
          out.type = std::make_unique<tt::ModuleType>(module_type_t(*mt->type));
          collect_exports(*out.type, modtype_exports_[out.id.stamp]);
          members_of_ast_modtype(*mt->type, modtype_members_[out.id.stamp], 0);
        }
        si.desc = std::move(out);
      } else if (auto* inc = std::get_if<Psig_include>(&it.desc)) {
        tt::Tsig_include out;
        out.mt = std::make_unique<tt::ModuleType>(module_type_t(inc->mt));
        out.attrs = &inc->attrs;
        // Include instantiates FRESH idents for the included items (typemod
        // substitutes new stamps); later sig items resolve to those.
        for (auto& [n, id] : exports_of(*out.mt).types) type_scope[n] = fresh_anon(n);
        for (auto& [n, id] : exports_of(*out.mt).modules) module_scope[n] = fresh_anon(n);
        si.desc = std::move(out);
      } else if (auto* ex = std::get_if<Psig_exception>(&it.desc)) {
        tt::Tsig_exception te;
        te.ctor = ext_ctor(ex->exn.ctor);
        te.attrs = &ex->exn.attrs;
        si.desc = std::move(te);
      } else if (auto* tx = std::get_if<Psig_typext>(&it.desc)) {
        auto& e = tx->ext;
        tt::Tsig_typext out;
        out.path = resolve_type(e.path.txt);
        for (auto& p : e.params)
          out.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
        for (auto& c : e.ctors) out.ctors.push_back(ext_ctor(c));
        out.private_ = e.priv == PrivateFlag::Private;
        out.attrs = &e.attrs;
        si.desc = std::move(out);
      } else if (auto* pr = std::get_if<Psig_primitive>(&it.desc)) {
        tt::Tsig_primitive tp;
        if (pr->pd.alias) tp.alias = resolve_prim_alias(*pr->pd.alias);
        tp.id = fresh_anon(pr->pd.name.txt);
        tp.loc = pr->pd.loc;
        tp.attrs = &pr->pd.attrs;
        if (pr->pd.type) tp.type = core_type(*pr->pd.type);
        tp.prims = pr->pd.prims;
        // A later `external v = z` in the same signature aliases this one, so
        // bind the name in the enclosing (signature) value scope.
        scopes.back()[pr->pd.name.txt] = tp.id;
        si.desc = std::move(tp);
      } else if (auto* at = std::get_if<Psig_attribute>(&it.desc)) {
        si.desc = tt::Tsig_attribute{at->name, &at->payload};
      } else if (auto* op = std::get_if<Psig_open>(&it.desc)) {
        tt::Tsig_open out;
        out.override_ = op->ovr == OverrideFlag::Override;
        out.path = resolve_module(op->id.txt);
        OpenEntry oe;
        oe.path = out.path;
        load_open_names_lid(op->id.txt, oe);
        opens.push_back(std::move(oe));
        si.desc = std::move(out);
      } else if (auto* cl = std::get_if<Psig_class>(&it.desc)) {
        tt::Tsig_class out;
        for (auto& d : cl->decls) {
          class_scope_[d.name.txt] = fresh_anon(d.name.txt);
          type_scope[d.name.txt] = fresh_anon(d.name.txt);
        }
        for (auto& d : cl->decls)
          out.decls.push_back(std::make_unique<tt::ClassTypeDeclaration>(
              class_type_declaration(d)));
        si.desc = std::move(out);
      } else if (auto* clt = std::get_if<Psig_class_type>(&it.desc)) {
        tt::Tsig_class_type out;
        for (auto& d : clt->decls) {
          class_scope_[d.name.txt] = fresh_anon(d.name.txt);
          type_scope[d.name.txt] = fresh_anon(d.name.txt);
        }
        for (auto& d : clt->decls)
          out.decls.push_back(std::make_unique<tt::ClassTypeDeclaration>(
              class_type_declaration(d)));
        si.desc = std::move(out);
      } else {
        throw TypeError("sigitem#" + std::to_string(it.desc.index()));
      }
      out.push_back(std::move(si));
    }
    pop();
    type_scope = std::move(st); module_scope = std::move(md);
    modtype_scope = std::move(mt); field_registry = std::move(fr);
    opens = std::move(op);
    return out;
  }

  tt::ModuleType module_type_t(const ModuleType& mt) {
    tt::ModuleType out;
    out.loc = mt.loc;
    if (auto* id = std::get_if<Pmty_ident>(&mt.desc)) {
      out.desc = tt::Tmty_ident{resolve_modtype(id->id.txt)};
    } else if (auto* sg = std::get_if<Pmty_signature>(&mt.desc)) {
      out.desc = tt::Tmty_signature{signature(sg->items)};
    } else if (auto* al = std::get_if<Pmty_alias>(&mt.desc)) {
      out.desc = tt::Tmty_alias{resolve_module(al->id.txt)};
    } else if (auto* fn = std::get_if<Pmty_functor>(&mt.desc)) {
      tt::Tmty_functor tf;
      auto saved = module_scope;
      if (auto* named = std::get_if<Functor_named>(&fn->param)) {
        tf.param_type = std::make_unique<tt::ModuleType>(module_type_t(*named->type));
        if (named->name.txt) tf.param = fresh_module(*named->name.txt);
      }  // Functor_unit: generative, no param/param_type
      tf.body = std::make_unique<tt::ModuleType>(module_type_t(*fn->body));
      module_scope = std::move(saved);
      out.desc = std::move(tf);
    } else if (auto* w = std::get_if<Pmty_with>(&mt.desc)) {
      tt::Tmty_with tw;
      tw.base = std::make_unique<tt::ModuleType>(module_type_t(*w->mt));
      SigExports ex = exports_of(*tw.base);
      for (auto& c : w->constraints) tw.constraints.push_back(with_item(c, ex));
      out.desc = std::move(tw);
    } else if (auto* to = std::get_if<Pmty_typeof>(&mt.desc)) {
      out.desc = tt::Tmty_typeof{
          std::make_unique<tt::ModuleExpr>(module_expr(*to->me))};
    } else {
      throw TypeError("module_type#" + std::to_string(mt.desc.index()));
    }
    return out;
  }

  // One `with ...` constraint.  The constrained item's path resolves inside the
  // base module type's own signature (reusing its ident stamps); an unknown
  // base (e.g. a cmi modtype) mints one shared fresh ident for both the path
  // and the declaration, keeping the stamp correlation the dump needs.
  tt::WithItem with_item(const WithConstraint& c, const SigExports& ex) {
    tt::WithItem out;
    auto lhs_type_decl = [&](const LongidentLoc& lid, const TypeDeclaration& td) {
      auto* l = std::get_if<Lident>(&lid.txt.v);
      if (!l) throw TypeError("with-type on dotted path");
      auto f = ex.types.find(l->name);
      tt::Ident id = f != ex.types.end() ? f->second : fresh_anon(l->name);
      out.path.v = tt::Pident{id};
      auto saved = type_scope;  // elaborate the decl under the resolved ident
      type_scope[td.name.txt] = id;
      auto d = type_declaration(td);
      type_scope = std::move(saved);
      return d;
    };
    auto lhs_module = [&](const LongidentLoc& lid) {
      auto* l = std::get_if<Lident>(&lid.txt.v);
      if (!l) throw TypeError("with-module on dotted path");
      auto f = ex.modules.find(l->name);
      tt::Ident id = f != ex.modules.end() ? f->second : fresh_anon(l->name);
      out.path.v = tt::Pident{id};
    };
    if (auto* t = std::get_if<Pwith_type>(&c)) {
      out.c = tt::Twith_type{lhs_type_decl(t->lid, *t->td)};
    } else if (auto* t = std::get_if<Pwith_typesubst>(&c)) {
      out.c = tt::Twith_typesubst{lhs_type_decl(t->lid, *t->td)};
    } else if (auto* m = std::get_if<Pwith_module>(&c)) {
      lhs_module(m->lid1);
      out.c = tt::Twith_module{resolve_module(m->lid2.txt)};
    } else if (auto* m = std::get_if<Pwith_modsubst>(&c)) {
      lhs_module(m->lid1);
      out.c = tt::Twith_modsubst{resolve_module(m->lid2.txt)};
    } else {
      throw TypeError("with-modtype constraint");
    }
    return out;
  }

  // A structure whose inferred signature has a shadowed name (the same name
  // bound twice in one namespace) is simplified, generating a coercion -- printed
  // as a transparent extra module_expr layer (Tmodtype_implicit, pr5164).
  struct ShadowSets {
    std::set<std::string> vals, types, mods, modtypes;
  };
  // Add an item's exported names to `s`, returning true as soon as any name
  // collides with one already present (a signature-simplify duplicate).  An
  // `open struct ... end` contributes the names of the anonymous structure,
  // which is how private helpers shadow earlier bindings.
  bool shadow_scan_item(const StructureItem& it, ShadowSets& s) {
    auto dup = [](std::set<std::string>& set, const std::string& n) {
      return !set.insert(n).second;
    };
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      for (auto& b : sv->bindings) {
        std::vector<std::string> vs;
        collect_pat_vars(b.pat, vs);
        for (auto& n : vs) if (dup(s.vals, n)) return true;
      }
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      if (dup(s.vals, pr->prim.name.txt)) return true;
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      for (auto& d : ty->decls) if (dup(s.types, d.name.txt)) return true;
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      if (mb->binding.name.txt && dup(s.mods, *mb->binding.name.txt)) return true;
    } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
      for (auto& b : rm->bindings)
        if (b.name.txt && dup(s.mods, *b.name.txt)) return true;
    } else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
      if (dup(s.modtypes, mt->name.txt)) return true;
    } else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
      // `open struct ... end` introduces the anonymous structure's bindings as
      // hidden/removable signature items, so any non-empty one forces the
      // signature-simplify coercion even when no name actually clashes.
      if (auto* ms = std::get_if<Pmod_structure>(&op->expr.desc))
        if (!ms->items.empty()) return true;
    }
    return false;
  }
  bool structure_shadows(const std::vector<StructureItem>& items) {
    ShadowSets s;
    for (auto& it : items)
      if (shadow_scan_item(it, s)) return true;
    return false;
  }

  // As above, but over the TRANSCRIBED items -- so an `include ME` contributes
  // the members it actually spliced in (from include_exports_ / cmi), letting a
  // later `let x`/second `include` that collides with an included name trigger
  // the signature-simplify coercion (pr5164, pr7601).  A miss here is harmless
  // (no wrapper, still valid); a false positive would add a spurious wrapper, so
  // only names we can resolve precisely are counted.
  bool structure_shadows_tt(const std::vector<tt::StructureItem>& items) {
    ShadowSets s;
    auto dup = [](std::set<std::string>& set, const std::string& n) {
      return !set.insert(n).second;
    };
    for (auto& it : items) {
      if (auto* v = std::get_if<tt::Tstr_value>(&it.desc)) {
        for (auto& b : v->bindings) {
          std::unordered_set<std::string> vs;
          pat_var_names(b.pat, vs);
          for (auto& n : vs) if (dup(s.vals, n)) return true;
        }
      } else if (auto* p = std::get_if<tt::Tstr_primitive>(&it.desc)) {
        if (dup(s.vals, p->id.name)) return true;
      } else if (auto* t = std::get_if<tt::Tstr_type>(&it.desc)) {
        for (auto& d : t->decls) if (dup(s.types, d.id.name)) return true;
      } else if (auto* m = std::get_if<tt::Tstr_module>(&it.desc)) {
        if (dup(s.mods, m->id.name)) return true;
      } else if (auto* rm = std::get_if<tt::Tstr_recmodule>(&it.desc)) {
        for (auto& b : rm->bindings) if (dup(s.mods, b.id.name)) return true;
      } else if (auto* mt = std::get_if<tt::Tstr_modtype>(&it.desc)) {
        if (dup(s.modtypes, mt->id.name)) return true;
      } else if (auto* inc = std::get_if<tt::Tstr_include>(&it.desc)) {
        ModExports iex;
        auto f = include_exports_.find(inc->expr.get());
        if (f != include_exports_.end()) iex = f->second;
        else if (auto* body = module_body(*inc->expr)) collect_module_exports(*body, iex);
        for (auto& n : iex.values) if (dup(s.vals, n)) return true;
        for (auto& n : iex.types) if (dup(s.types, n)) return true;
        for (auto& [n, st] : iex.submodule_stamps) if (dup(s.mods, n)) return true;
      } else if (auto* op = std::get_if<tt::Tstr_open>(&it.desc)) {
        // `open struct ... end`: the anon structure's bindings become hidden,
        // removable signature items, forcing the coercion even absent a clash.
        if (auto* body = module_body(*op->expr))
          if (!body->empty()) return true;
      }
    }
    return false;
  }

  // Wrap a global-rooted (cmi-loaded) module_expr ident in the transparent
  // strengthening coercion layer; pass anything else through unchanged.
  // A path is strengthened when its root is a global module, or a local alias
  // module (`module S = P`), including dotted paths through it (`S.Make`).
  bool path_global_or_alias(const tt::Path& p) {
    if (path_root_global(p)) return true;
    const tt::Path* root = &p;
    while (auto* pd = std::get_if<tt::Pdot>(&root->v)) root = pd->prefix.get();
    if (auto* pi = std::get_if<tt::Pident>(&root->v))
      return alias_module_stamps_.count(pi->id.stamp) > 0;
    return false;
  }

  tt::ModuleExprBox strengthen_global(tt::ModuleExprBox me) {
    if (auto* mi = std::get_if<tt::Tmod_ident>(&me->desc))
      if (path_global_or_alias(mi->path)) {
        auto wrap = std::make_unique<tt::ModuleExpr>();
        wrap->loc = me->loc;
        wrap->desc = tt::Tmod_constraint{std::move(me), nullptr, true};
        return wrap;
      }
    return me;
  }

  tt::ModuleExpr module_expr(const ModuleExpr& me) {
    tt::ModuleExpr out;
    out.loc = me.loc;
    if (auto* mi = std::get_if<Pmod_ident>(&me.desc)) {
      out.desc = tt::Tmod_ident{resolve_module(mi->id.txt)};
    } else if (auto* ms = std::get_if<Pmod_structure>(&me.desc)) {
      out.desc = tt::Tmod_structure{nested_structure(ms->items)};
      if (structure_shadows_tt(std::get<tt::Tmod_structure>(out.desc).items)) {
        // implicit signature-simplify coercion (Tmodtype_implicit)
        auto inner = std::make_unique<tt::ModuleExpr>(std::move(out));
        inner->loc = me.loc;
        out = tt::ModuleExpr{};
        out.loc = me.loc;
        out.desc = tt::Tmod_constraint{std::move(inner), nullptr, true};
      }
    } else if (auto* fn = std::get_if<Pmod_functor>(&me.desc)) {
      tt::Tmod_functor tf;
      auto saved = module_scope;
      if (auto* named = std::get_if<Functor_named>(&fn->param)) {
        tf.param_type = std::make_unique<tt::ModuleType>(module_type_t(*named->type));
        if (named->name.txt) {
          tf.param = fresh_module(*named->name.txt);
          functor_param_stamps_.insert(tf.param->stamp);
          // The param's signature members resolve inside the body (`open X`,
          // dotted access): register them like a local module's exports.
          ModExports ex;
          members_of_ast_modtype(*named->type, ex, 0);
          module_exports_[tf.param->stamp] = std::move(ex);
          // An inline-record constructor from the param's signature (`X.B { r }`)
          // keeps its Record_inlined repr in the body -- the signature scope
          // reverts field_registry, so register those fields persistently here.
          register_sig_inline_records(*named->type);
        }
      }  // Functor_unit: generative, no param/param_type
      tf.body = std::make_unique<tt::ModuleExpr>(module_expr(*fn->body));
      module_scope = std::move(saved);
      out.desc = std::move(tf);
    } else if (auto* ap = std::get_if<Pmod_apply>(&me.desc)) {
      // A cmi-loaded (global-rooted) functor path AND argument each carry an
      // implicit strengthening coercion -- a transparent extra module_expr layer.
      out.desc = tt::Tmod_apply{
          strengthen_global(std::make_unique<tt::ModuleExpr>(module_expr(*ap->f))),
          strengthen_global(std::make_unique<tt::ModuleExpr>(module_expr(*ap->arg)))};
    } else if (auto* cn = std::get_if<Pmod_constraint>(&me.desc)) {
      out.desc = tt::Tmod_constraint{
          std::make_unique<tt::ModuleExpr>(module_expr(*cn->me)),
          std::make_unique<tt::ModuleType>(module_type_t(*cn->mt))};
    } else if (auto* au = std::get_if<Pmod_apply_unit>(&me.desc)) {
      // OCaml's type_application gives the apply-unit node the functor's own
      // location, not the parse span that also covers the `()`.
      out.loc = au->f->loc;
      auto fnme = std::make_unique<tt::ModuleExpr>(module_expr(*au->f));
      if (auto* mi = std::get_if<tt::Tmod_ident>(&fnme->desc))
        if (path_root_global(mi->path)) {  // same strengthening as Tmod_apply
          auto wrap = std::make_unique<tt::ModuleExpr>();
          wrap->loc = fnme->loc;
          wrap->desc = tt::Tmod_constraint{std::move(fnme), nullptr, true};
          fnme = std::move(wrap);
        }
      out.desc = tt::Tmod_apply_unit{std::move(fnme)};
    } else if (auto* up = std::get_if<Pmod_unpack>(&me.desc)) {
      out.desc = tt::Tmod_unpack{std::make_unique<tt::Expression>(expr(*up->e))};
    } else {
      throw TypeError("module_expr#" + std::to_string(me.desc.index()));
    }
    return out;
  }

  // --- classes -------------------------------------------------------------
  Location none_loc() {
    Location l;
    l.start.cnum = -1;
    l.end.cnum = -1;
    l.ghost = true;
    return l;
  }
  // Elaborate a method body the way the typer does: `fun self-N -> body`, with a
  // Texp_poly extra on the body.  (Instance-variable references in the body would
  // become Texp_instvar -- handled once vals are tracked.)
  int object_no_ = 0;  // global per-class/object counter for the self-N display name
  // A class declaration reserves its class_num before its body is transcribed,
  // so a class whose body is NOT an object structure (`class u = base`) still
  // advances the counter, and its structure body (if any) uses this number.
  int pending_class_num_ = 0;
  // The self name bound in a class/object (`object (self) .. end`), or none for an
  // anonymous self.  Extracted from the parsed self pattern.
  std::optional<std::string> self_pat_name(const ast::Pattern& p) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) return v->name.txt;
    if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) return self_pat_name(*c->p);
    return std::nullopt;
  }
  tt::Expression elaborate_method(const Expression& body0, int self_n, Location selfloc,
                                  const std::optional<std::string>& self_name,
                                  const std::shared_ptr<MethsMap>& meths,
                                  bool poly = true) {
    const Expression* body = &body0;
    const CoreType* mty = nullptr;  // (method m : T = e): keep the type for the extra
    if (poly)
      if (auto* p = std::get_if<Pexp_poly>(&body->desc)) {
        if (p->t) mty = p->t->get();
        body = p->e.get();
      }
    Location floc = body0.loc;  // the synthetic function & its poly extra sit on the body
    floc.ghost = true;
    selfloc.ghost = true;       // the self parameter sits on the self location
    // The self parameter's idents are allocated BEFORE the body so a self-send in
    // the body resolves to the alias ident (self-N).  Bind the self name to it.
    tt::Ident self_inner = fresh_anon("self-*");
    tt::Ident self_alias = fresh_anon("self-" + std::to_string(self_n));
    // Hide any local that is also an instance variable (a class parameter) so
    // its reference in this body resolves to Texp_instvar, not the outer local.
    std::vector<std::pair<size_t, std::pair<std::string, tt::Ident>>> hidden;
    for (auto& [nm, id] : instvars_)
      for (size_t fi = scopes.size(); fi-- > 0;)
        if (auto f = scopes[fi].find(nm); f != scopes[fi].end()) {
          hidden.push_back({fi, {nm, f->second}});
          scopes[fi].erase(f);
          break;
        }
    push();
    if (self_name) {
      scopes.back()[*self_name] = self_alias;
      self_meths_[self_alias.stamp] = meths;
    }
    tt::Expression be = expr(*body);
    pop();
    for (auto& [fi, kv] : hidden) scopes[fi][kv.first] = kv.second;
    if (poly) {  // methods carry a Texp_poly extra; initializers do not
      tt::ExprExtra ex;
      ex.kind = tt::ExprExtra::Kind::Poly;
      ex.poly_has_type = mty != nullptr;
      // A method type annotation is always a Ttyp_poly; a bare (non-`'a.`) type is
      // wrapped in Ttyp_poly([], _) with the inner type's location.
      if (mty)
        ex.ctype = std::holds_alternative<Ptyp_poly>(mty->desc) ? core_type(*mty)
                                                                : poly_wrap(*mty);
      ex.loc = floc;
      be.extras.insert(be.extras.begin(), std::move(ex));  // Texp_poly first
    }
    // synthetic self parameter: Tpat_alias self-N (Tpat_var self-*), at the self loc
    tt::Pattern selfvar;
    selfvar.loc = selfloc;
    selfvar.desc = tt::Tpat_var{self_inner};
    tt::Pattern selfpat;
    selfpat.loc = selfloc;
    selfpat.desc = tt::Tpat_alias{self_alias,
                                  std::make_unique<tt::Pattern>(std::move(selfvar))};
    tt::FunctionParam fp;
    fp.label = ArgLabel{};
    fp.pat = std::make_unique<tt::Pattern>(std::move(selfpat));
    tt::Texp_function fn;
    fn.params.push_back(std::move(fp));
    fn.body = std::make_unique<tt::Expression>(std::move(be));
    tt::Expression out;
    // The method's synthetic function sits on the ghost body location; an
    // initializer's function keeps the (non-ghost) initializer-expression loc.
    out.loc = body0.loc;
    out.loc.ghost = poly;
    out.desc = std::move(fn);
    return out;
  }
  tt::ClassExpr class_structure_expr(const ast::ClassStructure& cs, Location celoc) {
    tt::ClassExpr out;
    out.loc = celoc;
    out.desc = tt::Tcl_structure{build_class_structure(cs)};
    return out;
  }
  // Build a class_structure (shared by classes and inline `object .. end`).
  tt::ClassStructure build_class_structure(const ast::ClassStructure& cs) {
    tt::ClassStructure ts;
    push();  // contain the self / instance-variable bindings to this structure
    // self pattern: Tpat_alias "selfpat-*" (<self>).  For an anonymous self the
    // inner is Tpat_any at the (zero-width) self location; for a named self it is
    // the elaborated self pattern (Tpat_var, plus a Tpat_extra_constraint for
    // `(self : 'ty)`).  The alias sits on the _none_ location.
    std::optional<std::string> self_name = self_pat_name(cs.self);
    tt::Pattern inner;
    if (std::holds_alternative<Ppat_any>(cs.self.desc)) {
      Location selfloc = cs.self.loc;
      selfloc.ghost = true;
      inner.loc = selfloc;
      inner.desc = tt::Tpat_any{};
    } else {
      inner = pattern(cs.self);
    }
    tt::Pattern selfp;
    selfp.loc = none_loc();
    selfp.desc = tt::Tpat_alias{fresh_anon("selfpat-*"),
                                std::make_unique<tt::Pattern>(std::move(inner))};
    ts.self = std::make_unique<tt::Pattern>(std::move(selfp));
    // Pre-register all instance variables (in scope in every method, any order).
    auto saved_iv = instvars_;
    for (auto& f : cs.fields)
      if (auto* v = std::get_if<Pcf_val>(&f.desc))
        instvars_[v->name.txt] = fresh_anon(v->name.txt);
    // Each method gets a shared ident (the class' "meths" table); a self-send
    // resolves the method to this ident (Tmeth_val).
    auto meths = std::make_shared<MethsMap>();
    for (auto& f : cs.fields)
      if (auto* m = std::get_if<Pcf_method>(&f.desc))
        (*meths)[m->name.txt] = fresh_anon(m->name.txt);
    // self-N shared by all this object's methods.  A class DECLARATION reserves
    // its number up front (pending_class_num_) -- even a non-structure body
    // consumes one (typeclass.ml's class_num); a standalone/nested `object` gets
    // a fresh number here.
    int self_n;
    if (pending_class_num_) { self_n = pending_class_num_; pending_class_num_ = 0; }
    else self_n = ++object_no_;
    for (auto& f : cs.fields) {
      tt::ClassField cf;
      cf.loc = f.loc;
      if (!f.attrs.empty()) cf.attrs = &f.attrs;
      if (auto* m = std::get_if<Pcf_method>(&f.desc)) {
        tt::Tcf_method tm;
        tm.name = m->name.txt;
        tm.private_ = m->priv == PrivateFlag::Private;
        if (auto* vk = std::get_if<Cfk_virtual>(&m->kind)) {  // method virtual m : t
          tm.virtual_ = true;
          tm.vtype = method_poly(*vk->type, f.loc, /*use_field_loc=*/false);
        } else {
          auto& cc = std::get<Cfk_concrete>(m->kind);
          tm.override_ = cc.ovr == OverrideFlag::Override;
          tm.expr = std::make_unique<tt::Expression>(
              elaborate_method(*cc.e, self_n, cs.self.loc, self_name, meths));
        }
        cf.desc = std::move(tm);
      } else if (auto* v = std::get_if<Pcf_val>(&f.desc)) {
        tt::Tcf_val tv;
        tv.name = v->name.txt;
        tv.mutable_ = v->mut == MutableFlag::Mutable;
        if (auto* vk = std::get_if<Cfk_virtual>(&v->kind)) {  // val virtual v : t
          tv.virtual_ = true;
          tv.vtype = core_type(*vk->type);
        } else {
          auto& cc = std::get<Cfk_concrete>(v->kind);
          tv.override_ = cc.ovr == OverrideFlag::Override;
          tv.expr = std::make_unique<tt::Expression>(expr(*cc.e));
        }
        cf.desc = std::move(tv);
      } else if (auto* in = std::get_if<Pcf_inherit>(&f.desc)) {
        tt::Tcf_inherit ti;
        ti.override_ = in->ovr == OverrideFlag::Override;
        if (in->as_) ti.super = in->as_->txt;
        // The parent class-expr carries its own coercion when it is an ident
        // (Pcl_constr -> Tcl_constraint); a non-ident parent (e.g. `inherit (c
        // args)`) is transcribed directly, matching typeclass.ml.
        ti.ce = std::make_unique<tt::ClassExpr>(class_expr_t(*in->ce));
        // Inherited instance variables become fresh instvars of this class
        // (minted here, after the parent class-expr, matching OCaml's order) so
        // a later method/initializer body sees `y` as Texp_instvar.  Ancestor
        // methods get fresh idents; `as super` sees them as a self-send table.
        int pstamp = parent_class_stamp(*in->ce);
        if (pstamp >= 0) {
          auto vi = class_vals_.find(pstamp);
          if (vi != class_vals_.end())
            for (auto& n : vi->second) instvars_[n] = fresh_anon(n);
          auto mi = class_methods_.find(pstamp);
          if (mi != class_methods_.end()) {
            auto anc = std::make_shared<MethsMap>();
            for (auto& n : mi->second) {
              tt::Ident id = fresh_anon(n);
              (*anc)[n] = id;
              (*meths)[n] = id;  // self can also call inherited methods
            }
            if (in->as_) {
              tt::Ident sup = fresh_local(in->as_->txt);  // `as super` binds a value
              self_meths_[sup.stamp] = anc;
            }
          }
        }
        cf.desc = std::move(ti);
      } else if (auto* ini = std::get_if<Pcf_initializer>(&f.desc)) {
        tt::Tcf_initializer tin;
        tin.expr = std::make_unique<tt::Expression>(
            elaborate_method(*ini->e, self_n, cs.self.loc, self_name, meths,
                             /*poly=*/false));
        cf.desc = std::move(tin);
      } else if (auto* c = std::get_if<Pcf_constraint>(&f.desc)) {
        tt::Tcf_constraint tc;
        tc.t1 = core_type(*c->t1);
        tc.t2 = core_type(*c->t2);
        cf.desc = std::move(tc);
      } else if (auto* at = std::get_if<Pcf_attribute>(&f.desc)) {  // [@@@attr]
        cf.desc = tt::Tcf_attribute{at->name, &at->payload};
      } else {
        throw TypeError("class_field#" + std::to_string(f.desc.index()));
      }
      ts.fields.push_back(std::move(cf));
    }
    instvars_ = std::move(saved_iv);
    pop();
    return ts;
  }
  void collect_pat_vars(const Pattern& p, std::vector<std::string>& out) {
    if (auto* v = std::get_if<Ppat_var>(&p.desc)) out.push_back(v->name.txt);
    else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) collect_pat_vars(*c->p, out);
    else if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) { for (auto& e : t->elems) collect_pat_vars(*e, out); }
    else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) { out.push_back(a->name.txt); collect_pat_vars(*a->p, out); }
  }
  tt::ClassExpr class_expr_t(const ClassExpr& ce) {
    if (auto* fn = std::get_if<Pcl_fun>(&ce.desc)) {  // class c <pat> = ..
      if (fn->default_) throw TypeError("class optional param");
      tt::ClassExpr out;
      out.loc = ce.loc;
      tt::Tcl_fun tf;
      tf.label = fn->label;
      tf.pat = std::make_unique<tt::Pattern>(pattern(fn->pat));  // the param ident
      // A class parameter is ALSO captured as an instance variable (a distinct
      // fresh ident): a method/initializer body sees it via self (Texp_instvar),
      // but a `val` initializer -- evaluated in the parameter scope before the
      // object exists -- sees the plain local (Texp_ident).  So keep the local
      // AND register the instvar; elaborate_method hides the local.
      std::vector<std::string> pvars;
      collect_pat_vars(fn->pat, pvars);
      for (auto& nm : pvars) instvars_[nm] = fresh_anon(nm);
      tf.body = std::make_unique<tt::ClassExpr>(class_expr_t(*fn->body));
      out.desc = std::move(tf);
      return out;
    }
    if (auto* ps = std::get_if<Pcl_structure>(&ce.desc))
      return class_structure_expr(ps->cs, ce.loc);
    if (auto* cc = std::get_if<Pcl_constr>(&ce.desc)) {  // a class path, e.g. `inherit b`
      tt::Tcl_ident ti;
      ti.path = resolve_class(cc->id.txt);
      for (auto& a : cc->args) ti.args.push_back(std::make_unique<tt::CoreType>(core_type(*a)));
      tt::ClassExpr inner;
      inner.loc = ce.loc;
      inner.desc = std::move(ti);
      // A class ident is ALWAYS coerced to its class type: Tcl_constraint(_, None),
      // a transparent extra class_expr layer at the same loc (typeclass.ml).
      tt::ClassExpr out;
      out.loc = ce.loc;
      out.desc = tt::Tcl_constraint{std::make_unique<tt::ClassExpr>(std::move(inner))};
      return out;
    }
    if (auto* cn = std::get_if<Pcl_constraint>(&ce.desc)) {  // (ce : ct)
      tt::ClassExpr out;
      out.loc = ce.loc;
      tt::Tcl_constraint tc;
      tc.ce = std::make_unique<tt::ClassExpr>(class_expr_t(*cn->ce));
      tc.ct = std::make_unique<tt::ClassType>(class_type_t(*cn->ct));
      out.desc = std::move(tc);
      return out;
    }
    if (auto* ap = std::get_if<Pcl_apply>(&ce.desc)) {  // ce arg ..
      tt::ClassExpr out;
      out.loc = ce.loc;
      tt::Tcl_apply ta;
      // `(ce) args`: the function part is transcribed directly -- OCaml's
      // typeclass does NOT wrap it in a Tcl_constraint (toplevel_lets).
      ta.fn = std::make_unique<tt::ClassExpr>(class_expr_t(*ap->ce));
      for (auto& [lbl, e] : ap->args)
        ta.args.emplace_back(lbl, std::make_unique<tt::Expression>(expr(*e)));
      out.desc = std::move(ta);
      return out;
    }
    if (auto* op = std::get_if<Pcl_open>(&ce.desc)) {  // let open M in ce
      tt::ClassExpr out;
      out.loc = ce.loc;
      tt::Tcl_open to;
      to.override_ = op->ovr == OverrideFlag::Override;
      to.path = resolve_module(op->id.txt);
      auto saved_opens = opens;
      OpenEntry oe;
      oe.path = to.path;
      load_open_names_lid(op->id.txt, oe);
      if (const ModExports* ex = exports_by_path(oe.path)) {
        for (auto& n : ex->values) oe.values.insert(n);
        for (auto& n : ex->types) oe.types.insert(n);
        for (auto& [n, st] : ex->submodule_stamps) oe.submodules.insert(n);
      }
      opens.push_back(std::move(oe));
      to.body = std::make_unique<tt::ClassExpr>(class_expr_t(*op->body));
      opens = std::move(saved_opens);
      out.desc = std::move(to);
      return out;
    }
    if (auto* lt = std::get_if<Pcl_let>(&ce.desc)) {  // let [rec] .. in ce
      tt::ClassExpr out;
      out.loc = ce.loc;
      tt::Tcl_let tl;
      tl.rf = lt->rf;
      // The let bindings are elaborated as ordinary value bindings; each bound
      // variable is then captured as a fresh instance variable (l2) so method
      // bodies see it, and shadowed as a local so the body's class_expr does not.
      tl.bindings = value_bindings(lt->rf, lt->bindings);
      std::vector<std::string> lvars;
      for (auto& b : lt->bindings) collect_pat_vars(b.pat, lvars);
      for (auto& nm : lvars) {
        tt::Ident src = resolve_local_ident(nm);  // the let-bound var (Texp_ident)
        tt::Ident iv = fresh_anon(nm);             // the instance-variable ident
        auto e = std::make_unique<tt::Expression>();
        e->loc = none_loc();
        e->loc.ghost = true;
        tt::Path p;
        p.v = tt::Pident{src};
        e->desc = tt::Texp_ident{p};
        tl.ivars.emplace_back(iv, std::move(e));
        scopes.back().erase(nm);
        instvars_[nm] = iv;
      }
      tl.body = std::make_unique<tt::ClassExpr>(class_expr_t(*lt->body));
      out.desc = std::move(tl);
      return out;
    }
    throw TypeError("class_expr#" + std::to_string(ce.desc.index()));
  }
  // Look up a just-bound local's ident (for class-let ivar rebinds).
  tt::Ident resolve_local_ident(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return fresh_anon(name);  // shouldn't happen for a just-bound let var
  }
  // The class-ident stamp a parent class-expr (`inherit P` / `inherit P a`)
  // denotes, if it's a locally-known class path; -1 otherwise.
  int parent_class_stamp(const ClassExpr& ce) {
    const ClassExpr* cur = &ce;
    while (auto* ap = std::get_if<Pcl_apply>(&cur->desc)) cur = &*ap->ce;
    if (auto* cn = std::get_if<Pcl_constr>(&cur->desc))
      if (auto* l = std::get_if<Lident>(&cn->id.txt.v)) {
        auto it = class_scope_.find(l->name);
        if (it != class_scope_.end()) return it->second.stamp;
      }
    return -1;
  }
  // A class' val/method names in declaration order, following `inherit` parents
  // (their names already recorded in class_{vals,methods}_).  Unwraps the
  // class-expr down to its structure.
  void collect_class_members(const ClassExpr& ce, std::vector<std::string>& vals,
                             std::vector<std::string>& meths) {
    const ClassExpr* cur = &ce;
    while (true) {
      if (auto* fn = std::get_if<Pcl_fun>(&cur->desc)) { cur = &*fn->body; continue; }
      if (auto* cn = std::get_if<Pcl_constraint>(&cur->desc)) { cur = &*cn->ce; continue; }
      break;
    }
    auto* ps = std::get_if<Pcl_structure>(&cur->desc);
    if (!ps) return;
    for (auto& f : ps->cs.fields) {
      if (auto* v = std::get_if<Pcf_val>(&f.desc)) vals.push_back(v->name.txt);
      else if (auto* m = std::get_if<Pcf_method>(&f.desc)) meths.push_back(m->name.txt);
      else if (auto* in = std::get_if<Pcf_inherit>(&f.desc)) {
        int ps2 = parent_class_stamp(*in->ce);
        if (ps2 >= 0) {
          auto vi = class_vals_.find(ps2);
          if (vi != class_vals_.end())
            for (auto& n : vi->second) vals.push_back(n);
          auto mi = class_methods_.find(ps2);
          if (mi != class_methods_.end())
            for (auto& n : mi->second) meths.push_back(n);
        }
      }
    }
  }

  tt::ClassDeclaration class_declaration(const ast::ClassDeclaration& d) {
    tt::ClassDeclaration out;
    out.loc = d.loc;
    out.virt = d.virt == VirtualFlag::Virtual;
    out.name = d.name.txt;
    for (auto& p : d.params) out.params.push_back(core_type(*p));
    auto saved_iv = instvars_;
    push();  // a scope for class parameters
    // Reserve this declaration's class_num (consumed by its structure body, or
    // dropped if the body is a class ident/apply) so self-N numbering matches
    // OCaml's global counter [yamagata].
    int saved_pending = pending_class_num_;
    pending_class_num_ = ++object_no_;
    out.expr = class_expr_t(d.expr);
    pending_class_num_ = saved_pending;
    pop();
    instvars_ = std::move(saved_iv);
    // Record this class' (own + inherited) val/method names so a later subclass
    // can inherit them.
    auto cs = class_scope_.find(d.name.txt);
    if (cs != class_scope_.end())
      collect_class_members(d.expr, class_vals_[cs->second.stamp],
                            class_methods_[cs->second.stamp]);
    return out;
  }
  // A method annotation is always a Ttyp_poly; a bare type is wrapped in
  // Ttyp_poly([], _).  Class-type methods locate the wrapper at the field
  // (use_field_loc); class-body virtual methods locate it at the inner type.
  tt::CoreType method_poly(const CoreType& t, Location field_loc, bool use_field_loc = true) {
    if (std::holds_alternative<Ptyp_poly>(t.desc)) return core_type(t);
    tt::CoreType inner = core_type(t);
    tt::CoreType poly;
    poly.loc = use_field_loc ? field_loc : inner.loc;
    poly.desc = tt::Ttyp_poly{{}, std::make_unique<tt::CoreType>(std::move(inner))};
    return poly;
  }
  tt::ClassType class_type_t(const ast::ClassType& ct) {
    tt::ClassType out;
    out.loc = ct.loc;
    if (auto* cn = std::get_if<Pcty_constr>(&ct.desc)) {
      tt::Tcty_constr tc;
      tc.path = resolve_class(cn->id.txt);
      for (auto& a : cn->args)
        tc.args.push_back(std::make_unique<tt::CoreType>(core_type(*a)));
      out.desc = std::move(tc);
    } else if (auto* sg = std::get_if<Pcty_signature>(&ct.desc)) {
      out.desc = tt::Tcty_signature{class_signature(sg->cs)};
    } else if (auto* ar = std::get_if<Pcty_arrow>(&ct.desc)) {
      tt::Tcty_arrow ta;
      ta.label = ar->label;
      ta.dom = std::make_unique<tt::CoreType>(core_type(*ar->dom));
      ta.cod = std::make_unique<tt::ClassType>(class_type_t(*ar->cod));
      out.desc = std::move(ta);
    } else {
      throw TypeError("class_type#" + std::to_string(ct.desc.index()));
    }
    return out;
  }
  tt::ClassSignature class_signature(const ast::ClassSignature& cs) {
    tt::ClassSignature out;
    out.self = core_type(*cs.self);
    for (auto& f : cs.fields) {
      tt::ClassTypeField cf;
      cf.loc = f.loc;
      if (auto* in = std::get_if<Pctf_inherit>(&f.desc)) {
        cf.desc = tt::Tctf_inherit{std::make_unique<tt::ClassType>(class_type_t(*in->ct))};
      } else if (auto* v = std::get_if<Pctf_val>(&f.desc)) {
        tt::Tctf_val tv;
        tv.name = v->name.txt;
        tv.mutable_ = v->mut == MutableFlag::Mutable;
        tv.virtual_ = v->virt == VirtualFlag::Virtual;
        tv.type = core_type(*v->type);
        cf.desc = std::move(tv);
      } else if (auto* m = std::get_if<Pctf_method>(&f.desc)) {
        tt::Tctf_method tm;
        tm.name = m->name.txt;
        tm.private_ = m->priv == PrivateFlag::Private;
        tm.virtual_ = m->virt == VirtualFlag::Virtual;
        tm.type = method_poly(*m->type, f.loc);
        cf.desc = std::move(tm);
      } else if (auto* c = std::get_if<Pctf_constraint>(&f.desc)) {
        tt::Tctf_constraint tc;
        tc.t1 = core_type(*c->t1);
        tc.t2 = core_type(*c->t2);
        cf.desc = std::move(tc);
      } else {
        throw TypeError("class_type_field#" + std::to_string(f.desc.index()));
      }
      out.fields.push_back(std::move(cf));
    }
    return out;
  }
  tt::ClassTypeDeclaration class_type_declaration(const ast::ClassTypeDeclaration& d) {
    tt::ClassTypeDeclaration out;
    out.loc = d.loc;
    out.virt = d.virt == VirtualFlag::Virtual;
    out.name = d.name.txt;
    out.attrs = &d.attrs;
    for (auto& p : d.params) out.params.push_back(core_type(*p));
    push();  // a scope for the class-type parameters
    out.expr = class_type_t(d.expr);
    pop();
    return out;
  }

  tt::StructureItem structure_item(const StructureItem& it) {
    tt::StructureItem si;
    si.loc = it.loc;
    if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
      tt::Tstr_value out;
      out.rf = sv->rf;
      out.bindings = value_bindings(sv->rf, sv->bindings);
      si.desc = std::move(out);
    } else if (auto* ev = std::get_if<Pstr_eval>(&it.desc)) {
      si.desc = tt::Tstr_eval{std::make_unique<tt::Expression>(expr(*ev->e))};
    } else if (auto* ty = std::get_if<Pstr_type>(&it.desc)) {
      tt::Tstr_type out;
      out.rf = ty->rf;
      out.decls = type_decls(ty->decls, ty->rf);
      si.desc = std::move(out);
    } else if (auto* pr = std::get_if<Pstr_primitive>(&it.desc)) {
      tt::Tstr_primitive tp;
      if (pr->prim.alias) tp.alias = resolve_prim_alias(*pr->prim.alias);
      tp.id = fresh_local(pr->prim.name.txt);  // `external f : ..` binds f
      tp.loc = pr->prim.loc;
      tp.attrs = &pr->prim.attrs;
      if (pr->prim.type) tp.type = core_type(*pr->prim.type);
      tp.prims = pr->prim.prims;
      for (auto& s : pr->prim.prims) {  // record %apply / %revapply externals
        if (s == "%apply") prim_apply_stamp_[tp.id.stamp] = 2;
        else if (s == "%revapply") prim_apply_stamp_[tp.id.stamp] = 1;
      }
      si.desc = std::move(tp);
    } else if (auto* op = std::get_if<Pstr_open>(&it.desc)) {
      tt::Tstr_open to;
      to.override_ = op->ovr == OverrideFlag::Override;
      if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
        // Path open: later references resolve THROUGH the path (Pdot).
        tt::Path mpath = resolve_module(mi->id.txt);
        to.expr = std::make_unique<tt::ModuleExpr>();
        to.expr->loc = op->expr.loc;
        to.expr->desc = tt::Tmod_ident{mpath};  // copy; mpath reused below
        OpenEntry oe;
        oe.path = std::move(mpath);
        load_open_names_lid(mi->id.txt, oe);  // stdlib cmi names
        if (const ModExports* ex = exports_by_path(oe.path)) {  // local module
          for (auto& n : ex->values) oe.values.insert(n);
          for (auto& n : ex->types) oe.types.insert(n);
          for (auto& [n, st] : ex->submodule_stamps) oe.submodules.insert(n);
        }
        opens.push_back(std::move(oe));
      } else {
        // Generalized open (struct literal / functor application): the items
        // instantiate FRESH idents that bind directly (Pident references).
        to.expr = std::make_unique<tt::ModuleExpr>(module_expr(op->expr));
        ModExports tmp;
        if (const ModExports* ex = exports_of_modexpr(*to.expr, tmp)) {
          for (auto& n : ex->values) fresh_local(n);
          for (auto& n : ex->types) fresh_type(n);
          for (auto& [n, st] : ex->submodule_stamps) {
            auto id = fresh_module(n);
            auto f = module_exports_.find(st);
            if (f != module_exports_.end()) module_exports_[id.stamp] = f->second;
          }
        }
      }
      si.desc = std::move(to);
    } else if (auto* ex = std::get_if<Pstr_exception>(&it.desc)) {
      tt::Tstr_exception te;
      te.ctor = ext_ctor(ex->exn.ctor);
      te.attrs = &ex->exn.attrs;
      si.desc = std::move(te);
    } else if (auto* tx = std::get_if<Pstr_typext>(&it.desc)) {
      auto& e = tx->ext;
      tt::Tstr_typext out;
      out.path = resolve_type(e.path.txt);
      for (auto& p : e.params)
        out.params.push_back(std::make_unique<tt::CoreType>(core_type(*p)));
      for (auto& c : e.ctors) out.ctors.push_back(ext_ctor(c));
      out.private_ = e.priv == PrivateFlag::Private;
      out.attrs = &e.attrs;
      si.desc = std::move(out);
    } else if (auto* mt = std::get_if<Pstr_modtype>(&it.desc)) {
      tt::Tstr_modtype out;
      out.id = fresh_modtype(mt->name.txt);
      out.attrs = &mt->attrs;
      if (mt->type) {
        modtype_ast_[mt->name.txt] = &*mt->type;
        out.type = std::make_unique<tt::ModuleType>(module_type_t(*mt->type));
        collect_exports(*out.type, modtype_exports_[out.id.stamp]);
        members_of_ast_modtype(*mt->type, modtype_members_[out.id.stamp], 0);
      }
      si.desc = std::move(out);
    } else if (auto* at = std::get_if<Pstr_attribute>(&it.desc)) {
      si.desc = tt::Tstr_attribute{at->name, &at->payload};
    } else if (auto* mb = std::get_if<Pstr_module>(&it.desc)) {
      auto& b = mb->binding;
      tt::Tstr_module tm;
      tm.present = !std::holds_alternative<Pmod_ident>(b.expr.desc);  // alias=Absent
      // ...but aliasing a bare functor PARAMETER stays Present.
      if (auto* mi = std::get_if<Pmod_ident>(&b.expr.desc))
        if (auto* l = std::get_if<Lident>(&mi->id.txt.v)) {
          auto it = module_scope.find(l->name);
          if (it != module_scope.end() &&
              functor_param_stamps_.count(it->second.stamp))
            tm.present = true;
        }
      // A plain `module M = E` is non-recursive: E is elaborated with M NOT yet
      // bound (so `module M = struct .. M.x .. end` sees an OUTER M), then M is
      // bound for the following items.
      tm.expr = std::make_unique<tt::ModuleExpr>(module_expr(b.expr));
      // Anonymous `module _ = E` binds no name and allocates no stamp (matching
      // the oracle's Ident.t option = None); it prints as `_`.
      if (b.name.txt) tm.id = fresh_module(*b.name.txt);
      else { tm.id.name = "_"; tm.id.stamp = -1; }
      tm.attrs = &b.attrs;
      // Record what M exports (for later local opens / dotted resolution).
      if (auto* body = module_body(*tm.expr)) {
        collect_module_exports(*body, module_exports_[tm.id.stamp]);
      } else if (auto* mi2 = std::get_if<tt::Tmod_ident>(&tm.expr->desc)) {
        if (auto* pi = std::get_if<tt::Pident>(&mi2->path.v)) {  // module A = B
          auto f = module_exports_.find(pi->id.stamp);
          if (f != module_exports_.end())
            module_exports_[tm.id.stamp] = f->second;
        }
      } else if (tm.id.stamp >= 0) {  // functor application, unpack, ...
        ModExports tmp;
        if (auto* up = std::get_if<Pmod_unpack>(&b.expr.desc)) {
          // `module M = (val e : S)`: members from the package's module type.
          if (auto* ce = std::get_if<Pexp_constraint>(&up->e->desc))
            if (auto* pk = std::get_if<Ptyp_package>(&ce->t->desc))
              members_of_modtype_lid(pk->path.txt, tmp, 0);
          module_exports_[tm.id.stamp] = std::move(tmp);
        } else if (const ModExports* ex = exports_of_modexpr(*tm.expr, tmp)) {
          module_exports_[tm.id.stamp] = *ex;
        }
      }
      // `module M = P` makes M an alias, so a later use of M as a functor
      // argument (or as the root of a functor path M.F) is strengthened.
      if (std::holds_alternative<tt::Tmod_ident>(tm.expr->desc))
        alias_module_stamps_.insert(tm.id.stamp);
      si.desc = std::move(tm);
    } else if (auto* in = std::get_if<Pstr_include>(&it.desc)) {
      auto me = std::make_unique<tt::ModuleExpr>(module_expr(in->expr));
      // Including an aliased module (a cmi Mty_alias like `include List`, or a
      // local `module A = ..` alias) expands it behind a transparent
      // strengthening constraint, like functor paths.
      if (auto* mi = std::get_if<tt::Tmod_ident>(&me->desc))
        if (path_global_or_alias(mi->path)) {
          auto wrap = std::make_unique<tt::ModuleExpr>();
          wrap->loc = me->loc;
          wrap->desc = tt::Tmod_constraint{std::move(me), nullptr, true};
          me = std::move(wrap);
        }
      // `include ME` splices ME's members into the enclosing structure under
      // FRESH idents (Env.enter_signature refreshes every item, even for a
      // struct literal or a path -- verified against the oracle), so later
      // bare references bind to the include's idents, not the source's.
      ModExports src;
      if (auto* mi = std::get_if<Pmod_ident>(&in->expr.desc)) {
        OpenEntry oe;
        load_open_names_lid(mi->id.txt, oe);  // cmi module members
        src.values = std::move(oe.values);
        src.types = std::move(oe.types);
        for (auto& n : oe.submodules) src.submodule_stamps.emplace(n, -1);
        if (const ModExports* ex = exports_by_path(resolve_module(mi->id.txt))) {
          for (auto& n : ex->values) src.values.insert(n);
          for (auto& n : ex->types) src.types.insert(n);
          for (auto& [n, st] : ex->submodule_stamps) src.submodule_stamps[n] = st;
        }
      } else if (auto* up = std::get_if<Pmod_unpack>(&in->expr.desc)) {
        // `include (val e : S)`: members from the package's module type.
        if (auto* ce = std::get_if<Pexp_constraint>(&up->e->desc))
          if (auto* pk = std::get_if<Ptyp_package>(&ce->t->desc))
            members_of_modtype_lid(pk->path.txt, src, 0);
      } else {
        ModExports tmp;
        if (const ModExports* ex = exports_of_modexpr(*me, tmp)) src = *ex;
      }
      ModExports fresh;  // this include's contribution to the enclosing module
      for (auto& n : src.values) { fresh_local(n); fresh.values.insert(n); }
      for (auto& n : src.types) { fresh_type(n); fresh.types.insert(n); }
      for (auto& [n, st] : src.submodule_stamps) {
        auto id = fresh_module(n);
        if (st >= 0) {
          auto f = module_exports_.find(st);
          if (f != module_exports_.end()) module_exports_[id.stamp] = f->second;
        }
        fresh.submodule_stamps[n] = id.stamp;
      }
      include_exports_[me.get()] = std::move(fresh);
      si.desc = tt::Tstr_include{std::move(me), &in->attrs};
    } else if (auto* cl = std::get_if<Pstr_class>(&it.desc)) {
      tt::Tstr_class tc;
      for (auto& d : cl->decls) {  // pre-register names (stamp first, mutual refs)
        class_scope_[d.name.txt] = fresh_anon(d.name.txt);
        type_scope[d.name.txt] = fresh_anon(d.name.txt);  // the object type ctor
      }
      for (auto& d : cl->decls) tc.decls.push_back(class_declaration(d));
      si.desc = std::move(tc);
    } else if (auto* clt = std::get_if<Pstr_class_type>(&it.desc)) {
      tt::Tstr_class_type tc;
      for (auto& d : clt->decls) {  // a class type introduces the `#c` class name
        class_scope_[d.name.txt] = fresh_anon(d.name.txt);
        type_scope[d.name.txt] = fresh_anon(d.name.txt);  // and the object type ctor
      }
      for (auto& d : clt->decls) tc.decls.push_back(class_type_declaration(d));
      si.desc = std::move(tc);
    } else if (auto* rm = std::get_if<Pstr_recmodule>(&it.desc)) {
      tt::Tstr_recmodule tr;
      std::vector<tt::Ident> ids;  // pre-bind all names (stamp order + mutual refs)
      for (auto& b : rm->bindings) {
        if (b.name.txt) ids.push_back(fresh_module(*b.name.txt));
        else ids.push_back(tt::Ident{"_", -1, tt::Ident::Local});  // anon: no stamp
      }
      // Each rec module's members come from its ascribed signature; register
      // them before typing any body (mutual references).
      for (size_t k = 0; k < rm->bindings.size(); ++k) {
        if (ids[k].stamp < 0) continue;
        if (auto* mc = std::get_if<Pmod_constraint>(&rm->bindings[k].expr.desc)) {
          ModExports ex;
          members_of_ast_modtype(*mc->mt, ex, 0);
          module_exports_[ids[k].stamp] = std::move(ex);
        }
      }
      for (size_t k = 0; k < rm->bindings.size(); ++k) {
        const ModuleExpr& be = rm->bindings[k].expr;
        auto me = module_expr(be);
        // The constrained module's typedtree location is the inner module's
        // (the `= struct ..`), not the binding's `: S = struct ..` span.
        if (auto* mc = std::get_if<Pmod_constraint>(&be.desc)) me.loc = mc->me->loc;
        tr.bindings.push_back({ids[k],
                               std::make_unique<tt::ModuleExpr>(std::move(me)),
                               &rm->bindings[k].attrs});
      }
      si.desc = std::move(tr);
    } else {
      throw TypeError("stritem#" + std::to_string(it.desc.index()));
    }
    return si;
  }
};

}  // namespace

typedtree::Structure type_structure(const ast::Structure& s) {
  Typer t;
  auto aux = infer_dump_aux(s);  // inference side-tables (Slice 3)
  t.partiality = &aux.match_partial;
  t.function_cases_partial = &aux.function_cases_partial;
  t.param_partial = &aux.param_partial;
  t.apply_plans = &aux.apply_plans;
  t.flatten_construct = &aux.flatten_construct;
  t.record_fields = &aux.record_fields;
  t.record_reprs = &aux.record_reprs;
  t.format_lits = &aux.format_lits;
  typedtree::Structure out;
  for (auto& it : s) out.push_back(t.structure_item(it));
  return out;
}

}  // namespace cppcaml
