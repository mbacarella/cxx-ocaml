// Port of lambda/switch.ml (cxx/PORTING.md stage 10): action stores and the
// compilation of integer switches into test trees and jump tables.  The
// functors become templates: CtxStore<A> / Store<A> take a policy with the
// Stored signature, Make<Arg> an Arg with the S signature (types and static
// functions).  Make's module-level state (the opt_count memo table `t` and
// `ok_inter`) is per instantiation, as it is per functor application.
//
// OCaml's evaluation order is kept where it creates idents or exit numbers:
// application arguments are evaluated right to left, `let .. and ..`
// bindings left to right, Array.map from the first element.  Integer
// arithmetic is OCaml's 63-bit arithmetic (the bounds of an unbounded switch
// are min_int / max_int).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace cppcaml::typing::switch_ {

// ---- OCaml's 63-bit int -----------------------------------------------------
inline constexpr long ocaml_max_int = (1L << 62) - 1;
inline constexpr long ocaml_min_int = -(1L << 62);
inline long wrap_int(unsigned long x) { return static_cast<long>(x << 1) >> 1; }
inline long iadd(long a, long b) { return wrap_int(static_cast<unsigned long>(a) + static_cast<unsigned long>(b)); }
inline long isub(long a, long b) { return wrap_int(static_cast<unsigned long>(a) - static_cast<unsigned long>(b)); }
inline long ineg(long a) { return wrap_int(0UL - static_cast<unsigned long>(a)); }
inline long iabs(long a) { return a >= 0 ? a : ineg(a); }

// type 'a shared = Shared of 'a | Single of 'a
template <class A>
struct Shared {
  bool shared;
  A act;
};

// ---- CtxStore / Store ------------------------------------------------------------
// A CtxStored policy S provides the types `t`, `key`, `context` and
//   static std::optional<key> make_key(const context&, const t&);
//   static bool same_key(const key&, const key&);
// same_key is `compare_key k1 k2 = 0`: the AMap of switch.ml is only
// searched by key (find / add) and iterated to mark entries shared, which
// does not depend on the iteration order, so an association list searched
// with the equality is the same map.
template <class S>
class CtxStore {
 public:
  using A = typename S::t;
  using Key = typename S::key;
  using Ctx = typename S::context;

  long act_store(const Ctx& ctx, const A& act) { return store(false, ctx, act); }
  long act_store_shared(const Ctx& ctx, const A& act) { return store(true, ctx, act); }
  std::vector<A> act_get() const {
    std::vector<A> r;
    for (auto& [_, act] : acts_) r.push_back(act);
    return r;
  }
  std::vector<Shared<A>> act_get_shared() const {
    std::vector<Shared<A>> acts;
    for (auto& [shared, act] : acts_) acts.push_back({shared, act});
    for (auto& e : map_)
      if (e.shared) acts[static_cast<std::size_t>(e.i)].shared = true;
    return acts;
  }

 private:
  struct Entry {
    Key key;
    bool shared;
    long i;
  };
  std::vector<Entry> map_;
  long next_ = 0;
  std::vector<std::pair<bool, A>> acts_;  // st.acts, in store order (the list reversed)

  long add(bool mustshare, const A& act) {
    long i = next_;
    acts_.push_back({mustshare, act});
    next_ = i + 1;
    return i;
  }
  long store(bool mustshare, const Ctx& ctx, const A& act) {
    std::optional<Key> key = S::make_key(ctx, act);
    if (key) {
      for (auto& e : map_)
        if (S::same_key(e.key, *key)) {
          if (!e.shared) e.shared = true;
          return e.i;
        }
      long i = add(mustshare, act);
      map_.push_back({*key, mustshare, i});
      return i;
    }
    return add(mustshare, act);
  }
};

// A Stored policy S provides `t`, `key`, make_key(const t&) and same_key.
template <class S>
struct UnitContext {
  using t = typename S::t;
  using key = typename S::key;
  using context = std::monostate;
  static std::optional<key> make_key(const context&, const t& act) { return S::make_key(act); }
  static bool same_key(const key& a, const key& b) { return S::same_key(a, b); }
};
template <class S>
class Store : public CtxStore<UnitContext<S>> {
  using Base = CtxStore<UnitContext<S>>;

 public:
  using A = typename S::t;
  long act_store(const A& act) { return Base::act_store(std::monostate{}, act); }
  long act_store_shared(const A& act) { return Base::act_store_shared(std::monostate{}, act); }
};

// ---- Make (Arg : S) ------------------------------------------------------------------
// Arg provides the types primitive, loc, arg, test, act and the static
// functions eqint() neint() leint() ltint() geint() gtint(), bind, make_const,
// make_offset, make_prim (args as a vector), make_isout, make_isin,
// make_is_nonzero, arg_as_test, make_if, make_switch (the acts array passed
// by reference: it may be updated in place), make_catch, make_exit.
template <class Arg>
struct Make {
  using act = typename Arg::act;
  using arg_t = typename Arg::arg;
  using loc_t = typename Arg::loc;

  // a (low, high, act) case of an 'a inter
  struct Case {
    long low, high, act;
  };
  using Cases = std::vector<Case>;
  template <class A>
  struct Inter {
    Cases cases;
    std::shared_ptr<const std::vector<A>> actions;
  };

  static constexpr int small_size_limit = 8;
  static constexpr int medium_size_limit = 16;

  static long get_act(const Cases& cases, long i) { return cases[static_cast<std::size_t>(i)].act; }
  static long get_low(const Cases& cases, long i) { return cases[static_cast<std::size_t>(i)].low; }
  static long get_high(const Cases& cases, long i) { return cases[static_cast<std::size_t>(i)].high; }

  // a "cost" as a number of tests in the worst case (ni: interval tests)
  struct Ctests {
    long n;
    long ni;
  };
  static constexpr Ctests too_much{ocaml_max_int, ocaml_max_int};

  static bool less_tests(const Ctests& c1, const Ctests& c2) {
    if (c1.n < c2.n) return true;
    if (c1.n == c2.n) return c1.ni < c2.ni;
    return false;
  }
  static bool eq_tests(const Ctests& c1, const Ctests& c2) { return c1.n == c2.n && c1.ni == c2.ni; }
  using Cost = std::pair<Ctests, Ctests>;
  static bool less2tests(const Cost& a, const Cost& b) {
    if (eq_tests(a.first, b.first)) return less_tests(a.second, b.second);
    return less_tests(a.first, b.first);
  }
  static void add_test(Ctests& t1, const Ctests& t2) {
    t1.n = iadd(t1.n, t2.n);
    t1.ni = iadd(t1.ni, t2.ni);
  }

  // t_ret = Inter of int * int | Sep of int | No
  struct TRet {
    enum class K { Inter, Sep, No } k;
    long i = 0, j = 0;
  };
  using OptResult = std::pair<TRet, Cost>;

  static Cases sub(const Cases& a, long pos, long len) {
    return Cases(a.begin() + pos, a.begin() + pos + len);
  }
  struct Coupe {
    long lim;
    Cases left, right;
  };
  static Coupe coupe(const Cases& cases, long i) {
    long l = get_low(cases, i);
    long n = static_cast<long>(cases.size());
    return {l, sub(cases, 0, i), sub(cases, i, n - i)};
  }

  static Cases case_append(const Cases& c1, const Cases& c2) {
    long len1 = static_cast<long>(c1.size()), len2 = static_cast<long>(c2.size());
    if (len1 == 0) return c2;
    if (len2 == 0) return c1;
    auto [l1, h1, act1] = c1[len1 - 1];
    auto [l2, h2, act2] = c2[0];
    if (act1 == act2) {
      Cases r(len1 + len2 - 1, c1[0]);
      for (long i = 0; i <= len1 - 2; ++i) r[i] = c1[i];
      long l, h;
      // `let l = .. and h = ..`
      if (len1 < 2) l = l1;
      else {
        long hh = r[len1 - 2].high;
        l = std::min(iadd(hh, 1), l1);
      }
      if (len2 < 2) h = h2;
      else {
        long ll = c2[1].low;
        h = std::max(h2, isub(ll, 1));
      }
      r[len1 - 1] = {l, h, act1};
      for (long i = 1; i <= len2 - 1; ++i) r[len1 - 1 + i] = c2[i];
      return r;
    }
    if (h1 > l1) {
      Cases r(len1 + len2, c1[0]);
      for (long i = 0; i <= len1 - 2; ++i) r[i] = c1[i];
      r[len1 - 1] = {l1, isub(l2, 1), act1};
      for (long i = 0; i <= len2 - 1; ++i) r[len1 + i] = c2[i];
      return r;
    }
    if (h2 > l2) {
      Cases r(len1 + len2, c1[0]);
      for (long i = 0; i <= len1 - 1; ++i) r[i] = c1[i];
      r[len1] = {iadd(h1, 1), h2, act2};
      for (long i = 1; i <= len2 - 1; ++i) r[len1 + i] = c2[i];
      return r;
    }
    Cases r = c1;
    r.insert(r.end(), c2.begin(), c2.end());
    return r;
  }

  struct CoupeInter {
    long low, high;
    Cases inside, outside;
  };
  static CoupeInter coupe_inter(long i, long j, const Cases& cases) {
    long lcases = static_cast<long>(cases.size());
    long low = get_low(cases, i);
    long high = get_high(cases, j);
    return {low, high, sub(cases, i, j - i + 1),
            case_append(sub(cases, 0, i), sub(cases, j + 1, lcases - (j + 1)))};
  }

  // kind = Kvalue of int | Kinter of int | Kempty; a key is a kind list
  enum class KindTag : std::uint8_t { Kvalue, Kinter, Kempty };
  using Key = std::vector<std::pair<KindTag, long>>;
  static inline std::map<Key, OptResult> t;  // Hashtbl t (a memo table)

  static Key make_key(const Cases& cases) {
    std::vector<std::pair<long, long>> seen;  // (act, index), head first
    long count = 0;
    auto got_it = [&](long act) {
      for (auto& [act0, index] : seen)
        if (act0 == act) return index;
      seen.insert(seen.begin(), {act, count});
      long r = count;
      ++count;
      return r;
    };
    auto make_one = [&](long l, long h, long act) -> std::pair<KindTag, long> {
      if (l == h) return {KindTag::Kvalue, got_it(act)};
      return {KindTag::Kinter, got_it(act)};
    };
    // `make_one l h act :: make_rec (i-1) l`: the tail is evaluated first,
    // so the actions are numbered from the first case on.
    long n = static_cast<long>(cases.size());
    std::vector<std::pair<KindTag, long>> ones(n);
    std::vector<bool> gap(n, false);  // Kempty before the element (in list order)
    for (long i = 0; i < n; ++i) {
      auto [l, h, act] = cases[i];
      ones[i] = make_one(l, h, act);
      if (i < n - 1) gap[i] = !(cases[i + 1].low == iadd(h, 1));
    }
    Key key;
    for (long i = n - 1; i >= 0; --i) {
      if (i < n - 1 && gap[i]) key.push_back({KindTag::Kempty, 0});
      key.push_back(ones[i]);
    }
    return key;
  }

  static bool same_act(const Cases& t) {
    long len = static_cast<long>(t.size());
    long a = get_act(t, len - 1);
    for (long i = len - 2; i >= 0; --i)
      if (get_act(t, i) != a) return false;
    return true;
  }

  static constexpr long inter_limit = 1L << 16;
  static inline bool ok_inter = false;

  static OptResult opt_count(const Cases& cases) {
    Key key = make_key(cases);
    if (auto it = t.find(key); it != t.end()) return it->second;
    OptResult r;
    long lcases = static_cast<long>(cases.size());
    if (lcases == 0) throw std::logic_error("Switch.opt_count");
    if (same_act(cases)) r = {TRet{TRet::K::No}, {{0, 0}, {0, 0}}};
    else if (lcases < small_size_limit) r = enum_(cases);
    else if (lcases < medium_size_limit) r = heuristic(cases);
    else r = divide(cases);
    t[key] = r;
    return r;
  }

  static OptResult divide(const Cases& cases) {
    long lcases = static_cast<long>(cases.size());
    long m = lcases / 2;
    Coupe c = coupe(cases, m);
    Ctests ci{1, 0}, cm{1, 0};
    auto [cml, cleft] = opt_count(c.left).second;
    auto [cmr, cright] = opt_count(c.right).second;
    add_test(ci, cleft);
    add_test(ci, cright);
    if (less_tests(cml, cmr)) add_test(cm, cmr);
    else add_test(cm, cml);
    return {TRet{TRet::K::Sep, m}, {cm, ci}};
  }

  static OptResult heuristic(const Cases& cases) {
    long lcases = static_cast<long>(cases.size());
    OptResult sep = divide(cases);
    OptResult inter;
    if (ok_inter) {
      long act0 = cases[0].act, act1 = cases[lcases - 1].act;
      if (act0 == act1) {
        CoupeInter ci_ = coupe_inter(1, lcases - 2, cases);
        auto [cmi, cinside] = opt_count(ci_.inside).second;
        auto [cmo, coutside] = opt_count(ci_.outside).second;
        Ctests cmij{1, ci_.low == ci_.high ? 0 : 1};
        Ctests cij{1, ci_.low == ci_.high ? 0 : 1};
        add_test(cij, cinside);
        add_test(cij, coutside);
        if (less_tests(cmi, cmo)) add_test(cmij, cmo);
        else add_test(cmij, cmi);
        inter = {TRet{TRet::K::Inter, 1, lcases - 2}, {cmij, cij}};
      } else
        inter = {TRet{TRet::K::Inter, -1, -1}, {too_much, too_much}};
    } else
      inter = {TRet{TRet::K::Inter, -1, -1}, {too_much, too_much}};
    if (less2tests(sep.second, inter.second)) return sep;
    return inter;
  }

  static OptResult enum_(const Cases& cases) {
    long lcases = static_cast<long>(cases.size());
    long lim;
    Cost with_sep;
    {
      long best = -1;
      Cost best_cost{too_much, too_much};
      for (long i = 1; i <= lcases - 1; ++i) {
        Coupe c = coupe(cases, i);
        Ctests ci{1, 0}, cm{1, 0};
        auto [cml, cleft] = opt_count(c.left).second;
        auto [cmr, cright] = opt_count(c.right).second;
        add_test(ci, cleft);
        add_test(ci, cright);
        if (less_tests(cml, cmr)) add_test(cm, cmr);
        else add_test(cm, cml);
        if (less2tests({cm, ci}, best_cost)) {
          best = i;
          best_cost = {cm, ci};
        }
      }
      lim = best;
      with_sep = best_cost;
    }
    long ilow, ihigh;
    Cost with_inter;
    if (!ok_inter) {
      long rlow = -1, rhigh = -1;
      Cost best_cost{too_much, too_much};
      for (long i = 1; i <= lcases - 2; ++i) {
        CoupeInter c = coupe_inter(i, i, cases);
        if (c.low == c.high) {
          auto [cmi, cinside] = opt_count(c.inside).second;
          auto [cmo, coutside] = opt_count(c.outside).second;
          Ctests cmij{1, 0}, cij{1, 0};
          add_test(cij, cinside);
          add_test(cij, coutside);
          if (less_tests(cmi, cmo)) add_test(cmij, cmo);
          else add_test(cmij, cmi);
          if (less2tests({cmij, cij}, best_cost)) {
            rlow = i;
            rhigh = i;
            best_cost = {cmij, cij};
          }
        }
      }
      ilow = rlow;
      ihigh = rhigh;
      with_inter = best_cost;
    } else {
      long rlow = -1, rhigh = -1;
      Cost best_cost{too_much, too_much};
      for (long i = 1; i <= lcases - 2; ++i)
        for (long j = i; j <= lcases - 2; ++j) {
          CoupeInter c = coupe_inter(i, j, cases);
          auto [cmi, cinside] = opt_count(c.inside).second;
          auto [cmo, coutside] = opt_count(c.outside).second;
          Ctests cmij{1, c.low == c.high ? 0 : 1};
          Ctests cij{1, c.low == c.high ? 0 : 1};
          add_test(cij, cinside);
          add_test(cij, coutside);
          if (less_tests(cmi, cmo)) add_test(cmij, cmo);
          else add_test(cmij, cmi);
          if (less2tests({cmij, cij}, best_cost)) {
            rlow = i;
            rhigh = j;
            best_cost = {cmij, cij};
          }
        }
      ilow = rlow;
      ihigh = rhigh;
      with_inter = best_cost;
    }
    TRet r{TRet::K::Inter, ilow, ihigh};
    Cost rc = with_inter;
    if (less2tests(with_sep, rc)) {
      r = TRet{TRet::K::Sep, lim};
      rc = with_sep;
    }
    return {r, rc};
  }

  // an input argument "shifted" by a (negative) offset
  struct TCtx {
    long off;
    arg_t arg;
  };
  using Fn = std::function<act(const TCtx&)>;

  static act make_if_test(const typename Arg::primitive& test, arg_t arg, long i, act ifso, act ifnot) {
    return Arg::make_if(Arg::make_prim(test, {arg, Arg::make_const(i)}), ifso, ifnot);
  }
  static act make_if_lt(arg_t arg, long i, act ifso, act ifnot) {
    if (i == 1) return make_if_test(Arg::leint(), arg, 0, ifso, ifnot);
    return make_if_test(Arg::ltint(), arg, i, ifso, ifnot);
  }
  static act make_if_ge(arg_t arg, long i, act ifso, act ifnot) {
    if (i == 1) return make_if_test(Arg::gtint(), arg, 0, ifso, ifnot);
    return make_if_test(Arg::geint(), arg, i, ifso, ifnot);
  }
  static act make_if_eq(arg_t arg, long i, act ifso, act ifnot) { return make_if_test(Arg::eqint(), arg, i, ifso, ifnot); }
  static act make_if_ne(arg_t arg, long i, act ifso, act ifnot) { return make_if_test(Arg::neint(), arg, i, ifso, ifnot); }
  static act make_if_nonzero(arg_t arg, act ifso, act ifnot) {
    return Arg::make_if(Arg::make_is_nonzero(arg), ifso, ifnot);
  }
  static act make_if_bool(arg_t arg, act ifso, act ifnot) { return Arg::make_if(Arg::arg_as_test(arg), ifso, ifnot); }
  static act do_make_if_out(arg_t h, arg_t arg, act ifso, act ifno) {
    return Arg::make_if(Arg::make_isout(h, arg), ifso, ifno);
  }
  static act do_make_if_in(arg_t h, arg_t arg, act ifso, act ifno) {
    return Arg::make_if(Arg::make_isin(h, arg), ifso, ifno);
  }
  // make_if_out / make_if_in: `do_make_if_* (make_const d) arg (mk_ifso ctx)
  // (mk_ifno ctx)` evaluates mk_ifno first
  template <class Do>
  static act make_if_inout(Do do_make, const TCtx& ctx, long l, long d, const Fn& mk_ifso, const Fn& mk_ifno) {
    if (l == 0) {
      act ifno = mk_ifno(ctx);
      act ifso = mk_ifso(ctx);
      return do_make(Arg::make_const(d), ctx.arg, ifso, ifno);
    }
    return Arg::bind(Arg::make_offset(ctx.arg, ineg(l)), [&](arg_t arg) {
      TCtx ctx2{iadd(ineg(l), ctx.off), arg};
      act ifno = mk_ifno(ctx2);
      act ifso = mk_ifso(ctx2);
      return do_make(Arg::make_const(d), arg, ifso, ifno);
    });
  }
  static act make_if_out(const TCtx& ctx, long l, long d, const Fn& mk_ifso, const Fn& mk_ifno) {
    return make_if_inout(do_make_if_out, ctx, l, d, mk_ifso, mk_ifno);
  }
  static act make_if_in(const TCtx& ctx, long l, long d, const Fn& mk_ifso, const Fn& mk_ifno) {
    return make_if_inout(do_make_if_in, ctx, l, d, mk_ifso, mk_ifno);
  }

  static act c_test(const TCtx& ctx, const Inter<Fn>& s) {
    const Cases& cases = s.cases;
    long lcases = static_cast<long>(cases.size());
    if (!(lcases > 0)) throw std::logic_error("Switch.c_test");
    if (lcases == 1) return (*s.actions)[static_cast<std::size_t>(get_act(cases, 0))](ctx);
    TRet w = opt_count(cases).first;
    switch (w.k) {
      case TRet::K::No: return (*s.actions)[static_cast<std::size_t>(get_act(cases, 0))](ctx);
      case TRet::K::Inter: {
        CoupeInter c = coupe_inter(w.i, w.j, cases);
        Ctests cinside = opt_count(c.inside).second.first;
        Ctests coutside = opt_count(c.outside).second.first;
        Inter<Fn> inside{c.inside, s.actions}, outside{c.outside, s.actions};
        if (c.low == c.high) {
          if (less_tests(coutside, cinside)) {
            act o = c_test(ctx, outside);
            act in = c_test(ctx, inside);
            return make_if_eq(ctx.arg, iadd(c.low, ctx.off), in, o);
          }
          act in = c_test(ctx, inside);
          act o = c_test(ctx, outside);
          return make_if_ne(ctx.arg, iadd(c.low, ctx.off), o, in);
        }
        Fn f_inside = [&](const TCtx& ctx) { return c_test(ctx, inside); };
        Fn f_outside = [&](const TCtx& ctx) { return c_test(ctx, outside); };
        if (less_tests(coutside, cinside))
          return make_if_in(ctx, iadd(c.low, ctx.off), isub(c.high, c.low), f_inside, f_outside);
        return make_if_out(ctx, iadd(c.low, ctx.off), isub(c.high, c.low), f_outside, f_inside);
      }
      case TRet::K::Sep: {
        long i = w.i;
        Coupe c = coupe(cases, i);
        Ctests cleft = opt_count(c.left).second.first;
        Ctests cright = opt_count(c.right).second.first;
        Inter<Fn> left{c.left, s.actions}, right{c.right, s.actions};
        long lim = c.lim;
        if (i == 1 && iadd(lim, ctx.off) == 1 && iadd(get_low(cases, 0), ctx.off) == 0) {
          act l = c_test(ctx, left);
          act r = c_test(ctx, right);
          if (lcases == 2 && iadd(get_high(cases, 1), ctx.off) == 1) return make_if_bool(ctx.arg, r, l);
          return make_if_nonzero(ctx.arg, r, l);
        }
        if (less_tests(cright, cleft)) {
          act r = c_test(ctx, right);
          act l = c_test(ctx, left);
          return make_if_lt(ctx.arg, iadd(lim, ctx.off), l, r);
        }
        act l = c_test(ctx, left);
        act r = c_test(ctx, right);
        return make_if_ge(ctx.arg, iadd(lim, ctx.off), r, l);
      }
    }
    throw std::logic_error("Switch.c_test");
  }

  static constexpr double theta = 0.33333;
  static constexpr long switch_min = 3;

  static bool particular_case(const Cases& cases, long i, long j) {
    if (!(j - i == 2)) return false;
    auto [l1, h1, act1] = cases[i];
    auto [l2, h2, act2] = cases[i + 1];
    auto [l3, h3, act3] = cases[i + 2];
    (void)h1;
    (void)h2;
    (void)act2;
    return iadd(l1, 1) == l2 && iadd(l2, 1) == l3 && l3 == h3 && act1 != act3;
  }

  // Approximation of the test sequence height, used to determine cluster
  // density.
  static long approx_count(const Cases& cases, long i, long j) {
    long l = j - i + 1;
    if (l < small_size_limit) return opt_count(sub(cases, i, l)).second.second.n;
    return l - 1;
  }

  static bool dense(const Cases& cases, long i, long j) {
    if (i == j) return true;
    long l = cases[i].low;
    long h = cases[j].high;
    long ntests = approx_count(cases, i, j);
    return particular_case(cases, i, j) ||
           (ntests >= switch_min &&
            static_cast<double>(ntests) + 1.0 >=
                theta * (static_cast<double>(h) - static_cast<double>(l) + 1.0));
  }

  static std::pair<long, std::vector<long>> comp_clusters(const Cases& cases) {
    long len = static_cast<long>(cases.size());
    std::vector<long> min_clusters(len, ocaml_max_int), k(len, 0);
    auto get_min = [&](long i) { return i < 0 ? 0 : min_clusters[i]; };
    for (long i = 0; i <= len - 1; ++i)
      for (long j = 0; j <= i; ++j)
        if (dense(cases, j, i) && iadd(get_min(j - 1), 1) < min_clusters[i]) {
          k[i] = j;
          min_clusters[i] = iadd(get_min(j - 1), 1);
        }
    return {min_clusters[len - 1], k};
  }

  // The code to generate a dense switch is provided by Arg.make_switch
  static Fn make_switch(const loc_t& loc, const Inter<act>& s, long i, long j) {
    const Cases& cases = s.cases;
    const std::vector<act>& actions = *s.actions;
    long ll = cases[i].low;
    long hh = cases[j].high;
    auto tbl = std::make_shared<std::vector<long>>(static_cast<std::size_t>(hh - ll + 1), 0);
    std::map<long, long> t;  // Hashtbl act -> index
    long index = 0;
    auto get_index = [&](long act) {
      if (auto it = t.find(act); it != t.end()) return it->second;
      long i = index;
      ++index;
      t[act] = i;
      return i;
    };
    for (long k = i; k <= j; ++k) {
      auto [l, h, act] = cases[k];
      long idx = get_index(act);
      for (long kk = l - ll; kk <= h - ll; ++kk) (*tbl)[kk] = idx;
    }
    auto acts = std::make_shared<std::vector<act>>(static_cast<std::size_t>(index), actions[0]);
    for (auto& [act, i] : t) (*acts)[i] = actions[act];
    return [loc, tbl, acts, ll](const TCtx& ctx) -> act {
      long off = isub(ineg(ll), ctx.off);
      if (off == 0) return Arg::make_switch(loc, ctx.arg, *tbl, *acts);
      return Arg::bind(Arg::make_offset(ctx.arg, off),
                       [&](arg_t arg) { return Arg::make_switch(loc, arg, *tbl, *acts); });
    };
  }

  static Inter<Fn> make_clusters(const loc_t& loc, const Inter<act>& s, long n_clusters, const std::vector<long>& k) {
    const Cases& cases = s.cases;
    auto actions = s.actions;
    long len = static_cast<long>(cases.size());
    Cases r(static_cast<std::size_t>(n_clusters), Case{0, 0, 0});
    std::map<long, std::pair<long, Fn>> t;  // Hashtbl
    long index = 0;
    long bidon = static_cast<long>(actions->size());
    auto get_index = [&](long act) {
      if (auto it = t.find(act); it != t.end()) return it->second.first;
      long i = index;
      ++index;
      t[act] = {i, [actions, act](const TCtx&) { return (*actions)[static_cast<std::size_t>(act)]; }};
      return i;
    };
    auto add_index = [&](Fn act) {
      long i = index;
      ++index;
      ++bidon;
      t[bidon] = {i, std::move(act)};
      return i;
    };
    long j = len - 1, ir = n_clusters - 1;
    for (;;) {
      long i = k[j];
      if (i == j) {
        auto [l, h, act] = cases[i];
        r[ir] = {l, h, get_index(act)};
      } else {
        long l = cases[i].low;
        long h = cases[j].high;
        r[ir] = {l, h, add_index(make_switch(loc, s, i, j))};
      }
      if (i > 0) {
        j = i - 1;
        ir = ir - 1;
      } else
        break;
    }
    std::vector<Fn> acts(static_cast<std::size_t>(index));
    for (auto& [_, e] : t) acts[e.first] = e.second;
    return {r, std::make_shared<const std::vector<Fn>>(std::move(acts))};
  }

  static act do_zyva(const loc_t& loc, std::pair<long, long> lh, arg_t arg, const Cases& cases,
                     std::vector<act> actions) {
    bool old_ok = ok_inter;
    // both [inter_limit] and [high] are strictly greater than [min_int]
    ok_inter = lh.first >= -inter_limit && lh.first <= inter_limit && iabs(lh.second) <= inter_limit;
    if (ok_inter != old_ok) t.clear();
    Inter<act> s{cases, std::make_shared<const std::vector<act>>(std::move(actions))};
    auto [n_clusters, k] = comp_clusters(s.cases);
    Inter<Fn> clusters = make_clusters(loc, s, n_clusters, k);
    return c_test(TCtx{0, arg}, clusters);
  }

  static std::pair<std::function<act(act)>, std::vector<act>> abstract_shared(const std::vector<Shared<act>>& actions) {
    std::function<act(act)> handlers = [](act x) { return x; };
    std::vector<act> out;
    for (auto& a : actions) {  // Array.map: from the first element
      if (!a.shared) {
        out.push_back(a.act);
        continue;
      }
      auto [i, h] = Arg::make_catch(a.act);
      auto oh = handlers;
      handlers = [h, oh](act x) { return h(oh(x)); };
      out.push_back(Arg::make_exit(i));
    }
    return {handlers, out};
  }

  template <class St>
  static act zyva(const loc_t& loc, std::pair<long, long> lh, arg_t arg, const Cases& cases, St& actions) {
    if (!(cases.size() > 0)) throw std::logic_error("Switch.zyva");
    auto shared = actions.act_get_shared();
    auto [hs, acts] = abstract_shared(shared);
    return hs(do_zyva(loc, lh, arg, cases, std::move(acts)));
  }

  template <class St>
  static act test_sequence(arg_t arg, const Cases& cases, St& actions) {
    if (!(cases.size() > 0)) throw std::logic_error("Switch.test_sequence");
    auto shared = actions.act_get_shared();
    auto [hs, acts] = abstract_shared(shared);
    bool old_ok = ok_inter;
    ok_inter = false;
    if (ok_inter != old_ok) t.clear();
    std::vector<Fn> fns;
    for (auto& a : acts) fns.push_back([a](const TCtx&) { return a; });
    Inter<Fn> s{cases, std::make_shared<const std::vector<Fn>>(std::move(fns))};
    return hs(c_test(TCtx{0, arg}, s));
  }
};

}  // namespace cppcaml::typing::switch_
