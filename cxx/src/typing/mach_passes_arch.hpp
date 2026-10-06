// The target-specific parts of the Mach passes: the methods each target's
// asmcomp/<arch>/{CSE,reload}.ml overrides in the generic classes, as free
// functions -- the target's (src/typing/<arch>/), deferring to the generic
// ones (mach_passes.cpp) as the overrides call super.
#pragma once

#include <utility>

#include "cppcaml/typing/mach.hpp"

namespace cppcaml::typing::mach_passes {

namespace csegen {
// CSEgen.op_class (Op_load and Op_store split by their argument)
enum class OpClass { Op_pure, Op_checkbound, Op_load_immutable, Op_load_mutable, Op_store_init, Op_store_assign, Op_other };
OpClass class_of_operation_generic(const mach::Operation& op);
OpClass class_of_operation(const mach::Operation& op);
bool is_cheap_operation_generic(const mach::Operation& op);
bool is_cheap_operation(const mach::Operation& op);
}  // namespace csegen

namespace reloadgen {
inline bool stackp(const reg::Reg* r) {
  using LK = reg::Location::K;
  return r->loc.k == LK::Local || r->loc.k == LK::Incoming || r->loc.k == LK::Outgoing || r->loc.k == LK::Domainstate;
}
inline bool same_loc(const reg::Location& a, const reg::Location& b) { return a.k == b.k && a.n == b.n; }
// The generic class's methods an override calls (self#..., super#...)
struct Self {
  virtual reg::Reg* makereg(reg::Reg* r) = 0;
  virtual reg::Regs makeregs(const reg::Regs& rv) = 0;
  virtual std::pair<reg::Regs, reg::Regs> reload_operation_generic(const mach::Operation& op, const reg::Regs& arg,
                                                                   const reg::Regs& res) = 0;
  virtual reg::Regs reload_test_generic(const mach::Test& tst, const reg::Regs& arg) = 0;

 protected:
  ~Self() = default;
};
std::pair<reg::Regs, reg::Regs> reload_operation(Self& self, const mach::Operation& op, const reg::Regs& arg,
                                                 const reg::Regs& res);
reg::Regs reload_test(Self& self, const mach::Test& tst, const reg::Regs& arg);
}  // namespace reloadgen

}  // namespace cppcaml::typing::mach_passes
