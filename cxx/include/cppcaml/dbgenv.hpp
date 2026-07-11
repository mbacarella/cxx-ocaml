#pragma once
// Cached lookup for debug-gate environment variables.
//
// The compiler sprinkles `if (getenv("CTDBG")) ...`-style trace gates through its
// hottest paths (type printing, translation).  getenv() rescans the whole
// environment on every call, and these gates fire per type node / per reloc --
// e.g. CONSTRDBG was queried 24k times in a single typecore compile.  The
// environment does not change during a run, so resolve each name ONCE and cache
// it.  Returns the value (nullptr if unset) so both presence checks and value
// comparisons work unchanged.
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace cppcaml {

inline const char* dbg_env(const char* name) {
  static std::unordered_map<std::string, const char*> cache;
  auto it = cache.find(name);
  if (it != cache.end()) return it->second;
  const char* v = std::getenv(name);
  return cache.emplace(name, v).first->second;
}

}  // namespace cppcaml
