// The target-specific parts of Stackframegen (asmcomp/<arch>/stackframe.ml
// overrides them): the target's (src/typing/<arch>/stackframe.cpp),
// deferring to the generic ones (linearize.cpp) as the overrides call super.
#pragma once

#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::stackframegen {
extern const long trap_handler_size;  // Size of an exception handler block on the stack
bool is_call_generic(const mach::Instruction& i);
bool is_call(const mach::Instruction& i);
bool frame_required_generic(const mach::Fundecl& f, bool contains_calls);
bool frame_required(const mach::Fundecl& f, bool contains_calls);
}  // namespace cppcaml::typing::stackframegen
