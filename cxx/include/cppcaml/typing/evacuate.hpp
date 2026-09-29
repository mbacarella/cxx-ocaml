// Evacuation: what the native back end still needs of the typing phase's
// zone copied out of it, so that the zone -- the parse tree, the typed
// tree, the types, the environments: the bulk of a unit's memory -- can be
// dropped once the unit is translated to Lambda (the OCaml GC reclaims them
// then; the port kept them to the end of the back end).
//
// An Evacuator walks what it is given and replaces every pointer into the
// dying zone by a copy in the current zone (one copy per object: sharing,
// and so the writers' output, is kept); what is outside the zone is left
// alone.  Lambda's nodes live in a zone of their own (lambda.cpp): they are
// rewritten in place, not copied.
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::evacuate {

class Evacuator {
 public:
  explicit Evacuator(const Zone& dying) : dying_(dying) {}

  bool owned(const void* p) const { return p && dying_.owns(static_cast<const char*>(p)); }

  std::string_view str(std::string_view s);
  Ident::t ident(Ident::t id);
  Path::t path(Path::t p);
  const PrimitiveDescription* prim_desc(const PrimitiveDescription* d);
  // an identity-only pointer (never read through): a fresh identity for
  // one in the dying zone (a new object could take its address)
  const void* identity(const void* p);
  Location location(const Location& l);
  debuginfo::ScopedLocation scoped_location(const debuginfo::ScopedLocation& l);
  debuginfo::scopes scopes(debuginfo::scopes s);
  const lambda::StructuredConstant* constant(const lambda::StructuredConstant* c);
  // a Lambda term: its nodes rewritten in place, what they point to copied
  void lambda(lambda::lambda l);
  const lambda::LFunction* lfunction(const lambda::LFunction* f);
  void program(lambda::Program& p);
  // (a check) the pointers into the dying zone left in a rewritten term:
  // one line per node that still has one
  std::vector<std::string> leftovers(lambda::lambda l);

 private:
  template <class T>
  Slice<T> slice_copy(Slice<T> s);
  lambda::Primitive primitive(const lambda::Primitive& p);

  const Zone& dying_;
  std::unordered_map<const void*, const void*> memo_;  // old object -> its copy
  std::unordered_map<const Location*, const Location*> locs_;  // (a location's address may be an identity)
  std::unordered_map<std::uint64_t, std::unordered_map<std::size_t, std::string_view>> strs_;
  std::unordered_set<lambda::lambda> seen_;
};

}  // namespace cppcaml::typing::evacuate
