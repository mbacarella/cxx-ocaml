// Port of middle_end/flambda/inconstant_idents.ml: which variables and sets
// of closures cannot be statically allocated (are "not constant"), by
// propagating "x in NC => y in NC" implications to a fixpoint.
#pragma once

#include <memory>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::inconstant_idents {

struct Result;  // { id : state Variable.Tbl.t; closure : state Set_of_closures_id.Tbl.t }

// inconstants_on_program ~compilation_unit ~backend program
std::shared_ptr<const Result> inconstants_on_program(compilation_unit::t compilation_unit,
                                                     const flambda::Program& program);
bool variable(variable::t var, const Result& r);
bool closure(set_of_closures_id::t cl, const Result& r);

}  // namespace cppcaml::typing::inconstant_idents
