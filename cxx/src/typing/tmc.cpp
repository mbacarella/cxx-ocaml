// Port of lambda/tmc.ml (cxx/PORTING.md stage 10).  See tmc.hpp.
//
// Ident stamps (Ident.create_local in declare_binding, Constr.with_placeholder,
// Constr.delay_impure and make_dps_variant, and duplicate_function's
// freshening) depend on the order code is produced, so it follows OCaml's:
// tuples, constructors, records and applications right to left, List.map and
// `let+ .. and+ ..` (translcore's transl_letop) left to right.
//
// Choice.t / Dps.t are only ever instantiated at [lambda] and at nests of
// pairs, lists and options of it (the `let+ .. and+ ..` of choice, and
// Choice.list).  Such a nest is represented flattened: `combine` over the
// component choices.  Pair evaluates its second component first, so a nest
// produces its components' code last to first, sums their delayed use
// counts and concatenates their tmc_calls in order, as `combine` does.
#include "cppcaml/typing/tmc.hpp"
#include "cppcaml/typing/location.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cppcaml::typing::tmc {

using Lam = typing::lambda::lambda;
using namespace typing::lambda;
using typing::lambda::ValueKind;
using PK = Primitive::K;

namespace {

// ---- destinations ------------------------------------------------------------
// offset destination (the offset as a lambda)
struct Dst {
  Ident::t var;
  Lam offset;
  ScopedLocation loc;
};

Slice<Lam> add_dst_args(const Dst& d, Slice<Lam> args) {
  std::vector<Lam> out;
  out.push_back(lvar(d.var));
  out.push_back(d.offset);
  out.insert(out.end(), args.begin(), args.end());
  return slice(out);
}

Lam assign_to_dst(const Dst& d, Lam lam) {
  Primitive p = prim(PK::Psetfield_computed);
  p.ptr = ImmediateOrPointer::Pointer;
  p.init = InitializationOrAssignment::Heap_initialization;
  return lprim(p, slice({lvar(d.var), d.offset, lam}), d.loc);
}

// ---- Constr --------------------------------------------------------------------
struct Constr {
  long tag;
  MutableFlag flag;
  BlockShape shape;
  std::vector<Lam> before;
  std::vector<Lam> after;
  ScopedLocation loc;
};

Lam constr_apply(const Constr& c, Lam t) {
  std::vector<Lam> block_args(c.before);
  block_args.push_back(t);
  block_args.insert(block_args.end(), c.after.begin(), c.after.end());
  Primitive p = prim(PK::Pmakeblock);
  p.n = c.tag;
  p.mut = c.flag;
  p.shape = c.shape;
  return lprim(p, slice(block_args), c.loc);
}

Lam with_placeholder(const Constr& constr, const std::function<Lam(const Dst&)>& body) {
  Constr mc = constr;
  mc.flag = MutableFlag::Mutable;
  Lam k_with_placeholder = constr_apply(mc, dummy_constant());
  long placeholder_pos = static_cast<long>(constr.before.size());
  Lam placeholder_pos_lam = lconst(const_int(placeholder_pos));
  Ident::t block_var = Ident::create_local(OCAML_LIT("block"));
  Lam b = body(Dst{block_var, placeholder_pos_lam, constr.loc});
  return llet(LetKind::Strict, ValueKind::gen(), block_var, k_with_placeholder, b);
}

Lam bind_list(long block_id, long arg_offset, const std::vector<Lam>& lambdas,
              const std::function<Lam(std::vector<Lam>)>& k) {
  // Note that the delayed subterms will be used exactly once in the
  // linear-static subterm.  So we are happy to delay constants, which we
  // would not want to duplicate.
  auto can_be_delayed = [](Lam l) { return l->kind == LK::Lvar || l->kind == LK::Lconst; };
  std::vector<std::pair<Ident::t, Lam>> bindings;  // (nullptr, _) = None
  std::vector<Lam> args;
  long i = 0;
  for (Lam lam : lambdas) {  // List.mapi: left to right
    if (can_be_delayed(lam)) {
      bindings.push_back({nullptr, nullptr});
      args.push_back(lam);
    } else {
      Ident::t v = Ident::create_local("block" + std::to_string(block_id) + "_arg" + std::to_string(arg_offset + i));
      bindings.push_back({v, lam});
      args.push_back(lvar(v));
    }
    ++i;
  }
  Lam body = k(std::move(args));
  for (std::size_t j = bindings.size(); j-- > 0;)
    if (bindings[j].first) body = llet(LetKind::Strict, ValueKind::gen(), bindings[j].first, bindings[j].second, body);
  return body;
}

Lam delay_impure(long block_id, const Constr& constr, const std::function<Lam(const Constr&)>& body) {
  return bind_list(block_id, 0, constr.before, [&](std::vector<Lam> vbefore) {
    long arg_offset = static_cast<long>(constr.before.size()) + 1;
    return bind_list(block_id, arg_offset, constr.after, [&](std::vector<Lam> vafter) {
      Constr c = constr;
      c.before = std::move(vbefore);
      c.after = std::move(vafter);
      return body(c);
    });
  });
}

// ---- Dps -----------------------------------------------------------------------
using Delayed = std::vector<Constr>;  // a list: front = head

struct Dps {
  std::function<Lam(const Delayed& delayed, bool tail, const Dst& dst)> code;
  int delayed_use_count;
};
using DpsP = std::shared_ptr<const Dps>;
using DpsFn = std::function<Lam(bool tail, const Dst& dst)>;

Lam write_to_dst(const Dst& dst, const Delayed& delayed, std::size_t from, Lam t) {
  for (std::size_t k = from; k < delayed.size(); ++k) t = constr_apply(delayed[k], t);
  return assign_to_dst(dst, t);
}

DpsP dps_lambda(Lam v) {
  return std::make_shared<Dps>(
      Dps{[v](const Delayed& delayed, bool, const Dst& dst) { return write_to_dst(dst, delayed, 0, v); }, 1});
}

Lam dps_run(const DpsP& d, bool tail, const Dst& dst) { return d->code(Delayed{}, tail, dst); }

DpsP reify_delay(DpsFn dps) {
  return std::make_shared<Dps>(Dps{[dps](const Delayed& delayed, bool tail, const Dst& dst) -> Lam {
                                     if (delayed.empty()) return dps(tail, dst);
                                     return with_placeholder(delayed[0], [&](const Dst& new_dst) {
                                       Lam b = dps(tail, new_dst);
                                       Lam a = write_to_dst(dst, delayed, 1, lvar(new_dst.var));
                                       return lsequence(a, b);
                                     });
                                   },
                                   1});
}

DpsP ensures_affine(const DpsP& d) {
  if (d->delayed_use_count <= 1) return d;
  return reify_delay([d](bool tail, const Dst& dst) { return dps_run(d, tail, dst); });
}

DpsP dps_make(DpsFn dps) { return reify_delay(std::move(dps)); }

DpsP delay_constructor(const Constr& constr, const DpsP& d0) {
  DpsP d = ensures_affine(d0);
  return std::make_shared<Dps>(Dps{[constr, d](const Delayed& delayed, bool tail, const Dst& dst) {
                                     long block_id = static_cast<long>(delayed.size());
                                     return delay_impure(block_id, constr, [&](const Constr& c) {
                                       Delayed nd;
                                       nd.reserve(delayed.size() + 1);
                                       nd.push_back(c);
                                       nd.insert(nd.end(), delayed.begin(), delayed.end());
                                       return d->code(nd, tail, dst);
                                     });
                                   },
                                   d->delayed_use_count});
}

// ---- Choice --------------------------------------------------------------------
struct Choice {
  DpsP dps;
  std::function<Lam()> direct;
  std::vector<TmcCallInformation> tmc_calls;
  bool benefits_from_dps;
  bool explicit_tailcall_request;
};
using ChoiceP = std::shared_ptr<const Choice>;

ChoiceP choice_lambda(Lam v) {
  return std::make_shared<Choice>(Choice{dps_lambda(v), [v] { return v; }, {}, false, false});
}

Lam choice_direct(const ChoiceP& c) { return c->direct(); }
Lam choice_dps(const ChoiceP& c, bool tail, const Dst& dst) { return dps_run(c->dps, tail, dst); }

// A nest of Choice.pair / Choice.list / Choice.option over [cs] (nullptr =
// None), mapped by [build] (see the header comment).
ChoiceP combine(std::vector<ChoiceP> cs, std::function<Lam(const std::vector<Lam>&)> build) {
  int count = 0;
  std::vector<TmcCallInformation> calls;
  bool benefits = false, expl = false;
  for (auto& c : cs)
    if (c) {
      count += c->dps->delayed_use_count;
      calls.insert(calls.end(), c->tmc_calls.begin(), c->tmc_calls.end());
      benefits = benefits || c->benefits_from_dps;
      expl = expl || c->explicit_tailcall_request;
    }
  auto dps = std::make_shared<Dps>(Dps{[cs, build](const Delayed& delayed, bool tail, const Dst& dst) {
                                         std::vector<Lam> r(cs.size(), nullptr);
                                         for (std::size_t k = cs.size(); k-- > 0;)
                                           if (cs[k]) r[k] = cs[k]->dps->code(delayed, tail, dst);
                                         return build(r);
                                       },
                                       count});
  auto direct = [cs, build] {
    std::vector<Lam> r(cs.size(), nullptr);
    for (std::size_t k = cs.size(); k-- > 0;)
      if (cs[k]) r[k] = cs[k]->direct();
    return build(r);
  };
  return std::make_shared<Choice>(Choice{dps, direct, std::move(calls), benefits, expl});
}

ChoiceP choice_map(const ChoiceP& c, std::function<Lam(Lam)> f) {
  return combine({c}, [f](const std::vector<Lam>& r) { return f(r[0]); });
}

// ---- the transformation --------------------------------------------------------
struct Specialized {
  long arity;
  Ident::t dps_id;
  FunctionKind direct_kind;
};
using Context = IdentMap<Specialized>;  // context.specialized

using Bindings = std::vector<std::pair<Ident::t, Lam>>;

Lam llets(LetKind lk, ValueKind vk, const Bindings& bindings, Lam body) {
  for (std::size_t k = bindings.size(); k-- > 0;) body = llet(lk, vk, bindings[k].first, bindings[k].second, body);
  return body;
}

const LFunction* find_candidate(Lam l) {
  if (auto* f = as<Lfunction>(l); f && f->f->attr.tmc_candidate) return f->f;
  return nullptr;
}

Context declare_binding(const Context& ctx, Ident::t var, Lam def) {
  const LFunction* lfun = find_candidate(def);
  if (!lfun) return ctx;
  long arity = static_cast<long>(lfun->params.size());
  Ident::t dps_id = Ident::create_local(std::string(ident::name(var)) + "_dps");
  Context c = ctx;
  c.insert_or_assign(var, Specialized{arity, dps_id, lfun->kind});
  return c;
}

Lam traverse(const Context& ctx, Lam t);
std::pair<Context, Bindings> traverse_let(const Context& outer_ctx, Ident::t var, Lam def);
std::pair<Context, std::vector<RecBinding>> traverse_letrec(const Context& ctx, Slice<RecBinding> bindings);
Slice<Lam> traverse_list(const Context& ctx, Slice<Lam> terms);

ChoiceP choice(const Context& ctx, bool tail, Lam t);

struct NoTmc {};

ChoiceP choice_apply(const Context& ctx, bool tail, const Lapply* node) {
  const LambdaApply& apply = node->ap;
  try {
    bool explicit_tailcall_request;
    switch (apply.ap_tailcall) {
      case TailcallAttribute::Default_tailcall: explicit_tailcall_request = false; break;
      case TailcallAttribute::Tailcall_expectation_true: explicit_tailcall_request = true; break;
      default: throw NoTmc{};
    }
    auto* f = as<Lvar>(apply.ap_func);
    if (!f) throw NoTmc{};
    auto it = ctx.find(f->id);
    if (it == ctx.end()) {
      if (tail)
        location::prerr_warning(to_location(apply.ap_loc),
                                warnings::Warning::make(warnings::Warning::K::Tmc_breaks_tailcall));
      throw NoTmc{};
    }
    Specialized specialized = it->second;
    // Support of tupled functions: the [function_kind] of the direct-style
    // function is identical to the one of the input function, which may be
    // Tupled, but the dps function is always Curried.
    auto exact = find_exact_application(specialized.direct_kind, specialized.arity, apply.ap_args);
    if (!exact) throw NoTmc{};
    Slice<Lam> args = *exact;
    // If we are calling a tmc-specializable function in tail context, then
    // both the direct-style and dps-style calls must be tailcalls.
    auto tailcall = [](bool tl) {
      return tl ? TailcallAttribute::Tailcall_expectation_true : TailcallAttribute::Default_tailcall;
    };
    DpsP dps = dps_make([apply, specialized, args, tailcall](bool tl, const Dst& dst) {
      LambdaApply ap = apply;
      ap.ap_tailcall = tailcall(tl);
      ap.ap_args = add_dst_args(dst, args);
      ap.ap_func = lvar(specialized.dps_id);
      return lapply(ap);
    });
    auto direct = [apply, tail, tailcall] {
      LambdaApply ap = apply;
      ap.ap_tailcall = tailcall(tail);
      return lapply(ap);
    };
    return std::make_shared<Choice>(Choice{dps,
                                           direct,
                                           {TmcCallInformation{apply.ap_loc, explicit_tailcall_request}},
                                           true,
                                           explicit_tailcall_request});
  } catch (const NoTmc&) {
    // [@tailcall false] is interpreted as a bailout annotation: "we are
    // (knowingly) leaving the dps calling convention".  It only has sense in
    // the DPS version of the generated code, not in direct style.
    LambdaApply no_bailout = apply;
    if (apply.ap_tailcall == TailcallAttribute::Tailcall_expectation_false && tail)
      no_bailout.ap_tailcall = TailcallAttribute::Default_tailcall;
    ChoiceP base = choice_lambda(lapply(apply));
    Choice c = *base;
    c.direct = [no_bailout] { return lapply(no_bailout); };
    return std::make_shared<Choice>(std::move(c));
  }
}

ChoiceP choice_makeblock(const Context& ctx, const Primitive& p, Slice<Lam> blockargs, const ScopedLocation& loc) {
  std::vector<ChoiceP> choices;
  for (Lam a : blockargs) choices.push_back(choice(ctx, false, a));  // List.map
  auto has_tmc_calls = [](const ChoiceP& c) { return !c->tmc_calls.empty(); };
  auto is_explicit = [](const ChoiceP& c) { return c->explicit_tailcall_request; };
  auto makeblock = [p, loc](const std::vector<Lam>& args) { return lprim(p, slice(args), loc); };

  // Choice.find_nonambiguous_tmc_call
  std::vector<ChoiceP> tmc_call_subterms;
  for (auto& c : choices)
    if (has_tmc_calls(c)) tmc_call_subterms.push_back(c);
  auto nonambiguous = [&](bool only_explicit_calls) -> ChoiceP {
    std::vector<Lam> before;
    std::size_t k = 0;
    for (;; ++k) {
      if (k == choices.size()) throw std::logic_error("Tmc.find_nonambiguous_tmc_call");
      auto& c = choices[k];
      if (has_tmc_calls(c) && (!only_explicit_calls || is_explicit(c))) break;
      before.push_back(choice_direct(c));
    }
    ChoiceP ch = choices[k];
    std::vector<Lam> after;
    for (std::size_t j = k + 1; j < choices.size(); ++j) after.push_back(choice_direct(choices[j]));
    Constr constr{p.n, p.mut, p.shape, std::move(before), std::move(after), loc};
    if (ch->tmc_calls.empty()) throw std::logic_error("Tmc.choice_makeblock");
    auto direct = [constr, ch] {
      if (!ch->benefits_from_dps) return constr_apply(constr, choice_direct(ch));
      return with_placeholder(constr, [&](const Dst& new_dst) {
        Lam v = lvar(new_dst.var);
        return lsequence(choice_dps(ch, false, new_dst), v);
      });
    };
    // Whether or not the caller provides a destination, we can always
    // provide a destination to our settable subterm, so the number of TMC
    // sub-calls is identical in the [direct] and [dps] versions.
    DpsP dps = delay_constructor(constr, ch->dps);
    return std::make_shared<Choice>(Choice{dps, direct, ch->tmc_calls, false, ch->explicit_tailcall_request});
  };
  auto ambiguous = [&](bool expl, std::vector<ChoiceP> subterms) -> ChoiceP {
    // An ambiguous term should not lead to an error if it not used in TMC
    // position: only the [dps] version fails.
    ChoiceP term_choice = combine(choices, makeblock);
    Choice c = *term_choice;
    Location eloc = debuginfo::to_location(loc);
    c.dps = dps_make([eloc, expl, subterms](bool, const Dst&) -> Lam {
      Error e{eloc, expl, {}};
      for (auto& t : subterms) e.arguments.push_back(t->tmc_calls);
      throw e;
    });
    return std::make_shared<Choice>(std::move(c));
  };
  if (tmc_call_subterms.empty()) {
    std::vector<Lam> args;
    for (auto& c : choices) args.push_back(choice_direct(c));
    return choice_lambda(makeblock(args));
  }
  if (tmc_call_subterms.size() == 1) return nonambiguous(false);
  std::vector<ChoiceP> explicit_subterms;
  for (auto& c : tmc_call_subterms)
    if (is_explicit(c)) explicit_subterms.push_back(c);
  if (explicit_subterms.empty()) return ambiguous(false, tmc_call_subterms);
  if (explicit_subterms.size() == 1) return nonambiguous(true);
  return ambiguous(true, explicit_subterms);
}

ChoiceP choice_prim(const Context& ctx, bool tail, const Lprim* node) {
  const Primitive& p = node->p;
  switch (p.kind) {
    // The important case is the construction case
    case PK::Pmakeblock: return choice_makeblock(ctx, p, node->args, node->loc);
    // Some primitives have arguments in tail-position
    case PK::Popaque: {
      if (node->args.size() != 1) throw std::invalid_argument("choice_prim");
      ScopedLocation loc = node->loc;
      return choice_map(choice(ctx, tail, node->args[0]),
                        [loc](Lam l1) { return lprim(prim(PK::Popaque), slice({l1}), loc); });
    }
    // in common cases we just return
    default: return choice_lambda(lprim(p, traverse_list(ctx, node->args), node->loc));
  }
}

ChoiceP choice(const Context& ctx, bool tail, Lam t) {
  switch (t->kind) {
    case LK::Lvar:
    case LK::Lmutvar:
    case LK::Lconst:
    case LK::Lfunction:
    case LK::Lsend:
    case LK::Lassign:
    case LK::Lfor:
    case LK::Lwhile: return choice_lambda(traverse(ctx, t));
    // [choice_prim] handles most primitives, but the important case of
    // construction [Lprim(Pmakeblock(...), ...)] is handled by
    // [choice_makeblock]
    case LK::Lprim: return choice_prim(ctx, tail, as<Lprim>(t));
    // [choice_apply] handles applications, in particular tail-calls which
    // generate Set choices at the leaves
    case LK::Lapply: return choice_apply(ctx, tail, as<Lapply>(t));
    case LK::Lsequence: {
      auto* s = as<Lsequence>(t);
      Lam l1 = traverse(ctx, s->l1);
      return choice_map(choice(ctx, tail, s->l2), [l1](Lam l2) { return lsequence(l1, l2); });
    }
    case LK::Lifthenelse: {
      auto* i = as<Lifthenelse>(t);
      Lam l1 = traverse(ctx, i->cond);
      // choice_pair: Choice.pair (choice t1, choice t2), a tuple: t2 first
      ChoiceP c3 = choice(ctx, tail, i->ifnot);
      ChoiceP c2 = choice(ctx, tail, i->ifso);
      return combine({c2, c3}, [l1](const std::vector<Lam>& r) { return lifthenelse(l1, r[0], r[1]); });
    }
    case LK::Lmutlet: {
      // mutable bindings are not TMC-specialized
      auto* x = as<Lmutlet>(t);
      Lam def = traverse(ctx, x->arg);
      ValueKind vk = x->k;
      Ident::t var = x->id;
      return choice_map(choice(ctx, tail, x->body), [vk, var, def](Lam body) { return lmutlet(vk, var, def, body); });
    }
    case LK::Llet: {
      auto* x = as<Llet>(t);
      auto [ctx2, bindings] = traverse_let(ctx, x->id, x->arg);
      LetKind lk = x->str;
      ValueKind vk = x->k;
      return choice_map(choice(ctx2, tail, x->body),
                        [lk, vk, bindings](Lam body) { return llets(lk, vk, bindings, body); });
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(t);
      auto [ctx2, bindings] = traverse_letrec(ctx, x->decl);
      Slice<RecBinding> bs = slice(bindings);
      return choice_map(choice(ctx2, tail, x->body), [bs](Lam body) { return lletrec(bs, body); });
    }
    case LK::Lswitch: {
      auto* x = as<Lswitch>(t);
      Lam l1 = traverse(ctx, x->arg);
      // let+ consts_rhs = .. and+ blocks_rhs = .. and+ sw_failaction = ..:
      // built left to right
      std::vector<ChoiceP> cs;
      for (auto& c : x->sw.sw_consts) cs.push_back(choice(ctx, tail, c.action));
      for (auto& c : x->sw.sw_blocks) cs.push_back(choice(ctx, tail, c.action));
      cs.push_back(x->sw.sw_failaction ? choice(ctx, tail, x->sw.sw_failaction) : nullptr);
      LambdaSwitch sw0 = x->sw;
      ScopedLocation loc = x->loc;
      return combine(cs, [l1, sw0, loc](const std::vector<Lam>& r) {
        LambdaSwitch sw = sw0;
        std::size_t k = 0;
        std::vector<SwitchCase> consts, blocks;
        for (auto& c : sw0.sw_consts) consts.push_back({c.key, r[k++]});
        for (auto& c : sw0.sw_blocks) blocks.push_back({c.key, r[k++]});
        sw.sw_consts = slice(consts);
        sw.sw_blocks = slice(blocks);
        sw.sw_failaction = r[k];
        return lswitch(l1, sw, loc);
      });
    }
    case LK::Lstringswitch: {
      auto* x = as<Lstringswitch>(t);
      Lam l1 = traverse(ctx, x->arg);
      std::vector<ChoiceP> cs;
      for (auto& c : x->cases) cs.push_back(choice(ctx, tail, c.action));
      cs.push_back(x->def ? choice(ctx, tail, x->def) : nullptr);
      Slice<StringCase> cases0 = x->cases;
      ScopedLocation loc = x->loc;
      return combine(cs, [l1, cases0, loc](const std::vector<Lam>& r) {
        std::vector<StringCase> cases;
        std::size_t k = 0;
        for (auto& c : cases0) cases.push_back({c.s, r[k++]});
        return lstringswitch(l1, slice(cases), r[k], loc);
      });
    }
    case LK::Lstaticraise: {
      auto* r = as<Lstaticraise>(t);
      return choice_lambda(lstaticraise(r->i, traverse_list(ctx, r->args)));
    }
    case LK::Ltrywith: {
      // in [try l1 with id -> l2], the term [l1] is not in tail-call
      // position (after it returns we need to remove the exception handler)
      auto* x = as<Ltrywith>(t);
      ChoiceP c1 = choice(ctx, false, x->body);
      ChoiceP c2 = choice(ctx, tail, x->handler);
      Ident::t id = x->exn;
      return combine({c1, c2}, [id](const std::vector<Lam>& r) { return ltrywith(r[0], id, r[1]); });
    }
    case LK::Lstaticcatch: {
      // In [static-catch l1 with ids -> l2], the term [l1] is in fact in
      // tail-position
      auto* x = as<Lstaticcatch>(t);
      ChoiceP c1 = choice(ctx, tail, x->body);
      ChoiceP c2 = choice(ctx, tail, x->handler);
      long i = x->i;
      Slice<Param> ids = x->params;
      return combine({c1, c2}, [i, ids](const std::vector<Lam>& r) { return lstaticcatch(r[0], i, ids, r[1]); });
    }
    case LK::Levent: {
      auto* e = as<Levent>(t);
      const LambdaEvent* ev = e->ev;
      return choice_map(choice(ctx, tail, e->l), [ev](Lam lam) { return levent(lam, ev); });
    }
    case LK::Lifused: {
      auto* u = as<Lifused>(t);
      Ident::t x = u->id;
      return choice_map(choice(ctx, tail, u->l), [x](Lam lam) { return lifused(x, lam); });
    }
  }
  throw std::logic_error("Tmc.choice");
}

Lam traverse(const Context& ctx, Lam t) {
  if (auto* x = as<Llet>(t)) {
    auto [ctx2, bindings] = traverse_let(ctx, x->id, x->arg);
    Lam body = traverse(ctx2, x->body);
    return llets(x->str, x->k, bindings, body);
  }
  if (auto* x = as<Lletrec>(t)) {
    auto [ctx2, bindings] = traverse_letrec(ctx, x->decl);
    return lletrec(slice(bindings), traverse(ctx2, x->body));
  }
  return shallow_map([&ctx](Lam l) { return traverse(ctx, l); }, t);
}

const LFunction* traverse_lfunction(const Context& ctx, const LFunction* lfun) {
  return map_lfunction([&ctx](Lam l) { return traverse(ctx, l); }, lfun);
}

std::vector<std::pair<Ident::t, const LFunction*>> make_dps_variant(Ident::t var, const Context& inner_ctx,
                                                                   const Context& outer_ctx, const LFunction* lfun) {
  Specialized special = inner_ctx.at(var);
  ChoiceP fun_choice = choice(outer_ctx, true, lfun->body);
  if (fun_choice->tmc_calls.empty())
    location::prerr_warning(to_location(lfun->loc), warnings::Warning::make(warnings::Warning::K::Unused_tmc_attribute));
  const LFunction* direct =
      lfunction_(lfun->kind, lfun->params, lfun->return_, choice_direct(fun_choice), lfun->attr, lfun->loc);
  // { var = create_local "dst"; offset = create_local "offset"; loc }: a
  // record, right to left
  Ident::t offset_id = Ident::create_local(OCAML_LIT("offset"));
  Ident::t dst_var = Ident::create_local(OCAML_LIT("dst"));
  Dst dst{dst_var, lvar(offset_id), lfun->loc};
  // lfunction' ~kind ~params ~return ~body ~attr ~loc: only ~body has
  // effects
  Lam body = choice_dps(fun_choice, true, dst);
  std::vector<Param> params;
  params.push_back({dst_var, ValueKind::gen()});
  params.push_back({offset_id, ValueKind::intval()});
  params.insert(params.end(), lfun->params.begin(), lfun->params.end());
  const LFunction* dps = duplicate_function(
      lfunction_(FunctionKind::Curried, slice(params), lfun->return_, body, lfun->attr, lfun->loc));
  return {{var, direct}, {special.dps_id, dps}};
}

std::pair<Context, Bindings> traverse_let(const Context& outer_ctx, Ident::t var, Lam def) {
  Context inner_ctx = declare_binding(outer_ctx, var, def);
  Bindings bindings;
  if (const LFunction* lfun = find_candidate(def)) {
    for (auto& [v, f] : make_dps_variant(var, inner_ctx, outer_ctx, lfun)) bindings.push_back({v, lfunction_node(f)});
  } else {
    bindings.push_back({var, traverse(outer_ctx, def)});
  }
  return {std::move(inner_ctx), std::move(bindings)};
}

std::pair<Context, std::vector<RecBinding>> traverse_letrec(const Context& ctx0, Slice<RecBinding> bindings) {
  Context ctx = ctx0;
  for (auto& rb : bindings) ctx = declare_binding(ctx, rb.id, lfunction_node(rb.def));
  std::vector<RecBinding> out;
  for (auto& rb : bindings) {  // List.concat_map
    if (rb.def->attr.tmc_candidate) {
      for (auto& [id, def] : make_dps_variant(rb.id, ctx, ctx, rb.def)) out.push_back({id, def});
    } else {
      out.push_back({rb.id, traverse_lfunction(ctx, rb.def)});
    }
  }
  return {std::move(ctx), std::move(out)};
}

Slice<Lam> traverse_list(const Context& ctx, Slice<Lam> terms) {
  std::vector<Lam> out;
  for (Lam t : terms) out.push_back(traverse(ctx, t));
  return slice(out);
}

}  // namespace

Lam rewrite(Lam t) {
  Context ctx;
  return traverse(ctx, t);
}

}  // namespace cppcaml::typing::tmc
