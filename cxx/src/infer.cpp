#include "cppcaml/infer.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>
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
  // Stamp the row's implicit tail variable with the current binding level, so a
  // value-restricted (expansive) row stays weak while a generalised one is
  // promoted to GENERIC -- this is what lets `show` name a weak row `as '_weak`
  // (`bar = wrap ()` -> `([< `Test ] as '_weak1)`) but leave a generic one plain.
  t->level = level;
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

// Primitive/abbreviation families (see the unify rule): the primitive path a
// construction/literal carries, and the stdlib abbreviation heads that expand
// to it.  family_prim_of: is this path one of the family primitives whose
// finalized heads instantiate fresh-copies (the original lazy/string slice)?
static const char* family_prim_of(const std::string& p) {
  if (p == "lazy_t") return "lazy_t";
  if (p == "string") return "string";
  if (p == "bytes") return "bytes";
  return nullptr;
}
// Any predefined scalar primitive (family relink priority 0).
static bool known_prim(const std::string& p) {
  return p == "int" || p == "char" || p == "float" || p == "bool" ||
         p == "unit" || p == "int32" || p == "int64" || p == "nativeint" ||
         p == "string" || p == "bytes" || p == "lazy_t";
}
// family_abbr_of: is this path `M.t` for a stdlib module M abbreviating a
// primitive?  (family relink priority 1)
static const char* family_abbr_of(const std::string& p) {
  if (p.size() < 3 || p.compare(p.size() - 2, 2, ".t") != 0) return nullptr;
  std::string head = p.substr(0, p.size() - 2);
  if (head.rfind("Stdlib__", 0) == 0) head = head.substr(8);
  else if (head.rfind("Stdlib.", 0) == 0) head = head.substr(7);
  if (head == "Lazy" || head == "CamlinternalLazy") return "lazy_t";
  if (head == "String" || head == "StringLabels") return "string";
  if (head == "Bytes" || head == "BytesLabels") return "bytes";
  if (head == "Int") return "int";
  if (head == "Char") return "char";
  if (head == "Float") return "float";
  if (head == "Bool") return "bool";
  if (head == "Unit") return "unit";
  if (head == "Int32") return "int32";
  if (head == "Int64") return "int64";
  if (head == "Nativeint") return "nativeint";
  return nullptr;
}
// Family relink priority: -1 = not a participant; 0 = predef primitive;
// 1 = stdlib abbreviation; 2 = functor-instance abbreviation (`HW.key`).
// Probed rule (ocamlc): on contact the LOWER-priority node adopts the higher
// (an int64-typed value used with `HW.mem` displays HW.key); equal priorities
// leave both (first-contact wins in the var's slot).
static int family_prio(const Type* t) {
  if (t->stamp) return -1;
  if (t->functor_abbrev) return 2;
  if (family_abbr_of(t->path)) return 1;
  if (known_prim(t->path)) return 0;
  return -1;
}

// Occurs-check (a var must not appear in the type it's unified with) plus the
// level-lowering that keeps generalization sound: every var reachable from t has
// its level capped at the bound var's level.
void Engine::occurs_and_lower(const TypePtr& var, const TypePtr& t0) {
  // Composite nodes are visited-guarded so a cyclic/heavily-shared row (a
  // recursive `[> `A of 'a] as 'a` or a DAG reached via thousands of paths)
  // is walked once, not looped/re-exploded.  Level-lowering is monotonic, so a
  // skipped re-visit is a no-op, not a lost cap.
  std::unordered_set<Type*> seen;
  std::function<void(const TypePtr&, bool)> go = [&](const TypePtr& x0, bool under_row) {
    TypePtr t = repr(x0);
    switch (t->kind) {
      case Type::Kind::Var:
        // An occurrence is legal when the path passes THROUGH a row (variant/
        // object) node: OCaml permits `let rec r = fun () -> `A r` (the cycle
        // goes through `[> `A of ..]`) without -rectypes.  The var then links
        // to a type containing itself; every walk is cycle-guarded (Way 1).
        if (t == var) {
          if (under_row) break;
          throw TypeError("occurs check: recursive type");
        }
        if (t->level > var->level) { note(t); t->level = var->level; }
        break;
      case Type::Kind::Arrow:
        if (!seen.insert(t.get()).second) break;
        go(t->dom, under_row);
        go(t->cod, under_row);
        break;
      case Type::Kind::Variant:
        // Cap the row's tail level too (keeps a row unified into a var weak).
        if (t->level != GENERIC_LEVEL && t->level > var->level) { note(t); t->level = var->level; }
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a, true);
        break;
      case Type::Kind::Object:
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a, true);
        break;
      case Type::Kind::Constr:
        // A structure captured by a var from an OUTER (older) binding escapes
        // the current item: finalize its family heads so a later contact can't
        // relink the now-stored path (ephetest3's `hw` -- its weak 'a takes
        // fill_hw's SW.data, which must survive the HW.key contacts; the rec
        // binding's own display uses the fun's arrow, so this doesn't hide
        // the param's adopted path).
        if (var->level < level && family_prio(t.get()) >= 0) {
          note(t);
          t->level = GENERIC_LEVEL;
        }
        [[fallthrough]];
      case Type::Kind::Tuple:
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a, under_row);
        break;
      case Type::Kind::Link:
      case Type::Kind::Any:
        break;  // repr already resolved / Any has no vars
    }
  };
  go(t0, false);
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
    // Package constrs (`(module X.S)`) also compare by last component: two
    // spellings of one modtype (X.Y.S via `module Y = X.Y`) must stay
    // compatible so their `with type` args unify (pr6954 solves 'at = unit
    // through exactly this).  Constr unification never LINKS the nodes, so
    // each occurrence keeps its own written path for display.
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
    // Primitive/abbreviation FAMILIES: a primitive (`lazy_t`, `string`,
    // `int64` -- what constructions and literals carry), a stdlib abbreviation
    // of it (`Lazy.t`, `String.t`, `Int64.t`), or a functor-instance
    // abbreviation (`HW.key` -- marked at from_cmi).  Compatible pairs unify,
    // and -- matching ocamlc's unify3 link of the expanded heads -- the
    // LOWER-priority node adopts the higher one, so a value flowing into an
    // abbreviation-typed context DURING inference displays the abbreviation
    // (hamming's Lazy.t, qsort's String.t, ephetest3's HW.key), while a
    // FINALIZED (GENERIC-stamped) node never relinks (`let l = lazy 1` /
    // `let z = 1L` keep their paths; instantiate's fresh head copies protect
    // the lazy/string slice the same way).  A priority TIE between two functor
    // abbreviations (HW.key vs SW.data -- same underlying type in a well-typed
    // program) links the FIRST argument to the second (ocamlc's unify3
    // link_type t1' t2'), so both display slots converge on one node.  Stamped
    // (local, possibly shadowing) types are excluded; prim-vs-prim and
    // abbr-vs-abbr of the SAME family pair only via a real path mismatch below.
    if (last(a->path) != last(b->path) &&
        a->args.size() == b->args.size()) {
      int fa = family_prio(a.get()), fb = family_prio(b.get());
      bool differ = fa >= 0 && fb >= 0 && fa != fb &&
          // prim/stdlib-abbr pairs must agree on WHICH primitive; a functor
          // abbreviation's expansion is unknown, so priority 2 pairs with any.
          (fa == 2 || fb == 2 ||
           ((fa == 0 ? a->path : std::string(family_abbr_of(a->path))) ==
            (fb == 0 ? b->path : std::string(family_abbr_of(b->path)))));
      bool tie = (fa == 2 && fb == 2) ||
                 (fa == 1 && fb == 1 &&
                  std::string(family_abbr_of(a->path)) == family_abbr_of(b->path));
      if (differ || tie) {
        const TypePtr& lo = tie ? a : (fa < fb ? a : b);
        const TypePtr& hi = tie ? b : (fa < fb ? b : a);
        for (size_t i = 0; i < lo->args.size(); ++i)
          unify(lo->args[i], hi->args[i]);
        if (lo->level != GENERIC_LEVEL) {  // finalized nodes keep their path
          note(lo);
          lo->kind = Type::Kind::Link;
          lo->link = hi;
        }
        return;
      }
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
    // Inherited row types (`[< int u]`) define the allowed-set bound, so any tags
    // in this merge are just marking PRESENCE, not widening the row: keep labels
    // empty and route every tag to `present`.  (No inherited: normal tag union.)
    std::vector<TypePtr> inh = a->inherited;
    for (auto& it : b->inherited) inh.push_back(it);
    std::vector<std::string> present = a->present;
    auto add_present = [&](const std::string& p) {
      for (auto& x : present) if (x == p) return;
      present.push_back(p);
    };
    for (auto& p : b->present) add_present(p);
    if (!inh.empty()) {
      for (auto& t : tags) add_present(t);
      tags.clear(); ats.clear(); has.clear();
    }
    // An open CONSTRUCTED row `[>` (its tags are by construction PRESENT)
    // merging with an upper bound `[<`: record the constructed tags as present;
    // and when present covers the whole allowed set the row is EXACT (lower =
    // upper -> ocamlc prints `[ .. ]`, e.g. matching `` `A x `` against a
    // `[> `A of ..]` value closes the row).
    if (inh.empty() && a->variant_kind != 2 && b->variant_kind != 2 &&
        (a->variant_kind == 0) != (b->variant_kind == 0)) {
      const TypePtr& lo = a->variant_kind == 0 ? a : b;
      for (auto& tg : lo->labels) add_present(tg);
      if (present.size() == tags.size()) { vk = 2; present.clear(); }
    }
    TypePtr m = variant_type(tags, ats, has, vk);
    m->inherited = std::move(inh);
    m->present = std::move(present);
    // Carry an abbreviation stamp (`'a lambda`) through the merge -- but only
    // when the merged tag set is still the abbreviation's (a grown set is no
    // longer that abbreviation; sets only grow in a merge, so size suffices).
    if (!a->abbrev.empty() && m->labels.size() == a->labels.size()) {
      m->abbrev = a->abbrev; m->abbrev_args = a->abbrev_args;
    } else if (!b->abbrev.empty() && m->labels.size() == b->labels.size()) {
      m->abbrev = b->abbrev; m->abbrev_args = b->abbrev_args;
    }
    // A composite's level is the min of its parts: merging a generic row with a
    // weak one yields a weak row (value restriction wins); two generics stay
    // generic.  (variant_type stamped the engine level; override with the merge.)
    m->level = std::min(a->level, b->level);
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
  // `memo` dedups shared-DAG subtrees to ONE copy (a heavily-shared arrow-DAG
  // reached via thousands of paths must not be re-copied per path -- that is the
  // exponential blow-up that crashed the earlier level-aware attempts).
  // `on_stack` makes a cyclic row (recursive `[> `A of 'a] as 'a`) terminate:
  // a back-edge shares the original node instead of looping.  Share-unchanged
  // (return the original when no child changed) is preserved -- it is what keeps
  // a monomorphic function-param row a single node so its tags accumulate.
  std::unordered_map<Type*, TypePtr> memo;
  std::unordered_set<Type*> on_stack;
  // A GENERIC variant row is fresh-copied at each use so value-restriction at one
  // use can't weaken the shared scheme -- but ONLY when the row is small and
  // acyclic (bounded DFS).  A giant shared/cyclic row (mixin's object+variant DAG)
  // fails the bound and is shared unchanged: no fresh copy, no display change, no
  // blow-up.  This confines weak-`as` to simple rows like `bar`'s `[< `Test ]`.
  std::function<bool(const TypePtr&, std::unordered_set<Type*>&, int&)> fits =
      [&](const TypePtr& x0, std::unordered_set<Type*>& stk, int& budget) -> bool {
    TypePtr t = repr(x0);
    if (--budget < 0) return false;
    if (t->kind == Type::Kind::Var || t->kind == Type::Kind::Any) return true;
    if (!stk.insert(t.get()).second) return false;  // back-edge: cyclic
    bool ok = true;
    if (t->kind == Type::Kind::Arrow) ok = fits(t->dom, stk, budget) && fits(t->cod, stk, budget);
    else for (auto& a : t->args) { if (!fits(a, stk, budget)) { ok = false; break; } }
    stk.erase(t.get());
    return ok;
  };
  auto small_acyclic = [&](const TypePtr& t) {
    std::unordered_set<Type*> stk; int budget = 64; return fits(t, stk, budget);
  };
  // A small CYCLIC generic row region (e.g. `let rec r = fun () -> `A r`'s
  // 'a = unit -> [> `A of 'a]): the whole cycle must be copied per use so a
  // later pattern match can CLOSE the instance's row without touching the
  // scheme (ocamlc: `let (`A x) = r ()` gives x an exact `[ `A of 'a ]`).
  // Guards: region bounded (<=64 composite nodes -- mixin's giant DAG shares as
  // before), contains a back-edge, and every variant/object in it is GENERIC
  // (a non-generic row must stay shared so its tags keep accumulating).
  auto cyclic_region_ok = [&](const TypePtr& root) -> bool {
    std::unordered_set<Type*> seen;
    bool back_edge = false, ok = true;
    std::function<void(const TypePtr&, std::unordered_set<Type*>&)> go =
        [&](const TypePtr& x0, std::unordered_set<Type*>& stk) {
      if (!ok || seen.size() > 64) { ok = ok && seen.size() <= 64; return; }
      TypePtr t = repr(x0);
      if (t->kind == Type::Kind::Var || t->kind == Type::Kind::Any) return;
      if (stk.count(t.get())) { back_edge = true; return; }
      if (!seen.insert(t.get()).second) return;
      if ((t->kind == Type::Kind::Variant || t->kind == Type::Kind::Object) &&
          t->level != GENERIC_LEVEL) { ok = false; return; }
      stk.insert(t.get());
      if (t->kind == Type::Kind::Arrow) { go(t->dom, stk); go(t->cod, stk); }
      else for (auto& a : t->args) go(a, stk);
      stk.erase(t.get());
    };
    std::unordered_set<Type*> stk;
    go(root, stk);
    return ok && back_edge && seen.size() <= 64;
  };
  // Cycle-preserving copy: pre-register each composite's fresh shell in `memo`
  // BEFORE recursing, so a back-edge resolves to the in-progress copy (the
  // instance gets its own cycle, isolated from the scheme's).
  std::function<TypePtr(const TypePtr&)> ccopy = [&](const TypePtr& t0) -> TypePtr {
    TypePtr t = repr(t0);
    auto mit = memo.find(t.get());
    if (mit != memo.end()) return mit->second;
    switch (t->kind) {
      case Type::Kind::Var: {
        if (t->level == GENERIC_LEVEL) {
          auto it = mapping.find(t.get());
          TypePtr fv = it != mapping.end() ? it->second : (mapping[t.get()] = fresh_var());
          memo[t.get()] = fv;
          return fv;
        }
        memo[t.get()] = t;
        return t;
      }
      case Type::Kind::Arrow: {
        TypePtr r = arrow(t->dom, t->cod, t->arrow_label, t->arrow_lbl);
        memo[t.get()] = r;
        r->dom = ccopy(t->dom);
        r->cod = ccopy(t->cod);
        return r;
      }
      case Type::Kind::Tuple:
      case Type::Kind::Constr:
      case Type::Kind::Object:
      case Type::Kind::Variant: {
        TypePtr r;
        if (t->kind == Type::Kind::Tuple) r = tuple(t->args);
        else if (t->kind == Type::Kind::Constr) { r = constr(t->path, t->args, t->stamp); r->functor_abbrev = t->functor_abbrev; r->abbrev = t->abbrev; r->labels = t->labels; }
        else if (t->kind == Type::Kind::Object) r = object_type(t->labels, t->args);
        else {
          r = variant_type(t->labels, t->args, t->tag_has_arg, t->variant_kind);
          r->present = t->present;
          r->inherited = t->inherited;
          r->abbrev = t->abbrev;
          r->abbrev_args = t->abbrev_args;
          r->level = level;  // fresh weak instance (the scheme's row was generic)
        }
        memo[t.get()] = r;
        for (auto& a : r->args) a = ccopy(a);
        if (r->kind == Type::Kind::Variant)
          for (auto& aa : r->abbrev_args) aa = ccopy(aa);
        return r;
      }
      case Type::Kind::Link:
      case Type::Kind::Any:
        memo[t.get()] = t;
        return t;
    }
    return t;
  };
  std::function<TypePtr(const TypePtr&)> copy = [&](const TypePtr& t0) -> TypePtr {
    TypePtr t = repr(t0);
    auto mit = memo.find(t.get());
    if (mit != memo.end()) return mit->second;
    switch (t->kind) {
      case Type::Kind::Var:
        if (t->level == GENERIC_LEVEL) {
          auto it = mapping.find(t.get());
          TypePtr fv = it != mapping.end() ? it->second : (mapping[t.get()] = fresh_var());
          memo[t.get()] = fv;
          return fv;
        }
        memo[t.get()] = t;
        return t;  // free (non-generic) var: shared, not copied
      case Type::Kind::Arrow: {
        if (!on_stack.insert(t.get()).second) return t;  // cyclic back-edge: share
        TypePtr d = copy(t->dom), c = copy(t->cod);
        on_stack.erase(t.get());
        // Compare against the REPR of each child: a link-wrapped child (a var
        // already resolved to a concrete type) resolves to the same node when
        // truly unchanged -- comparing to the wrapper pointer spuriously marked
        // it "changed" and re-copied shareable structure per use.
        TypePtr r = (d == repr(t->dom) && c == repr(t->cod))
                      ? t  // no generic inside: share
                      : arrow(std::move(d), std::move(c), t->arrow_label, t->arrow_lbl);
        memo[t.get()] = r;
        return r;
      }
      case Type::Kind::Tuple:
      case Type::Kind::Constr:
      case Type::Kind::Object:
      case Type::Kind::Variant: {
        if (!on_stack.insert(t.get()).second) return t;  // cyclic back-edge: share
        std::vector<TypePtr> as;
        bool changed = false;
        as.reserve(t->args.size());
        for (auto& a : t->args) {  // repr-compare: see the Arrow case
          as.push_back(copy(a));
          if (as.back() != repr(a)) changed = true;
        }
        on_stack.erase(t.get());
        // A generic variant row gets a FRESH weak node per use (see `fits`), so a
        // value-restricted use (`bar = wrap ()`) can weaken its own copy without
        // touching the scheme.  Others share-unchanged when nothing changed.
        // A SCHEME family head (cmi/functor value) also copies per use: the
        // live instance can adopt abbreviations (fill_hw's SW.merge result
        // relinking to HW.key) while the scheme node never changes.  Plain
        // annotation-finalized heads (GENERIC, no scheme_head) share -- copying
        // them cascaded `changed` through parents and split shared rows
        // (ref_spec's `as 'a`).
        bool weak_copy = t->kind == Type::Kind::Variant &&
                         t->level == GENERIC_LEVEL && small_acyclic(t);
        bool sch_head = t->kind == Type::Kind::Constr && t->scheme_head &&
                        t->level == GENERIC_LEVEL;
        TypePtr r;
        if (!changed && !weak_copy && !sch_head) r = t;  // monomorphic composite: share the node
        else if (t->kind == Type::Kind::Tuple) r = tuple(std::move(as));
        else if (t->kind == Type::Kind::Constr) { r = constr(t->path, std::move(as), t->stamp); r->functor_abbrev = t->functor_abbrev; r->abbrev = t->abbrev; r->labels = t->labels; }
        else if (t->kind == Type::Kind::Object) r = object_type(t->labels, std::move(as));
        else {
          r = variant_type(t->labels, std::move(as), t->tag_has_arg, t->variant_kind);
          r->present = t->present;
          r->inherited = t->inherited;  // ground types (int/t): share unexpanded
          r->abbrev = t->abbrev;
          r->level = weak_copy ? level : t->level;  // fresh weak, else preserve
          // Register BEFORE copying abbrev_args: an abbreviation arg can reach
          // back into this very row (`'a lambda` with 'a tied to the row), and
          // t is already off the on_stack here -- without the memo entry the
          // re-entry recurses forever (mixin stack-overflow).
          memo[t.get()] = r;
          for (auto& aa : t->abbrev_args) r->abbrev_args.push_back(copy(aa));
          return r;
        }
        memo[t.get()] = r;
        return r;
      }
      case Type::Kind::Link:
        return copy(t);  // repr already resolved; unreachable
      case Type::Kind::Any:
        memo[t.get()] = t;
        return t;  // dynamic: shared, not copied
    }
    return t;
  };
  // A scheme whose reachable region is small, CYCLIC, and all-generic-rows is
  // instantiated by the cycle-preserving copy, so each use gets its own cycle
  // (a pattern match can then close the instance's row without mutating the
  // scheme).  Everything else takes the share-unchanged copy.
  if (cyclic_region_ok(scheme)) return ccopy(scheme);
  return copy(scheme);
}

void Engine::finalize_family_heads(const TypePtr& t0, bool scheme) {
  std::unordered_set<Type*> seen;
  std::function<void(const TypePtr&)> go = [&](const TypePtr& x0) {
    TypePtr t = repr(x0);
    switch (t->kind) {
      case Type::Kind::Arrow:
        if (!seen.insert(t.get()).second) break;
        go(t->dom);
        go(t->cod);
        break;
      case Type::Kind::Constr:
        if (family_prio(t.get()) >= 0) {
          t->level = GENERIC_LEVEL;
          if (scheme) t->scheme_head = true;
        }
        [[fallthrough]];
      case Type::Kind::Tuple:
      case Type::Kind::Variant:
      case Type::Kind::Object:
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a);
        break;
      case Type::Kind::Var:
      case Type::Kind::Link:
      case Type::Kind::Any:
        break;
    }
  };
  go(t0);
}

void Engine::generalize(const TypePtr& t0) {
  // Visited-guarded (see occurs_and_lower): promoting a var to GENERIC is
  // idempotent, so guarding composite re-visits is pure cycle/DAG safety with
  // no behaviour change.
  std::unordered_set<Type*> seen;
  std::function<void(const TypePtr&)> go = [&](const TypePtr& x0) {
    TypePtr t = repr(x0);
    switch (t->kind) {
      case Type::Kind::Var:
        if (t->level > level) t->level = GENERIC_LEVEL;
        break;
      case Type::Kind::Arrow:
        if (!seen.insert(t.get()).second) break;
        go(t->dom);
        go(t->cod);
        break;
      case Type::Kind::Variant:
        // Promote the row's tail level too, so a generalised row prints plain.
        if (t->level > level) t->level = GENERIC_LEVEL;
        [[fallthrough]];
      case Type::Kind::Tuple:
      case Type::Kind::Object:
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a);
        break;
      case Type::Kind::Constr:
        // A FINALIZED `lazy_t` head is stamped GENERIC so instantiate
        // fresh-copies it per use: a later `Lazy.force l` / `f l` then relinks
        // the COPY, not the binding's displayed node (ocamlc: `let l = lazy 1`
        // stays `lazy_t` after later uses; only a same-rec-group flow -- where
        // the node is still unstamped and shared -- adopts `Lazy.t`, hamming).
        // (family-head finalization moved to finalize_family_heads -- top-level only)
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a);
        break;
      case Type::Kind::Link:
      case Type::Kind::Any:
        break;
    }
  };
  go(t0);
}

// Lower a NON-generalized binding's vars (value restriction) to the current
// level instead of leaving them at the higher binding level.  Otherwise a var
// created at the inner level but tied to an outer mutable cell (`let r = ref []`)
// stays trapped above the current level, and a LATER sibling let's generalize()
// wrongly makes it generic -- breaking `let h e = r := e :: !r in (h, !r)` whose
// element type must stay shared (`('a -> unit) * 'a list`, not `* 'b list`).
void Engine::demote(const TypePtr& t0) {
  // Visited-guarded (see occurs_and_lower): level-lowering is monotonic, so a
  // skipped re-visit is a no-op -- pure cycle/DAG safety.
  std::unordered_set<Type*> seen;
  std::function<void(const TypePtr&)> go = [&](const TypePtr& x0) {
    TypePtr t = repr(x0);
    switch (t->kind) {
      case Type::Kind::Var:
        if (t->level != GENERIC_LEVEL && t->level > level) { note(t); t->level = level; }
        break;
      case Type::Kind::Arrow:
        if (!seen.insert(t.get()).second) break;
        go(t->dom);
        go(t->cod);
        break;
      case Type::Kind::Variant:
        // Lower the row's tail level too (value restriction keeps it weak).
        if (t->level != GENERIC_LEVEL && t->level > level) { note(t); t->level = level; }
        [[fallthrough]];
      case Type::Kind::Tuple:
      case Type::Kind::Object:
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a);
        break;
      case Type::Kind::Constr:
        // Weak bindings are FINALIZED too: stamp the lazy head (see generalize)
        // so a later use can't relink the binding's displayed node.
        // (family-head finalization moved to finalize_family_heads -- top-level only)
        if (!seen.insert(t.get()).second) break;
        for (auto& a : t->args) go(a);
        break;
      case Type::Kind::Link:
      case Type::Kind::Any:
        break;
    }
  };
  go(t0);
}

namespace {
// Count how many times each polymorphic-variant/object row node is referenced,
// so the printer can name a SHARED one (appears 2+ times) or a CYCLIC one
// (reached 2+ via the cycle) with `as 'a`.  Variant/Object are visited-guarded
// (cycle-safe); the ref count is taken before the guard so a node reached via two
// paths still counts twice.
void count_refs(const TypePtr& t0, std::unordered_map<Type*, int>& rc,
                std::unordered_set<Type*>& seen,
                std::unordered_set<Type*>& stk) {
  TypePtr t = Engine::repr(t0);
  switch (t->kind) {
    case Type::Kind::Variant:
    case Type::Kind::Object:
      rc[t.get()]++;
      if (!seen.insert(t.get()).second) return;
      for (auto& a : t->args) count_refs(a, rc, seen, stk);
      // An abbrev row prints its abbrev_args INSTEAD of its tag args, so they
      // count as printed occurrences too (and can carry cycles).
      for (auto& aa : t->abbrev_args) count_refs(aa, rc, seen, stk);
      break;
    case Type::Kind::Arrow:
      // Guarded by the DFS STACK only: a CYCLE edge back into an arrow must not
      // inflate the inner row's refcount (the cyclic arrow gets the `as` name
      // and back-refs on re-entry, so the row prints once -- ocamlc's
      // `unit -> [> `A of 'a ] as 'a`).  A DAG-shared arrow reached again LATER
      // however PRINTS again (arrows are only named when cyclic), so its row
      // must be re-counted -- a full seen-guard would lose the row's `as 'a`
      // (recursive_module_init's `stub:('a -> int) -> .. -> ('a -> int)`).
      if (!stk.insert(t.get()).second) return;
      count_refs(t->dom, rc, seen, stk); count_refs(t->cod, rc, seen, stk);
      stk.erase(t.get());
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
      // Same stack-guard as Arrow: a cycle edge back into a constr/tuple gets
      // the `as` name at that node (show_rec), so the row beneath prints once.
      if (!stk.insert(t.get()).second) return;
      for (auto& a : t->args) count_refs(a, rc, seen, stk);
      stk.erase(t.get());
      break;
    default: break;
  }
}

// Nodes that are ON a cycle entered from the root walk (re-entered while still
// on the DFS stack).  A cyclic ARROW needs `as` naming (`unit -> [> `A of 'a ]
// as 'a` -- ocamlc binds 'a at the arrow when the cycle closes there); a merely
// DAG-shared arrow must NOT be named (ocamlc reprints it).  Variants/objects
// keep their refcount-based naming (sharing OR cycles both name them).
void find_cycles(const TypePtr& t0, std::unordered_set<Type*>& on_stack,
                 std::unordered_set<Type*>& done, std::unordered_set<Type*>& cyc) {
  TypePtr t = Engine::repr(t0);
  switch (t->kind) {
    case Type::Kind::Arrow:
    case Type::Kind::Tuple:
    case Type::Kind::Constr:
    case Type::Kind::Variant:
    case Type::Kind::Object: {
      if (on_stack.count(t.get())) { cyc.insert(t.get()); return; }
      if (done.count(t.get())) return;
      on_stack.insert(t.get());
      if (t->kind == Type::Kind::Arrow) {
        find_cycles(t->dom, on_stack, done, cyc);
        find_cycles(t->cod, on_stack, done, cyc);
      } else {
        for (auto& a : t->args) find_cycles(a, on_stack, done, cyc);
        for (auto& ih : t->inherited) find_cycles(ih, on_stack, done, cyc);
        for (auto& aa : t->abbrev_args) find_cycles(aa, on_stack, done, cyc);
      }
      on_stack.erase(t.get());
      done.insert(t.get());
      break;
    }
    default: break;
  }
}

// Type-variable display name for the i-th distinct printed variable, matching
// ocamlc's scheme: 'a..'z, then 'a1..'z1, 'a2..  (letter = i mod 26, numeric
// suffix = i / 26, empty when 0).  Using a single letter (i % 26) collides past
// 'z, which under first-appearance normalisation merges distinct vars.
static std::string tvar_letter(size_t i) {
  std::string s(1, (char)('a' + i % 26));
  if (i >= 26) s += std::to_string(i / 26);
  return s;
}

// `cp` = context precedence required by the parent position: 0 top (arrow ok),
// 1 arrow-domain (tuple ok, arrow needs parens), 2 atom (both need parens).
// A row node referenced 2+ times (`rc`) -- shared across the type OR recursive --
// is named `as 'aN` at its first full print and back-referenced `'aN` afterwards,
// matching ocamlc.  `printed` marks nodes whose print has begun (so a re-entry,
// cyclic or a later occurrence, emits the back-reference and can't loop).
// Heads of dotted constr paths in the type currently being shown (set by
// show()): a named first-class-module param `(module M : S)` (name stored in
// the constr's abbrev) prints its name only when the shown type actually
// depends on M -- ocamlc's modular-explicits display rule.
thread_local const std::set<std::string>* g_pkg_dep_heads = nullptr;

void collect_dotted_heads(const TypePtr& t0, std::set<std::string>& heads,
                          std::unordered_set<Type*>& seen) {
  TypePtr t = Engine::repr(t0);
  if (!t || !seen.insert(t.get()).second) return;
  if (t->kind == Type::Kind::Constr && t->path.rfind("(module ", 0) != 0) {
    size_t dot = t->path.find('.');
    if (dot != std::string::npos) heads.insert(t->path.substr(0, dot));
  }
  if (t->dom) collect_dotted_heads(t->dom, heads, seen);
  if (t->cod) collect_dotted_heads(t->cod, heads, seen);
  for (auto& a : t->args) collect_dotted_heads(a, heads, seen);
  for (auto& a : t->abbrev_args) collect_dotted_heads(a, heads, seen);
  for (auto& a : t->inherited) collect_dotted_heads(a, heads, seen);
}

void show_rec(const TypePtr& t0, std::string& out, int cp,
              std::unordered_map<Type*, std::string>& names,
              std::unordered_set<Type*>& printed,
              const std::unordered_map<Type*, int>& rc,
              const std::unordered_set<Type*>& cyc) {
  TypePtr t = Engine::repr(t0);
  // A named row (or cyclic arrow/constr/tuple) already being/having-been
  // printed: emit the back-reference.  Non-row nodes only enter `printed` when
  // cyclic-named below, so a merely DAG-shared one still reprints.
  if (t->kind != Type::Kind::Var && printed.count(t.get())) {
    out += names[t.get()];
    return;
  }
  switch (t->kind) {
    case Type::Kind::Var: {
      auto it = names.find(t.get());
      if (it == names.end()) {
        std::string n = "'" + tvar_letter(names.size());
        if (t->level == GENERIC_LEVEL) n += "";  // generic: plain 'a
        else n = "'_" + std::to_string(t->id);   // weak/free: '_N
        it = names.emplace(t.get(), n).first;
      }
      out += it->second;
      break;
    }
    case Type::Kind::Arrow: {
      // A CYCLIC arrow (the recursion closes here, e.g. `let rec r = fun () ->
      // `A r` where 'a = unit -> [> `A of 'a]) is named `as 'aN` at its first
      // print and back-referenced afterwards, like a shared row.  `as` binds
      // loosest, so at top level no parens: `unit -> [> `A of 'a ] as 'a`.
      bool named = cyc.count(t.get());
      if (named) {
        if (!names.count(t.get()))
          names[t.get()] = "'" + tvar_letter(names.size());
        printed.insert(t.get());
      }
      std::string body;
      if (t->arrow_label == 1) { body += t->arrow_lbl + ":";        // ~lbl:
        show_rec(t->dom, body, 1, names, printed, rc, cyc);
      } else if (t->arrow_label == 2) {  // ?lbl: -- internal type is `T option`,
        body += "?" + t->arrow_lbl + ":";  // but ocamlc displays the bare T
        TypePtr d = Engine::repr(t->dom);
        if (d->kind == Type::Kind::Constr && d->args.size() == 1 &&
            (d->path == "option" || d->path == "Stdlib.option"))
          show_rec(d->args[0], body, 1, names, printed, rc, cyc);
        else
          show_rec(t->dom, body, 1, names, printed, rc, cyc);
      } else {
        show_rec(t->dom, body, 1, names, printed, rc, cyc);   // domain: a tuple is fine unparen'd
      }
      body += " -> ";
      show_rec(t->cod, body, 0, names, printed, rc, cyc);   // -> is right-assoc: codomain stays top
      if (named) body += " as " + names[t.get()];
      if (cp > 0) out += "(" + body + ")";
      else out += body;
      break;
    }
    case Type::Kind::Tuple: {
      bool named = cyc.count(t.get()) != 0;  // cyclic: `(.. * ..) as 'aN`
      if (named) {
        if (!names.count(t.get()))
          names[t.get()] = "'" + tvar_letter(names.size());
        printed.insert(t.get());
      }
      std::string body;
      for (size_t i = 0; i < t->args.size(); ++i) {
        if (i) body += " * ";
        show_rec(t->args[i], body, 2, names, printed, rc, cyc);  // components bind tighter than *
      }
      if (named) body += " as " + names[t.get()];
      if (cp > (named ? 0 : 1)) out += "(" + body + ")";
      else out += body;
      break;
    }
    case Type::Kind::Constr: {
      // A cyclic constr is `as`-named like a cyclic arrow (`< bark : 'a ->
      // unit > t as 'a` -- ocamlc binds 'a at the constr the cycle re-enters).
      bool named = cyc.count(t.get()) != 0;
      if (named) {
        if (!names.count(t.get()))
          names[t.get()] = "'" + tvar_letter(names.size());
        printed.insert(t.get());
      }
      // A package constr's args are its `with type` constraints (rendered
      // after the path below), not type parameters.
      bool pkg = t->path.rfind("(module ", 0) == 0;
      std::string body;
      if (pkg) {
      } else if (t->args.size() == 1) { show_rec(t->args[0], body, 2, names, printed, rc, cyc); body += " "; }
      else if (t->args.size() > 1) {
        body += "(";
        for (size_t i = 0; i < t->args.size(); ++i) {
          if (i) body += ", ";
          show_rec(t->args[i], body, 0, names, printed, rc, cyc);
        }
        body += ") ";
      }
      std::string path = t->path;
      // Stdlib is opened by default, so its types print unqualified
      // (Stdlib.out_channel -> out_channel, Stdlib.Gc.stat -> Gc.stat).
      if (path.rfind("Stdlib.", 0) == 0) path = path.substr(7);
      // The printf format type is normalised to format6; a 3-parameter one prints
      // as its `format` abbreviation, matching ocamlc.
      if (path == "format6" && t->args.size() == 3) path = "format";
      // A 4-parameter format6 is the `format4` abbreviation (its 3 middle params
      // coincide); we only ever reach 4 args from a `format4` source annotation.
      if (path == "format6" && t->args.size() == 4) path = "format4";
      // Lazy.t is the public abbreviation of CamlinternalLazy.t; print the former.
      if (path == "CamlinternalLazy.t") path = "Lazy.t";
      // A package constr carrying its module name (`(module M : S)`, name in
      // abbrev) prints the name only when the shown type depends on M.
      if (!t->abbrev.empty() && pkg && g_pkg_dep_heads &&
          g_pkg_dep_heads->count(t->abbrev))
        path = "(module " + t->abbrev + " : " + path.substr(8);
      // Package `with type` constraints (labels/args pairs).
      if (pkg && !t->labels.empty() && t->labels.size() == t->args.size()) {
        path = path.substr(0, path.size() - 1);
        for (size_t i = 0; i < t->labels.size(); ++i) {
          path += (i ? " and type " : " with type ") + t->labels[i] + " = ";
          show_rec(t->args[i], path, 0, names, printed, rc, cyc);
        }
        path += ")";
      }
      body += path;
      if (named) body += " as " + names[t.get()];
      if (named && cp > 0) out += "(" + body + ")";
      else out += body;
      break;
    }
    case Type::Kind::Object:
    case Type::Kind::Variant: {
      // A row referenced 2+ times (shared or recursive; an exact `[ .. ]` variant
      // has no row variable so is never named) is named `as 'aN`: assign the name
      // and mark `printed` BEFORE building the body, so a re-entry emits the
      // back-reference `'aN`.
      // An exact `[ .. ]` variant has no row variable, so it is never named.
      // A weak (non-generic) variant row carries a weak tail variable that ocamlc
      // names even at a single occurrence (`bar`'s `([< `Test ] as '_weak1)`);
      // Objects are not level-stamped, so only Variant weak-naming applies here.
      bool exact = t->kind == Type::Kind::Variant && t->variant_kind == 2;
      auto it = rc.find(t.get());
      bool shared = it != rc.end() && it->second >= 2 && !exact;
      bool weak = t->kind == Type::Kind::Variant && t->level != GENERIC_LEVEL && !exact;
      // A row ON A CYCLE is named even when exact (`[ `Abs of .. * 'a ] as 'a`
      // -- an exact row has no row variable, so sharing doesn't name it, but a
      // recursive one MUST be to terminate; ocamlc prints exactly this).
      bool multi = shared || weak || cyc.count(t.get()) != 0;
      if (multi) {
        if (!names.count(t.get()))
          names[t.get()] = "'" + tvar_letter(names.size());
        printed.insert(t.get());
      }
      std::string body;
      if (t->kind == Type::Kind::Object) {
        body = "< ";
        for (size_t i = 0; i < t->labels.size(); ++i) {
          if (i) body += "; ";
          body += t->labels[i] + " : ";
          show_rec(t->args[i], body, 0, names, printed, rc, cyc);
        }
        body += " >";
      } else if (!t->abbrev.empty() &&
                 [&] {  // a row that is its own abbreviation argument (the
                   // fixpoint `'a lambda as 'a`, e.g. free1 = fix free_lambda)
                   // prints UNFOLDED (ocamlc: `[ `Abs .. | `App .. ] as 'a`).
                   for (auto& aa : t->abbrev_args)
                     if (Engine::repr(aa).get() == t.get()) return false;
                   return true;
                 }()) {
        // A row expanded from an abbreviation whose tag set is intact prints
        // the NAME, like ocamlc: exact -> `'a lambda`; bounded -> `[< var ]`;
        // open -> `[> var ]`.
        std::string ab;
        if (t->abbrev_args.size() == 1) {
          show_rec(t->abbrev_args[0], ab, 2, names, printed, rc, cyc);
          ab += " ";
        } else if (t->abbrev_args.size() > 1) {
          ab += "(";
          for (size_t i = 0; i < t->abbrev_args.size(); ++i) {
            if (i) ab += ", ";
            show_rec(t->abbrev_args[i], ab, 0, names, printed, rc, cyc);
          }
          ab += ") ";
        }
        ab += t->abbrev;
        body = t->variant_kind == 2 ? ab
             : (t->variant_kind == 1 ? "[< " + ab + " ]" : "[> " + ab + " ]");
      } else {
        std::vector<size_t> ord(t->labels.size());  // ocamlc: tags alphabetical
        for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::sort(ord.begin(), ord.end(),
                  [&](size_t x, size_t y) { return t->labels[x] < t->labels[y]; });
        body = t->variant_kind == 1 ? "[< " : t->variant_kind == 2 ? "[ " : "[> ";
        bool wrote = false;
        for (auto& ih : t->inherited) {  // inherited row types: `[< int u | .. ]`
          if (wrote) body += " | ";
          show_rec(ih, body, 0, names, printed, rc, cyc);
          wrote = true;
        }
        for (size_t n = 0; n < ord.size(); ++n) {
          size_t i = ord[n];
          if (wrote) body += " | ";
          body += "`" + t->labels[i];
          if (i < t->tag_has_arg.size() && t->tag_has_arg[i]) {
            body += " of ";
            show_rec(t->args[i], body, 0, names, printed, rc, cyc);
          }
          wrote = true;
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
  { std::unordered_set<Type*> seen, stk; count_refs(t, rc, seen, stk); }
  std::unordered_set<Type*> cyc;          // cycle members (arrow `as` naming)
  { std::unordered_set<Type*> on_stack, done; find_cycles(t, on_stack, done, cyc); }
  std::set<std::string> heads;            // dotted-path heads (package naming)
  { std::unordered_set<Type*> seen; collect_dotted_heads(t, heads, seen); }
  g_pkg_dep_heads = &heads;
  std::string out;
  std::unordered_map<Type*, std::string> names;
  std::unordered_set<Type*> printed;
  show_rec(t, out, 0, names, printed, rc, cyc);
  g_pkg_dep_heads = nullptr;
  return out;
}

}  // namespace cppcaml::infer
