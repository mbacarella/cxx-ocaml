// The target's Arch (asmcomp/<arch>/arch.ml): its addressing modes, specific
// operations and constants.  One back end per build, as OCaml configures
// one (cxx/Makefile's ARCH); mach.hpp includes this after Reg.
#pragma once

#if defined(CPPCAML_ARCH_arm64)
#include "cppcaml/typing/arm64/arch.hpp"
#else
#include "cppcaml/typing/amd64/arch.hpp"
#endif
