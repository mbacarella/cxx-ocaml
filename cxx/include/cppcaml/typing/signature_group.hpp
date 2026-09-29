// Port of typing/signature_group.ml: iterate over a signature by syntactic
// groups (classes, class types and private row types add ghost components
// to the signature where they are defined; recursive groups).
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::signature_group {

struct SigItem {
  const SignatureItem* src;  // the syntactic item
  std::vector<const SignatureItem*> post_ghosts;
};
std::vector<const SignatureItem*> flatten(const SigItem& x);

struct CoreRecGroup {  // Not_rec of sig_item | Rec_group of sig_item list
  bool is_rec;
  std::vector<SigItem> items;  // Not_rec: one item
};
std::vector<SigItem> rec_items(const CoreRecGroup& g);

struct RecGroup {
  std::vector<const SignatureItem*> pre_ghosts;
  CoreRecGroup group;
};
// next sg: the first group and the rest (as an index into sg)
std::optional<std::pair<RecGroup, Signature>> next(Signature sg);
std::vector<RecGroup> seq(Signature sg);
void iter(const std::function<void(const RecGroup&)>& f, Signature sg);

struct InPlacePatch {
  Signature ghosts;
  const SignatureItem* replace_by;  // option
};
// replace_in_place f sg: f ~ghosts item returns Some (info, patch) | None
template <class Info>
std::optional<std::pair<Info, Signature>> replace_in_place(
    const std::function<std::optional<std::pair<Info, InPlacePatch>>(Signature ghosts, const SignatureItem*)>& f,
    Signature sg);

// ---- implementation of the template ----
Signature update_rec_next(RecStatus rs, Signature rem);
std::optional<std::pair<std::string_view, RecStatus>> recursive_sigitem(const SignatureItem* it);

template <class Info>
std::optional<std::pair<Info, Signature>> replace_in_place(
    const std::function<std::optional<std::pair<Info, InPlacePatch>>(Signature ghosts, const SignatureItem*)>& f,
    Signature sg0) {
  using L = std::vector<const SignatureItem*>;
  // before: an OCaml list kept in reverse order (head first)
  L before;
  Signature sg = sg0;
  for (;;) {
    auto n = next(sg);
    if (!n) return std::nullopt;
    const RecGroup& item = n->first;
    sg = n->second;
    L ghosts(item.pre_ghosts.begin(), item.pre_ghosts.end());
    L before_group;  // head first
    std::vector<SigItem> current = rec_items(item.group);
    for (std::size_t k = 0;; ++k) {
      // commit ghosts = before_group @ List.rev_append ghosts before
      auto commit = [&](const L& gh) {
        L r = before_group;
        for (auto it = gh.rbegin(); it != gh.rend(); ++it) r.push_back(*it);
        r.insert(r.end(), before.begin(), before.end());
        return r;
      };
      if (k == current.size()) {
        before = commit(ghosts);
        break;
      }
      const SigItem& a = current[k];
      auto r = f(slice(ghosts), a.src);
      if (r) {
        auto& [info, patch] = *r;
        L after;
        for (std::size_t j = k + 1; j < current.size(); ++j) {
          auto fl = flatten(current[j]);
          after.insert(after.end(), fl.begin(), fl.end());
        }
        after.insert(after.end(), sg.begin(), sg.end());
        auto rs = recursive_sigitem(a.src);
        if (rs && !patch.replace_by) {
          Signature u = update_rec_next(rs->second, slice(after));
          after = L(u.begin(), u.end());
        }
        L pg(patch.ghosts.begin(), patch.ghosts.end());
        L before2 = commit(pg);
        if (patch.replace_by) before2.insert(before2.begin(), patch.replace_by);
        // List.rev_append before after
        L out(before2.rbegin(), before2.rend());
        out.insert(out.end(), after.begin(), after.end());
        return std::make_pair(info, slice(out));
      }
      // before_group = List.rev_append a.post_ghosts (a.src :: before_group)
      L bg{a.src};
      bg.insert(bg.end(), before_group.begin(), before_group.end());
      L bg2(a.post_ghosts.rbegin(), a.post_ghosts.rend());
      bg2.insert(bg2.end(), bg.begin(), bg.end());
      before_group = bg2;
    }
  }
}

}  // namespace cppcaml::typing::signature_group
