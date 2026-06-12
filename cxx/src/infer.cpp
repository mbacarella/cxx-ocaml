#include "cppcaml/infer.hpp"

#include <functional>
#include <unordered_map>

namespace cppcaml::infer {

TypePtr Engine::fresh_var() {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Var;
  t->level = level;
  t->id = next_id_++;
  return t;
}
TypePtr Engine::any() {
  if (!any_) {
    any_ = std::make_shared<Type>();
    any_->kind = Type::Kind::Any;
  }
  return any_;
}
TypePtr Engine::arrow(TypePtr dom, TypePtr cod, int label, std::string lbl) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Arrow;
  t->dom = std::move(dom);
  t->cod = std::move(cod);
  t->arrow_label = label;
  t->arrow_lbl = std::move(lbl);
  t->id = next_id_++;
  return t;
}
TypePtr Engine::tuple(std::vector<TypePtr> elems) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Tuple;
  t->args = std::move(elems);
  t->id = next_id_++;
  return t;
}
TypePtr Engine::constr(std::string path, std::vector<TypePtr> args, int stamp) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Constr;
  t->path = std::move(path);
  t->args = std::move(args);
  t->stamp = stamp;
  t->id = next_id_++;
  return t;
}

Engine* Engine::trail_owner_ = nullptr;

void Engine::note(const TypePtr& n) {
  if (window_depth_) trail_.push_back({n, n->kind, n->link, n->level});
}
size_t Engine::mark() {
  ++window_depth_;
  trail_owner_ = this;
  return trail_.size();
}
void Engine::undo_to(size_t m) {
  while (trail_.size() > m) {
    Trail& e = trail_.back();
    e.node->kind = e.kind;
    e.node->link = e.link;
    e.node->level = e.level;
    trail_.pop_back();
  }
  if (--window_depth_ == 0) {
    trail_owner_ = nullptr;
    trail_.clear();
  }
}

TypePtr Engine::repr(TypePtr t) {
  while (t && t->kind == Type::Kind::Link) {
    // path compression: collapse chains as we walk -- trailed while a window is
    // open (a compressed link skipping over an in-window binding would survive
    // the rollback and resurrect it).
    if (t->link && t->link->kind == Type::Kind::Link) {
      if (trail_owner_) trail_owner_->note(t);
      t->link = repr(t->link);
    }
    t = t->link;
  }
  return t;
}

// Occurs-check (a var must not appear in the type it's unified with) plus the
// level-lowering that keeps generalization sound: every var reachable from t has
// its level capped at the bound var's level.
void Engine::occurs_and_lower(const TypePtr& var, const TypePtr& t0) {
  TypePtr t = repr(t0);
  switch (t->kind) {
    case Type::Kind::Var:
      if (t == var) throw TypeError("occurs check: recursive type");
      if (t->level > var->level) { note(t); t->level = var->level; }
      break;
    case Type::Kind::Arrow:
      occurs_and_lower(var, t->dom);
      occurs_and_lower(var, t->cod);
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
      for (auto& a : t->args) occurs_and_lower(var, a);
      break;
    case Type::Kind::Link:
    case Type::Kind::Any:
      break;  // repr already resolved / Any has no vars
  }
}

void Engine::unify(const TypePtr& a0, const TypePtr& b0) {
  TypePtr a = repr(a0), b = repr(b0);
  if (a == b) return;
  // Any absorbs: it unifies with anything.  If the other side is a variable,
  // link it to Any so it too becomes dynamic and can't later clash.
  if (a->kind == Type::Kind::Any || b->kind == Type::Kind::Any) {
    TypePtr var = a->kind == Type::Kind::Var ? a : b->kind == Type::Kind::Var ? b : nullptr;
    if (var) { note(var); var->kind = Type::Kind::Link; var->link = any(); }
    return;
  }
  if (a->kind == Type::Kind::Var) {
    occurs_and_lower(a, b);
    note(a);
    a->kind = Type::Kind::Link;
    a->link = b;
    return;
  }
  if (b->kind == Type::Kind::Var) {
    occurs_and_lower(b, a);
    note(b);
    b->kind = Type::Kind::Link;
    b->link = a;
    return;
  }
  if (a->kind == Type::Kind::Arrow && b->kind == Type::Kind::Arrow) {
    unify(a->dom, b->dom);
    unify(a->cod, b->cod);
    return;
  }
  if (a->kind == Type::Kind::Tuple && b->kind == Type::Kind::Tuple) {
    if (a->args.size() != b->args.size())
      throw TypeError("tuple arity mismatch");
    for (size_t i = 0; i < a->args.size(); ++i) unify(a->args[i], b->args[i]);
    return;
  }
  if (a->kind == Type::Kind::Constr && b->kind == Type::Kind::Constr) {
    // Compare type-constructor paths by their last component: the same type can
    // reach us with different qualifications (e.g. `t` via `open Lazy` vs the
    // source's `Lazy.t`, or stdlib's `Stdlib__M.t` vs `M.t`) and we have no Env
    // to canonicalize paths.  Matching on the final component avoids those
    // spurious clashes; over-accepting two distinct same-named types is a far
    // smaller cost here than false-rejecting valid code.
    auto last = [](const std::string& p) {
      auto d = p.rfind('.');
      return d == std::string::npos ? p : p.substr(d + 1);
    };
    // Distinct local type identities never unify, even with matching names (e.g.
    // a shadowed `type t`).  This is the only place stamps tighten unification;
    // unstamped constructors fall through to the name-based comparison, so it is
    // purely additive and can't introduce a false rejection.
    if (a->stamp && b->stamp && a->stamp != b->stamp)
      throw TypeError("type constructor mismatch: " + a->path + " vs " + b->path);
    if (last(a->path) != last(b->path) || a->args.size() != b->args.size())
      throw TypeError("type constructor mismatch: " + a->path + " vs " + b->path);
    for (size_t i = 0; i < a->args.size(); ++i) unify(a->args[i], b->args[i]);
    return;
  }
  throw TypeError("cannot unify incompatible types");
}

TypePtr Engine::instantiate(const TypePtr& scheme) {
  std::unordered_map<Type*, TypePtr> mapping;  // generic var -> fresh var
  std::function<TypePtr(const TypePtr&)> copy = [&](const TypePtr& t0) -> TypePtr {
    TypePtr t = repr(t0);
    switch (t->kind) {
      case Type::Kind::Var:
        if (t->level == GENERIC_LEVEL) {
          auto it = mapping.find(t.get());
          if (it != mapping.end()) return it->second;
          auto fv = fresh_var();
          mapping[t.get()] = fv;
          return fv;
        }
        return t;  // free var: shared, not copied
      case Type::Kind::Arrow:
        return arrow(copy(t->dom), copy(t->cod), t->arrow_label, t->arrow_lbl);
      case Type::Kind::Tuple: {
        std::vector<TypePtr> es;
        for (auto& a : t->args) es.push_back(copy(a));
        return tuple(std::move(es));
      }
      case Type::Kind::Constr: {
        std::vector<TypePtr> as;
        for (auto& a : t->args) as.push_back(copy(a));
        return constr(t->path, std::move(as), t->stamp);
      }
      case Type::Kind::Link:
        return copy(t);  // repr resolved; unreachable
      case Type::Kind::Any:
        return t;  // dynamic: shared, not copied
    }
    return t;
  };
  return copy(scheme);
}

void Engine::generalize(const TypePtr& t0) {
  TypePtr t = repr(t0);
  switch (t->kind) {
    case Type::Kind::Var:
      if (t->level > level) t->level = GENERIC_LEVEL;
      break;
    case Type::Kind::Arrow:
      generalize(t->dom);
      generalize(t->cod);
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
      for (auto& a : t->args) generalize(a);
      break;
    case Type::Kind::Link:
    case Type::Kind::Any:
      break;
  }
}

namespace {
void show_rec(const TypePtr& t0, std::string& out, bool paren,
              std::unordered_map<Type*, std::string>& names) {
  TypePtr t = Engine::repr(t0);
  switch (t->kind) {
    case Type::Kind::Var: {
      auto it = names.find(t.get());
      if (it == names.end()) {
        std::string n = "'" + std::string(1, 'a' + (char)(names.size() % 26));
        if (t->level == GENERIC_LEVEL) n += "";  // generic: plain 'a
        else n = "'_" + std::to_string(t->id);   // weak/free: '_N
        it = names.emplace(t.get(), n).first;
      }
      out += it->second;
      break;
    }
    case Type::Kind::Arrow:
      if (paren) out += "(";
      show_rec(t->dom, out, true, names);
      out += " -> ";
      show_rec(t->cod, out, false, names);
      if (paren) out += ")";
      break;
    case Type::Kind::Tuple:
      if (paren) out += "(";
      for (size_t i = 0; i < t->args.size(); ++i) {
        if (i) out += " * ";
        show_rec(t->args[i], out, true, names);
      }
      if (paren) out += ")";
      break;
    case Type::Kind::Constr:
      if (t->args.size() == 1) { show_rec(t->args[0], out, true, names); out += " "; }
      else if (t->args.size() > 1) {
        out += "(";
        for (size_t i = 0; i < t->args.size(); ++i) {
          if (i) out += ", ";
          show_rec(t->args[i], out, false, names);
        }
        out += ") ";
      }
      out += t->path;
      break;
    case Type::Kind::Link:
      break;
    case Type::Kind::Any:
      out += "_";
      break;
  }
}
}  // namespace

std::string show(const TypePtr& t) {
  std::string out;
  std::unordered_map<Type*, std::string> names;
  show_rec(t, out, false, names);
  return out;
}

}  // namespace cppcaml::infer
