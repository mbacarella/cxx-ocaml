// Port of parsing/pprintast.ml's Doc longident printers (TYPECHECKER.md stage 9).
#include "cppcaml/typing/pprintast.hpp"

#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/path.hpp"

namespace cppcaml::typing::pprintast {

using namespace format_doc;

namespace {

bool in(char c, std::string_view cs) { return cs.find(c) != std::string_view::npos; }
constexpr std::string_view prefix_symbols = "!?~";
constexpr std::string_view infix_symbols = "=<>@^|&+-*/$%#";

bool special_infix_string(std::string_view s) {
  for (const char* x : {"asr", "land", "lor", "lsl", "lsr", "lxor", "mod", "or", ":=", "!=", "::"})
    if (s == x) return true;
  return false;
}

bool letop(std::string_view s) { return s.size() > 3 && s.substr(0, 3) == "let" && in(s[3], infix_symbols); }
bool andop(std::string_view s) { return s.size() > 3 && s.substr(0, 3) == "and" && in(s[3], infix_symbols); }

enum class Fixity { Normal, Infix, Prefix, Mixfix, Letop, Andop };
Fixity fixity_of_string(std::string_view s) {
  if (s.empty()) return Fixity::Normal;
  if (special_infix_string(s)) return Fixity::Infix;
  if (in(s[0], infix_symbols)) return Fixity::Infix;
  if (in(s[0], prefix_symbols)) return Fixity::Prefix;
  if (s[0] == '.') return Fixity::Mixfix;
  if (letop(s)) return Fixity::Letop;
  if (andop(s)) return Fixity::Andop;
  return Fixity::Normal;
}

bool needs_parens(LongidentKind kind, std::string_view txt) {
  if (kind == LongidentKind::Type) return false;
  Fixity fix = fixity_of_string(txt);
  return fix == Fixity::Infix || fix == Fixity::Mixfix || fix == Fixity::Letop || fix == Fixity::Andop ||
         (!txt.empty() && in(txt[0], prefix_symbols));
}

bool needs_spaces(std::string_view txt) { return !txt.empty() && (txt[0] == '*' || txt.back() == '*'); }

}  // namespace

void ident_of_name(LongidentKind kind, Formatter& ppf, std::string_view txt) {
  const char* format;
  if (path::is_keyword(txt)) {
    if (kind == LongidentKind::Constr && (txt == "true" || txt == "false"))
      format = "%s";
    else if (kind == LongidentKind::Value)
      format = special_infix_string(txt) ? "(%s)" : "\\#%s";
    else
      format = "\\#%s";
  } else if (!needs_parens(kind, txt)) {
    format = "%s";
  } else if (needs_spaces(txt)) {
    format = "(@;%s@;)";
  } else {
    format = "(%s)";
  }
  fprintf(ppf, format, txt);
}

void any_longident(LongidentKind kind, Formatter& ppf, Longident::t l) {
  switch (l->kind) {
    case Longident::Kind::Lident: ident_of_name(kind, ppf, l->s); break;
    case Longident::Kind::Ldot: {
      auto prefix = [&](Formatter& f) { any_longident(LongidentKind::Other, f, l->l1); };
      std::string_view txt = l->s;
      if (!needs_parens(kind, txt))
        fprintf(ppf, "%a.%a", prefix, [&](Formatter& f) { ident_of_name(kind, f, txt); });
      else if (needs_spaces(txt))
        fprintf(ppf, "%a.(@;%s@;)", prefix, txt);
      else
        fprintf(ppf, "%a.(%s)", prefix, txt);
      break;
    }
    case Longident::Kind::Lapply:
      fprintf(ppf, "%a(%a)", [&](Formatter& f) { any_longident(LongidentKind::Other, f, l->l1); },
              [&](Formatter& f) { any_longident(LongidentKind::Other, f, l->l2); });
      break;
  }
}

void longident(Formatter& ppf, Longident::t l) { any_longident(LongidentKind::Other, ppf, l); }
void constr(Formatter& ppf, Longident::t l) { any_longident(LongidentKind::Constr, ppf, l); }
void type_longident(Formatter& ppf, Longident::t l) { any_longident(LongidentKind::Type, ppf, l); }
void value_longident(Formatter& ppf, Longident::t l) { any_longident(LongidentKind::Value, ppf, l); }
void tyvar(Formatter& ppf, std::string_view s) { oprint::tyvar(ppf, s); }

}  // namespace cppcaml::typing::pprintast
