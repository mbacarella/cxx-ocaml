// stdlib's Map.Make and Set.Make, ported exactly -- with utils/
// identifiable.ml's Make_map / Make_set additions -- for the flambda port.
//
// Exactly: the same AVL trees (the same shapes, hence the same orders where
// a shape shows), the same physical-equality shortcuts (`add` of an equal
// binding returns the map itself; union / filter / remove return an input
// unchanged when nothing changed; Set's union / inter / diff have none of
// them in 5.5.1), and the callbacks called in the order
// ocamlopt's code calls them -- left to right for iter / fold / map / mapi /
// filter / filter_map / partition, but for merge and union the order the
// argument evaluation of map.ml's expressions gives (right to left).  A
// callback that creates a Variable (a stamp) sees the same order as in
// ocamlopt.
//
// A map or a set is one pointer to an immutable zone-allocated node: copying
// one costs nothing; an update shares all but a path.  OCaml's Not_found
// (find, min_elt ...) is a null result here.
#pragma once

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing {

template <class K, class Cmp>
class OSet;

// ---- Map.Make ---------------------------------------------------------------
template <class K, class V, class Cmp>
class OMap {
 public:
  struct Node {
    const Node* l;
    K v;
    V d;
    const Node* r;
    int h;
  };
  using Set = OSet<K, Cmp>;

  OMap() = default;
  explicit OMap(const Node* t) : t_(t) {}
  const Node* root() const { return t_; }
  bool is_empty() const { return t_ == nullptr; }
  bool same_as(const OMap& o) const { return t_ == o.t_; }  // ==

  static OMap singleton(const K& k, const V& d) { return OMap(make<Node>(nullptr, k, d, nullptr, 1)); }
  OMap add(const K& k, const V& d) const { return OMap(add_(k, d, t_)); }
  const V* find_opt(const K& k) const {
    for (const Node* n = t_; n;) {
      int c = Cmp{}(k, n->v);
      if (c == 0) return &n->d;
      n = c < 0 ? n->l : n->r;
    }
    return nullptr;
  }
  bool mem(const K& k) const { return find_opt(k) != nullptr; }
  OMap remove(const K& k) const { return OMap(remove_(k, t_)); }
  // (null: Not_found)
  const Node* min_binding() const { return t_ ? min_(t_) : nullptr; }
  const Node* max_binding() const {
    const Node* n = t_;
    if (!n) return nullptr;
    while (n->r) n = n->r;
    return n;
  }
  const Node* choose() const { return min_binding(); }

  template <class F>  // f k d
  void iter(F&& f) const { iter_(t_, f); }
  template <class A, class F>  // f k d acc -> acc
  A fold(F&& f, A acc) const { return fold_(t_, f, std::move(acc)); }
  template <class F>  // f d -> d'
  auto map(F&& f) const {
    using W = decltype(f(std::declval<const V&>()));
    return OMap<K, W, Cmp>(map_<W>(t_, f));
  }
  template <class F>  // f k d -> d'
  auto mapi(F&& f) const {
    using W = decltype(f(std::declval<const K&>(), std::declval<const V&>()));
    return OMap<K, W, Cmp>(mapi_<W>(t_, f));
  }
  template <class P>
  bool for_all(P&& p) const { return for_all_(t_, p); }
  template <class P>
  bool exists(P&& p) const { return exists_(t_, p); }
  template <class P>
  OMap filter(P&& p) const { return OMap(filter_(t_, p)); }
  template <class F>  // f k d -> std::optional<W> (Map.filter_map: W may differ from V)
  auto filter_map(F&& f) const {
    using W = typename decltype(f(std::declval<const K&>(), std::declval<const V&>()))::value_type;
    return OMap<K, W, Cmp>(filter_map_<W>(t_, f));
  }
  template <class P>
  std::pair<OMap, OMap> partition(P&& p) const {
    auto [a, b] = partition_(t_, p);
    return {OMap(a), OMap(b)};
  }
  // merge f m1 m2: f k (d1 or null) (d2 or null) -> std::optional<V>
  template <class F>
  static OMap merge(F&& f, const OMap& a, const OMap& b) { return OMap(merge_(f, a.t_, b.t_)); }
  // union f m1 m2: f k d1 d2 -> std::optional<V>
  template <class F>
  static OMap union_(F&& f, const OMap& a, const OMap& b) { return OMap(union__(f, a.t_, b.t_)); }
  long cardinal() const { return cardinal_(t_); }
  std::vector<std::pair<K, V>> bindings() const {
    std::vector<std::pair<K, V>> out;
    iter([&](const K& k, const V& d) { out.emplace_back(k, d); });
    return out;
  }
  // equal cmp m1 m2
  template <class Eq>
  static bool equal(Eq&& eq, const OMap& a, const OMap& b) {
    std::vector<const Node*> ea, eb;
    cons_enum(a.t_, ea);
    cons_enum(b.t_, eb);
    for (;;) {
      if (ea.empty() || eb.empty()) return ea.empty() && eb.empty();
      const Node* x = ea.back();
      const Node* y = eb.back();
      ea.pop_back();
      eb.pop_back();
      if (Cmp{}(x->v, y->v) != 0 || !eq(x->d, y->d)) return false;
      cons_enum(x->r, ea);
      cons_enum(y->r, eb);
    }
  }

  // ---- Identifiable.Make_map ----
  // of_list: List.fold_left add empty
  static OMap of_list(const std::vector<std::pair<K, V>>& l) {
    OMap m;
    for (auto& [k, v] : l) m = m.add(k, v);
    return m;
  }
  // union_right m1 m2: m2's binding where both have one (merge)
  static OMap union_right(const OMap& a, const OMap& b) {
    return merge([](const K&, const V* x, const V* y) -> std::optional<V> {
      if (y) return *y;
      if (x) return *x;
      return std::nullopt;
    }, a, b);
  }
  static OMap union_left(const OMap& a, const OMap& b) { return union_right(b, a); }
  template <class F>  // f d1 d2 -> d
  static OMap union_merge(F&& f, const OMap& a, const OMap& b) {
    return merge([&](const K&, const V* x, const V* y) -> std::optional<V> {
      if (!x) return y ? std::optional<V>(*y) : std::nullopt;
      if (!y) return *x;
      return f(*x, *y);
    }, a, b);
  }
  Set keys() const {
    return fold([](const K& k, const V&, Set s) { return s.add(k); }, Set());
  }
  std::vector<V> data() const {
    std::vector<V> out;
    iter([&](const K&, const V& d) { out.push_back(d); });
    return out;
  }
  template <class F>  // of_set f set
  static OMap of_set(F&& f, const Set& s) {
    return s.fold([&](const K& e, OMap m) { return m.add(e, f(e)); }, OMap());
  }
  // map_keys f m: of_list (List.map (fun (k, v) -> f k, v) (bindings m))
  template <class F>
  OMap map_keys(F&& f) const {
    std::vector<std::pair<K, V>> b = bindings();
    std::vector<std::pair<K, V>> l;
    l.reserve(b.size());
    // List.map applies f from the last element to the first (stdlib's
    // List.map: let r = f a in r :: map f l -- left to right)
    for (auto& [k, v] : b) l.emplace_back(f(k), v);
    return of_list(l);
  }

 private:
  const Node* t_ = nullptr;

  static int height(const Node* n) { return n ? n->h : 0; }
  static const Node* create(const Node* l, const K& x, const V& d, const Node* r) {
    int hl = height(l), hr = height(r);
    return make<Node>(l, x, d, r, hl >= hr ? hl + 1 : hr + 1);
  }
  static const Node* bal(const Node* l, const K& x, const V& d, const Node* r) {
    int hl = height(l), hr = height(r);
    if (hl > hr + 2) {
      const Node* ll = l->l;
      const Node* lr = l->r;
      if (height(ll) >= height(lr)) return create(ll, l->v, l->d, create(lr, x, d, r));
      return create(create(ll, l->v, l->d, lr->l), lr->v, lr->d, create(lr->r, x, d, r));
    }
    if (hr > hl + 2) {
      const Node* rl = r->l;
      const Node* rr = r->r;
      if (height(rr) >= height(rl)) return create(create(l, x, d, rl), r->v, r->d, rr);
      return create(create(l, x, d, rl->l), rl->v, rl->d, create(rl->r, r->v, r->d, rr));
    }
    return make<Node>(l, x, d, r, hl >= hr ? hl + 1 : hr + 1);
  }
  // d == data: a pointer's identity, an immediate's value; another value is
  // a fresh block (never the same)
  static bool same(const V& a, const V& b) {
    if constexpr (std::is_pointer_v<V> || std::is_arithmetic_v<V> || std::is_enum_v<V>) return a == b;
    else return false;
  }
  static const Node* add_(const K& x, const V& data, const Node* m) {
    if (!m) return make<Node>(nullptr, x, data, nullptr, 1);
    int c = Cmp{}(x, m->v);
    if (c == 0) return same(m->d, data) ? m : make<Node>(m->l, x, data, m->r, m->h);
    if (c < 0) {
      const Node* ll = add_(x, data, m->l);
      return ll == m->l ? m : bal(ll, m->v, m->d, m->r);
    }
    const Node* rr = add_(x, data, m->r);
    return rr == m->r ? m : bal(m->l, m->v, m->d, rr);
  }
  static const Node* min_(const Node* t) {
    while (t->l) t = t->l;
    return t;
  }
  static const Node* remove_min_binding(const Node* t) {
    if (!t->l) return t->r;
    return bal(remove_min_binding(t->l), t->v, t->d, t->r);
  }
  static const Node* merge2(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    const Node* m = min_(t2);
    return bal(t1, m->v, m->d, remove_min_binding(t2));
  }
  static const Node* remove_(const K& x, const Node* m) {
    if (!m) return nullptr;
    int c = Cmp{}(x, m->v);
    if (c == 0) return merge2(m->l, m->r);
    if (c < 0) {
      const Node* ll = remove_(x, m->l);
      return ll == m->l ? m : bal(ll, m->v, m->d, m->r);
    }
    const Node* rr = remove_(x, m->r);
    return rr == m->r ? m : bal(m->l, m->v, m->d, rr);
  }
  template <class F>
  static void iter_(const Node* n, F& f) {
    while (n) {
      iter_(n->l, f);
      f(n->v, n->d);
      n = n->r;
    }
  }
  template <class F, class A>
  static A fold_(const Node* n, F& f, A acc) {
    while (n) {
      acc = f(n->v, n->d, fold_(n->l, f, std::move(acc)));
      n = n->r;
    }
    return acc;
  }
  template <class W, class F>
  static const typename OMap<K, W, Cmp>::Node* map_(const Node* n, F& f) {
    if (!n) return nullptr;
    auto l = map_<W>(n->l, f);
    W d = f(n->d);
    auto r = map_<W>(n->r, f);
    return make<typename OMap<K, W, Cmp>::Node>(l, n->v, d, r, n->h);
  }
  template <class W, class F>
  static const typename OMap<K, W, Cmp>::Node* mapi_(const Node* n, F& f) {
    if (!n) return nullptr;
    auto l = mapi_<W>(n->l, f);
    W d = f(n->v, n->d);
    auto r = mapi_<W>(n->r, f);
    return make<typename OMap<K, W, Cmp>::Node>(l, n->v, d, r, n->h);
  }
  template <class P>
  static bool for_all_(const Node* n, P& p) {
    return !n || (p(n->v, n->d) && for_all_(n->l, p) && for_all_(n->r, p));
  }
  template <class P>
  static bool exists_(const Node* n, P& p) {
    return n && (p(n->v, n->d) || exists_(n->l, p) || exists_(n->r, p));
  }
  static const Node* add_min_binding(const K& k, const V& x, const Node* n) {
    if (!n) return make<Node>(nullptr, k, x, nullptr, 1);
    return bal(add_min_binding(k, x, n->l), n->v, n->d, n->r);
  }
  static const Node* add_max_binding(const K& k, const V& x, const Node* n) {
    if (!n) return make<Node>(nullptr, k, x, nullptr, 1);
    return bal(n->l, n->v, n->d, add_max_binding(k, x, n->r));
  }
  static const Node* join(const Node* l, const K& v, const V& d, const Node* r) {
    if (!l) return add_min_binding(v, d, r);
    if (!r) return add_max_binding(v, d, l);
    if (l->h > r->h + 2) return bal(l->l, l->v, l->d, join(l->r, v, d, r));
    if (r->h > l->h + 2) return bal(join(l, v, d, r->l), r->v, r->d, r->r);
    return create(l, v, d, r);
  }
  static const Node* concat(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    const Node* m = min_(t2);
    return join(t1, m->v, m->d, remove_min_binding(t2));
  }
  static const Node* concat_or_join(const Node* t1, const K& v, const std::optional<V>& d, const Node* t2) {
    return d ? join(t1, v, *d, t2) : concat(t1, t2);
  }
  struct Split {
    const Node* l;
    const V* d;  // null: None
    const Node* r;
  };
  static Split split(const K& x, const Node* n) {
    if (!n) return {nullptr, nullptr, nullptr};
    int c = Cmp{}(x, n->v);
    if (c == 0) return {n->l, &n->d, n->r};
    if (c < 0) {
      Split s = split(x, n->l);
      return {s.l, s.d, join(s.r, n->v, n->d, n->r)};
    }
    Split s = split(x, n->r);
    return {join(n->l, n->v, n->d, s.l), s.d, s.r};
  }
  template <class P>
  static const Node* filter_(const Node* m, P& p) {
    if (!m) return nullptr;
    const Node* l = filter_(m->l, p);
    bool pvd = p(m->v, m->d);
    const Node* r = filter_(m->r, p);
    if (pvd) return l == m->l && r == m->r ? m : join(l, m->v, m->d, r);
    return concat(l, r);
  }
  template <class W, class F>
  static const typename OMap<K, W, Cmp>::Node* filter_map_(const Node* m, F& f) {
    if (!m) return nullptr;
    auto l = filter_map_<W>(m->l, f);
    std::optional<W> fvd = f(m->v, m->d);
    auto r = filter_map_<W>(m->r, f);
    return fvd ? OMap<K, W, Cmp>::join(l, m->v, *fvd, r) : OMap<K, W, Cmp>::concat(l, r);
  }
  template <class K2, class V2, class C2>
  friend class OMap;
  template <class P>
  static std::pair<const Node*, const Node*> partition_(const Node* m, P& p) {
    if (!m) return {nullptr, nullptr};
    auto [lt, lf] = partition_(m->l, p);
    bool pvd = p(m->v, m->d);
    auto [rt, rf] = partition_(m->r, p);
    if (pvd) return {join(lt, m->v, m->d, rt), concat(lf, rf)};
    return {concat(lt, rt), join(lf, m->v, m->d, rf)};
  }
  // concat_or_join (merge f l1 l2) v (f v ...) (merge f r1 r2): ocamlopt
  // evaluates the arguments right to left
  template <class F>
  static const Node* merge_(F& f, const Node* s1, const Node* s2) {
    if (!s1 && !s2) return nullptr;
    if (s1 && s1->h >= height(s2)) {
      Split s = split(s1->v, s2);
      const Node* r = merge_(f, s1->r, s.r);
      std::optional<V> d = f(s1->v, &s1->d, s.d);
      const Node* l = merge_(f, s1->l, s.l);
      return concat_or_join(l, s1->v, d, r);
    }
    Split s = split(s2->v, s1);
    const Node* r = merge_(f, s.r, s2->r);
    std::optional<V> d = f(s2->v, s.d, &s2->d);
    const Node* l = merge_(f, s.l, s2->l);
    return concat_or_join(l, s2->v, d, r);
  }
  // let l = union f l1 l2 and r = union f r1 r2 in ... (left to right),
  // then f
  template <class F>
  static const Node* union__(F& f, const Node* s1, const Node* s2) {
    if (!s1) return s2;
    if (!s2) return s1;
    if (s1->h >= s2->h) {
      Split s = split(s1->v, s2);
      const Node* l = union__(f, s1->l, s.l);
      const Node* r = union__(f, s1->r, s.r);
      if (!s.d) return join(l, s1->v, s1->d, r);
      return concat_or_join(l, s1->v, f(s1->v, s1->d, *s.d), r);
    }
    Split s = split(s2->v, s1);
    const Node* l = union__(f, s.l, s2->l);
    const Node* r = union__(f, s.r, s2->r);
    if (!s.d) return join(l, s2->v, s2->d, r);
    return concat_or_join(l, s2->v, f(s2->v, *s.d, s2->d), r);
  }
  static long cardinal_(const Node* n) { return n ? cardinal_(n->l) + 1 + cardinal_(n->r) : 0; }
  // cons_enum n e: n's left spine onto e, its leftmost node (the next) last
  static void cons_enum(const Node* n, std::vector<const Node*>& e) {
    for (; n; n = n->l) e.push_back(n);
  }

  template <class, class, class>
  friend class OMap;
};

// ---- Set.Make ---------------------------------------------------------------
template <class K, class Cmp>
class OSet {
 public:
  struct Node {
    const Node* l;
    K v;
    const Node* r;
    int h;
  };
  OSet() = default;
  explicit OSet(const Node* t) : t_(t) {}
  const Node* root() const { return t_; }
  bool is_empty() const { return t_ == nullptr; }
  bool same_as(const OSet& o) const { return t_ == o.t_; }

  static OSet singleton(const K& x) { return OSet(make<Node>(nullptr, x, nullptr, 1)); }
  OSet add(const K& x) const { return OSet(add_(x, t_)); }
  bool mem(const K& x) const {
    for (const Node* n = t_; n;) {
      int c = Cmp{}(x, n->v);
      if (c == 0) return true;
      n = c < 0 ? n->l : n->r;
    }
    return false;
  }
  const K* find_opt(const K& x) const {
    for (const Node* n = t_; n;) {
      int c = Cmp{}(x, n->v);
      if (c == 0) return &n->v;
      n = c < 0 ? n->l : n->r;
    }
    return nullptr;
  }
  OSet remove(const K& x) const { return OSet(remove_(x, t_)); }
  static OSet union_(const OSet& a, const OSet& b) { return OSet(union__(a.t_, b.t_)); }
  static OSet inter(const OSet& a, const OSet& b) { return OSet(inter_(a.t_, b.t_)); }
  static OSet diff(const OSet& a, const OSet& b) { return OSet(diff_(a.t_, b.t_)); }
  static bool subset(const OSet& a, const OSet& b) { return subset_(a.t_, b.t_); }
  static int compare(const OSet& a, const OSet& b) {
    std::vector<const Node*> ea, eb;
    cons_enum(a.t_, ea);
    cons_enum(b.t_, eb);
    for (;;) {
      if (ea.empty()) return eb.empty() ? 0 : -1;
      if (eb.empty()) return 1;
      const Node* x = ea.back();
      const Node* y = eb.back();
      ea.pop_back();
      eb.pop_back();
      int c = Cmp{}(x->v, y->v);
      if (c != 0) return c;
      cons_enum(x->r, ea);
      cons_enum(y->r, eb);
    }
  }
  static bool equal(const OSet& a, const OSet& b) { return compare(a, b) == 0; }
  // (null: Not_found)
  const K* min_elt() const {
    const Node* n = t_;
    if (!n) return nullptr;
    while (n->l) n = n->l;
    return &n->v;
  }
  const K* max_elt() const {
    const Node* n = t_;
    if (!n) return nullptr;
    while (n->r) n = n->r;
    return &n->v;
  }
  const K* choose() const { return min_elt(); }

  template <class F>
  void iter(F&& f) const { iter_(t_, f); }
  template <class A, class F>  // f e acc -> acc
  A fold(F&& f, A acc) const { return fold_(t_, f, std::move(acc)); }
  template <class P>
  bool for_all(P&& p) const { return for_all_(t_, p); }
  template <class P>
  bool exists(P&& p) const { return exists_(t_, p); }
  template <class P>
  OSet filter(P&& p) const { return OSet(filter_(t_, p)); }
  template <class P>
  std::pair<OSet, OSet> partition(P&& p) const {
    auto [a, b] = partition_(t_, p);
    return {OSet(a), OSet(b)};
  }
  long cardinal() const { return cardinal_(t_); }
  std::vector<K> elements() const {
    std::vector<K> out;
    iter([&](const K& x) { out.push_back(x); });
    return out;
  }

  // ---- Identifiable.Make_set ----
  // of_list: [] -> empty | [t] -> singleton t | t :: q -> fold_left add (singleton t) q
  static OSet of_list(const std::vector<K>& l) {
    if (l.empty()) return OSet();
    OSet s = singleton(l[0]);
    for (std::size_t i = 1; i < l.size(); ++i) s = s.add(l[i]);
    return s;
  }
  // map f s = of_list (List.map f (elements s))
  template <class F>
  OSet map(F&& f) const {
    std::vector<K> e = elements();
    std::vector<K> l;
    l.reserve(e.size());
    for (const K& x : e) l.push_back(f(x));
    return of_list(l);
  }

 private:
  const Node* t_ = nullptr;

  static int height(const Node* n) { return n ? n->h : 0; }
  static const Node* create(const Node* l, const K& v, const Node* r) {
    int hl = height(l), hr = height(r);
    return make<Node>(l, v, r, hl >= hr ? hl + 1 : hr + 1);
  }
  static const Node* bal(const Node* l, const K& v, const Node* r) {
    int hl = height(l), hr = height(r);
    if (hl > hr + 2) {
      const Node* ll = l->l;
      const Node* lr = l->r;
      if (height(ll) >= height(lr)) return create(ll, l->v, create(lr, v, r));
      return create(create(ll, l->v, lr->l), lr->v, create(lr->r, v, r));
    }
    if (hr > hl + 2) {
      const Node* rl = r->l;
      const Node* rr = r->r;
      if (height(rr) >= height(rl)) return create(create(l, v, rl), r->v, rr);
      return create(create(l, v, rl->l), rl->v, create(rl->r, r->v, rr));
    }
    return make<Node>(l, v, r, hl >= hr ? hl + 1 : hr + 1);
  }
  static const Node* add_(const K& x, const Node* t) {
    if (!t) return make<Node>(nullptr, x, nullptr, 1);
    int c = Cmp{}(x, t->v);
    if (c == 0) return t;
    if (c < 0) {
      const Node* ll = add_(x, t->l);
      return ll == t->l ? t : bal(ll, t->v, t->r);
    }
    const Node* rr = add_(x, t->r);
    return rr == t->r ? t : bal(t->l, t->v, rr);
  }
  static const Node* add_min_element(const K& x, const Node* n) {
    if (!n) return make<Node>(nullptr, x, nullptr, 1);
    return bal(add_min_element(x, n->l), n->v, n->r);
  }
  static const Node* add_max_element(const K& x, const Node* n) {
    if (!n) return make<Node>(nullptr, x, nullptr, 1);
    return bal(n->l, n->v, add_max_element(x, n->r));
  }
  static const Node* join(const Node* l, const K& v, const Node* r) {
    if (!l) return add_min_element(v, r);
    if (!r) return add_max_element(v, l);
    if (l->h > r->h + 2) return bal(l->l, l->v, join(l->r, v, r));
    if (r->h > l->h + 2) return bal(join(l, v, r->l), r->v, r->r);
    return create(l, v, r);
  }
  static const Node* min_(const Node* t) {
    while (t->l) t = t->l;
    return t;
  }
  static const Node* remove_min_elt(const Node* t) {
    if (!t->l) return t->r;
    return bal(remove_min_elt(t->l), t->v, t->r);
  }
  static const Node* merge2(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    return bal(t1, min_(t2)->v, remove_min_elt(t2));
  }
  static const Node* concat(const Node* t1, const Node* t2) {
    if (!t1) return t2;
    if (!t2) return t1;
    return join(t1, min_(t2)->v, remove_min_elt(t2));
  }
  struct Split {
    const Node* l;
    bool present;
    const Node* r;
  };
  static Split split(const K& x, const Node* n) {
    if (!n) return {nullptr, false, nullptr};
    int c = Cmp{}(x, n->v);
    if (c == 0) return {n->l, true, n->r};
    if (c < 0) {
      Split s = split(x, n->l);
      return {s.l, s.present, join(s.r, n->v, n->r)};
    }
    Split s = split(x, n->r);
    return {join(n->l, n->v, s.l), s.present, s.r};
  }
  static const Node* remove_(const K& x, const Node* t) {
    if (!t) return nullptr;
    int c = Cmp{}(x, t->v);
    if (c == 0) return merge2(t->l, t->r);
    if (c < 0) {
      const Node* ll = remove_(x, t->l);
      return ll == t->l ? t : bal(ll, t->v, t->r);
    }
    const Node* rr = remove_(x, t->r);
    return rr == t->r ? t : bal(t->l, t->v, rr);
  }
  // 5.5.1's set.ml: union / inter / diff rebuild through join and concat
  // (trunk's keep an input when nothing changed); a constructor's arguments
  // are evaluated right to left
  static const Node* union__(const Node* s1, const Node* s2) {
    if (!s1) return s2;
    if (!s2) return s1;
    if (s1->h >= s2->h) {
      if (s2->h == 1) return add_(s2->v, s1);
      Split s = split(s1->v, s2);
      const Node* rr = union__(s1->r, s.r);
      const Node* ll = union__(s1->l, s.l);
      return join(ll, s1->v, rr);
    }
    if (s1->h == 1) return add_(s1->v, s2);
    Split s = split(s2->v, s1);
    const Node* rr = union__(s.r, s2->r);
    const Node* ll = union__(s.l, s2->l);
    return join(ll, s2->v, rr);
  }
  static const Node* inter_(const Node* s1, const Node* s2) {
    if (!s1 || !s2) return nullptr;
    Split s = split(s1->v, s2);
    const Node* rr = inter_(s1->r, s.r);
    const Node* ll = inter_(s1->l, s.l);
    return s.present ? join(ll, s1->v, rr) : concat(ll, rr);
  }
  static const Node* diff_(const Node* s1, const Node* s2) {
    if (!s1) return nullptr;
    if (!s2) return s1;
    Split s = split(s1->v, s2);
    const Node* rr = diff_(s1->r, s.r);
    const Node* ll = diff_(s1->l, s.l);
    return s.present ? concat(ll, rr) : join(ll, s1->v, rr);
  }
  static bool subset_(const Node* s1, const Node* s2) {
    if (!s1) return true;
    if (!s2) return false;
    int c = Cmp{}(s1->v, s2->v);
    if (c == 0) return subset_(s1->l, s2->l) && subset_(s1->r, s2->r);
    if (c < 0) {
      Node half{s1->l, s1->v, nullptr, 0};
      return subset_(&half, s2->l) && subset_(s1->r, s2);
    }
    Node half{nullptr, s1->v, s1->r, 0};
    return subset_(&half, s2->r) && subset_(s1->l, s2);
  }
  template <class F>
  static void iter_(const Node* n, F& f) {
    while (n) {
      iter_(n->l, f);
      f(n->v);
      n = n->r;
    }
  }
  template <class F, class A>
  static A fold_(const Node* n, F& f, A acc) {
    while (n) {
      acc = f(n->v, fold_(n->l, f, std::move(acc)));
      n = n->r;
    }
    return acc;
  }
  template <class P>
  static bool for_all_(const Node* n, P& p) { return !n || (p(n->v) && for_all_(n->l, p) && for_all_(n->r, p)); }
  template <class P>
  static bool exists_(const Node* n, P& p) { return n && (p(n->v) || exists_(n->l, p) || exists_(n->r, p)); }
  template <class P>
  static const Node* filter_(const Node* t, P& p) {
    if (!t) return nullptr;
    const Node* l = filter_(t->l, p);
    bool pv = p(t->v);
    const Node* r = filter_(t->r, p);
    if (pv) return l == t->l && r == t->r ? t : join(l, t->v, r);
    return concat(l, r);
  }
  template <class P>
  static std::pair<const Node*, const Node*> partition_(const Node* t, P& p) {
    if (!t) return {nullptr, nullptr};
    auto [lt, lf] = partition_(t->l, p);
    bool pv = p(t->v);
    auto [rt, rf] = partition_(t->r, p);
    if (pv) return {join(lt, t->v, rt), concat(lf, rf)};
    return {concat(lt, rt), join(lf, t->v, rf)};
  }
  static long cardinal_(const Node* n) { return n ? cardinal_(n->l) + 1 + cardinal_(n->r) : 0; }
  // cons_enum n e: n's left spine onto e, its leftmost node (the next) last
  static void cons_enum(const Node* n, std::vector<const Node*>& e) {
    for (; n; n = n->l) e.push_back(n);
  }
};

}  // namespace cppcaml::typing
