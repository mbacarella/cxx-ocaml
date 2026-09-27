// Port of typing/signature_matching.ml (with utils/stable_matching.ml,
// TYPECHECKER.md stage 9): the suggestions of a signature mismatch report
// -- the missing items, and the fuzzy renamings (a stable marriage between
// the extra and the missing items by edit distance, compatible items only).
#pragma once

#include <vector>

#include "cppcaml/typing/includemod.hpp"

namespace cppcaml::typing::signature_matching {

struct Alteration {  // Missing_item | Possible_match of Ident.t Location.loc
  bool missing;
  Ident::t id = nullptr;
  Location loc;
};
template <class A>
struct Suggestion {
  const SignatureItem* subject;
  A alteration;
};
struct Report {
  std::vector<Suggestion<Alteration>> alterations;
  std::vector<Suggestion<includemod::error::SigitemSymptom>> incompatibles;
};

Report suggest(const includemod::error::SignatureSymptom& sgs);

}  // namespace cppcaml::typing::signature_matching
