// Port of typing/ident.ml.  See ident.hpp.
#include "cppcaml/typing/ident.hpp"

#include <stdexcept>

namespace cppcaml::typing {

namespace ident {

static int g_currentstamp = 0;
static int g_predefstamp = 0;
static int g_reinit_level = -1;

int& currentstamp() { return g_currentstamp; }
int& predefstamp() { return g_predefstamp; }

// Types installs the trail logger (types.ml: `Ident.Unscoped.change_log :=`).
void (*unscoped_change_log)(const Unscoped::Change&) = nullptr;

Unscoped* Unscoped::create(std::string_view s) {
  ++g_currentstamp;
  return make<Unscoped>(State::Udesc, zborrow(s), g_currentstamp, nullptr);
}

Unscoped::Desc Unscoped::get_desc(const Unscoped* us) {
  while (us->state == State::Ulink) us = us->ulink;
  return {us->name, us->stamp};
}

Unscoped* Unscoped::refresh(const Unscoped* us) { return create(get_desc(us).name); }

bool Unscoped::equal(const Unscoped* a, const Unscoped* b) {
  return get_desc(a).name == get_desc(b).name;
}

bool Unscoped::same(const Unscoped* a, const Unscoped* b) {
  return stamp_of(a) == stamp_of(b);
}

Unscoped* Unscoped::repr(Unscoped* us) {
  while (us->state == State::Ulink) us = us->ulink;
  return us;
}

void Unscoped::undo_change(const Change& c) {
  c.us->state = c.state;
  c.us->name = c.name;
  c.us->stamp = c.stamp;
  c.us->ulink = c.ulink;
}

void Unscoped::link(Unscoped* a, Unscoped* b) {
  a = repr(a);
  b = repr(b);
  if (a == b) return;
  if (!unscoped_change_log) throw std::logic_error("Ident.Unscoped.change_log");
  unscoped_change_log({a, a->state, a->name, a->stamp, a->ulink});
  a->state = State::Ulink;
  a->ulink = b;
}

}  // namespace ident

using K = Ident::Kind;

Ident::t Ident::create_scoped(int scope, std::string_view s) {
  ++ident::g_currentstamp;
  return make<Ident>(K::Scoped, zborrow(s), ident::g_currentstamp, scope, nullptr);
}

Ident::t Ident::create_local(std::string_view s) {
  ++ident::g_currentstamp;
  return make<Ident>(K::Local, zborrow(s), ident::g_currentstamp, 0, nullptr);
}

Ident::t Ident::of_unscoped(ident::Unscoped* u) {
  return make<Ident>(K::Unscoped, std::string_view{}, 0, 0, u);
}

Ident::t Ident::create_predef(std::string_view s) {
  ++ident::g_predefstamp;
  return make<Ident>(K::Predef, zborrow(s), ident::g_predefstamp, 0, nullptr);
}

Ident::t Ident::create_persistent(std::string_view s) {
  return make<Ident>(K::Global, zborrow(s), 0, 0, nullptr);
}

Ident::t Ident::make_raw(Kind k, std::string_view name, int stamp, int scope,
                         ident::Unscoped* us) {
  return make<Ident>(k, zborrow(name), stamp, scope, us);
}

namespace ident {

Unscoped* find_unscoped(t id) {
  return id->kind == K::Unscoped ? id->us : nullptr;
}

std::string_view name(t id) {
  if (id->kind == K::Unscoped) return Unscoped::name_of(id->us);
  return id->name_;
}

t rename(t id) {
  switch (id->kind) {
    case K::Local:
    case K::Scoped:
      ++g_currentstamp;
      return make<Ident>(K::Local, id->name_, g_currentstamp, 0, nullptr);
    case K::Unscoped:
      return Ident::of_unscoped(Unscoped::refresh(id->us));
    default:
      throw std::logic_error("Ident.rename " + std::string(name(id)));
  }
}

std::string unique_name(t id) {
  switch (id->kind) {
    case K::Local:
    case K::Scoped:
      return std::string(id->name_) + "_" + std::to_string(id->stamp_);
    case K::Unscoped: {
      auto d = Unscoped::get_desc(id->us);
      return std::string(d.name) + "_" + std::to_string(d.stamp);
    }
    case K::Global:
      return std::string(id->name_) + "_0";
    case K::Predef:
      return std::string(id->name_);
  }
  return {};
}

std::string unique_toplevel_name(t id) {
  switch (id->kind) {
    case K::Local:
    case K::Scoped:
      return std::string(id->name_) + "/" + std::to_string(id->stamp_);
    case K::Unscoped: {
      auto d = Unscoped::get_desc(id->us);
      return std::string(d.name) + "/" + std::to_string(d.stamp);
    }
    default:
      return std::string(id->name_);
  }
}

bool persistent(t id) { return id->kind == K::Global; }

bool equal(t a, t b) {
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case K::Local:
    case K::Scoped:
    case K::Global:
      return a->name_ == b->name_;
    case K::Unscoped:
      return Unscoped::equal(a->us, b->us);
    case K::Predef:
      return a->stamp_ == b->stamp_;
  }
  return false;
}

bool same(t a, t b) {
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case K::Local:
    case K::Scoped:
    case K::Predef:
      return a->stamp_ == b->stamp_;
    case K::Unscoped:
      return Unscoped::same(a->us, b->us);
    case K::Global:
      return a->name_ == b->name_;
  }
  return false;
}

int stamp(t id) {
  switch (id->kind) {
    case K::Local:
    case K::Scoped:
      return id->stamp_;
    case K::Unscoped:
      return Unscoped::stamp_of(id->us);
    default:
      return 0;
  }
}

int compare_stamp(t a, t b) {
  int x = stamp(a), y = stamp(b);
  return x < y ? -1 : x > y ? 1 : 0;
}

int scope(t id) {
  switch (id->kind) {
    case K::Scoped: return id->scope_;
    case K::Local: return highest_scope;
    default: return lowest_scope;
  }
}

void reinit() {
  if (g_reinit_level < 0) g_reinit_level = g_currentstamp;
  else g_currentstamp = g_reinit_level;
}

bool global(t id) { return id->kind == K::Global || id->kind == K::Predef; }
bool is_predef(t id) { return id->kind == K::Predef; }
bool is_unscoped(t id) { return id->kind == K::Unscoped; }

static int cmp_int(int a, int b) { return a < b ? -1 : a > b ? 1 : 0; }
static int cmp_str(std::string_view a, std::string_view b) {
  int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// ident.ml `compare`: Local > Scoped > Global > Predef > Unscoped.
int compare(t x, t y) {
  auto rank = [](K k) {
    switch (k) {
      case K::Local: return 4;
      case K::Scoped: return 3;
      case K::Global: return 2;
      case K::Predef: return 1;
      case K::Unscoped: return 0;
    }
    return 0;
  };
  if (x->kind != y->kind) return rank(x->kind) > rank(y->kind) ? 1 : -1;
  switch (x->kind) {
    case K::Local:
    case K::Scoped: {
      int c = x->stamp_ - y->stamp_;
      if (c != 0) return c;
      return cmp_str(x->name_, y->name_);
    }
    case K::Global:
      return cmp_str(x->name_, y->name_);
    case K::Predef:
      return cmp_int(x->stamp_, y->stamp_);
    case K::Unscoped: {
      auto a = Unscoped::get_desc(x->us), b = Unscoped::get_desc(y->us);
      int c = a.stamp - b.stamp;
      if (c != 0) return c;
      return cmp_str(a.name, b.name);
    }
  }
  return 0;
}

int hash(t id) {
  auto n = name(id);
  return (n.empty() ? 0 : static_cast<unsigned char>(n[0])) ^ stamp(id);
}

std::string print(t id) {
  switch (id->kind) {
    case K::Global:
    case K::Predef:
      return std::string(id->name_) + "!";
    case K::Local:
    case K::Scoped:
      return std::string(id->name_);
    case K::Unscoped:
      return "U:" + std::string(Unscoped::name_of(id->us));
  }
  return {};
}

}  // namespace ident

}  // namespace cppcaml::typing
