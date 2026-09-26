// Port of parsing/longident.ml for the typer.  See longident.hpp.
#include "cppcaml/typing/longident.hpp"

namespace cppcaml::typing {

using K = Longident::Kind;

Longident::t Longident::lident(std::string_view s) { return make<Longident>(K::Lident, zstr(s)); }
Longident::t Longident::ldot(t prefix, const Location& prefix_loc, std::string_view s,
                             const Location& s_loc) {
  return make<Longident>(K::Ldot, zstr(s), s_loc, prefix, prefix_loc);
}
Longident::t Longident::lapply(t f, const Location& f_loc, t a, const Location& a_loc) {
  return make<Longident>(K::Lapply, std::string_view{}, Location{}, f, f_loc, a, a_loc);
}

namespace longident {

std::string_view last(t lid) {
  switch (lid->kind) {
    case K::Lident:
    case K::Ldot: return lid->s;
    case K::Lapply: throw std::logic_error("Longident.last");
  }
  return {};
}

std::vector<std::string_view> flatten(t lid) {
  std::vector<std::string_view> acc;
  for (;;) {
    switch (lid->kind) {
      case K::Lident:
        acc.insert(acc.begin(), lid->s);
        return acc;
      case K::Ldot:
        acc.insert(acc.begin(), lid->s);
        lid = lid->l1;
        continue;
      case K::Lapply:
        throw std::logic_error("Longident.flat");
    }
  }
}

// Plain dotted rendering; Pprintast's operator/raw-identifier handling comes
// with the error-message stage.
std::string to_string(t lid) {
  switch (lid->kind) {
    case K::Lident: return std::string(lid->s);
    case K::Ldot: return to_string(lid->l1) + "." + std::string(lid->s);
    case K::Lapply: return to_string(lid->l1) + "(" + to_string(lid->l2) + ")";
  }
  return {};
}

t of_ast(const ast::Longident& lid, const Location& loc) {
  if (auto* i = std::get_if<ast::Lident>(&lid.v)) return Longident::lident(i->name);
  if (auto* d = std::get_if<ast::Ldot>(&lid.v))
    return Longident::ldot(of_ast(*d->prefix, loc), loc, d->name, loc);
  auto* a = std::get_if<ast::Lapply>(&lid.v);
  return Longident::lapply(of_ast(*a->f, loc), loc, of_ast(*a->x, loc), loc);
}

Location loc_of_ast(const ast::Location& l, std::string_view fname) {
  Location r;
  r.loc_start = Position{zstr(fname), l.start.lnum, l.start.bol, l.start.cnum};
  r.loc_end = Position{zstr(fname), l.end.lnum, l.end.bol, l.end.cnum};
  r.loc_ghost = l.ghost;
  return r;
}

}  // namespace longident

}  // namespace cppcaml::typing
