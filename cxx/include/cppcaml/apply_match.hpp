// Pure matching of an application's arguments against the callee's labelled /
// optional parameter signature -- the representation-neutral core shared by the
// Lambda back end (apply_labeled / build_apply) and, for Slice 3, the typed-tree
// transcriber (reconstructing Texp_apply's argument list with omitted optionals
// filled as ghost None).  References argument INDICES only; no codegen, no AST.
//
// Mirrors typecore/translcore: labelled args may be reordered to parameter
// order, an optional parameter given `~l:e` is Some-wrapped, a `?l:e` passed
// directly, and a missing parameter becomes either an omitted-None slot (when a
// later POSITIONAL argument forces the default) or an eta parameter of a partial
// application.
#pragma once

#include <string>
#include <vector>

namespace cppcaml::applymatch {

// Label kinds: 0 Nolabel, 1 Labelled, 2 Optional.
struct Param { int label; std::string name; };
struct Arg { int label; std::string name; };

struct Slot {
  bool omitted = false;      // not supplied by the call
  bool none_fill = false;    // omitted optional defaulted to None (vs kept as an eta param)
  int arg_index = -1;        // when !omitted: index into the written args
  bool optional = false;     // the callee parameter is optional
  bool some_wrap = false;    // provided optional given as `~l:e` -> wrap the value in Some
  int param_label = 0;       // the callee parameter's label kind
  std::string param_name;    // the callee parameter's name
};

struct Result {
  bool ok = false;             // false -> caller should apply the args verbatim
  std::vector<Slot> slots;     // callee-parameter order, trailing-omitted dropped
  std::vector<int> leftover;   // over-application arg indices, in source order
};

inline Result match(const std::vector<Param>& params, const std::vector<Arg>& args) {
  Result r;
  std::vector<bool> used(args.size(), false);
  int last_arg = -1;
  // OCaml fills a required labelled parameter POSITIONALLY (label omitted at the
  // call) only in a TOTAL application -- one supplying enough arguments to cover
  // every non-optional parameter.  In a PARTIAL application the positional args
  // fill only the Nolabel parameters, and each still-unmatched labelled parameter
  // stays pending (an eta parameter of the resulting closure): `let g ~x a b in
  // g 1 2` is `fun ~x -> g ~x 1 2`, NOT `g ~x:1 2 <partial>`.  Totality must
  // count the POSITIONAL args against the non-optional params NO labelled
  // argument names -- counting all args against all non-optional params treated
  // `f ~a ~b ~c x y` on (?o ~a ~b ~c pos1 ~d pos2) as saturating and routed y
  // into ~d (typedecl's is_reachable wrapper put `path` in ~from_ty; bootstrap
  // bug#13 -- ~from_ty must stay PENDING, an eta param of the partial result).
  size_t npos_args = 0;
  for (const Arg& a : args) if (a.label == 0) ++npos_args;
  size_t nreq_unnamed = 0;
  for (const Param& p : params) {
    if (p.label == 2) continue;
    bool named = false;
    if (p.label == 1)
      for (const Arg& a : args)
        if (a.label == 1 && a.name == p.name) { named = true; break; }
    if (!named) ++nreq_unnamed;
  }
  bool total = npos_args >= nreq_unnamed;
  for (const Param& p : params) {
    int found = -1, fk = 0;
    for (size_t i = 0; i < args.size(); ++i) {
      if (used[i]) continue;
      int k = args[i].label;
      if (p.label == 0 && k == 0) { found = (int)i; break; }
      if (p.label == 1 && k == 1 && args[i].name == p.name) { found = (int)i; break; }
      if (p.label == 2 && (k == 1 || k == 2) && args[i].name == p.name) {
        found = (int)i;
        fk = k;
        break;
      }
    }
    // A required (Labelled, non-optional) parameter with no explicitly-labelled
    // argument is filled by the next positional (Nolabel) argument -- but only in
    // a total application (see `total` above); OCaml lets `foo 2` supply `~bar`
    // positionally when saturating the call.  The slot still carries the param's
    // label so the typed tree records `Labelled "bar"`.
    if (found < 0 && p.label == 1 && total)
      for (size_t i = 0; i < args.size(); ++i)
        if (!used[i] && args[i].label == 0) { found = (int)i; break; }
    Slot s;
    s.param_label = p.label;
    s.param_name = p.name;
    s.optional = p.label == 2;
    if (found < 0) {
      s.omitted = true;
      if (p.label == 2) {  // a later positional forces the default to None; else eta
        for (size_t i = 0; i < args.size(); ++i)
          if (!used[i] && args[i].label == 0) { s.none_fill = true; break; }
      }
      r.slots.push_back(std::move(s));
      continue;
    }
    used[found] = true;
    s.arg_index = found;
    s.some_wrap = p.label == 2 && fk == 1;
    r.slots.push_back(std::move(s));
    last_arg = (int)r.slots.size() - 1;
  }
  if (last_arg < 0) return r;          // nothing matched -> verbatim apply (ok=false)
  r.slots.resize(last_arg + 1);        // drop trailing omitted (params beyond the call)
  bool has_omitted = false;
  for (const Slot& s : r.slots) if (s.omitted) has_omitted = true;
  for (size_t i = 0; i < args.size(); ++i)
    if (!used[i]) {
      if (args[i].label != 0) return r;  // stray labelled over-app -> bail (ok=false)
      r.leftover.push_back((int)i);
    }
  // Over-application past omitted parameters is fine when every omitted slot is
  // an optional defaulted to None -- the leftover positionals themselves force
  // the defaults (translcore fills None and applies the leftovers to the
  // result).  A genuinely pending (eta) omitted parameter with leftover args
  // remains too complex -> verbatim bail.  (Bailing here is NOT safe on a
  // labelled call: the verbatim apply passes args in written order, unwrapped
  // -- bootstrap bug#13 put a longident in Location.aligned_error_hint's fmt.)
  if (!r.leftover.empty() && has_omitted)
    for (const Slot& s : r.slots)
      if (s.omitted && !s.none_fill) return r;
  r.ok = true;
  return r;
}

}  // namespace cppcaml::applymatch
