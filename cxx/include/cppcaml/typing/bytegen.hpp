// Port of bytecomp/bytegen.mli (cxx/PORTING.md stage 10): Lambda to the
// abstract machine's instructions (instruct.hpp).
#pragma once

#include "cppcaml/typing/instruct.hpp"
#include "cppcaml/typing/lambda.hpp"

namespace cppcaml::typing::bytegen {

instruct::code compile_implementation(std::string_view modulename, lambda::lambda expr);
// compile_phrase: instruction list * bool (the toplevel's; not needed by ocamlc)
instruct::DebugEvent* merge_events(instruct::DebugEvent* ev1, instruct::DebugEvent* ev2);

}  // namespace cppcaml::typing::bytegen
