// Port of asmcomp/selectgen.ml: the generic instruction selector, which
// each target's Selection specializes (src/typing/<arch>/selection.cpp, as
// asmcomp/<arch>/selection.ml inherits Selectgen.selector_generic).
#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/selection.hpp"

namespace cppcaml::typing::selection {

using namespace cmm;
using namespace mach;
using reg::Reg;
using reg::Regs;
using MC = MachtypeComponent;
using OK = cmm::Operation::K;
using MK = mach::Operation::K;
using IO = IntegerOperation;
using IK = Instruction::K;
using Chunk = cmm::MemoryChunk;
namespace A = arch;
using AK = A::AddressingMode::K;
using SK = A::SpecificOperation::K;

[[noreturn]] inline void fatal(const std::string& s) { throw std::runtime_error(s); }

constexpr long size_addr = 8, size_int = 8, size_float = 8;

struct IdentCmp {
  int operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b); }
};
struct LongCmp {
  int operator()(long a, long b) const { return a < b ? -1 : a > b ? 1 : 0; }
};

// ---- environment ----
struct VarEntry {
  Regs regs;
  MutableFlag mut;
};
struct Environment {
  PMap<Ident::t, VarEntry, IdentCmp> vars;
  // Which registers must be populated when jumping to the given handler.
  PMap<long, std::vector<Regs>, LongCmp> static_exceptions;
};

inline Environment env_add(const VarWithProvenance& var, const Regs& regs, const Environment& env,
                    MutableFlag mut = MutableFlag::Immutable) {
  Environment e = env;
  e.vars = env.vars.add(var.var, VarEntry{regs, mut});
  return e;
}
inline Environment env_add_static_exception(long id, const std::vector<Regs>& v, const Environment& env) {
  Environment e = env;
  e.static_exceptions = env.static_exceptions.add(id, v);
  return e;
}
inline const Regs* env_find(Ident::t id, const Environment& env) {
  const VarEntry* e = env.vars.find_opt(id);
  return e ? &e->regs : nullptr;
}
inline const Regs* env_find_mut(Ident::t id, const Environment& env) {
  const VarEntry* e = env.vars.find_opt(id);
  if (!e) return nullptr;
  if (e->mut != MutableFlag::Mutable) fatal("Selectgen.env_find_mut: not mutable");
  return &e->regs;
}

// Infer the type of the result of an operation
inline Machtype oper_result_type(const cmm::Operation& op) {
  switch (op.kind) {
    case OK::Capply:
    case OK::Cextcall: return op.ty;
    case OK::Cload:
      if (op.chunk == Chunk::Word_val) return typ_val();
      if (op.chunk == Chunk::Single || op.chunk == Chunk::Double) return typ_float();
      return typ_int();
    case OK::Calloc: return typ_val();
    case OK::Cstore: return typ_void();
    case OK::Cdls_get: return typ_val();
    case OK::Caddv: return typ_val();
    case OK::Cadda: return typ_addr();
    case OK::Cnegf:
    case OK::Cabsf:
    case OK::Caddf:
    case OK::Csubf:
    case OK::Cmulf:
    case OK::Cdivf:
    case OK::Cfloatofint: return typ_float();
    case OK::Craise:
    case OK::Ccheckbound:
    case OK::Cpoll: return typ_void();
    case OK::Copaque: return typ_val();
    default: return typ_int();  // arithmetic, comparisons, Cintoffloat, Catomic_fetch_add
  }
}

// Infer the size in bytes of the result of an expression whose evaluation
// may be deferred (cf. [emit_parts]).
inline long size_component(MC c) {
  switch (c) {
    case MC::Val:
    case MC::Addr: return size_addr;
    case MC::Int: return size_int;
    case MC::Float: return size_float;
  }
  return 0;
}
inline long size_machtype(const std::vector<MC>& mty) {
  long size = 0;
  for (MC c : mty) size += size_component(c);
  return size;
}
inline long size_expr(const Environment& env, expression exp) {
  std::function<long(const PMap<Ident::t, long, IdentCmp>&, expression)> size =
      [&](const PMap<Ident::t, long, IdentCmp>& localenv, expression e) -> long {
    switch (e->kind) {
      case EK::Cconst_int:
      case EK::Cconst_natint: return size_int;
      case EK::Cconst_symbol: return size_addr;
      case EK::Cconst_float: return size_float;
      case EK::Cvar:
      case EK::Cvar_mut: {
        Ident::t id = e->kind == EK::Cvar ? static_cast<const Cvar*>(e)->id : static_cast<const Cvar_mut*>(e)->id;
        if (const long* s = localenv.find_opt(id)) return *s;
        if (const Regs* regs = env_find(id, env)) return size_machtype(reg::typv(*regs));
        fatal("Selection.size_expr: unbound var " + ident::unique_name(id));
      }
      case EK::Ctuple: {
        long sz = 0;
        for (expression x : static_cast<const Ctuple*>(e)->el) sz += size(localenv, x);
        return sz;
      }
      case EK::Cop: {
        Machtype t = oper_result_type(static_cast<const Cop*>(e)->op);
        return size_machtype(std::vector<MC>(t.begin(), t.end()));
      }
      case EK::Clet: {
        auto* x = static_cast<const Clet*>(e);
        return size(localenv.add(x->id.var, size(localenv, x->def)), x->body);
      }
      case EK::Csequence: return size(localenv, static_cast<const Csequence*>(e)->e2);
      default: fatal("Selection.size_expr");
    }
  };
  return size(PMap<Ident::t, long, IdentCmp>{}, exp);
}

// Swap the two arguments of an integer comparison
inline mach::IntegerComparison swap_intcomp(const mach::IntegerComparison& c) {
  return {c.is_signed, lambda::swap_integer_comparison(c.c)};
}

// Naming of registers
inline bool all_regs_anonymous(const Regs& rv) {
  for (Reg* r : rv)
    if (!reg::anonymous(r)) return false;
  return true;
}

inline void name_regs(const VarWithProvenance& id, const Regs& rv) {
  if (rv.size() == 1) {
    rv[0]->raw_name = {reg::RawName::K::Var, id.var};
    return;
  }
  for (std::size_t i = 0; i < rv.size(); ++i) {
    rv[i]->raw_name = {reg::RawName::K::Var, id.var};
    rv[i]->part = static_cast<long>(i);
  }
}

// Name of function being compiled
inline std::string_view current_function_name;

// ---- (co)effects ----
enum class Effect { None, Raise, Arbitrary };
inline Effect join_effect(Effect a, Effect b) {
  if (a == Effect::None) return b;
  if (b == Effect::None) return a;
  if (a == Effect::Raise && b == Effect::Raise) return Effect::Raise;
  return Effect::Arbitrary;
}
enum class Coeffect { None, Read_mutable, Arbitrary };
inline Coeffect join_coeffect(Coeffect a, Coeffect b) {
  if (a == Coeffect::None) return b;
  if (b == Coeffect::None) return a;
  if (a == Coeffect::Read_mutable && b == Coeffect::Read_mutable) return Coeffect::Read_mutable;
  return Coeffect::Arbitrary;
}
struct EC {
  Effect e = Effect::None;
  Coeffect ce = Coeffect::None;
};
inline EC ec_join(const EC& a, const EC& b) { return {join_effect(a.e, b.e), join_coeffect(a.ce, b.ce)}; }
inline bool pure_and_copure(const EC& x) { return x.e == Effect::None && x.ce == Coeffect::None; }
inline const EC ec_arbitrary{Effect::Arbitrary, Coeffect::Arbitrary};

inline bool cint(expression e, long& n) { return is_cint(e, n); }

class Selector {
 public:
  virtual ~Selector() = default;
  // A fresh selector of the same class (OCaml's {< instr_seq = dummy_instr >})
  virtual std::unique_ptr<Selector> fresh() const = 0;

  Instr instr_seq = dummy_instr();

  // A syntactic criterion used in addition to judgements about (co)effects
  // as to whether the evaluation of a given expression may be deferred by
  // [emit_parts].
  virtual bool is_simple_expr(expression e) {
    switch (e->kind) {
      case EK::Cconst_int:
      case EK::Cconst_natint:
      case EK::Cconst_float:
      case EK::Cconst_symbol:
      case EK::Cvar:
      case EK::Creturn_addr: return true;
      case EK::Ctuple:
        for (expression x : static_cast<const Ctuple*>(e)->el)
          if (!is_simple_expr(x)) return false;
        return true;
      case EK::Clet: {
        auto* x = static_cast<const Clet*>(e);
        return is_simple_expr(x->def) && is_simple_expr(x->body);
      }
      case EK::Clet_mut: {
        auto* x = static_cast<const Clet_mut*>(e);
        return is_simple_expr(x->def) && is_simple_expr(x->body);
      }
      case EK::Cphantom_let: return is_simple_expr(static_cast<const Cphantom_let*>(e)->body);
      case EK::Csequence: {
        auto* x = static_cast<const Csequence*>(e);
        return is_simple_expr(x->e1) && is_simple_expr(x->e2);
      }
      case EK::Cop: {
        auto* x = static_cast<const Cop*>(e);
        switch (x->op.kind) {
          // The following may have side effects
          case OK::Capply:
          case OK::Cextcall:
          case OK::Calloc:
          case OK::Cstore:
          case OK::Craise:
          case OK::Copaque:
          case OK::Catomic_fetch_add:
          case OK::Cpoll: return false;
          // The remaining operations are simple if their args are
          default: break;
        }
        for (expression a : x->args)
          if (!is_simple_expr(a)) return false;
        return true;
      }
      default: return false;
    }
  }

  // Analyses the effects and coeffects of an expression.
  virtual EC effects_of(expression e) {
    auto join_list = [&](Slice<expression> l) {
      EC r;
      for (expression x : l) r = ec_join(r, effects_of(x));
      return r;
    };
    switch (e->kind) {
      case EK::Cconst_int:
      case EK::Cconst_natint:
      case EK::Cconst_float:
      case EK::Cconst_symbol:
      case EK::Cvar:
      case EK::Creturn_addr: return {};
      case EK::Cvar_mut: return {Effect::None, Coeffect::Read_mutable};
      case EK::Ctuple: return join_list(static_cast<const Ctuple*>(e)->el);
      case EK::Clet: {
        auto* x = static_cast<const Clet*>(e);
        return ec_join(effects_of(x->def), effects_of(x->body));
      }
      case EK::Clet_mut: {
        auto* x = static_cast<const Clet_mut*>(e);
        return ec_join(effects_of(x->def), effects_of(x->body));
      }
      case EK::Cphantom_let: return effects_of(static_cast<const Cphantom_let*>(e)->body);
      case EK::Csequence: {
        auto* x = static_cast<const Csequence*>(e);
        return ec_join(effects_of(x->e1), effects_of(x->e2));
      }
      case EK::Cifthenelse: {
        auto* x = static_cast<const Cifthenelse*>(e);
        return ec_join(effects_of(x->cond), ec_join(effects_of(x->ifso), effects_of(x->ifnot)));
      }
      case EK::Cop: {
        auto* x = static_cast<const Cop*>(e);
        EC from_op;
        switch (x->op.kind) {
          case OK::Capply:
          case OK::Cextcall:
          case OK::Copaque:
          case OK::Catomic_fetch_add:
          case OK::Cpoll: from_op = ec_arbitrary; break;
          case OK::Calloc: break;
          case OK::Cstore: from_op = {Effect::Arbitrary, Coeffect::None}; break;
          case OK::Craise:
          case OK::Ccheckbound: from_op = {Effect::Raise, Coeffect::None}; break;
          case OK::Cload:
            if (x->op.mut == MutableFlag::Mutable) from_op = {Effect::None, Coeffect::Read_mutable};
            break;
          case OK::Cdls_get: from_op = {Effect::None, Coeffect::Read_mutable}; break;
          default: break;
        }
        return ec_join(from_op, join_list(x->args));
      }
      default: return ec_arbitrary;  // Cassign, Cswitch, Ccatch, Cexit, Ctrywith
    }
  }

  // Says whether an integer constant is a suitable immediate argument for
  // the given integer operation
  virtual bool is_immediate(IO op, long n) {
    switch (op) {
      case IO::Ilsl:
      case IO::Ilsr:
      case IO::Iasr: return n >= 0 && n < size_int * 8;
      default: return false;
    }
  }

  // Says whether an integer constant is a suitable immediate argument for
  // the given integer test
  virtual bool is_immediate_test(const mach::IntegerComparison& cmp, long n) = 0;

  // Selection of addressing modes
  virtual std::pair<A::AddressingMode, expression> select_addressing(Chunk chunk, expression exp) = 0;

  // Default instruction selection for stores (of words)
  virtual std::pair<mach::Operation, expression> select_store(bool is_assign, const A::AddressingMode& addr,
                                                              expression exp) {
    mach::Operation op{MK::Istore};
    op.chunk = Chunk::Word_val;
    op.addr = addr;
    op.is_assign = is_assign;
    return {op, exp};
  }

  static mach::Operation intop(IO op) {
    mach::Operation o{MK::Iintop};
    o.intop = {op};
    return o;
  }
  static mach::Operation intop_imm(IO op, long n) {
    mach::Operation o{MK::Iintop_imm};
    o.intop = {op};
    o.n = n;
    return o;
  }
  static mach::Operation intcomp(MK k, const mach::IntegerComparison& c, long n = 0) {
    mach::Operation o{k};
    o.intop = {IO::Icomp, c};
    o.n = n;
    return o;
  }

  using Sel = std::pair<mach::Operation, std::vector<expression>>;
  Sel select_arith_comm(IO op, Slice<expression> args) {
    long n;
    if (args.size() == 2 && cint(args[1], n) && is_immediate(op, n)) return {intop_imm(op, n), {args[0]}};
    if (args.size() == 2 && cint(args[0], n) && is_immediate(op, n)) return {intop_imm(op, n), {args[1]}};
    return {intop(op), vec(args)};
  }
  Sel select_arith(IO op, Slice<expression> args) {
    long n;
    if (args.size() == 2 && cint(args[1], n) && is_immediate(op, n)) return {intop_imm(op, n), {args[0]}};
    return {intop(op), vec(args)};
  }
  Sel select_arith_comp(const mach::IntegerComparison& cmp, Slice<expression> args) {
    long n;
    if (args.size() == 2 && cint(args[1], n) && is_immediate(IO::Icomp, n))
      return {intcomp(MK::Iintop_imm, cmp, n), {args[0]}};
    if (args.size() == 2 && cint(args[0], n) && is_immediate(IO::Icomp, n))
      return {intcomp(MK::Iintop_imm, swap_intcomp(cmp), n), {args[1]}};
    return {intcomp(MK::Iintop, cmp), vec(args)};
  }
  static std::vector<expression> vec(Slice<expression> s) { return std::vector<expression>(s.begin(), s.end()); }

  static mach::Operation simple(MK k) { return mach::Operation{k}; }
  // Default instruction selection for operators
  virtual Sel select_operation(const cmm::Operation& op, Slice<expression> args, const debuginfo::t& dbg) {
    switch (op.kind) {
      case OK::Capply:
        if (!args.empty())
          if (auto* s = as<Cconst_symbol>(args[0])) {
            mach::Operation o{MK::Icall_imm};
            o.func = s->s;
            return {o, std::vector<expression>(args.begin() + 1, args.end())};
          }
        return {simple(MK::Icall_ind), vec(args)};
      case OK::Cextcall: {
        mach::Operation o{MK::Iextcall};
        o.func = op.name;
        o.alloc = op.alloc;
        o.ty_res = op.ty;
        o.ty_args = op.ty_args;
        o.stack_ofs = -1;
        return {o, vec(args)};
      }
      case OK::Cload:
        if (args.size() == 1) {
          auto [addr, eloc] = select_addressing(op.chunk, args[0]);
          mach::Operation o{MK::Iload};
          o.chunk = op.chunk;
          o.addr = addr;
          o.mut = op.mut;
          o.is_atomic = op.is_atomic;
          return {o, {eloc}};
        }
        break;
      case OK::Cstore:
        if (args.size() == 2) {
          auto [addr, eloc] = select_addressing(op.chunk, args[0]);
          bool is_assign = op.init == lambda::InitializationOrAssignment::Assignment;
          if (op.chunk == Chunk::Word_int || op.chunk == Chunk::Word_val) {
            auto [o, newarg2] = select_store(is_assign, addr, args[1]);
            return {o, {newarg2, eloc}};
          }
          mach::Operation o{MK::Istore};
          o.chunk = op.chunk;
          o.addr = addr;
          o.is_assign = is_assign;
          return {o, {args[1], eloc}};  // Inversion addr/datum in Istore
        }
        break;
      case OK::Catomic_fetch_add: return {simple(MK::Iatomic_fetch_add), vec(args)};
      case OK::Cdls_get: return {simple(MK::Idls_get), vec(args)};
      case OK::Cpoll: return {simple(MK::Ipoll), vec(args)};
      case OK::Calloc: return {simple(MK::Ialloc), vec(args)};
      case OK::Caddi: return select_arith_comm(IO::Iadd, args);
      case OK::Csubi: return select_arith(IO::Isub, args);
      case OK::Cmuli: return select_arith_comm(IO::Imul, args);
      case OK::Cmulhi: return select_arith_comm(IO::Imulh, args);
      case OK::Cdivi: return {intop(IO::Idiv), vec(args)};
      case OK::Cmodi: return {intop(IO::Imod), vec(args)};
      case OK::Cand: return select_arith_comm(IO::Iand, args);
      case OK::Cor: return select_arith_comm(IO::Ior, args);
      case OK::Cxor: return select_arith_comm(IO::Ixor, args);
      case OK::Clsl: return select_arith(IO::Ilsl, args);
      case OK::Clsr: return select_arith(IO::Ilsr, args);
      case OK::Casr: return select_arith(IO::Iasr, args);
      case OK::Ccmpi: return select_arith_comp({true, op.icmp}, args);
      case OK::Caddv: return select_arith_comm(IO::Iadd, args);
      case OK::Cadda: return select_arith_comm(IO::Iadd, args);
      case OK::Ccmpa: return select_arith_comp({false, op.icmp}, args);
      case OK::Ccmpf: {
        mach::Operation o{MK::Icompf};
        o.fcmp = op.fcmp;
        return {o, vec(args)};
      }
      case OK::Cnegf: return {simple(MK::Inegf), vec(args)};
      case OK::Cabsf: return {simple(MK::Iabsf), vec(args)};
      case OK::Caddf: return {simple(MK::Iaddf), vec(args)};
      case OK::Csubf: return {simple(MK::Isubf), vec(args)};
      case OK::Cmulf: return {simple(MK::Imulf), vec(args)};
      case OK::Cdivf: return {simple(MK::Idivf), vec(args)};
      case OK::Cfloatofint: return {simple(MK::Ifloatofint), vec(args)};
      case OK::Cintoffloat: return {simple(MK::Iintoffloat), vec(args)};
      case OK::Ccheckbound: return select_arith(IO::Icheckbound, args);
      default: break;
    }
    fatal("Selection.select_oper");
  }

  // Instruction selection for conditionals
  std::pair<Test, expression> select_condition(expression e) {
    long n;
    if (auto* c = as<Cop>(e); c && c->args.size() == 2) {
      if (c->op.kind == OK::Ccmpi || c->op.kind == OK::Ccmpa) {
        bool sgn = c->op.kind == OK::Ccmpi;
        if (cint(c->args[1], n) && is_immediate_test({sgn, c->op.icmp}, n))
          return {{Test::K::Iinttest_imm, {sgn, c->op.icmp}, n}, c->args[0]};
        if (cint(c->args[0], n) && is_immediate_test({sgn, lambda::swap_integer_comparison(c->op.icmp)}, n))
          return {{Test::K::Iinttest_imm, {sgn, lambda::swap_integer_comparison(c->op.icmp)}, n}, c->args[1]};
        return {{Test::K::Iinttest, {sgn, c->op.icmp}}, ctuple(c->args)};
      }
      if (c->op.kind == OK::Ccmpf) {
        Test t{Test::K::Ifloattest};
        t.fcmp = c->op.fcmp;
        return {t, ctuple(c->args)};
      }
      if (c->op.kind == OK::Cand && is_cint_eq(c->args[1], 1)) return {{Test::K::Ioddtest}, c->args[0]};
    } else if (c && (c->op.kind == OK::Ccmpi || c->op.kind == OK::Ccmpa)) {
      return {{Test::K::Iinttest, {c->op.kind == OK::Ccmpi, c->op.icmp}}, ctuple(c->args)};
    } else if (c && c->op.kind == OK::Ccmpf) {
      Test t{Test::K::Ifloattest};
      t.fcmp = c->op.fcmp;
      return {t, ctuple(c->args)};
    }
    return {{Test::K::Itruetest}, e};
  }

  // Return an array of fresh registers of the given type.
  Regs regs_for(Machtype tys) { return reg::createv(tys); }

  // Buffering of instruction sequences
  void insert_debug(const Instruction& desc, const debuginfo::t& dbg, const Regs& arg, const Regs& res) {
    instr_seq = instr_cons_debug(desc, arg, res, dbg, instr_seq);
  }
  void insert(const Instruction& desc, const Regs& arg, const Regs& res) {
    instr_seq = instr_cons(desc, arg, res, instr_seq);
  }
  Instr extract_onto(Instr o) {
    Instr res = o;
    for (Instr i = instr_seq; i != dummy_instr(); i = i->next) {
      Instr c = copy(i);
      c->next = res;
      res = c;
    }
    return res;
  }
  Instr extract() { return extract_onto(end_instr()); }

  // Insert a sequence of moves from one pseudoreg set to another.
  void insert_move(Reg* src, Reg* dst) {
    if (src->stamp != dst->stamp) insert(iop(mach::mop(MK::Imove)), {src}, {dst});
  }
  void insert_moves(const Regs& src, const Regs& dst) {
    for (std::size_t i = 0; i < std::min(src.size(), dst.size()); ++i) insert_move(src[i], dst[i]);
  }

  // Insert moves and stack offsets for function arguments and results
  void insert_move_args(const Regs& arg, const Regs& loc, long stacksize) {
    if (stacksize != 0) {
      mach::Operation o{MK::Istackoffset};
      o.n = stacksize;
      insert(iop(o), {}, {});
    }
    insert_moves(arg, loc);
  }
  void insert_move_results(const Regs& loc, const Regs& res, long stacksize) {
    if (stacksize != 0) {
      mach::Operation o{MK::Istackoffset};
      o.n = -stacksize;
      insert(iop(o), {}, {});
    }
    insert_moves(loc, res);
  }

  // Add an Iop opcode.  Can be overridden by processor description to insert
  // moves before and after the operation, i.e. for two-address instructions,
  // or instructions using dedicated registers.
  virtual Regs insert_op_debug(const mach::Operation& op, const debuginfo::t& dbg, const Regs& rs, const Regs& rd) {
    insert_debug(iop(op), dbg, rs, rd);
    return rd;
  }
  Regs insert_op(const mach::Operation& op, const Regs& rs, const Regs& rd) {
    return insert_op_debug(op, debuginfo::none(), rs, rd);
  }

  using Res = std::optional<Regs>;

  // "Join" two instruction sequences, making sure they return their
  // results in the same registers.
  Res join(const Res& opt_r1, Selector& seq1, const Res& opt_r2, Selector& seq2) {
    if (!opt_r1) return opt_r2;
    if (!opt_r2) return opt_r1;
    const Regs& r1 = *opt_r1;
    const Regs& r2 = *opt_r2;
    if (r1.size() != r2.size()) fatal("Selectgen.join");
    Regs r(r1.size(), nullptr);
    for (std::size_t i = 0; i < r1.size(); ++i) {
      if (reg::anonymous(r1[i]) && ge_component(r1[i]->typ, r2[i]->typ)) {
        r[i] = r1[i];
        seq2.insert_move(r2[i], r1[i]);
      } else if (reg::anonymous(r2[i]) && ge_component(r2[i]->typ, r1[i]->typ)) {
        r[i] = r2[i];
        seq1.insert_move(r1[i], r2[i]);
      } else {
        MC typ = lub_component(r1[i]->typ, r2[i]->typ);
        r[i] = reg::create(typ);
        seq1.insert_move(r1[i], r[i]);
        seq2.insert_move(r2[i], r[i]);
      }
    }
    return r;
  }

  // Same, for N branches
  Res join_array(std::vector<std::pair<Res, std::unique_ptr<Selector>>>& rs) {
    const Regs* templ = nullptr;
    std::vector<MC> types;
    for (auto& [r, _] : rs) {
      if (!r) continue;
      if (!templ) {
        templ = &*r;
        types = reg::typv(*r);
      } else {
        for (std::size_t k = 0; k < types.size(); ++k) types[k] = lub_component((*r)[k]->typ, types[k]);
      }
    }
    if (!templ) return std::nullopt;
    Regs res;
    for (MC t : types) res.push_back(reg::create(t));
    for (auto& [r, s] : rs)
      if (r) s->insert_moves(*r, res);
    return res;
  }

  // Add the instructions for the given expression at the end of the self
  // sequence
  Res emit_expr(const Environment& env, expression exp) {
    switch (exp->kind) {
      case EK::Cconst_int: {
        Regs r = regs_for(typ_int());
        mach::Operation o{MK::Iconst_int};
        o.n = static_cast<const Cconst_int*>(exp)->n;
        return insert_op(o, {}, r);
      }
      case EK::Cconst_natint: {
        Regs r = regs_for(typ_int());
        mach::Operation o{MK::Iconst_int};
        o.n = static_cast<const Cconst_natint*>(exp)->n;
        return insert_op(o, {}, r);
      }
      case EK::Cconst_float: {
        Regs r = regs_for(typ_float());
        mach::Operation o{MK::Iconst_float};
        double d = static_cast<const Cconst_float*>(exp)->f;
        std::memcpy(&o.n, &d, sizeof d);
        return insert_op(o, {}, r);
      }
      case EK::Cconst_symbol: {
        // Cconst_symbol _ evaluates to a statically-allocated address, so
        // its value fits in a typ_int register and is never changed by the
        // GC.
        Regs r = regs_for(typ_int());
        mach::Operation o{MK::Iconst_symbol};
        o.func = static_cast<const Cconst_symbol*>(exp)->s;
        return insert_op(o, {}, r);
      }
      case EK::Creturn_addr: {
        Regs r = regs_for(typ_int());
        return insert_op(simple(MK::Ireturn_addr), {}, r);
      }
      case EK::Cvar:
      case EK::Cvar_mut: {
        Ident::t v = exp->kind == EK::Cvar ? static_cast<const Cvar*>(exp)->id : static_cast<const Cvar_mut*>(exp)->id;
        if (const Regs* r = env_find(v, env)) return *r;
        fatal("Selection.emit_expr: unbound var " + ident::unique_name(v));
      }
      case EK::Clet: {
        auto* x = static_cast<const Clet*>(exp);
        Res r1 = emit_expr(env, x->def);
        if (!r1) return std::nullopt;
        return emit_expr(bind_let(env, x->id, *r1), x->body);
      }
      case EK::Clet_mut: {
        auto* x = static_cast<const Clet_mut*>(exp);
        Res r1 = emit_expr(env, x->def);
        if (!r1) return std::nullopt;
        return emit_expr(bind_let_mut(env, x->id, x->ty, *r1), x->body);
      }
      case EK::Cphantom_let: return emit_expr(env, static_cast<const Cphantom_let*>(exp)->body);
      case EK::Cassign: {
        auto* x = static_cast<const Cassign*>(exp);
        const Regs* rv = env_find_mut(x->id, env);
        if (!rv) fatal("Selection.emit_expr: unbound var " + std::string(ident::name(x->id)));
        Res r1 = emit_expr(env, x->e);
        if (!r1) return std::nullopt;
        insert_moves(*r1, *rv);
        return Regs{};
      }
      case EK::Ctuple: {
        auto* x = static_cast<const Ctuple*>(exp);
        if (x->el.empty()) return Regs{};
        auto parts = emit_parts_list(env, x->el);
        if (!parts) return std::nullopt;
        return emit_tuple(parts->second, parts->first);
      }
      case EK::Cop: return emit_op(env, static_cast<const Cop*>(exp));
      case EK::Csequence: {
        auto* x = static_cast<const Csequence*>(exp);
        if (!emit_expr(env, x->e1)) return std::nullopt;
        return emit_expr(env, x->e2);
      }
      case EK::Cifthenelse: {
        auto* x = static_cast<const Cifthenelse*>(exp);
        auto [cond, earg] = select_condition(x->cond);
        Res rarg = emit_expr(env, earg);
        if (!rarg) return std::nullopt;
        auto [rif, sif] = emit_sequence(env, x->ifso);
        auto [relse, selse] = emit_sequence(env, x->ifnot);
        Res r = join(rif, *sif, relse, *selse);
        Instruction d = idesc(IK::Iifthenelse);
        d.test = cond;
        // Iifthenelse(cond, sif#extract, selse#extract): right to left
        d.ifnot = selse->extract();
        d.ifso = sif->extract();
        insert(d, *rarg, {});
        return r;
      }
      case EK::Cswitch: {
        auto* x = static_cast<const Cswitch*>(exp);
        Res rsel = emit_expr(env, x->e);
        if (!rsel) return std::nullopt;
        std::vector<std::pair<Res, std::unique_ptr<Selector>>> rscases;
        for (auto& c : x->cases) rscases.push_back(emit_sequence(env, c.e));
        Res r = join_array(rscases);
        Instruction d = idesc(IK::Iswitch);
        d.index = x->index;
        for (auto& [_, s] : rscases) d.cases.push_back(s->extract());
        insert(d, *rsel, {});
        return r;
      }
      case EK::Ccatch: return emit_catch(env, static_cast<const Ccatch*>(exp));
      case EK::Cexit: {
        auto* x = static_cast<const Cexit*>(exp);
        auto parts = emit_parts_list(env, x->args);
        if (!parts) return std::nullopt;
        Regs src = emit_tuple(parts->second, parts->first);
        const std::vector<Regs>* dest_args = env.static_exceptions.find_opt(x->n);
        if (!dest_args) fatal("Selection.emit_expr: unbound label " + std::to_string(x->n));
        // Intermediate registers to handle cases where some registers from
        // src are present in dest
        Regs tmp_regs = reg::createv_like(src);
        insert_moves(src, tmp_regs);
        Regs dest;
        for (const Regs& d : *dest_args) dest.insert(dest.end(), d.begin(), d.end());
        insert_moves(tmp_regs, dest);
        Instruction d = idesc(IK::Iexit);
        d.nfail = x->n;
        insert(d, {}, {});
        return std::nullopt;
      }
      case EK::Ctrywith: {
        auto* x = static_cast<const Ctrywith*>(exp);
        auto [r1, s1] = emit_sequence(env, x->body);
        Regs rv = regs_for(typ_val());
        auto [r2, s2] = emit_sequence(env_add(x->exn, rv, env), x->handler);
        Res r = join(r1, *s1, r2, *s2);
        Instruction d = idesc(IK::Itrywith);
        // Itrywith(s1#extract, instr_cons (Iop Imove) [|loc_exn_bucket|] rv
        // (s2#extract)): right to left
        Instr h = instr_cons(iop(simple(MK::Imove)), {proc::loc_exn_bucket()}, rv, s2->extract());
        d.ifso = s1->extract();
        d.ifnot = h;
        insert(d, {}, {});
        return r;
      }
    }
    fatal("Selection.emit_expr");
  }

  Res emit_op(const Environment& env, const Cop* x) {
    const debuginfo::t& dbg = x->dbg;
    if (x->op.kind == OK::Craise && x->args.size() == 1) {
      Res r1 = emit_expr(env, x->args[0]);
      if (!r1) return std::nullopt;
      Regs rd{proc::loc_exn_bucket()};
      insert(iop(simple(MK::Imove)), *r1, rd);
      Instruction d = idesc(IK::Iraise);
      d.raise = x->op.raise;
      insert_debug(d, dbg, rd, {});
      return std::nullopt;
    }
    if (x->op.kind == OK::Copaque) {
      auto parts = emit_parts_list(env, x->args);
      if (!parts) return std::nullopt;
      Regs rs = emit_tuple(parts->second, parts->first);
      return insert_op_debug(simple(MK::Iopaque), dbg, rs, rs);
    }
    auto parts = emit_parts_list(env, x->args);
    if (!parts) return std::nullopt;
    const Environment& env2 = parts->second;
    Machtype ty = oper_result_type(x->op);
    auto [new_op, new_args] = select_operation(x->op, slice(parts->first), dbg);
    switch (new_op.k) {
      case MK::Icall_ind: {
        Regs r1 = emit_tuple(env2, new_args);
        Regs rarg(r1.begin() + 1, r1.end());
        Regs rd = regs_for(ty);
        auto [loc_arg, stack_ofs] = proc::loc_arguments(reg::typv(rarg));
        Regs loc_res = proc::loc_results(reg::typv(rd));
        insert_move_args(rarg, loc_arg, stack_ofs);
        Regs a{r1[0]};
        a.insert(a.end(), loc_arg.begin(), loc_arg.end());
        insert_debug(iop(new_op), dbg, a, loc_res);
        insert_move_results(loc_res, rd, stack_ofs);
        return rd;
      }
      case MK::Icall_imm: {
        Regs r1 = emit_tuple(env2, new_args);
        Regs rd = regs_for(ty);
        auto [loc_arg, stack_ofs] = proc::loc_arguments(reg::typv(r1));
        Regs loc_res = proc::loc_results(reg::typv(rd));
        insert_move_args(r1, loc_arg, stack_ofs);
        insert_debug(iop(new_op), dbg, loc_arg, loc_res);
        insert_move_results(loc_res, rd, stack_ofs);
        return rd;
      }
      case MK::Iextcall: {
        auto [loc_arg, stack_ofs] = emit_extcall_args(env2, new_op.ty_args, new_args);
        Regs rd = regs_for(ty);
        mach::Operation o = new_op;
        o.stack_ofs = stack_ofs;
        Regs loc_res = insert_op_debug(o, dbg, loc_arg, proc::loc_external_results(reg::typv(rd)));
        insert_move_results(loc_res, rd, stack_ofs);
        return rd;
      }
      case MK::Ialloc: {
        Regs rd = regs_for(typ_val());
        long bytes = size_expr(env2, ctuple(slice(new_args)));
        long alloc_words = bytes / size_addr;
        mach::Operation o{MK::Ialloc};
        o.n = bytes;
        o.dbginfo = {{alloc_words, dbg}};
        insert_debug(iop(o), dbg, {}, rd);
        emit_stores(env2, new_args, rd);
        return rd;
      }
      default: {
        Regs r1 = emit_tuple(env2, new_args);
        Regs rd = regs_for(ty);
        return insert_op_debug(new_op, dbg, r1, rd);
      }
    }
  }

  Res emit_catch(const Environment& env0, const Ccatch* x) {
    if (x->handlers.empty()) return emit_expr(env0, x->body);
    struct H {
      long nfail;
      Slice<CatchParam> ids;
      std::vector<Regs> rs;
      expression e2;
    };
    std::vector<H> handlers;
    for (auto& h : x->handlers) {
      std::vector<Regs> rs;
      for (auto& p : h.ids) {
        Regs r = regs_for(p.ty);
        name_regs(p.id, r);
        rs.push_back(r);
      }
      handlers.push_back({h.n, h.ids, rs, h.body});
    }
    // Since the handlers may be recursive, and called from the body, the
    // same environment is used for translating both the handlers and the
    // body.
    Environment env = env0;
    for (auto& h : handlers) env = env_add_static_exception(h.nfail, h.rs, env);
    auto [r_body, s_body] = emit_sequence(env, x->body);
    std::vector<std::pair<long, std::pair<Res, std::unique_ptr<Selector>>>> l;
    for (auto& h : handlers) {
      Environment new_env = env;
      for (std::size_t k = 0; k < h.ids.size(); ++k) new_env = env_add(h.ids[k].id, h.rs[k], new_env);
      l.push_back({h.nfail, emit_sequence(new_env, h.e2)});
    }
    std::vector<std::pair<Res, std::unique_ptr<Selector>>> a;
    a.push_back({r_body, std::move(s_body)});
    for (auto& [_, rs] : l) a.push_back({rs.first, std::move(rs.second)});
    Res r = join_array(a);
    Instruction d = idesc(IK::Icatch);
    d.rec = x->rec;
    // Icatch (rec_flag, List.map aux l, s_body#extract): right to left
    Instr body = a[0].second->extract();
    for (std::size_t k = 0; k < l.size(); ++k) d.handlers.push_back({l[k].first, a[k + 1].second->extract()});
    d.body = body;
    insert(d, {}, {});
    return r;
  }

  std::pair<Res, std::unique_ptr<Selector>> emit_sequence(const Environment& env, expression exp) {
    std::unique_ptr<Selector> s = fresh();
    Res r = s->emit_expr(env, exp);
    return {r, std::move(s)};
  }

  Environment bind_let(const Environment& env, const VarWithProvenance& v, const Regs& r1) {
    if (all_regs_anonymous(r1)) {
      name_regs(v, r1);
      return env_add(v, r1, env);
    }
    Regs rv = reg::createv_like(r1);
    name_regs(v, rv);
    insert_moves(r1, rv);
    return env_add(v, rv, env);
  }

  Environment bind_let_mut(const Environment& env, const VarWithProvenance& v, Machtype k, const Regs& r1) {
    Regs rv = regs_for(k);
    name_regs(v, rv);
    insert_moves(r1, rv);
    return env_add(v, rv, env, MutableFlag::Mutable);
  }

  // The following two functions, [emit_parts] and [emit_parts_list], force
  // right-to-left evaluation order as required by the Flambda [Un_anf]
  // pass (and to be consistent with the bytecode compiler).
  std::optional<std::pair<expression, Environment>> emit_parts(const Environment& env, const EC& effects_after,
                                                               expression exp) {
    EC ec = effects_of(exp);
    bool may_defer_evaluation;
    if (ec.e == Effect::Arbitrary || ec.e == Effect::Raise) {
      // Preserve the ordering of effectful expressions by evaluating them
      // early (in the correct order) and assigning their results to
      // temporaries.
      may_defer_evaluation = pure_and_copure(effects_after);
    } else if (ec.ce == Coeffect::None) {
      // Pure expressions may be moved.
      may_defer_evaluation = true;
    } else if (ec.ce == Coeffect::Read_mutable) {
      may_defer_evaluation = effects_after.e != Effect::Arbitrary;
    } else {
      may_defer_evaluation = effects_after.e == Effect::None;
    }
    // Even though some expressions may look like they can be deferred from
    // the (co)effect analysis, it may be forbidden to move them.
    if (may_defer_evaluation && is_simple_expr(exp)) return std::pair{exp, env};
    Res r = emit_expr(env, exp);
    if (!r) return std::nullopt;
    if (r->empty()) return std::pair{ctuple({}), env};
    // The normal case
    Var id = Ident::create_local("bind");
    if (all_regs_anonymous(*r))
      // r is an anonymous, unshared register; use it directly
      return std::pair{cvar(id), env_add({id, nullptr}, *r, env)};
    // Introduce a fresh temp to hold the result
    Regs tmp = reg::createv_like(*r);
    insert_moves(*r, tmp);
    return std::pair{cvar(id), env_add({id, nullptr}, tmp, env)};
  }

  std::optional<std::pair<std::vector<expression>, Environment>> emit_parts_list(const Environment& env,
                                                                                 Slice<expression> exp_list) {
    // Annotate each expression with the (co)effects that happen after it
    // when the original expression list is evaluated from right to left.
    // The resulting expression list has the rightmost expression first.
    std::vector<std::pair<expression, EC>> rtl;
    EC effects_after;
    for (expression e : exp_list) {
      EC exp_effect = effects_of(e);
      rtl.insert(rtl.begin(), {e, effects_after});
      effects_after = ec_join(exp_effect, effects_after);
    }
    std::vector<expression> result;  // built by consing: the leftmost first at the end
    Environment cur = env;
    for (auto& [e, after] : rtl) {
      auto p = emit_parts(cur, after, e);
      if (!p) return std::nullopt;
      result.insert(result.begin(), p->first);
      cur = p->second;
    }
    return std::pair{result, cur};
  }

  std::vector<Regs> emit_tuple_not_flattened(const Environment& env, const std::vector<expression>& exp_list) {
    // Again, force right-to-left evaluation
    std::vector<Regs> r(exp_list.size());
    for (std::size_t k = exp_list.size(); k-- > 0;) {
      Res loc_exp = emit_expr(env, exp_list[k]);
      if (!loc_exp) fatal("Selectgen.emit_tuple");  // should have been caught in emit_parts
      r[k] = *loc_exp;
    }
    return r;
  }

  Regs emit_tuple(const Environment& env, const std::vector<expression>& exp_list) {
    Regs r;
    for (const Regs& x : emit_tuple_not_flattened(env, exp_list)) r.insert(r.end(), x.begin(), x.end());
    return r;
  }

  std::pair<Regs, long> emit_extcall_args(const Environment& env, Slice<Exttype> ty_args0,
                                          const std::vector<expression>& args0) {
    std::vector<Regs> args = emit_tuple_not_flattened(env, args0);
    std::vector<Exttype> ty_args(ty_args0.begin(), ty_args0.end());
    if (ty_args.empty()) ty_args.assign(args.size(), Exttype::XInt);
    auto [locs, stack_ofs] = proc::loc_external_arguments(ty_args);
    if (stack_ofs != 0) {
      mach::Operation o{MK::Istackoffset};
      o.n = stack_ofs;
      insert(iop(o), {}, {});
    }
    for (std::size_t i = 0; i < args.size(); ++i) insert_move_extcall_arg(ty_args[i], args[i], locs[i]);
    Regs r;
    for (const Regs& l : locs) r.insert(r.end(), l.begin(), l.end());
    return {r, stack_ofs};
  }

  // The default implementation is one or two ordinary moves.  (Two in the
  // case of an int64 argument on a 32-bit platform.)  It can be overridden to
  // use special move instructions, for example a "32-bit move" instruction for
  // int32 arguments.
  virtual void insert_move_extcall_arg(Exttype, const Regs& src, const Regs& dst) { insert_moves(src, dst); }

  void emit_stores(const Environment& env, const std::vector<expression>& data, const Regs& regs_addr) {
    A::AddressingMode a = A::offset_addressing(A::identity_addressing(), -size_int);
    for (expression e : data) {
      auto [op, arg] = select_store(false, a, e);
      Res regs = emit_expr(env, arg);
      if (!regs) fatal("Selectgen.emit_stores");
      if (op.k == MK::Istore) {
        for (Reg* r : *regs) {
          mach::Operation o{MK::Istore};
          o.chunk = r->typ == MC::Float ? Chunk::Double : Chunk::Word_val;
          o.addr = a;
          o.is_assign = false;
          Regs args{r};
          args.insert(args.end(), regs_addr.begin(), regs_addr.end());
          insert(iop(o), args, {});
          a = A::offset_addressing(a, size_component(r->typ));
        }
      } else {
        Regs args = *regs;
        args.insert(args.end(), regs_addr.begin(), regs_addr.end());
        insert(iop(op), args, {});
        a = A::offset_addressing(a, size_expr(env, e));
      }
    }
  }

  // Same, but in tail position
  void emit_return(const Environment& env, expression exp) {
    Res r = emit_expr(env, exp);
    if (!r) return;
    Regs loc = proc::loc_results(reg::typv(*r));
    insert_moves(*r, loc);
    insert(idesc(IK::Ireturn), loc, {});
  }

  void emit_tail(const Environment& env, expression exp) {
    switch (exp->kind) {
      case EK::Clet: {
        auto* x = static_cast<const Clet*>(exp);
        Res r1 = emit_expr(env, x->def);
        if (!r1) return;
        emit_tail(bind_let(env, x->id, *r1), x->body);
        return;
      }
      case EK::Clet_mut: {
        auto* x = static_cast<const Clet_mut*>(exp);
        Res r1 = emit_expr(env, x->def);
        if (!r1) return;
        emit_tail(bind_let_mut(env, x->id, x->ty, *r1), x->body);
        return;
      }
      case EK::Cphantom_let: emit_tail(env, static_cast<const Cphantom_let*>(exp)->body); return;
      case EK::Cop: {
        auto* x = static_cast<const Cop*>(exp);
        if (x->op.kind != OK::Capply) break;
        const debuginfo::t& dbg = x->dbg;
        Machtype ty = x->op.ty;
        auto parts = emit_parts_list(env, x->args);
        if (!parts) return;
        const Environment& env2 = parts->second;
        auto [new_op, new_args] = select_operation(x->op, slice(parts->first), dbg);
        if (new_op.k == MK::Icall_ind) {
          Regs r1 = emit_tuple(env2, new_args);
          Regs rarg(r1.begin() + 1, r1.end());
          auto [loc_arg, stack_ofs] = proc::loc_arguments(reg::typv(rarg));
          Regs a{r1[0]};
          a.insert(a.end(), loc_arg.begin(), loc_arg.end());
          if (stack_ofs == 0) {
            insert_moves(rarg, loc_arg);
            insert_debug(iop(simple(MK::Itailcall_ind)), dbg, a, {});
          } else {
            Regs rd = regs_for(ty);
            Regs loc_res = proc::loc_results(reg::typv(rd));
            insert_move_args(rarg, loc_arg, stack_ofs);
            insert_debug(iop(new_op), dbg, a, loc_res);
            mach::Operation o{MK::Istackoffset};
            o.n = -stack_ofs;
            insert(iop(o), {}, {});
            insert(idesc(IK::Ireturn), loc_res, {});
          }
          return;
        }
        if (new_op.k == MK::Icall_imm) {
          Regs r1 = emit_tuple(env2, new_args);
          auto [loc_arg, stack_ofs] = proc::loc_arguments(reg::typv(r1));
          mach::Operation call{MK::Itailcall_imm};
          call.func = new_op.func;
          if (stack_ofs == 0) {
            insert_moves(r1, loc_arg);
            insert_debug(iop(call), dbg, loc_arg, {});
          } else if (new_op.func == current_function_name) {
            Regs loc_arg2 = proc::loc_parameters(reg::typv(r1));
            insert_moves(r1, loc_arg2);
            insert_debug(iop(call), dbg, loc_arg2, {});
          } else {
            Regs rd = regs_for(ty);
            Regs loc_res = proc::loc_results(reg::typv(rd));
            insert_move_args(r1, loc_arg, stack_ofs);
            insert_debug(iop(new_op), dbg, loc_arg, loc_res);
            mach::Operation o{MK::Istackoffset};
            o.n = -stack_ofs;
            insert(iop(o), {}, {});
            insert(idesc(IK::Ireturn), loc_res, {});
          }
          return;
        }
        fatal("Selection.emit_tail");
      }
      case EK::Csequence: {
        auto* x = static_cast<const Csequence*>(exp);
        if (!emit_expr(env, x->e1)) return;
        emit_tail(env, x->e2);
        return;
      }
      case EK::Cifthenelse: {
        auto* x = static_cast<const Cifthenelse*>(exp);
        auto [cond, earg] = select_condition(x->cond);
        Res rarg = emit_expr(env, earg);
        if (!rarg) return;
        Instruction d = idesc(IK::Iifthenelse);
        d.test = cond;
        // Iifthenelse(cond, emit_tail_sequence eif, emit_tail_sequence
        // eelse): right to left
        d.ifnot = emit_tail_sequence(env, x->ifnot);
        d.ifso = emit_tail_sequence(env, x->ifso);
        insert(d, *rarg, {});
        return;
      }
      case EK::Cswitch: {
        auto* x = static_cast<const Cswitch*>(exp);
        Res rsel = emit_expr(env, x->e);
        if (!rsel) return;
        Instruction d = idesc(IK::Iswitch);
        d.index = x->index;
        for (auto& c : x->cases) d.cases.push_back(emit_tail_sequence(env, c.e));
        insert(d, *rsel, {});
        return;
      }
      case EK::Ccatch: {
        auto* x = static_cast<const Ccatch*>(exp);
        if (x->handlers.empty()) {
          emit_tail(env, x->body);
          return;
        }
        struct H {
          long nfail;
          Slice<CatchParam> ids;
          std::vector<Regs> rs;
          expression e2;
        };
        std::vector<H> handlers;
        for (auto& h : x->handlers) {
          std::vector<Regs> rs;
          for (auto& p : h.ids) {
            Regs r = regs_for(p.ty);
            name_regs(p.id, r);
            rs.push_back(r);
          }
          handlers.push_back({h.n, h.ids, rs, h.body});
        }
        Environment env2 = env;
        for (auto& h : handlers) env2 = env_add_static_exception(h.nfail, h.rs, env2);
        Instr s_body = emit_tail_sequence(env2, x->body);
        Instruction d = idesc(IK::Icatch);
        d.rec = x->rec;
        for (auto& h : handlers) {
          Environment new_env = env2;
          for (std::size_t k = 0; k < h.ids.size(); ++k) new_env = env_add(h.ids[k].id, h.rs[k], new_env);
          d.handlers.push_back({h.nfail, emit_tail_sequence(new_env, h.e2)});
        }
        d.body = s_body;
        insert(d, {}, {});
        return;
      }
      case EK::Ctrywith: {
        auto* x = static_cast<const Ctrywith*>(exp);
        auto [opt_r1, s1] = emit_sequence(env, x->body);
        Regs rv = regs_for(typ_val());
        Instr s2 = emit_tail_sequence(env_add(x->exn, rv, env), x->handler);
        Instruction d = idesc(IK::Itrywith);
        Instr h = instr_cons(iop(simple(MK::Imove)), {proc::loc_exn_bucket()}, rv, s2);
        d.ifso = s1->extract();
        d.ifnot = h;
        insert(d, {}, {});
        if (opt_r1) {
          Regs loc = proc::loc_results(reg::typv(*opt_r1));
          insert_moves(*opt_r1, loc);
          insert(idesc(IK::Ireturn), loc, {});
        }
        return;
      }
      default: break;
    }
    emit_return(env, exp);
  }

  Instr emit_tail_sequence(const Environment& env, expression exp) {
    std::unique_ptr<Selector> s = fresh();
    s->emit_tail(env, exp);
    return s->extract();
  }

  // Sequentialization of a function definition
  mach::Fundecl emit_fundecl(const FuncNames& future_funcnames, const cmm::Fundecl& f) {
    current_function_name = f.fun_name;
    std::vector<Regs> rargs;
    for (auto& p : f.fun_args) {
      Regs r = regs_for(p.ty);
      name_regs(p.id, r);
      rargs.push_back(r);
    }
    Regs rarg;
    for (const Regs& r : rargs) rarg.insert(rarg.end(), r.begin(), r.end());
    Regs loc_arg = proc::loc_parameters(reg::typv(rarg));
    // List.fold_right2: the last argument added first
    Environment env;
    for (std::size_t k = f.fun_args.size(); k-- > 0;) env = env_add(f.fun_args[k].id, rargs[k], env);
    emit_tail(env, f.fun_body);
    Instr body = extract();
    instr_seq = dummy_instr();
    insert_moves(loc_arg, rarg);
    Instr polled_body = body;
    if (polling::requires_prologue_poll(future_funcnames, f.fun_name, body))
      polled_body = instr_cons_debug(iop(simple(MK::Ipoll)), {}, {}, f.fun_dbg, body);
    Instr body_with_prologue = extract_onto(polled_body);
    mach::Fundecl r;
    r.fun_name = f.fun_name;
    r.fun_args = loc_arg;
    r.fun_body = body_with_prologue;
    r.fun_codegen_options = f.fun_codegen_options;
    r.fun_dbg = f.fun_dbg;
    r.fun_poll = f.fun_poll;
    r.fun_num_stack_slots.assign(proc::num_register_classes, 0);
    return r;
  }
};

}  // namespace cppcaml::typing::selection
