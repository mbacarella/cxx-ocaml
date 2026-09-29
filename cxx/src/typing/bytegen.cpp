// Port of bytecomp/bytegen.ml (TYPECHECKER.md stage 10): translation of
// lambda terms to lists of instructions.
//
// The instruction lists are persistent cons lists (instruct.hpp): each
// comp_expr call receives its continuation and prepends to it.  OCaml
// evaluates a call's arguments right to left, so `comp_expr ... e1 sz
// (Kpush :: comp_expr ... e2 sz cont)` compiles e2 BEFORE e1 -- the order
// labels are allocated in.  Every continuation below is computed first, in
// its own statement, to keep that order.
#include "cppcaml/typing/bytegen.hpp"

#include <stdexcept>
#include <string>
#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/matching.hpp"
#include "cppcaml/typing/switch.hpp"

namespace cppcaml::typing::bytegen {

namespace {

using namespace instruct;
namespace L = typing::lambda;
using L::lambda;
using PK = L::Primitive::K;
using SCK = L::StructuredConstant::Kind;

[[noreturn]] void fatal_error(const std::string& msg) { throw std::logic_error(msg); }

// Config.stack_threshold / Config.stack_safety_margin
constexpr long stack_threshold = 32;
constexpr long stack_safety_margin = 6;

// ---- small constructors ---------------------------------------------------------

Instruction K(IK k, long n = 0) { return instr(k, n); }
Instruction kconst(const L::StructuredConstant* c) {
  Instruction i = instr(IK::Kconst);
  i.cst = c;
  return i;
}
Instruction kccall(std::string_view name, long arity, const CcallHint* hint) {
  Instruction i = instr(IK::Kccall, arity);
  i.prim = name;
  i.ccall_hint = hint;
  return i;
}
Instruction kintcomp(L::IntegerComparison c) {
  Instruction i = instr(IK::Kintcomp);
  i.icmp = c;
  return i;
}
Instruction kmakeblock(long size, long tag, MutableFlag mut) {
  Instruction i = instr(IK::Kmakeblock, size);
  i.m = tag;
  i.mut = mut;
  return i;
}
Instruction kevent(DebugEvent* ev) {
  Instruction i = instr(IK::Kevent);
  i.event = ev;
  return i;
}
bool is(code c, IK k) { return c && c->hd.k == k; }

// ---- Label generation ----

long label_counter = 0;

long new_label() { return ++label_counter; }

// ---- Operations on compilation environments ----

// (the records' identities are Marshal's sharing in the debug events:
// empty_env is one static record, every other construction a fresh one)
CompilationEnv empty_env() {
  static const void* const obj = [] {
    ZoneScope perm(permanent_zone());
    return fresh_identity();
  }();
  CompilationEnv e{};
  e.obj = obj;
  return e;
}

// Add a stack-allocated variable
CompilationEnv add_var(Ident::t id, long pos, CompilationEnv env) {
  return CompilationEnv{env.ce_stack.add(id, pos), env.ce_closure, fresh_identity()};
}

CompilationEnv add_vars(Slice<Ident::t> idlist, long pos, CompilationEnv env) {
  for (Ident::t id : idlist) env = add_var(id, pos++, env);
  return env;
}

// Compute the closure environment
template <class A, class F>
std::pair<ident::Tbl<A>, long> add_positions(ident::Tbl<A> entries, F pos_to_entry, long pos, long delta,
                                              Slice<Ident::t> ids) {
  for (Ident::t id : ids) {
    entries = entries.add(id, pos_to_entry(pos));
    pos += delta;
  }
  return {entries, pos};
}

// function_definition = Single_non_recursive | Multiple_recursive of Ident.t list
ident::Tbl<ClosureEntry> closure_entries(const Slice<Ident::t>* multiple_recursive, Slice<Ident::t> fvs) {
  ident::Tbl<ClosureEntry> funct_entries;
  long pos_end_functs;
  if (!multiple_recursive) {
    // No need to store the function in the environment, but we still need to
    // reserve a slot in the closure block
    pos_end_functs = 3;
  } else {
    auto r = add_positions(
        ident::Tbl<ClosureEntry>{}, [](long pos) { return ClosureEntry{ClosureEntry::K::Function, pos}; }, 0, 3,
        *multiple_recursive);
    funct_entries = r.first;
    pos_end_functs = r.second;
  }
  // [pos_end_functs] points after an eventual infix tag; the last function
  // needs none, so free variables start at [pos_end_functs - 1].
  auto r = add_positions(
      funct_entries, [](long pos) { return ClosureEntry{ClosureEntry::K::Free_variable, pos}; },
      pos_end_functs - 1, 1, fvs);
  return r.first;
}

// ---- Examination of the continuation ----

// Return a label to the beginning of the given continuation.
std::pair<label, code> label_code(code cont) {
  if (is(cont, IK::Kbranch) || is(cont, IK::Klabel)) return {cont->hd.n, cont};
  label lbl = new_label();
  return {lbl, cons(K(IK::Klabel, lbl), cont)};
}

// Return a branch to the continuation.
std::pair<Instruction, code> make_branch_2(std::optional<label> lbl, long n, code cont, code c) {
  for (;;) {
    if (is(c, IK::Kreturn)) return {K(IK::Kreturn, n + c->hd.n), cont};
    if (is(c, IK::Klabel)) {
      c = c->tl;
      continue;
    }
    if (is(c, IK::Kpop)) {
      n += c->hd.n;
      c = c->tl;
      continue;
    }
    if (lbl) return {K(IK::Kbranch, *lbl), cont};
    label l = new_label();
    return {K(IK::Kbranch, l), cons(K(IK::Klabel, l), cont)};
  }
}

std::pair<Instruction, code> make_branch(code cont) {
  if (is(cont, IK::Kbranch) || is(cont, IK::Kreturn) || is(cont, IK::Kraise)) return {cont->hd, cont};
  if (is(cont, IK::Klabel)) return make_branch_2(cont->hd.n, 0, cont, cont);
  return make_branch_2(std::nullopt, 0, cont, cont);
}

// Avoid a branch to a label that follows immediately
code branch_to(label lbl, code cont) {
  if (is(cont, IK::Klabel) && cont->hd.n == lbl) return cont;
  return cons(K(IK::Kbranch, lbl), cont);
}

// Discard all instructions up to the next label.
code discard_dead_code(code c) {
  while (c && !(c->hd.k == IK::Klabel || c->hd.k == IK::Krestart || c->hd.k == IK::Ksetglobal)) c = c->tl;
  return c;
}

// Check if we're in tailcall position
bool is_tailcall(code c) {
  for (;;) {
    if (is(c, IK::Kreturn)) return true;
    if (is(c, IK::Klabel) || is(c, IK::Kpop)) {
      c = c->tl;
      continue;
    }
    return false;
  }
}

// Will this primitive result in an OCaml call which would benefit from the
// tail call optimization?
bool preserve_tailcall_for_prim(const L::Primitive& p) {
  switch (p.kind) {
    case PK::Popaque: case PK::Psequor: case PK::Psequand:
    case PK::Prunstack: case PK::Pperform: case PK::Presume: case PK::Preperform: case PK::Ppoll:
      return true;
    default:
      return false;
  }
}

// Add a Kpop N instruction in front of a continuation
code add_pop(long n, code cont) {
  for (;;) {
    if (n == 0) return cont;
    if (is(cont, IK::Kpop)) {
      n += cont->hd.n;
      cont = cont->tl;
      continue;
    }
    if (is(cont, IK::Kreturn)) return cons(K(IK::Kreturn, n + cont->hd.n), cont->tl);
    if (is(cont, IK::Kraise)) return cont;
    return cons(K(IK::Kpop, n), cont);
  }
}

// Add the constant "unit" in front of a continuation
code add_const_unit(code cont) {
  if (is(cont, IK::Kacc) || is(cont, IK::Kconst) || is(cont, IK::Kgetglobal) || is(cont, IK::Kpush_retaddr))
    return cont;
  return cons(kconst(L::const_unit()), cont);
}

code push_dummies(long n, code k) {
  // Kconst const_unit :: Kpush :: push_dummies (n-1) k
  for (; n > 0; --n) k = cons(kconst(L::const_unit()), cons(K(IK::Kpush), k));
  return k;
}

// ---- Merging consecutive events ----

DebugEvent* copy_event(const DebugEvent* ev, DebugEventKind kind, DebugEventInfo info, DebugEventRepr repr) {
  DebugEvent* e = make<DebugEvent>(*ev);
  e->ev_pos = 0;  // patched in emitcode
  e->ev_kind = kind;
  e->ev_info = info;
  e->ev_repr = repr;
  return e;
}

DebugEventInfo merge_infos(const DebugEvent* ev, const DebugEvent* ev2) {
  if (ev->ev_info.k == DebugEventInfoK::Event_other) return ev2->ev_info;
  if (ev2->ev_info.k == DebugEventInfoK::Event_other) return ev->ev_info;
  fatal_error("Bytegen.merge_infos");
}

DebugEventRepr merge_repr(const DebugEvent* ev, const DebugEvent* ev2) {
  const DebugEventRepr& a = ev->ev_repr;
  const DebugEventRepr& b = ev2->ev_repr;
  using RK = DebugEventReprK;
  if (a.k == RK::Event_none) return b;
  if (b.k == RK::Event_none) return a;
  if (a.k == RK::Event_parent && b.k == RK::Event_child && a.ref == b.ref && a.ref->contents == 1)
    return DebugEventRepr{};
  if (a.k == RK::Event_child && b.k == RK::Event_parent && a.ref == b.ref) return DebugEventRepr{RK::Event_parent, a.ref};
  fatal_error("Bytegen.merge_repr");
}

DebugEvent* merge_events_(DebugEvent* ev, DebugEvent* ev2) {
  using EK = DebugEventKindK;
  DebugEvent* maj;
  DebugEvent* min;
  if (ev->ev_kind.k == EK::Event_pseudo) {  // Discard pseudo-events
    maj = ev2, min = ev;
  } else if (ev2->ev_kind.k == EK::Event_pseudo) {
    maj = ev, min = ev2;
  } else if (ev->ev_kind.k == EK::Event_before) {  // Keep following event, supposedly more informative
    maj = ev2, min = ev;
  } else {  // Event_after: discard following events, supposedly less informative
    maj = ev, min = ev2;
  }
  // copy_event maj maj.ev_kind (merge_infos maj min) (merge_repr maj min):
  // arguments right to left
  DebugEventRepr repr = merge_repr(maj, min);
  DebugEventInfo info = merge_infos(maj, min);
  return copy_event(maj, maj->ev_kind, info, repr);
}

code weaken_event(DebugEvent* ev, code cont) {
  if (ev->ev_kind.k == DebugEventKindK::Event_after) {
    if (is(cont, IK::Kpush) && is(cont->tl, IK::Kevent) &&
        cont->tl->hd.event->ev_repr.k == DebugEventReprK::Event_none) {
      DebugEvent* ev2 = cont->tl->hd.event;
      code c = cont->tl->tl;
      if (ev->ev_info.k == DebugEventInfoK::Event_return) {
        // Weaken event
        auto* repr = make<L::IntRef>(L::IntRef{1});
        DebugEvent* e1 = copy_event(ev, DebugEventKind{DebugEventKindK::Event_pseudo}, ev->ev_info,
                                    DebugEventRepr{DebugEventReprK::Event_parent, repr});
        DebugEvent* e2 = copy_event(ev2, ev2->ev_kind, ev2->ev_info, DebugEventRepr{DebugEventReprK::Event_child, repr});
        return cons(kevent(e1), cons(K(IK::Kpush), cons(kevent(e2), c)));
      }
      // Only keep following event, equivalent
      return cont;
    }
    return cons(kevent(ev), cont);
  }
  return cons(kevent(ev), cont);
}

code add_event(DebugEvent* ev, code cont) {
  if (is(cont, IK::Kevent)) return weaken_event(merge_events_(ev, cont->hd.event), cont->tl);
  return weaken_event(ev, cont);
}

const env::Summary* env_empty_summary() {
  static const env::Summary s{env::Summary::Kind::Env_empty};
  return &s;
}

// Pseudo events are ignored by the debugger. They are only used for
// generating backtraces.
code add_pseudo_event(const L::ScopedLocation& loc, std::string_view modname, code c) {
  if (!clflags::debug) return c;
  std::string_view ev_defname = debuginfo::string_of_scoped_location(loc);
  DebugEvent* ev = make<DebugEvent>();
  ev->ev_pos = 0;  // patched in emitcode
  ev->ev_module = modname;
  ev->ev_loc = debuginfo::to_location(loc);
  ev->ev_defname = ev_defname;
  ev->ev_kind = DebugEventKind{DebugEventKindK::Event_pseudo};
  ev->ev_info = DebugEventInfo{DebugEventInfoK::Event_other};  // Dummy
  ev->ev_typenv = env_empty_summary();                          // Dummy
  ev->ev_typsubst = subst::identity();                          // Dummy
  ev->ev_compenv = empty_env();                                 // Dummy
  ev->ev_stacksize = 0;                                         // Dummy
  ev->ev_repr = DebugEventRepr{};                               // Dummy
  return add_event(ev, c);
}

// ---- Compilation of a lambda expression ----

struct TryBlocks {  // int list (compared physically)
  long sz;
  const TryBlocks* next;
};
struct StaticRaise {  // (int * (int * int * int list)) list
  long i;
  label lbl;
  long size;
  const TryBlocks* tb;
  const StaticRaise* next;
};
struct StackInfo {
  const TryBlocks* try_blocks = nullptr;         // stack size for each nested try block
  const StaticRaise* sz_static_raises = nullptr;  // staticraise numbers -> (lbl, stack size, try_blocks)
  long* max_stack_used = nullptr;                 // maximal stack size reached in the current body
};

StackInfo create_stack_info() { return StackInfo{nullptr, nullptr, make<long>(0)}; }

StackInfo push_static_raise(const StackInfo& si, long i, label lbl_handler, long sz) {
  StackInfo r = si;
  r.sz_static_raises = make<StaticRaise>(StaticRaise{i, lbl_handler, sz, si.try_blocks, si.sz_static_raises});
  return r;
}

const StaticRaise& find_raise_label(const StackInfo& si, long i) {
  for (const StaticRaise* r = si.sz_static_raises; r; r = r->next)
    if (r->i == i) return *r;
  fatal_error("exit(" + std::to_string(i) + ") outside appropriated catch");
}

// Will the translation of l lead to a jump to label ?
std::optional<label> code_as_jump(const StackInfo& si, lambda l, long sz) {
  if (auto* r = L::as<L::Lstaticraise>(l); r && r->args.empty()) {
    const StaticRaise& f = find_raise_label(si, r->i);
    if (sz == f.size && f.tb == si.try_blocks) return f.lbl;
  }
  return std::nullopt;
}

// Function bodies that remain to be compiled
struct FunctionToCompile {
  Slice<Ident::t> params;          // function parameters
  lambda body;                     // the function body
  label lbl;                       // the label of the function entry
  ident::Tbl<ClosureEntry> entries;  // offsets of the free variables and mutually recursive functions
  long rec_pos;                    // rank in recursive definition
};

std::vector<FunctionToCompile> functions_to_compile;  // a Stack

// Name of current compilation unit (for debugging events)
std::string_view compunit_name = "";

void check_stack(const StackInfo& si, long sz) {
  long* curr = si.max_stack_used;
  if (sz > *curr) *curr = sz;
}

// Translate a primitive to a bytecode instruction (possibly a call to a C function)

Instruction comp_bint_primitive(BoxedInteger bi, std::string_view suff, Slice<lambda> args) {
  std::string_view pref = bi == BoxedInteger::Pnativeint ? "caml_nativeint_"
                          : bi == BoxedInteger::Pint32   ? "caml_int32_"
                                                         : "caml_int64_";
  return kccall(zstr(std::string(pref) + std::string(suff)), static_cast<long>(args.size()), nullptr);
}

const CcallHint* hint_unsafe(bool unsafe) {
  if (!unsafe) return nullptr;
  return make<CcallHint>(CcallHint{CcallHint::K::Hint_unsafe});
}

std::string_view primitive_native_name(const PrimitiveDescription* p) {
  return !p->prim_native_name.empty() ? p->prim_native_name : p->prim_name;
}

Instruction comp_primitive(const StackInfo& si, const L::Primitive& p, long sz, Slice<lambda> args) {
  check_stack(si, sz);
  switch (p.kind) {
    case PK::Pgetglobal: {
      Instruction i = K(IK::Kgetglobal);
      i.id = p.id;
      return i;
    }
    case PK::Psetglobal: {
      Instruction i = K(IK::Ksetglobal);
      i.id = p.id;
      return i;
    }
    case PK::Pintcomp: return kintcomp(p.icmp);
    case PK::Pphyscomp: {
      Instruction i = K(IK::Kphyscomp);
      i.pcmp = p.pcmp;
      return i;
    }
    case PK::Pcompare_ints: return kccall("caml_int_compare", 2, nullptr);
    case PK::Pcompare_floats: return kccall("caml_float_compare", 2, nullptr);
    case PK::Pcompare_bints: return comp_bint_primitive(p.bi, "compare", args);
    case PK::Pfield: return K(IK::Kgetfield, p.n);
    case PK::Pfield_computed: return K(IK::Kgetvectitem);
    case PK::Psetfield: return K(IK::Ksetfield, p.n);
    case PK::Psetfield_computed: return K(IK::Ksetvectitem);
    case PK::Psetfloatfield: return K(IK::Ksetfloatfield, p.n);
    case PK::Pduprecord: return kccall("caml_obj_dup", 1, nullptr);
    case PK::Pccall: {
      const PrimitiveDescription* d = p.ccall;
      // If we have a native name, we probably have a specialized
      // representation of arguments/result.
      const CcallHint* hint = nullptr;
      if (primitive_native_name(d) != d->prim_name) {
        auto* h = make<CcallHint>(CcallHint{CcallHint::K::Hint_primitive});
        h->prim = d;
        hint = h;
      }
      return kccall(d->prim_name, d->prim_arity, hint);
    }
    case PK::Pperform:
      check_stack(si, sz + 4);
      return K(IK::Kperform);
    case PK::Pnegint: return K(IK::Knegint);
    case PK::Paddint: return K(IK::Kaddint);
    case PK::Psubint: return K(IK::Ksubint);
    case PK::Pmulint: return K(IK::Kmulint);
    case PK::Pdivint: return K(IK::Kdivint);
    case PK::Pmodint: return K(IK::Kmodint);
    case PK::Pandint: return K(IK::Kandint);
    case PK::Porint: return K(IK::Korint);
    case PK::Pxorint: return K(IK::Kxorint);
    case PK::Plslint: return K(IK::Klslint);
    case PK::Plsrint: return K(IK::Klsrint);
    case PK::Pasrint: return K(IK::Kasrint);
    case PK::Poffsetint: return K(IK::Koffsetint, p.n);
    case PK::Poffsetref: return K(IK::Koffsetref, p.n);
    case PK::Pintoffloat: return kccall("caml_int_of_float", 1, nullptr);
    case PK::Pfloatofint: return kccall("caml_float_of_int", 1, nullptr);
    case PK::Pnegfloat: return kccall("caml_neg_float", 1, nullptr);
    case PK::Pabsfloat: return kccall("caml_abs_float", 1, nullptr);
    case PK::Paddfloat: return kccall("caml_add_float", 2, nullptr);
    case PK::Psubfloat: return kccall("caml_sub_float", 2, nullptr);
    case PK::Pmulfloat: return kccall("caml_mul_float", 2, nullptr);
    case PK::Pdivfloat: return kccall("caml_div_float", 2, nullptr);
    case PK::Pstringlength: return kccall("caml_ml_string_length", 1, nullptr);
    case PK::Pbyteslength: return kccall("caml_ml_bytes_length", 1, nullptr);
    case PK::Pstringrefs: return kccall("caml_string_get", 2, nullptr);
    case PK::Pbytesrefs: return kccall("caml_bytes_get", 2, nullptr);
    case PK::Pbytessets: return kccall("caml_bytes_set", 3, nullptr);
    case PK::Pstringrefu: return K(IK::Kgetstringchar);
    case PK::Pbytesrefu: return K(IK::Kgetbyteschar);
    case PK::Pbytessetu: return K(IK::Ksetbyteschar);
    case PK::Pstring_load_16: return kccall("caml_string_get16", 2, hint_unsafe(p.unsafe));
    case PK::Pstring_load_32: return kccall("caml_string_get32", 2, hint_unsafe(p.unsafe));
    case PK::Pstring_load_64: return kccall("caml_string_get64", 2, hint_unsafe(p.unsafe));
    case PK::Pbytes_set_16: return kccall("caml_bytes_set16", 3, hint_unsafe(p.unsafe));
    case PK::Pbytes_set_32: return kccall("caml_bytes_set32", 3, hint_unsafe(p.unsafe));
    case PK::Pbytes_set_64: return kccall("caml_bytes_set64", 3, hint_unsafe(p.unsafe));
    case PK::Pbytes_load_16: return kccall("caml_bytes_get16", 2, hint_unsafe(p.unsafe));
    case PK::Pbytes_load_32: return kccall("caml_bytes_get32", 2, hint_unsafe(p.unsafe));
    case PK::Pbytes_load_64: return kccall("caml_bytes_get64", 2, hint_unsafe(p.unsafe));
    case PK::Parraylength: {
      Instruction i = K(IK::Kvectlength);
      i.array = p.array;
      return i;
    }
    case PK::Parrayrefs:
      return p.array == L::ArrayKind::Pgenarray     ? kccall("caml_array_get", 2, nullptr)
             : p.array == L::ArrayKind::Pfloatarray ? kccall("caml_floatarray_get", 2, nullptr)
                                                    : kccall("caml_array_get_addr", 2, nullptr);
    case PK::Parraysets:
      return p.array == L::ArrayKind::Pgenarray     ? kccall("caml_array_set", 3, nullptr)
             : p.array == L::ArrayKind::Pfloatarray ? kccall("caml_floatarray_set", 3, nullptr)
                                                    : kccall("caml_array_set_addr", 3, nullptr);
    case PK::Parrayrefu:
      return p.array == L::ArrayKind::Pgenarray     ? kccall("caml_array_unsafe_get", 2, nullptr)
             : p.array == L::ArrayKind::Pfloatarray ? kccall("caml_floatarray_unsafe_get", 2, nullptr)
                                                    : K(IK::Kgetvectitem);
    case PK::Parraysetu:
      return p.array == L::ArrayKind::Pgenarray     ? kccall("caml_array_unsafe_set", 3, nullptr)
             : p.array == L::ArrayKind::Pfloatarray ? kccall("caml_floatarray_unsafe_set", 3, nullptr)
                                                    : K(IK::Ksetvectitem);
    case PK::Pctconst: {
      std::string_view const_name;
      switch (p.ctconst) {
        case L::CompileTimeConstant::Big_endian: const_name = "big_endian"; break;
        case L::CompileTimeConstant::Word_size: const_name = "word_size"; break;
        case L::CompileTimeConstant::Int_size: const_name = "int_size"; break;
        case L::CompileTimeConstant::Max_wosize: const_name = "max_wosize"; break;
        case L::CompileTimeConstant::Ostype_unix: const_name = "ostype_unix"; break;
        case L::CompileTimeConstant::Ostype_win32: const_name = "ostype_win32"; break;
        case L::CompileTimeConstant::Ostype_cygwin: const_name = "ostype_cygwin"; break;
        case L::CompileTimeConstant::Backend_type: const_name = "backend_type"; break;
        case L::CompileTimeConstant::Standard_library_default: const_name = "standard_library_default"; break;
      }
      return kccall(zstr("caml_sys_const_" + std::string(const_name)), 1, nullptr);
    }
    case PK::Pisint: return K(IK::Kisint);
    case PK::Pisout: return K(IK::Kisout);
    case PK::Pcheckbound: return kccall("caml_check_bound", 2, nullptr);
    case PK::Pbintofint: return comp_bint_primitive(p.bi, "of_int", args);
    case PK::Pintofbint: return comp_bint_primitive(p.bi, "to_int", args);
    case PK::Pcvtbint: {
      using B = BoxedInteger;
      B src = p.bi, dst = p.bi2;
      if (src == B::Pint32 && dst == B::Pnativeint) return kccall("caml_nativeint_of_int32", 1, nullptr);
      if (src == B::Pnativeint && dst == B::Pint32) return kccall("caml_nativeint_to_int32", 1, nullptr);
      if (src == B::Pint32 && dst == B::Pint64) return kccall("caml_int64_of_int32", 1, nullptr);
      if (src == B::Pint64 && dst == B::Pint32) return kccall("caml_int64_to_int32", 1, nullptr);
      if (src == B::Pnativeint && dst == B::Pint64) return kccall("caml_int64_of_nativeint", 1, nullptr);
      if (src == B::Pint64 && dst == B::Pnativeint) return kccall("caml_int64_to_nativeint", 1, nullptr);
      fatal_error("Bytegen.comp_primitive: invalid Pcvtbint cast");
    }
    case PK::Pnegbint: return comp_bint_primitive(p.bi, "neg", args);
    case PK::Paddbint: return comp_bint_primitive(p.bi, "add", args);
    case PK::Psubbint: return comp_bint_primitive(p.bi, "sub", args);
    case PK::Pmulbint: return comp_bint_primitive(p.bi, "mul", args);
    case PK::Pdivbint: return comp_bint_primitive(p.bi, "div", args);
    case PK::Pmodbint: return comp_bint_primitive(p.bi, "mod", args);
    case PK::Pandbint: return comp_bint_primitive(p.bi, "and", args);
    case PK::Porbint: return comp_bint_primitive(p.bi, "or", args);
    case PK::Pxorbint: return comp_bint_primitive(p.bi, "xor", args);
    case PK::Plslbint: return comp_bint_primitive(p.bi, "shift_left", args);
    case PK::Plsrbint: return comp_bint_primitive(p.bi, "shift_right_unsigned", args);
    case PK::Pasrbint: return comp_bint_primitive(p.bi, "shift_right", args);
    case PK::Pbintcomp: {
      auto* h = make<CcallHint>(CcallHint{CcallHint::K::Hint_int});
      h->bi = p.bi;
      std::string_view name;
      switch (p.icmp) {
        case L::IntegerComparison::Ceq: name = "caml_equal"; break;
        case L::IntegerComparison::Cne: name = "caml_notequal"; break;
        case L::IntegerComparison::Clt: name = "caml_lessthan"; break;
        case L::IntegerComparison::Cgt: name = "caml_greaterthan"; break;
        case L::IntegerComparison::Cle: name = "caml_lessequal"; break;
        case L::IntegerComparison::Cge: name = "caml_greaterequal"; break;
      }
      return kccall(name, 2, h);
    }
    case PK::Pbigarrayref:
    case PK::Pbigarrayset: {
      auto* h = make<CcallHint>(CcallHint{CcallHint::K::Hint_bigarray});
      h->unsafe = p.unsafe;
      h->elt_kind = p.ba_kind;
      h->layout = p.ba_layout;
      bool set = p.kind == PK::Pbigarrayset;
      return kccall(zstr((set ? "caml_ba_set_" : "caml_ba_get_") + std::to_string(p.n)), p.n + (set ? 2 : 1), h);
    }
    case PK::Pbigarraydim: return kccall(zstr("caml_ba_dim_" + std::to_string(p.n)), 1, nullptr);
    case PK::Pbigstring_load_16: return kccall("caml_ba_uint8_get16", 2, hint_unsafe(p.unsafe));
    case PK::Pbigstring_load_32: return kccall("caml_ba_uint8_get32", 2, hint_unsafe(p.unsafe));
    case PK::Pbigstring_load_64: return kccall("caml_ba_uint8_get64", 2, hint_unsafe(p.unsafe));
    case PK::Pbigstring_set_16: return kccall("caml_ba_uint8_set16", 3, hint_unsafe(p.unsafe));
    case PK::Pbigstring_set_32: return kccall("caml_ba_uint8_set32", 3, hint_unsafe(p.unsafe));
    case PK::Pbigstring_set_64: return kccall("caml_ba_uint8_set64", 3, hint_unsafe(p.unsafe));
    case PK::Pbswap16: return kccall("caml_bswap16", 1, nullptr);
    case PK::Pbbswap: return comp_bint_primitive(p.bi, "bswap", args);
    case PK::Pint_as_pointer: return kccall("caml_int_as_pointer", 1, nullptr);
    case PK::Pbytes_to_string: return kccall("caml_string_of_bytes", 1, nullptr);
    case PK::Pbytes_of_string: return kccall("caml_bytes_of_string", 1, nullptr);
    case PK::Patomic_load: return kccall("caml_atomic_load_field", 2, nullptr);
    case PK::Patomic_fetch_add: return kccall("caml_atomic_fetch_add_field", 3, nullptr);
    case PK::Pdls_get: return kccall("caml_domain_dls_get", 1, nullptr);
    case PK::Ppoll: return kccall("caml_process_pending_actions_with_root", 1, nullptr);
    // The cases below are handled in [comp_expr] before the [comp_primitive] call
    case PK::Prunstack: case PK::Presume: case PK::Preperform:
    case PK::Pignore: case PK::Popaque:
    case PK::Pnot: case PK::Psequand: case PK::Psequor:
    case PK::Praise:
    case PK::Pmakearray: case PK::Pduparray:
    case PK::Pfloatcomp:
    case PK::Pmakeblock:
    case PK::Pmakelazyblock:
    case PK::Pfloatfield:
      fatal_error("Bytegen.comp_primitive");
  }
  fatal_error("Bytegen.comp_primitive");
}

const ClosureHint* closure_hint(const L::LFunction* f) {
  std::vector<L::ValueKind> params;
  for (const L::Param& p : f->params) params.push_back(p.kind);
  return make<ClosureHint>(
      ClosureHint{slice(params), f->return_, f->attr.inline_, f->attr.specialise, f->attr.is_a_functor});
}

bool is_immed(long n) { return immed_min <= n && n <= immed_max; }

// Storer = Switch.Store (lambda keys, Stdlib.compare, Lambda.make_key)
struct StoredLambda {
  using t = lambda;
  using key = lambda;
  static std::optional<key> make_key(const t& l) { return L::make_key(l); }
  static bool same_key(const key& a, const key& b) { return L::equal_lambda(a, b); }
};
using Storer = switch_::Store<StoredLambda>;

bool is_const_int(lambda l, long* n) {
  auto* c = L::as<L::Lconst>(l);
  if (!c || c->c->kind != SCK::Const_int) return false;
  *n = c->c->i;
  return true;
}

code comp_expr(const StackInfo& si, const CompilationEnv& env, lambda exp, long sz, code cont);
code comp_args(const StackInfo& si, const CompilationEnv& env, Slice<lambda> argl, long sz, code cont);
code comp_expr_list(const StackInfo& si, const CompilationEnv& env, const std::vector<lambda>& exprl,
                    std::size_t from, long sz, code cont);
code comp_exit_args(const StackInfo& si, const CompilationEnv& env, Slice<lambda> argl, long sz, long pos,
                    code cont);
code comp_binary_test(const StackInfo& si, const CompilationEnv& env, lambda cond, lambda ifso, lambda ifnot,
                      long sz, code cont);

code comp_args_v(const StackInfo& si, const CompilationEnv& env, const std::vector<lambda>& argl, long sz,
                 code cont) {
  return comp_args(si, env, slice(argl), sz, cont);
}

std::vector<lambda> lvars(const std::vector<Ident::t>& ids) {
  std::vector<lambda> r;
  r.reserve(ids.size());
  for (Ident::t id : ids) r.push_back(L::lvar(id));
  return r;
}

// Compile an expression.
// The value of the expression is left in the accumulator.
//   env = compilation environment
//   exp = the lambda expression to compile
//   sz = current size of the stack frame
//   cont = list of instructions to execute afterwards
// Result = list of instructions that evaluate exp, then perform cont.
code comp_expr(const StackInfo& si, const CompilationEnv& env, lambda exp, long sz, code cont) {
  check_stack(si, sz);
  switch (exp->kind) {
    case L::LK::Lvar:
    case L::LK::Lmutvar: {
      Ident::t id = exp->kind == L::LK::Lvar ? L::as<L::Lvar>(exp)->id : L::as<L::Lmutvar>(exp)->id;
      if (const long* pos = env.ce_stack.find_same_opt(id)) return cons(K(IK::Kacc, sz - *pos), cont);
      auto not_found = [&]() -> code { fatal_error("Bytegen.comp_expr: var " + ident::unique_name(id)); };
      if (!env.ce_closure.in_closure) return not_found();
      const ClosureEntry* e = env.ce_closure.entries.find_same_opt(id);
      if (!e) return not_found();
      if (e->k == ClosureEntry::K::Free_variable) return cons(K(IK::Kenvacc, e->pos - env.ce_closure.env_pos), cont);
      return cons(K(IK::Koffsetclosure, e->pos - env.ce_closure.env_pos), cont);
    }
    case L::LK::Lconst:
      return cons(kconst(L::as<L::Lconst>(exp)->c), cont);
    case L::LK::Lapply: {
      const L::LambdaApply& ap = L::as<L::Lapply>(exp)->ap;
      Slice<lambda> args = ap.ap_args;
      long nargs = static_cast<long>(args.size());
      if (is_tailcall(cont)) {
        Instruction at = K(IK::Kappterm, nargs);
        at.m = sz + nargs;
        code c1 = cons(at, discard_dead_code(cont));
        code c2 = comp_expr(si, env, ap.ap_func, sz + nargs, c1);
        return comp_args(si, env, args, sz, cons(K(IK::Kpush), c2));
      }
      if (nargs < 4) {
        code c2 = comp_expr(si, env, ap.ap_func, sz + nargs, cons(K(IK::Kapply, nargs), cont));
        return comp_args(si, env, args, sz, cons(K(IK::Kpush), c2));
      }
      auto [lbl, cont1] = label_code(cont);
      code c2 = comp_expr(si, env, ap.ap_func, sz + 3 + nargs, cons(K(IK::Kapply, nargs), cont1));
      code c3 = comp_args(si, env, args, sz + 3, cons(K(IK::Kpush), c2));
      return cons(K(IK::Kpush_retaddr, lbl), c3);
    }
    case L::LK::Lsend: {
      auto* s = L::as<L::Lsend>(exp);
      if (s->k == L::MethKind::Cached) fatal_error("Bytegen.comp_expr: Lsend Cached");
      long nargs = static_cast<long>(s->args.size()) + 1;
      Instruction getmethod;
      std::vector<lambda> args2;
      long n;
      if (s->k == L::MethKind::Self) {
        getmethod = K(IK::Kgetmethod);
        args2 = {s->met, s->obj};
      } else if (is_const_int(s->met, &n)) {
        getmethod = K(IK::Kgetpubmet, n);
        args2 = {s->obj};
      } else {
        getmethod = K(IK::Kgetdynmet);
        args2 = {s->met, s->obj};
      }
      for (lambda a : s->args) args2.push_back(a);
      if (is_tailcall(cont)) {
        Instruction at = K(IK::Kappterm, nargs);
        at.m = sz + nargs;
        return comp_args_v(si, env, args2, sz, cons(getmethod, cons(at, discard_dead_code(cont))));
      }
      if (nargs < 4) return comp_args_v(si, env, args2, sz, cons(getmethod, cons(K(IK::Kapply, nargs), cont)));
      auto [lbl, cont1] = label_code(cont);
      code c = comp_args_v(si, env, args2, sz + 3, cons(getmethod, cons(K(IK::Kapply, nargs), cont1)));
      return cons(K(IK::Kpush_retaddr, lbl), c);
    }
    case L::LK::Lfunction: {  // assume kind = Curried
      const L::LFunction* lfun = L::as<L::Lfunction>(exp)->f;
      cont = add_pseudo_event(lfun->loc, compunit_name, cont);
      label lbl = new_label();
      L::IdentSet fvset = L::free_variables(exp);
      std::vector<Ident::t> fv(fvset.begin(), fvset.end());
      auto entries = closure_entries(nullptr, slice(fv));
      std::vector<Ident::t> params;
      for (const L::Param& p : lfun->params) params.push_back(p.id);
      functions_to_compile.push_back(FunctionToCompile{slice(params), lfun->body, lbl, entries, 0});
      Instruction cl = K(IK::Kclosure, lbl);
      cl.m = static_cast<long>(fv.size());
      cl.closure_hint = closure_hint(lfun);
      return comp_args_v(si, env, lvars(fv), sz, cons(cl, cont));
    }
    case L::LK::Llet:
    case L::LK::Lmutlet: {
      Ident::t id;
      lambda arg, body;
      if (auto* l = L::as<L::Llet>(exp)) {
        id = l->id, arg = l->arg, body = l->body;
      } else {
        auto* m = L::as<L::Lmutlet>(exp);
        id = m->id, arg = m->arg, body = m->body;
      }
      code c = comp_expr(si, add_var(id, sz + 1, env), body, sz + 1, add_pop(1, cont));
      return comp_expr(si, env, arg, sz, cons(K(IK::Kpush), c));
    }
    case L::LK::Lletrec: {
      auto* lr = L::as<L::Lletrec>(exp);
      Slice<L::RecBinding> decl = lr->decl;
      long ndecl = static_cast<long>(decl.size());
      L::IdentSet fvset = L::free_variables(L::lletrec(decl, L::lambda_unit()));
      std::vector<Ident::t> fv(fvset.begin(), fvset.end());
      std::vector<Ident::t> rec_idents_v;
      for (const L::RecBinding& b : decl) rec_idents_v.push_back(b.id);
      Slice<Ident::t> rec_idents = slice(rec_idents_v);
      auto entries = closure_entries(&rec_idents, slice(fv));
      std::vector<label> lbls;
      long pos = 0;
      for (const L::RecBinding& b : decl) {
        label lbl = new_label();
        std::vector<Ident::t> params;
        for (const L::Param& p : b.def->params) params.push_back(p.id);
        functions_to_compile.push_back(FunctionToCompile{slice(params), b.def->body, lbl, entries, pos});
        lbls.push_back(lbl);
        ++pos;
      }
      std::vector<ClosureLabel> lbl_hints;
      for (std::size_t k = 0; k < lbls.size(); ++k) lbl_hints.push_back(ClosureLabel{lbls[k], closure_hint(decl[k].def)});
      code body = comp_expr(si, add_vars(rec_idents, sz + 1, env), lr->body, sz + ndecl, add_pop(ndecl, cont));
      Instruction cr = K(IK::Kclosurerec, static_cast<long>(fv.size()));
      cr.closures = slice(lbl_hints);
      return comp_args_v(si, env, lvars(fv), sz, cons(cr, body));
    }
    case L::LK::Lprim: {
      auto* lp = L::as<L::Lprim>(exp);
      const L::Primitive& p = lp->p;
      Slice<lambda> args = lp->args;
      std::size_t na = args.size();
      switch (p.kind) {
        case PK::Popaque:
          if (na == 1) return comp_expr(si, env, args[0], sz, cont);
          break;
        case PK::Pignore:
          if (na == 1) return comp_expr(si, env, args[0], sz, add_const_unit(cont));
          break;
        case PK::Pnot:
          if (na == 1) {
            code newcont;
            if (is(cont, IK::Kbranchif))
              newcont = cons(K(IK::Kbranchifnot, cont->hd.n), cont->tl);
            else if (is(cont, IK::Kbranchifnot))
              newcont = cons(K(IK::Kbranchif, cont->hd.n), cont->tl);
            else
              newcont = cons(K(IK::Kboolnot), cont);
            return comp_expr(si, env, args[0], sz, newcont);
          }
          break;
        case PK::Psequand:
        case PK::Psequor:
          if (na == 2) {
            bool is_and = p.kind == PK::Psequand;
            IK same = is_and ? IK::Kbranchifnot : IK::Kbranchif;
            IK other = is_and ? IK::Kbranchif : IK::Kbranchifnot;
            IK strict = is_and ? IK::Kstrictbranchifnot : IK::Kstrictbranchif;
            lambda exp1 = args[0], exp2 = args[1];
            if (is(cont, same)) {
              code c = comp_expr(si, env, exp2, sz, cont);
              return comp_expr(si, env, exp1, sz, cons(K(same, cont->hd.n), c));
            }
            if (is(cont, other)) {
              label lbl = cont->hd.n;
              auto [lbl2, cont2] = label_code(cont->tl);
              code c = comp_expr(si, env, exp2, sz, cons(K(other, lbl), cont2));
              return comp_expr(si, env, exp1, sz, cons(K(same, lbl2), c));
            }
            auto [lbl, cont1] = label_code(cont);
            code c = comp_expr(si, env, exp2, sz, cont1);
            return comp_expr(si, env, exp1, sz, cons(K(strict, lbl), c));
          }
          break;
        case PK::Praise:
          if (na == 1) {
            Instruction r = K(IK::Kraise);
            r.raise = p.raise;
            return comp_expr(si, env, args[0], sz, cons(r, discard_dead_code(cont)));
          }
          break;
        case PK::Paddint:
        case PK::Psubint: {
          long n;
          if (na == 2 && is_const_int(args[1], &n)) {
            if (p.kind == PK::Paddint && is_immed(n))
              return comp_expr(si, env, args[0], sz, cons(K(IK::Koffsetint, n), cont));
            if (p.kind == PK::Psubint && is_immed(-n))
              return comp_expr(si, env, args[0], sz, cons(K(IK::Koffsetint, -n), cont));
          }
          break;
        }
        case PK::Poffsetint:
          if (na == 1 && !is_immed(p.n))
            return comp_expr(si, env, args[0], sz,
                             cons(K(IK::Kpush), cons(kconst(L::const_int(p.n)), cons(K(IK::Kaddint), cont))));
          break;
        case PK::Pmakearray: {
          cont = add_pseudo_event(lp->loc, compunit_name, cont);
          long n = static_cast<long>(na);
          switch (p.array) {
            case L::ArrayKind::Pintarray:
            case L::ArrayKind::Paddrarray:
              return comp_args(si, env, args, sz, cons(kmakeblock(n, 0, p.mut), cont));
            case L::ArrayKind::Pfloatarray: {
              Instruction mf = K(IK::Kmakefloatblock, n);
              mf.mut = p.mut;
              return comp_args(si, env, args, sz, cons(mf, cont));
            }
            case L::ArrayKind::Pgenarray:
              if (na == 0) return cons(kmakeblock(0, 0, p.mut), cont);
              return comp_args(si, env, args, sz,
                               cons(kmakeblock(n, 0, p.mut),
                                    cons(kccall("caml_array_of_uniform_array", 1, nullptr), cont)));
          }
          break;
        }
        case PK::Presume:
        case PK::Prunstack: {
          long nargs = static_cast<long>(na) - 1;
          if (nargs != 2) fatal_error("Bytegen: assertion failed (Presume/Prunstack arity)");
          if (is_tailcall(cont))
            // Resumeterm pushes no extra words
            return comp_args(si, env, args, sz, cons(K(IK::Kresumeterm, sz + nargs), discard_dead_code(cont)));
          // Resume itself pushes 3 words, and perform does not push any
          check_stack(si, sz + 3);
          return comp_args(si, env, args, sz, cons(K(IK::Kresume), cont));
        }
        case PK::Preperform: {
          long nargs = static_cast<long>(na) - 1;
          if (nargs != 1) fatal_error("Bytegen: assertion failed (Preperform arity)");
          // Reperformterm resets the stack before pushing 3 words
          check_stack(si, 3);
          if (is_tailcall(cont))
            return comp_args(si, env, args, sz, cons(K(IK::Kreperformterm, sz + nargs), discard_dead_code(cont)));
          fatal_error("Reperform used in non-tail position");
        }
        case PK::Pduparray: {
          if (na == 1) {
            if (auto* inner = L::as<L::Lprim>(args[0]); inner && inner->p.kind == PK::Pmakearray) {
              if (inner->p.array != p.array) fatal_error("Bytegen: assertion failed (Pduparray kind)");
              L::Primitive mk = L::prim(PK::Pmakearray);
              mk.array = p.array;
              mk.mut = p.mut;
              return comp_expr(si, env, L::lprim(mk, inner->args, lp->loc), sz, cont);
            }
            static const PrimitiveDescription* prim_obj_dup = [] {
              ZoneScope perm(permanent_zone());
              auto* d = make<PrimitiveDescription>();
              d->prim_name = "caml_obj_dup";
              d->prim_arity = 1;
              d->prim_alloc = true;
              d->prim_native_name = "";
              d->prim_native_repr_args = slice(std::vector<NativeRepr>(1, NativeRepr{}));
              d->prim_native_repr_res = NativeRepr{};
              return d;
            }();
            L::Primitive cc = L::prim(PK::Pccall);
            cc.ccall = prim_obj_dup;
            return comp_expr(si, env, L::lprim(cc, args, lp->loc), sz, cont);
          }
          fatal_error("Bytegen.comp_expr: Pduparray takes exactly one arg");
        }
        // Integer first for enabling further optimization (cf. emitcode.ml)
        case PK::Pintcomp:
          if (na == 2 && L::as<L::Lconst>(args[1])) {
            L::Primitive p2 = L::prim(PK::Pintcomp);
            p2.icmp = L::swap_integer_comparison(p.icmp);
            Slice<lambda> args2 = slice(std::vector<lambda>{args[1], args[0]});
            long nargs = 1;
            Instruction i = comp_primitive(si, p2, sz + nargs - 1, args2);
            return comp_args(si, env, args2, sz, cons(i, cont));
          }
          break;
        // Constant first for enabling further optimization (cf. emitcode.ml)
        case PK::Pphyscomp:
          if (na == 2 && L::as<L::Lconst>(args[1])) {
            Slice<lambda> args2 = slice(std::vector<lambda>{args[1], args[0]});
            long nargs = 1;
            Instruction i = comp_primitive(si, p, sz + nargs - 1, args2);
            return comp_args(si, env, args2, sz, cons(i, cont));
          }
          break;
        case PK::Pfloatcomp: {
          using FC = L::FloatComparison;
          auto cc = [](std::string_view name) { return kccall(name, 2, nullptr); };
          code c;
          switch (p.fcmp) {
            case FC::CFeq: c = cons(cc("caml_eq_float"), cont); break;
            case FC::CFneq: c = cons(cc("caml_neq_float"), cont); break;
            case FC::CFlt: c = cons(cc("caml_lt_float"), cont); break;
            case FC::CFnlt: c = cons(cc("caml_lt_float"), cons(K(IK::Kboolnot), cont)); break;
            case FC::CFgt: c = cons(cc("caml_gt_float"), cont); break;
            case FC::CFngt: c = cons(cc("caml_gt_float"), cons(K(IK::Kboolnot), cont)); break;
            case FC::CFle: c = cons(cc("caml_le_float"), cont); break;
            case FC::CFnle: c = cons(cc("caml_le_float"), cons(K(IK::Kboolnot), cont)); break;
            case FC::CFge: c = cons(cc("caml_ge_float"), cont); break;
            case FC::CFnge: c = cons(cc("caml_ge_float"), cons(K(IK::Kboolnot), cont)); break;
          }
          return comp_args(si, env, args, sz, c);
        }
        case PK::Pmakeblock:
          cont = add_pseudo_event(lp->loc, compunit_name, cont);
          return comp_args(si, env, args, sz, cons(kmakeblock(static_cast<long>(na), p.n, p.mut), cont));
        case PK::Pmakelazyblock:
          if (na == 1) {
            cont = add_pseudo_event(lp->loc, compunit_name, cont);
            return comp_args(si, env, args, sz,
                             cons(kmakeblock(1, L::tag_of_lazy_tag(p.lazy_tag), MutableFlag::Mutable), cont));
          }
          break;
        case PK::Pfloatfield:
          cont = add_pseudo_event(lp->loc, compunit_name, cont);
          return comp_args(si, env, args, sz, cons(K(IK::Kgetfloatfield, p.n), cont));
        default:
          break;
      }
      long nargs = static_cast<long>(na) - 1;
      Instruction i = comp_primitive(si, p, sz + nargs - 1, args);
      return comp_args(si, env, args, sz, cons(i, cont));
    }
    case L::LK::Lstaticcatch: {
      auto* sc = L::as<L::Lstaticcatch>(exp);
      std::vector<Ident::t> vars_v;
      for (const L::Param& p : sc->params) vars_v.push_back(p.id);
      Slice<Ident::t> vars = slice(vars_v);
      long nvars = static_cast<long>(vars.size());
      auto [branch1, cont1] = make_branch(cont);
      if (nvars != 1) {  // general case
        code h = comp_expr(si, add_vars(vars, sz + 1, env), sc->handler, sz + nvars, add_pop(nvars, cont1));
        auto [lbl_handler, cont2] = label_code(h);
        StackInfo si2 = push_static_raise(si, sc->i, lbl_handler, sz + nvars);
        code b = comp_expr(si2, env, sc->body, sz + nvars, add_pop(nvars, cons(branch1, cont2)));
        return push_dummies(nvars, b);
      }
      // small optimization for nvars = 1
      Ident::t var = vars[0];
      code h = comp_expr(si, add_var(var, sz + 1, env), sc->handler, sz + 1, add_pop(1, cont1));
      auto [lbl_handler, cont2] = label_code(cons(K(IK::Kpush), h));
      StackInfo si2 = push_static_raise(si, sc->i, lbl_handler, sz);
      return comp_expr(si2, env, sc->body, sz, cons(branch1, cont2));
    }
    case L::LK::Lstaticraise: {
      auto* sr = L::as<L::Lstaticraise>(exp);
      cont = discard_dead_code(cont);
      const StaticRaise& f = find_raise_label(si, sr->i);
      cont = branch_to(f.lbl, cont);
      // loop sz tbb: built from the innermost try block outwards
      std::vector<const TryBlocks*> chain;
      for (const TryBlocks* tbb = si.try_blocks; tbb != f.tb; tbb = tbb->next) {
        if (!tbb) fatal_error("Bytegen: assertion failed (Lstaticraise try blocks)");
        chain.push_back(tbb);
      }
      // loop sz tbb = if tb == tbb then add_pop (sz-size) cont
      //   else add_pop (sz-try_sz-4) (Kpoptrap :: loop try_sz rest)
      long last_sz = chain.empty() ? sz : chain.back()->sz;
      code c = add_pop(last_sz - f.size, cont);
      for (std::size_t k = chain.size(); k-- > 0;) {
        long outer_sz = k == 0 ? sz : chain[k - 1]->sz;
        c = add_pop(outer_sz - chain[k]->sz - 4, cons(K(IK::Kpoptrap), c));
      }
      if (sr->args.size() == 1)  // optim, argument passed in accumulator
        return comp_expr(si, env, sr->args[0], sz, c);
      return comp_exit_args(si, env, sr->args, sz, f.size, c);
    }
    case L::LK::Ltrywith: {
      auto* tw = L::as<L::Ltrywith>(exp);
      auto [branch1, cont1] = make_branch(cont);
      label lbl_handler = new_label();
      code h = comp_expr(si, add_var(tw->exn, sz + 1, env), tw->handler, sz + 1, add_pop(1, cont1));
      code body_cont = cons(K(IK::Kpoptrap), cons(branch1, cons(K(IK::Klabel, lbl_handler), cons(K(IK::Kpush), h))));
      StackInfo si2 = si;
      si2.try_blocks = make<TryBlocks>(TryBlocks{sz, si.try_blocks});
      code l = comp_expr(si2, env, tw->body, sz + 4, body_cont);
      return cons(K(IK::Kpushtrap, lbl_handler), l);
    }
    case L::LK::Lifthenelse: {
      auto* ite = L::as<L::Lifthenelse>(exp);
      return comp_binary_test(si, env, ite->cond, ite->ifso, ite->ifnot, sz, cont);
    }
    case L::LK::Lsequence: {
      auto* s = L::as<L::Lsequence>(exp);
      code c = comp_expr(si, env, s->l2, sz, cont);
      return comp_expr(si, env, s->l1, sz, c);
    }
    case L::LK::Lwhile: {
      auto* w = L::as<L::Lwhile>(exp);
      label lbl_loop = new_label();
      label lbl_test = new_label();
      code c = comp_expr(si, env, w->cond, sz, cons(K(IK::Kbranchif, lbl_loop), add_const_unit(cont)));
      code b = comp_expr(si, env, w->body, sz, cons(K(IK::Klabel, lbl_test), c));
      return cons(K(IK::Kbranch, lbl_test), cons(K(IK::Klabel, lbl_loop), cons(K(IK::Kcheck_signals), b)));
    }
    case L::LK::Lfor: {
      auto* f = L::as<L::Lfor>(exp);
      label lbl_loop = new_label();
      label lbl_exit = new_label();
      bool upto = f->dir == parsetree::DirectionFlag::Upto;
      long offset = upto ? 1 : -1;
      L::IntegerComparison comp = upto ? L::IntegerComparison::Cgt : L::IntegerComparison::Clt;
      code tail = cons(K(IK::Kacc, 1),
                       cons(K(IK::Kpush),
                            cons(K(IK::Koffsetint, offset),
                                 cons(K(IK::Kassign, 2),
                                      cons(K(IK::Kacc, 1),
                                           cons(kintcomp(L::IntegerComparison::Cne),
                                                cons(K(IK::Kbranchif, lbl_loop),
                                                     cons(K(IK::Klabel, lbl_exit),
                                                          add_const_unit(add_pop(2, cont))))))))));
      code body = comp_expr(si, add_var(f->id, sz + 1, env), f->body, sz + 2, tail);
      code c = cons(K(IK::Kpush),
                    cons(K(IK::Kpush),
                         cons(K(IK::Kacc, 2),
                              cons(kintcomp(comp),
                                   cons(K(IK::Kbranchif, lbl_exit),
                                        cons(K(IK::Klabel, lbl_loop), cons(K(IK::Kcheck_signals), body)))))));
      code c2 = comp_expr(si, env, f->hi, sz + 1, c);
      return comp_expr(si, env, f->lo, sz, cons(K(IK::Kpush), c2));
    }
    case L::LK::Lswitch: {
      auto* s = L::as<L::Lswitch>(exp);
      const L::LambdaSwitch& sw = s->sw;
      auto [branch, cont1] = make_branch(cont);
      code c = discard_dead_code(cont1);
      // Build indirection vectors
      Storer store;
      std::vector<long> act_consts(static_cast<std::size_t>(sw.sw_numconsts), 0);
      std::vector<long> act_blocks(static_cast<std::size_t>(sw.sw_numblocks), 0);
      if (sw.sw_failaction) (void)store.act_store(sw.sw_failaction);  // default is index 0
      for (const L::SwitchCase& sc : sw.sw_consts) act_consts[static_cast<std::size_t>(sc.key)] = store.act_store(sc.action);
      for (const L::SwitchCase& sc : sw.sw_blocks) act_blocks[static_cast<std::size_t>(sc.key)] = store.act_store(sc.action);
      // Compile and label actions
      std::vector<lambda> acts = store.act_get();
      std::vector<label> lbls(acts.size(), 0);
      for (std::size_t i = acts.size(); i-- > 0;) {
        auto [lbl, c1] = label_code(comp_expr(si, env, acts[i], sz, cons(branch, c)));
        lbls[i] = lbl;
        c = discard_dead_code(c1);
      }
      // Build label vectors
      std::vector<label> lbl_blocks(static_cast<std::size_t>(sw.sw_numblocks), 0);
      for (std::size_t i = lbl_blocks.size(); i-- > 0;) lbl_blocks[i] = lbls[static_cast<std::size_t>(act_blocks[i])];
      std::vector<label> lbl_consts(static_cast<std::size_t>(sw.sw_numconsts), 0);
      for (std::size_t i = lbl_consts.size(); i-- > 0;) lbl_consts[i] = lbls[static_cast<std::size_t>(act_consts[i])];
      Instruction ks = K(IK::Kswitch);
      ks.sw_consts = slice(lbl_consts);
      ks.sw_blocks = slice(lbl_blocks);
      return comp_expr(si, env, s->arg, sz, cons(ks, c));
    }
    case L::LK::Lstringswitch: {
      auto* s = L::as<L::Lstringswitch>(exp);
      return comp_expr(si, env, matching::expand_stringswitch(s->loc, s->arg, s->cases, s->def), sz, cont);
    }
    case L::LK::Lassign: {
      auto* a = L::as<L::Lassign>(exp);
      const long* pos = env.ce_stack.find_same_opt(a->id);
      if (!pos) fatal_error("Bytegen.comp_expr: assign");
      return comp_expr(si, env, a->e, sz, cons(K(IK::Kassign, sz - *pos), cont));
    }
    case L::LK::Levent: {
      auto* le = L::as<L::Levent>(exp);
      lambda lam = le->l;
      const L::LambdaEvent* lev = le->ev;
      std::string_view ev_defname = debuginfo::string_of_scoped_location(lev->lev_loc);
      auto event = [&](DebugEventKind kind, DebugEventInfo info) {
        DebugEvent* ev = make<DebugEvent>();
        ev->ev_pos = 0;  // patched in emitcode
        ev->ev_module = compunit_name;
        ev->ev_loc = debuginfo::to_location(lev->lev_loc);
        ev->ev_kind = kind;
        ev->ev_defname = ev_defname;
        ev->ev_info = info;
        ev->ev_typenv = env::summary(lev->lev_env);
        ev->ev_typsubst = subst::identity();
        ev->ev_compenv = env;
        ev->ev_stacksize = sz;
        DebugEventRepr repr{};
        if (lev->lev_repr) {
          bool fn = lev->lev_kind == L::EventKind::Lev_function;
          if (lev->lev_repr->contents == 1)
            repr = DebugEventRepr{fn ? DebugEventReprK::Event_child : DebugEventReprK::Event_parent, lev->lev_repr};
          else
            repr = DebugEventRepr{fn ? DebugEventReprK::Event_parent : DebugEventReprK::Event_child, lev->lev_repr};
        }
        ev->ev_repr = repr;
        return ev;
      };
      switch (lev->lev_kind) {
        case L::EventKind::Lev_before: {
          code c = comp_expr(si, env, lam, sz, cont);
          DebugEvent* ev = event(DebugEventKind{DebugEventKindK::Event_before}, DebugEventInfo{DebugEventInfoK::Event_other});
          return add_event(ev, c);
        }
        case L::EventKind::Lev_function: {
          code c = comp_expr(si, env, lam, sz, cont);
          DebugEvent* ev = event(DebugEventKind{DebugEventKindK::Event_pseudo}, DebugEventInfo{DebugEventInfoK::Event_function});
          return add_event(ev, c);
        }
        case L::EventKind::Lev_pseudo: {
          code c = comp_expr(si, env, lam, sz, cont);
          DebugEvent* ev = event(DebugEventKind{DebugEventKindK::Event_pseudo}, DebugEventInfo{DebugEventInfoK::Event_other});
          return add_event(ev, c);
        }
        case L::EventKind::Lev_after: {
          bool preserve_tailcall = true;
          if (auto* pr = L::as<L::Lprim>(lam)) preserve_tailcall = preserve_tailcall_for_prim(pr->p);
          if (preserve_tailcall && is_tailcall(cont))
            // don't destroy tail call opt
            return comp_expr(si, env, lam, sz, cont);
          DebugEventInfo info{DebugEventInfoK::Event_other};
          if (auto* ap = L::as<L::Lapply>(lam))
            info = DebugEventInfo{DebugEventInfoK::Event_return, static_cast<long>(ap->ap.ap_args.size())};
          else if (auto* sd = L::as<L::Lsend>(lam))
            info = DebugEventInfo{DebugEventInfoK::Event_return, static_cast<long>(sd->args.size()) + 1};
          else if (auto* pr = L::as<L::Lprim>(lam))
            info = DebugEventInfo{DebugEventInfoK::Event_return, static_cast<long>(pr->args.size())};
          DebugEvent* ev = event(DebugEventKind{DebugEventKindK::Event_after, lev->lev_after_type}, info);
          code cont1 = add_event(ev, cont);
          return comp_expr(si, env, lam, sz, cont1);
        }
      }
      fatal_error("Bytegen.comp_expr: event kind");
    }
    case L::LK::Lifused:
      return comp_expr(si, env, L::as<L::Lifused>(exp)->l, sz, cont);
  }
  fatal_error("Bytegen.comp_expr");
}

// Compile a list of arguments [e1; ...; eN] to a primitive operation.
// The values of eN ... e2 are pushed on the stack, e2 at top of stack,
// then e3, then ... The value of e1 is left in the accumulator.
code comp_args(const StackInfo& si, const CompilationEnv& env, Slice<lambda> argl, long sz, code cont) {
  std::vector<lambda> rev(argl.begin(), argl.end());
  std::reverse(rev.begin(), rev.end());
  return comp_expr_list(si, env, rev, 0, sz, cont);
}

// comp_expr_list [x1; ...; xn]: x_n is compiled first (it is inside the
// continuation of x_(n-1)), down to x1.
code comp_expr_list(const StackInfo& si, const CompilationEnv& env, const std::vector<lambda>& exprl,
                    std::size_t from, long sz, code cont) {
  std::size_t n = exprl.size() - from;
  if (n == 0) return cont;
  // sizes: x_(from+k) is compiled at sz + k
  code c = cont;
  for (std::size_t k = exprl.size(); k-- > from;) {
    long s = sz + static_cast<long>(k - from);
    if (k + 1 == exprl.size())
      c = comp_expr(si, env, exprl[k], s, c);
    else
      c = comp_expr(si, env, exprl[k], s, cons(K(IK::Kpush), c));
  }
  return c;
}

code comp_exit_args(const StackInfo& si, const CompilationEnv& env, Slice<lambda> argl, long sz, long pos,
                    code cont) {
  // comp_expr_list_assign (List.rev argl) sz pos cont
  std::vector<lambda> rev(argl.begin(), argl.end());
  std::reverse(rev.begin(), rev.end());
  // exp :: rem -> comp_expr exp sz (Kassign (sz-pos) :: comp_expr_list_assign rem sz (pos-1) cont)
  code c = cont;
  for (std::size_t k = rev.size(); k-- > 0;) {
    long p = pos - static_cast<long>(k);
    c = comp_expr(si, env, rev[k], sz, cons(K(IK::Kassign, sz - p), c));
  }
  return c;
}

// Compile an if-then-else test.
code comp_binary_test(const StackInfo& si, const CompilationEnv& env, lambda cond, lambda ifso, lambda ifnot,
                      long sz, code cont) {
  code cont_cond;
  const L::Lconst* un = L::as<L::Lconst>(ifnot);
  if (un && L::equal_structured_constant(un->c, L::const_unit())) {
    auto [lbl_end, cont1] = label_code(cont);
    cont_cond = cons(K(IK::Kstrictbranchifnot, lbl_end), comp_expr(si, env, ifso, sz, cont1));
  } else if (auto label = code_as_jump(si, ifso, sz)) {
    code c = comp_expr(si, env, ifnot, sz, cont);
    cont_cond = cons(K(IK::Kbranchif, *label), c);
  } else if (auto label2 = code_as_jump(si, ifnot, sz)) {
    code c = comp_expr(si, env, ifso, sz, cont);
    cont_cond = cons(K(IK::Kbranchifnot, *label2), c);
  } else {
    auto [branch_end, cont1] = make_branch(cont);
    auto [lbl_not, cont2] = label_code(comp_expr(si, env, ifnot, sz, cont1));
    cont_cond = cons(K(IK::Kbranchifnot, lbl_not), comp_expr(si, env, ifso, sz, cons(branch_end, cont2)));
  }
  return comp_expr(si, env, cond, sz, cont_cond);
}

// ---- Compilation of a code block (with tracking of stack usage) ----

code comp_block(const CompilationEnv& env, lambda exp, long sz, code cont) {
  StackInfo si = create_stack_info();
  code c = comp_expr(si, env, exp, sz, cont);
  long used_safe = *si.max_stack_used + stack_safety_margin;
  if (used_safe > stack_threshold)
    return cons(kconst(L::const_int(used_safe)), cons(kccall("caml_ensure_stack_capacity", 1, nullptr), c));
  return c;
}

// ---- Compilation of functions ----

code comp_function(const FunctionToCompile& tc, code cont) {
  long arity = static_cast<long>(tc.params.size());
  auto [ce_stack, last_pos] = add_positions(ident::Tbl<long>{}, [](long pos) { return pos; }, arity, -1, tc.params);
  (void)last_pos;
  CompilationEnv env{ce_stack, ClosureEnv{true, tc.entries, 3 * tc.rec_pos, fresh_identity()}, fresh_identity()};
  code c = comp_block(env, tc.body, arity, cons(K(IK::Kreturn, arity), cont));
  if (arity > 1) return cons(K(IK::Krestart), cons(K(IK::Klabel, tc.lbl), cons(K(IK::Kgrab, arity - 1), c)));
  return cons(K(IK::Klabel, tc.lbl), c);
}

code comp_remainder(code cont) {
  code c = cont;
  while (!functions_to_compile.empty()) {
    FunctionToCompile tc = functions_to_compile.back();
    functions_to_compile.pop_back();
    c = comp_function(tc, c);
  }
  return c;
}

// ---- Compilation of a lambda phrase ----

void reset() {
  label_counter = 0;
  compunit_name = "";
  functions_to_compile.clear();
}

}  // namespace

DebugEvent* merge_events(DebugEvent* ev1, DebugEvent* ev2) { return merge_events_(ev1, ev2); }

instruct::code compile_implementation(std::string_view modulename, L::lambda expr) {
  reset();
  compunit_name = zborrow(modulename);
  struct Finally {
    ~Finally() { reset(); }
  } finally;
  code init_code = comp_block(empty_env(), expr, 0, nullptr);
  if (!functions_to_compile.empty()) {
    label lbl_init = new_label();
    return cons(K(IK::Kbranch, lbl_init), comp_remainder(cons(K(IK::Klabel, lbl_init), init_code)));
  }
  return init_code;
}

}  // namespace cppcaml::typing::bytegen
