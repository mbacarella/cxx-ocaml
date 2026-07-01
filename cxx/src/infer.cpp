#include "cppcaml/infer.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

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

TypePtr Engine::object_type(std::vector<std::string> names, std::vector<TypePtr> types) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Object;
  t->labels = std::move(names);
  t->args = std::move(types);
  t->id = next_id_++;
  return t;
}

TypePtr Engine::variant_type(std::vector<std::string> tags, std::vector<TypePtr> argtys,
                             std::vector<char> has_arg, int variant_kind) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Kind::Variant;
  t->labels = std::move(tags);
  t->args = std::move(argtys);
  t->tag_has_arg = std::move(has_arg);
  t->variant_kind = variant_kind;
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
    case Type::Kind::Object:
    case Type::Kind::Variant:
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
    if (a->args.size() != b->args.size()) {
      if (lenient) return;
      throw TypeError("tuple arity mismatch");
    }
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
    if (a->stamp && b->stamp && a->stamp != b->stamp) {
      if (lenient) return;
      throw TypeError("type constructor mismatch: " + a->path + " vs " + b->path);
    }
    if (last(a->path) != last(b->path) || a->args.size() != b->args.size()) {
      if (lenient) return;
      throw TypeError("type constructor mismatch: " + a->path + " vs " + b->path);
    }
    for (size_t i = 0; i < a->args.size(); ++i) unify(a->args[i], b->args[i]);
    return;
  }
  if (a->kind == Type::Kind::Object && b->kind == Type::Kind::Object) {
    // Unify the types of methods present in both; don't require equal method
    // sets (an open object row would need row variables, which we don't model --
    // best-effort, and only ever in the non-strict passes).
    for (size_t i = 0; i < a->labels.size(); ++i)
      for (size_t j = 0; j < b->labels.size(); ++j)
        if (a->labels[i] == b->labels[j]) unify(a->args[i], b->args[j]);
    return;
  }
  if (a->kind == Type::Kind::Variant && b->kind == Type::Kind::Variant) {
    // Merge the two rows into their tag-union (`[> `A]` + `[> `B]` = `[> `A | `B]`;
    // two matched `[< ..]` arms union likewise), unifying a shared tag's argument;
    // link both sides to the merge.  Link FIRST so a self-referential arg (a
    // recursive `[> `A of 'a] as 'a`) resolves to the merge node instead of
    // looping the shared-arg unify.
    std::vector<std::string> tags = a->labels;
    std::vector<TypePtr> ats = a->args;
    std::vector<char> has = a->tag_has_arg;
    for (size_t j = 0; j < b->labels.size(); ++j) {
      size_t k = 0;
      for (; k < tags.size(); ++k) if (tags[k] == b->labels[j]) break;
      if (k >= tags.size()) { tags.push_back(b->labels[j]); ats.push_back(b->args[j]); has.push_back(b->tag_has_arg[j]); }
    }
    // The TIGHTER row kind wins the merge (exact 2 > upper `[<` 1 > open `[>` 0):
    // an annotation `[ `A | `B ]` / `[< `A ]` unified with the body's matched/
    // constructed rows keeps the declared bound for display.
    int vk = std::max(a->variant_kind, b->variant_kind);
    TypePtr m = variant_type(tags, ats, has, vk);
    note(a); a->kind = Type::Kind::Link; a->link = m;
    note(b); b->kind = Type::Kind::Link; b->link = m;
    // Now unify shared-tag arguments (a/b already point at m, so a recursive arg
    // that is a/b won't re-enter this merge).
    for (size_t j = 0; j < b->labels.size(); ++j)
      for (size_t k = 0; k < m->labels.size(); ++k)
        if (m->labels[k] == b->labels[j]) { unify(m->args[k], b->args[j]); break; }
    return;
  }
  if (lenient) return;
  throw TypeError("cannot unify incompatible types");
}

TypePtr Engine::instantiate(const TypePtr& scheme) {
  std::unordered_map<Type*, TypePtr> mapping;  // generic var -> fresh var
  // Copy replacing generic vars with fresh ones, but SHARING any subtree that
  // contains none (return the original node when no child changed).  This keeps a
  // monomorphic function-param row shared, so its tags accumulate across
  // `f `A; f `B` -- copying it would give each use a fresh row and lose the union.
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
        return t;  // free (non-generic) var: shared, not copied
      case Type::Kind::Arrow: {
        TypePtr d = copy(t->dom), c = copy(t->cod);
        if (d == t->dom && c == t->cod) return t;  // no generic inside: share
        return arrow(std::move(d), std::move(c), t->arrow_label, t->arrow_lbl);
      }
      case Type::Kind::Tuple:
      case Type::Kind::Constr:
      case Type::Kind::Object:
      case Type::Kind::Variant: {
        std::vector<TypePtr> as;
        bool changed = false;
        as.reserve(t->args.size());
        for (auto& a : t->args) { as.push_back(copy(a)); if (as.back() != a) changed = true; }
        if (!changed) return t;  // monomorphic composite: share the node
        if (t->kind == Type::Kind::Tuple) return tuple(std::move(as));
        if (t->kind == Type::Kind::Constr) return constr(t->path, std::move(as), t->stamp);
        if (t->kind == Type::Kind::Object) return object_type(t->labels, std::move(as));
        return variant_type(t->labels, std::move(as), t->tag_has_arg, t->variant_kind);
      }
      case Type::Kind::Link:
        return copy(t);  // repr already resolved; unreachable
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
    case Type::Kind::Object:
    case Type::Kind::Variant:
      for (auto& a : t->args) generalize(a);
      break;
    case Type::Kind::Link:
    case Type::Kind::Any:
      break;
  }
}

// Lower a NON-generalized binding's vars (value restriction) to the current
// level instead of leaving them at the higher binding level.  Otherwise a var
// created at the inner level but tied to an outer mutable cell (`let r = ref []`)
// stays trapped above the current level, and a LATER sibling let's generalize()
// wrongly makes it generic -- breaking `let h e = r := e :: !r in (h, !r)` whose
// element type must stay shared (`('a -> unit) * 'a list`, not `* 'b list`).
void Engine::demote(const TypePtr& t0) {
  TypePtr t = repr(t0);
  switch (t->kind) {
    case Type::Kind::Var:
      if (t->level != GENERIC_LEVEL && t->level > level) { note(t); t->level = level; }
      break;
    case Type::Kind::Arrow:
      demote(t->dom);
      demote(t->cod);
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
    case Type::Kind::Object:
    case Type::Kind::Variant:
      for (auto& a : t->args) demote(a);
      break;
    case Type::Kind::Link:
    case Type::Kind::Any:
      break;
  }
}

namespace {
// Count how many times each polymorphic-variant/object row node is referenced,
// so the printer can name a SHARED one (appears 2+ times) or a CYCLIC one
// (reached 2+ via the cycle) with `as 'a`.  Variant/Object are visited-guarded
// (cycle-safe); the ref count is taken before the guard so a node reached via two
// paths still counts twice.
void count_refs(const TypePtr& t0, std::unordered_map<Type*, int>& rc,
                std::unordered_set<Type*>& seen) {
  TypePtr t = Engine::repr(t0);
  switch (t->kind) {
    case Type::Kind::Variant:
    case Type::Kind::Object:
      rc[t.get()]++;
      if (!seen.insert(t.get()).second) return;
      for (auto& a : t->args) count_refs(a, rc, seen);
      break;
    case Type::Kind::Arrow:
      count_refs(t->dom, rc, seen); count_refs(t->cod, rc, seen);
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
      for (auto& a : t->args) count_refs(a, rc, seen);
      break;
    default: break;
  }
}

// `cp` = context precedence required by the parent position: 0 top (arrow ok),
// 1 arrow-domain (tuple ok, arrow needs parens), 2 atom (both need parens).
// A row node referenced 2+ times (`rc`) -- shared across the type OR recursive --
// is named `as 'aN` at its first full print and back-referenced `'aN` afterwards,
// matching ocamlc.  `printed` marks nodes whose print has begun (so a re-entry,
// cyclic or a later occurrence, emits the back-reference and can't loop).
void show_rec(const TypePtr& t0, std::string& out, int cp,
              std::unordered_map<Type*, std::string>& names,
              std::unordered_set<Type*>& printed,
              const std::unordered_map<Type*, int>& rc) {
  TypePtr t = Engine::repr(t0);
  // A named row already being/having-been printed: emit the back-reference.
  if ((t->kind == Type::Kind::Variant || t->kind == Type::Kind::Object) &&
      printed.count(t.get())) {
    out += names[t.get()];
    return;
  }
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
      if (cp > 0) out += "(";
      if (t->arrow_label == 1) { out += t->arrow_lbl + ":";        // ~lbl:
        show_rec(t->dom, out, 1, names, printed, rc);
      } else if (t->arrow_label == 2) {  // ?lbl: -- internal type is `T option`,
        out += "?" + t->arrow_lbl + ":";  // but ocamlc displays the bare T
        TypePtr d = Engine::repr(t->dom);
        if (d->kind == Type::Kind::Constr && d->args.size() == 1 &&
            (d->path == "option" || d->path == "Stdlib.option"))
          show_rec(d->args[0], out, 1, names, printed, rc);
        else
          show_rec(t->dom, out, 1, names, printed, rc);
      } else {
        show_rec(t->dom, out, 1, names, printed, rc);   // domain: a tuple is fine unparen'd
      }
      out += " -> ";
      show_rec(t->cod, out, 0, names, printed, rc);   // -> is right-assoc: codomain stays top
      if (cp > 0) out += ")";
      break;
    case Type::Kind::Tuple:
      if (cp > 1) out += "(";
      for (size_t i = 0; i < t->args.size(); ++i) {
        if (i) out += " * ";
        show_rec(t->args[i], out, 2, names, printed, rc);  // components bind tighter than *
      }
      if (cp > 1) out += ")";
      break;
    case Type::Kind::Constr: {
      if (t->args.size() == 1) { show_rec(t->args[0], out, 2, names, printed, rc); out += " "; }
      else if (t->args.size() > 1) {
        out += "(";
        for (size_t i = 0; i < t->args.size(); ++i) {
          if (i) out += ", ";
          show_rec(t->args[i], out, 0, names, printed, rc);
        }
        out += ") ";
      }
      std::string path = t->path;
      // Stdlib is opened by default, so its types print unqualified
      // (Stdlib.out_channel -> out_channel, Stdlib.Gc.stat -> Gc.stat).
      if (path.rfind("Stdlib.", 0) == 0) path = path.substr(7);
      // The printf format type is normalised to format6; a 3-parameter one prints
      // as its `format` abbreviation, matching ocamlc.
      if (path == "format6" && t->args.size() == 3) path = "format";
      // Lazy.t is the public abbreviation of CamlinternalLazy.t; print the former.
      if (path == "CamlinternalLazy.t") path = "Lazy.t";
      out += path;
      break;
    }
    case Type::Kind::Object:
    case Type::Kind::Variant: {
      // A row referenced 2+ times (shared or recursive; an exact `[ .. ]` variant
      // has no row variable so is never named) is named `as 'aN`: assign the name
      // and mark `printed` BEFORE building the body, so a re-entry emits the
      // back-reference `'aN`.
      auto it = rc.find(t.get());
      bool multi = it != rc.end() && it->second >= 2 &&
                   !(t->kind == Type::Kind::Variant && t->variant_kind == 2);
      if (multi) {
        if (!names.count(t.get()))
          names[t.get()] = "'" + std::string(1, 'a' + (char)(names.size() % 26));
        printed.insert(t.get());
      }
      std::string body;
      if (t->kind == Type::Kind::Object) {
        body = "< ";
        for (size_t i = 0; i < t->labels.size(); ++i) {
          if (i) body += "; ";
          body += t->labels[i] + " : ";
          show_rec(t->args[i], body, 0, names, printed, rc);
        }
        body += " >";
      } else {
        std::vector<size_t> ord(t->labels.size());  // ocamlc: tags alphabetical
        for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::sort(ord.begin(), ord.end(),
                  [&](size_t x, size_t y) { return t->labels[x] < t->labels[y]; });
        body = t->variant_kind == 1 ? "[< " : t->variant_kind == 2 ? "[ " : "[> ";
        for (size_t n = 0; n < ord.size(); ++n) {
          size_t i = ord[n];
          if (n) body += " | ";
          body += "`" + t->labels[i];
          if (i < t->tag_has_arg.size() && t->tag_has_arg[i]) {
            body += " of ";
            show_rec(t->args[i], body, 0, names, printed, rc);
          }
        }
        if (!t->present.empty()) {  // `[< L > `P1 `P2 ]` present tags
          std::vector<std::string> pr = t->present;
          std::sort(pr.begin(), pr.end());
          body += " >";
          for (auto& p : pr) body += " `" + p;
        }
        body += " ]";
      }
      if (multi)
        out += (cp > 0 ? "(" : "") + body + " as " + names[t.get()] + (cp > 0 ? ")" : "");
      else out += body;
      break;
    }
    case Type::Kind::Link:
      break;
    case Type::Kind::Any:
      out += "_";
      break;
  }
}
}  // namespace

std::string show(const TypePtr& t) {
  std::unordered_map<Type*, int> rc;      // row-node reference counts (for `as 'a`)
  { std::unordered_set<Type*> seen; count_refs(t, rc, seen); }
  std::string out;
  std::unordered_map<Type*, std::string> names;
  std::unordered_set<Type*> printed;
  show_rec(t, out, 0, names, printed, rc);
  return out;
}

}  // namespace cppcaml::infer
