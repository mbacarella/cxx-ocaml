// Port of typing/shape.ml.  See shape.hpp.
#include "cppcaml/typing/shape.hpp"

namespace cppcaml::typing::shape {

std::string_view to_string(SigComponentKind k) {
  switch (k) {
    case SigComponentKind::Value: return "value";
    case SigComponentKind::Type: return "type";
    case SigComponentKind::Constructor: return "constructor";
    case SigComponentKind::Label: return "label";
    case SigComponentKind::Module: return "module";
    case SigComponentKind::Module_type: return "module type";
    case SigComponentKind::Extension_constructor: return "extension constructor";
    case SigComponentKind::Class: return "class";
    case SigComponentKind::Class_type: return "class type";
  }
  return "";
}
bool can_appear_in_types(SigComponentKind k) {
  return !(k == SigComponentKind::Value || k == SigComponentKind::Extension_constructor);
}

// Stdlib.compare on (string, constant constructor): caml_string_compare
// (memcmp of the common prefix, then lengths), then the constructor index
int ItemCmp::operator()(const Item& a, const Item& b) const {
  int c = a.name.compare(b.name);
  if (c != 0) return c < 0 ? -1 : 1;
  return a.kind < b.kind ? -1 : a.kind > b.kind ? 1 : 0;
}

namespace item {
Item make(std::string_view str, SigComponentKind ns) { return {str, ns}; }
Item value(Ident::t id) { return {ident::name(id), SigComponentKind::Value}; }
Item type_(Ident::t id) { return {ident::name(id), SigComponentKind::Type}; }
Item constr(Ident::t id) { return {ident::name(id), SigComponentKind::Constructor}; }
Item label(Ident::t id) { return {ident::name(id), SigComponentKind::Label}; }
Item module_(Ident::t id) { return {ident::name(id), SigComponentKind::Module}; }
Item module_type(Ident::t id) { return {ident::name(id), SigComponentKind::Module_type}; }
Item extension_constructor(Ident::t id) { return {ident::name(id), SigComponentKind::Extension_constructor}; }
Item class_(Ident::t id) { return {ident::name(id), SigComponentKind::Class}; }
Item class_type(Ident::t id) { return {ident::name(id), SigComponentKind::Class_type}; }
}  // namespace item

static Shape* mk(const Uid* uid, Shape::Kind k) {
  Shape* s = make<Shape>();
  if (uid) {
    s->has_uid = true;
    s->uid = *uid;
  }
  s->kind = k;
  return s;
}

t strip_head_aliases(t s) {
  while (s->kind == Shape::Kind::Alias) s = s->t1;
  return s;
}

std::pair<Ident::t, t> fresh_var(const Uid& uid, std::string_view name) {
  Ident::t v = Ident::create_local(name);
  Shape* s = mk(&uid, Shape::Kind::Var);
  s->var = v;
  return {v, s};
}

// `let for_unnamed_functor_param = Ident.create_local "()"`: a
// module-initialization value, created on first use
Ident::t for_unnamed_functor_param() {
  static Ident::t id = [] {
    ZoneScope perm(permanent_zone());
    return Ident::create_local(OCAML_LIT("()"));
  }();
  return id;
}

t var(const Uid& uid, Ident::t id) {
  Shape* s = mk(&uid, Shape::Kind::Var);
  s->var = id;
  return s;
}
t abs(const Uid* uid, Ident::t v, t body) {
  Shape* s = mk(uid, Shape::Kind::Abs);
  s->var = v;
  s->t1 = body;
  return s;
}
t str(const Uid* uid, ItemMap m) {
  Shape* s = mk(uid, Shape::Kind::Struct);
  s->map = m;
  return s;
}
t alias(const Uid* uid, t a) {
  Shape* s = mk(uid, Shape::Kind::Alias);
  s->t1 = a;
  return s;
}
t leaf(const Uid& uid) { return mk(&uid, Shape::Kind::Leaf); }
t approx(t a) {
  Shape* s = make<Shape>(*a);
  s->approximated = true;
  return s;
}
t proj(const Uid* uid, t a, const Item& it) {
  switch (a->kind) {
    // When stuck projecting in a leaf we propagate the leaf as a best effort
    case Shape::Kind::Leaf: return approx(a);
    case Shape::Kind::Struct:
      if (const t* r = a->map.find_opt(it)) return *r;
      return approx(a);  // ill-typed program
    default: {
      Shape* s = mk(uid, Shape::Kind::Proj);
      s->t1 = a;
      s->item = it;
      return s;
    }
  }
}
t app(const Uid* uid, t f, t arg) {
  Shape* s = mk(uid, Shape::Kind::App);
  s->t1 = f;
  s->t2 = arg;
  return s;
}
std::optional<std::pair<Ident::t, t>> decompose_abs(t s) {
  if (s->kind == Shape::Kind::Abs) return std::make_pair(s->var, s->t1);
  return std::nullopt;
}
t dummy_mod() {
  static t d = [] {
    ZoneScope perm(permanent_zone());
    return str(nullptr, ItemMap{});
  }();
  return d;
}

t of_path(const std::function<t(SigComponentKind, Ident::t)>& find_shape, SigComponentKind ns, Path::t path) {
  // Path of constructor M.t.C [Pextra_ty("M.t", "C")], of label M.t.lbl, of
  // label of inline record M.t.C.lbl [Pextra_ty(Pextra_ty("M.t", "C"), "lbl")]
  std::function<t(SigComponentKind, Path::t)> aux = [&](SigComponentKind k, Path::t p) -> t {
    switch (p->kind) {
      case Path::Kind::Pident: return find_shape(k, p->id);
      case Path::Kind::Pdot: return proj(nullptr, aux(SigComponentKind::Module, p->p1), Item{p->s, k});
      case Path::Kind::Papply: {
        // app (aux Module p1) ~arg:(aux Module p2): right to left
        t a = aux(SigComponentKind::Module, p->p2);
        t f = aux(SigComponentKind::Module, p->p1);
        return app(nullptr, f, a);
      }
      case Path::Kind::Pextra_ty:
        if (p->extra == Path::Extra::Pcstr_ty) {
          if (k == SigComponentKind::Label && p->p1->kind == Path::Kind::Pextra_ty)
            return proj(nullptr, aux(SigComponentKind::Constructor, p->p1), Item{p->s, k});
          return proj(nullptr, aux(SigComponentKind::Type, p->p1), Item{p->s, k});
        }
        return aux(SigComponentKind::Extension_constructor, p->p1);
    }
    throw std::logic_error("Shape.of_path");
  };
  return aux(ns, path);
}

t for_persistent_unit(std::string_view s) {
  Uid u = uid::of_compilation_unit_id(ident::name(Ident::create_persistent(s)));
  Shape* r = mk(&u, Shape::Kind::Comp_unit);
  r->str = zborrow(s);
  return r;
}
t leaf_for_unpack() {
  Shape* r = mk(nullptr, Shape::Kind::Pack);
  r->var = Ident::create_local(OCAML_LIT("Pkg"));
  return r;
}
t set_uid_if_none(t s, const Uid& u) {
  if (s->has_uid) return s;
  Shape* r = make<Shape>(*s);
  r->has_uid = true;
  r->uid = u;
  return r;
}

namespace map {
ItemMap empty() { return ItemMap{}; }
ItemMap add(ItemMap m, const Item& it, t s) { return m.add(it, s); }
ItemMap add_value(ItemMap m, Ident::t id, const Uid& uid) { return m.add(item::value(id), leaf(uid)); }
ItemMap add_value_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::value(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_type(ItemMap m, Ident::t id, t s) { return m.add(item::type_(id), s); }
ItemMap add_type_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::type_(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_constr(ItemMap m, Ident::t id, t s) { return m.add(item::constr(id), s); }
ItemMap add_constr_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::constr(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_label(ItemMap m, Ident::t id, const Uid& uid) { return m.add(item::label(id), leaf(uid)); }
ItemMap add_label_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::label(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_module(ItemMap m, Ident::t id, t s) { return m.add(item::module_(id), s); }
ItemMap add_module_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::module_(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_module_type(ItemMap m, Ident::t id, const Uid& uid) { return m.add(item::module_type(id), leaf(uid)); }
ItemMap add_module_type_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::module_type(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_extcons(ItemMap m, Ident::t id, t s) { return m.add(item::extension_constructor(id), s); }
ItemMap add_extcons_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::extension_constructor(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_class(ItemMap m, Ident::t id, const Uid& uid) { return m.add(item::class_(id), leaf(uid)); }
ItemMap add_class_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::class_(id);
  return m.add(it, proj(nullptr, s, it));
}
ItemMap add_class_type(ItemMap m, Ident::t id, const Uid& uid) { return m.add(item::class_type(id), leaf(uid)); }
ItemMap add_class_type_proj(ItemMap m, Ident::t id, t s) {
  Item it = item::class_type(id);
  return m.add(it, proj(nullptr, s, it));
}
}  // namespace map

}  // namespace cppcaml::typing::shape
