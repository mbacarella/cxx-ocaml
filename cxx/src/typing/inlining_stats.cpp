// Port of middle_end/flambda/inlining_stats.ml (see inlining_stats.hpp).
#include "cppcaml/typing/inlining_stats.hpp"

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inlining_stats {

namespace {
[[noreturn]] void report_not_ported() { misc::fatal_error("-inlining-report is not ported yet"); }
}  // namespace

namespace closure_stack {
ClosureStack note_entering_closure(ClosureStack t, variable::t, const debuginfo::t&) {
  if (!clflags::inlining_report) return t;
  report_not_ported();
}
ClosureStack note_entering_call(ClosureStack t, variable::t, const debuginfo::t&) {
  if (!clflags::inlining_report) return t;
  report_not_ported();
}
ClosureStack note_entering_inlined(ClosureStack t) {
  if (!clflags::inlining_report) return t;
  report_not_ported();
}
ClosureStack note_entering_specialised(ClosureStack t, const variable::Set&) {
  if (!clflags::inlining_report) return t;
  report_not_ported();
}
}  // namespace closure_stack

void record_decision(const Decision&, ClosureStack) {
  if (clflags::inlining_report) report_not_ported();
}

void save_then_forget_decisions(const std::string&) {
  if (clflags::inlining_report) report_not_ported();
}

}  // namespace cppcaml::typing::inlining_stats
