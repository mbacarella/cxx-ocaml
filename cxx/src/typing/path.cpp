// Port of typing/path.ml.  See path.hpp.
#include "cppcaml/typing/path.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace cppcaml::typing {

using K = Path::Kind;

Path::t Path::pident(Ident::t id) { return make<Path>(K::Pident, id); }
Path::t Path::pdot(t p, std::string_view s) {
  return make<Path>(K::Pdot, nullptr, p, nullptr, zborrow(s));
}
Path::t Path::papply(t f, t a) { return make<Path>(K::Papply, nullptr, f, a); }
Path::t Path::pextra_ty(t p, Extra e, std::string_view s) {
  return make<Path>(K::Pextra_ty, nullptr, p, nullptr, zborrow(s), e);
}

namespace path {

template <class Cmp>
static bool same_aux(Cmp& ident_cmp, t p1, t p2) {
  if (p1 == p2) return true;
  if (p1->kind != p2->kind) return false;
  switch (p1->kind) {
    case K::Pident:
      return ident_cmp(p1->id, p2->id);
    case K::Pdot:
      return p1->s == p2->s && same_aux(ident_cmp, p1->p1, p2->p1);
    case K::Papply:
      return same_aux(ident_cmp, p1->p1, p2->p1) && same_aux(ident_cmp, p1->p2, p2->p2);
    case K::Pextra_ty: {
      bool same_extra =
          p1->extra == p2->extra && (p1->extra == Path::Extra::Pext_ty || p1->s == p2->s);
      return same_extra && same_aux(ident_cmp, p1->p1, p2->p1);
    }
  }
  return false;
}

bool same(t p1, t p2) {
  auto c = [](Ident::t a, Ident::t b) { return ident::same(a, b); };
  return same_aux(c, p1, p2);
}

static bool ident_equiv(const std::vector<std::pair<Ident::t, Ident::t>>& id_pairs,
                        Ident::t i1, Ident::t i2) {
  using IK = Ident::Kind;
  if (i1->kind != i2->kind) return false;
  switch (i1->kind) {
    case IK::Local:
    case IK::Scoped:
    case IK::Predef:
      return i1->stamp_ == i2->stamp_;
    case IK::Global:
      return i1->name_ == i2->name_;
    case IK::Unscoped: {
      // ident.ml Unscoped.equiv
      int s1 = ident::Unscoped::stamp_of(i1->us), s2 = ident::Unscoped::stamp_of(i2->us);
      if (s1 == s2) return true;
      for (auto& [a, b] : id_pairs) {
        if (!ident::is_unscoped(a) || !ident::is_unscoped(b)) continue;
        int sa = ident::Unscoped::stamp_of(a->us), sb = ident::Unscoped::stamp_of(b->us);
        if ((sa == s1 && sb == s2) || (sb == s1 && sa == s2)) return true;
      }
      return false;
    }
  }
  return false;
}

bool equiv(const std::vector<std::pair<Ident::t, Ident::t>>& id_pairs, t p1, t p2) {
  auto c = [&](Ident::t a, Ident::t b) { return ident_equiv(id_pairs, a, b); };
  return same_aux(c, p1, p2);
}

static int rank(K k) {
  switch (k) {
    case K::Pident: return 0;
    case K::Pdot: return 1;
    case K::Papply: return 2;
    case K::Pextra_ty: return 3;
  }
  return 0;
}

static int compare_extra(t a, t b) {
  if (a->extra == Path::Extra::Pcstr_ty && b->extra == Path::Extra::Pcstr_ty) {
    int c = a->s.compare(b->s);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }
  if (a->extra == b->extra) return 0;
  return a->extra == Path::Extra::Pcstr_ty ? -1 : 1;
}

int compare(t p1, t p2) {
  if (p1 == p2) return 0;
  if (p1->kind != p2->kind) return rank(p1->kind) < rank(p2->kind) ? -1 : 1;
  switch (p1->kind) {
    case K::Pident:
      return ident::compare(p1->id, p2->id);
    case K::Pdot: {
      int h = compare(p1->p1, p2->p1);
      if (h != 0) return h;
      int c = p1->s.compare(p2->s);
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    case K::Papply: {
      int h = compare(p1->p1, p2->p1);
      return h != 0 ? h : compare(p1->p2, p2->p2);
    }
    case K::Pextra_ty: {
      int h = compare_extra(p1, p2);
      return h != 0 ? h : compare(p1->p1, p2->p1);
    }
  }
  return 0;
}

std::optional<Ident::t> find_free_opt(const std::vector<Ident::t>& ids, t p) {
  switch (p->kind) {
    case K::Pident:
      for (auto id : ids)
        if (ident::same(p->id, id)) return id;
      return std::nullopt;
    case K::Pdot:
    case K::Pextra_ty:
      return find_free_opt(ids, p->p1);
    case K::Papply: {
      auto r = find_free_opt(ids, p->p1);
      return r ? r : find_free_opt(ids, p->p2);
    }
  }
  return std::nullopt;
}

bool exists_free(const std::vector<Ident::t>& ids, t p) {
  return find_free_opt(ids, p).has_value();
}

int scope(t p) {
  switch (p->kind) {
    case K::Pident: return ident::scope(p->id);
    case K::Pdot:
    case K::Pextra_ty: return scope(p->p1);
    case K::Papply: return std::max(scope(p->p1), scope(p->p2));
  }
  return 0;
}

t subst(const std::vector<std::pair<Ident::t, t>>& id_map, t p) {
  if (id_map.empty()) return p;
  bool changed = false;
  auto aux = [&](auto& self, t q) -> t {
    switch (q->kind) {
      case K::Pident:
        for (auto& [i, np] : id_map)
          if (ident::same(i, q->id)) {
            changed = true;
            return np;
          }
        return Path::pident(q->id);
      case K::Pdot:
        return Path::pdot(self(self, q->p1), q->s);
      case K::Pextra_ty:
        return Path::pextra_ty(self(self, q->p1), q->extra, q->s);
      case K::Papply:
        return Path::papply(self(self, q->p1), self(self, q->p2));
    }
    return q;
  };
  t r = aux(aux, p);
  return changed ? r : p;
}

ident::Unscoped* check_for_unbound_unscoped_idents(const UnscopedSet& idl, t p) {
  switch (p->kind) {
    case K::Pident: {
      ident::Unscoped* us = ident::find_unscoped(p->id);
      if (!us) return nullptr;
      for (ident::Unscoped* x : idl)
        if (ident::Unscoped::same(us, x)) return nullptr;
      return us;
    }
    case K::Pdot:
    case K::Pextra_ty:
      return check_for_unbound_unscoped_idents(idl, p->p1);
    case K::Papply:
      if (auto* r = check_for_unbound_unscoped_idents(idl, p->p1)) return r;
      return check_for_unbound_unscoped_idents(idl, p->p2);
  }
  return nullptr;
}

bool contains_unscoped_ident(t p) {
  switch (p->kind) {
    case K::Pident: return ident::find_unscoped(p->id) != nullptr;
    case K::Pdot: return contains_unscoped_ident(p->p1);
    case K::Papply: return contains_unscoped_ident(p->p1) || contains_unscoped_ident(p->p2);
    case K::Pextra_ty: return contains_unscoped_ident(p->p1);
  }
  return false;
}

// lexer.mll all_keywords (Lexer.is_keyword with the default keyword set).
static bool is_keyword(std::string_view s) {
  static const std::unordered_set<std::string_view> kw = {
      "and", "as", "assert", "begin", "class", "constraint", "do", "done",
      "downto", "effect", "else", "end", "exception", "external", "false",
      "for", "fun", "function", "functor", "if", "in", "include", "inherit",
      "initializer", "lazy", "let", "match", "method", "module", "mutable",
      "new", "nonrec", "object", "of", "open", "or", "private", "rec", "sig",
      "struct", "then", "to", "true", "try", "type", "val", "virtual", "when",
      "while", "with", "lor", "lxor", "mod", "land", "lsl", "lsr", "asr"};
  return kw.count(s) != 0;
}

static std::string maybe_escape(std::string_view s) {
  return is_keyword(s) ? "\\#" + std::string(s) : std::string(s);
}

std::string name(t p) {
  switch (p->kind) {
    case K::Pident:
      return maybe_escape(ident::name(p->id));
    case K::Pdot:
      return name(p->p1) + "." + maybe_escape(p->s);
    case K::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty) return name(p->p1) + "." + maybe_escape(p->s);
      return name(p->p1);
    case K::Papply:
      return name(p->p1) + "(" + name(p->p2) + ")";
  }
  return {};
}

Ident::t head(t p) {
  switch (p->kind) {
    case K::Pident: return p->id;
    case K::Pdot:
    case K::Pextra_ty: return head(p->p1);
    case K::Papply: throw std::logic_error("Path.head");
  }
  return nullptr;
}

std::vector<Ident::t> heads(t p) {
  std::vector<Ident::t> acc;
  auto go = [&](auto& self, t q) -> void {
    switch (q->kind) {
      case K::Pident: acc.insert(acc.begin(), q->id); return;
      case K::Pdot:
      case K::Pextra_ty: self(self, q->p1); return;
      case K::Papply: self(self, q->p2); self(self, q->p1); return;
    }
  };
  go(go, p);
  return acc;
}

std::string last(t p) {
  switch (p->kind) {
    case K::Pident: return std::string(ident::name(p->id));
    case K::Pdot: return std::string(p->s);
    case K::Pextra_ty:
      if (p->extra == Path::Extra::Pcstr_ty) return std::string(p->s);
      return last(p->p1);
    case K::Papply: return last(p->p2);
  }
  return {};
}

t scrape_extra_ty(t p) {
  while (p->kind == K::Pextra_ty) p = p->p1;
  return p;
}

bool is_constructor_typath(t p) { return p->kind == K::Pextra_ty; }

std::optional<std::pair<Ident::t, std::vector<std::string_view>>> flatten(t p) {
  std::vector<std::string_view> acc;
  for (;;) {
    switch (p->kind) {
      case K::Pident:
        return std::make_pair(p->id, acc);
      case K::Pdot:
        acc.insert(acc.begin(), p->s);
        p = p->p1;
        break;
      case K::Pextra_ty:
        if (p->extra == Path::Extra::Pcstr_ty) acc.insert(acc.begin(), p->s);
        p = p->p1;
        break;
      case K::Papply:
        return std::nullopt;
    }
  }
}

}  // namespace path

}  // namespace cppcaml::typing
