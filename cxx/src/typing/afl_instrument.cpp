// Port of asmcomp/afl_instrument.ml.  See afl_instrument.hpp.
//
// The locations are drawn from the stdlib's default Random state (the
// one [Random.int] uses, never self-initialized by ocamlopt), so the port
// carries that PRNG: Random.State's LXM generator (runtime/prng.c) from
// [mk_default]'s state, and [State.int]'s rejection sampling.  One state
// per process, as ocamlopt's: it runs on across the files of one
// invocation.  OCaml evaluates a constructor's arguments right to left,
// and both Random and Backend_var.create_local have effects: "right to
// left" marks the places.
#include "cppcaml/typing/afl_instrument.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::afl_instrument {

using namespace cmm;
using OK = Operation::K;
using MC = MemoryChunk;
using Dbg = debuginfo::t;

namespace {

// stdlib/random.ml: the default state ([State.make [| 314159265 |]]),
// runtime/prng.c's caml_lxm_next
struct Lxm {
  std::uint64_t a = static_cast<std::uint64_t>(-6196874289567705097LL) | 1;  // set: must be odd
  std::uint64_t s = 586573249833713189ULL;
  std::uint64_t x0 = static_cast<std::uint64_t>(-8591268803865043407LL);
  std::uint64_t x1 = 6388613595849772044ULL;

  static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

  std::uint64_t next() {
    std::uint64_t z = s + x0;
    z = (z ^ (z >> 32)) * 0xdaba0b6eb09322e3ULL;
    z = (z ^ (z >> 32)) * 0xdaba0b6eb09322e3ULL;
    z = (z ^ (z >> 32));
    s = s * 0xd1342543de82ef95ULL + a;
    std::uint64_t q0 = x0, q1 = x1;
    q1 ^= q0;
    q0 = rotl(q0, 24);
    q0 = q0 ^ q1 ^ (q1 << 16);
    q1 = rotl(q1, 37);
    x0 = q0;
    x1 = q1;
    return z;
  }
};

Lxm& default_state() {
  static Lxm st;
  return st;
}

// Random.int bound = State.int_aux s bound max_int31
long random_int(long n) {
  constexpr long mask = 0x3FFFFFFF;  // max_int31
  if (n > mask || n <= 0) throw std::invalid_argument("Random.int");
  for (;;) {
    // Int64.to_int (next s) land mask: the low 30 bits
    long r = static_cast<long>(default_state().next() & static_cast<std::uint64_t>(mask));
    long v = r % n;
    if (!(r - v > mask - n + 1)) return v;
  }
}

expression afl_area_ptr(const Dbg& dbg) { return cconst_symbol("caml_afl_area_ptr", dbg); }
expression afl_prev_loc(const Dbg& dbg) { return cconst_symbol("caml_afl_prev_loc", dbg); }
constexpr long afl_map_size = 1L << 16;

VarWithProvenance vp(Var v) { return {v, nullptr}; }

expression instrument(expression e);

expression with_afl_logging(expression b, const Dbg& dbg) {
  if (clflags::afl_inst_ratio < 100 && random_int(100) >= clflags::afl_inst_ratio) return instrument(b);
  // The instrumentation that afl-fuzz requires is:
  //   cur_location = <COMPILE_TIME_RANDOM>;
  //   shared_mem[cur_location ^ prev_location]++;
  //   prev_location = cur_location >> 1;
  long cur_location = random_int(afl_map_size);
  Var cur_pos = Ident::create_local("pos");
  Var afl_area = Ident::create_local("shared_mem");
  auto load = [](MC chunk) { return cload(chunk, MutableFlag::Mutable, false); };
  expression instrumentation = clet(
      vp(afl_area), cop(load(MC::Word_int), {afl_area_ptr(dbg)}, dbg),
      clet(vp(cur_pos),
           cop(op(OK::Cxor), {cop(load(MC::Word_int), {afl_prev_loc(dbg)}, dbg), cconst_int(cur_location, dbg)},
               dbg),
           csequence(cop(cstore(MC::Byte_unsigned, lambda::InitializationOrAssignment::Assignment),
                         {cop(op(OK::Cadda), {cvar(afl_area), cvar(cur_pos)}, dbg),
                          cop(op(OK::Cadda),
                              {cop(load(MC::Byte_unsigned),
                                   {cop(op(OK::Cadda), {cvar(afl_area), cvar(cur_pos)}, dbg)}, dbg),
                               cconst_int(1, dbg)},
                              dbg)},
                         dbg),
                     cop(cstore(MC::Word_int, lambda::InitializationOrAssignment::Assignment),
                         {afl_prev_loc(dbg), cconst_int(cur_location >> 1, dbg)}, dbg))));
  return csequence(instrumentation, instrument(b));
}

Slice<expression> instrument_list(Slice<expression> es) {  // List.map instrument: left to right
  std::vector<expression> r;
  r.reserve(es.size());
  for (expression x : es) r.push_back(instrument(x));
  return slice(r);
}

expression instrument(expression e) {
  switch (e->kind) {
    // these cases add logging, as they may be targets of conditional branches
    case EK::Cifthenelse: {
      auto* x = static_cast<const Cifthenelse*>(e);
      // right to left
      expression ifnot = with_afl_logging(x->ifnot, x->ifnot_dbg);
      expression ifso = with_afl_logging(x->ifso, x->ifso_dbg);
      expression cond = instrument(x->cond);
      return cifthenelse(cond, x->ifso_dbg, ifso, x->ifnot_dbg, ifnot, x->dbg);
    }
    case EK::Ctrywith: {
      auto* x = static_cast<const Ctrywith*>(e);
      // right to left
      expression handler = with_afl_logging(x->handler, x->dbg);
      expression body = instrument(x->body);
      return ctrywith(body, x->exn, handler, x->dbg);
    }
    case EK::Cswitch: {
      auto* x = static_cast<const Cswitch*>(e);
      std::vector<SwitchCase> cases;  // Array.map: left to right
      cases.reserve(x->cases.size());
      for (auto& c : x->cases) cases.push_back({with_afl_logging(c.e, c.dbg), c.dbg});
      expression arg = instrument(x->e);
      return cswitch(arg, x->index, slice(cases), x->dbg);
    }
    // these cases add no logging, but instrument subexpressions
    case EK::Clet: {
      auto* x = static_cast<const Clet*>(e);
      expression body = instrument(x->body);  // right to left
      expression def = instrument(x->def);
      return clet(x->id, def, body);
    }
    case EK::Clet_mut: {
      auto* x = static_cast<const Clet_mut*>(e);
      expression body = instrument(x->body);  // right to left
      expression def = instrument(x->def);
      return clet_mut(x->id, x->ty, def, body);
    }
    case EK::Cphantom_let:
      throw std::logic_error("Afl_instrument: Cphantom_let (Closure makes none)");
    case EK::Cassign: {
      auto* x = static_cast<const Cassign*>(e);
      return cassign(x->id, instrument(x->e));
    }
    case EK::Ctuple: {
      auto* x = static_cast<const Ctuple*>(e);
      return ctuple(instrument_list(x->el));
    }
    case EK::Cop: {
      auto* x = static_cast<const Cop*>(e);
      return cop(x->op, instrument_list(x->args), x->dbg);
    }
    case EK::Csequence: {
      auto* x = static_cast<const Csequence*>(e);
      expression e2 = instrument(x->e2);  // right to left
      expression e1 = instrument(x->e1);
      return csequence(e1, e2);
    }
    case EK::Ccatch: {
      auto* x = static_cast<const Ccatch*>(e);
      std::vector<Handler> hs;  // the handlers first (a let), left to right
      hs.reserve(x->handlers.size());
      for (auto& h : x->handlers) hs.push_back({h.n, h.ids, instrument(h.body), h.dbg});
      expression body = instrument(x->body);
      return ccatch_node(x->rec, slice(hs), body);
    }
    case EK::Cexit: {
      auto* x = static_cast<const Cexit*>(e);
      return cexit(x->n, instrument_list(x->args));
    }
    // these are base cases and have no logging
    case EK::Cconst_int:
    case EK::Cconst_natint:
    case EK::Cconst_float:
    case EK::Cconst_symbol:
    case EK::Creturn_addr:
    case EK::Cvar:
    case EK::Cvar_mut: return e;
  }
  throw std::logic_error("Afl_instrument.instrument");
}

}  // namespace

expression instrument_function(expression c, const Dbg& dbg) { return with_afl_logging(c, dbg); }

expression instrument_initialiser(expression c, const std::function<Dbg()>& dbg) {
  // Each instrumented module calls caml_setup_afl at
  // initialisation, which is a no-op on the second and subsequent
  // calls.  (Right to left: the logging first.)
  expression logged = with_afl_logging(c, dbg());
  expression setup =
      cop(cextcall("caml_setup_afl", typ_int(), Slice<Exttype>{}, false), {cconst_int(0, dbg())}, dbg());
  return csequence(setup, logged);
}

}  // namespace cppcaml::typing::afl_instrument
