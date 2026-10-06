// Port of asmcomp/arm64/selection.ml: instruction selection for the ARM
// processor, Selectgen's selector specialized.  OCaml's patterns are tried
// in order, a failed guard falling through to the next.
#include "../selectgen.hpp"

namespace cppcaml::typing::selection {

namespace {

using RK = A::FloatRounding;
using AO = A::ArithOperation;

bool is_offset(Chunk chunk, long n) {
  if (n >= -256 && n <= 255) return true;  // 9 bits signed unscaled
  if (n < 0) return false;
  switch (chunk) {  // 12 bits unsigned, scaled by chunk size
    case Chunk::Byte_unsigned:
    case Chunk::Byte_signed: return n < 0x1000;
    case Chunk::Sixteen_unsigned:
    case Chunk::Sixteen_signed: return (n & 1) == 0 && (n >> 1) < 0x1000;
    case Chunk::Thirtytwo_unsigned:
    case Chunk::Thirtytwo_signed:
    case Chunk::Single: return (n & 3) == 0 && (n >> 2) < 0x1000;
    case Chunk::Sixtyfour:
    case Chunk::Word_int:
    case Chunk::Word_val:
    case Chunk::Double: return (n & 7) == 0 && (n >> 3) < 0x1000;
  }
  return false;
}

bool is_logical_immediate(long n) { return A::is_logical_immediate(n); }

// Signed immediates are simpler
bool is_immediate_signed(long n) {
  long mn = -n;
  return (n & 0xFFF) == n || (n & 0xFFF000) == n || (mn & 0xFFF) == mn || (mn & 0xFFF000) == mn;
}

// If you update [inline_ops], you may need to update [is_simple_expr] and/or
// [effects_of], below.
bool is_inline_op(std::string_view fn) {
  return fn == "sqrt" || fn == "caml_bswap16_direct" || fn == "caml_fma" || fn == "caml_int32_direct_bswap" ||
         fn == "caml_int64_direct_bswap" || fn == "caml_nativeint_direct_bswap" || fn == "caml_round" ||
         fn == "caml_trunc" || fn == "ceil" || fn == "floor" || fn == "caml_int_clz_direct" ||
         fn == "caml_int_ctz_direct";
}

bool use_direct_addressing(std::string_view) { return !clflags::dlcode && !A::macosx(); }

bool is_stack_slot(const Regs& rv) {
  using LK = reg::Location::K;
  return rv.size() == 1 && rv[0]->loc.k != LK::Unknown && rv[0]->loc.k != LK::Reg;
}

// Cop(op, [a; b], _) with op one of [ks]
const Cop* cop2(expression e, std::initializer_list<OK> ks) {
  auto* c = as<Cop>(e);
  if (!c || c->args.size() != 2) return nullptr;
  for (OK k : ks)
    if (c->op.kind == k) return c;
  return nullptr;
}
// Cop(op, [arg; Cconst_int n], _) with 0 < n < 64: arg and n
bool shift_by(expression e, OK k, expression& arg, long& n) {
  const Cop* c = cop2(e, {k});
  if (!c || !is_cint(c->args[1], n) || !(n > 0 && n < 64)) return false;
  arg = c->args[0];
  return true;
}
const Cop* is_op(expression e, OK k) {
  auto* c = as<Cop>(e);
  return c && c->op.kind == k ? c : nullptr;
}
bool is_extcall(const cmm::Operation& op, std::string_view name, std::initializer_list<Exttype> ty_args) {
  return op.name == name && !op.alloc &&
         std::equal(op.ty_args.begin(), op.ty_args.end(), ty_args.begin(), ty_args.end());
}

class Arm64Selector final : public Selector {
 public:
  std::unique_ptr<Selector> fresh() const override { return std::make_unique<Arm64Selector>(); }

  bool is_immediate_test(const mach::IntegerComparison&, long n) override { return is_immediate_signed(n); }

  bool is_immediate(IO op, long n) override {
    switch (op) {
      case IO::Iadd:
      case IO::Isub: return n <= 0xFFF'FFF && n >= -0xFFF'FFF;
      case IO::Iand:
      case IO::Ior:
      case IO::Ixor: return is_logical_immediate(n);
      case IO::Icomp:
      case IO::Icheckbound: return is_immediate_signed(n);
      default: return Selector::is_immediate(op, n);
    }
  }

  // inlined floating-point ops are simple if their arguments are
  bool is_simple_expr(expression e) override {
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
    long n;
    if (const Cop* c = cop2(exp, {OK::Caddv, OK::Cadda})) {
      if (auto* s = as<Cconst_symbol>(c->args[0]); s && is_cint(c->args[1], n) && use_direct_addressing(s->s))
        return {{AK::Ibased, s->s, n}, ctuple({})};
      if (is_cint(c->args[1], n) && is_offset(chunk, n)) return {A::iindexed(n), c->args[0]};
      if (const Cop* add = cop2(c->args[1], {OK::Caddi}); add && is_cint(add->args[1], n) && is_offset(chunk, n))
        return {A::iindexed(n), cop(c->op, {c->args[0], add->args[0]}, c->dbg)};
    }
    if (auto* s = as<Cconst_symbol>(exp); s && use_direct_addressing(s->s)) return {{AK::Ibased, s->s, 0}, ctuple({})};
    return {A::iindexed(0), exp};
  }

  static mach::Operation specific(SK k) {
    mach::Operation o{MK::Ispecific};
    o.spec.k = k;
    return o;
  }
  static mach::Operation shiftarith(AO op, long n) {
    mach::Operation o = specific(SK::Ishiftarith);
    o.spec.arith = op;
    o.spec.n = n;
    return o;
  }
  static mach::Operation spec_n(SK k, long n) {
    mach::Operation o = specific(k);
    o.spec.n = n;
    return o;
  }
  static mach::Operation roundf(RK r) {
    mach::Operation o = specific(SK::Iroundf);
    o.spec.rounding = r;
    return o;
  }

  Sel select_operation(const cmm::Operation& op, Slice<expression> args, const debuginfo::t& dbg) override {
    expression a, b;
    long n;
    switch (op.kind) {
      // Integer addition
      case OK::Caddi:
      case OK::Caddv:
      case OK::Cadda: {
        if (args.size() != 2) break;
        // Shift-add
        if (shift_by(args[1], OK::Clsl, a, n)) return {shiftarith(AO::Ishiftadd, n), {args[0], a}};
        if (shift_by(args[1], OK::Casr, a, n)) return {shiftarith(AO::Ishiftadd, -n), {args[0], a}};
        if (shift_by(args[0], OK::Clsl, a, n)) return {shiftarith(AO::Ishiftadd, n), {args[1], a}};
        if (shift_by(args[0], OK::Casr, a, n)) return {shiftarith(AO::Ishiftadd, -n), {args[1], a}};
        // Multiply-add: [arg1; Cop(Cmuli, args2, dbg)] | [Cop(Cmuli, args2, dbg); arg1]
        const Cop* mul = is_op(args[1], OK::Cmuli);
        expression arg1 = args[0];
        if (!mul) {
          mul = is_op(args[0], OK::Cmuli);
          arg1 = args[1];
        }
        if (mul) {
          Sel s = select_operation(mul->op, mul->args, mul->dbg);
          if (s.first.k == MK::Iintop_imm && s.first.intop.op == IO::Ilsl && s.second.size() == 1)
            return {shiftarith(AO::Ishiftadd, s.first.n), {arg1, s.second[0]}};
          if (s.first.k == MK::Iintop && s.first.intop.op == IO::Imul && s.second.size() == 2)
            return {specific(SK::Imuladd), {s.second[0], s.second[1], arg1}};
        }
        break;
      }
      // Integer subtraction
      case OK::Csubi: {
        if (args.size() != 2) break;
        // Shift-sub
        if (shift_by(args[1], OK::Clsl, a, n)) return {shiftarith(AO::Ishiftsub, n), {args[0], a}};
        if (shift_by(args[1], OK::Casr, a, n)) return {shiftarith(AO::Ishiftsub, -n), {args[0], a}};
        // Multiply-sub
        if (const Cop* mul = is_op(args[1], OK::Cmuli)) {
          Sel s = select_operation(mul->op, mul->args, mul->dbg);
          if (s.first.k == MK::Iintop_imm && s.first.intop.op == IO::Ilsl && s.second.size() == 1)
            return {shiftarith(AO::Ishiftsub, s.first.n), {args[0], s.second[0]}};
          if (s.first.k == MK::Iintop && s.first.intop.op == IO::Imul && s.second.size() == 2)
            return {specific(SK::Imulsub), {s.second[0], s.second[1], args[0]}};
        }
        break;
      }
      // Checkbounds
      case OK::Ccheckbound:
        if (args.size() == 2 && shift_by(args[0], OK::Clsr, a, n))
          return {spec_n(SK::Ishiftcheckbound, n), {a, args[1]}};
        break;
      // Recognize sign extension
      case OK::Casr: {
        long n2;
        if (args.size() == 2 && shift_by(args[0], OK::Clsl, a, n) && is_cint(args[1], n2) && n2 == n)
          return {spec_n(SK::Isignext, 64 - n), {a}};
        break;
      }
      // Use trivial addressing mode for atomic loads
      case OK::Cload:
        if (op.is_atomic) {
          mach::Operation o{MK::Iload};
          o.chunk = op.chunk;
          o.addr = A::iindexed(0);
          o.mut = op.mut;
          o.is_atomic = true;
          return {o, vec(args)};
        }
        break;
      // Recognize floating-point negate and multiply
      case OK::Cnegf:
        if (args.size() == 1)
          if (const Cop* m = is_op(args[0], OK::Cmulf)) return {specific(SK::Inegmulf), vec(m->args)};
        break;
      // Recognize floating-point multiply and add/sub
      case OK::Caddf:
        if (args.size() == 2) {
          const Cop* m = is_op(args[1], OK::Cmulf);
          expression arg = args[0];
          if (!m) {
            m = is_op(args[0], OK::Cmulf);
            arg = args[1];
          }
          if (m) {
            std::vector<expression> v{arg};
            v.insert(v.end(), m->args.begin(), m->args.end());
            return {specific(SK::Imuladdf), v};
          }
        }
        break;
      case OK::Csubf:
        if (args.size() == 2) {
          if (const Cop* m = is_op(args[1], OK::Cmulf)) {
            std::vector<expression> v{args[0]};
            v.insert(v.end(), m->args.begin(), m->args.end());
            return {specific(SK::Imulsubf), v};
          }
          if (const Cop* m = is_op(args[0], OK::Cmulf)) {
            std::vector<expression> v{args[1]};
            v.insert(v.end(), m->args.begin(), m->args.end());
            return {specific(SK::Inegmulsubf), v};
          }
        }
        break;
      case OK::Cextcall: {
        std::string_view fn = op.name;
        using X = Exttype;
        // Recognize floating-point square root
        if (fn == "sqrt") return {specific(SK::Isqrtf), vec(args)};
        // Only the unboxed [Float.fma] passes floats in registers, hence the
        // argument types.
        if (is_extcall(op, "caml_fma", {X::XFloat, X::XFloat, X::XFloat})) {
          if (args.size() == 3) return {specific(SK::Imuladdf), {args[2], args[0], args[1]}};
          break;
        }
        // Only the unboxed rounding externals pass their argument in a
        // register, hence the argument type.
        if (is_extcall(op, "caml_round", {X::XFloat})) return {roundf(RK::Rnearest_away), vec(args)};
        if (is_extcall(op, "caml_trunc", {X::XFloat})) return {roundf(RK::Rtoward_zero), vec(args)};
        if (is_extcall(op, "ceil", {X::XFloat})) return {roundf(RK::Rtoward_pos), vec(args)};
        if (is_extcall(op, "floor", {X::XFloat})) return {roundf(RK::Rtoward_neg), vec(args)};
        // Recognize bswap instructions
        if (fn == "caml_bswap16_direct") return {spec_n(SK::Ibswap, 16), vec(args)};
        if (fn == "caml_int32_direct_bswap") return {spec_n(SK::Ibswap, 32), vec(args)};
        if (fn == "caml_int64_direct_bswap" || fn == "caml_nativeint_direct_bswap")
          return {spec_n(SK::Ibswap, 64), vec(args)};
        if (fn == "caml_int_clz_direct") return {specific(SK::Iclz), vec(args)};
        if (fn == "caml_int_ctz_direct") return {specific(SK::Ictz), vec(args)};
        break;
      }
      default: break;
    }
    // Other operations are regular
    return Selector::select_operation(op, args, dbg);
  }

  void insert_move_extcall_arg(Exttype ty_arg, const Regs& src, const Regs& dst) override {
    if (A::macosx() && ty_arg == Exttype::XInt32 && is_stack_slot(dst))
      insert(iop(specific(SK::Imove32)), src, dst);
    else insert_moves(src, dst);
  }
};

}  // namespace

mach::Fundecl fundecl(const FuncNames& future_funcnames, const cmm::Fundecl& f) {
  Arm64Selector s;
  return s.emit_fundecl(future_funcnames, f);
}

}  // namespace cppcaml::typing::selection
