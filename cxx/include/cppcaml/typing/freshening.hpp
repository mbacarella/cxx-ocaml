// Port of middle_end/flambda/freshening.ml.  Ported so far: Project_var
// (the freshening of closure ids and closure variables an approximation
// records).
#pragma once

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::freshening {

namespace project_var {
struct T {
  variable::Map<variable::t> vars_within_closure;  // Var_within_closure.Map
  variable::Map<variable::t> closure_id;           // Closure_id.Map
};
inline T empty() { return {}; }
void print(format::Formatter& ppf, const T& t);
variable::t apply_closure_id(const T& t, variable::t closure_id);
variable::t apply_var_within_closure(const T& t, variable::t var_in_closure);
T compose(const T& earlier, const T& later);
}  // namespace project_var

}  // namespace cppcaml::typing::freshening
