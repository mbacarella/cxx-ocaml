// Port of typing/ident.ml (TYPECHECKER.md).  An Ident.t is an immutable value;
// here a zone-allocated `Ident` reached through `Ident::t` (a const pointer),
// compared with same/equal, never by pointer.
#pragma once

#include <cstdint>
#include <string>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

namespace ident {

inline constexpr int lowest_scope = 0;
inline constexpr int highest_scope = 100'000'000;

// ident.ml `Unscoped`: a mutable cell (Udesc {name; stamp} | Ulink t).
struct Unscoped {
  enum class State : std::uint8_t { Udesc, Ulink };
  State state;
  std::string_view name;  // Udesc
  int stamp = 0;          // Udesc
  Unscoped* ulink = nullptr;  // Ulink

  struct Desc { std::string_view name; int stamp; };
  static Unscoped* create(std::string_view s);
  static Desc get_desc(const Unscoped* us);
  static std::string_view name_of(const Unscoped* us) { return get_desc(us).name; }
  static Unscoped* refresh(const Unscoped* us);
  static bool equal(const Unscoped* a, const Unscoped* b);
  static int stamp_of(const Unscoped* us) { return get_desc(us).stamp; }
  static bool same(const Unscoped* a, const Unscoped* b);
  static Unscoped* repr(Unscoped* us);
  // `change` = (us, old state); logged through Types' trail (Cuident).
  struct Change { Unscoped* us; State state; std::string_view name; int stamp; Unscoped* ulink; };
  static void undo_change(const Change& c);
  static void link(Unscoped* a, Unscoped* b);
};

}  // namespace ident

struct Ident {
  enum class Kind : std::uint8_t { Local, Scoped, Global, Predef, Unscoped };
  Kind kind;
  std::string_view name_;   // all but Unscoped
  int stamp_ = 0;           // Local / Scoped / Predef
  int scope_ = 0;           // Scoped
  ident::Unscoped* us = nullptr;  // Unscoped

  using t = const Ident*;

  static t create_scoped(int scope, std::string_view s);
  static t create_local(std::string_view s);
  static t of_unscoped(ident::Unscoped* u);
  static t create_predef(std::string_view s);
  static t create_persistent(std::string_view s);
  // Rebuild an ident read from a cmi (the stamp is the writer's).
  static t make_raw(Kind k, std::string_view name, int stamp, int scope,
                    ident::Unscoped* us);
};

namespace ident {

using t = Ident::t;

ident::Unscoped* find_unscoped(t id);
std::string_view name(t id);
t rename(t id);
std::string unique_name(t id);
std::string unique_toplevel_name(t id);
bool persistent(t id);
bool equal(t a, t b);
bool same(t a, t b);
int stamp(t id);
int compare_stamp(t a, t b);
int scope(t id);
void reinit();
bool global(t id);
bool is_predef(t id);
bool is_unscoped(t id);
int compare(t a, t b);
int hash(t id);
// Ident.print with Clflags.unique_ids off (the compiler's default).
std::string print(t id);

// The stamp counters (Local_store refs in ident.ml).
int& currentstamp();
int& predefstamp();

// ---- 'a Ident.tbl -------------------------------------------------------------
// ident.ml's balanced tree keyed by name, each node keeping the shadowed
// bindings of the same name in `previous`.  Persistent; nodes in the zone.
template <class A>
struct TblData {
  Ident::t ident;
  std::string_view name;
  int stamp;
  A data;
  const TblData* previous;
};

template <class A>
class Tbl {
 public:
  struct Node {
    const Node* l;
    const TblData<A>* d;
    const Node* r;
    int h;
  };
  struct NotFound {};

  Tbl() = default;
  explicit Tbl(const Node* n) : t_(n) {}
  bool is_empty() const { return t_ == nullptr; }
  bool same_as(const Tbl& o) const { return t_ == o.t_; }

  Tbl add(Ident::t id, const A& data) const { return Tbl(add_(id, data, t_)); }
  Tbl remove(Ident::t id) const { return Tbl(remove_(id, t_)); }

  // find_same: raises NotFound
  const A& find_same(Ident::t id) const {
    std::string_view n = name(id);
    for (const Node* x = t_; x;) {
      int c = n.compare(x->d->name);
      if (c == 0) {
        for (const TblData<A>* k = x->d; k; k = k->previous)
          if (same(id, k->ident)) return get_data(k);
        throw NotFound{};
      }
      x = c < 0 ? x->l : x->r;
    }
    throw NotFound{};
  }
  const A* find_same_opt(Ident::t id) const {
    try {
      return &find_same(id);
    } catch (const NotFound&) {
      return nullptr;
    }
  }
  // find_name: the most recent binding of the name
  std::pair<Ident::t, const A*> find_name(std::string_view n) const {
    for (const Node* x = t_; x;) {
      int c = n.compare(x->d->name);
      if (c == 0) return {x->d->ident, &get_data(x->d)};
      x = c < 0 ? x->l : x->r;
    }
    throw NotFound{};
  }
  // find_all: every binding of the name, most recent first
  std::vector<std::pair<Ident::t, const A*>> find_all(std::string_view n) const {
    std::vector<std::pair<Ident::t, const A*>> out;
    for (const Node* x = t_; x;) {
      int c = n.compare(x->d->name);
      if (c == 0) {
        for (const TblData<A>* k = x->d; k; k = k->previous)
          out.emplace_back(k->ident, &get_data(k));
        return out;
      }
      x = c < 0 ? x->l : x->r;
    }
    return out;
  }
  // fold_name f tbl acc: each name's most recent binding, in the order of
  // ident.ml's fold_aux (right subtree first, then the node, then left)
  template <class F>
  void fold_name(F&& f) const {
    fold_aux(t_, [&](const TblData<A>* k) { f(k->ident, get_data(k)); });
  }
  // fold_all: every binding, each name's chain oldest first
  template <class F>
  void fold_all(F&& f) const {
    fold_aux(t_, [&](const TblData<A>* k) { fold_data(f, k); });
  }
  // iter: in-order (left, node, right), most recent binding per name
  template <class F>
  void iter(F&& f) const { iter_(t_, f); }

 private:
  static const A& get_data(const TblData<A>* k) {
    if (stamp(k->ident) != k->stamp) throw std::logic_error("Ident.get_data");
    return k->data;
  }
  static int height(const Node* n) { return n ? n->h : 0; }
  static const Node* mknode(const Node* l, const TblData<A>* d, const Node* r) {
    int hl = height(l), hr = height(r);
    return make<Node>(l, d, r, hl >= hr ? hl + 1 : hr + 1);
  }
  static const Node* balance(const Node* l, const TblData<A>* d, const Node* r) {
    int hl = height(l), hr = height(r);
    if (hl > hr + 1) {
      if (height(l->l) >= height(l->r)) return mknode(l->l, l->d, mknode(l->r, d, r));
      const Node* lr = l->r;
      return mknode(mknode(l->l, l->d, lr->l), lr->d, mknode(lr->r, d, r));
    }
    if (hr > hl + 1) {
      if (height(r->r) >= height(r->l)) return mknode(mknode(l, d, r->l), r->d, r->r);
      const Node* rl = r->l;
      return mknode(mknode(l, d, rl->l), rl->d, mknode(rl->r, r->d, r->r));
    }
    return mknode(l, d, r);
  }
  static const Node* add_(Ident::t id, const A& data, const Node* t) {
    std::string_view n = name(id);
    if (!t)
      return make<Node>(nullptr, make<TblData<A>>(id, n, stamp(id), data, nullptr), nullptr, 1);
    int c = n.compare(t->d->name);
    if (c == 0)
      return make<Node>(t->l, make<TblData<A>>(id, n, stamp(id), data, t->d), t->r, t->h);
    if (c < 0) return balance(add_(id, data, t->l), t->d, t->r);
    return balance(t->l, t->d, add_(id, data, t->r));
  }
  static const Node* min_binding(const Node* t) {
    while (t->l) t = t->l;
    return t;
  }
  static const Node* remove_min_binding(const Node* t) {
    if (!t->l) return t->r;
    return balance(remove_min_binding(t->l), t->d, t->r);
  }
  static const Node* merge(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    return balance(t1, min_binding(t2)->d, remove_min_binding(t2));
  }
  static const Node* remove_(Ident::t id, const Node* m) {
    if (!m) return nullptr;
    int c = name(id).compare(m->d->name);
    if (c == 0) {
      if (!m->d->previous) return merge(m->l, m->r);
      return make<Node>(m->l, m->d->previous, m->r, m->h);
    }
    if (c < 0) {
      const Node* ll = remove_(id, m->l);
      return ll == m->l ? m : balance(ll, m->d, m->r);
    }
    const Node* rr = remove_(id, m->r);
    return rr == m->r ? m : balance(m->l, m->d, rr);
  }
  // fold_aux f stack accu t: node's RIGHT subtree is folded first... in the
  // ident.ml code `fold_aux f (l :: stack) (f k accu) r`: the node, then its
  // right subtree, then the stacked left subtrees.
  template <class G>
  static void fold_aux(const Node* t, G&& g) {
    std::vector<const Node*> stack;
    for (;;) {
      if (!t) {
        if (stack.empty()) return;
        t = stack.back();
        stack.pop_back();
        continue;
      }
      stack.push_back(t->l);
      g(t->d);
      t = t->r;
    }
  }
  template <class F>
  static void fold_data(F& f, const TblData<A>* d) {
    // fold_data f d accu = f k.ident (data) (fold_data f k.previous accu):
    // the oldest binding is applied first
    std::vector<const TblData<A>*> chain;
    for (; d; d = d->previous) chain.push_back(d);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) f((*it)->ident, get_data(*it));
  }
  template <class F>
  static void iter_(const Node* t, F& f) {
    if (!t) return;
    iter_(t->l, f);
    f(t->d->ident, get_data(t->d));
    iter_(t->r, f);
  }
  const Node* t_ = nullptr;
};

}  // namespace ident

}  // namespace cppcaml::typing
