// Port of asmcomp/cmm.ml.  See cmm.hpp.
#include "cppcaml/typing/cmm.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace cppcaml::typing::cmm {

namespace {
using MC = MachtypeComponent;
Machtype one(MC c) {
  auto* p = static_cast<MC*>(zone().alloc(sizeof(MC), alignof(MC)));
  *p = c;
  return Machtype{p, 1};
}
[[noreturn]] void fatal(const std::string& s) { throw std::runtime_error(s); }
}  // namespace

Machtype typ_void() { return {}; }
Machtype typ_val() {
  static Machtype t = one(MC::Val);
  return t;
}
Machtype typ_addr() {
  static Machtype t = one(MC::Addr);
  return t;
}
Machtype typ_int() {
  static Machtype t = one(MC::Int);
  return t;
}
Machtype typ_float() {
  static Machtype t = one(MC::Float);
  return t;
}

// Machtype components are partially ordered as follows:
//
//     Addr     Float
//      ^
//      |
//     Val
//      ^
//      |
//     Int
MachtypeComponent lub_component(MC a, MC b) {
  if (a == MC::Float || b == MC::Float) {
    // Float unboxing code must be sure to avoid this case.
    if (a != b) fatal("Cmm.lub_component");
    return MC::Float;
  }
  if (a == MC::Addr || b == MC::Addr) return MC::Addr;
  if (a == MC::Val || b == MC::Val) return MC::Val;
  return MC::Int;
}
bool ge_component(MC a, MC b) {
  if (a == MC::Float || b == MC::Float) {
    if (a != b) fatal("Cmm.ge_component");
    return true;
  }
  switch (a) {
    case MC::Int: return b == MC::Int;
    case MC::Val: return b != MC::Addr;
    default: return true;  // Addr
  }
}

Machtype machtype_of_exttype(Exttype t) { return t == Exttype::XFloat ? typ_float() : typ_int(); }
Machtype machtype_of_exttype_list(Slice<Exttype> xtl) {
  std::vector<MC> v;
  for (Exttype t : xtl) v.push_back(t == Exttype::XFloat ? MC::Float : MC::Int);
  return slice(v);
}

// ---- labels ----------------------------------------------------------------------------------
namespace {
constexpr Label init_label = 99;
Label label_counter = init_label;
}  // namespace
void set_label(Label l) {
  if (l < label_counter)
    fatal("Cannot set label counter to " + std::to_string(l) + ", it must be >= " + std::to_string(label_counter));
  label_counter = l;
}
Label cur_label() { return label_counter; }
Label new_label() { return ++label_counter; }
void reset() { label_counter = init_label; }

// ---- operations ------------------------------------------------------------------------------
Operation capply(Machtype ty) {
  Operation o{Operation::K::Capply};
  o.ty = ty;
  return o;
}
Operation cextcall(std::string_view name, Machtype ty_res, Slice<Exttype> ty_args, bool alloc) {
  Operation o{Operation::K::Cextcall};
  o.name = name;
  o.ty = ty_res;
  o.ty_args = ty_args;
  o.alloc = alloc;
  return o;
}
Operation cload(MemoryChunk chunk, MutableFlag mut, bool is_atomic) {
  Operation o{Operation::K::Cload};
  o.chunk = chunk;
  o.mut = mut;
  o.is_atomic = is_atomic;
  return o;
}
Operation cstore(MemoryChunk chunk, lambda::InitializationOrAssignment init) {
  Operation o{Operation::K::Cstore};
  o.chunk = chunk;
  o.init = init;
  return o;
}
Operation ccmpi(IntegerComparison c) {
  Operation o{Operation::K::Ccmpi};
  o.icmp = c;
  return o;
}
Operation ccmpa(IntegerComparison c) {
  Operation o{Operation::K::Ccmpa};
  o.icmp = c;
  return o;
}
Operation ccmpf(FloatComparison c) {
  Operation o{Operation::K::Ccmpf};
  o.fcmp = c;
  return o;
}
Operation craise(lambda::RaiseKind k) {
  Operation o{Operation::K::Craise};
  o.raise = k;
  return o;
}

// ---- constructors ----------------------------------------------------------------------------
namespace {
template <class T>
T* node() {
  auto* n = make<T>();
  n->kind = T::K;
  return n;
}
}  // namespace
expression cconst_int(long n, const debuginfo::t& dbg) {
  auto* e = node<Cconst_int>();
  e->n = n;
  e->dbg = dbg;
  return e;
}
expression cconst_natint(std::int64_t n, const debuginfo::t& dbg) {
  auto* e = node<Cconst_natint>();
  e->n = n;
  e->dbg = dbg;
  return e;
}
expression cconst_float(double f, const debuginfo::t& dbg) {
  auto* e = node<Cconst_float>();
  e->f = f;
  e->dbg = dbg;
  return e;
}
expression cconst_symbol(std::string_view s, const debuginfo::t& dbg) {
  auto* e = node<Cconst_symbol>();
  e->s = s;
  e->dbg = dbg;
  return e;
}
expression cvar(Var id) {
  auto* e = node<Cvar>();
  e->id = id;
  return e;
}
expression cvar_mut(Var id) {
  auto* e = node<Cvar_mut>();
  e->id = id;
  return e;
}
expression clet(VarWithProvenance id, expression def, expression body) {
  auto* e = node<Clet>();
  e->id = id;
  e->def = def;
  e->body = body;
  return e;
}
expression clet_mut(VarWithProvenance id, Machtype ty, expression def, expression body) {
  auto* e = node<Clet_mut>();
  e->id = id;
  e->ty = ty;
  e->def = def;
  e->body = body;
  return e;
}
expression cassign(Var id, expression x) {
  auto* e = node<Cassign>();
  e->id = id;
  e->e = x;
  return e;
}
expression ctuple(Slice<expression> el) {
  auto* e = node<Ctuple>();
  e->el = el;
  return e;
}
expression cop(const Operation& o, Slice<expression> args, const debuginfo::t& dbg) {
  auto* e = node<Cop>();
  e->op = o;
  e->args = args;
  e->dbg = dbg;
  return e;
}
expression cop(const Operation& o, std::initializer_list<expression> args, const debuginfo::t& dbg) {
  return cop(o, slice(std::vector<expression>(args)), dbg);
}
expression csequence(expression e1, expression e2) {
  auto* e = node<Csequence>();
  e->e1 = e1;
  e->e2 = e2;
  return e;
}
expression cifthenelse(expression cond, const debuginfo::t& ifso_dbg, expression ifso, const debuginfo::t& ifnot_dbg,
                       expression ifnot, const debuginfo::t& dbg) {
  auto* e = node<Cifthenelse>();
  e->cond = cond;
  e->ifso_dbg = ifso_dbg;
  e->ifso = ifso;
  e->ifnot_dbg = ifnot_dbg;
  e->ifnot = ifnot;
  e->dbg = dbg;
  return e;
}
expression cswitch(expression x, Slice<long> index, Slice<SwitchCase> cases, const debuginfo::t& dbg) {
  auto* e = node<Cswitch>();
  e->e = x;
  e->index = index;
  e->cases = cases;
  e->dbg = dbg;
  return e;
}
expression ccatch_node(RecFlag rec, Slice<Handler> handlers, expression body) {
  auto* e = node<Ccatch>();
  e->rec = rec;
  e->handlers = handlers;
  e->body = body;
  return e;
}
expression cexit(long n, Slice<expression> args) {
  auto* e = node<Cexit>();
  e->n = n;
  e->args = args;
  return e;
}
expression ctrywith(expression body, VarWithProvenance exn, expression handler, const debuginfo::t& dbg) {
  auto* e = node<Ctrywith>();
  e->body = body;
  e->exn = exn;
  e->handler = handler;
  e->dbg = dbg;
  return e;
}
expression creturn_addr() { return node<Creturn_addr>(); }
expression ccatch(long i, Slice<CatchParam> ids, expression e1, expression e2, const debuginfo::t& dbg) {
  return ccatch_node(RecFlag::Nonrecursive, slice(std::vector<Handler>{{i, ids, e2, dbg}}), e1);
}

DataItem data_sym(DataItem::K k, std::string_view s) {
  DataItem d{k};
  d.s = s;
  return d;
}
DataItem data_int(DataItem::K k, std::int64_t n) {
  DataItem d{k};
  d.n = n;
  return d;
}
DataItem data_float(DataItem::K k, double f) {
  DataItem d{k};
  d.f = f;
  return d;
}

// ---- traversals ------------------------------------------------------------------------------
bool iter_shallow_tail(const std::function<void(expression)>& f, expression e) {
  switch (e->kind) {
    case EK::Clet: f(static_cast<const Clet*>(e)->body); return true;
    case EK::Cphantom_let: f(static_cast<const Cphantom_let*>(e)->body); return true;
    case EK::Clet_mut: f(static_cast<const Clet_mut*>(e)->body); return true;
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(e);
      f(x->ifso);
      f(x->ifnot);
      return true;
    }
    case EK::Csequence: f(static_cast<const Csequence*>(e)->e2); return true;
    case EK::Cswitch:
      for (auto& c : static_cast<const Cswitch*>(e)->cases) f(c.e);
      return true;
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(e);
      for (auto& h : x->handlers) f(h.body);
      f(x->body);
      return true;
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(e);
      f(x->body);
      f(x->handler);
      return true;
    }
    case EK::Cexit: return true;
    case EK::Cop: return static_cast<const Cop*>(e)->op.kind == Operation::K::Craise;
    default: return false;
  }
}

// map_tail and map_shallow build constructors: their arguments are
// evaluated right to left
expression map_tail(const std::function<expression(expression)>& f, expression e) {
  switch (e->kind) {
    case EK::Clet: {
      auto* x = static_cast<const Clet*>(e);
      return clet(x->id, x->def, map_tail(f, x->body));
    }
    case EK::Clet_mut: {
      auto* x = static_cast<const Clet_mut*>(e);
      return clet_mut(x->id, x->ty, x->def, map_tail(f, x->body));
    }
    case EK::Cphantom_let: {
      auto* x = static_cast<const Cphantom_let*>(e);
      auto* n = node<Cphantom_let>();
      n->id = x->id;
      n->body = map_tail(f, x->body);
      return n;
    }
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(e);
      expression ifnot = map_tail(f, x->ifnot);
      expression ifso = map_tail(f, x->ifso);
      return cifthenelse(x->cond, x->ifso_dbg, ifso, x->ifnot_dbg, ifnot, x->dbg);
    }
    case EK::Csequence: {
      auto* x = static_cast<const Csequence*>(e);
      return csequence(x->e1, map_tail(f, x->e2));
    }
    case EK::Cswitch: {
      auto* x = static_cast<const Cswitch*>(e);
      std::vector<SwitchCase> cases;  // Array.map: left to right
      for (auto& c : x->cases) cases.push_back({map_tail(f, c.e), c.dbg});
      return cswitch(x->e, x->index, slice(cases), x->dbg);
    }
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(e);
      expression body = map_tail(f, x->body);
      std::vector<Handler> hs;  // List.map: left to right
      for (auto& h : x->handlers) hs.push_back({h.n, h.ids, map_tail(f, h.body), h.dbg});
      return ccatch_node(x->rec, slice(hs), body);
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(e);
      expression handler = map_tail(f, x->handler);
      expression body = map_tail(f, x->body);
      return ctrywith(body, x->exn, handler, x->dbg);
    }
    case EK::Cexit: return e;
    case EK::Cop:
      if (static_cast<const Cop*>(e)->op.kind == Operation::K::Craise) return e;
      return f(e);
    default: return f(e);
  }
}

expression map_shallow(const std::function<expression(expression)>& f, expression e) {
  auto map_list = [&](Slice<expression> l) {
    std::vector<expression> r;  // List.map: left to right
    for (expression x : l) r.push_back(f(x));
    return slice(r);
  };
  switch (e->kind) {
    case EK::Clet: {
      auto* x = static_cast<const Clet*>(e);
      expression body = f(x->body);
      expression def = f(x->def);
      return clet(x->id, def, body);
    }
    case EK::Clet_mut: {
      auto* x = static_cast<const Clet_mut*>(e);
      expression body = f(x->body);
      expression def = f(x->def);
      return clet_mut(x->id, x->ty, def, body);
    }
    case EK::Cphantom_let: {
      auto* x = static_cast<const Cphantom_let*>(e);
      auto* n = node<Cphantom_let>();
      n->id = x->id;
      n->body = f(x->body);
      return n;
    }
    case EK::Cassign: {
      auto* x = static_cast<const Cassign*>(e);
      return cassign(x->id, f(x->e));
    }
    case EK::Ctuple: return ctuple(map_list(static_cast<const Ctuple*>(e)->el));
    case EK::Cop: {
      auto* x = static_cast<const Cop*>(e);
      return cop(x->op, map_list(x->args), x->dbg);
    }
    case EK::Csequence: {
      auto* x = static_cast<const Csequence*>(e);
      expression e2 = f(x->e2);
      expression e1 = f(x->e1);
      return csequence(e1, e2);
    }
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(e);
      expression ifnot = f(x->ifnot);
      expression ifso = f(x->ifso);
      expression cond = f(x->cond);
      return cifthenelse(cond, x->ifso_dbg, ifso, x->ifnot_dbg, ifnot, x->dbg);
    }
    case EK::Cswitch: {
      auto* x = static_cast<const Cswitch*>(e);
      std::vector<SwitchCase> cases;
      for (auto& c : x->cases) cases.push_back({f(c.e), c.dbg});
      return cswitch(x->e, x->index, slice(cases), x->dbg);
    }
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(e);
      expression body = f(x->body);
      std::vector<Handler> hs;
      for (auto& h : x->handlers) hs.push_back({h.n, h.ids, f(h.body), h.dbg});
      return ccatch_node(x->rec, slice(hs), body);
    }
    case EK::Cexit: {
      auto* x = static_cast<const Cexit*>(e);
      return cexit(x->n, map_list(x->args));
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(e);
      expression handler = f(x->handler);
      expression body = f(x->body);
      return ctrywith(body, x->exn, handler, x->dbg);
    }
    default: return e;
  }
}

}  // namespace cppcaml::typing::cmm
