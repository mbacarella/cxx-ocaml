// Port of middle_end/flambda/inlining_stats.ml and inlining_stats_types.ml:
// the inlining decisions (Inlining_decision makes them), recorded for
// -inlining-report.  The report itself is not ported yet: with
// -inlining-report the middle end stops.
#pragma once

#include <string>
#include <vector>

#include "cppcaml/typing/flambda.hpp"
#include "cppcaml/typing/inlining_cost.hpp"

namespace cppcaml::typing::inlining_stats {

using Wsb = inlining_cost::WhetherSufficientBenefit;

// Inlining_stats_types: Inlined / Not_inlined / Specialised /
// Not_specialised / Prevented, as a kind and the arguments it carries
struct Inlined {
  enum class Kind : unsigned char { Classic_mode, Annotation, Decl_local_to_application, Without_subfunctions,
                                    With_subfunctions } kind;
  Wsb wsb1{}, wsb2{};
};
struct NotInlined {
  enum class Kind : unsigned char { Classic_mode, Above_threshold, Annotation, No_useful_approximations,
                                    Unrolling_depth_exceeded, Self_call, Without_subfunctions,
                                    With_subfunctions } kind;
  long size = 0;  // Above_threshold
  Wsb wsb1{}, wsb2{};
};
struct Specialised {
  enum class Kind : unsigned char { Annotation, Without_subfunctions, With_subfunctions } kind;
  Wsb wsb1{}, wsb2{};
};
struct NotSpecialised {
  enum class Kind : unsigned char { Classic_mode, Above_threshold, Annotation, Not_recursive, Not_closed,
                                    No_invariant_parameters, No_useful_approximations, Self_call,
                                    Not_beneficial } kind;
  long size = 0;  // Above_threshold
  Wsb wsb1{}, wsb2{};
};
enum class Prevented : unsigned char { Function_prevented_from_inlining, Level_exceeded };
struct Decision {
  enum class Kind : unsigned char { Prevented, Specialised, Inlined, Unchanged } kind;
  Prevented prevented = Prevented::Function_prevented_from_inlining;
  Specialised specialised{};
  NotSpecialised not_specialised{};
  Inlined inlined{};
  NotInlined not_inlined{};
};

// Closure_stack.t (only kept with -inlining-report)
struct ClosureStackNode;
using ClosureStack = const ClosureStackNode*;
namespace closure_stack {
inline ClosureStack create() { return nullptr; }
ClosureStack note_entering_closure(ClosureStack t, variable::t closure_id, const debuginfo::t& dbg);
ClosureStack note_entering_call(ClosureStack t, variable::t closure_id, const debuginfo::t& dbg);
ClosureStack note_entering_inlined(ClosureStack t);
ClosureStack note_entering_specialised(ClosureStack t, const variable::Set& closure_ids);
}  // namespace closure_stack

void record_decision(const Decision& decision, ClosureStack closure_stack);
// save_then_forget_decisions ~output_prefix
void save_then_forget_decisions(const std::string& output_prefix);

}  // namespace cppcaml::typing::inlining_stats
