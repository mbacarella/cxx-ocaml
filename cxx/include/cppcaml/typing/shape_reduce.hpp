// Port of typing/shape_reduce.mli (TYPECHECKER.md, stage 10, for the .cmt):
// the strong call-by-need reduction of shapes.  Only Local_reduce over
// Env.empty is ported -- the reduction Typemod runs on a unit's shape before
// Cmt_format saves it (no unit shapes are read, no environment lookups).
#pragma once

#include "cppcaml/typing/shape.hpp"

namespace cppcaml::typing::shape_reduce {

// Shape_reduce.local_reduce Env.empty t
shape::t local_reduce_empty(shape::t t);

// Local_store's memo tables (per compilation unit)
void reset();

}  // namespace cppcaml::typing::shape_reduce
