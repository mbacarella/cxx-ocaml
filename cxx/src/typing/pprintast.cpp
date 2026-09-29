// Port of parsing/pprintast.ml's Doc longident printers (cxx/PORTING.md stage 9).
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

namespace {
// nominal_exp doc exp: appends to [ppf]; false = None
bool nominal_exp_rec(Formatter& ppf, const parsetree::Expression* exp) {
  namespace pt = parsetree;
  if (!exp->pexp_attributes.empty()) return false;
  const pt::ExpressionDesc* d = exp->pexp_desc;
  if (auto* i = pt::as<pt::Pexp_ident>(d)) {
    any_longident(LongidentKind::Value, ppf, i->lid.txt);
    return true;
  }
  if (auto* v = pt::as<pt::Pexp_variant>(d)) {
    if (v->arg) return false;
    fprintf(ppf, "`%s", v->label);
    return true;
  }
  if (auto* c = pt::as<pt::Pexp_construct>(d)) {
    if (c->arg) return false;
    any_longident(LongidentKind::Constr, ppf, c->lid.txt);
    return true;
  }
  if (auto* f = pt::as<pt::Pexp_field>(d)) {
    if (!nominal_exp_rec(ppf, f->exp)) return false;
    fprintf(ppf, ".%t", [&](Formatter& ff) { any_longident(LongidentKind::Value, ff, f->lid.txt); });
    return true;
  }
  if (auto* sd = pt::as<pt::Pexp_send>(d)) {
    if (!nominal_exp_rec(ppf, sd->exp)) return false;
    fprintf(ppf, "#%s", sd->meth.txt);
    return true;
  }
  if (auto* c = pt::as<pt::Pexp_constant>(d)) {
    const pt::ConstantDesc& cd = c->c.pconst_desc;
    switch (cd.kind) {
      case pt::ConstantDesc::Kind::Pconst_string: return false;
      case pt::ConstantDesc::Kind::Pconst_char: fprintf(ppf, "%C", cd.c); return true;
      case pt::ConstantDesc::Kind::Pconst_integer:
      case pt::ConstantDesc::Kind::Pconst_float:
        fprintf(ppf, "%s%t", cd.s, [&](Formatter& ff) {
          if (cd.has_suffix) pp_print_char(ff, cd.suffix);
        });
        return true;
    }
  }
  return false;
}
}  // namespace

std::optional<Doc> nominal_exp(const parsetree::Expression* exp) {
  Formatter f;
  if (!nominal_exp_rec(f, exp)) return std::nullopt;
  return std::move(f.doc);
}

}  // namespace cppcaml::typing::pprintast
