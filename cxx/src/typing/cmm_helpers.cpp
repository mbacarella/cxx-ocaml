// Port of asmcomp/cmm_helpers.ml, cmmgen_state.ml and strmatch.ml.  See
// cmm_helpers.hpp.  Effects (fresh variables, raise counts, constant
// symbols, needed curry/apply/send functions) happen in OCaml's evaluation
// order: a constructor's or an application's arguments right to left.
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/cmm_helpers.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/switch.hpp"

namespace cppcaml::typing::cmmgen_state {

namespace {
struct State {
  std::map<std::string_view, Constant> constants;
  std::vector<std::vector<cmm::DataItem>> data_items;  // newest first
  std::unordered_map<std::string_view, const clambda::UStructuredConstant*> structured_constants;
  std::deque<const clambda::UFunction*> functions;
};
State& state() {
  static State s;
  return s;
}
}  // namespace

void add_constant(std::string_view sym, Constant cst) { state().constants[sym] = std::move(cst); }
void add_data_items(std::vector<cmm::DataItem> items) {
  auto& d = state().data_items;
  d.insert(d.begin(), std::move(items));
}
void add_function(const clambda::UFunction* f) { state().functions.push_back(f); }
std::map<std::string_view, Constant> get_and_clear_constants() {
  std::map<std::string_view, Constant> c = std::move(state().constants);
  state().constants.clear();
  return c;
}
std::vector<cmm::DataItem> get_and_clear_data_items() {
  // List.concat (List.rev state.data_items)
  std::vector<cmm::DataItem> r;
  auto& d = state().data_items;
  for (auto it = d.rbegin(); it != d.rend(); ++it) r.insert(r.end(), it->begin(), it->end());
  d.clear();
  return r;
}
const clambda::UFunction* next_function() {
  auto& q = state().functions;
  if (q.empty()) return nullptr;
  const clambda::UFunction* f = q.front();
  q.pop_front();
  return f;
}
bool no_more_functions() { return state().functions.empty(); }
void set_structured_constants(const std::vector<clambda::PreallocatedConstant>& l) {
  auto& t = state().structured_constants;
  t.clear();
  for (auto& c : l) t[c.symbol] = c.definition;  // Hashtbl.add: the last added is found
}
void add_structured_constant(std::string_view sym, const clambda::UStructuredConstant* cst) {
  state().structured_constants[sym] = cst;  // Hashtbl.replace
}
const clambda::UStructuredConstant* structured_constant_of_sym(std::string_view s) {
  if (const clambda::UStructuredConstant* c = compilenv::structured_constant_of_symbol(s)) return c;
  auto& t = state().structured_constants;
  auto it = t.find(s);
  return it == t.end() ? nullptr : it->second;
}

}  // namespace cppcaml::typing::cmmgen_state

namespace cppcaml::typing::cmm_helpers {

using namespace cmm;
using OK = Operation::K;
using MC = MemoryChunk;
using clambda::MemoryAccessSize;
using IC = lambda::IntegerComparison;
using FC = lambda::FloatComparison;
namespace L = lambda;

namespace {

[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }

// ---- OCaml's 63-bit int arithmetic -----------------------------------------------------------
constexpr long max_int = (1L << 62) - 1;
constexpr long min_int = -(1L << 62);
long wrap(std::uint64_t x) { return static_cast<long>(x << 1) >> 1; }
long iadd(long a, long b) { return wrap(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b)); }
long isub(long a, long b) { return wrap(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)); }
long imul(long a, long b) { return wrap(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)); }
long ineg(long a) { return wrap(0 - static_cast<std::uint64_t>(a)); }
long ilsl(long a, long k) { return wrap(static_cast<std::uint64_t>(a) << (k & 63)); }
long idiv(long a, long b) { return b == -1 ? ineg(a) : a / b; }
long imod(long a, long b) { return b == -1 ? 0 : a % b; }

Operation O(OK k) { return op(k); }
Slice<Exttype> no_exttypes() { return {}; }
Slice<Exttype> exttypes(std::initializer_list<Exttype> l) { return slice(std::vector<Exttype>(l)); }
VarWithProvenance vpc(Var v) { return {v, nullptr}; }

}  // namespace

bool no_overflow_add(long a, long b) { return ((a ^ b) | (a ^ ~iadd(a, b))) < 0; }
bool no_overflow_sub(long a, long b) { return ((a ^ ~b) | (b ^ isub(a, b))) < 0; }
bool no_overflow_mul(long a, long b) { return !((a == min_int && b < 0) || (b != 0 && idiv(imul(a, b), b) != a)); }
bool no_overflow_lsl(long a, long k) { return 0 <= k && k < 64 - 1 && (min_int >> k) <= a && a <= (max_int >> k); }
long log2(long n) { return n <= 1 ? 0 : 1 + log2(n >> 1); }

// ---- Local binding of complex expressions ----------------------------------------------------
expression bind(const char* name, expression arg, const Body& fn) {
  switch (arg->kind) {
    case EK::Cvar:
    case EK::Cconst_int:
    case EK::Cconst_natint:
    case EK::Cconst_symbol: return fn(arg);
    default: {
      Var id = Ident::create_local(name);
      return clet(vpc(id), arg, fn(cvar(id)));
    }
  }
}

expression bind_load(const char* name, expression arg, const Body& fn) {
  if (auto* c = as<Cop>(arg); c && c->op.kind == OK::Cload && c->args.size() == 1 && c->args[0]->kind == EK::Cvar)
    return fn(arg);
  return bind(name, arg, fn);
}

namespace {
constexpr std::int64_t caml_black = std::int64_t{3} << 8;  // cf. runtime/caml/gc.h

Operation mk_load_immut(MC c) { return cload(c, MutableFlag::Immutable); }
Operation mk_load_mut_op(MC c) { return cload(c, MutableFlag::Mutable); }

// Obj's tags
constexpr long closure_tag = 247, object_tag = 248, infix_tag = 249, string_tag = 252, double_tag = 253,
               double_array_tag = 254, custom_tag = 255;

expression floatarray_tag(const Dbg& dbg) { return cconst_int(double_array_tag, dbg); }
}  // namespace

expression mk_load_mut(MC c, expression arg, const Dbg& dbg) { return cop(mk_load_mut_op(c), {arg}, dbg); }
expression mk_load_atomic(MC c, expression arg, const Dbg& dbg) {
  return cop(cload(c, MutableFlag::Mutable, true), {arg}, dbg);
}

// Block headers. Meaning of the tag field: see stdlib/obj.ml
std::int64_t block_header(long tag, long sz) { return (static_cast<std::int64_t>(sz) << 10) + tag; }
// Static data corresponding to "value"s must be marked black in case we are
// in no-naked-pointers mode.  See [caml_darken] and the code below that
// emits structured constants and static module definitions.
namespace {
std::int64_t black_block_header(long tag, long sz) { return block_header(tag, sz) | caml_black; }
std::int64_t white_closure_header(long sz) { return block_header(closure_tag, sz); }
std::int64_t floatarray_header(long len) {
  // Zero-sized float arrays have tag zero for consistency with
  // [caml_alloc_float_array].
  if (len == 0) return block_header(0, 0);
  return block_header(double_array_tag, len * size_float / size_addr);
}
std::int64_t string_header(long len) { return block_header(string_tag, (len + size_addr) / size_addr); }
constexpr long pos_arity_in_closinfo = 8 * size_addr - 8;  // arity = the top 8 bits of the closinfo word
}  // namespace
std::int64_t black_closure_header(long sz) { return black_block_header(closure_tag, sz); }
std::int64_t infix_header(long ofs) { return block_header(infix_tag, ofs); }
const std::int64_t float_header = block_header(double_tag, size_float / size_addr);
const std::int64_t boxedint32_header = block_header(custom_tag, 2);
const std::int64_t boxedint64_header = block_header(custom_tag, 1 + 8 / size_addr);
const std::int64_t boxedintnat_header = block_header(custom_tag, 2);
const char* const caml_nativeint_ops = "caml_nativeint_ops";
const char* const caml_int32_ops = "caml_int32_ops";
const char* const caml_int64_ops = "caml_int64_ops";

std::int64_t closure_info(long arity, long startenv) {
  return (static_cast<std::int64_t>(static_cast<std::uint64_t>(arity) << pos_arity_in_closinfo)) +
         ((static_cast<std::int64_t>(startenv) << 1) + 1);
}

namespace {
expression alloc_float_header(const Dbg& dbg) { return cconst_natint(float_header, dbg); }
expression alloc_floatarray_header(long len, const Dbg& dbg) { return cconst_natint(floatarray_header(len), dbg); }
expression alloc_closure_header(long sz, const Dbg& dbg) { return cconst_natint(white_closure_header(sz), dbg); }
}  // namespace
expression alloc_infix_header(long ofs, const Dbg& dbg) { return cconst_natint(infix_header(ofs), dbg); }
expression alloc_closure_info(long arity, long startenv, const Dbg& dbg) {
  return cconst_natint(closure_info(arity, startenv), dbg);
}

// ---- Integers --------------------------------------------------------------------------------
namespace {
constexpr long max_repr_int = max_int >> 1;
constexpr long min_repr_int = min_int >> 1;
std::int64_t tag_const(long n) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(n) << 1) + 1; }
long untag_const(std::int64_t n) {
  if ((n & 1) != 1) fatal("Cmm_helpers.untag_const was called on an non-tagged constant");
  return wrap(static_cast<std::uint64_t>(n >> 1));
}
}  // namespace

expression int_const(const Dbg& dbg, long n) {
  if (n <= max_repr_int && n >= min_repr_int) return cconst_int((n << 1) + 1, dbg);
  return cconst_natint(tag_const(n), dbg);
}

expression natint_const_untagged(const Dbg& dbg, std::int64_t n) {
  if (n > max_int || n < min_int) return cconst_natint(n, dbg);
  return cconst_int(static_cast<long>(n), dbg);
}

DataItem cint_const(long n) { return data_int(DataItem::K::Cint, tag_const(n)); }

namespace {
expression add_no_overflow(long n, long x, expression c, const Dbg& dbg) {
  long d = iadd(n, x);
  if (d == 0) return c;
  return cop(O(OK::Caddi), {c, cconst_int(d, dbg)}, dbg);
}
// Cop(op, [a; Cconst_int (x, _)], _)
bool op_c_const(expression e, OK k, expression& a, long& x) {
  auto* c = as<Cop>(e);
  if (!c || c->op.kind != k || c->args.size() != 2 || !is_cint(c->args[1], x)) return false;
  a = c->args[0];
  return true;
}
// Cop(op, [Cconst_int (x, _); a], _)
bool op_const_c(expression e, OK k, long& x, expression& a) {
  auto* c = as<Cop>(e);
  if (!c || c->op.kind != k || c->args.size() != 2 || !is_cint(c->args[0], x)) return false;
  a = c->args[1];
  return true;
}
}  // namespace

expression add_const(expression c, long n, const Dbg& dbg) {
  if (n == 0) return c;
  long x;
  expression a;
  if (is_cint(c, x) && no_overflow_add(x, n)) return cconst_int(x + n, dbg);
  if (op_const_c(c, OK::Caddi, x, a) && no_overflow_add(n, x)) return add_no_overflow(n, x, a, dbg);
  if (op_c_const(c, OK::Caddi, a, x) && no_overflow_add(n, x)) return add_no_overflow(n, x, a, dbg);
  if (op_const_c(c, OK::Csubi, x, a) && no_overflow_add(n, x))
    return cop(O(OK::Csubi), {cconst_int(n + x, dbg), a}, dbg);
  if (op_c_const(c, OK::Csubi, a, x) && no_overflow_sub(n, x)) return add_const(a, n - x, dbg);
  return cop(O(OK::Caddi), {c, cconst_int(n, dbg)}, dbg);
}

expression incr_int(expression c, const Dbg& dbg) { return add_const(c, 1, dbg); }
expression decr_int(expression c, const Dbg& dbg) { return add_const(c, -1, dbg); }

namespace {
expression offset_addr(expression c1, expression c2, const Dbg& dbg) {
  if (is_cint_eq(c2, 0)) return c1;
  if (auto* n = as<Cconst_natint>(c2); n && n->n == 0) return c1;
  expression c;
  long n1;
  if (op_c_const(c1, OK::Cadda, c, n1)) return cop(O(OK::Cadda), {c, add_const(c2, n1, dbg)}, dbg);
  return cop(O(OK::Cadda), {c1, c2}, dbg);
}
}  // namespace

expression add_int(expression c1, expression c2, const Dbg& dbg) {
  long n;
  if (is_cint(c1, n)) return add_const(c2, n, dbg);
  if (is_cint(c2, n)) return add_const(c1, n, dbg);
  expression a;
  if (op_c_const(c1, OK::Caddi, a, n)) return add_const(add_int(a, c2, dbg), n, dbg);
  if (op_c_const(c2, OK::Caddi, a, n)) return add_const(add_int(c1, a, dbg), n, dbg);
  return cop(O(OK::Caddi), {c1, c2}, dbg);
}

expression sub_int(expression c1, expression c2, const Dbg& dbg) {
  long n;
  expression a;
  if (is_cint(c2, n) && n != min_int) return add_const(c1, -n, dbg);
  if (op_c_const(c2, OK::Caddi, a, n) && n != min_int) return add_const(sub_int(c1, a, dbg), -n, dbg);
  if (op_c_const(c1, OK::Caddi, a, n)) return add_const(sub_int(a, c2, dbg), n, dbg);
  return cop(O(OK::Csubi), {c1, c2}, dbg);
}

expression lsl_int(expression c1, expression c2, const Dbg& dbg) {
  long n1, n2;
  expression c;
  if (op_c_const(c1, OK::Clsl, c, n1) && is_cint(c2, n2) && n1 > 0 && n2 > 0 && n1 + n2 < size_int * 8)
    return cop(O(OK::Clsl), {c, cconst_int(n1 + n2, dbg)}, dbg);
  if (op_c_const(c1, OK::Caddi, c, n1) && is_cint(c2, n2) && no_overflow_lsl(n1, n2))
    return add_const(lsl_int(c, c2, dbg), ilsl(n1, n2), dbg);
  return cop(O(OK::Clsl), {c1, c2}, dbg);
}

namespace {
bool is_power2(long n) { return n == ilsl(1, log2(n)); }
expression mult_power2(expression c, long n, const Dbg& dbg) { return lsl_int(c, cconst_int(log2(n), dbg), dbg); }
}  // namespace

expression mul_int(expression c1, expression c2, const Dbg& dbg) {
  long n, k;
  expression c;
  if (is_cint_eq(c2, 0)) return csequence(c1, cconst_int(0, dbg));
  if (is_cint_eq(c1, 0)) return csequence(c2, cconst_int(0, dbg));
  if (is_cint_eq(c2, 1)) return c1;
  if (is_cint_eq(c1, 1)) return c2;
  if (is_cint_eq(c2, -1)) return sub_int(cconst_int(0, dbg), c1, dbg);
  if (is_cint_eq(c1, -1)) return sub_int(cconst_int(0, dbg), c2, dbg);
  if (is_cint(c2, n) && is_power2(n)) return mult_power2(c1, n, dbg);
  if (is_cint(c1, n) && is_power2(n)) return mult_power2(c2, n, dbg);
  if (op_c_const(c1, OK::Caddi, c, n) && is_cint(c2, k) && no_overflow_mul(n, k))
    return add_const(mul_int(c, cconst_int(k, dbg), dbg), imul(n, k), dbg);
  if (is_cint(c1, k) && op_c_const(c2, OK::Caddi, c, n) && no_overflow_mul(n, k))
    return add_const(mul_int(c, cconst_int(k, dbg), dbg), imul(n, k), dbg);
  return cop(O(OK::Cmuli), {c1, c2}, dbg);
}

expression ignore_low_bit_int(expression e) {
  expression a;
  long one;
  if (op_c_const(e, OK::Caddi, a, one) && one == 1) {
    expression c;
    long n;
    if (op_c_const(a, OK::Clsl, c, n) && n > 0) return a;
  }
  if (op_c_const(e, OK::Cor, a, one) && one == 1) return a;
  return e;
}

// removes the 1-bit sign-extension left by untag_int (tag_int c)
expression ignore_high_bit_int(expression e) {
  expression a, c;
  long n;
  if (op_c_const(e, OK::Casr, a, n) && n == 1 && op_c_const(a, OK::Clsl, c, n) && n == 1) return c;
  return e;
}

expression lsr_int(expression c1, expression c2, const Dbg& dbg) {
  long n;
  if (is_cint_eq(c2, 0)) return c1;
  if (is_cint(c2, n) && n > 0) return cop(O(OK::Clsr), {ignore_low_bit_int(c1), c2}, dbg);
  return cop(O(OK::Clsr), {c1, c2}, dbg);
}

expression asr_int(expression c1, expression c2, const Dbg& dbg) {
  long n;
  if (is_cint_eq(c2, 0)) return c1;
  if (is_cint(c2, n) && n > 0) return cop(O(OK::Casr), {ignore_low_bit_int(c1), c2}, dbg);
  return cop(O(OK::Casr), {c1, c2}, dbg);
}

expression tag_int(expression i, const Dbg& dbg) {
  long n;
  expression c;
  if (is_cint(i, n)) return int_const(dbg, n);
  if (op_c_const(i, OK::Casr, c, n) && n > 0)
    return cop(O(OK::Cor), {asr_int(c, cconst_int(n - 1, dbg), dbg), cconst_int(1, dbg)}, dbg);
  return incr_int(lsl_int(i, cconst_int(1, dbg), dbg), dbg);
}

expression untag_int(expression i, const Dbg& dbg) {
  long n, one;
  if (is_cint(i, n)) return cconst_int(n >> 1, dbg);
  expression a, c;
  if (op_c_const(i, OK::Cor, a, one) && one == 1) {
    if (op_c_const(a, OK::Casr, c, n) && n > 0 && n < size_int * 8)
      return cop(O(OK::Casr), {c, cconst_int(n + 1, dbg)}, dbg);
    if (op_c_const(a, OK::Clsr, c, n) && n > 0 && n < size_int * 8)
      return cop(O(OK::Clsr), {c, cconst_int(n + 1, dbg)}, dbg);
  }
  return asr_int(i, cconst_int(1, dbg), dbg);
}

expression mk_if_then_else(const Dbg& dbg, expression cond, const Dbg& ifso_dbg, expression ifso,
                           const Dbg& ifnot_dbg, expression ifnot) {
  if (is_cint_eq(cond, 0)) return ifnot;
  if (is_cint_eq(cond, 1)) return ifso;
  return cifthenelse(cond, ifso_dbg, ifso, ifnot_dbg, ifnot, dbg);
}

expression mk_not(const Dbg& dbg, expression cmm) {
  expression a, c;
  long one, n;
  if (auto* add = as<Cop>(cmm); add && op_c_const(cmm, OK::Caddi, a, one) && one == 1 &&
                                op_c_const(a, OK::Clsl, c, n) && n == 1) {
    const Dbg& dbg1 = add->dbg;
    if (auto* cmp = as<Cop>(c); cmp && cmp->args.size() == 2) {
      if (cmp->op.kind == OK::Ccmpi)
        return tag_int(cop(ccmpi(L::negate_integer_comparison(cmp->op.icmp)), cmp->args, cmp->dbg), dbg1);
      if (cmp->op.kind == OK::Ccmpa)
        return tag_int(cop(ccmpa(L::negate_integer_comparison(cmp->op.icmp)), cmp->args, cmp->dbg), dbg1);
      if (cmp->op.kind == OK::Ccmpf)
        return tag_int(cop(ccmpf(L::negate_float_comparison(cmp->op.fcmp)), cmp->args, cmp->dbg), dbg1);
    }
    // 0 -> 3, 1 -> 1
    return cop(O(OK::Csubi), {cconst_int(3, dbg), cop(O(OK::Clsl), {c, cconst_int(1, dbg)}, dbg)}, dbg);
  }
  if (is_cint_eq(cmm, 3)) return cconst_int(1, dbg);
  if (is_cint_eq(cmm, 1)) return cconst_int(3, dbg);
  // 1 -> 3, 3 -> 1
  return cop(O(OK::Csubi), {cconst_int(4, dbg), cmm}, dbg);
}

expression mk_compare_ints(const Dbg& dbg, expression a1, expression a2) {
  auto cmp = [](auto x, auto y) -> long { return x < y ? -1 : x > y ? 1 : 0; };
  auto* i1 = as<Cconst_int>(a1);
  auto* i2 = as<Cconst_int>(a2);
  auto* n1 = as<Cconst_natint>(a1);
  auto* n2 = as<Cconst_natint>(a2);
  if (i1 && i2) return int_const(dbg, cmp(i1->n, i2->n));
  if (n1 && n2) return int_const(dbg, cmp(n1->n, n2->n));
  if (i1 && n2) return int_const(dbg, cmp(static_cast<std::int64_t>(i1->n), n2->n));
  if (n1 && i2) return int_const(dbg, cmp(n1->n, static_cast<std::int64_t>(i2->n)));
  return bind("int_cmp", a2, [&](expression a2) {
    return bind("int_cmp", a1, [&](expression a1) {
      expression op1 = cop(ccmpi(IC::Cgt), {a1, a2}, dbg);
      expression op2 = cop(ccmpi(IC::Clt), {a1, a2}, dbg);
      return tag_int(sub_int(op1, op2, dbg), dbg);
    });
  });
}

expression mk_compare_floats(const Dbg& dbg, expression a1, expression a2) {
  return bind("float_cmp", a2, [&](expression a2) {
    return bind("float_cmp", a1, [&](expression a1) {
      expression op1 = cop(ccmpf(FC::CFgt), {a1, a2}, dbg);
      expression op2 = cop(ccmpf(FC::CFlt), {a1, a2}, dbg);
      expression op3 = cop(ccmpf(FC::CFeq), {a1, a1}, dbg);
      expression op4 = cop(ccmpf(FC::CFeq), {a2, a2}, dbg);
      // If both operands a1 and a2 are not NaN, then op3 = op4 = 1, and the
      // result is op1 - op2.  If at least one of the operands is NaN, then
      // op1 = op2 = 0, and the result is op3 - op4, which orders NaN before
      // other values.  See also caml_float_compare_unboxed in
      // runtime/floats.c
      return tag_int(add_int(sub_int(op1, op2, dbg), sub_int(op3, op4, dbg), dbg), dbg);
    });
  });
}

expression create_loop(expression body, const Dbg& dbg) {
  long cont = L::next_raise_count();
  expression call_cont = cexit(cont, {});
  expression b = csequence(body, call_cont);
  return ccatch_node(cmm::RecFlag::Recursive, slice(std::vector<Handler>{{cont, {}, b, dbg}}), call_cont);
}

// ---- Turning integer divisions into multiply-high then shift ---------------------------------
namespace {
// Unsigned comparison between native integers.
int ucompare(std::int64_t x, std::int64_t y) {
  auto ux = static_cast<std::uint64_t>(x), uy = static_cast<std::uint64_t>(y);
  return ux < uy ? -1 : ux > uy ? 1 : 0;
}
// Unsigned division and modulus at type nativeint.  Algorithm: Hacker's
// Delight section 9.3
std::pair<std::int64_t, std::int64_t> udivmod(std::int64_t n, std::int64_t d) {
  auto sub = [](std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
  };
  if (d < 0) {
    if (ucompare(n, d) < 0) return {0, n};
    return {1, sub(n, d)};
  }
  std::int64_t q = static_cast<std::int64_t>(
      static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::uint64_t>(n) >> 1) / d) << 1);
  std::int64_t r = sub(n, static_cast<std::int64_t>(static_cast<std::uint64_t>(q) * static_cast<std::uint64_t>(d)));
  if (ucompare(r, d) >= 0) return {q + 1, sub(r, d)};
  return {q, r};
}
// Compute division parameters.  Algorithm: Hacker's Delight chapter 10,
// fig 10-1.
std::pair<std::int64_t, long> divimm_parameters(std::int64_t d) {
  auto sub = [](std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
  };
  auto shl1 = [](std::int64_t a) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) << 1); };
  auto succ = [](std::int64_t a) { return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + 1); };
  constexpr long size = 64;
  const std::int64_t twopsm1 = std::numeric_limits<std::int64_t>::min();  // 2^63
  std::int64_t nc = sub(sub(twopsm1, 1), udivmod(twopsm1, d).second);
  long p = size - 1;
  auto [q1, r1] = udivmod(twopsm1, nc);
  auto [q2, r2] = udivmod(twopsm1, d);
  for (;;) {
    p = p + 1;
    q1 = shl1(q1);
    r1 = shl1(r1);
    if (ucompare(r1, nc) >= 0) {
      q1 = succ(q1);
      r1 = sub(r1, nc);
    }
    q2 = shl1(q2);
    r2 = shl1(r2);
    if (ucompare(r2, d) >= 0) {
      q2 = succ(q2);
      r2 = sub(r2, d);
    }
    std::int64_t delta = sub(d, r2);
    if (ucompare(q1, delta) < 0 || (q1 == delta && r1 == 0)) continue;
    return {succ(q2), p - size};
  }
}

expression raise_symbol(const Dbg& dbg, std::string_view symb) {
  return cop(craise(L::RaiseKind::Raise_regular), {cconst_symbol(symb, dbg)}, dbg);
}

expression div_int(expression c1, expression c2, L::IsSafe is_safe, const Dbg& dbg) {
  long n1, n;
  if (is_cint_eq(c2, 0)) return csequence(c1, raise_symbol(dbg, "caml_exn_Division_by_zero"));
  if (is_cint_eq(c2, 1)) return c1;
  if (is_cint(c1, n1) && is_cint(c2, n)) return cconst_int(idiv(n1, n), dbg);
  if (is_cint(c2, n) && n != min_int) {
    long l = log2(n);
    if (n == ilsl(1, l)) {
      // Algorithm:
      //   t = shift-right-signed(c1, l - 1)
      //   t = shift-right(t, W - l)
      //   t = c1 + t
      //   res = shift-right-signed(c1 + t, l)
      return cop(O(OK::Casr),
                 {bind("dividend", c1,
                       [&](expression c1) {
                         expression t = asr_int(c1, cconst_int(l - 1, dbg), dbg);
                         t = lsr_int(t, cconst_int(64 - l, dbg), dbg);
                         return add_int(c1, t, dbg);
                       }),
                  cconst_int(l, dbg)},
                 dbg);
    }
    if (n < 0) return sub_int(cconst_int(0, dbg), div_int(c1, cconst_int(-n, dbg), is_safe, dbg), dbg);
    auto [m, p] = divimm_parameters(n);
    // Algorithm:
    //   t = multiply-high-signed(c1, m)
    //   if m < 0, t = t + c1
    //   if p > 0, t = shift-right-signed(t, p)
    //   res = t + sign-bit(c1)
    return bind("dividend", c1, [&, m = m, p = p](expression c1) {
      expression t = cop(O(OK::Cmulhi), {c1, natint_const_untagged(dbg, m)}, dbg);
      if (m < 0) t = cop(O(OK::Caddi), {t, c1}, dbg);
      if (p > 0) t = cop(O(OK::Casr), {t, cconst_int(p, dbg)}, dbg);
      return add_int(t, lsr_int(c1, cconst_int(64 - 1, dbg), dbg), dbg);
    });
  }
  if (clflags::unsafe || is_safe == L::IsSafe::Unsafe) return cop(O(OK::Cdivi), {c1, c2}, dbg);
  return bind("divisor", c2, [&](expression c2) {
    return bind("dividend", c1, [&](expression c1) {
      return cifthenelse(c2, dbg, cop(O(OK::Cdivi), {c1, c2}, dbg), dbg,
                         raise_symbol(dbg, "caml_exn_Division_by_zero"), dbg);
    });
  });
}

expression mod_int(expression c1, expression c2, L::IsSafe is_safe, const Dbg& dbg) {
  long n1, n;
  if (is_cint_eq(c2, 0)) return csequence(c1, raise_symbol(dbg, "caml_exn_Division_by_zero"));
  if (is_cint_eq(c2, 1) || is_cint_eq(c2, -1)) return csequence(c1, cconst_int(0, dbg));
  if (is_cint(c1, n1) && is_cint(c2, n)) return cconst_int(imod(n1, n), dbg);
  if (is_cint(c2, n) && n != min_int) {
    long l = log2(n);
    if (n == ilsl(1, l)) {
      // Algorithm:
      //   t = shift-right-signed(c1, l - 1)
      //   t = shift-right(t, W - l)
      //   t = c1 + t
      //   t = bit-and(t, -n)
      //   res = c1 - t
      return bind("dividend", c1, [&](expression c1) {
        expression t = asr_int(c1, cconst_int(l - 1, dbg), dbg);
        t = lsr_int(t, cconst_int(64 - l, dbg), dbg);
        t = add_int(c1, t, dbg);
        t = cop(O(OK::Cand), {t, cconst_int(-n, dbg)}, dbg);
        return sub_int(c1, t, dbg);
      });
    }
    return bind("dividend", c1,
                [&](expression c1) { return sub_int(c1, mul_int(div_int(c1, c2, is_safe, dbg), c2, dbg), dbg); });
  }
  // Flambda already generates that test
  if (clflags::unsafe || is_safe == L::IsSafe::Unsafe) return cop(O(OK::Cmodi), {c1, c2}, dbg);
  return bind("divisor", c2, [&](expression c2) {
    return bind("dividend", c1, [&](expression c1) {
      return cifthenelse(c2, dbg, cop(O(OK::Cmodi), {c1, c2}, dbg), dbg,
                         raise_symbol(dbg, "caml_exn_Division_by_zero"), dbg);
    });
  });
}

// Division or modulo on boxed integers.  The overflow case min_int / -1
// can occur, in which case we force x / -1 = -x and x mod -1 = 0.
// (PR#5513).
bool is_different_from(long x, expression e) {
  if (auto* c = as<Cconst_int>(e)) return c->n != x;
  if (auto* c = as<Cconst_natint>(e)) return c->n != static_cast<std::int64_t>(x);
  return false;
}

using DivOp = expression (*)(expression, expression, L::IsSafe, const Dbg&);
expression safe_divmod_bi(DivOp mkop, L::IsSafe is_safe, const std::function<expression(expression, const Dbg&)>& mkm1,
                          expression c1, expression c2, BoxedInteger bi, const Dbg& dbg) {
  return bind("divisor", c2, [&](expression c2) {
    return bind("dividend", c1, [&](expression c1) {
      expression c = mkop(c1, c2, is_safe, dbg);
      // Arch.division_crashes_on_overflow
      if (bi != BoxedInteger::Pint32 && !is_different_from(-1, c2))
        return cifthenelse(cop(ccmpi(IC::Cne), {c2, cconst_int(-1, dbg)}, dbg), dbg, c, dbg, mkm1(c1, dbg), dbg);
      return c;
    });
  });
}
}  // namespace

expression safe_div_bi(L::IsSafe is_safe, expression c1, expression c2, BoxedInteger bi, const Dbg& dbg) {
  return safe_divmod_bi(
      div_int, is_safe,
      [](expression c1, const Dbg& dbg) { return cop(O(OK::Csubi), {cconst_int(0, dbg), c1}, dbg); }, c1, c2, bi,
      dbg);
}
expression safe_mod_bi(L::IsSafe is_safe, expression c1, expression c2, BoxedInteger bi, const Dbg& dbg) {
  return safe_divmod_bi(
      mod_int, is_safe, [](expression, const Dbg& dbg) { return cconst_int(0, dbg); }, c1, c2, bi, dbg);
}

// ---- Bool ------------------------------------------------------------------------------------
expression test_bool(const Dbg& dbg, expression cmm) {
  expression a, c;
  long one, n;
  if (op_c_const(cmm, OK::Caddi, a, one) && one == 1 && op_c_const(a, OK::Clsl, c, n) && n == 1) return c;
  if (auto* k = as<Cconst_int>(cmm)) return cconst_int(k->n == 1 ? 0 : 1, k->dbg);
  return cop(ccmpi(IC::Cne), {cmm, cconst_int(1, dbg)}, dbg);
}

// ---- Float -----------------------------------------------------------------------------------
expression box_float(const Dbg& dbg, expression c) { return cop(O(OK::Calloc), {alloc_float_header(dbg), c}, dbg); }

expression unbox_float(const Dbg& dbg, expression e) {
  return map_tail(
      [&](expression cmm) -> expression {
        if (auto* a = as<Cop>(cmm); a && a->op.kind == OK::Calloc && a->args.size() == 2)
          if (auto* h = as<Cconst_natint>(a->args[0]); h && h->n == float_header) return a->args[1];
        if (auto* s = as<Cconst_symbol>(cmm)) {
          const clambda::UStructuredConstant* c = cmmgen_state::structured_constant_of_sym(s->s);
          if (c && c->kind == clambda::UStructuredConstant::Kind::Uconst_float)
            return cconst_float(c->f, dbg);  // or keep _dbg?
          return cop(mk_load_immut(MC::Double), {cmm}, dbg);
        }
        return cop(mk_load_immut(MC::Double), {cmm}, dbg);
      },
      e);
}

// Conversions for 16-bit floats
expression float_of_float16(const Dbg& dbg, expression c) {
  return cop(cextcall("caml_double_of_float16", typ_float(), exttypes({Exttype::XInt}), false), {c}, dbg);
}
expression float16_of_float(const Dbg& dbg, expression c) {
  return cop(cextcall("caml_float16_of_double", typ_int(), exttypes({Exttype::XFloat}), false), {c}, dbg);
}

// Complex
namespace {
expression box_complex(const Dbg& dbg, expression c_re, expression c_im) {
  return cop(O(OK::Calloc), {alloc_floatarray_header(2, dbg), c_re, c_im}, dbg);
}
expression complex_re(expression c, const Dbg& dbg) { return cop(mk_load_immut(MC::Double), {c}, dbg); }
expression complex_im(expression c, const Dbg& dbg) {
  return cop(mk_load_immut(MC::Double), {cop(O(OK::Cadda), {c, cconst_int(size_float, dbg)}, dbg)}, dbg);
}
}  // namespace

// ---- Unit ------------------------------------------------------------------------------------
expression return_unit(const Dbg& dbg, expression c) { return csequence(c, cconst_int(1, dbg)); }

expression remove_unit(expression c) {
  switch (c->kind) {
    case EK::Cconst_int:
      if (is_cint_eq(c, 1)) return ctuple({});
      break;
    case EK::Csequence: {
      auto* x = static_cast<const Csequence*>(c);
      if (is_cint_eq(x->e2, 1)) return x->e1;
      return csequence(x->e1, remove_unit(x->e2));
    }
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(c);
      // right to left
      expression ifnot = remove_unit(x->ifnot);
      expression ifso = remove_unit(x->ifso);
      return cifthenelse(x->cond, x->ifso_dbg, ifso, x->ifnot_dbg, ifnot, x->dbg);
    }
    case EK::Cswitch: {
      auto* x = static_cast<const Cswitch*>(c);
      std::vector<SwitchCase> cases;
      for (auto& k : x->cases) cases.push_back({remove_unit(k.e), k.dbg});
      return cswitch(x->e, x->index, slice(cases), x->dbg);
    }
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(c);
      expression body = remove_unit(x->body);
      std::vector<Handler> hs;
      for (auto& h : x->handlers) hs.push_back({h.n, h.ids, remove_unit(h.body), h.dbg});
      return ccatch_node(x->rec, slice(hs), body);
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(c);
      expression handler = remove_unit(x->handler);
      expression body = remove_unit(x->body);
      return ctrywith(body, x->exn, handler, x->dbg);
    }
    case EK::Clet: {
      auto* x = static_cast<const Clet*>(c);
      return clet(x->id, x->def, remove_unit(x->body));
    }
    case EK::Cop: {
      auto* x = static_cast<const Cop*>(c);
      if (x->op.kind == OK::Capply) return cop(capply(typ_void()), x->args, x->dbg);
      if (x->op.kind == OK::Cextcall)
        return cop(cextcall(x->op.name, typ_void(), x->op.ty_args, x->op.alloc), x->args, x->dbg);
      break;
    }
    case EK::Cexit: return c;
    case EK::Ctuple:
      if (static_cast<const Ctuple*>(c)->el.empty()) return c;
      break;
    default: break;
  }
  return csequence(c, ctuple({}));
}

expression field_address(expression ptr, long n, const Dbg& dbg) {
  if (n == 0) return ptr;
  return cop(O(OK::Cadda), {ptr, cconst_int(n * size_addr, dbg)}, dbg);
}

expression get_field_gen(MutableFlag mut, expression ptr, long n, const Dbg& dbg, MC chunk) {
  return cop(cload(chunk, mut), {field_address(ptr, n, dbg)}, dbg);
}

namespace {
expression get_field_codepointer(MutableFlag mut, expression ptr, long n, const Dbg& dbg) {
  return cop(cload(MC::Word_int, mut), {field_address(ptr, n, dbg)}, dbg);
}

expression set_field(expression ptr, long n, expression newval, L::InitializationOrAssignment init,
                     const Dbg& dbg) {
  return cop(cstore(MC::Word_val, init), {field_address(ptr, n, dbg), newval}, dbg);
}

expression get_header(expression ptr, const Dbg& dbg) {
  // Headers can be mutated when forcing a lazy value. However, for all
  // purposes that the mutability tag currently serves in the compiler,
  // header loads can be marked as [Immutable], since the runtime should
  // ensure that there is no data race on headers.
  return cop(mk_load_immut(MC::Word_int), {cop(O(OK::Cadda), {ptr, cconst_int(-size_int, dbg)}, dbg)}, dbg);
}

// Config.reserved_header_bits = 0
expression get_header_masked(expression ptr, const Dbg& dbg) { return get_header(ptr, dbg); }

constexpr long tag_offset = big_endian ? -1 : -size_int;
}  // namespace

expression get_tag(expression ptr, const Dbg& dbg) {
  // Same comment as [get_header] above
  return cop(mk_load_immut(MC::Byte_unsigned), {cop(O(OK::Cadda), {ptr, cconst_int(tag_offset, dbg)}, dbg)}, dbg);
}

namespace {
expression get_size(expression ptr, const Dbg& dbg) {
  return cop(O(OK::Clsr), {get_header_masked(ptr, dbg), cconst_int(10, dbg)}, dbg);
}

// Array indexing
const long log2_size_addr = log2(size_addr);
const long log2_size_float = log2(size_float);
constexpr long wordsize_shift = 9;
const long numfloat_shift = 9 + log2_size_float - log2_size_addr;

expression is_addr_array_hdr(expression hdr, const Dbg& dbg) {
  return cop(ccmpi(IC::Cne), {cop(O(OK::Cand), {hdr, cconst_int(255, dbg)}, dbg), floatarray_tag(dbg)}, dbg);
}
expression is_addr_array_ptr(expression ptr, const Dbg& dbg) {
  return cop(ccmpi(IC::Cne), {get_tag(ptr, dbg), floatarray_tag(dbg)}, dbg);
}
expression addr_array_length_shifted(expression hdr, const Dbg& dbg) {
  return cop(O(OK::Clsr), {hdr, cconst_int(wordsize_shift, dbg)}, dbg);
}
expression float_array_length_shifted(expression hdr, const Dbg& dbg) {
  return cop(O(OK::Clsr), {hdr, cconst_int(numfloat_shift, dbg)}, dbg);
}
expression lsl_const(expression c, long n, const Dbg& dbg) {
  if (n == 0) return c;
  return cop(O(OK::Clsl), {c, cconst_int(n, dbg)}, dbg);
}

// Produces a pointer to the element of the array [ptr] on the position
// [ofs] with the given element [log2size] log2 element size. [ofs] is
// given as a tagged int expression.  [add_int_typ]: the result's C-- type
// is Int (bigarray indexing, outside the heap) instead of Addr.
expression array_indexing(long log2size, expression ptr, expression ofs, const Dbg& dbg, bool typ_int = false) {
  Operation add = O(typ_int ? OK::Caddi : OK::Cadda);
  long n;
  if (is_cint(ofs, n)) {
    long i = n >> 1;
    if (i == 0) return ptr;
    return cop(add, {ptr, cconst_int(ilsl(i, log2size), dbg)}, dbg);
  }
  expression a, c;
  long one;
  if (auto* o = as<Cop>(ofs); o && op_c_const(ofs, OK::Caddi, a, one)) {
    long sh;
    if (one == 1 && op_c_const(a, OK::Clsl, c, sh) && sh == 1) return cop(add, {ptr, lsl_const(c, log2size, dbg)}, o->dbg);
    n = one;
    if (log2size == 0)
      return cop(add, {cop(add, {ptr, untag_int(a, dbg)}, dbg), cconst_int(n >> 1, dbg)}, o->dbg);
    return cop(add, {cop(add, {ptr, lsl_const(a, log2size - 1, dbg)}, dbg), cconst_int(ilsl(n - 1, log2size - 1), dbg)},
               dbg);
  }
  if (log2size == 0) return cop(add, {ptr, untag_int(ofs, dbg)}, dbg);
  return cop(add, {cop(add, {ptr, lsl_const(ofs, log2size - 1, dbg)}, dbg), cconst_int(ilsl(-1, log2size - 1), dbg)},
             dbg);
}
}  // namespace

expression field_address_computed(expression ptr, expression ofs, const Dbg& dbg) {
  return array_indexing(log2_size_addr, ptr, ofs, dbg);
}

expression addr_array_ref(expression arr, expression ofs, const Dbg& dbg) {
  return cop(mk_load_mut_op(MC::Word_val), {array_indexing(log2_size_addr, arr, ofs, dbg)}, dbg);
}
namespace {
expression int_array_ref(expression arr, expression ofs, const Dbg& dbg) {
  return cop(mk_load_mut_op(MC::Word_int), {array_indexing(log2_size_addr, arr, ofs, dbg)}, dbg);
}
expression unboxed_float_array_ref(expression arr, expression ofs, const Dbg& dbg) {
  return cop(mk_load_mut_op(MC::Double), {array_indexing(log2_size_float, arr, ofs, dbg)}, dbg);
}
expression float_array_ref(expression arr, expression ofs, const Dbg& dbg) {
  return box_float(dbg, unboxed_float_array_ref(arr, ofs, dbg));
}
expression addr_array_set(expression arr, expression ofs, expression newval, const Dbg& dbg) {
  return cop(cextcall("caml_modify", typ_void(), no_exttypes(), false),
             {array_indexing(log2_size_addr, arr, ofs, dbg), newval}, dbg);
}
expression addr_array_initialize(expression arr, expression ofs, expression newval, const Dbg& dbg) {
  return cop(cextcall("caml_initialize", typ_void(), no_exttypes(), false),
             {array_indexing(log2_size_addr, arr, ofs, dbg), newval}, dbg);
}
expression int_array_set(expression arr, expression ofs, expression newval, const Dbg& dbg) {
  return cop(cstore(MC::Word_int, L::InitializationOrAssignment::Assignment),
             {array_indexing(log2_size_addr, arr, ofs, dbg), newval}, dbg);
}
expression float_array_set(expression arr, expression ofs, expression newval, const Dbg& dbg) {
  return cop(cstore(MC::Double, L::InitializationOrAssignment::Assignment),
             {array_indexing(log2_size_float, arr, ofs, dbg), newval}, dbg);
}
}  // namespace

// Length of string block
expression string_length(expression exp, const Dbg& dbg) {
  return bind("str", exp, [&](expression str) {
    Var tmp_var = Ident::create_local("tmp");
    return clet(vpc(tmp_var),
                cop(O(OK::Csubi),
                    {cop(O(OK::Clsl), {get_size(str, dbg), cconst_int(log2_size_addr, dbg)}, dbg), cconst_int(1, dbg)},
                    dbg),
                cop(O(OK::Csubi),
                    {cvar(tmp_var),
                     cop(mk_load_mut_op(MC::Byte_unsigned), {cop(O(OK::Cadda), {str, cvar(tmp_var)}, dbg)}, dbg)},
                    dbg));
  });
}

namespace {
expression bigstring_length(expression ba, const Dbg& dbg) {
  return cop(mk_load_mut_op(MC::Word_int), {field_address(ba, 5, dbg)}, dbg);
}

// Message sending
expression lookup_tag(expression obj, expression tag, const Dbg& dbg) {
  return bind("tag", tag, [&](expression tag) {
    return cop(cextcall("caml_get_public_method", typ_val(), no_exttypes(), false), {obj, tag}, dbg);
  });
}

expression lookup_label(expression obj, expression lab, const Dbg& dbg) {
  return bind("lab", lab, [&](expression lab) {
    expression table = cop(mk_load_mut_op(MC::Word_val), {obj}, dbg);
    return addr_array_ref(table, lab, dbg);
  });
}

expression call_cached_method(expression obj, expression tag, expression cache, expression pos,
                              const std::vector<expression>& args, const Dbg& dbg) {
  long arity = static_cast<long>(args.size());
  expression c = array_indexing(log2_size_addr, cache, pos, dbg);
  compilenv::need_send_fun(arity);
  std::vector<expression> a{cconst_symbol(zstr("caml_send" + std::to_string(arity)), dbg), obj, tag, c};
  a.insert(a.end(), args.begin(), args.end());
  return cop(capply(typ_val()), slice(a), dbg);
}

// Allocation
using SetFn = expression (*)(expression, expression, expression, const Dbg&);
expression make_alloc_generic(SetFn set_fn, const Dbg& dbg, long tag, long wordsize,
                              const std::vector<expression>& args) {
  if (wordsize <= max_young_wosize) {
    std::vector<expression> a{cconst_natint(block_header(tag, wordsize), dbg)};
    a.insert(a.end(), args.begin(), args.end());
    return cop(O(OK::Calloc), slice(a), dbg);
  }
  Var id = Ident::create_local("*alloc*");
  // fill_fields idx = function [] -> Cvar id | e1::el -> Csequence(set_fn .., fill_fields (idx + 2) el)
  expression fill = cvar(id);
  for (std::size_t k = args.size(); k-- > 0;)
    fill = csequence(set_fn(cvar(id), cconst_int(1 + 2 * static_cast<long>(k), dbg), args[k], dbg), fill);
  return clet(vpc(id),
              cop(cextcall("caml_alloc_shr_check_gc", typ_val(), no_exttypes(), true),
                  {cconst_int(wordsize, dbg), cconst_int(tag, dbg)}, dbg),
              fill);
}

expression addr_array_init(expression arr, expression ofs, expression newval, const Dbg& dbg) {
  return cop(cextcall("caml_initialize", typ_void(), no_exttypes(), false),
             {array_indexing(log2_size_addr, arr, ofs, dbg), newval}, dbg);
}
}  // namespace

expression make_alloc(const Dbg& dbg, long tag, const std::vector<expression>& args) {
  return make_alloc_generic(addr_array_init, dbg, tag, static_cast<long>(args.size()), args);
}

expression make_float_alloc(const Dbg& dbg, long tag, const std::vector<expression>& args) {
  return make_alloc_generic(float_array_set, dbg, tag, static_cast<long>(args.size()) * size_float / size_addr,
                            args);
}

// Bounds checking
expression make_checkbound(const Dbg& dbg, const std::vector<expression>& args) {
  if (args.size() == 2) {
    expression a1;
    long n, m;
    if (op_c_const(args[0], OK::Clsr, a1, n) && is_cint(args[1], m) && ilsl(m, n) > n)
      return cop(O(OK::Ccheckbound), {a1, cconst_int(isub(iadd(ilsl(m, n), ilsl(1, n)), 1), dbg)}, dbg);
  }
  return cop(O(OK::Ccheckbound), slice(args), dbg);
}

// Record application and currying functions
namespace {
std::string_view apply_function_sym(long n) {
  compilenv::need_apply_fun(n);
  return zstr("caml_apply" + std::to_string(n));
}
}  // namespace
std::string_view curry_function_sym(long n) {
  compilenv::need_curry_fun(n);
  if (n >= 0) return zstr("caml_curry" + std::to_string(n));
  return zstr("caml_tuplify" + std::to_string(-n));
}

// ---- Big arrays ------------------------------------------------------------------------------
namespace {
using BK = L::BigarrayKind;
long bigarray_elt_size(BK k) {
  switch (k) {
    case BK::Pbigarray_unknown: fatal("Cmm_helpers.bigarray_elt_size");
    case BK::Pbigarray_float16: return 2;
    case BK::Pbigarray_float32: return 4;
    case BK::Pbigarray_float64: return 8;
    case BK::Pbigarray_sint8: return 1;
    case BK::Pbigarray_uint8: return 1;
    case BK::Pbigarray_sint16: return 2;
    case BK::Pbigarray_uint16: return 2;
    case BK::Pbigarray_int32: return 4;
    case BK::Pbigarray_int64: return 8;
    case BK::Pbigarray_caml_int: return size_int;
    case BK::Pbigarray_native_int: return size_int;
    case BK::Pbigarray_complex32: return 8;
    case BK::Pbigarray_complex64: return 16;
  }
  return 0;
}

// Produces a pointer to the element of the bigarray [b] on the position
// [args].  [args] is given as a list of tagged int expressions, one per
// array dimension.
expression bigarray_indexing(bool unsafe, BK elt_kind, L::BigarrayLayout layout, expression b,
                             const std::vector<expression>& args, const Dbg& dbg) {
  auto check_ba_bound = [&](expression bound, expression idx, expression v) {
    return csequence(make_checkbound(dbg, {bound, idx}), v);
  };
  // Validates the given multidimensional offset against the array bounds
  // and transforms it into a one dimensional offset.  The offsets are
  // expressions evaluating to tagged int.
  std::function<expression(long, long, std::size_t, const std::vector<expression>&)> ba_indexing =
      [&](long dim_ofs, long delta_ofs, std::size_t k, const std::vector<expression>& l) -> expression {
    if (k >= l.size()) fatal("Cmm_helpers.bigarray_indexing");
    if (k + 1 == l.size()) {
      expression arg = l[k];
      if (unsafe) return arg;
      return bind("idx", arg, [&](expression idx) {
        // Load the untagged int bound for the given dimension
        expression bound = cop(mk_load_mut_op(MC::Word_int), {field_address(b, dim_ofs, dbg)}, dbg);
        expression idxn = untag_int(idx, dbg);
        return check_ba_bound(bound, idxn, idx);
      });
    }
    expression arg1 = l[k];
    // The remainder of the list is transformed into a one dimensional offset
    expression rem = ba_indexing(dim_ofs + delta_ofs, delta_ofs, k + 1, l);
    // Load the untagged int bound for the given dimension
    expression bound = cop(mk_load_mut_op(MC::Word_int), {field_address(b, dim_ofs, dbg)}, dbg);
    if (unsafe) return add_int(mul_int(decr_int(rem, dbg), bound, dbg), arg1, dbg);
    return bind("idx", arg1, [&](expression idx) {
      return bind("bound", bound, [&](expression bound) {
        expression idxn = untag_int(idx, dbg);
        // [offset = rem * (tag_int bound) + idx]
        expression offset = add_int(mul_int(decr_int(rem, dbg), bound, dbg), idx, dbg);
        return check_ba_bound(bound, idxn, offset);
      });
    });
  };
  // The offset as an expression evaluating to int
  expression offset;
  switch (layout) {
    case L::BigarrayLayout::Pbigarray_c_layout: {
      std::vector<expression> rev(args.rbegin(), args.rend());
      offset = ba_indexing(4 + static_cast<long>(args.size()), -1, 0, rev);
      break;
    }
    case L::BigarrayLayout::Pbigarray_fortran_layout: {
      std::vector<expression> l;
      for (expression idx : args) l.push_back(sub_int(idx, cconst_int(2, dbg), dbg));
      offset = ba_indexing(5, 1, 0, l);
      break;
    }
    default: fatal("Cmm_helpers.bigarray_indexing: layout");
  }
  long elt_size = bigarray_elt_size(elt_kind);
  // [array_indexing] can simplify the given expressions
  return array_indexing(log2(elt_size), cop(mk_load_mut_op(MC::Word_int), {field_address(b, 1, dbg)}, dbg), offset,
                        dbg);
}

MC bigarray_word_kind(BK k) {
  switch (k) {
    case BK::Pbigarray_unknown: fatal("Cmm_helpers.bigarray_word_kind");
    case BK::Pbigarray_float16: return MC::Sixteen_unsigned;
    case BK::Pbigarray_float32: return MC::Single;
    case BK::Pbigarray_float64: return MC::Double;
    case BK::Pbigarray_sint8: return MC::Byte_signed;
    case BK::Pbigarray_uint8: return MC::Byte_unsigned;
    case BK::Pbigarray_sint16: return MC::Sixteen_signed;
    case BK::Pbigarray_uint16: return MC::Sixteen_unsigned;
    case BK::Pbigarray_int32: return MC::Thirtytwo_signed;
    case BK::Pbigarray_int64: return MC::Sixtyfour;
    case BK::Pbigarray_caml_int: return MC::Sixtyfour;
    case BK::Pbigarray_native_int: return MC::Sixtyfour;
    case BK::Pbigarray_complex32: return MC::Single;
    case BK::Pbigarray_complex64: return MC::Double;
  }
  return MC::Word_int;
}
}  // namespace

expression bigarray_get(bool unsafe, BK elt_kind, L::BigarrayLayout layout, expression b,
                        const std::vector<expression>& args, const Dbg& dbg) {
  return bind("ba", b, [&](expression b) -> expression {
    if (elt_kind == BK::Pbigarray_complex32 || elt_kind == BK::Pbigarray_complex64) {
      MC kind = bigarray_word_kind(elt_kind);
      long sz = bigarray_elt_size(elt_kind) / 2;
      return bind("addr", bigarray_indexing(unsafe, elt_kind, layout, b, args, dbg), [&](expression addr) {
        return bind("reval", cop(mk_load_mut_op(kind), {addr}, dbg), [&](expression reval) {
          return bind("imval",
                      cop(mk_load_mut_op(kind), {cop(O(OK::Cadda), {addr, cconst_int(sz, dbg)}, dbg)}, dbg),
                      [&](expression imval) { return box_complex(dbg, reval, imval); });
        });
      });
    }
    return cop(mk_load_mut_op(bigarray_word_kind(elt_kind)), {bigarray_indexing(unsafe, elt_kind, layout, b, args, dbg)},
               dbg);
  });
}

expression bigarray_set(bool unsafe, BK elt_kind, L::BigarrayLayout layout, expression b,
                        const std::vector<expression>& args, expression newval, const Dbg& dbg) {
  return bind("ba", b, [&](expression b) -> expression {
    if (elt_kind == BK::Pbigarray_complex32 || elt_kind == BK::Pbigarray_complex64) {
      MC kind = bigarray_word_kind(elt_kind);
      long sz = bigarray_elt_size(elt_kind) / 2;
      return bind("newval", newval, [&](expression newv) {
        return bind("addr", bigarray_indexing(unsafe, elt_kind, layout, b, args, dbg), [&](expression addr) {
          return csequence(
              cop(cstore(kind, L::InitializationOrAssignment::Assignment), {addr, complex_re(newv, dbg)}, dbg),
              cop(cstore(kind, L::InitializationOrAssignment::Assignment),
                  {cop(O(OK::Cadda), {addr, cconst_int(sz, dbg)}, dbg), complex_im(newv, dbg)}, dbg));
        });
      });
    }
    return cop(cstore(bigarray_word_kind(elt_kind), L::InitializationOrAssignment::Assignment),
               {bigarray_indexing(unsafe, elt_kind, layout, b, args, dbg), newval}, dbg);
  });
}

// low_32 x is a value which agrees with x on at least the low 32 bits
expression low_32(const Dbg& dbg, expression x) {
  expression a, c;
  long n;
  // Ignore sign and zero extensions, which do not affect the low bits
  if (op_c_const(x, OK::Casr, a, n) && n == 32 && op_c_const(a, OK::Clsl, c, n) && n == 32) return low_32(dbg, c);
  if (auto* o = as<Cop>(x); o && o->op.kind == OK::Cand && o->args.size() == 2)
    if (auto* m = as<Cconst_natint>(o->args[1]); m && m->n == 0xFFFFFFFFLL) return low_32(dbg, o->args[0]);
  if (auto* l = as<Clet>(x)) return clet(l->id, l->def, low_32(dbg, l->body));
  return x;
}

namespace {
// sign_extend_32 sign-extends values from 32 bits to the word size.
expression sign_extend_32(const Dbg& dbg, expression e) {
  return cop(O(OK::Casr), {cop(O(OK::Clsl), {low_32(dbg, e), cconst_int(32, dbg)}, dbg), cconst_int(32, dbg)}, dbg);
}
// zero_extend_32 zero-extends values from 32 bits to the word size.
expression zero_extend_32(const Dbg& dbg, expression e) {
  return cop(O(OK::Cand), {low_32(dbg, e), natint_const_untagged(dbg, 0xFFFFFFFFLL)}, dbg);
}

// Boxed integers
const char* operations_boxed_int(BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return caml_nativeint_ops;
    case BoxedInteger::Pint32: return caml_int32_ops;
    default: return caml_int64_ops;
  }
}
std::int64_t header_boxed_int(BoxedInteger bi) {
  switch (bi) {
    case BoxedInteger::Pnativeint: return boxedintnat_header;
    case BoxedInteger::Pint32: return boxedint32_header;
    default: return boxedint64_header;
  }
}

bool alloc_matches_boxed_int(BoxedInteger bi, expression hdr, expression ops) {
  auto* h = as<Cconst_natint>(hdr);
  auto* s = as<Cconst_symbol>(ops);
  if (!h || !s) return false;
  return h->n == header_boxed_int(bi) && s->s == operations_boxed_int(bi);
}
}  // namespace

expression box_int_gen(const Dbg& dbg, BoxedInteger bi, expression arg) {
  expression arg2 = bi == BoxedInteger::Pint32 ? sign_extend_32(dbg, arg) : arg;
  return cop(O(OK::Calloc),
             {cconst_natint(header_boxed_int(bi), dbg), cconst_symbol(operations_boxed_int(bi), dbg), arg2}, dbg);
}

expression unbox_int(const Dbg& dbg, BoxedInteger bi, expression e) {
  auto default_ = [&](expression arg) {
    MC memory_chunk = bi == BoxedInteger::Pint32 ? MC::Thirtytwo_signed : MC::Word_int;
    return cop(mk_load_immut(memory_chunk), {cop(O(OK::Cadda), {arg, cconst_int(size_addr, dbg)}, dbg)}, dbg);
  };
  return map_tail(
      [&](expression cmm) -> expression {
        if (auto* a = as<Cop>(cmm); a && a->op.kind == OK::Calloc && a->args.size() == 3 &&
                                    alloc_matches_boxed_int(bi, a->args[0], a->args[1])) {
          // Force sign-extension of low 32 bits
          if (bi == BoxedInteger::Pint32) return sign_extend_32(dbg, a->args[2]);
          return a->args[2];
        }
        if (auto* s = as<Cconst_symbol>(cmm)) {
          using SCK = clambda::UStructuredConstant::Kind;
          const clambda::UStructuredConstant* c = cmmgen_state::structured_constant_of_sym(s->s);
          if (c && c->kind == SCK::Uconst_nativeint && bi == BoxedInteger::Pnativeint)
            return natint_const_untagged(dbg, c->i);
          if (c && c->kind == SCK::Uconst_int32 && bi == BoxedInteger::Pint32) return natint_const_untagged(dbg, c->i);
          if (c && c->kind == SCK::Uconst_int64 && bi == BoxedInteger::Pint64) return natint_const_untagged(dbg, c->i);
          return default_(cmm);
        }
        return default_(cmm);
      },
      e);
}

expression make_unsigned_int(BoxedInteger bi, expression arg, const Dbg& dbg) {
  if (bi == BoxedInteger::Pint32) return zero_extend_32(dbg, arg);
  return arg;
}

namespace {
// Arch.allow_unaligned_access
expression unaligned_load(MemoryAccessSize size, expression ptr, expression idx, const Dbg& dbg) {
  MC c = size == MemoryAccessSize::Sixteen      ? MC::Sixteen_unsigned
         : size == MemoryAccessSize::Thirty_two ? MC::Thirtytwo_unsigned
                                                : MC::Sixtyfour;
  return cop(mk_load_mut_op(c), {offset_addr(ptr, idx, dbg)}, dbg);
}
expression unaligned_set(MemoryAccessSize size, expression ptr, expression idx, expression newval, const Dbg& dbg) {
  MC c = size == MemoryAccessSize::Sixteen      ? MC::Sixteen_unsigned
         : size == MemoryAccessSize::Thirty_two ? MC::Thirtytwo_unsigned
                                                : MC::Sixtyfour;
  return cop(cstore(c, L::InitializationOrAssignment::Assignment), {offset_addr(ptr, idx, dbg), newval}, dbg);
}

expression max_or_zero(expression a, const Dbg& dbg) {
  return bind("size", a, [&](expression a) {
    // equivalent to
    //   Cifthenelse(Cop(Ccmpi Cle, [a; cconst_int 0]), cconst_int 0, a)
    //
    // if a is positive, sign is 0 hence sign_negation is full of 1 so
    // sign_negation&a = a; if a is negative, sign is full of 1 hence
    // sign_negation is 0 so sign_negation&a = 0
    expression sign = cop(O(OK::Casr), {a, cconst_int(size_int * 8 - 1, dbg)}, dbg);
    expression sign_negation = cop(O(OK::Cxor), {sign, cconst_int(-1, dbg)}, dbg);
    return cop(O(OK::Cand), {sign_negation, a}, dbg);
  });
}

expression check_bound(L::IsSafe safety, MemoryAccessSize access_size, const Dbg& dbg, expression length,
                       expression a2, expression k) {
  if (safety == L::IsSafe::Unsafe) return k;
  long offset = access_size == MemoryAccessSize::Sixteen ? 1 : access_size == MemoryAccessSize::Thirty_two ? 3 : 7;
  expression a1 = sub_int(length, cconst_int(offset, dbg), dbg);
  return csequence(make_checkbound(dbg, {max_or_zero(a1, dbg), a2}), k);
}

expression box_sized(MemoryAccessSize size, const Dbg& dbg, expression exp) {
  switch (size) {
    case MemoryAccessSize::Sixteen: return tag_int(exp, dbg);
    case MemoryAccessSize::Thirty_two: return box_int_gen(dbg, BoxedInteger::Pint32, exp);
    default: return box_int_gen(dbg, BoxedInteger::Pint64, exp);
  }
}
}  // namespace

expression opaque(expression e, const Dbg& dbg) { return cop(O(OK::Copaque), {e}, dbg); }

// Simplification of some primitives into C calls
const PrimitiveDescription* primitive_simple(std::string_view name, long arity, bool alloc);
namespace {
const PrimitiveDescription* default_prim(const std::string& name) {
  return primitive_simple(zstr(name), 0 /*ignored*/, true);
}
}  // namespace

// Primitive.simple ~name ~arity ~alloc
const PrimitiveDescription* primitive_simple(std::string_view name, long arity, bool alloc) {
  return make<PrimitiveDescription>(name, arity, alloc, empty_native_name(),
                                    slice(std::vector<NativeRepr>(static_cast<std::size_t>(arity), NativeRepr{})),
                                    NativeRepr{});
}
clambda::Primitive simplif_primitive(const clambda::Primitive& p) {
  using PK = clambda::Primitive::K;
  auto ccall = [](const std::string& name) {
    clambda::Primitive r{PK::Pccall};
    r.ccall = default_prim(name);
    return r;
  };
  if (p.kind == PK::Pduprecord) return ccall("caml_obj_dup");
  if (p.kind == PK::Pbigarrayref &&
      (p.ba_kind == BK::Pbigarray_unknown || p.ba_layout == L::BigarrayLayout::Pbigarray_unknown_layout))
    return ccall("caml_ba_get_" + std::to_string(p.n));
  if (p.kind == PK::Pbigarrayset &&
      (p.ba_kind == BK::Pbigarray_unknown || p.ba_layout == L::BigarrayLayout::Pbigarray_unknown_layout))
    return ccall("caml_ba_set_" + std::to_string(p.n));
  return p;
}

// Build switchers both for constants and blocks
expression transl_isout(expression h, expression arg, const Dbg& dbg) {
  return tag_int(cop(ccmpa(IC::Clt), {h, arg}, dbg), dbg);
}

// Operations on OCaml values
expression add_int_caml(expression a1, expression a2, const Dbg& dbg) { return decr_int(add_int(a1, a2, dbg), dbg); }

// Unary primitive delayed to reuse add_int_caml
expression offsetint(long n, expression arg, const Dbg& dbg) {
  if (no_overflow_lsl(n, 1)) return add_const(arg, n << 1, dbg);
  return add_int_caml(arg, int_const(dbg, n), dbg);
}

expression sub_int_caml(expression a1, expression a2, const Dbg& dbg) { return incr_int(sub_int(a1, a2, dbg), dbg); }

expression mul_int_caml(expression a1, expression a2, const Dbg& dbg) {
  // decrementing the non-constant part helps when the multiplication is
  // followed by an addition; for example, using this trick compiles
  // (100 * a + 7) into (+ ( * a 100) -85) rather than
  // (+ ( * 200 (>>s a 1)) 15)
  if (a1->kind == EK::Cconst_int) return incr_int(mul_int(untag_int(a1, dbg), decr_int(a2, dbg), dbg), dbg);
  return incr_int(mul_int(decr_int(a1, dbg), untag_int(a2, dbg), dbg), dbg);
}

expression div_int_caml(L::IsSafe is_safe, expression a1, expression a2, const Dbg& dbg) {
  return tag_int(div_int(untag_int(a1, dbg), untag_int(a2, dbg), is_safe, dbg), dbg);
}
expression mod_int_caml(L::IsSafe is_safe, expression a1, expression a2, const Dbg& dbg) {
  return tag_int(mod_int(untag_int(a1, dbg), untag_int(a2, dbg), is_safe, dbg), dbg);
}
expression and_int_caml(expression a1, expression a2, const Dbg& dbg) { return cop(O(OK::Cand), {a1, a2}, dbg); }
expression or_int_caml(expression a1, expression a2, const Dbg& dbg) { return cop(O(OK::Cor), {a1, a2}, dbg); }
expression xor_int_caml(expression a1, expression a2, const Dbg& dbg) {
  return cop(O(OK::Cor),
             {cop(O(OK::Cxor), {ignore_low_bit_int(a1), ignore_low_bit_int(a2)}, dbg), cconst_int(1, dbg)}, dbg);
}
expression lsl_int_caml(expression a1, expression a2, const Dbg& dbg) {
  return incr_int(lsl_int(decr_int(a1, dbg), untag_int(a2, dbg), dbg), dbg);
}
expression lsr_int_caml(expression a1, expression a2, const Dbg& dbg) {
  return cop(O(OK::Cor), {lsr_int(a1, untag_int(a2, dbg), dbg), cconst_int(1, dbg)}, dbg);
}
expression asr_int_caml(expression a1, expression a2, const Dbg& dbg) {
  return cop(O(OK::Cor), {asr_int(a1, untag_int(a2, dbg), dbg), cconst_int(1, dbg)}, dbg);
}
expression int_comp_caml(IC cmp, expression a1, expression a2, const Dbg& dbg) {
  return tag_int(cop(ccmpi(cmp), {a1, a2}, dbg), dbg);
}

// ---- Build an actual switch (ie jump table) --------------------------------------------------
// This function takes a switch on immediate values, for example:
//   int 0: 1
//   int 1: 3
//   int 2: 5
//
// It tries to perform two optimizations:
// - If the switch implements an affine function [x -> a*x + b], produce the
//   affine expression [a * arg + b]. In particular, when a=1 and b=0,
//   return the argument [arg] unchanged.
// - If the switch only has constant right-hand-sides (but is not an affine
//   function), produce a table lookup.
expression make_switch(bool tagged, expression arg, Slice<long> cases, const std::vector<SwitchCase>& actions,
                       const Dbg& dbg) {
  // We only apply those optimizations if the right-hand-side is made of
  // valid OCaml constants. In particular, if all machine integers
  // appearing in the right-hand-side are tagged (least bit 1).
  auto extract_uconstant = [](const SwitchCase& c) -> std::optional<DataItem> {
    if (auto* k = as<Cconst_int>(c.e); k && (k->n & 1) == 1) return data_int(DataItem::K::Cint, k->n);
    if (auto* k = as<Cconst_natint>(c.e); k && (k->n & 1) == 1) return data_int(DataItem::K::Cint, k->n);
    if (auto* k = as<Cconst_symbol>(c.e)) return data_sym(DataItem::K::Csymbol_address, k->s);
    return std::nullopt;
  };
  expression arg_tagged, arg_untagged;
  if (tagged) {
    arg_tagged = arg;
    arg_untagged = untag_int(arg, dbg);
  } else {
    arg_tagged = tag_int(arg, dbg);
    arg_untagged = arg;
  }
  std::vector<DataItem> const_actions;
  for (const SwitchCase& a : actions) {
    std::optional<DataItem> d = extract_uconstant(a);
    if (!d) return cswitch(arg_untagged, cases, slice(actions), dbg);  // We need an untagged argument here.
    const_actions.push_back(*d);
  }
  // extract_affine
  long length = static_cast<long>(cases.size());
  if (length >= 2) {
    const DataItem& a0 = const_actions[cases[0]];
    const DataItem& a1 = const_actions[cases[1]];
    if (a0.kind == DataItem::K::Cint && a1.kind == DataItem::K::Cint) {
      // The right-hand-sides are tagged, so we can translate them back to
      // OCaml integers without loss of information, to compute the offset
      // and slope on OCaml integers.
      long v0 = untag_const(a0.n), v1 = untag_const(a1.n);
      long slope = isub(v1, v0);
      bool all = true;
      for (long i = 0; i < length && all; ++i) {
        const DataItem& d = const_actions[cases[i]];
        all = d.kind == DataItem::K::Cint && untag_const(d.n) == iadd(imul(slope, i), v0);
      }
      if (all) {
        // Asking for a tagged argument here does not introduce extra
        // tagging, as any (tag_int ..) logic around the argument will be
        // undone by [mul_int_caml].
        return add_int_caml(mul_int_caml(int_const(dbg, slope), arg_tagged, dbg), int_const(dbg, v0), dbg);
      }
    }
  }
  // make_table_lookup: we need a tagged argument here, to call a
  // [*_array_ref] helper.
  std::string_view table = compilenv::new_const_symbol();
  std::vector<DataItem> elems;
  for (long c : cases) elems.push_back(const_actions[c]);
  cmmgen_state::add_constant(table, cmmgen_state::Constant{false, cmmgen_state::IsGlobal::Local, {}, {}, elems});
  // Constant integers loaded from a table are tagged, so that Cload never
  // produces untagged integers.
  return addr_array_ref(cconst_symbol(table, dbg), arg_tagged, dbg);
}

namespace {
struct SArgBlocks {
  using primitive = Operation;
  using loc = Dbg;
  using arg = expression;
  using test = expression;
  using act = expression;

  static primitive eqint() { return ccmpi(IC::Ceq); }
  static primitive neint() { return ccmpi(IC::Cne); }
  static primitive leint() { return ccmpi(IC::Cle); }
  static primitive ltint() { return ccmpi(IC::Clt); }
  static primitive geint() { return ccmpi(IC::Cge); }
  static primitive gtint() { return ccmpi(IC::Cgt); }

  // CR mshinwell: GPR#2294 will fix the Debuginfo here
  static arg make_const(long i) { return cconst_int(i, debuginfo::none()); }
  static test make_prim(const primitive& p, std::vector<arg> args) {
    return cop(p, slice(args), debuginfo::none());
  }
  static arg make_offset(arg a, long n) { return add_const(a, n, debuginfo::none()); }
  static test make_isout(arg h, arg a) { return cop(ccmpa(IC::Clt), {h, a}, debuginfo::none()); }
  static test make_isin(arg h, arg a) { return cop(ccmpa(IC::Cge), {h, a}, debuginfo::none()); }
  static test make_is_nonzero(arg a) { return a; }
  static test arg_as_test(arg a) { return a; }
  static act make_if(test cond, act ifso, act ifnot) {
    return cifthenelse(cond, debuginfo::none(), ifso, debuginfo::none(), ifnot, debuginfo::none());
  }
  static act make_switch(const loc& dbg, arg a, const std::vector<long>& cases, std::vector<act>& actions) {
    std::vector<SwitchCase> acts;
    for (act e : actions) acts.push_back({e, dbg});
    return cmm_helpers::make_switch(false, a, slice(cases), acts, dbg);
  }
  static act bind(arg a, const std::function<act(arg)>& body) { return cmm_helpers::bind("switcher", a, body); }
  static std::pair<long, std::function<act(act)>> make_catch(act handler) {
    if (auto* e = as<Cexit>(handler); e && e->args.empty()) return {e->n, [](act x) { return x; }};
    Dbg dbg = debuginfo::none();
    long i = L::next_raise_count();
    return {i, [i, handler, dbg](act body) -> act {
              if (auto* e = as<Cexit>(body)) {
                if (i == e->n) return handler;
                return body;
              }
              return ccatch(i, {}, body, handler, dbg);
            }};
  }
  static act make_exit(long i) { return cexit(i, {}); }
};

// Switch.CtxStore with its AMap an AVL map on [compare_key], as OCaml's:
// the Cmm keys' comparison is not an order (two exits to the same handler
// are equal whatever their indexes), so which key a lookup meets depends on
// the tree.
template <class S>
class AvlStore {
 public:
  using A = typename S::t;
  using Key = typename S::key;
  long act_store(const typename S::context& ctx, const A& act) {
    std::optional<Key> key = S::make_key(ctx, act);
    if (!key) return add(false, act);
    if (const std::pair<bool, long>* e = map_.find_opt(*key)) {
      long i = e->second;
      if (!e->first) map_ = map_.add(*key, {true, i});
      return i;
    }
    long i = add(false, act);
    map_ = map_.add(*key, {false, i});
    return i;
  }
  std::vector<switch_::Shared<A>> act_get_shared() const {
    std::vector<switch_::Shared<A>> acts;
    for (auto& [shared, act] : acts_) acts.push_back({shared, act});
    map_.iter([&](const Key&, const std::pair<bool, long>& e) {
      if (e.first) acts[static_cast<std::size_t>(e.second)].shared = true;
    });
    return acts;
  }

 private:
  struct Cmp {
    int operator()(const Key& a, const Key& b) const { return S::compare_key(a, b); }
  };
  PMap<Key, std::pair<bool, long>, Cmp> map_;
  std::vector<std::pair<bool, A>> acts_;  // in store order
  long add(bool mustshare, const A& act) {
    acts_.push_back({mustshare, act});
    return static_cast<long>(acts_.size()) - 1;
  }
};

// cmm store, as sharing as normally been detected in previous phases, we
// only share exits.  Some specific patterns can lead to switches where
// several cases point to the same action, but this action is not an exit
// (see GPR#1370).  The addition of the index in the action array as
// context allows to share them correctly without duplication.
struct StoreExpForSwitchArg {
  using t = expression;
  using key = std::pair<std::optional<long>, long>;
  using context = long;
  static std::optional<key> make_key(const context& index, const t& expr) {
    std::optional<long> continuation;
    if (auto* e = as<Cexit>(expr); e && e->args.empty()) continuation = e->n;
    return key{continuation, index};
  }
  static int compare_key(const key& a, const key& b) {
    if (a.first && b.first && *a.first == *b.first) return 0;
    return a.second < b.second ? -1 : a.second > b.second ? 1 : 0;
  }
};
using StoreExpForSwitch = AvlStore<StoreExpForSwitchArg>;

// For string switches, we can use a generic store
struct StoreExpArg {
  using t = expression;
  using key = long;
  using context = std::monostate;
  static std::optional<key> make_key(const context&, const t& expr) {
    if (auto* e = as<Cexit>(expr); e && e->args.empty()) return e->n;
    return std::nullopt;
  }
  static int compare_key(const key& a, const key& b) { return a < b ? -1 : a > b ? 1 : 0; }
};
struct StoreExp : AvlStore<StoreExpArg> {
  long act_store(const expression& act) { return AvlStore<StoreExpArg>::act_store(std::monostate{}, act); }
};

using SwitcherBlocks = switch_::Make<SArgBlocks>;
using SCase = SwitcherBlocks::Case;

// Int switcher, arg in [low..high], cases is list of individual cases, and
// is sorted by first component
expression transl_int_switch(const Dbg& dbg, expression arg, long low, long high,
                             const std::vector<std::pair<long, expression>>& cases0, expression default_) {
  if (cases0.empty()) fatal("Cmm_helpers.transl_int_switch");
  StoreExp store;
  if (store.act_store(default_) != 0) fatal("Cmm_helpers.transl_int_switch");
  std::vector<std::pair<long, long>> cases;
  for (auto& [i, act] : cases0) cases.push_back({i, store.act_store(act)});
  std::vector<SCase> inters;
  // inters plow phigh pact rem (iterative: each call conses in front of the
  // rest, so the results come out in order)
  auto run = [&](long plow, long phigh, long pact, std::size_t k) {
    for (;;) {
      if (k == cases.size()) {
        if (phigh == high) inters.push_back({plow, phigh, pact});
        else {
          inters.push_back({plow, phigh, pact});
          inters.push_back({phigh + 1, high, 0});
        }
        return;
      }
      auto [i, act] = cases[k];
      ++k;
      if (i == phigh + 1) {
        if (pact == act) {
          phigh = i;
          continue;
        }
        inters.push_back({plow, phigh, pact});
        plow = i;
        phigh = i;
        pact = act;
        continue;
      }
      // insert default
      if (pact == 0) {
        if (act == 0) {
          phigh = i;
          continue;
        }
        inters.push_back({plow, i - 1, pact});
        plow = i;
        phigh = i;
        pact = act;
        continue;
      }
      // pact <> 0
      inters.push_back({plow, phigh, pact});
      if (act == 0) {
        plow = phigh + 1;
        phigh = i;
        pact = 0;
        continue;
      }
      inters.push_back({phigh + 1, i - 1, 0});
      plow = i;
      phigh = i;
      pact = act;
    }
  };
  auto [k0, act0] = cases[0];
  if (k0 == low) run(k0, k0, act0, 1);
  else run(low, k0 - 1, 0, 0);
  return bind("switcher", arg, [&](expression a) { return SwitcherBlocks::zyva(dbg, {low, high}, a, inters, store); });
}
}  // namespace

expression transl_switch_clambda(const Dbg& loc, expression arg, Slice<long> index0,
                                 const std::vector<expression>& cases) {
  StoreExpForSwitch store;
  std::vector<long> index;
  for (long j : index0) index.push_back(store.act_store(j, cases[static_cast<std::size_t>(j)]));
  long n_index = static_cast<long>(index.size());
  std::vector<SCase> inters;  // built in front
  long this_high = n_index - 1, this_low = n_index - 1, this_act = index[n_index - 1];
  for (long i = n_index - 2; i >= 0; --i) {
    long act = index[i];
    if (act == this_act) --this_low;
    else {
      inters.insert(inters.begin(), {this_low, this_high, this_act});
      this_high = i;
      this_low = i;
      this_act = act;
    }
  }
  inters.insert(inters.begin(), {0, this_high, this_act});
  if (inters.size() == 1) return cases[0];
  return bind("switcher", arg,
              [&](expression a) { return SwitcherBlocks::zyva(loc, {0, n_index - 1}, a, inters, store); });
}

// ---- Strmatch (Make with string_block_length = get_size, transl_switch = transl_int_switch) --
namespace strmatch {

using Pat = std::vector<std::int64_t>;
struct Case {
  Pat ps;
  expression act;
};
using Cases = std::vector<Case>;

Var gen_cell_id() { return Ident::create_local("cell"); }
Var gen_size_id() { return Ident::create_local("size"); }

expression mk_let_cell(Var id, expression str, long ind, expression body) {
  Dbg dbg = debuginfo::none();
  expression cell = cop(cload(MC::Word_int, MutableFlag::Mutable),
                        {cop(O(OK::Cadda), {str, cconst_int(size_int * ind, dbg)}, dbg)}, dbg);
  return clet(vpc(id), cell, body);
}

expression mk_let_size(Var id, expression str, expression body) {
  expression size = get_size(str, debuginfo::none());
  return clet(vpc(id), size, body);
}

expression mk_cmp_gen(IC cmp_op, Var id, std::int64_t nat, expression ifso, expression ifnot) {
  Dbg dbg = debuginfo::none();
  expression test = cop(ccmpi(cmp_op), {cvar(id), cconst_natint(nat, dbg)}, dbg);
  return cifthenelse(test, dbg, ifso, dbg, ifnot, dbg);
}

// Compile strings to a lists of words [native ints]
Pat pat_of_string(std::string_view str) {
  long len = static_cast<long>(str.size());
  long n = len / size_addr + 1;
  auto get_byte = [&](long i) -> long {
    if (i < len) return static_cast<unsigned char>(str[i]);
    if (i < n * size_addr - 1) return 0;
    return n * size_addr - 1 - len;
  };
  Pat r;
  for (long ind = 0; ind < n; ++ind) {
    std::uint64_t w = 0;
    long imin = ind * size_addr, imax = (ind + 1) * size_addr - 1;
    for (long i = imax; i >= imin; --i) w = (w << 8) | static_cast<std::uint64_t>(get_byte(i));
    r.push_back(static_cast<std::int64_t>(w));
  }
  return r;
}

// Discriminating heuristics
std::vector<long> count_arities(const Cases& cases) {
  std::vector<std::set<std::int64_t>> sets(cases.front().ps.size());
  for (const Case& c : cases)
    for (std::size_t k = 0; k < c.ps.size(); ++k) sets[k].insert(c.ps[k]);
  std::vector<long> r;
  for (auto& s : sets) r.push_back(static_cast<long>(s.size()));
  return r;
}
long count_arities_first(const Cases& cases) {
  std::set<std::int64_t> s;
  for (const Case& c : cases) s.insert(c.ps.front());
  return static_cast<long>(s.size());
}
long count_arities_length(const Cases& cases) {
  std::set<long> s;
  for (const Case& c : cases) s.insert(static_cast<long>(c.ps.size()));
  return static_cast<long>(s.size());
}
long best_col(const Cases& cases) {
  std::vector<long> ars = count_arities(cases);
  long kbest = -1, best = max_int;
  for (long k = 0; k < static_cast<long>(ars.size()); ++k)
    if (ars[k] < best) {
      kbest = k;
      best = ars[k];
    }
  return kbest;
}
template <class T>
std::vector<T> swap_list(long k, const std::vector<T>& xs) {
  std::vector<T> r{xs[k]};
  for (long i = 0; i < static_cast<long>(xs.size()); ++i)
    if (i != k) r.push_back(xs[i]);
  return r;
}
void best_first(std::vector<long>& idxs, Cases& cases) {
  if (idxs.size() <= 1) return;  // optimisation: one column only
  long k = best_col(cases);
  if (k == 0) return;
  idxs = swap_list(k, idxs);
  for (Case& c : cases) c.ps = swap_list(k, c.ps);
}

// Divide according to first column: (key, cases) sorted by key, each
// group's cases in their original order
template <class K, class F>
std::vector<std::pair<K, Cases>> divide(const Cases& cases, F key_of) {
  std::map<K, Cases> env;
  for (const Case& c : cases) env[key_of(c)].push_back(c);
  return std::vector<std::pair<K, Cases>>(env.begin(), env.end());
}

std::vector<std::pair<std::int64_t, Cases>> by_cell(const Cases& cases) {
  Cases rest;
  for (const Case& c : cases) rest.push_back(c);
  auto groups = divide<std::int64_t>(rest, [](const Case& c) { return c.ps.front(); });
  for (auto& [_, cs] : groups)
    for (Case& c : cs) c.ps.erase(c.ps.begin());
  return groups;
}

using CompileRec = std::function<expression(expression, expression, Cases)>;

// Switch according to one cell
expression match_oncell(const CompileRec& compile_rec, expression str, expression default_, long idx,
                        const std::vector<std::pair<std::int64_t, Cases>>& env) {
  Var id = gen_cell_id();
  std::function<expression(std::size_t, std::size_t)> comp_rec = [&](std::size_t lo, std::size_t hi) -> expression {
    std::size_t len = hi - lo;
    if (len <= 3) {
      // List.fold_right: the last case first
      expression r = default_;
      for (std::size_t k = hi; k-- > lo;) r = mk_cmp_gen(IC::Ceq, id, env[k].first, compile_rec(str, default_, env[k].second), r);
      return r;
    }
    std::size_t mid = lo + len / 2;
    // mk_lt id midkey (comp_rec lt) (comp_rec ge): right to left
    expression ge = comp_rec(mid, hi);
    expression lt = comp_rec(lo, mid);
    return mk_cmp_gen(IC::Clt, id, env[mid].first, lt, ge);
  };
  return mk_let_cell(id, str, idx, comp_rec(0, env.size()));
}

// Recursive 'list of cells' compile function: choose the matched cell and
// switch on it (patterns and idx all have the same length)
expression do_compile_pats(std::vector<long> idxs, expression str, expression default_, Cases cases) {
  if (idxs.empty()) return cases.empty() ? default_ : cases.front().act;
  best_first(idxs, cases);
  long idx = idxs.front();
  std::vector<long> rest(idxs.begin() + 1, idxs.end());
  return match_oncell([rest](expression s, expression d, Cases cs) { return do_compile_pats(rest, s, d, cs); }, str,
                      default_, idx, by_cell(cases));
}

// Switch according to pattern size.  from_ind is the starting index, it
// can be zero or one (when the switch on the cell 0 has already been
// performed.  In that latter case pattern len is string length-1 and is
// corrected.
expression compile_by_size(const Dbg& dbg, long from_ind, expression str, expression default_, const Cases& cases) {
  auto groups = divide<long>(cases, [](const Case& c) { return static_cast<long>(c.ps.size()); });
  std::vector<std::pair<long, expression>> size_cases;
  for (auto& [len0, cs] : groups) {
    long len = len0 + from_ind;
    std::vector<long> interval;
    for (long m = from_ind; m < len; ++m) interval.push_back(m);
    size_cases.push_back({len, do_compile_pats(interval, str, default_, cs)});
  }
  Var id = gen_size_id();
  expression sw = transl_int_switch(dbg, cvar(id), 1, max_int, size_cases, default_);
  return mk_let_size(id, str, sw);
}

// Compilation entry point: we choose to switch either on size or on first
// cell, using the 'least discriminant' heuristics.
expression top_compile(const Dbg& debuginfo, expression str, expression default_, const Cases& cases) {
  long a_len = count_arities_length(cases);
  long a_fst = count_arities_first(cases);
  if (a_len <= a_fst) return compile_by_size(debuginfo, 0, str, default_, cases);
  auto compile_size_rest = [&debuginfo](expression s, expression d, Cases cs) {
    return compile_by_size(debuginfo, 1, s, d, cs);
  };
  return match_oncell(compile_size_rest, str, default_, 0, by_cell(cases));
}

}  // namespace strmatch

expression strmatch_compile(const Dbg& dbg, expression str, expression default_,
                            const std::vector<std::pair<std::string_view, expression>>& cases0) {
  // We do not attempt to really optimise default=None
  std::vector<std::pair<std::string_view, expression>> cases = cases0;
  expression def = default_;
  if (!def) {
    if (cases.empty()) fatal("Strmatch.compile");
    def = cases.front().second;
    cases.erase(cases.begin());
  }
  // List.rev_map
  strmatch::Cases cs;
  for (auto it = cases.rbegin(); it != cases.rend(); ++it) cs.push_back({strmatch::pat_of_string(it->first), it->second});
  // catch dbg default (fun default -> top_compile dbg str default cases)
  if (auto* e = as<Cexit>(def); e && e->args.empty()) return strmatch::top_compile(dbg, str, def, cs);
  long e = L::next_raise_count();
  return ccatch(e, {}, strmatch::top_compile(dbg, str, cexit(e, {}), cs), def, dbg);
}

// ---- Applications ----------------------------------------------------------------------------
expression ptr_offset(expression ptr, long offset, const Dbg& dbg) {
  if (offset == 0) return ptr;
  return cop(O(OK::Caddv), {ptr, cconst_int(offset * size_addr, dbg)}, dbg);
}

expression direct_apply(std::string_view lbl, const std::vector<expression>& args, const Dbg& dbg) {
  std::vector<expression> a{cconst_symbol(lbl, dbg)};
  a.insert(a.end(), args.begin(), args.end());
  return cop(capply(typ_val()), slice(a), dbg);
}

expression generic_apply(MutableFlag mut, expression clos, const std::vector<expression>& args, const Dbg& dbg) {
  if (args.size() == 1) {
    expression arg = args[0];
    return bind("fun", clos, [&](expression clos) {
      return cop(capply(typ_val()), {get_field_codepointer(mut, clos, 0, dbg), arg, clos}, dbg);
    });
  }
  long arity = static_cast<long>(args.size());
  std::vector<expression> cargs{cconst_symbol(apply_function_sym(arity), dbg)};
  cargs.insert(cargs.end(), args.begin(), args.end());
  cargs.push_back(clos);
  return cop(capply(typ_val()), slice(cargs), dbg);
}

expression send(L::MethKind kind, expression met, expression obj, const std::vector<expression>& args,
                const Dbg& dbg) {
  auto call_met = [&](expression obj, const std::vector<expression>& args, expression clos) {
    // met is never a simple expression, so it never gets turned into an
    // Immutable load
    std::vector<expression> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    return generic_apply(MutableFlag::Mutable, clos, a, dbg);
  };
  return bind("obj", obj, [&](expression obj) -> expression {
    if (kind == L::MethKind::Self)
      return bind("met", lookup_label(obj, met, dbg), [&](expression clos) { return call_met(obj, args, clos); });
    if (kind == L::MethKind::Cached && args.size() >= 2) {
      std::vector<expression> rest(args.begin() + 2, args.end());
      return call_cached_method(obj, met, args[0], args[1], rest, dbg);
    }
    return bind("met", lookup_tag(obj, met, dbg), [&](expression clos) { return call_met(obj, args, clos); });
  });
}

// ---- Primitives ------------------------------------------------------------------------------
expression floatfield(long n, expression ptr, const Dbg& dbg) {
  return cop(mk_load_mut_op(MC::Double),
             {n == 0 ? ptr : cop(O(OK::Cadda), {ptr, cconst_int(n * size_float, dbg)}, dbg)}, dbg);
}

expression int_as_pointer(expression arg, const Dbg& dbg) {
  return cop(O(OK::Caddi), {arg, cconst_int(-1, dbg)}, dbg);  // always a pointer outside the heap
}

expression raise_prim(L::RaiseKind k, expression arg, const Dbg& dbg) {
  if (clflags::debug) return cop(craise(k), {arg}, dbg);
  return cop(craise(L::RaiseKind::Raise_notrace), {arg}, dbg);
}

expression negint(expression arg, const Dbg& dbg) { return cop(O(OK::Csubi), {cconst_int(2, dbg), arg}, dbg); }

expression offsetref(long n, expression arg, const Dbg& dbg) {
  return return_unit(dbg, bind("ref", arg, [&](expression arg) {
                       return cop(cstore(MC::Word_int, L::InitializationOrAssignment::Assignment),
                                  {arg, add_const(cop(mk_load_mut_op(MC::Word_int), {arg}, dbg), ilsl(n, 1), dbg)},
                                  dbg);
                     }));
}

expression arraylength(L::ArrayKind kind, expression arg, const Dbg& dbg) {
  expression hdr = get_header_masked(arg, dbg);
  switch (kind) {
    case L::ArrayKind::Pgenarray: {
      // wordsize_shift = numfloat_shift on 64-bit
      expression len = cop(O(OK::Clsr), {hdr, cconst_int(wordsize_shift, dbg)}, dbg);
      return cop(O(OK::Cor), {len, cconst_int(1, dbg)}, dbg);
    }
    case L::ArrayKind::Paddrarray:
    case L::ArrayKind::Pintarray:
      return cop(O(OK::Cor), {addr_array_length_shifted(hdr, dbg), cconst_int(1, dbg)}, dbg);
    case L::ArrayKind::Pfloatarray:
      return cop(O(OK::Cor), {float_array_length_shifted(hdr, dbg), cconst_int(1, dbg)}, dbg);
  }
  return hdr;
}

expression bbswap(BoxedInteger bi, expression arg, const Dbg& dbg) {
  const char* prim = bi == BoxedInteger::Pnativeint ? "nativeint" : bi == BoxedInteger::Pint32 ? "int32" : "int64";
  Exttype tyarg = bi == BoxedInteger::Pnativeint ? Exttype::XInt
                  : bi == BoxedInteger::Pint32   ? Exttype::XInt32
                                                 : Exttype::XInt64;
  return cop(cextcall(zstr(std::string("caml_") + prim + "_direct_bswap"), typ_int(), exttypes({tyarg}), false), {arg},
             dbg);
}

expression bswap16(expression arg, const Dbg& dbg) {
  return cop(cextcall("caml_bswap16_direct", typ_int(), no_exttypes(), false), {arg}, dbg);
}

// Helper for compilation of initialization and assignment operations
namespace {
enum class AssignmentKind { Caml_modify, Caml_initialize, Simple };
AssignmentKind assignment_kind(L::ImmediateOrPointer ptr, L::InitializationOrAssignment init) {
  if (ptr == L::ImmediateOrPointer::Immediate) return AssignmentKind::Simple;
  if (init == L::InitializationOrAssignment::Assignment) return AssignmentKind::Caml_modify;
  return AssignmentKind::Caml_initialize;
}
}  // namespace

expression setfield(long n, L::ImmediateOrPointer ptr, L::InitializationOrAssignment init, expression arg1,
                    expression arg2, const Dbg& dbg) {
  switch (assignment_kind(ptr, init)) {
    case AssignmentKind::Caml_modify:
      return return_unit(
          dbg, cop(cextcall("caml_modify", typ_void(), no_exttypes(), false), {field_address(arg1, n, dbg), arg2}, dbg));
    case AssignmentKind::Caml_initialize:
      return return_unit(dbg, cop(cextcall("caml_initialize", typ_void(), no_exttypes(), false),
                                  {field_address(arg1, n, dbg), arg2}, dbg));
    case AssignmentKind::Simple: return return_unit(dbg, set_field(arg1, n, arg2, init, dbg));
  }
  return arg1;
}

expression setfloatfield(long n, L::InitializationOrAssignment init, expression arg1, expression arg2,
                         const Dbg& dbg) {
  return return_unit(
      dbg, cop(cstore(MC::Double, init),
               {n == 0 ? arg1 : cop(O(OK::Cadda), {arg1, cconst_int(n * size_float, dbg)}, dbg), arg2}, dbg));
}

expression stringref_unsafe(expression arg1, expression arg2, const Dbg& dbg) {
  return tag_int(cop(mk_load_mut_op(MC::Byte_unsigned), {offset_addr(arg1, untag_int(arg2, dbg), dbg)}, dbg), dbg);
}

expression stringref_safe(expression arg1, expression arg2, const Dbg& dbg) {
  return tag_int(bind("index", untag_int(arg2, dbg),
                      [&](expression idx) {
                        return bind("str", arg1, [&](expression str) {
                          return csequence(make_checkbound(dbg, {string_length(str, dbg), idx}),
                                           cop(mk_load_mut_op(MC::Byte_unsigned), {offset_addr(str, idx, dbg)}, dbg));
                        });
                      }),
                 dbg);
}

expression string_load(MemoryAccessSize size, L::IsSafe unsafe, expression arg1, expression arg2, const Dbg& dbg) {
  return box_sized(size, dbg, bind("index", untag_int(arg2, dbg), [&](expression idx) {
                     return bind("str", arg1, [&](expression str) {
                       return check_bound(unsafe, size, dbg, string_length(str, dbg), idx,
                                          unaligned_load(size, str, idx, dbg));
                     });
                   }));
}

expression bigstring_load(MemoryAccessSize size, L::IsSafe unsafe, expression arg1, expression arg2,
                          const Dbg& dbg) {
  return box_sized(size, dbg, bind("index", untag_int(arg2, dbg), [&](expression idx) {
                     return bind("ba", arg1, [&](expression ba) {
                       return bind("ba_data", cop(mk_load_mut_op(MC::Word_int), {field_address(ba, 1, dbg)}, dbg),
                                   [&](expression ba_data) {
                                     return check_bound(unsafe, size, dbg, bigstring_length(ba, dbg), idx,
                                                        unaligned_load(size, ba_data, idx, dbg));
                                   });
                     });
                   }));
}

expression arrayref_unsafe(L::ArrayKind kind, expression arg1, expression arg2, const Dbg& dbg) {
  switch (kind) {
    case L::ArrayKind::Pgenarray:
      return bind("index", arg2, [&](expression idx) {
        return bind("arr", arg1, [&](expression arr) {
          return cifthenelse(is_addr_array_ptr(arr, dbg), dbg, addr_array_ref(arr, idx, dbg), dbg,
                             float_array_ref(arr, idx, dbg), dbg);
        });
      });
    case L::ArrayKind::Paddrarray: return addr_array_ref(arg1, arg2, dbg);
    // CR mshinwell: for int/addr_array_ref move "dbg" to first arg
    case L::ArrayKind::Pintarray: return int_array_ref(arg1, arg2, dbg);
    case L::ArrayKind::Pfloatarray: return float_array_ref(arg1, arg2, dbg);
  }
  return arg1;
}

expression arrayref_safe(L::ArrayKind kind, expression arg1, expression arg2, const Dbg& dbg) {
  switch (kind) {
    case L::ArrayKind::Pgenarray:
      return bind("index", arg2, [&](expression idx) {
        return bind("arr", arg1, [&](expression arr) {
          return bind("header", get_header_masked(arr, dbg), [&](expression hdr) {
            // wordsize_shift = numfloat_shift
            return csequence(make_checkbound(dbg, {addr_array_length_shifted(hdr, dbg), idx}),
                             cifthenelse(is_addr_array_hdr(hdr, dbg), dbg, addr_array_ref(arr, idx, dbg), dbg,
                                         float_array_ref(arr, idx, dbg), dbg));
          });
        });
      });
    case L::ArrayKind::Paddrarray:
      return bind("index", arg2, [&](expression idx) {
        return bind("arr", arg1, [&](expression arr) {
          return csequence(
              make_checkbound(dbg, {addr_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
              addr_array_ref(arr, idx, dbg));
        });
      });
    case L::ArrayKind::Pintarray:
      return bind("index", arg2, [&](expression idx) {
        return bind("arr", arg1, [&](expression arr) {
          return csequence(
              make_checkbound(dbg, {addr_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
              int_array_ref(arr, idx, dbg));
        });
      });
    case L::ArrayKind::Pfloatarray:
      return box_float(dbg, bind("index", arg2, [&](expression idx) {
                         return bind("arr", arg1, [&](expression arr) {
                           return csequence(
                               make_checkbound(dbg,
                                               {float_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
                               unboxed_float_array_ref(arr, idx, dbg));
                         });
                       }));
  }
  return arg1;
}

expression setfield_computed(L::ImmediateOrPointer ptr, L::InitializationOrAssignment init, expression arg1,
                             expression arg2, expression arg3, const Dbg& dbg) {
  switch (assignment_kind(ptr, init)) {
    case AssignmentKind::Caml_modify: return return_unit(dbg, addr_array_set(arg1, arg2, arg3, dbg));
    case AssignmentKind::Caml_initialize: return return_unit(dbg, addr_array_initialize(arg1, arg2, arg3, dbg));
    case AssignmentKind::Simple: return return_unit(dbg, int_array_set(arg1, arg2, arg3, dbg));
  }
  return arg1;
}

expression bytesset_unsafe(expression arg1, expression arg2, expression arg3, const Dbg& dbg) {
  return return_unit(dbg, cop(cstore(MC::Byte_unsigned, L::InitializationOrAssignment::Assignment),
                              {offset_addr(arg1, untag_int(arg2, dbg), dbg), ignore_high_bit_int(untag_int(arg3, dbg))},
                              dbg));
}

expression bytesset_safe(expression arg1, expression arg2, expression arg3, const Dbg& dbg) {
  return return_unit(dbg, bind("newval", ignore_high_bit_int(untag_int(arg3, dbg)), [&](expression newval) {
                       return bind("index", untag_int(arg2, dbg), [&](expression idx) {
                         return bind("str", arg1, [&](expression str) {
                           return csequence(make_checkbound(dbg, {string_length(str, dbg), idx}),
                                            cop(cstore(MC::Byte_unsigned, L::InitializationOrAssignment::Assignment),
                                                {offset_addr(str, idx, dbg), newval}, dbg));
                         });
                       });
                     }));
}

expression arrayset_unsafe(L::ArrayKind kind, expression arg1, expression arg2, expression arg3, const Dbg& dbg) {
  expression r = nullptr;  // (every kind is a case)
  switch (kind) {
    case L::ArrayKind::Pgenarray:
      r = bind("newval", arg3, [&](expression newval) {
        return bind("index", arg2, [&](expression index) {
          return bind("arr", arg1, [&](expression arr) {
            return cifthenelse(is_addr_array_ptr(arr, dbg), dbg, addr_array_set(arr, index, newval, dbg), dbg,
                               float_array_set(arr, index, unbox_float(dbg, newval), dbg), dbg);
          });
        });
      });
      break;
    case L::ArrayKind::Paddrarray: r = addr_array_set(arg1, arg2, arg3, dbg); break;
    case L::ArrayKind::Pintarray: r = int_array_set(arg1, arg2, arg3, dbg); break;
    case L::ArrayKind::Pfloatarray: r = float_array_set(arg1, arg2, arg3, dbg); break;
  }
  return return_unit(dbg, r);
}

expression arrayset_safe(L::ArrayKind kind, expression arg1, expression arg2, expression arg3, const Dbg& dbg) {
  expression r = nullptr;  // (every kind is a case)
  switch (kind) {
    case L::ArrayKind::Pgenarray:
      r = bind("newval", arg3, [&](expression newval) {
        return bind("index", arg2, [&](expression idx) {
          return bind("arr", arg1, [&](expression arr) {
            return bind("header", get_header_masked(arr, dbg), [&](expression hdr) {
              return csequence(make_checkbound(dbg, {addr_array_length_shifted(hdr, dbg), idx}),
                               cifthenelse(is_addr_array_hdr(hdr, dbg), dbg, addr_array_set(arr, idx, newval, dbg), dbg,
                                           float_array_set(arr, idx, unbox_float(dbg, newval), dbg), dbg));
            });
          });
        });
      });
      break;
    case L::ArrayKind::Paddrarray:
      r = bind("newval", arg3, [&](expression newval) {
        return bind("index", arg2, [&](expression idx) {
          return bind("arr", arg1, [&](expression arr) {
            return csequence(
                make_checkbound(dbg, {addr_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
                addr_array_set(arr, idx, newval, dbg));
          });
        });
      });
      break;
    case L::ArrayKind::Pintarray:
      r = bind("newval", arg3, [&](expression newval) {
        return bind("index", arg2, [&](expression idx) {
          return bind("arr", arg1, [&](expression arr) {
            return csequence(
                make_checkbound(dbg, {addr_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
                int_array_set(arr, idx, newval, dbg));
          });
        });
      });
      break;
    case L::ArrayKind::Pfloatarray:
      r = bind_load("newval", arg3, [&](expression newval) {
        return bind("index", arg2, [&](expression idx) {
          return bind("arr", arg1, [&](expression arr) {
            return csequence(
                make_checkbound(dbg, {float_array_length_shifted(get_header_masked(arr, dbg), dbg), idx}),
                float_array_set(arr, idx, newval, dbg));
          });
        });
      });
      break;
  }
  return return_unit(dbg, r);
}

expression bytes_set(MemoryAccessSize size, L::IsSafe unsafe, expression arg1, expression arg2, expression arg3,
                     const Dbg& dbg) {
  return return_unit(dbg, bind("newval", arg3, [&](expression newval) {
                       return bind("index", untag_int(arg2, dbg), [&](expression idx) {
                         return bind("str", arg1, [&](expression str) {
                           return check_bound(unsafe, size, dbg, string_length(str, dbg), idx,
                                              unaligned_set(size, str, idx, newval, dbg));
                         });
                       });
                     }));
}

expression bigstring_set(MemoryAccessSize size, L::IsSafe unsafe, expression arg1, expression arg2, expression arg3,
                         const Dbg& dbg) {
  return return_unit(dbg, bind("newval", arg3, [&](expression newval) {
                       return bind("index", untag_int(arg2, dbg), [&](expression idx) {
                         return bind("ba", arg1, [&](expression ba) {
                           return bind("ba_data",
                                       cop(mk_load_mut_op(MC::Word_int), {field_address(ba, 1, dbg)}, dbg),
                                       [&](expression ba_data) {
                                         return check_bound(unsafe, size, dbg, bigstring_length(ba, dbg), idx,
                                                            unaligned_set(size, ba_data, idx, newval, dbg));
                                       });
                         });
                       });
                     }));
}

// ---- Symbols ---------------------------------------------------------------------------------
std::vector<DataItem> cdefine_symbol(const Symb& symb) {
  if (symb.second == cmmgen_state::IsGlobal::Global)
    return {data_sym(DataItem::K::Cglobal_symbol, symb.first), data_sym(DataItem::K::Cdefine_symbol, symb.first)};
  return {data_sym(DataItem::K::Cdefine_symbol, symb.first)};
}

namespace {
std::vector<DataItem> cat(std::vector<DataItem> a, const std::vector<DataItem>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
}  // namespace

std::vector<DataItem> emit_block(const Symb& symb, std::int64_t white_header, std::vector<DataItem> cont) {
  // Headers for structured constants must be marked black in case we are in
  // no-naked-pointers mode.  See [caml_darken].
  std::int64_t black_header = white_header | caml_black;
  std::vector<DataItem> r{data_int(DataItem::K::Cint, black_header)};
  r = cat(r, cdefine_symbol(symb));
  return cat(r, cont);
}

namespace {
std::vector<DataItem> emit_string_constant_fields(std::string_view s, std::vector<DataItem> cont) {
  long n = size_int - 1 - static_cast<long>(s.size()) % size_int;
  std::vector<DataItem> r{data_sym(DataItem::K::Cstring, s), data_int(DataItem::K::Cskip, n),
                          data_int(DataItem::K::Cint8, n)};
  return cat(r, cont);
}
}  // namespace

std::vector<DataItem> emit_float_constant(const Symb& symb, double f, std::vector<DataItem> cont) {
  std::vector<DataItem> r{data_float(DataItem::K::Cdouble, f)};
  return emit_block(symb, float_header, cat(r, cont));
}
std::vector<DataItem> emit_string_constant(const Symb& symb, std::string_view s, std::vector<DataItem> cont) {
  return emit_block(symb, string_header(static_cast<long>(s.size())), emit_string_constant_fields(s, std::move(cont)));
}
std::vector<DataItem> emit_int32_constant(const Symb& symb, std::int64_t n, std::vector<DataItem> cont) {
  std::vector<DataItem> r{data_sym(DataItem::K::Csymbol_address, caml_int32_ops), data_int(DataItem::K::Cint32, n),
                          data_int(DataItem::K::Cint32, 0)};
  return emit_block(symb, boxedint32_header, cat(r, cont));
}
std::vector<DataItem> emit_int64_constant(const Symb& symb, std::int64_t n, std::vector<DataItem> cont) {
  std::vector<DataItem> r{data_sym(DataItem::K::Csymbol_address, caml_int64_ops), data_int(DataItem::K::Cint, n)};
  return emit_block(symb, boxedint64_header, cat(r, cont));
}
std::vector<DataItem> emit_nativeint_constant(const Symb& symb, std::int64_t n, std::vector<DataItem> cont) {
  std::vector<DataItem> r{data_sym(DataItem::K::Csymbol_address, caml_nativeint_ops), data_int(DataItem::K::Cint, n)};
  return emit_block(symb, boxedintnat_header, cat(r, cont));
}
std::vector<DataItem> emit_float_array_constant(const Symb& symb, Slice<double> fields, std::vector<DataItem> cont) {
  std::vector<DataItem> r;
  for (double f : fields) r.push_back(data_float(DataItem::K::Cdouble, f));
  return emit_block(symb, floatarray_header(static_cast<long>(fields.size())), cat(r, cont));
}

// To compile "let rec" over values
long fundecls_size(Slice<const clambda::UFunction*> fundecls) {
  long sz = -1;
  for (const clambda::UFunction* f : fundecls) {
    // arity 1 does not need an indirect call handler.  arity 0 cannot be
    // indirect called.  For other arities there is an indirect call handler.
    long indirect_call_code_pointer_size = (f->arity == 0 || f->arity == 1) ? 0 : 1;
    sz = sz + 1 + 2 + indirect_call_code_pointer_size;
  }
  return sz;
}

// Emit constant closures
std::vector<DataItem> emit_constant_closure(const Symb& symb, Slice<const clambda::UFunction*> fundecls,
                                            std::vector<DataItem> clos_vars, std::vector<DataItem> cont) {
  using DK = DataItem::K;
  auto closure_symbol = [&](const clambda::UFunction* f) -> std::vector<DataItem> {
    if constexpr (config::flambda) return cdefine_symbol({zstr(std::string(f->label) + "_closure"), symb.second});
    else return {};
  };
  if (fundecls.empty()) {
    // This should probably not happen: dead code has normally been
    // eliminated and a closure cannot be accessed without going through a
    // [Project_closure], which depends on the function.
    return cat(cat(cdefine_symbol(symb), clos_vars), cont);
  }
  long startenv = fundecls_size(fundecls);
  // emit_others pos rem: a cons chain, evaluated from its tail (the curry
  // functions needed in reverse)
  std::function<std::vector<DataItem>(long, std::size_t)> emit_others = [&](long pos,
                                                                           std::size_t k) -> std::vector<DataItem> {
    if (k == fundecls.size()) return cat(clos_vars, cont);
    const clambda::UFunction* f2 = fundecls[k];
    if (f2->arity == 1 || f2->arity == 0) {
      std::vector<DataItem> rest = emit_others(pos + 3, k + 1);
      std::vector<DataItem> r{data_int(DK::Cint, infix_header(pos))};
      r = cat(r, closure_symbol(f2));
      r = cat(r, {data_sym(DK::Csymbol_address, f2->label), data_int(DK::Cint, closure_info(f2->arity, startenv - pos))});
      return cat(r, rest);
    }
    std::vector<DataItem> rest = emit_others(pos + 4, k + 1);
    std::string_view curry = curry_function_sym(f2->arity);
    std::vector<DataItem> r{data_int(DK::Cint, infix_header(pos))};
    r = cat(r, closure_symbol(f2));
    r = cat(r, {data_sym(DK::Csymbol_address, curry), data_int(DK::Cint, closure_info(f2->arity, startenv - pos)),
                data_sym(DK::Csymbol_address, f2->label)});
    return cat(r, rest);
  };
  const clambda::UFunction* f1 = fundecls[0];
  std::vector<DataItem> tail;
  if (f1->arity == 1 || f1->arity == 0) {
    std::vector<DataItem> rest = emit_others(3, 1);
    tail = {data_sym(DK::Csymbol_address, f1->label), data_int(DK::Cint, closure_info(f1->arity, startenv))};
    tail = cat(tail, rest);
  } else {
    std::vector<DataItem> rest = emit_others(4, 1);
    std::string_view curry = curry_function_sym(f1->arity);
    tail = {data_sym(DK::Csymbol_address, curry), data_int(DK::Cint, closure_info(f1->arity, startenv)),
            data_sym(DK::Csymbol_address, f1->label)};
    tail = cat(tail, rest);
  }
  std::vector<DataItem> r{
      data_int(DK::Cint, black_closure_header(fundecls_size(fundecls) + static_cast<long>(clos_vars.size())))};
  r = cat(r, cdefine_symbol(symb));
  r = cat(r, closure_symbol(f1));
  return cat(r, tail);
}

// Build preallocated blocks (used for Flambda [Initialize_symbol]
// constructs, and Clambda global module)
std::vector<Phrase> emit_preallocated_blocks(const std::vector<clambda::PreallocatedBlock>& blocks,
                                             std::vector<Phrase> cont) {
  using DK = DataItem::K;
  // emit_gc_roots_table ~symbols cont
  std::string_view table_symbol = compilenv::make_symbol(std::string_view("gc_roots"));
  Phrase roots;
  roots.data = {data_sym(DK::Cglobal_symbol, table_symbol), data_sym(DK::Cdefine_symbol, table_symbol)};
  for (auto& b : blocks) roots.data.push_back(data_sym(DK::Csymbol_address, b.symbol));
  roots.data.push_back(data_int(DK::Cint, 0));
  cont.insert(cont.begin(), std::move(roots));
  // List.fold_left preallocate_block c1 blocks
  for (auto& b : blocks) {
    // These words will be registered as roots and as such must contain
    // valid values, in case we are in no-naked-pointers mode.  Likewise the
    // block header must be black, below (see [caml_darken]), since the
    // overall record may be referenced.
    std::vector<DataItem> space;
    for (auto& field : b.fields) {
      if (!field) space.push_back(data_int(DK::Cint, 1));  // Val_unit
      else if (field->is_ref) space.push_back(data_sym(DK::Csymbol_address, field->ref));
      else space.push_back(cint_const(field->i));
    }
    Symb symb{b.symbol, b.exported ? cmmgen_state::IsGlobal::Global : cmmgen_state::IsGlobal::Local};
    Phrase p;
    p.data = emit_block(symb, block_header(b.tag, static_cast<long>(b.fields.size())), space);
    cont.insert(cont.begin(), std::move(p));
  }
  return cont;
}

}  // namespace cppcaml::typing::cmm_helpers

// ---- The startup module ------------------------------------------------------------------------
namespace cppcaml::typing::cmm_helpers {
using namespace cmm;
namespace {
using DK = DataItem::K;
constexpr MutableFlag Mut = MutableFlag::Mutable;
std::string_view zcat(const std::string& s) { return zstr(s); }
Phrase function_phrase(std::string_view name, std::vector<CatchParam> args, expression body,
                       std::vector<CodegenOption> opts = {}) {
  auto* fd = make<Fundecl>();
  fd->fun_name = name;
  fd->fun_args = slice(args);
  fd->fun_body = body;
  fd->fun_codegen_options = slice(opts);
  fd->fun_poll = L::PollAttribute::Default_poll;
  Phrase p;
  p.fn = fd;
  return p;
}
CatchParam param(Var v, Machtype ty) { return {vpc(v), ty}; }

//   while (li < hi) { // no need to check the 1st time
//     mi = ((li+hi) >> 1) | 1;
//     if (tag < Field(meths,mi)) hi = mi-2;
//     else li = mi;
//   }
//   *cache = (li-3)*sizeof(value)+1;
//   return Field (meths, li-1);
expression cache_public_method(expression meths, expression tag, expression cache, const Dbg& dbg) {
  long raise_num = L::next_raise_count();
  auto ci = [&](long i) { return cconst_int(i, dbg); };
  Var li = Ident::create_local("*li*");
  Var hi = Ident::create_local("*hi*");
  Var mi = Ident::create_local("*mi*");
  Var tagged = Ident::create_local("*tagged*");
  expression loop_body = clet(
      vpc(mi),
      cop(O(OK::Cor), {cop(O(OK::Clsr), {cop(O(OK::Caddi), {cvar_mut(li), cvar_mut(hi)}, dbg), ci(1)}, dbg), ci(1)},
          dbg),
      csequence(cifthenelse(cop(ccmpi(IntegerComparison::Clt),
                                {tag, cop(mk_load_mut_op(MC::Word_int),
                                          {cop(O(OK::Cadda), {meths, lsl_const(cvar(mi), log2_size_addr, dbg)}, dbg)},
                                          dbg)},
                                dbg),
                            dbg, cassign(hi, cop(O(OK::Csubi), {cvar(mi), ci(2)}, dbg)), dbg, cassign(li, cvar(mi)),
                            dbg),
                cifthenelse(cop(ccmpi(IntegerComparison::Cge), {cvar_mut(li), cvar_mut(hi)}, dbg), dbg,
                            cexit(raise_num, {}), dbg, ctuple({}), dbg)));
  expression search = ccatch(raise_num, {}, create_loop(loop_body, dbg), ctuple({}), dbg);
  expression result = clet(
      vpc(tagged),
      cop(O(OK::Caddi), {lsl_const(cvar_mut(li), log2_size_addr, dbg), ci(1 - 3 * size_addr)}, dbg),
      csequence(cop(cstore(MC::Word_int, L::InitializationOrAssignment::Assignment), {cache, cvar(tagged)}, dbg),
                cvar(tagged)));
  return clet_mut(vpc(li), typ_int(), ci(3),
                  clet_mut(vpc(hi), typ_int(), cop(mk_load_mut_op(MC::Word_int), {meths}, dbg),
                           csequence(search, result)));
}

// Generate an application function:
//   (defun caml_applyN (a1 ... aN clos)
//     (if (= clos.arity N)
//       (app clos.direct a1 ... aN clos)
//       (let (clos1 (app clos.code a1 clos)
//             clos2 (app clos1.code a2 clos)
//             ...
//             closN-1 (app closN-2.code aN-1 closN-2))
//         (app closN-1.code aN closN-1))))
struct ApplyBody {
  std::vector<Var> args;
  Var clos;
  expression body;
};
ApplyBody apply_function_body(long arity) {
  Dbg dbg;
  std::vector<Var> arg(static_cast<std::size_t>(arity), Ident::create_local("arg"));
  for (long i = 1; i < arity; ++i) arg[static_cast<std::size_t>(i)] = Ident::create_local("arg");
  Var clos = Ident::create_local("clos");
  std::function<expression(Var, long)> app_fun = [&](Var clos, long n) -> expression {
    expression call = cop(capply(typ_val()),
                          {get_field_codepointer(Mut, cvar(clos), 0, dbg), cvar(arg[static_cast<std::size_t>(n)]),
                           cvar(clos)},
                          dbg);
    if (n == arity - 1) return call;
    Var newclos = Ident::create_local("clos");
    expression rest = app_fun(newclos, n + 1);
    return clet(vpc(newclos), call, rest);
  };
  if (arity == 1) return {arg, clos, app_fun(clos, 0)};
  expression slow = app_fun(clos, 0);
  std::vector<expression> direct{get_field_codepointer(Mut, cvar(clos), 2, dbg)};
  for (Var a : arg) direct.push_back(cvar(a));
  direct.push_back(cvar(clos));
  expression body = cifthenelse(
      cop(ccmpi(IntegerComparison::Ceq),
          {cop(O(OK::Casr), {get_field_gen(Mut, cvar(clos), 1, dbg), cconst_int(pos_arity_in_closinfo, dbg)}, dbg),
           cconst_int(arity, dbg)},
          dbg),
      dbg, cop(capply(typ_val()), slice(direct), dbg), dbg, slow, dbg);
  return {arg, clos, body};
}

Phrase send_function(long arity) {
  Dbg dbg;
  auto ci = [&](long i) { return cconst_int(i, dbg); };
  ApplyBody ab = apply_function_body(1 + arity);
  Var cache = Ident::create_local("cache");
  Var obj = ab.args.front();
  Var tag = Ident::create_local("tag");
  Var meths = Ident::create_local("meths");
  Var cached = Ident::create_local("cached");
  Var real = Ident::create_local("real");
  expression mask = get_field_gen(Mut, cvar(meths), 1, dbg);
  expression cached_pos = cvar(cached);
  expression tag_pos =
      cop(O(OK::Cadda), {cop(O(OK::Cadda), {cached_pos, cvar(meths)}, dbg), ci(3 * size_addr - 1)}, dbg);
  expression tag_ = cop(mk_load_mut_op(MC::Word_int), {tag_pos}, dbg);
  expression lookup = cache_public_method(cvar(meths), cvar(tag), cvar(cache), dbg);
  expression clos = clet(
      vpc(meths), cop(mk_load_mut_op(MC::Word_val), {cvar(obj)}, dbg),
      clet(vpc(cached),
           cop(O(OK::Cand), {cop(mk_load_mut_op(MC::Word_int), {cvar(cache)}, dbg), mask}, dbg),
           clet(vpc(real),
                cifthenelse(cop(ccmpa(IntegerComparison::Cne), {tag_, cvar(tag)}, dbg), dbg, lookup, dbg,
                            cached_pos, dbg),
                cop(mk_load_mut_op(MC::Word_val),
                    {cop(O(OK::Cadda), {cop(O(OK::Cadda), {cvar(real), cvar(meths)}, dbg), ci(2 * size_addr - 1)},
                         dbg)},
                    dbg))));
  expression body = clet(vpc(ab.clos), clos, ab.body);
  std::vector<CatchParam> args{param(obj, typ_val()), param(tag, typ_int()), param(cache, typ_addr())};
  for (std::size_t i = 1; i < ab.args.size(); ++i) args.push_back(param(ab.args[i], typ_val()));
  return function_phrase(zcat("caml_send" + std::to_string(arity)), std::move(args), body);
}

Phrase apply_function(long arity) {
  ApplyBody ab = apply_function_body(arity);
  std::vector<CatchParam> args;
  for (Var a : ab.args) args.push_back(param(a, typ_val()));
  args.push_back(param(ab.clos, typ_val()));
  return function_phrase(zcat("caml_apply" + std::to_string(arity)), std::move(args), ab.body);
}

// Generate tuplifying functions:
//    (defun caml_tuplifyN (arg clos)
//      (app clos.direct #0(arg) ... #N-1(arg) clos))
Phrase tuplify_function(long arity) {
  Dbg dbg;
  Var arg = Ident::create_local("arg");
  Var clos = Ident::create_local("clos");
  std::vector<expression> a{get_field_codepointer(Mut, cvar(clos), 2, dbg)};
  for (long i = 0; i < arity; ++i) a.push_back(get_field_gen(Mut, cvar(arg), i, dbg));
  a.push_back(cvar(clos));
  return function_phrase(zcat("caml_tuplify" + std::to_string(arity)),
                         {param(arg, typ_val()), param(clos, typ_val())}, cop(capply(typ_val()), slice(a), dbg));
}

// Generate currying functions (see cmm_helpers.ml); the "_app" shortcuts
// only below max_arity_optimized (PR#5933)
constexpr long max_arity_optimized = 15;
Phrase final_curry_function(long arity) {
  Dbg dbg;
  Var last_arg = Ident::create_local("arg");
  Var last_clos = Ident::create_local("clos");
  std::function<expression(std::vector<expression>, Var, long)> curry_fun =
      [&](std::vector<expression> args, Var clos, long n) -> expression {
    if (n == 0) {
      std::vector<expression> a{get_field_codepointer(Mut, cvar(clos), 2, dbg)};
      a.insert(a.end(), args.begin(), args.end());
      a.push_back(cvar(last_arg));
      a.push_back(cvar(clos));
      return cop(capply(typ_val()), slice(a), dbg);
    }
    bool last = n == arity - 1 || arity > max_arity_optimized;
    Var newclos = Ident::create_local("clos");
    args.insert(args.begin(), get_field_gen(Mut, cvar(clos), last ? 2 : 3, dbg));
    expression rest = curry_fun(std::move(args), newclos, n - 1);
    return clet(vpc(newclos), get_field_gen(Mut, cvar(clos), last ? 3 : 4, dbg), rest);
  };
  return function_phrase(zcat("caml_curry" + std::to_string(arity) + "_" + std::to_string(arity - 1)),
                         {param(last_arg, typ_val()), param(last_clos, typ_val())},
                         curry_fun({}, last_clos, arity - 1));
}

void intermediate_curry_functions(long arity, long num, std::vector<Phrase>& out) {
  Dbg dbg;
  if (num == arity - 1) {
    out.push_back(final_curry_function(arity));
    return;
  }
  std::string name1 = "caml_curry" + std::to_string(arity);
  std::string name2 = num == 0 ? name1 : name1 + "_" + std::to_string(num);
  Var arg = Ident::create_local("arg");
  Var clos = Ident::create_local("clos");
  std::string next = name1 + "_" + std::to_string(num + 1);
  expression body;
  if (arity - num > 2 && arity <= max_arity_optimized)
    body = cop(O(OK::Calloc),
               {alloc_closure_header(5, dbg), cconst_symbol(zcat(next), dbg),
                alloc_closure_info(arity - num - 1, 3, dbg), cconst_symbol(zcat(next + "_app"), dbg), cvar(arg),
                cvar(clos)},
               dbg);
  else
    body = cop(O(OK::Calloc),
               {alloc_closure_header(4, dbg), cconst_symbol(zcat(next), dbg), alloc_closure_info(1, 2, dbg),
                cvar(arg), cvar(clos)},
               dbg);
  out.push_back(function_phrase(zcat(name2), {param(arg, typ_val()), param(clos, typ_val())}, body));
  if (arity <= max_arity_optimized && arity - num > 2) {
    std::vector<Var> direct_args;
    for (long i = num + 2; i <= arity; ++i) direct_args.push_back(Ident::create_local(zcat("arg" + std::to_string(i))));
    std::function<expression(long, std::vector<expression>, Var)> iter = [&](long i, std::vector<expression> args,
                                                                            Var clos) -> expression {
      if (i == 0) {
        std::vector<expression> a{get_field_codepointer(Mut, cvar(clos), 2, dbg)};
        a.insert(a.end(), args.begin(), args.end());
        a.push_back(cvar(clos));
        return cop(capply(typ_val()), slice(a), dbg);
      }
      Var newclos = Ident::create_local("clos");
      args.insert(args.begin(), get_field_gen(Mut, cvar(clos), 3, dbg));
      expression rest = iter(i - 1, std::move(args), newclos);
      return clet(vpc(newclos), get_field_gen(Mut, cvar(clos), 4, dbg), rest);
    };
    std::vector<CatchParam> fun_args;
    std::vector<expression> dargs;
    for (Var a : direct_args) {
      fun_args.push_back(param(a, typ_val()));
      dargs.push_back(cvar(a));
    }
    fun_args.push_back(param(clos, typ_val()));
    expression b = iter(num + 1, dargs, clos);
    out.push_back(function_phrase(zcat(next + "_app"), std::move(fun_args), b));
  }
  intermediate_curry_functions(arity, num + 1, out);
}

std::vector<Phrase> curry_function(long arity) {
  if (arity == 0) fatal("Cmm_helpers.curry_function");
  std::vector<Phrase> r;
  if (arity > 0) intermediate_curry_functions(arity, 0, r);
  else r.push_back(tuplify_function(-arity));
  return r;
}

std::vector<DataItem> symbols_table(std::string_view symbol, const std::vector<std::string_view>& namelist,
                                    const char* id) {
  std::vector<DataItem> r{data_sym(DK::Cglobal_symbol, symbol), data_sym(DK::Cdefine_symbol, symbol)};
  for (std::string_view name : namelist) r.push_back(data_sym(DK::Csymbol_address, compilenv::make_symbol_in(name, id)));
  r.push_back(data_int(DK::Cint, 0));
  return r;
}

Phrase segment_table(const std::vector<std::string_view>& namelist, std::string_view symbol, const char* begname,
                     const char* endname) {
  Phrase p;
  p.data = {data_sym(DK::Cglobal_symbol, symbol), data_sym(DK::Cdefine_symbol, symbol)};
  for (std::string_view name : namelist) {
    p.data.push_back(data_sym(DK::Csymbol_address, compilenv::make_symbol_in(name, begname)));
    p.data.push_back(data_sym(DK::Csymbol_address, compilenv::make_symbol_in(name, endname)));
  }
  p.data.push_back(data_int(DK::Cint, 0));
  return p;
}
}  // namespace

const std::vector<std::string_view> builtin_exceptions = {
    "Out_of_memory",  "Sys_error",      "Failure",        "Invalid_argument", "End_of_file",
    "Division_by_zero", "Not_found",    "Match_failure",  "Stack_overflow",   "Sys_blocked_io",
    "Assert_failure", "Undefined_recursive_module", "Todo"};

// These apply funs are always present in the main program because the
// run-time system needs them (cf. runtime/<arch>.S)
std::vector<Phrase> generic_functions(bool shared, const std::vector<const cmx_format::UnitInfos*>& units) {
  std::set<long> apply, send, curry;
  for (const cmx_format::UnitInfos* ui : units) {
    apply.insert(ui->ui_apply_fun.begin(), ui->ui_apply_fun.end());
    send.insert(ui->ui_send_fun.begin(), ui->ui_send_fun.end());
    curry.insert(ui->ui_curry_fun.begin(), ui->ui_curry_fun.end());
  }
  if (!shared) apply.insert({2, 3});
  // Int.Set.fold, each consed on the accumulator
  std::vector<Phrase> accu;
  for (long n : apply) accu.insert(accu.begin(), apply_function(n));
  for (long n : send) accu.insert(accu.begin(), send_function(n));
  for (long n : curry) {
    std::vector<Phrase> c = curry_function(n);
    accu.insert(accu.begin(), c.begin(), c.end());
  }
  return accu;
}

// Generate the entry point:
//   CAMLprim value caml_program()
//   {
//     int id = 0;
//     while (true) {
//       if (id == len_caml_globals_entry_functions) goto out;
//       caml_globals_entry_functions[id]();
//       caml_globals_inited += 1;
//       id += 1;
//     }
//     out:
//     return 1;
//   }
std::vector<Phrase> entry_point(const std::vector<std::string_view>& namelist) {
  Dbg dbg;
  auto ci = [&](long i) { return cconst_int(i, dbg); };
  auto incr_global_inited = [&] {
    return cop(cstore(MC::Word_int, L::InitializationOrAssignment::Assignment),
               {cconst_symbol("caml_globals_inited", dbg),
                cop(O(OK::Caddi),
                    {cop(mk_load_mut_op(MC::Word_int), {cconst_symbol("caml_globals_inited", dbg)}, dbg), ci(1)},
                    dbg)},
               dbg);
  };
  std::string_view table_symbol = compilenv::make_symbol(std::string_view("caml_globals_entry_functions"));
  auto call = [&](expression i) {
    // address of caml_globals_entry_functions[i]
    expression entry_slot = cop(O(OK::Cadda),
                                {cconst_symbol(table_symbol, dbg),
                                 cop(O(OK::Clsl), {i, ci(log2(size_addr))}, dbg)},
                                dbg);
    return csequence(cop(capply(typ_void()), {cop(mk_load_immut(MC::Word_int), {entry_slot}, dbg)}, dbg),
                     incr_global_inited());
  };
  Phrase data;
  data.data.push_back(data_sym(DK::Cdefine_symbol, table_symbol));
  for (std::string_view name : namelist)
    data.data.push_back(data_sym(DK::Csymbol_address, compilenv::make_symbol_in(name, "entry")));
  long raise_num = L::next_raise_count();
  Var id = Ident::create_local("*id*");
  expression var_id = cvar(id);
  expression high = ci(static_cast<long>(namelist.size()));
  expression next_iteration = cexit(raise_num, slice(std::vector<expression>{cop(O(OK::Caddi), {var_id, ci(1)}, dbg)}));
  Handler h{raise_num, slice(std::vector<CatchParam>{param(id, typ_int())}),
            cifthenelse(cop(ccmpi(IntegerComparison::Ceq), {var_id, high}, dbg), dbg, ctuple({}), dbg,
                        csequence(call(var_id), next_iteration), dbg),
            dbg};
  expression body = ccatch_node(cmm::RecFlag::Recursive, slice(std::vector<Handler>{h}),
                                cexit(raise_num, slice(std::vector<expression>{ci(0)})));
  std::vector<Phrase> r{std::move(data)};
  r.push_back(function_phrase("caml_program", {}, csequence(body, ci(1)), {CodegenOption::Reduce_code_size}));
  return r;
}

// Generate the table of globals
Phrase global_table(const std::vector<std::string_view>& namelist) {
  Phrase p;
  p.data = symbols_table("caml_globals", namelist, "gc_roots");
  return p;
}

Phrase reference_symbols(const std::vector<std::string_view>& namelist) {
  Phrase p;
  for (std::string_view name : namelist) p.data.push_back(data_sym(DK::Csymbol_address, name));
  return p;
}

Phrase global_data(std::string_view name, std::string_view marshaled) {
  Phrase p;
  p.data = emit_string_constant({name, cmmgen_state::IsGlobal::Global}, marshaled, {});
  return p;
}

// Generate the master table of frame descriptors
Phrase frame_table(const std::vector<std::string_view>& namelist) {
  Phrase p;
  p.data = symbols_table("caml_frametable", namelist, "frametable");
  return p;
}

// Generate the table of module data and code segments
Phrase data_segment_table(const std::vector<std::string_view>& namelist) {
  return segment_table(namelist, "caml_data_segments", "data_begin", "data_end");
}
Phrase code_segment_table(const std::vector<std::string_view>& namelist) {
  return segment_table(namelist, "caml_code_segments", "code_begin", "code_end");
}

// Initialize a predefined exception
Phrase predef_exception(long i, std::string_view name) {
  std::string_view name_sym = compilenv::new_const_symbol();
  std::vector<DataItem> fields{data_sym(DK::Csymbol_address, name_sym), cint_const(-i - 1)};
  std::vector<DataItem> data_items = emit_string_constant({name_sym, cmmgen_state::IsGlobal::Local}, name, {});
  fields.insert(fields.end(), data_items.begin(), data_items.end());
  Phrase p;
  p.data = emit_block({zcat("caml_exn_" + std::string(name)), cmmgen_state::IsGlobal::Global},
                      block_header(object_tag, 2), fields);
  return p;
}

Phrase emit_global_string_constant(std::string_view name, std::string_view value) {
  Phrase p;
  p.data = emit_string_constant({name, cmmgen_state::IsGlobal::Global}, value, {});
  return p;
}

}  // namespace cppcaml::typing::cmm_helpers
