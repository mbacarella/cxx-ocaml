// See signature_matching.hpp.  Stable_matching is ported here (its only
// user), with stdlib's Array.sort (a heap sort: its order of equal elements
// is part of the result) and String.edit_distance.
#include "cppcaml/typing/signature_matching.hpp"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <string>

namespace cppcaml::typing::signature_matching {

namespace {

// ---- String.edit_distance ?limit s0 s1 ----------------------------------------------

std::vector<std::uint32_t> uchars(std::string_view s) {
  std::vector<std::uint32_t> r;
  std::size_t i = 0;
  while (i < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
      r.push_back(0xFFFD);  // Uchar.rep
      ++i;
      continue;
    }
    std::uint32_t u = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    bool ok = true;
    for (std::size_t k = 1; k < len; ++k) {
      unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc >> 6) != 2) ok = false;
      u = (u << 6) | (cc & 0x3F);
    }
    if (!ok) {
      r.push_back(0xFFFD);
      ++i;
      continue;
    }
    r.push_back(u);
    i += len;
  }
  return r;
}

long string_edit_distance(std::string_view s, std::string_view s1str, long limit) {
  if (limit <= 1) return s == s1str ? 0 : limit;
  std::vector<std::uint32_t> s0 = uchars(s), s1 = uchars(s1str);
  long len0 = static_cast<long>(s0.size()), len1 = static_cast<long>(s1.size());
  limit = std::min(std::max(len0, len1), limit);
  if (std::labs(len1 - len0) >= limit) return limit;
  if (!(len0 > len1)) {
    std::swap(s0, s1);
    std::swap(len0, len1);
  }
  long ignore = limit + 1;
  std::vector<long> row_minus2(len1 + 1, ignore), row_minus1(len1 + 1), row(len1 + 1, ignore);
  for (long x = 0; x <= len1; ++x) row_minus1[x] = x;
  long d = -1;
  for (long i = 1;; ++i) {
    if (i > len0) {
      d = row_minus1[len1];
      break;
    }
    long row_min = LONG_MAX;
    row[0] = i;
    long jmax = std::min(len1, i + limit - 1);
    if (jmax < 0) jmax = len1;
    for (long j = std::max(1L, i - limit); j <= jmax; ++j) {
      long cost = s0[i - 1] == s1[j - 1] ? 0 : 1;
      long m = std::min(row_minus1[j - 1] + cost, std::min(row_minus1[j] + 1, row[j - 1] + 1));
      if (i > 1 && j > 1 && s0[i - 1] == s1[j - 2] && s0[i - 2] == s1[j - 1]) m = std::min(m, row_minus2[j - 2] + cost);
      row[j] = m;
      row_min = std::min(row_min, m);
    }
    if (row_min >= limit) {
      d = limit;
      break;
    }
    // loop row_minus1 row row_minus2
    std::vector<long> tmp = std::move(row_minus2);
    row_minus2 = std::move(row_minus1);
    row_minus1 = std::move(row);
    row = std::move(tmp);
  }
  return d > limit ? limit : d;
}

// ---- Array.sort (stdlib array.ml's heap sort) --------------------------------------

template <class T, class Cmp>
void ocaml_array_sort(std::vector<T>& a, Cmp cmp) {
  long l = static_cast<long>(a.size());
  struct Bottom {
    long i;
  };
  auto maxson = [&](long l, long i) -> long {
    long i31 = i + i + i + 1;
    long x = i31;
    if (i31 + 2 < l) {
      if (cmp(a[i31], a[i31 + 1]) < 0) x = i31 + 1;
      if (cmp(a[x], a[i31 + 2]) < 0) x = i31 + 2;
      return x;
    }
    if (i31 + 1 < l && cmp(a[i31], a[i31 + 1]) < 0) return i31 + 1;
    if (i31 < l) return i31;
    throw Bottom{i};
  };
  auto trickle = [&](long l, long i, T e) {
    try {
      for (;;) {
        long j = maxson(l, i);
        if (cmp(a[j], e) > 0) {
          a[i] = a[j];
          i = j;
        } else {
          a[i] = e;
          return;
        }
      }
    } catch (const Bottom& b) {
      a[b.i] = e;
    }
  };
  auto bubble = [&](long l, long i) -> long {
    try {
      for (;;) {
        long j = maxson(l, i);
        a[i] = a[j];
        i = j;
      }
    } catch (const Bottom& b) {
      return b.i;
    }
  };
  auto trickleup = [&](long i, T e) {
    for (;;) {
      long father = (i - 1) / 3;
      if (cmp(a[father], e) < 0) {
        a[i] = a[father];
        if (father > 0) {
          i = father;
          continue;
        }
        a[0] = e;
        return;
      }
      a[i] = e;
      return;
    }
  };
  for (long i = (l + 1) / 3 - 1; i >= 0; --i) trickle(l, i, a[i]);
  for (long i = l - 1; i >= 2; --i) {
    T e = a[i];
    a[i] = a[0];
    trickleup(bubble(i, 0), e);
  }
  if (l > 1) {
    T e = a[1];
    a[1] = a[0];
    a[0] = e;
  }
}

// ---- Stable_matching -------------------------------------------------------------------

struct Layer {
  std::vector<long> left_candidates;  // an OCaml list, head first
  long pref;
};

// group_by: the sorted (index, distance) array in layers of equal distance
std::vector<Layer> group_by(const std::vector<std::pair<long, long>>& a) {
  std::vector<Layer> out;
  long current = 0;
  std::vector<long> acc;
  for (const auto& [x, dist] : a) {
    if (dist == current) {
      acc.insert(acc.begin(), x);
    } else if (acc.empty()) {
      current = dist;
      acc = {x};
    } else {
      out.push_back({acc, current});
      current = dist;
      acc = {x};
    }
  }
  if (!acc.empty()) out.push_back({acc, current});
  return out;
}

struct TieList {  // First_round {front; second_round} | Second_round of int list
  bool first_round = true;
  std::vector<long> front, second_round;  // head first
  std::vector<long> second;               // Second_round
};
TieList first_round(std::vector<long> front, std::vector<long> second_round) {
  return TieList{true, std::move(front), std::move(second_round), {}};
}
std::optional<std::pair<long, TieList>> tl_next(const TieList& tl) {
  if (!tl.first_round) {
    if (tl.second.empty()) return std::nullopt;
    TieList r = tl;
    long a = r.second.front();
    r.second.erase(r.second.begin());
    return std::make_pair(a, r);
  }
  if (!tl.front.empty()) {
    TieList r = tl;
    long a = r.front.front();
    r.front.erase(r.front.begin());
    return std::make_pair(a, r);
  }
  std::vector<long> rev(tl.second_round.rbegin(), tl.second_round.rend());
  if (rev.empty()) return std::nullopt;
  long a = rev.front();
  rev.erase(rev.begin());
  TieList r;
  r.first_round = false;
  r.second = rev;
  return std::make_pair(a, r);
}
TieList delay_to_second_round(TieList tl, long x) {
  if (tl.first_round) tl.second_round.insert(tl.second_round.begin(), x);
  return tl;
}
TieList replace_front(long x, TieList tl) {
  if (tl.first_round)
    tl.front.insert(tl.front.begin(), x);
  else
    tl.second.insert(tl.second.begin(), x);
  return tl;
}

struct LeftState {  // Left_unpaired | Left_paired of int * distance
  bool paired = false;
  long i = 0, d = 0;
};
enum class Phase { First, Second };
struct ActiveRight {
  std::vector<Layer> previous_layers;  // head first
  TieList current_layer;
  long current_distance;
  bool paired;
  Phase phase;
  std::vector<Layer> next_layers;  // a sequence
  std::size_t next_pos = 0;
};
struct MState {
  std::vector<LeftState> left;
  std::vector<std::shared_ptr<ActiveRight>> right;  // null = None
  std::vector<long> reactivated;                    // head first
};

using Compatible = std::function<bool(long, long)>;

bool is_never_paired(const MState& st, long j) { return !st.left[j].paired; }

bool has_alternative_choices(const Compatible& compatible, MState& st, long ir, ActiveRight& r) {
  for (;;) {
    TieList& cl = r.current_layer;
    if (!cl.first_round || cl.front.size() < 2) return false;
    long a = cl.front[0], b = cl.front[1];
    std::vector<long> q(cl.front.begin() + 2, cl.front.end());
    if (!compatible(b, ir)) {
      std::vector<long> f{a};
      f.insert(f.end(), q.begin(), q.end());
      r.current_layer = first_round(f, cl.second_round);
      continue;
    }
    if (is_never_paired(st, b)) return true;
    std::vector<long> f{a};
    f.insert(f.end(), q.begin(), q.end());
    std::vector<long> sr{b};
    sr.insert(sr.end(), cl.second_round.begin(), cl.second_round.end());
    r.current_layer = first_round(f, sr);
  }
}

std::pair<long, TieList> skip_paired(const MState& st, TieList dq) {
  for (;;) {
    auto n = tl_next(dq);
    if (!n) throw std::logic_error("Stable_matching.skip_paired");
    auto [first, others] = *n;
    if (is_never_paired(st, first)) return {first, others};
    dq = delay_to_second_round(others, first);
  }
}

bool has_weak_pair(const Compatible& compatible, MState& st, long j) {
  if (!st.left[j].paired) return false;
  long i = st.left[j].i;
  if (!st.right[i]) throw std::logic_error("Stable_matching.has_weak_pair");
  return has_alternative_choices(compatible, st, i, *st.right[i]);
}

std::optional<Phase> phase(const MState& st, long i) {
  if (!st.right[i]) return std::nullopt;
  return st.right[i]->phase;
}

void prepare_tie_list(MState& st, const Layer& layer, long r) {
  std::vector<long> first, later;
  for (long i : layer.left_candidates) (is_never_paired(st, i) ? first : later).push_back(i);
  TieList tl = first_round(first, later);
  if (!st.right[r]) return;
  st.right[r]->current_distance = layer.pref;
  st.right[r]->current_layer = tl;
}

void second_phase(MState& st, long ir, ActiveRight& r) {
  std::vector<Layer> layers(r.previous_layers.rbegin(), r.previous_layers.rend());
  r.previous_layers.clear();
  r.phase = Phase::Second;
  if (layers.empty()) throw std::logic_error("Stable_matching.second_phase");
  prepare_tie_list(st, layers[0], ir);
  r.next_layers.assign(layers.begin() + 1, layers.end());
  r.next_pos = 0;
}

bool next_layer(MState& st, long ir, ActiveRight& r) {
  if (r.next_pos >= r.next_layers.size()) {
    if (r.phase == Phase::First) {
      second_phase(st, ir, r);
      return true;
    }
    return false;
  }
  Layer layer = r.next_layers[r.next_pos++];
  r.previous_layers.insert(r.previous_layers.begin(), layer);
  prepare_tie_list(st, layer, ir);
  return true;
}

std::optional<long> get_left_candidate(const Compatible& compatible, MState& st, long ir, ActiveRight& r) {
  for (;;) {
    if (has_alternative_choices(compatible, st, ir, r)) {
      auto [f, others] = skip_paired(st, r.current_layer);
      r.current_layer = others;
      return f;
    }
    if (auto n = tl_next(r.current_layer)) {
      r.current_layer = n->second;
      return n->first;
    }
    if (!next_layer(st, ir, r)) return std::nullopt;
  }
}

std::optional<long> get_compatible_left_candidate(const Compatible& compatible, MState& st, long ir,
                                                  ActiveRight& r) {
  for (;;) {
    std::optional<long> c = get_left_candidate(compatible, st, ir, r);
    if (!c) return std::nullopt;
    if (compatible(*c, ir)) return c;
  }
}

void reject(MState& st, long i) {
  if (!st.right[i]) return;
  ActiveRight& right = *st.right[i];
  right.paired = false;
  auto n = tl_next(right.current_layer);
  if (!n) {
    st.right[i] = nullptr;
    return;
  }
  right.current_layer = delay_to_second_round(n->second, n->first);
  st.reactivated.insert(st.reactivated.begin(), i);
}

bool accepted_proposal(const Compatible& compatible, MState& st, long i, long j, long d) {
  if (has_weak_pair(compatible, st, j)) return true;
  if (!st.left[j].paired) return true;
  long i2 = st.left[j].i, d2 = st.left[j].d;
  if (d < d2) return true;
  if (d != d2) return false;
  return phase(st, i) == Phase::Second && phase(st, i2) == Phase::First;
}

void pair(MState& st, long i, long j, long d) {
  if (st.right[i]) {
    st.right[i]->paired = true;
    st.right[i]->current_layer = replace_front(j, st.right[i]->current_layer);
  }
  if (st.left[j].paired) reject(st, st.left[j].i);
  st.left[j] = LeftState{true, i, d};
}

struct Matches {
  std::vector<long> left;
  std::vector<std::pair<long, long>> pairs;
  std::vector<long> right;
};

Matches matches(const Compatible& compatible, const std::function<std::vector<Layer>(long)>& preferences, long lsize,
                long rsize) {
  MState st;
  st.left.assign(lsize, LeftState{});
  for (long r = 0; r < rsize; ++r) {
    std::vector<Layer> seq = preferences(r);
    if (seq.empty()) {
      st.right.push_back(nullptr);
      continue;
    }
    auto ar = std::make_shared<ActiveRight>();
    ar->paired = false;
    ar->phase = Phase::First;
    ar->current_distance = seq[0].pref;
    ar->current_layer = first_round(seq[0].left_candidates, {});
    ar->previous_layers = {seq[0]};
    ar->next_layers.assign(seq.begin() + 1, seq.end());
    st.right.push_back(ar);
  }
  std::vector<long> todo;
  for (long i = 0; i < rsize; ++i) todo.push_back(i);
  for (;;) {
    for (long i : todo) {
      std::shared_ptr<ActiveRight> right = st.right[i];
      if (!right) continue;
      // proposals
      for (;;) {
        std::optional<long> j = get_compatible_left_candidate(compatible, st, i, *right);
        if (!j) break;
        if (accepted_proposal(compatible, st, i, *j, right->current_distance)) {
          pair(st, i, *j, right->current_distance);
          break;
        }
      }
    }
    if (st.reactivated.empty()) break;
    todo = st.reactivated;
    st.reactivated.clear();
  }
  Matches m;
  for (long l = 0; l < lsize; ++l) {
    if (st.left[l].paired)
      m.pairs.push_back({l, st.left[l].i});
    else
      m.left.push_back(l);
  }
  for (long r = 0; r < rsize; ++r)
    if (!st.right[r] || !st.right[r]->paired) m.right.push_back(r);
  return m;
}

struct Item {  // (signature_item, signature_item) Item.t: name, item, kind (= item)
  std::string name;
  const SignatureItem* item;
};
struct ItemMatches {  // (Item.t, signature_item) matches
  std::vector<Item> left;
  std::vector<std::pair<const SignatureItem*, const SignatureItem*>> pairs;
  std::vector<Item> right;
};

long cutoff(const std::string& name) {
  long n = static_cast<long>(name.size());
  if (n <= 1) return 0;
  if (n <= 4) return 1;
  if (n <= 8) return 2;
  if (n <= 11) return 3;
  return n / 4;
}

constexpr long max_right_items = 20;

using Compat = std::function<bool(const SignatureItem*, const SignatureItem*)>;

ItemMatches fuzzy_match_names(const Compat& compatibility, const std::vector<Item>& left0,
                              const std::vector<Item>& right0) {
  std::vector<Item> right(right0.begin(), right0.begin() + std::min<long>(max_right_items, right0.size()));
  std::vector<Item> right_rest(right0.begin() + right.size(), right0.end());
  const std::vector<Item>& left = left0;
  Compatible compatible = [&](long i, long j) { return compatibility(left[i].item, right[j].item); };
  auto preferences = [&](long r) {
    const std::string& name = right[r].name;
    long co = cutoff(name);
    std::vector<std::pair<long, long>> a;
    for (long i = 0; i < static_cast<long>(left.size()); ++i) {
      long d = string_edit_distance(name, left[i].name, 1 + co);
      if (d <= co) a.push_back({i, d});
    }
    ocaml_array_sort(a, [](const std::pair<long, long>& x, const std::pair<long, long>& y) {
      return x.second < y.second ? -1 : x.second > y.second ? 1 : 0;
    });
    return group_by(a);
  };
  Matches m = matches(compatible, preferences, static_cast<long>(left.size()), static_cast<long>(right.size()));
  ItemMatches r;
  for (long l : m.left) r.left.push_back(left[l]);
  for (long x : m.right) r.right.push_back(right[x]);
  for (auto& [l, x] : m.pairs) r.pairs.push_back({left[l].item, right[x].item});
  r.right.insert(r.right.end(), right_rest.begin(), right_rest.end());
  return r;
}

// ---- Signature_matching ----

enum KindIx { Module_types, Modules, Types, Class_types, Values, Classes, Extensions, N_kinds };

KindIx kind_of(const SignatureItem* it) {
  using K = SignatureItem::Kind;
  switch (it->kind) {
    case K::Sig_module: return Modules;
    case K::Sig_type: return Types;
    case K::Sig_modtype: return Module_types;
    case K::Sig_class_type: return Class_types;
    case K::Sig_value: return Values;
    case K::Sig_class: return Classes;
    case K::Sig_typext: return Extensions;
  }
  return Values;
}

using Check = std::function<bool(env::t, subst::t, const SignatureItem*, const SignatureItem*)>;
Check check_of(KindIx k) {
  namespace C = includemod::check;
  switch (k) {
    case Module_types:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::module_types(e, s, a->mtd, b->mtd);
      };
    case Modules:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::modules(e, s, a->md, b->md);
      };
    case Types:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::types(e, s, a->type, b->type);
      };
    case Class_types:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::class_types(e, s, a->clty, b->clty);
      };
    case Values:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::values(e, s, a->value, b->value);
      };
    case Classes:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::classes(e, s, a->cls, b->cls);
      };
    default:
      return [](env::t e, subst::t s, const SignatureItem* a, const SignatureItem* b) {
        return C::extensions(e, s, a->ext, b->ext);
      };
  }
}

Location item_loc(const SignatureItem* it) {
  using K = SignatureItem::Kind;
  switch (it->kind) {
    case K::Sig_value: return it->value->val_loc;
    case K::Sig_type: return it->type->type_loc;
    case K::Sig_typext: return it->ext->ext_loc;
    case K::Sig_module: return it->md->md_loc;
    case K::Sig_modtype: return it->mtd->mtd_loc;
    case K::Sig_class: return it->cls->cty_loc;
    case K::Sig_class_type: return it->clty->clty_loc;
  }
  return location::none();
}

}  // namespace

Report suggest(const includemod::error::SignatureSymptom& sgs) {
  // init: classify conses, so each kind's list is reversed
  ItemMatches map[N_kinds];
  auto mk = [](const SignatureItem* it) { return Item{std::string(ident::name(types::signature_item_id(it))), it}; };
  for (const SignatureItem* it : sgs.additions) {
    auto& l = map[kind_of(it)].left;
    l.insert(l.begin(), mk(it));
  }
  for (auto it = sgs.missings.rbegin(); it != sgs.missings.rend(); ++it) {
    auto& r = map[kind_of(*it)].right;
    r.insert(r.begin(), mk(*it));
  }
  env::t env = sgs.env;
  subst::t subst = sgs.subst;
  // iterate env subst 6: the static kinds, while progress
  for (int lim = 6;; --lim) {
    bool progress = false;
    for (KindIx k : {Module_types, Modules, Types}) {
      Check c = check_of(k);
      ItemMatches& current = map[k];
      ItemMatches m = fuzzy_match_names(
          [&](const SignatureItem* a, const SignatureItem* b) { return c(env, subst, a, b); }, current.left,
          current.right);
      if (m.pairs.empty()) continue;
      progress = true;
      for (auto& [l, r] : m.pairs) subst = includemod::item_subst(types::signature_item_id(l), r, subst);
      m.pairs.insert(m.pairs.end(), current.pairs.begin(), current.pairs.end());
      current = m;
    }
    if (!(progress && lim > 0)) break;
  }
  // value_suggestions: the dynamic kinds, once
  for (KindIx k : {Values, Classes, Class_types, Extensions}) {
    Check c = check_of(k);
    map[k] = fuzzy_match_names([&](const SignatureItem* a, const SignatureItem* b) { return c(env, subst, a, b); },
                               map[k].left, map[k].right);
  }
  Report rep;
  for (auto& [it, sy] : sgs.incompatibles) rep.incompatibles.push_back({it, sy});
  // [] |> collect module_types |> modules |> types |> class_types |> classes
  // |> values |> extensions: each prepends
  for (KindIx k : {Module_types, Modules, Types, Class_types, Classes, Values, Extensions}) {
    std::vector<Suggestion<Alteration>> add;
    for (auto& [l, r] : map[k].pairs) add.push_back({r, Alteration{false, types::signature_item_id(l), item_loc(l)}});
    for (const Item& x : map[k].right) add.push_back({x.item, Alteration{true}});
    rep.alterations.insert(rep.alterations.begin(), add.begin(), add.end());
  }
  return rep;
}

}  // namespace cppcaml::typing::signature_matching
