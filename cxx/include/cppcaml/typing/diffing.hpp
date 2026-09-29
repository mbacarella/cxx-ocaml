// Port of utils/diffing.ml and utils/diffing_with_keys.ml (cxx/PORTING.md
// stage 9): the Wagner-Fischer style patch computation behind the error
// messages of record / variant definition mismatches and functor
// applications, with states that may extend either list while diffing,
// and Diffing_with_keys' refinement of a patch into swaps and moves.
#pragma once

#include <algorithm>
#include <climits>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/format_doc.hpp"

namespace cppcaml::typing::diffing {

enum class ChangeKind { Deletion, Insertion, Modification, Preservation };

// the semantic tag of Misc.Style.Style (style k): "@style:<ANSI codes>"
// (misc::style::mark_open_tag; no markup with colours off)
inline std::string style_tag(ChangeKind k) {
  switch (k) {
    case ChangeKind::Preservation: return "@style:32";                // [FG Green]
    case ChangeKind::Deletion: case ChangeKind::Insertion: return "@style:31;1";  // [FG Red; Bold]
    case ChangeKind::Modification: return "@style:35;1";              // [FG Magenta; Bold]
  }
  return "@style:0";
}

// Diffing.prefix ppf (pos, p)
inline void prefix(format_doc::Formatter& ppf, long pos, ChangeKind p) {
  format_doc::pp_open_stag(ppf, style_tag(p));
  format_doc::fprintf(ppf, "%i. ", pos);
  format_doc::pp_close_stag(ppf);
}

// ('left, 'right, 'eq, 'diff) change
template <class L, class R, class Eq, class D>
struct Change {
  enum class K : std::uint8_t { Delete, Insert, Keep, Change };
  K k;
  L left{};   // Delete / Keep / Change
  R right{};  // Insert / Keep / Change
  Eq eq{};    // Keep
  D diff{};   // Change
};

template <class Eq, class D>
struct TestResult {  // (eq, diff) result
  bool ok;
  Eq eq{};
  D err{};
};

// Diffing.Define(D) ... Generic: a state that may extend the line (left)
// or the column (right) list
template <class L, class R, class Eq, class D, class State>
struct Define {
  using change = Change<L, R, Eq, D>;
  using patch = std::vector<change>;

  struct FullState {
    std::vector<L> line;
    std::vector<R> column;
    State state;
  };

  struct Core {
    std::function<long(const change&)> weight;
    std::function<TestResult<Eq, D>(const State&, const L&, const R&)> test;
    std::function<FullState(const change&, const FullState&)> update;
  };

 private:
  struct Matrix {
    long lines = 0, columns = 0;
    std::vector<std::vector<std::optional<FullState>>> states;
    std::vector<std::vector<long>> weights;
    std::vector<std::vector<std::optional<change>>> diffs;

    static Matrix make(long l, long c) {
      Matrix m;
      m.lines = l;
      m.columns = c;
      m.states.assign(l + 1, std::vector<std::optional<FullState>>(c + 1));
      m.weights.assign(l + 1, std::vector<long>(c + 1, LONG_MAX));
      m.diffs.assign(l + 1, std::vector<std::optional<change>>(c + 1));
      return m;
    }
    Matrix reshape(long l, long c) const {
      Matrix n = make(l, c);
      for (long i = 0; i <= l; ++i)
        for (long j = 0; j <= c; ++j)
          if (i <= lines && j <= columns) {
            n.states[i][j] = states[i][j];
            n.weights[i][j] = weights[i][j];
            n.diffs[i][j] = diffs[i][j];
          }
      return n;
    }
    std::optional<L> line(long i, long j) const {
      const auto& st = states[i][j];
      if (!st || i >= static_cast<long>(st->line.size())) return std::nullopt;
      return st->line[i];
    }
    std::optional<R> column(long i, long j) const {
      const auto& st = states[i][j];
      if (!st || j >= static_cast<long>(st->column.size())) return std::nullopt;
      return st->column[j];
    }
    void set(long i, long j, std::optional<change> diff, long weight, FullState state) {
      weights[i][j] = weight;
      states[i][j] = std::move(state);
      diffs[i][j] = std::move(diff);
    }
    std::optional<std::pair<long, long>> shape_at(long i, long j) const {
      const auto& st = states[i][j];
      if (!st) return std::nullopt;
      return std::make_pair(static_cast<long>(st->line.size()), static_cast<long>(st->column.size()));
    }
    std::pair<long, long> real_shape() const {
      long l = lines, c = columns;
      for (long i = 0; i <= lines; ++i)
        for (long j = 0; j <= columns; ++j)
          if (auto s = shape_at(i, j)) {
            l = std::max(l, s->first);
            c = std::max(c, s->second);
          }
      return {l, c};
    }
  };

  static void compute_column0(const Core& X, Matrix& tbl, long i) {
    const auto& st = tbl.states[i - 1][0];
    if (!st) return;
    std::optional<L> line = tbl.line(i - 1, 0);
    if (!line) return;
    change diff{change::K::Delete, *line};
    long w = X.weight(diff) + tbl.weights[i - 1][0];
    tbl.set(i, 0, diff, w, X.update(diff, *st));
  }
  static void compute_line0(const Core& X, Matrix& tbl, long j) {
    const auto& st = tbl.states[0][j - 1];
    if (!st) return;
    std::optional<R> column = tbl.column(0, j - 1);
    if (!column) return;
    change diff{change::K::Insert};
    diff.right = *column;
    long w = X.weight(diff) + tbl.weights[0][j - 1];
    tbl.set(0, j, diff, w, X.update(diff, *st));
  }
  struct Proposition {
    long weight;
    change diff;
    FullState state;
  };
  static void compute_inner_cell(const Core& X, Matrix& tbl, long i, long j) {
    auto compute_proposition = [&](long ii, long jj, std::optional<change> diff) -> std::optional<Proposition> {
      if (!diff) return std::nullopt;
      const auto& localstate = tbl.states[ii][jj];
      if (!localstate) return std::nullopt;
      return Proposition{X.weight(*diff) + tbl.weights[ii][jj], *diff, *localstate};
    };
    std::optional<Proposition> del, insert, diag;
    {
      std::optional<change> diff;
      if (std::optional<L> x = tbl.line(i - 1, j)) diff = change{change::K::Delete, *x};
      del = compute_proposition(i - 1, j, diff);
    }
    {
      std::optional<change> diff;
      if (std::optional<R> x = tbl.column(i, j - 1)) {
        change c{change::K::Insert};
        c.right = *x;
        diff = c;
      }
      insert = compute_proposition(i, j - 1, diff);
    }
    {
      std::optional<change> diff;
      const auto& state = tbl.states[i - 1][j - 1];
      if (state) {
        std::optional<L> line = tbl.line(i - 1, j - 1);
        if (line) {
          std::optional<R> column = tbl.column(i - 1, j - 1);
          if (column) {
            TestResult<Eq, D> r = X.test(state->state, *line, *column);
            change c{r.ok ? change::K::Keep : change::K::Change, *line};
            c.right = *column;
            if (r.ok)
              c.eq = r.eq;
            else
              c.diff = r.err;
            diff = c;
          }
        }
      }
      diag = compute_proposition(i - 1, j - 1, diff);
    }
    // select_best_proposition [del; insert; diag]: the first of minimal weight
    std::optional<Proposition> best;
    for (auto* p : {&del, &insert, &diag}) {
      if (!*p) continue;
      if (!best || (*p)->weight < best->weight) best = **p;
    }
    if (!best) return;
    FullState state = X.update(best->diff, best->state);
    tbl.set(i, j, best->diff, best->weight, std::move(state));
  }
  static void compute_cell(const Core& X, Matrix& m, long i, long j) {
    if (m.diffs[i][j]) return;
    if (i == 0 && j == 0) return;
    if (i == 0)
      compute_line0(X, m, j);
    else if (j == 0)
      compute_column0(X, m, i);
    else
      compute_inner_cell(X, m, i, j);
  }
  static Matrix compute_matrix(const Core& X, const FullState& state0) {
    Matrix m = Matrix::make(0, 0);
    m.set(0, 0, std::nullopt, 0, state0);
    while (true) {
      auto [l, c] = m.real_shape();
      if (l > m.lines || c > m.columns) {
        m = m.reshape(l, c);
        for (long i = 0; i <= l; ++i)
          for (long j = 0; j <= c; ++j) compute_cell(X, m, i, j);
      } else {
        return m;
      }
    }
  }
  static std::pair<long, long> select_final_state(const Matrix& m0) {
    long bi = 0, bj = 0, bw = LONG_MAX;
    for (long i = 0; i <= m0.lines; ++i)
      for (long j = 0; j <= m0.columns; ++j) {
        auto s = m0.shape_at(i, j);
        if (s && s->first == i && s->second == j) {
          long w = m0.weights[i][j];
          if (w < bw) {
            bi = i;
            bj = j;
            bw = w;
          }
        }
      }
    return {bi, bj};
  }
  static patch construct_patch(const Matrix& m0) {
    patch acc;
    auto [i, j] = select_final_state(m0);
    while (!(i == 0 && j == 0)) {
      const std::optional<change>& d = m0.diffs[i][j];
      if (!d) throw std::logic_error("Diffing.construct_patch");
      acc.push_back(*d);
      switch (d->k) {
        case change::K::Keep:
        case change::K::Change:
          --i;
          --j;
          break;
        case change::K::Delete: --i; break;
        case change::K::Insert: --j; break;
      }
    }
    std::reverse(acc.begin(), acc.end());
    return acc;
  }

 public:
  // Generic(X).compute_matrix + construct_patch
  static patch diff(const Core& X, FullState init) { return construct_patch(compute_matrix(X, init)); }

  // Simple: the update only changes the state
  static patch simple(const std::function<long(const change&)>& weight,
                      const std::function<TestResult<Eq, D>(const State&, const L&, const R&)>& test,
                      const std::function<State(const change&, const State&)>& update, const State& state,
                      std::vector<L> line, std::vector<R> column) {
    Core X{weight, test, [update](const change& d, const FullState& fs) {
             FullState r = fs;
             r.state = update(d, fs.state);
             return r;
           }};
    return diff(X, FullState{std::move(line), std::move(column), state});
  }
  // Left_variadic / Right_variadic: the update may extend the line / column
  static patch left_variadic(const std::function<long(const change&)>& weight,
                             const std::function<TestResult<Eq, D>(const State&, const L&, const R&)>& test,
                             const std::function<std::pair<State, std::vector<L>>(const change&, const State&)>& update,
                             const State& state, std::vector<L> line, std::vector<R> column) {
    Core X{weight, test, [update](const change& d, const FullState& fs) {
             auto [st, a] = update(d, fs.state);
             FullState r = fs;
             r.state = st;
             r.line.insert(r.line.end(), a.begin(), a.end());
             return r;
           }};
    return diff(X, FullState{std::move(line), std::move(column), state});
  }
  static patch right_variadic(const std::function<long(const change&)>& weight,
                              const std::function<TestResult<Eq, D>(const State&, const L&, const R&)>& test,
                              const std::function<std::pair<State, std::vector<R>>(const change&, const State&)>& update,
                              const State& state, std::vector<L> line, std::vector<R> column) {
    Core X{weight, test, [update](const change& d, const FullState& fs) {
             auto [st, a] = update(d, fs.state);
             FullState r = fs;
             r.state = st;
             r.column.insert(r.column.end(), a.begin(), a.end());
             return r;
           }};
    return diff(X, FullState{std::move(line), std::move(column), state});
  }
};

// ---- Diffing_with_keys ------------------------------------------------------------

template <class A>
struct WithPos {
  long pos;
  A data;
};
template <class A>
std::vector<WithPos<A>> with_pos(const std::vector<A>& l) {
  std::vector<WithPos<A>> r;
  long n = 0;
  for (const A& x : l) r.push_back({++n, x});
  return r;
}

// ('l, 'r, 'diff) mismatch
template <class L, class R, class D>
struct Mismatch {
  enum class K : std::uint8_t { Name, Type };
  K k;
  long pos = 0;
  std::string got_name, expected_name;  // Name
  bool types_match = false;             // Name
  L got{};                              // Type
  R expected{};                         // Type
  D reason{};                           // Type
};

// ('l, 'r, 'diff) change
template <class L, class R, class D>
struct KeyedChange {
  enum class K : std::uint8_t { Change, Swap, Move, Insert, Delete };
  K k;
  Mismatch<L, R, D> change{};           // Change
  long pos1 = 0, pos2 = 0;              // Swap pos
  std::string first, last;              // Swap
  std::string name;                     // Move
  long got = 0, expected = 0;           // Move
  long pos = 0;                         // Insert / Delete
  R insert{};                           // Insert
  L del{};                              // Delete
};

// Diffing_with_keys.prefix
template <class L, class R, class D>
void prefix(format_doc::Formatter& ppf, const KeyedChange<L, R, D>& x) {
  using K = typename KeyedChange<L, R, D>::K;
  ChangeKind kind = x.k == K::Insert ? ChangeKind::Insertion
                  : x.k == K::Delete ? ChangeKind::Deletion
                                     : ChangeKind::Modification;
  format_doc::pp_open_stag(ppf, style_tag(kind));
  switch (x.k) {
    case K::Change: format_doc::fprintf(ppf, "%i. ", x.change.pos); break;
    case K::Insert:
    case K::Delete: format_doc::fprintf(ppf, "%i. ", x.pos); break;
    case K::Swap: format_doc::fprintf(ppf, "%i<->%i. ", x.pos1, x.pos2); break;
    case K::Move: format_doc::fprintf(ppf, "%i->%i. ", x.expected, x.got); break;
  }
  format_doc::pp_close_stag(ppf);
}

struct Unit {};

// Diffing_with_keys.Define(D).Simple(Impl).diff
template <class L, class R, class D, class State>
struct KeyedDiff {
  using M = Mismatch<L, R, D>;
  using DD = Define<WithPos<L>, WithPos<R>, Unit, M, State>;
  using change = typename DD::change;
  using composite = KeyedChange<L, R, D>;
  using TR = TestResult<Unit, M>;

  std::function<long(const change&)> weight;
  std::function<TR(const State&, const WithPos<L>&, const WithPos<R>&)> test;
  std::function<State(const change&, const State&)> update;
  std::function<std::string(const L&)> key_left;
  std::function<std::string(const R&)> key_right;

 private:
  // ('l,'r) partial_cycle
  struct Partial {
    enum class K { Left, Right, Both } k;
    long pos = 0;
    State state{};
    WithPos<L> l{};  // Left: x (Change: (x, y) -> the left of the pair)
    WithPos<R> r{};
    // Both (state, l, r): l = (ll, lr), r = (rl, rr) for swaps; got/expected for moves
    WithPos<L> ll{};
    WithPos<R> lr{};
    WithPos<L> rl{};
    WithPos<R> rr{};
    bool pair = false;  // a Change edge (x, y) rather than an Insert / Delete
  };

  static std::optional<Partial> merge_edge(const Partial& ex, const std::optional<Partial>& ey) {
    if (!ey) return ex;
    if (ex.k == Partial::K::Left && ey->k == Partial::K::Right) return both(ex, *ey);
    if (ex.k == Partial::K::Right && ey->k == Partial::K::Left) return both(*ey, ex);
    if (ex.k == Partial::K::Both) return ex;
    if (ey->k == Partial::K::Both) return *ey;
    return ex;
  }
  // Left (lpos, lstate, l), Right (rpos, rstate, r) -> Both (state, l, r)
  static Partial both(const Partial& left, const Partial& right) {
    Partial b;
    b.k = Partial::K::Both;
    b.state = left.pos < right.pos ? right.state : left.state;
    b.pair = left.pair;
    // the payloads: a Change edge carries (x, y); an Insert / Delete one x
    b.ll = left.ll;
    b.lr = left.lr;
    b.rl = right.ll;
    b.rr = right.lr;
    b.l = left.l;
    b.r = right.r;
    return b;
  }

 public:
  std::vector<composite> diff(const State& state, const std::vector<L>& left, const std::vector<R>& right) const {
    std::vector<WithPos<L>> l = with_pos(left);
    std::vector<WithPos<R>> r = with_pos(right);
    typename DD::patch raw = DD::simple(weight, [this](const State& s, const WithPos<L>& x, const WithPos<R>& y) {
      return test(s, x, y);
    }, update, state, l, r);
    return refine(state, raw);
  }

 private:
  std::vector<composite> refine(const State& state0, const typename DD::patch& patch) const {
    // two_cycles state patch
    std::map<std::pair<std::string, std::string>, Partial> swaps;
    std::map<std::string, Partial> moves;
    State state = state0;
    for (const change& d : patch) {
      State cur = state;
      state = update(d, state);
      switch (d.k) {
        case change::K::Change: {
          std::string kx = key_left(d.left.data), ky = key_right(d.right.data);
          Partial e;
          e.k = kx <= ky ? Partial::K::Left : Partial::K::Right;
          e.pos = d.left.pos;
          e.state = cur;
          e.pair = true;
          e.ll = d.left;
          e.lr = d.right;
          auto key = kx <= ky ? std::make_pair(kx, ky) : std::make_pair(ky, kx);
          auto it = swaps.find(key);
          std::optional<Partial> old;
          if (it != swaps.end()) old = it->second;
          if (auto m = merge_edge(e, old)) swaps[key] = *m;
          break;
        }
        case change::K::Insert: {
          std::string k = key_right(d.right.data);
          Partial e;
          e.k = Partial::K::Right;
          e.pos = d.right.pos;
          e.state = cur;
          e.r = d.right;
          e.lr = d.right;
          auto it = moves.find(k);
          std::optional<Partial> old;
          if (it != moves.end()) old = it->second;
          if (auto m = merge_edge(e, old)) moves[k] = *m;
          break;
        }
        case change::K::Delete: {
          std::string k = key_left(d.left.data);
          Partial e;
          e.k = Partial::K::Left;
          e.pos = d.left.pos;
          e.state = cur;
          e.l = d.left;
          e.ll = d.left;
          auto it = moves.find(k);
          std::optional<Partial> old;
          if (it != moves.end()) old = it->second;
          if (auto m = merge_edge(e, old)) moves[k] = *m;
          break;
        }
        default: break;
      }
    }
    auto swap = [&](const WithPos<L>& x, const WithPos<R>& y)
        -> std::optional<std::pair<WithPos<std::string>, WithPos<std::string>>> {
      std::string kx = key_left(x.data), ky = key_right(y.data);
      auto key = kx <= ky ? std::make_pair(kx, ky) : std::make_pair(ky, kx);
      auto it = swaps.find(key);
      if (it == swaps.end() || it->second.k != Partial::K::Both) return std::nullopt;
      const Partial& b = it->second;
      // match test state ll rr, test state rl lr with Ok _, Ok _ (a tuple:
      // right to left)
      TR t2 = test(b.state, b.rl, b.lr);
      TR t1 = test(b.state, b.ll, b.rr);
      if (t1.ok && t2.ok) return std::make_pair(WithPos<std::string>{b.ll.pos, kx}, WithPos<std::string>{b.rl.pos, ky});
      return std::nullopt;
    };
    auto move = [&](const std::string& name) -> std::optional<composite> {
      auto it = moves.find(name);
      if (it == moves.end() || it->second.k != Partial::K::Both) return std::nullopt;
      const Partial& b = it->second;
      // Both (state, got, expected): the Left (Delete) side is got
      TR t = test(b.state, b.l, b.r);
      if (!t.ok) return std::nullopt;
      composite c{composite::K::Move};
      c.name = name;
      c.got = b.l.pos;
      c.expected = b.r.pos;
      return c;
    };
    std::vector<composite> out;
    for (const change& d : patch) {
      switch (d.k) {
        case change::K::Keep: break;
        case change::K::Insert: {
          if (auto m = move(key_right(d.right.data))) {
            out.push_back(*m);
          } else {
            composite c{composite::K::Insert};
            c.pos = d.right.pos;
            c.insert = d.right.data;
            out.push_back(c);
          }
          break;
        }
        case change::K::Delete: {
          if (move(key_left(d.left.data))) break;
          composite c{composite::K::Delete};
          c.pos = d.left.pos;
          c.del = d.left.data;
          out.push_back(c);
          break;
        }
        case change::K::Change: {
          if (auto sw = swap(d.left, d.right)) {
            if (d.left.pos == sw->first.pos) {
              composite c{composite::K::Swap};
              c.pos1 = sw->first.pos;
              c.pos2 = sw->second.pos;
              c.first = sw->first.data;
              c.last = sw->second.data;
              out.push_back(c);
            }
          } else {
            composite c{composite::K::Change};
            c.change = d.diff;
            out.push_back(c);
          }
          break;
        }
      }
    }
    return out;
  }
};

}  // namespace cppcaml::typing::diffing
