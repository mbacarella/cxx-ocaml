// Port of typing/shape_reduce.mli (TYPECHECKER.md, stage 10, for the .cmt):
// the strong call-by-need reduction of shapes -- Local_reduce (no unit
// shapes are read, fuel 10): the reduction Typemod runs on a unit's shape
// before Cmt_format saves it, and the per-occurrence reduction of
// -bin-annot-occurrences (local_reduce_for_uid), which looks identifiers
// up in the environment of the occurrence.
#pragma once

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/shape.hpp"

namespace cppcaml::typing::shape_reduce {

// type result
struct Result {
  enum class Kind : std::uint8_t {
    Resolved, Resolved_alias, Resolved_local_use, Unresolved, Approximated, Missing_uid,
    Internal_error_missing_uid
  };
  Kind kind;
  // Resolved / Resolved_alias / Resolved_local_use: the uid; Approximated:
  // the uid option (has_uid false = None) and its Some block's identity
  bool has_uid = false;
  Uid uid{};
  const void* uid_obj = nullptr;
  const Result* alias = nullptr;  // Resolved_alias's result
  shape::t shape = nullptr;       // Unresolved / Missing_uid
};

// local_reduce env t
shape::t local_reduce(env::t env, shape::t t);
// Shape_reduce.local_reduce Env.empty t
shape::t local_reduce_empty(shape::t t);
// local_reduce_for_uid env ~namespace path shape
const Result* local_reduce_for_uid(env::t env, shape::SigComponentKind ns, Path::t path, shape::t shape);

// Local_store's memo tables (per compilation unit)
void reset();

}  // namespace cppcaml::typing::shape_reduce
