// stdlib/list.ml's List.sort_uniq, transliterated: which of two equal
// elements survives is the merge's choice, and where the elements are
// objects whose identity reaches marshaled output (Consistbl.extract's
// names), that choice is OCaml's.
#pragma once

#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace cppcaml::typing::list_sort {

// sort_uniq cmp l (cmp: <0 / 0 / >0); the lists are deques, head first
template <class T, class Cmp>
std::vector<T> sort_uniq(const std::vector<T>& l, Cmp cmp) {
  using L = std::deque<T>;
  auto rev_append = [](L& l2, L& accu) {
    while (!l2.empty()) {
      accu.push_front(std::move(l2.front()));
      l2.pop_front();
    }
  };
  // rev_merge l1 l2 accu
  auto rev_merge = [&](L l1, L l2) {
    L accu;
    for (;;) {
      if (l1.empty()) {
        rev_append(l2, accu);
        return accu;
      }
      if (l2.empty()) {
        rev_append(l1, accu);
        return accu;
      }
      int c = cmp(l1.front(), l2.front());
      if (c == 0) {
        accu.push_front(l1.front());
        l1.pop_front();
        l2.pop_front();
      } else if (c < 0) {
        accu.push_front(l1.front());
        l1.pop_front();
      } else {
        accu.push_front(l2.front());
        l2.pop_front();
      }
    }
  };
  // rev_merge_rev l1 l2 accu
  auto rev_merge_rev = [&](L l1, L l2) {
    L accu;
    for (;;) {
      if (l1.empty()) {
        rev_append(l2, accu);
        return accu;
      }
      if (l2.empty()) {
        rev_append(l1, accu);
        return accu;
      }
      int c = cmp(l1.front(), l2.front());
      if (c == 0) {
        accu.push_front(l1.front());
        l1.pop_front();
        l2.pop_front();
      } else if (c > 0) {
        accu.push_front(l1.front());
        l1.pop_front();
      } else {
        accu.push_front(l2.front());
        l2.pop_front();
      }
    }
  };
  // sort n l / rev_sort n l: (the sorted prefix of length n, the rest's position)
  std::function<std::pair<L, std::size_t>(std::size_t, std::size_t, bool)> sort =
      [&](std::size_t n, std::size_t pos, bool rev) -> std::pair<L, std::size_t> {
    // sort: ascending (cmp as is); rev_sort: descending (the comparisons flipped)
    auto lt = [&](const T& a, const T& b) { return rev ? cmp(a, b) > 0 : cmp(a, b) < 0; };
    if (n == 2) {
      const T& x1 = l[pos];
      const T& x2 = l[pos + 1];
      int c = cmp(x1, x2);
      L s;
      if (c == 0) s = {x1};
      else if (lt(x1, x2)) s = {x1, x2};
      else s = {x2, x1};
      return {s, pos + 2};
    }
    if (n == 3) {
      const T& x1 = l[pos];
      const T& x2 = l[pos + 1];
      const T& x3 = l[pos + 2];
      L s;
      int c = cmp(x1, x2);
      if (c == 0) {
        int c2 = cmp(x1, x3);
        if (c2 == 0) s = {x1};
        else if (lt(x1, x3)) s = {x1, x3};
        else s = {x3, x1};
      } else if (lt(x1, x2)) {
        int c2 = cmp(x2, x3);
        if (c2 == 0) s = {x1, x2};
        else if (lt(x2, x3)) s = {x1, x2, x3};
        else {
          int c3 = cmp(x1, x3);
          if (c3 == 0) s = {x1, x2};
          else if (lt(x1, x3)) s = {x1, x3, x2};
          else s = {x3, x1, x2};
        }
      } else {
        int c2 = cmp(x1, x3);
        if (c2 == 0) s = {x2, x1};
        else if (lt(x1, x3)) s = {x2, x1, x3};
        else {
          int c3 = cmp(x2, x3);
          if (c3 == 0) s = {x2, x1};
          else if (lt(x2, x3)) s = {x2, x3, x1};
          else s = {x3, x2, x1};
        }
      }
      return {s, pos + 3};
    }
    std::size_t n1 = n >> 1, n2 = n - n1;
    // sort: rev_sort both halves, rev_merge_rev; rev_sort: sort both, rev_merge
    auto [s1, p2] = sort(n1, pos, !rev);
    auto [s2, p3] = sort(n2, p2, !rev);
    return {rev ? rev_merge(std::move(s1), std::move(s2)) : rev_merge_rev(std::move(s1), std::move(s2)), p3};
  };
  if (l.size() < 2) return l;
  L s = sort(l.size(), 0, false).first;
  return std::vector<T>(s.begin(), s.end());
}

}  // namespace cppcaml::typing::list_sort
