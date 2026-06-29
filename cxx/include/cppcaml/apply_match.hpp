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
  if (!r.leftover.empty() && has_omitted) return r;  // over-app + gap: too complex
  r.ok = true;
  return r;
}

}  // namespace cppcaml::applymatch
