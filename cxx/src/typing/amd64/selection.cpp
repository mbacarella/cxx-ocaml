// Port of asmcomp/amd64/selection.ml: instruction selection for amd64,
// Selectgen's selector specialized.
#include "../selectgen.hpp"

namespace cppcaml::typing::selection {

namespace {

// inline_ops
bool is_inline_op(std::string_view fn) {
  return fn == "sqrt" || fn == "caml_bswap16_direct" || fn == "caml_int32_direct_bswap" ||
         fn == "caml_int64_direct_bswap" || fn == "caml_nativeint_direct_bswap";
}

bool is_immediate32(long n) { return n <= 0x7FFFFFFFL && n >= -0x80000000L; }

// Misc overflow checks on OCaml's 63-bit ints
long wrap(std::uint64_t x) { return static_cast<long>(x << 1) >> 1; }
bool no_overflow_add(long a, long b) {
  return ((a ^ b) | (a ^ ~wrap(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b)))) < 0;
}
bool no_overflow_sub(long a, long b) {
  return ((a ^ ~b) | (b ^ wrap(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)))) < 0;
}
constexpr long min_int = -(1L << 62), max_int = (1L << 62) - 1;
bool no_overflow_mul(long a, long b) {
  long p = wrap(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
  long q = b == -1 ? wrap(0 - static_cast<std::uint64_t>(p)) : (b != 0 ? p / b : 0);
  return !((a == min_int && b < 0) || (b != 0 && q != a));
}
bool no_overflow_lsl(long a, long k) { return 0 <= k && k < 63 && (min_int >> k) <= a && a <= (max_int >> k); }

// ---- addressing modes ----
struct AddrExpr {  // Asymbol | Alinear | Aadd | Ascale | Ascaledadd
  enum class K { Asymbol, Alinear, Aadd, Ascale, Ascaledadd } k;
  std::string_view sym;
  expression e1 = nullptr, e2 = nullptr;
  long scale = 0;
};


std::pair<AddrExpr, long> select_addr(expression exp) {
  std::pair<AddrExpr, long> def{{AddrExpr::K::Alinear, {}, exp}, 0};
  if (auto* s = as<Cconst_symbol>(exp); s && !clflags::dlcode) return {{AddrExpr::K::Asymbol, s->s}, 0};
  auto* c = as<Cop>(exp);
  if (!c) return def;
  long m;
  OK k = c->op.kind;
  bool addlike = k == OK::Caddi || k == OK::Caddv || k == OK::Cadda;
  if (addlike && c->args.size() == 2) {
    expression arg = nullptr;
    if (cint(c->args[1], m)) arg = c->args[0];
    else if (cint(c->args[0], m)) arg = c->args[1];
    if (arg) {
      auto [a, n] = select_addr(arg);
      if (no_overflow_add(n, m)) return {a, n + m};
      return def;
    }
  }
  if (k == OK::Csubi && c->args.size() == 2 && cint(c->args[1], m)) {
    auto [a, n] = select_addr(c->args[0]);
    if (no_overflow_sub(n, m)) return {a, n - m};
    return def;
  }
  if (k == OK::Clsl && c->args.size() == 2 && cint(c->args[1], m) && (m == 1 || m == 2 || m == 3)) {
    auto [a, n] = select_addr(c->args[0]);
    if (a.k == AddrExpr::K::Alinear && no_overflow_lsl(n, m))
      return {{AddrExpr::K::Ascale, {}, a.e1, nullptr, 1L << m}, n << m};
    return def;
  }
  if (k == OK::Cmuli && c->args.size() == 2) {
    expression arg = nullptr;
    if (cint(c->args[1], m) && (m == 2 || m == 4 || m == 8)) arg = c->args[0];
    else if (cint(c->args[0], m) && (m == 2 || m == 4 || m == 8)) arg = c->args[1];
    if (arg) {
      auto [a, n] = select_addr(arg);
      if (a.k == AddrExpr::K::Alinear && no_overflow_mul(n, m)) return {{AddrExpr::K::Ascale, {}, a.e1, nullptr, m}, n * m};
      return def;
    }
  }
  if (addlike && c->args.size() == 2) {
    expression arg1 = c->args[0], arg2 = c->args[1];
    // `match (select_addr arg1, select_addr arg2) with`: left to right
    auto r1 = select_addr(arg1);
    auto r2 = select_addr(arg2);
    auto [a1, n1] = r1;
    auto [a2, n2] = r2;
    if (a1.k == AddrExpr::K::Alinear && a2.k == AddrExpr::K::Alinear && no_overflow_add(n1, n2))
      return {{AddrExpr::K::Aadd, {}, a1.e1, a2.e1}, n1 + n2};
    if (a1.k == AddrExpr::K::Alinear && a2.k == AddrExpr::K::Ascale && no_overflow_add(n1, n2))
      return {{AddrExpr::K::Ascaledadd, {}, a1.e1, a2.e1, a2.scale}, n1 + n2};
    if (a1.k == AddrExpr::K::Ascale && a2.k == AddrExpr::K::Alinear && no_overflow_add(n2, n1))
      return {{AddrExpr::K::Ascaledadd, {}, a2.e1, a1.e1, a1.scale}, n2 + n1};
    if (a2.k == AddrExpr::K::Ascale) return {{AddrExpr::K::Ascaledadd, {}, arg1, a2.e1, a2.scale}, n2};
    if (a1.k == AddrExpr::K::Ascale) return {{AddrExpr::K::Ascaledadd, {}, arg2, a1.e1, a1.scale}, n1};
    return {{AddrExpr::K::Aadd, {}, arg1, arg2}, 0};
  }
  return def;
}

std::pair<A::AddressingMode, expression> select_addressing(Chunk, expression exp) {
  auto [a, d] = select_addr(exp);
  // PR#4625: displacement must be a signed 32-bit immediate
  if (!is_immediate32(d)) return {A::iindexed(0), exp};
  switch (a.k) {
    case AddrExpr::K::Asymbol: return {{AK::Ibased, a.sym, 0, d}, ctuple({})};
    case AddrExpr::K::Alinear: return {A::iindexed(d), a.e1};
    case AddrExpr::K::Aadd: return {{AK::Iindexed2, {}, 0, d}, ctuple(slice(std::vector<expression>{a.e1, a.e2}))};
    case AddrExpr::K::Ascale: return {{AK::Iscaled, {}, a.scale, d}, a.e1};
    case AddrExpr::K::Ascaledadd:
      return {{AK::Iindexed2scaled, {}, a.scale, d}, ctuple(slice(std::vector<expression>{a.e1, a.e2}))};
  }
  return {A::iindexed(d), exp};
}

// Structural equality of Cmm expressions (the [loc = loc'] of the store
// recognition)
bool equal_dbg(const debuginfo::t& a, const debuginfo::t& b) { return debuginfo::compare(a, b) == 0; }
bool equal_expr(expression a, expression b);
bool equal_list(Slice<expression> a, Slice<expression> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (!equal_expr(a[k], b[k])) return false;
  return true;
}
bool equal_op(const cmm::Operation& a, const cmm::Operation& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case OK::Capply: return std::equal(a.ty.begin(), a.ty.end(), b.ty.begin(), b.ty.end());
    case OK::Cextcall:
      return a.name == b.name && std::equal(a.ty.begin(), a.ty.end(), b.ty.begin(), b.ty.end()) &&
             std::equal(a.ty_args.begin(), a.ty_args.end(), b.ty_args.begin(), b.ty_args.end()) && a.alloc == b.alloc;
    case OK::Cload: return a.chunk == b.chunk && a.mut == b.mut && a.is_atomic == b.is_atomic;
    case OK::Cstore: return a.chunk == b.chunk && a.init == b.init;
    case OK::Ccmpi:
    case OK::Ccmpa: return a.icmp == b.icmp;
    case OK::Ccmpf: return a.fcmp == b.fcmp;
    case OK::Craise: return a.raise == b.raise;
    default: return true;
  }
}
bool equal_expr(expression a, expression b) {
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case EK::Cconst_int: {
      auto *x = static_cast<const Cconst_int*>(a), *y = static_cast<const Cconst_int*>(b);
      return x->n == y->n && equal_dbg(x->dbg, y->dbg);
    }
    case EK::Cconst_natint: {
      auto *x = static_cast<const Cconst_natint*>(a), *y = static_cast<const Cconst_natint*>(b);
      return x->n == y->n && equal_dbg(x->dbg, y->dbg);
    }
    case EK::Cconst_symbol: {
      auto *x = static_cast<const Cconst_symbol*>(a), *y = static_cast<const Cconst_symbol*>(b);
      return x->s == y->s && equal_dbg(x->dbg, y->dbg);
    }
    case EK::Cvar: return ident::same(static_cast<const Cvar*>(a)->id, static_cast<const Cvar*>(b)->id);
    case EK::Cvar_mut: return ident::same(static_cast<const Cvar_mut*>(a)->id, static_cast<const Cvar_mut*>(b)->id);
    case EK::Ctuple: return equal_list(static_cast<const Ctuple*>(a)->el, static_cast<const Ctuple*>(b)->el);
    case EK::Cop: {
      auto *x = static_cast<const Cop*>(a), *y = static_cast<const Cop*>(b);
      return equal_op(x->op, y->op) && equal_list(x->args, y->args) && equal_dbg(x->dbg, y->dbg);
    }
    case EK::Csequence: {
      auto *x = static_cast<const Csequence*>(a), *y = static_cast<const Csequence*>(b);
      return equal_expr(x->e1, y->e1) && equal_expr(x->e2, y->e2);
    }
    case EK::Clet: {
      auto *x = static_cast<const Clet*>(a), *y = static_cast<const Clet*>(b);
      return ident::same(x->id.var, y->id.var) && x->id.provenance == y->id.provenance && equal_expr(x->def, y->def) &&
             equal_expr(x->body, y->body);
    }
    default: return false;  // not in a store's address
  }
}

class Amd64Selector final : public Selector {
 public:
  std::unique_ptr<Selector> fresh() const override { return std::make_unique<Amd64Selector>(); }

  bool is_immediate(IO op, long n) override {
    switch (op) {
      case IO::Iadd:
      case IO::Isub:
      case IO::Imul:
      case IO::Iand:
      case IO::Ior:
      case IO::Ixor:
      case IO::Icomp:
      case IO::Icheckbound: return is_immediate32(n);
      default: return Selector::is_immediate(op, n);
    }
  }

  bool is_immediate_test(const mach::IntegerComparison&, long n) override { return is_immediate32(n); }

  bool is_simple_expr(expression e) override {
    // inlined ops are simple if their arguments are
    if (auto* x = as<Cop>(e); x && x->op.kind == OK::Cextcall && is_inline_op(x->op.name)) {
      for (expression a : x->args)
        if (!is_simple_expr(a)) return false;
      return true;
    }
    return Selector::is_simple_expr(e);
  }

  EC effects_of(expression e) override {
    if (auto* x = as<Cop>(e); x && x->op.kind == OK::Cextcall && is_inline_op(x->op.name)) {
      EC r;
      for (expression a : x->args) r = ec_join(r, effects_of(a));
      return r;
    }
    return Selector::effects_of(e);
  }

  std::pair<A::AddressingMode, expression> select_addressing(Chunk chunk, expression exp) override {
    return selection::select_addressing(chunk, exp);
  }

  // Instruction selection for stores: immediate stores
  std::pair<mach::Operation, expression> select_store(bool is_assign, const A::AddressingMode& addr,
                                                      expression exp) override {
    long n;
    if (cint(exp, n) && is_immediate32(n)) {
      mach::Operation op{MK::Ispecific};
      op.spec = {SK::Istore_int, addr, n, is_assign};
      return {op, ctuple({})};
    }
    if (auto* c = as<Cconst_natint>(exp); c && c->n <= 0x7FFFFFFFLL && c->n >= -0x80000000LL) {
      mach::Operation op{MK::Ispecific};
      op.spec = {SK::Istore_int, addr, c->n, is_assign};
      return {op, ctuple({})};
    }
    return Selector::select_store(is_assign, addr, exp);
  }

  static mach::Operation specific(SK k, A::AddressingMode addr = {}) {
    mach::Operation o{MK::Ispecific};
    o.spec.k = k;
    o.spec.addr = addr;
    return o;
  }

  // Recognize float arithmetic with mem
  Sel select_floatarith(bool commutative, MK regular_op, A::FloatOperation mem_op, Slice<expression> args) {
    auto double_load = [](expression e) -> const Cop* {
      auto* c = as<Cop>(e);
      return c && c->op.kind == OK::Cload && c->op.chunk == Chunk::Double && c->args.size() == 1 ? c : nullptr;
    };
    if (args.size() != 2) fatal("Selection.select_floatarith");
    if (auto* l = double_load(args[1])) {
      auto [addr, arg2] = select_addressing(Chunk::Double, l->args[0]);
      mach::Operation o = specific(SK::Ifloatarithmem, addr);
      o.spec.fop = mem_op;
      return {o, {args[0], arg2}};
    }
    if (auto* l = double_load(args[0]); l && commutative) {
      auto [addr, arg1] = select_addressing(Chunk::Double, l->args[0]);
      mach::Operation o = specific(SK::Ifloatarithmem, addr);
      o.spec.fop = mem_op;
      return {o, {args[1], arg1}};
    }
    return {simple(regular_op), {args[0], args[1]}};
  }

  Sel select_operation(const cmm::Operation& op, Slice<expression> args, const debuginfo::t& dbg) override {
    switch (op.kind) {
      // Recognize the LEA instruction
      case OK::Caddi:
      case OK::Caddv:
      case OK::Cadda:
      case OK::Csubi: {
        auto [addr, arg] = select_addressing(Chunk::Word_int, cop(op, args, dbg));
        if (addr.k == AK::Iindexed || (addr.k == AK::Iindexed2 && addr.displ == 0)) break;
        return {specific(SK::Ilea, addr), {arg}};
      }
      // Recognize float arithmetic with memory.
      case OK::Caddf: return select_floatarith(true, MK::Iaddf, A::FloatOperation::Ifloatadd, args);
      case OK::Csubf: return select_floatarith(false, MK::Isubf, A::FloatOperation::Ifloatsub, args);
      case OK::Cmulf: return select_floatarith(true, MK::Imulf, A::FloatOperation::Ifloatmul, args);
      case OK::Cdivf: return select_floatarith(false, MK::Idivf, A::FloatOperation::Ifloatdiv, args);
      case OK::Cextcall: {
        std::string_view fn = op.name;
        if (fn == "sqrt" && !op.alloc) {
          if (args.size() == 1) {
            if (auto* l = as<Cop>(args[0]);
                l && l->op.kind == OK::Cload && l->op.chunk == Chunk::Double && l->args.size() == 1) {
              auto [addr, arg] = select_addressing(Chunk::Double, l->args[0]);
              return {specific(SK::Ifloatsqrtf, addr), {arg}};
            }
            return {specific(SK::Isqrtf), {args[0]}};
          }
          fatal("Selection: sqrt");
        }
        auto bswap = [&](long w) {
          mach::Operation o = specific(SK::Ibswap);
          o.spec.n = w;
          return Sel{o, vec(args)};
        };
        if (fn == "caml_bswap16_direct") return bswap(16);
        if (fn == "caml_int32_direct_bswap") return bswap(32);
        if (fn == "caml_int64_direct_bswap" || fn == "caml_nativeint_direct_bswap") return bswap(64);
        break;
      }
      // Recognize store instructions
      case OK::Cstore:
        if ((op.chunk == Chunk::Word_int || op.chunk == Chunk::Word_val) && args.size() == 2) {
          long n;
          if (auto* add = as<Cop>(args[1]); add && add->op.kind == OK::Caddi && add->args.size() == 2 &&
                                            cint(add->args[1], n) && is_immediate32(n)) {
            if (auto* ld = as<Cop>(add->args[0]);
                ld && ld->op.kind == OK::Cload && ld->args.size() == 1 && equal_expr(args[0], ld->args[0])) {
              auto [addr, arg] = select_addressing(op.chunk, args[0]);
              mach::Operation o = specific(SK::Ioffset_loc, addr);
              o.spec.n = n;
              return {o, {arg}};
            }
          }
        }
        break;
      // Recognize sign extension
      case OK::Casr:
        if (args.size() == 2 && is_cint_eq(args[1], 32))
          if (auto* l = as<Cop>(args[0]);
              l && l->op.kind == OK::Clsl && l->args.size() == 2 && is_cint_eq(l->args[1], 32))
            return {specific(SK::Isextend32), {l->args[0]}};
        break;
      // Recognize zero extension
      case OK::Cand:
        if (args.size() == 2) {
          auto is_mask = [](expression e) {
            if (is_cint_eq(e, 0xffffffffL)) return true;
            auto* n = as<Cconst_natint>(e);
            return n && n->n == 0xffffffffLL;
          };
          if (is_mask(args[1])) return {specific(SK::Izextend32), {args[0]}};
          if (is_mask(args[0])) return {specific(SK::Izextend32), {args[1]}};
        }
        break;
      default: break;
    }
    return Selector::select_operation(op, args, dbg);
  }

  // Special constraints on operand and result registers
  std::optional<std::pair<Regs, Regs>> pseudoregs_for_operation(const mach::Operation& op, const Regs& arg,
                                                                const Regs& res) {
    Reg* rax = proc::phys_reg(0);
    Reg* rcx = proc::phys_reg(5);
    Reg* rdx = proc::phys_reg(4);
    switch (op.k) {
      // Two-address binary operations: arg.(0) and res.(0) must be the same
      case MK::Iintop:
        switch (op.intop.op) {
          case IO::Iadd:
          case IO::Isub:
          case IO::Imul:
          case IO::Iand:
          case IO::Ior:
          case IO::Ixor: return std::pair{Regs{res[0], arg[1]}, res};
          // For imulq, first arg must be in rax, rax is clobbered, and
          // result is in rdx.
          case IO::Imulh: return std::pair{Regs{rax, arg[1]}, Regs{rdx}};
          // For shifts with variable shift count, second arg must be in rcx
          case IO::Ilsl:
          case IO::Ilsr:
          case IO::Iasr: return std::pair{Regs{res[0], rcx}, res};
          // For div and mod, first arg must be in rax, rdx is clobbered, and
          // result is in rax or rdx respectively.  Keep it simple, just
          // force second argument in rcx.
          case IO::Idiv: return std::pair{Regs{rax, rcx}, Regs{rax}};
          case IO::Imod: return std::pair{Regs{rax, rcx}, Regs{rdx}};
          default: return std::nullopt;
        }
      case MK::Iaddf:
      case MK::Isubf:
      case MK::Imulf:
      case MK::Idivf: return std::pair{Regs{res[0], arg[1]}, res};
      // One-address unary operations: arg.(0) and res.(0) must be the same
      case MK::Iintop_imm:
        switch (op.intop.op) {
          case IO::Iadd:
          case IO::Isub:
          case IO::Imul:
          case IO::Iand:
          case IO::Ior:
          case IO::Ixor:
          case IO::Ilsl:
          case IO::Ilsr:
          case IO::Iasr: return std::pair{res, res};
          default: return std::nullopt;
        }
      case MK::Iabsf:
      case MK::Inegf: return std::pair{res, res};
      case MK::Ispecific:
        if (op.spec.k == SK::Ibswap) {
          if (op.spec.n == 32 || op.spec.n == 64) return std::pair{res, res};
          // For xchg, args must be a register allowing access to high 8 bit
          // register (rax, rbx, rcx or rdx). Keep it simple, just force the
          // argument in rax.
          if (op.spec.n == 16) return std::pair{Regs{rax}, Regs{rax}};
        }
        if (op.spec.k == SK::Ifloatarithmem) {
          Regs arg2 = arg;
          arg2[0] = res[0];
          return std::pair{arg2, res};
        }
        return std::nullopt;
      case MK::Icompf: {
        // We need to temporarily store the result of the comparison in a
        // float register, but we don't want to clobber any of the inputs if
        // they would still be live after this operation -- so we add a fresh
        // register as both an input and output.
        Reg* treg = reg::create(MC::Float);
        using FC = lambda::FloatComparison;
        bool is_swapped = op.fcmp == FC::CFgt || op.fcmp == FC::CFngt || op.fcmp == FC::CFge || op.fcmp == FC::CFnge;
        return std::pair{is_swapped ? Regs{arg[0], treg} : Regs{treg, arg[1]}, Regs{res[0], treg}};
      }
      default: return std::nullopt;
    }
  }

  Regs insert_op_debug(const mach::Operation& op, const debuginfo::t& dbg, const Regs& rs,
                       const Regs& rd) override {
    if (auto p = pseudoregs_for_operation(op, rs, rd)) {
      auto& [rsrc, rdst] = *p;
      insert_moves(rs, rsrc);
      insert_debug(iop(op), dbg, rsrc, rdst);
      insert_moves(rdst, rd);
      return rd;
    }
    return Selector::insert_op_debug(op, dbg, rs, rd);
  }
};

}  // namespace

mach::Fundecl fundecl(const FuncNames& future_funcnames, const cmm::Fundecl& f) {
  Amd64Selector s;
  return s.emit_fundecl(future_funcnames, f);
}

}  // namespace cppcaml::typing::selection
