// Port of asmcomp/selectgen.ml with asmcomp/amd64/selection.ml's selector,
// asmcomp/polling.ml and asmcomp/dataflow.ml: instruction selection from
// Cmm to Mach, and the insertion of polling points.
#pragma once

#include <set>
#include <string_view>

#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::selection {

using FuncNames = std::set<std::string_view>;

// Selection.fundecl ~future_funcnames
mach::Fundecl fundecl(const FuncNames& future_funcnames, const cmm::Fundecl& f);
void reset();

}  // namespace cppcaml::typing::selection

namespace cppcaml::typing::polling {
// Polling.Error (Poll_error of (polling_point * Debuginfo.t) list)
struct PollError {
  enum class Point { Alloc, Poll, Function_call, External_call };
  std::vector<std::pair<Point, debuginfo::t>> points;
};
void report_error(format_doc::Formatter& ppf, const PollError& e);
mach::Fundecl instrument_fundecl(const mach::Fundecl& f);
bool requires_prologue_poll(const selection::FuncNames& future_funcnames, std::string_view fun_name, mach::Instr i);
}  // namespace cppcaml::typing::polling
