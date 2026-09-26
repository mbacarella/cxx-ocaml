// Port of typing/signature_group.ml.  See signature_group.hpp.
#include "cppcaml/typing/signature_group.hpp"

#include "cppcaml/typing/btype.hpp"

namespace cppcaml::typing::signature_group {

using SK = SignatureItem::Kind;

std::vector<const SignatureItem*> flatten(const SigItem& x) {
  std::vector<const SignatureItem*> r{x.src};
  r.insert(r.end(), x.post_ghosts.begin(), x.post_ghosts.end());
  return r;
}

std::vector<SigItem> rec_items(const CoreRecGroup& g) { return g.items; }

static Signature tail(Signature s, std::size_t n) { return Slice<const SignatureItem*>(s.begin() + n, s.size() - n); }

static std::optional<std::pair<SigItem, Signature>> next_group(Signature sg) {
  if (sg.empty()) return std::nullopt;
  const SignatureItem* src = sg[0];
  Signature q = tail(sg, 1);
  std::vector<const SignatureItem*> ghosts;
  switch (src->kind) {
    case SK::Sig_class:
      // a class declaration for [c] is followed by the ghost declarations of
      // class type [c], and type [c]
      if (q.size() < 2) throw std::logic_error("Signature_group.next_group");
      ghosts = {q[0], q[1]};
      q = tail(q, 2);
      break;
    case SK::Sig_class_type:
      // a class type declaration for [ct] is followed by the ghost
      // declaration of type [ct]
      if (q.size() < 1) throw std::logic_error("Signature_group.next_group");
      ghosts = {q[0]};
      q = tail(q, 1);
      break;
    default: break;
  }
  return std::make_pair(SigItem{src, ghosts}, q);
}

std::optional<std::pair<std::string_view, RecStatus>> recursive_sigitem(const SignatureItem* it) {
  switch (it->kind) {
    case SK::Sig_type:
    case SK::Sig_class:
    case SK::Sig_class_type:
    case SK::Sig_module: return std::make_pair(ident::name(it->id), it->rec);
    default: return std::nullopt;
  }
}

std::optional<std::pair<RecGroup, Signature>> next(Signature x) {
  std::vector<const SignatureItem*> pre;  // head first (reversed)
  auto cons_group = [&](std::vector<SigItem> group_rev, Signature q) {
    std::vector<SigItem> group(group_rev.rbegin(), group_rev.rend());
    std::vector<const SignatureItem*> p(pre.rbegin(), pre.rend());
    return std::make_pair(RecGroup{p, CoreRecGroup{true, group}}, q);
  };
  Signature l = x;
  for (;;) {  // not_in_group
    auto ng = next_group(l);
    if (!ng) {
      if (!pre.empty()) throw std::logic_error("Signature_group.next");
      return std::nullopt;
    }
    auto& [elt, q] = *ng;
    auto rs = recursive_sigitem(elt.src);
    if (rs && btype::is_row_name(rs->first)) {
      pre.insert(pre.begin(), elt.src);
      l = q;
      continue;
    }
    if (!rs || rs->second == RecStatus::Trec_not) {
      std::vector<const SignatureItem*> p(pre.rbegin(), pre.rend());
      return std::make_pair(RecGroup{p, CoreRecGroup{false, {elt}}}, q);
    }
    // in_group
    std::vector<SigItem> group{elt};  // reversed
    Signature rem = q;
    for (;;) {
      auto ng2 = next_group(rem);
      if (!ng2) return cons_group(group, Signature{});
      auto& [elt2, next2] = *ng2;
      auto rs2 = recursive_sigitem(elt2.src);
      if (rs2 && rs2->second == RecStatus::Trec_next) {
        group.insert(group.begin(), elt2);
        rem = next2;
        continue;
      }
      return cons_group(group, rem);
    }
  }
}

std::vector<RecGroup> seq(Signature sg) {
  std::vector<RecGroup> out;
  for (;;) {
    auto n = next(sg);
    if (!n) return out;
    out.push_back(n->first);
    sg = n->second;
  }
}
void iter(const std::function<void(const RecGroup&)>& f, Signature sg) {
  for (;;) {
    auto n = next(sg);
    if (!n) return;
    f(n->first);
    sg = n->second;
  }
}

Signature update_rec_next(RecStatus rs, Signature rem) {
  if (rs == RecStatus::Trec_next) return rem;
  if (rem.empty()) return rem;
  const SignatureItem* h = rem[0];
  if ((h->kind == SK::Sig_type || h->kind == SK::Sig_module) && h->rec == RecStatus::Trec_next) {
    auto* n = make<SignatureItem>(*h);
    n->rec = rs;
    std::vector<const SignatureItem*> out{n};
    out.insert(out.end(), rem.begin() + 1, rem.end());
    return slice(out);
  }
  return rem;
}

}  // namespace cppcaml::typing::signature_group
