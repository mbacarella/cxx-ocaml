// Ports of the Mach passes between instruction selection and
// linearization (amd64): asmcomp/comballoc.ml, CSEgen.ml + amd64/CSE.ml,
// liveness.ml, deadcode.ml, spill.ml, split.ml, interf.ml, coloring.ml and
// reloadgen.ml + amd64/reload.ml.
#pragma once

#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::mach_passes {

mach::Fundecl comballoc(const mach::Fundecl& f);
mach::Fundecl cse(const mach::Fundecl& f);
void liveness(const mach::Fundecl& f);  // annotates i.live
mach::Fundecl deadcode(const mach::Fundecl& f);
mach::Fundecl spill(const mach::Fundecl& f);
mach::Fundecl split(const mach::Fundecl& f);
void interf_build_graph(const mach::Fundecl& f);
std::vector<long> coloring_allocate_registers();
// Reload.fundecl f num_stack_slots: the new function and whether register
// allocation must be redone
std::pair<mach::Fundecl, bool> reload(const mach::Fundecl& f, const std::vector<long>& num_stack_slots);

}  // namespace cppcaml::typing::mach_passes
