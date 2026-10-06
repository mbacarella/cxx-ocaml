// Port of asmcomp/arm64/stackframe.ml: Stackframegen's class specialized.
#include "../stackframegen.hpp"

namespace cppcaml::typing::stackframegen {

const long trap_handler_size = 16;

bool is_call(const mach::Instruction& i) { return is_call_generic(i); }

bool frame_required(const mach::Fundecl& f, bool contains_calls) { return frame_required_generic(f, contains_calls); }

}  // namespace cppcaml::typing::stackframegen
