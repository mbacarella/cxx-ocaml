// Port of middle_end/flambda/un_anf.ml (see un_anf.hpp).
//
// (Clflags.debug_full, which would leave phantom lets for the debugger, is
// set by no option: the phantom lets are never made.)
#include "cppcaml/typing/un_anf.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/convert_primitives.hpp"
#include "cppcaml/typing/lambda.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printclambda.hpp"

namespace cppcaml::typing::un_anf {

using namespace clambda;

namespace {

// V.Set: only searched (membership)
using VSet = std::unordered_set<Var>;

// We say that a [V.t] is "linear" iff: (a) it is used exactly once; (b) it
// is never assigned to (using [Uassign]).
struct VarInfo {
  VSet used_let_bound_vars;
  VSet linear_let_bound_vars;
  VSet assigned;
  VSet closure_environment;
};

Var closure_environment_var(const UFunction& f) {
  // The argument after the arity is the environment
  if (static_cast<long>(f.params.size()) == f.arity + 1) {
    Var env_var = f.params[static_cast<std::size_t>(f.arity)].var.var;
    if (ident::name(env_var) != "env") misc::fatal_error("Un_anf.closure_environment_var");  // (assert)
    return env_var;
  }
  return nullptr;  // closed function, no environment
}

enum class Uses : unsigned char { Zero, One, More_than_one, Assigned };
struct VarDesc {
  long definition_depth;
  Uses uses;
};

VarInfo make_var_info(ulambda clam) {
  std::unordered_map<Var, VarDesc> t;
  VSet environment_vars;
  auto add_use = [&](Var var, long depth) {
    auto it = t.find(var);
    if (it == t.end()) return;  // Variable is not let-bound
    VarDesc& d = it->second;
    if (!(d.definition_depth <= depth)) misc::fatal_error("Un_anf.incr_uses");  // (assert)
    switch (d.uses) {
      case Uses::Zero: d.uses = d.definition_depth < depth ? Uses::More_than_one : Uses::One; break;
      case Uses::One: d.uses = Uses::More_than_one; break;
      case Uses::More_than_one: case Uses::Assigned: break;
    }
  };
  std::function<void(ulambda, long)> loop = [&](ulambda l, long depth) {
    switch (l->kind) {
      case UK::Uvar: add_use(as<Uvar>(l)->id, depth); break;
      case UK::Uconst: break;
      case UK::Udirect_apply:
        for (ulambda a : as<Udirect_apply>(l)->args) loop(a, depth);
        break;
      case UK::Ugeneric_apply: {
        auto* x = as<Ugeneric_apply>(l);
        loop(x->f, depth);
        for (ulambda a : x->args) loop(a, depth);
        break;
      }
      case UK::Uclosure: {
        auto* x = as<Uclosure>(l);
        for (ulambda a : x->fv) loop(a, depth);
        for (const UFunction* f : x->funs) {
          if (Var env_var = closure_environment_var(*f)) environment_vars.insert(env_var);
          loop(f->body, depth + 1);
        }
        break;
      }
      case UK::Uoffset: loop(as<Uoffset>(l)->l, depth); break;
      case UK::Ulet: {
        auto* x = as<Ulet>(l);
        t[x->id.var] = VarDesc{depth, Uses::Zero};  // V.Tbl.add
        loop(x->arg, depth);
        loop(x->body, depth);
        break;
      }
      case UK::Uphantom_let: loop(as<Uphantom_let>(l)->body, depth); break;
      case UK::Uprim:
        for (ulambda a : as<Uprim>(l)->args) loop(a, depth);
        break;
      case UK::Uswitch: {
        auto* x = as<Uswitch>(l);
        loop(x->arg, depth);
        for (ulambda a : x->sw.us_actions_consts) loop(a, depth);
        for (ulambda a : x->sw.us_actions_blocks) loop(a, depth);
        break;
      }
      case UK::Ustringswitch: {
        auto* x = as<Ustringswitch>(l);
        loop(x->arg, depth);
        for (const UStringCase& c : x->cases) loop(c.action, depth);
        if (x->def) loop(x->def, depth);
        break;
      }
      case UK::Ustaticfail:
        for (ulambda a : as<Ustaticfail>(l)->args) loop(a, depth);
        break;
      case UK::Ucatch: {
        auto* x = as<Ucatch>(l);
        loop(x->body, depth);
        loop(x->handler, depth);
        break;
      }
      case UK::Utrywith: {
        auto* x = as<Utrywith>(l);
        loop(x->body, depth);
        loop(x->handler, depth);
        break;
      }
      case UK::Uifthenelse: {
        auto* x = as<Uifthenelse>(l);
        loop(x->cond, depth);
        loop(x->ifso, depth);
        loop(x->ifnot, depth);
        break;
      }
      case UK::Usequence: {
        auto* x = as<Usequence>(l);
        loop(x->l1, depth);
        loop(x->l2, depth);
        break;
      }
      case UK::Uwhile: {
        auto* x = as<Uwhile>(l);
        loop(x->cond, depth + 1);
        loop(x->body, depth + 1);
        break;
      }
      case UK::Ufor: {
        auto* x = as<Ufor>(l);
        loop(x->lo, depth);
        loop(x->hi, depth);
        loop(x->body, depth + 1);
        break;
      }
      case UK::Uassign: {
        auto* x = as<Uassign>(l);
        auto it = t.find(x->id);
        if (it == t.end()) {
          format::Formatter f;
          format::fprintf(f, "make_var_info: Assigned variable %a not let-bound",
                          [&](format::Formatter& g) { printclambda::print_var(g, x->id); });
          misc::fatal_error(f.contents());
        }
        it->second.uses = Uses::Assigned;
        loop(x->e, depth);
        break;
      }
      case UK::Usend: {
        auto* x = as<Usend>(l);
        loop(x->met, depth);
        loop(x->obj, depth);
        for (ulambda a : x->args) loop(a, depth);
        break;
      }
      case UK::Uunreachable: break;
    }
  };
  loop(clam, 0);
  VarInfo info;
  for (const auto& [var, desc] : t) {
    switch (desc.uses) {
      case Uses::Zero: break;
      case Uses::One:
        info.linear_let_bound_vars.insert(var);
        info.used_let_bound_vars.insert(var);
        break;
      case Uses::More_than_one: info.used_let_bound_vars.insert(var); break;
      case Uses::Assigned:
        info.used_let_bound_vars.insert(var);
        info.assigned.insert(var);
        break;
    }
  }
  info.closure_environment = std::move(environment_vars);
  return info;
}

// When sequences of [let]-bindings match the evaluation order in a
// subsequent primitive or function application whose arguments are
// linearly-used non-assigned variables bound by such lets (possibly
// interspersed with other variables that are known to be constant), and it
// is known that there were no intervening side-effects during the
// evaluation of the [let]-bindings, permit substitution of the variables
// for their defining expressions.  (un_anf.ml)
VSet let_bound_vars_that_can_be_moved(const VarInfo& var_info, ulambda clam) {
  VSet obviously_constant;
  VSet can_move;
  std::vector<Var> let_stack;  // (the list's head at the back)
  auto examine_argument_list = [&](const std::vector<ulambda>& args) {
    // Start at the most recent let binding and the leftmost argument (the
    // last argument to be evaluated).
    std::size_t k = 0;
    for (;;) {
      if (k == args.size()) return;  // the remaining let-bound vars stay on the stack
      if (let_stack.empty()) return;
      auto* v = as<Uvar>(args[k]);
      if (v && obviously_constant.count(v->id)) {
        ++k;
        continue;
      }
      if (v && ident::same(let_stack.back(), v->id) && !var_info.assigned.count(v->id)) {
        if (!var_info.used_let_bound_vars.count(v->id) || !var_info.linear_let_bound_vars.count(v->id))
          misc::fatal_error("Un_anf.let_bound_vars_that_can_be_moved");  // (assert)
        can_move.insert(v->id);
        let_stack.pop_back();
        ++k;
        continue;
      }
      // The [let] sequence has ceased to match the evaluation order or we
      // have encountered some complicated argument.
      let_stack.clear();
      return;
    }
  };
  auto vec = [](Slice<ulambda> s) { return std::vector<ulambda>(s.begin(), s.end()); };
  std::function<void(ulambda)> loop = [&](ulambda l) {
    switch (l->kind) {
      case UK::Uvar:
        if (var_info.assigned.count(as<Uvar>(l)->id)) let_stack.clear();
        break;
      case UK::Uconst: break;
      case UK::Udirect_apply: examine_argument_list(vec(as<Udirect_apply>(l)->args)); break;
      case UK::Ugeneric_apply: {
        auto* x = as<Ugeneric_apply>(l);
        std::vector<ulambda> args = vec(x->args);
        args.push_back(x->f);
        examine_argument_list(args);
        break;
      }
      case UK::Uclosure:
        // Start a new let stack for speed.
        for (const UFunction* f : as<Uclosure>(l)->funs) {
          let_stack.clear();
          loop(f->body);
          let_stack.clear();
        }
        break;
      case UK::Uoffset: examine_argument_list({as<Uoffset>(l)->l}); break;
      case UK::Ulet: {
        auto* x = as<Ulet>(l);
        Var var = x->id.var;
        if (as<Uconst>(x->arg)) {
          // The defining expression is obviously constant, so we don't have
          // to put this [let] on the stack, and we don't have to traverse
          // the defining expression either.
          obviously_constant.insert(var);
          loop(x->body);
        } else {
          loop(x->arg);
          if (var_info.linear_let_bound_vars.count(var)) let_stack.push_back(var);
          // If we encounter a non-linear [let]-binding then we must clear
          // the let stack, since we cannot now move any previous binding
          // across the non-linear one.
          else let_stack.clear();
          loop(x->body);
        }
        break;
      }
      case UK::Uphantom_let: loop(as<Uphantom_let>(l)->body); break;
      case UK::Uprim: examine_argument_list(vec(as<Uprim>(l)->args)); break;
      case UK::Uswitch: {
        auto* x = as<Uswitch>(l);
        examine_argument_list({x->arg});
        for (ulambda a : x->sw.us_actions_consts) {
          let_stack.clear();
          loop(a);
        }
        for (ulambda a : x->sw.us_actions_blocks) {
          let_stack.clear();
          loop(a);
        }
        let_stack.clear();
        break;
      }
      case UK::Ustringswitch: {
        auto* x = as<Ustringswitch>(l);
        examine_argument_list({x->arg});
        for (const UStringCase& c : x->cases) {
          let_stack.clear();
          loop(c.action);
        }
        let_stack.clear();
        if (x->def) loop(x->def);
        let_stack.clear();
        break;
      }
      case UK::Ustaticfail: examine_argument_list(vec(as<Ustaticfail>(l)->args)); break;
      case UK::Ucatch: {
        auto* x = as<Ucatch>(l);
        let_stack.clear();
        loop(x->body);
        let_stack.clear();
        loop(x->handler);
        let_stack.clear();
        break;
      }
      case UK::Utrywith: {
        auto* x = as<Utrywith>(l);
        let_stack.clear();
        loop(x->body);
        let_stack.clear();
        loop(x->handler);
        let_stack.clear();
        break;
      }
      case UK::Uifthenelse: {
        auto* x = as<Uifthenelse>(l);
        examine_argument_list({x->cond});
        let_stack.clear();
        loop(x->ifso);
        let_stack.clear();
        loop(x->ifnot);
        let_stack.clear();
        break;
      }
      case UK::Usequence: {
        auto* x = as<Usequence>(l);
        loop(x->l1);
        let_stack.clear();
        loop(x->l2);
        let_stack.clear();
        break;
      }
      case UK::Uwhile: {
        auto* x = as<Uwhile>(l);
        let_stack.clear();
        loop(x->cond);
        let_stack.clear();
        loop(x->body);
        let_stack.clear();
        break;
      }
      case UK::Ufor:
        let_stack.clear();
        loop(as<Ufor>(l)->body);
        let_stack.clear();
        break;
      case UK::Uassign: case UK::Usend: case UK::Uunreachable: let_stack.clear(); break;
    }
  };
  loop(clam);
  return can_move;
}

using Env = lambda::IdentPMap<ulambda>;

std::vector<ulambda> map_slice(Slice<ulambda> s, const std::function<ulambda(ulambda)>& f) {
  std::vector<ulambda> r;  // (List.map / Array.map: pure, any order)
  for (ulambda x : s) r.push_back(f(x));
  return r;
}

// Substitution of an expression for a let-moveable variable can cause the
// surrounding expression to become fixed.  To avoid confusion, do the
// let-moveable substitutions first.
ulambda substitute_let_moveable(const VSet& is_let_moveable, const Env& env, ulambda clam) {
  auto sub = [&](ulambda l) { return substitute_let_moveable(is_let_moveable, env, l); };
  auto subs = [&](Slice<ulambda> s) { return slice(map_slice(s, sub)); };
  switch (clam->kind) {
    case UK::Uvar: {
      Var var = as<Uvar>(clam)->id;
      if (!is_let_moveable.count(var)) return clam;
      if (const ulambda* d = env.find_opt(var)) return *d;
      format::Formatter f;
      format::fprintf(f, "substitute_let_moveable: Unbound variable %a",
                      [&](format::Formatter& g) { printclambda::print_var(g, var); });
      misc::fatal_error(f.contents());
    }
    case UK::Uconst: return clam;
    case UK::Udirect_apply: {
      auto* x = as<Udirect_apply>(clam);
      return udirect_apply(x->f, subs(x->args), x->dbg);
    }
    case UK::Ugeneric_apply: {
      auto* x = as<Ugeneric_apply>(clam);
      ulambda f = sub(x->f);
      return ugeneric_apply(f, subs(x->args), x->dbg);
    }
    case UK::Uclosure: {
      auto* x = as<Uclosure>(clam);
      std::vector<const UFunction*> funs;
      for (const UFunction* f : x->funs) {
        UFunction c = *f;
        c.body = sub(f->body);
        funs.push_back(make<UFunction>(c));
      }
      return uclosure(slice(funs), subs(x->fv));
    }
    case UK::Uoffset: {
      auto* x = as<Uoffset>(clam);
      return uoffset(sub(x->l), x->ofs);
    }
    case UK::Ulet: {
      auto* x = as<Ulet>(clam);
      ulambda def = sub(x->arg);
      if (is_let_moveable.count(x->id.var))
        return substitute_let_moveable(is_let_moveable, env.add(x->id.var, def), x->body);
      return ulet(x->mut, x->k, x->id, def, sub(x->body));
    }
    case UK::Uphantom_let: {
      auto* x = as<Uphantom_let>(clam);
      return uphantom_let(x->id, x->def, sub(x->body));
    }
    case UK::Uprim: {
      auto* x = as<Uprim>(clam);
      return uprim(x->p, subs(x->args), x->dbg);
    }
    case UK::Uswitch: {
      auto* x = as<Uswitch>(clam);
      ulambda cond = sub(x->arg);
      USwitch sw = x->sw;
      sw.us_actions_consts = subs(x->sw.us_actions_consts);
      sw.us_actions_blocks = subs(x->sw.us_actions_blocks);
      return uswitch(cond, sw, x->dbg);
    }
    case UK::Ustringswitch: {
      auto* x = as<Ustringswitch>(clam);
      ulambda cond = sub(x->arg);
      std::vector<UStringCase> cases;
      for (const UStringCase& c : x->cases) cases.push_back({c.s, sub(c.action)});
      return ustringswitch(cond, slice(cases), x->def ? sub(x->def) : nullptr);
    }
    case UK::Ustaticfail: {
      auto* x = as<Ustaticfail>(clam);
      return ustaticfail(x->i, subs(x->args));
    }
    case UK::Ucatch: {
      auto* x = as<Ucatch>(clam);
      ulambda body = sub(x->body);
      return ucatch(x->i, x->vars, body, sub(x->handler));
    }
    case UK::Utrywith: {
      auto* x = as<Utrywith>(clam);
      ulambda body = sub(x->body);
      return utrywith(body, x->exn, sub(x->handler));
    }
    case UK::Uifthenelse: {
      auto* x = as<Uifthenelse>(clam);
      ulambda c = sub(x->cond);
      ulambda a = sub(x->ifso);
      return uifthenelse(c, a, sub(x->ifnot));
    }
    case UK::Usequence: {
      auto* x = as<Usequence>(clam);
      ulambda a = sub(x->l1);
      return usequence(a, sub(x->l2));
    }
    case UK::Uwhile: {
      auto* x = as<Uwhile>(clam);
      ulambda c = sub(x->cond);
      return uwhile(c, sub(x->body));
    }
    case UK::Ufor: {
      auto* x = as<Ufor>(clam);
      ulambda lo = sub(x->lo);
      ulambda hi = sub(x->hi);
      return ufor(x->id, lo, hi, x->dir, sub(x->body));
    }
    case UK::Uassign: {
      auto* x = as<Uassign>(clam);
      return uassign(x->id, sub(x->e));
    }
    case UK::Usend: {
      auto* x = as<Usend>(clam);
      ulambda e1 = sub(x->met);
      ulambda e2 = sub(x->obj);
      return usend(x->k, e1, e2, subs(x->args), x->dbg);
    }
    case UK::Uunreachable: return uunreachable();
  }
  misc::fatal_error("Un_anf.substitute_let_moveable");
}

// We say that an expression is "moveable" iff it has neither effects nor
// coeffects.  (See semantics_of_primitives.mli.)
enum class Moveable : unsigned char { Fixed, Constant, Moveable };

Moveable both_moveable(Moveable a, Moveable b) {
  if (a == Moveable::Fixed || b == Moveable::Fixed) return Moveable::Fixed;
  if (a == Moveable::Constant && b == Moveable::Constant) return Moveable::Constant;
  return Moveable::Moveable;
}

Moveable primitive_moveable(const Primitive& prim, Slice<ulambda> args, const VarInfo& var_info) {
  if (prim.kind == Primitive::K::Pfield && args.size() == 1) {
    if (auto* c = as<Uconst>(args[0]); c && c->c.kind == UConstant::Kind::Uconst_ref)
      // Allow field access of symbols to be moveable.
      return Moveable::Moveable;
    if (auto* v = as<Uvar>(args[0]); v && var_info.closure_environment.count(v->id))
      // accesses to the function environment is coeffect free: this block
      // is never mutated
      return Moveable::Moveable;
  }
  auto [e, c] = semantics_of_primitives::for_primitive(prim);
  if (e == semantics_of_primitives::Effects::No_effects && c == semantics_of_primitives::Coeffects::No_coeffects)
    return Moveable::Moveable;
  return Moveable::Fixed;
}

// moveable_for_env = Constant | Moveable
struct EnvEntry {
  bool constant;
  ulambda def;
};
using UEnv = lambda::IdentPMap<EnvEntry>;

std::pair<ulambda, Moveable> un_anf_and_moveable(const VarInfo& var_info, const UEnv& env, ulambda clam);
ulambda un_anf(const VarInfo& var_info, const UEnv& env, ulambda clam) {
  return un_anf_and_moveable(var_info, env, clam).first;
}
// (List.fold_right: the arguments last first -- pure)
std::pair<Slice<ulambda>, Moveable> un_anf_list_and_moveable(const VarInfo& var_info, const UEnv& env,
                                                             Slice<ulambda> clams) {
  std::vector<ulambda> l(clams.size());
  Moveable acc = Moveable::Moveable;
  for (std::size_t k = clams.size(); k-- > 0;) {
    auto [c, m] = un_anf_and_moveable(var_info, env, clams[k]);
    l[k] = c;
    acc = both_moveable(m, acc);
  }
  return {slice(l), acc};
}
Slice<ulambda> un_anf_list(const VarInfo& var_info, const UEnv& env, Slice<ulambda> clams) {
  return un_anf_list_and_moveable(var_info, env, clams).first;
}

// Eliminate, through substitution, [let]-bindings of linear variables with
// moveable defining expressions.
std::pair<ulambda, Moveable> un_anf_and_moveable(const VarInfo& var_info, const UEnv& env, ulambda clam) {
  auto un = [&](ulambda l) { return un_anf(var_info, env, l); };
  auto uns = [&](Slice<ulambda> s) { return un_anf_list(var_info, env, s); };
  using M = Moveable;
  switch (clam->kind) {
    case UK::Uvar: {
      Var var = as<Uvar>(clam)->id;
      if (const EnvEntry* e = env.find_opt(var)) return {e->def, e->constant ? M::Constant : M::Moveable};
      return {clam, var_info.assigned.count(var) ? M::Fixed : M::Moveable};
    }
    case UK::Uconst:
      // Constant closures are rewritten separately.
      return {clam, M::Constant};
    case UK::Udirect_apply: {
      auto* x = as<Udirect_apply>(clam);
      return {udirect_apply(x->f, uns(x->args), x->dbg), M::Fixed};
    }
    case UK::Ugeneric_apply: {
      auto* x = as<Ugeneric_apply>(clam);
      ulambda f = un(x->f);
      return {ugeneric_apply(f, uns(x->args), x->dbg), M::Fixed};
    }
    case UK::Uclosure: {
      auto* x = as<Uclosure>(clam);
      std::vector<const UFunction*> funs;
      for (const UFunction* f : x->funs) {
        UFunction c = *f;
        c.body = un(f->body);
        funs.push_back(make<UFunction>(c));
      }
      return {uclosure(slice(funs), uns(x->fv)), M::Fixed};
    }
    case UK::Uoffset: {
      auto* x = as<Uoffset>(clam);
      auto [l, m] = un_anf_and_moveable(var_info, env, x->l);
      return {uoffset(l, x->ofs), both_moveable(M::Moveable, m)};
    }
    case UK::Ulet: {
      auto* x = as<Ulet>(clam);
      if (auto* v = as<Uvar>(x->body); v && ident::same(x->id.var, v->id))
        return un_anf_and_moveable(var_info, env, x->arg);
      auto [def, def_moveable] = un_anf_and_moveable(var_info, env, x->arg);
      Var var = x->id.var;
      bool is_linear = var_info.linear_let_bound_vars.count(var);
      bool is_used = var_info.used_let_bound_vars.count(var);
      bool is_assigned = var_info.assigned.count(var);
      if (def_moveable != M::Fixed && !is_used)
        // A moveable expression that is never used may be eliminated.
        return un_anf_and_moveable(var_info, env, x->body);
      if ((def_moveable == M::Constant && is_used && !is_assigned) ||
          (def_moveable == M::Moveable && is_linear && is_used && !is_assigned)) {
        // A constant expression bound to an unassigned variable can replace
        // any occurrences of the variable; a moveable expression bound to a
        // linear unassigned [V.t] may replace the single occurrence of the
        // variable.
        UEnv env2 = env.add(var, EnvEntry{def_moveable == M::Constant, def});
        return un_anf_and_moveable(var_info, env2, x->body);
      }
      auto [body, body_moveable] = un_anf_and_moveable(var_info, env, x->body);
      return {ulet(x->mut, x->k, x->id, def, body), both_moveable(def_moveable, body_moveable)};
    }
    case UK::Uphantom_let: {
      auto* x = as<Uphantom_let>(clam);
      auto [body, m] = un_anf_and_moveable(var_info, env, x->body);
      return {uphantom_let(x->id, x->def, body), m};
    }
    case UK::Uprim: {
      auto* x = as<Uprim>(clam);
      auto [args, args_moveable] = un_anf_list_and_moveable(var_info, env, x->args);
      return {uprim(x->p, args, x->dbg), both_moveable(args_moveable, primitive_moveable(x->p, args, var_info))};
    }
    case UK::Uswitch: {
      auto* x = as<Uswitch>(clam);
      ulambda cond = un(x->arg);
      USwitch sw = x->sw;
      sw.us_actions_consts = slice(map_slice(x->sw.us_actions_consts, un));
      sw.us_actions_blocks = slice(map_slice(x->sw.us_actions_blocks, un));
      return {uswitch(cond, sw, x->dbg), M::Fixed};
    }
    case UK::Ustringswitch: {
      auto* x = as<Ustringswitch>(clam);
      ulambda cond = un(x->arg);
      std::vector<UStringCase> cases;
      for (const UStringCase& c : x->cases) cases.push_back({c.s, un(c.action)});
      return {ustringswitch(cond, slice(cases), x->def ? un(x->def) : nullptr), M::Fixed};
    }
    case UK::Ustaticfail: {
      auto* x = as<Ustaticfail>(clam);
      return {ustaticfail(x->i, uns(x->args)), M::Fixed};
    }
    case UK::Ucatch: {
      auto* x = as<Ucatch>(clam);
      ulambda body = un(x->body);
      return {ucatch(x->i, x->vars, body, un(x->handler)), M::Fixed};
    }
    case UK::Utrywith: {
      auto* x = as<Utrywith>(clam);
      ulambda body = un(x->body);
      return {utrywith(body, x->exn, un(x->handler)), M::Fixed};
    }
    case UK::Uifthenelse: {
      auto* x = as<Uifthenelse>(clam);
      auto [c, cm] = un_anf_and_moveable(var_info, env, x->cond);
      auto [a, am] = un_anf_and_moveable(var_info, env, x->ifso);
      auto [b, bm] = un_anf_and_moveable(var_info, env, x->ifnot);
      return {uifthenelse(c, a, b), both_moveable(cm, both_moveable(am, bm))};
    }
    case UK::Usequence: {
      auto* x = as<Usequence>(clam);
      ulambda a = un(x->l1);
      return {usequence(a, un(x->l2)), M::Fixed};
    }
    case UK::Uwhile: {
      auto* x = as<Uwhile>(clam);
      ulambda c = un(x->cond);
      return {uwhile(c, un(x->body)), M::Fixed};
    }
    case UK::Ufor: {
      auto* x = as<Ufor>(clam);
      ulambda lo = un(x->lo);
      ulambda hi = un(x->hi);
      return {ufor(x->id, lo, hi, x->dir, un(x->body)), M::Fixed};
    }
    case UK::Uassign: {
      auto* x = as<Uassign>(clam);
      return {uassign(x->id, un(x->e)), M::Fixed};
    }
    case UK::Usend: {
      auto* x = as<Usend>(clam);
      ulambda e1 = un(x->met);
      ulambda e2 = un(x->obj);
      return {usend(x->k, e1, e2, uns(x->args), x->dbg), M::Fixed};
    }
    case UK::Uunreachable: return {uunreachable(), M::Fixed};
  }
  misc::fatal_error("Un_anf.un_anf_and_moveable");
}

}  // namespace

ulambda apply(symbol::t what, format::Formatter& ppf_dump, ulambda clam) {
  VarInfo var_info = make_var_info(clam);
  VSet moveable = let_bound_vars_that_can_be_moved(var_info, clam);
  clam = substitute_let_moveable(moveable, Env{}, clam);
  VarInfo var_info2 = make_var_info(clam);
  clam = un_anf(var_info2, UEnv{}, clam);
  if (clflags::dump_clambda) {
    format::fprintf(ppf_dump, "@.un-anf (%a):@ %a@.", [&](format::Formatter& f) { symbol::print(f, what); },
                    [&](format::Formatter& f) { printclambda::clambda(f, clam); });
  }
  return clam;
}

}  // namespace cppcaml::typing::un_anf
