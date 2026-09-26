// Port of bytecomp/instruct.mli (TYPECHECKER.md stage 10): the abstract
// machine's instructions, the compilation environments and the debugging
// events, as Bytegen produces them and Emitcode / Printinstr consume them.
#pragma once

#include <cstdint>

#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/ident.hpp"
#include "cppcaml/typing/lambda.hpp"
#include "cppcaml/typing/subst.hpp"

namespace cppcaml::typing::instruct {

// ---- compilation environments -------------------------------------------------

struct ClosureEntry {  // Free_variable of int | Function of int
  enum class K : std::uint8_t { Free_variable, Function } k;
  long pos;
};

struct ClosureEnv {  // Not_in_closure | In_closure of { entries; env_pos }
  bool in_closure = false;
  ident::Tbl<ClosureEntry> entries;
  long env_pos = 0;
  // In_closure's block identity (the debug events marshal it): a fresh
  // token where Bytegen builds a new In_closure, kept by copies; nullptr =
  // none (then equal contents are taken as one block)
  const void* obj = nullptr;
};

struct CompilationEnv {
  ident::Tbl<long> ce_stack;  // positions of variables in the stack
  ClosureEnv ce_closure;      // structure of the heap-allocated env
  // the record's identity, as ClosureEnv::obj (a fresh token per `{ce_stack;
  // ce_closure}` record Bytegen builds)
  const void* obj = nullptr;
};

// ---- debugging events (runtime/backtrace_byt.c reads these) --------------------

enum class DebugEventKindK : std::uint8_t { Event_before, Event_after, Event_pseudo };
struct DebugEventKind {
  DebugEventKindK k;
  TypeExpr* after_type = nullptr;  // Event_after
};

enum class DebugEventInfoK : std::uint8_t { Event_function, Event_return, Event_other };
struct DebugEventInfo {
  DebugEventInfoK k;
  long return_arity = 0;  // Event_return
};

enum class DebugEventReprK : std::uint8_t { Event_none, Event_parent, Event_child };
struct DebugEventRepr {
  DebugEventReprK k = DebugEventReprK::Event_none;
  lambda::IntRef* ref = nullptr;  // Event_parent / Event_child
};

struct DebugEvent {
  long ev_pos;                   // mutable: position in bytecode
  std::string_view ev_module;    // name of defining module
  Location ev_loc;               // location in source file
  DebugEventKind ev_kind;        // before/after event
  std::string_view ev_defname;   // enclosing definition
  DebugEventInfo ev_info;        // extra information
  const env::Summary* ev_typenv; // typing environment
  subst::t ev_typsubst;          // substitution over types
  CompilationEnv ev_compenv;     // compilation environment
  long ev_stacksize;             // size of stack frame
  DebugEventRepr ev_repr;        // position of the representative
};

// ---- optimization hints ---------------------------------------------------------

struct ClosureHint {
  Slice<lambda::ValueKind> params;
  lambda::ValueKind return_;
  lambda::InlineAttribute inline_;
  lambda::SpecialiseAttribute specialise;
  bool is_a_functor;
};

struct CcallHint {
  enum class K : std::uint8_t { Hint_unsafe, Hint_int, Hint_bigarray, Hint_primitive } k;
  BoxedInteger bi = BoxedInteger::Pnativeint;  // Hint_int
  bool unsafe = false;                                          // Hint_bigarray
  lambda::BigarrayKind elt_kind = lambda::BigarrayKind::Pbigarray_unknown;
  lambda::BigarrayLayout layout = lambda::BigarrayLayout::Pbigarray_unknown_layout;
  const PrimitiveDescription* prim = nullptr;                   // Hint_primitive
};

struct OptimizationHint {
  enum class K : std::uint8_t {
    Hint_immutable_block, Hint_arraylength, Hint_closures, Hint_ccall, Hint_physical_comparison
  } k;
  lambda::ArrayKind array = lambda::ArrayKind::Pgenarray;  // Hint_arraylength
  Slice<ClosureHint> closures;                             // Hint_closures
  CcallHint ccall{};                                       // Hint_ccall
};

// ---- instructions -------------------------------------------------------------------

using label = long;

enum class IK : std::uint8_t {
  Klabel, Kacc, Kenvacc, Kpush, Kpop, Kassign, Kpush_retaddr, Kapply, Kappterm, Kreturn,
  Krestart, Kgrab, Kclosure, Kclosurerec, Koffsetclosure, Kgetglobal, Ksetglobal, Kconst,
  Kmakeblock, Kmakefloatblock, Kgetfield, Ksetfield, Kgetfloatfield, Ksetfloatfield,
  Kvectlength, Kgetvectitem, Ksetvectitem, Kgetstringchar, Kgetbyteschar, Ksetbyteschar,
  Kbranch, Kbranchif, Kbranchifnot, Kstrictbranchif, Kstrictbranchifnot, Kswitch, Kboolnot,
  Kpushtrap, Kpoptrap, Kraise, Kcheck_signals, Kccall,
  Knegint, Kaddint, Ksubint, Kmulint, Kdivint, Kmodint,
  Kandint, Korint, Kxorint, Klslint, Klsrint, Kasrint,
  Kintcomp, Kphyscomp, Koffsetint, Koffsetref, Kisint, Kisout,
  Kgetmethod, Kgetpubmet, Kgetdynmet, Kevent, Kperform, Kresume, Kresumeterm,
  Kreperformterm, Kstop
};

struct ClosureLabel {  // label * closure_hint
  label lbl;
  const ClosureHint* hint;
};

// One instruction.  The operand fields a variant uses:
//   n: Klabel/Kpush_retaddr/Kbranch*/Kpushtrap (the label), Kacc, Kenvacc, Kpop,
//      Kassign, Kapply, Kappterm (nargs), Kreturn, Kgrab, Kclosure (the label),
//      Kclosurerec (the free-variable count), Koffsetclosure, Kmakeblock (size),
//      Kmakefloatblock (size), Kget/setfield, Kget/setfloatfield, Kccall (arity),
//      Koffsetint, Koffsetref, Kgetpubmet, Kresumeterm, Kreperformterm
//   m: Kappterm (slot size), Kclosure (the free-variable count), Kmakeblock (tag)
struct Instruction {
  IK k;
  long n = 0;
  long m = 0;
  MutableFlag mut = MutableFlag::Immutable;         // Kmakeblock / Kmakefloatblock
  const ClosureHint* closure_hint = nullptr;                        // Kclosure
  Slice<ClosureLabel> closures;                                     // Kclosurerec
  Ident::t id = nullptr;                                            // Kgetglobal / Ksetglobal
  const lambda::StructuredConstant* cst = nullptr;                  // Kconst
  lambda::ArrayKind array = lambda::ArrayKind::Pgenarray;           // Kvectlength
  Slice<label> sw_consts, sw_blocks;                                // Kswitch
  lambda::RaiseKind raise = lambda::RaiseKind::Raise_regular;       // Kraise
  std::string_view prim;                                            // Kccall
  const CcallHint* ccall_hint = nullptr;                            // Kccall (option)
  lambda::IntegerComparison icmp = lambda::IntegerComparison::Ceq;  // Kintcomp
  lambda::PhysicalComparison pcmp = lambda::PhysicalComparison::CPeq;  // Kphyscomp
  DebugEvent* event = nullptr;                                      // Kevent
};

// instruction list: a persistent cons list in the zone (Bytegen inspects the
// head of its continuations and prepends; the tail is shared).
struct Cell {
  Instruction hd;
  const Cell* tl;
};
using code = const Cell*;  // nullptr = []

inline code cons(const Instruction& i, code c) { return make<Cell>(Cell{i, c}); }
inline Instruction instr(IK k, long n = 0) {
  Instruction i{};
  i.k = k;
  i.n = n;
  return i;
}

inline constexpr long immed_min = -0x40000000;
inline constexpr long immed_max = 0x3FFFFFFF;

}  // namespace cppcaml::typing::instruct
