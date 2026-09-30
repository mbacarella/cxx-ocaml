// Port of asmcomp/cmm.ml: the C-- intermediate language (Cmmgen's output,
// Selection's input).  Zone-allocated, immutable nodes like Clambda's.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "cppcaml/typing/clambda.hpp"

namespace cppcaml::typing::cmm {

using clambda::VarWithProvenance;
using Var = Ident::t;
using lambda::FloatComparison;
using lambda::IntegerComparison;

enum class MachtypeComponent : std::uint8_t { Val, Addr, Int, Float };
using Machtype = Slice<MachtypeComponent>;
Machtype typ_void();
Machtype typ_val();
Machtype typ_addr();
Machtype typ_int();
Machtype typ_float();
MachtypeComponent lub_component(MachtypeComponent a, MachtypeComponent b);
bool ge_component(MachtypeComponent a, MachtypeComponent b);

enum class Exttype : std::uint8_t { XInt, XInt32, XInt64, XFloat };
Machtype machtype_of_exttype(Exttype t);
Machtype machtype_of_exttype_list(Slice<Exttype> xtl);

using Label = long;
Label new_label();
Label cur_label();
void set_label(Label l);

enum class RecFlag : std::uint8_t { Nonrecursive, Recursive };

enum class MemoryChunk : std::uint8_t {
  Byte_unsigned, Byte_signed, Sixteen_unsigned, Sixteen_signed, Thirtytwo_unsigned, Thirtytwo_signed,
  Sixtyfour, Word_int, Word_val, Single, Double
};

struct Operation {
  enum class K : std::uint8_t {
    Capply, Cextcall, Cload, Calloc, Cstore, Caddi, Csubi, Cmuli, Cmulhi, Cdivi, Cmodi, Cand, Cor, Cxor,
    Clsl, Clsr, Casr, Ccmpi, Caddv, Cadda, Ccmpa, Cnegf, Cabsf, Caddf, Csubf, Cmulf, Cdivf, Cfloatofint,
    Cintoffloat, Ccmpf, Craise, Ccheckbound, Copaque, Cdls_get, Cpoll
  };
  K kind;
  Machtype ty;                   // Capply's result; Cextcall's ty_res
  std::string_view name;         // Cextcall
  Slice<Exttype> ty_args;        // Cextcall
  bool alloc = false;            // Cextcall
  MemoryChunk chunk = MemoryChunk::Word_int;  // Cload / Cstore
  MutableFlag mut = MutableFlag::Immutable;   // Cload
  bool is_atomic = false;                     // Cload
  lambda::InitializationOrAssignment init = lambda::InitializationOrAssignment::Assignment;  // Cstore
  IntegerComparison icmp = IntegerComparison::Ceq;  // Ccmpi / Ccmpa
  FloatComparison fcmp = FloatComparison::CFeq;     // Ccmpf
  lambda::RaiseKind raise = lambda::RaiseKind::Raise_regular;  // Craise
};
inline Operation op(Operation::K k) { return Operation{k}; }
Operation capply(Machtype ty);
Operation cextcall(std::string_view name, Machtype ty_res, Slice<Exttype> ty_args, bool alloc);
Operation cload(MemoryChunk chunk, MutableFlag mut, bool is_atomic = false);
Operation cstore(MemoryChunk chunk, lambda::InitializationOrAssignment init);
Operation ccmpi(IntegerComparison c);
Operation ccmpa(IntegerComparison c);
Operation ccmpf(FloatComparison c);
Operation craise(lambda::RaiseKind k);

enum class EK : std::uint8_t {
  Cconst_int, Cconst_natint, Cconst_float, Cconst_symbol, Cvar, Cvar_mut, Clet, Clet_mut, Cphantom_let, Cassign,
  Ctuple, Cop, Csequence, Cifthenelse, Cswitch, Ccatch, Cexit, Ctrywith, Creturn_addr
};
struct ExprNode {
  EK kind;
};
using expression = const ExprNode*;
template <class T>
const T* as(expression e) {
  return e && e->kind == T::K ? static_cast<const T*>(e) : nullptr;
}
#define CMM_CTOR(Name) \
  struct Name : ExprNode { \
    static constexpr EK K = EK::Name;
CMM_CTOR(Cconst_int) long n; debuginfo::t dbg; };
CMM_CTOR(Cconst_natint) std::int64_t n; debuginfo::t dbg; };
CMM_CTOR(Cconst_float) double f; debuginfo::t dbg; };
CMM_CTOR(Cconst_symbol) std::string_view s; debuginfo::t dbg; };
CMM_CTOR(Cvar) Var id; };
CMM_CTOR(Cvar_mut) Var id; };
CMM_CTOR(Clet) VarWithProvenance id; expression def; expression body; };
CMM_CTOR(Clet_mut) VarWithProvenance id; Machtype ty; expression def; expression body; };
CMM_CTOR(Cphantom_let) VarWithProvenance id; expression body; };  // Closure never makes phantom lets
CMM_CTOR(Cassign) Var id; expression e; };
CMM_CTOR(Ctuple) Slice<expression> el; };
CMM_CTOR(Cop) Operation op; Slice<expression> args; debuginfo::t dbg; };
CMM_CTOR(Csequence) expression e1; expression e2; };
CMM_CTOR(Cifthenelse) expression cond; debuginfo::t ifso_dbg; expression ifso; debuginfo::t ifnot_dbg; expression ifnot; debuginfo::t dbg; };
struct SwitchCase {
  expression e;
  debuginfo::t dbg;
};
CMM_CTOR(Cswitch) expression e; Slice<long> index; Slice<SwitchCase> cases; debuginfo::t dbg; };
struct CatchParam {
  VarWithProvenance id;
  Machtype ty;
};
struct Handler {
  long n;
  Slice<CatchParam> ids;
  expression body;
  debuginfo::t dbg;
};
CMM_CTOR(Ccatch) RecFlag rec; Slice<Handler> handlers; expression body; };
CMM_CTOR(Cexit) long n; Slice<expression> args; };
CMM_CTOR(Ctrywith) expression body; VarWithProvenance exn; expression handler; debuginfo::t dbg; };
CMM_CTOR(Creturn_addr) };
#undef CMM_CTOR

// constructors (fresh zone nodes)
expression cconst_int(long n, const debuginfo::t& dbg);
expression cconst_natint(std::int64_t n, const debuginfo::t& dbg);
expression cconst_float(double f, const debuginfo::t& dbg);
expression cconst_symbol(std::string_view s, const debuginfo::t& dbg);
expression cvar(Var id);
expression cvar_mut(Var id);
expression clet(VarWithProvenance id, expression def, expression body);
expression clet_mut(VarWithProvenance id, Machtype ty, expression def, expression body);
expression cassign(Var id, expression e);
expression ctuple(Slice<expression> el);
expression cop(const Operation& op, Slice<expression> args, const debuginfo::t& dbg);
expression cop(const Operation& op, std::initializer_list<expression> args, const debuginfo::t& dbg);
expression csequence(expression e1, expression e2);
expression cifthenelse(expression cond, const debuginfo::t& ifso_dbg, expression ifso, const debuginfo::t& ifnot_dbg,
                       expression ifnot, const debuginfo::t& dbg);
expression cswitch(expression e, Slice<long> index, Slice<SwitchCase> cases, const debuginfo::t& dbg);
expression ccatch_node(RecFlag rec, Slice<Handler> handlers, expression body);
expression cexit(long n, Slice<expression> args);
expression ctrywith(expression body, VarWithProvenance exn, expression handler, const debuginfo::t& dbg);
expression creturn_addr();
// ccatch (i, ids, e1, e2, dbg) = Ccatch(Nonrecursive, [i, ids, e2, dbg], e1)
expression ccatch(long i, Slice<CatchParam> ids, expression e1, expression e2, const debuginfo::t& dbg);

// Cconst_int (n, _) patterns
inline bool is_cint(expression e, long& n) {
  auto* c = as<Cconst_int>(e);
  if (!c) return false;
  n = c->n;
  return true;
}
inline bool is_cint_eq(expression e, long n) {
  auto* c = as<Cconst_int>(e);
  return c && c->n == n;
}

enum class CodegenOption : std::uint8_t { Reduce_code_size, No_CSE };

struct Fundecl {
  std::string_view fun_name;
  Slice<CatchParam> fun_args;  // (VP.t * machtype) list
  expression fun_body;
  Slice<CodegenOption> fun_codegen_options;
  lambda::PollAttribute fun_poll;
  debuginfo::t fun_dbg;
};

struct DataItem {
  enum class K : std::uint8_t {
    Cdefine_symbol, Cglobal_symbol, Cint8, Cint16, Cint32, Cint, Csingle, Cdouble, Csymbol_address, Cstring,
    Cskip, Calign
  };
  K kind;
  std::string_view s;  // symbols, Cstring
  std::int64_t n = 0;  // Cint8 / Cint16 / Cint32 / Cint / Cskip / Calign
  double f = 0;        // Csingle / Cdouble
};
DataItem data_sym(DataItem::K k, std::string_view s);
DataItem data_int(DataItem::K k, std::int64_t n);
DataItem data_float(DataItem::K k, double f);

struct Phrase {  // Cfunction of fundecl | Cdata of data_item list
  const Fundecl* fn = nullptr;
  std::vector<DataItem> data;
};

bool iter_shallow_tail(const std::function<void(expression)>& f, expression e);
expression map_tail(const std::function<expression(expression)>& f, expression e);
expression map_shallow(const std::function<expression(expression)>& f, expression e);

void reset();

}  // namespace cppcaml::typing::cmm
