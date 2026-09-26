// stdlib list.ml's List.stable_sort (= List.sort), ported exactly: the typer
// sorts with comparators that raise (Typetexp's duplicate constraints, ...),
// so the sequence of comparisons must be OCaml's.  Lists are vectors, head
// first.
#pragma once

#include <utility>
#include <vector>

namespace cppcaml::typing::ocaml_list {

namespace detail {
template <class T, class C>
struct Sorter {
  C& cmp;
  using L = std::vector<T>;
  // rev_append a[i..] accu (accu is built head-first: we prepend)
  static void rev_append_into(const L& l, std::size_t i, L& accu_rev) {
    // accu_rev holds the accumulator reversed (its head at the back)
    for (std::size_t k = i; k < l.size(); ++k) accu_rev.push_back(l[k]);
  }
  L rev_merge(const L& l1, const L& l2) {
    L accu_rev;  // head of accu at the back
    std::size_t i = 0, j = 0;
    for (;;) {
      if (i == l1.size()) { rev_append_into(l2, j, accu_rev); break; }
      if (j == l2.size()) { rev_append_into(l1, i, accu_rev); break; }
      if (cmp(l1[i], l2[j]) <= 0) accu_rev.push_back(l1[i++]);
      else accu_rev.push_back(l2[j++]);
    }
    return L(accu_rev.rbegin(), accu_rev.rend());
  }
  L rev_merge_rev(const L& l1, const L& l2) {
    L accu_rev;
    std::size_t i = 0, j = 0;
    for (;;) {
      if (i == l1.size()) { rev_append_into(l2, j, accu_rev); break; }
      if (j == l2.size()) { rev_append_into(l1, i, accu_rev); break; }
      if (cmp(l1[i], l2[j]) > 0) accu_rev.push_back(l1[i++]);
      else accu_rev.push_back(l2[j++]);
    }
    return L(accu_rev.rbegin(), accu_rev.rend());
  }
  // sort n l -> (sorted prefix of length n, rest index)
  std::pair<L, std::size_t> sort(std::size_t n, const L& l, std::size_t at) {
    if (n == 2) {
      const T &x1 = l[at], &x2 = l[at + 1];
      L s = cmp(x1, x2) <= 0 ? L{x1, x2} : L{x2, x1};
      return {s, at + 2};
    }
    if (n == 3) {
      const T &x1 = l[at], &x2 = l[at + 1], &x3 = l[at + 2];
      L s;
      if (cmp(x1, x2) <= 0) {
        if (cmp(x2, x3) <= 0) s = {x1, x2, x3};
        else if (cmp(x1, x3) <= 0) s = {x1, x3, x2};
        else s = {x3, x1, x2};
      } else if (cmp(x1, x3) <= 0) s = {x2, x1, x3};
      else if (cmp(x2, x3) <= 0) s = {x2, x3, x1};
      else s = {x3, x2, x1};
      return {s, at + 3};
    }
    std::size_t n1 = n >> 1, n2 = n - n1;
    auto [s1, l2] = rev_sort(n1, l, at);
    auto [s2, tl] = rev_sort(n2, l, l2);
    return {rev_merge_rev(s1, s2), tl};
  }
  std::pair<L, std::size_t> rev_sort(std::size_t n, const L& l, std::size_t at) {
    if (n == 2) {
      const T &x1 = l[at], &x2 = l[at + 1];
      L s = cmp(x1, x2) > 0 ? L{x1, x2} : L{x2, x1};
      return {s, at + 2};
    }
    if (n == 3) {
      const T &x1 = l[at], &x2 = l[at + 1], &x3 = l[at + 2];
      L s;
      if (cmp(x1, x2) > 0) {
        if (cmp(x2, x3) > 0) s = {x1, x2, x3};
        else if (cmp(x1, x3) > 0) s = {x1, x3, x2};
        else s = {x3, x1, x2};
      } else if (cmp(x1, x3) > 0) s = {x2, x1, x3};
      else if (cmp(x2, x3) > 0) s = {x2, x3, x1};
      else s = {x3, x2, x1};
      return {s, at + 3};
    }
    std::size_t n1 = n >> 1, n2 = n - n1;
    auto [s1, l2] = sort(n1, l, at);
    auto [s2, tl] = sort(n2, l, l2);
    return {rev_merge(s1, s2), tl};
  }
};
}  // namespace detail

// List.stable_sort cmp l (cmp returns <0 / 0 / >0)
template <class T, class C>
std::vector<T> stable_sort(C&& cmp, const std::vector<T>& l) {
  if (l.size() < 2) return l;
  detail::Sorter<T, C> s{cmp};
  return s.sort(l.size(), l, 0).first;
}

}  // namespace cppcaml::typing::ocaml_list
