// Port of parsing/longident.ml for the typer.  See longident.hpp.
#include "cppcaml/typing/longident.hpp"

namespace cppcaml::typing {

using K = Longident::Kind;

Longident::t Longident::lident(std::string_view s) { return make<Longident>(K::Lident, zborrow(s)); }
Longident::t Longident::ldot(t prefix, const Location& prefix_loc, std::string_view s,
                             const Location& s_loc) {
  return make<Longident>(K::Ldot, zborrow(s), s_loc, prefix, prefix_loc);
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

bool same(t a, t b) {
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case K::Lident: return a->s == b->s;
    case K::Ldot: return a->s == b->s && same(a->l1, b->l1);
    case K::Lapply: return same(a->l1, b->l1) && same(a->l2, b->l2);
  }
  return false;
}

static int cmp_str(std::string_view a, std::string_view b) {
  int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}
static int cmp_long(long a, long b) { return a < b ? -1 : a > b ? 1 : 0; }
static int compare_position(const Position& a, const Position& b) {
  if (int c = cmp_str(a.pos_fname, b.pos_fname)) return c;
  if (int c = cmp_long(a.pos_lnum, b.pos_lnum)) return c;
  if (int c = cmp_long(a.pos_bol, b.pos_bol)) return c;
  return cmp_long(a.pos_cnum, b.pos_cnum);
}
int compare_location(const Location& a, const Location& b) {
  if (int c = compare_position(a.loc_start, b.loc_start)) return c;
  if (int c = compare_position(a.loc_end, b.loc_end)) return c;
  return cmp_long(a.loc_ghost, b.loc_ghost);
}
// constructors with arguments are compared by tag first: Lident (0) <
// Ldot (1) < Lapply (2); `t loc` records compare txt then loc
int compare_poly(t a, t b) {
  if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
  switch (a->kind) {
    case K::Lident: return cmp_str(a->s, b->s);
    case K::Ldot:
      if (int c = compare_poly(a->l1, b->l1)) return c;
      if (int c = compare_location(a->l1_loc, b->l1_loc)) return c;
      if (int c = cmp_str(a->s, b->s)) return c;
      return compare_location(a->s_loc, b->s_loc);
    case K::Lapply:
      if (int c = compare_poly(a->l1, b->l1)) return c;
      if (int c = compare_location(a->l1_loc, b->l1_loc)) return c;
      if (int c = compare_poly(a->l2, b->l2)) return c;
      return compare_location(a->l2_loc, b->l2_loc);
  }
  return 0;
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
  r.loc_start = Position{zborrow(fname), l.start.lnum, l.start.bol, l.start.cnum};
  r.loc_end = Position{zborrow(fname), l.end.lnum, l.end.bol, l.end.cnum};
  r.loc_ghost = l.ghost;
  return r;
}

}  // namespace longident

}  // namespace cppcaml::typing
