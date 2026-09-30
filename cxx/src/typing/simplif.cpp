// Port of lambda/simplif.ml (cxx/PORTING.md stage 10).  See simplif.hpp.
//
// Where simplif.ml builds a constructor, tuple or record whose components
// have effects (Ident.rename, the substitution tables, next_raise_count),
// the components are evaluated as OCaml does: right to left, a record's
// fields right to left in definition order, `let ... and ...` and List.map
// left to right.
#include "cppcaml/typing/simplif.hpp"
#include "cppcaml/typing/location.hpp"

#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/tmc.hpp"

namespace cppcaml::typing::simplif {

using Lam = typing::lambda::lambda;
using namespace typing::lambda;
using typing::lambda::ValueKind;
using PK = Primitive::K;

namespace {

template <class F>
Slice<Lam> map_list(Slice<Lam> l, F&& f) {  // List.map: left to right
  std::vector<Lam> out;
  out.reserve(l.size());
  for (Lam x : l) out.push_back(f(x));
  return slice(out);
}

// ---- To transform let-bound references into variables ------------------------
struct RealReference {};

void check_function_escape(Ident::t id, const LFunction* lfun) {
  for (auto& p : lfun->params)
    if (ident::same(id, p.id)) throw std::logic_error("Simplif.check_function_escape");
  if (free_variables(lfun->body).count(id)) throw RealReference{};
}

Lam eliminate_ref(Ident::t id, Lam lam) {
  auto er = [&](Lam l) { return eliminate_ref(id, l); };
  switch (lam->kind) {
    case LK::Lvar:
      if (ident::same(as<Lvar>(lam)->id, id)) throw RealReference{};
      return lam;
    case LK::Lmutvar:
    case LK::Lconst: return lam;
    case LK::Lapply: {
      auto* a = as<Lapply>(lam);
      LambdaApply ap = a->ap;
      ap.ap_args = map_list(a->ap.ap_args, er);
      ap.ap_func = er(a->ap.ap_func);
      return lapply(ap);
    }
    case LK::Lfunction:
      check_function_escape(id, as<Lfunction>(lam)->f);
      return lam;
    case LK::Llet: {
      auto* x = as<Llet>(lam);
      Lam e2 = er(x->body);
      Lam e1 = er(x->arg);
      return llet(x->str, x->k, x->id, e1, e2);
    }
    case LK::Lmutlet: {
      auto* x = as<Lmutlet>(lam);
      Lam e2 = er(x->body);
      Lam e1 = er(x->arg);
      return lmutlet(x->k, x->id, e1, e2);
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(lam);
      for (auto& rb : x->decl) check_function_escape(id, rb.def);
      return lletrec(x->decl, er(x->body));
    }
    case LK::Lprim: {
      auto* p = as<Lprim>(lam);
      auto is_id = [&](Lam l) {
        auto* v = as<Lvar>(l);
        return v && ident::same(v->id, id);
      };
      if (p->p.kind == PK::Pfield && p->p.n == 0 && p->args.size() == 1 && is_id(p->args[0])) return lmutvar(id);
      if (p->p.kind == PK::Psetfield && p->p.n == 0 && p->args.size() == 2 && is_id(p->args[0]))
        return lassign(id, er(p->args[1]));
      if (p->p.kind == PK::Poffsetref && p->args.size() == 1 && is_id(p->args[0])) {
        Primitive off = prim(PK::Poffsetint);
        off.n = p->p.n;
        return lassign(id, lprim(off, slice({lmutvar(id)}), p->loc));
      }
      return lprim(p->p, map_list(p->args, er), p->loc);
    }
    case LK::Lswitch: {
      auto* x = as<Lswitch>(lam);
      LambdaSwitch sw = x->sw;
      sw.sw_failaction = x->sw.sw_failaction ? er(x->sw.sw_failaction) : nullptr;
      std::vector<SwitchCase> blocks, consts;
      for (auto& c : x->sw.sw_blocks) blocks.push_back({c.key, er(c.action)});
      for (auto& c : x->sw.sw_consts) consts.push_back({c.key, er(c.action)});
      sw.sw_blocks = slice(blocks);
      sw.sw_consts = slice(consts);
      Lam e = er(x->arg);
      return lswitch(e, sw, x->loc);
    }
    case LK::Lstringswitch: {
      auto* x = as<Lstringswitch>(lam);
      Lam d = x->def ? er(x->def) : nullptr;
      std::vector<StringCase> cs;
      for (auto& c : x->cases) cs.push_back({c.s, er(c.action)});
      Lam e = er(x->arg);
      return lstringswitch(e, slice(cs), d, x->loc);
    }
    case LK::Lstaticraise: {
      auto* r = as<Lstaticraise>(lam);
      return lstaticraise(r->i, map_list(r->args, er));
    }
    case LK::Lstaticcatch: {
      auto* c = as<Lstaticcatch>(lam);
      Lam h = er(c->handler);
      Lam b = er(c->body);
      return lstaticcatch(b, c->i, c->params, h);
    }
    case LK::Ltrywith: {
      auto* t = as<Ltrywith>(lam);
      Lam h = er(t->handler);
      Lam b = er(t->body);
      return ltrywith(b, t->exn, h);
    }
    case LK::Lifthenelse: {
      auto* i = as<Lifthenelse>(lam);
      Lam e3 = er(i->ifnot);
      Lam e2 = er(i->ifso);
      Lam e1 = er(i->cond);
      return lifthenelse(e1, e2, e3);
    }
    case LK::Lsequence: {
      auto* s = as<Lsequence>(lam);
      Lam e2 = er(s->l2);
      Lam e1 = er(s->l1);
      return lsequence(e1, e2);
    }
    case LK::Lwhile: {
      auto* w = as<Lwhile>(lam);
      Lam e2 = er(w->body);
      Lam e1 = er(w->cond);
      return lwhile(e1, e2);
    }
    case LK::Lfor: {
      auto* x = as<Lfor>(lam);
      Lam e3 = er(x->body);
      Lam e2 = er(x->hi);
      Lam e1 = er(x->lo);
      return lfor(x->id, e1, e2, x->dir, e3);
    }
    case LK::Lassign: {
      auto* a = as<Lassign>(lam);
      return lassign(a->id, er(a->e));
    }
    case LK::Lsend: {
      auto* s = as<Lsend>(lam);
      Slice<Lam> el = map_list(s->args, er);
      Lam o = er(s->obj);
      Lam m = er(s->met);
      return lsend(s->k, m, o, el, s->loc);
    }
    case LK::Levent: {
      auto* e = as<Levent>(lam);
      return levent(er(e->l), e->ev);
    }
    case LK::Lifused: {
      auto* u = as<Lifused>(lam);
      return lifused(u->id, er(u->l));
    }
  }
  throw std::logic_error("Simplif.eliminate_ref");
}

// ---- Simplification of exits -------------------------------------------------
struct Exit {
  long count;
  long max_depth;
};

struct SimplifyExits {
  std::unordered_map<long, Exit> exits;  // Hashtbl: only find/add, never iterated
  // subst: i -> (xs, handler); Hashtbl.add shadows, only found (never removed)
  std::map<long, std::pair<Slice<Param>, Lam>> subst;

  Exit get_exit(long i) {
    auto it = exits.find(i);
    return it == exits.end() ? Exit{0, 0} : it->second;
  }
  void incr_exit(long i, long nb, long d) {
    auto it = exits.find(i);
    if (it != exits.end()) {
      it->second.count += nb;
      it->second.max_depth = std::max(it->second.max_depth, d);
    } else {
      exits.emplace(i, Exit{nb, d});
    }
  }

  void count(long try_depth, Lam lam) {
    switch (lam->kind) {
      case LK::Lvar:
      case LK::Lmutvar:
      case LK::Lconst: return;
      case LK::Lapply: {
        auto* a = as<Lapply>(lam);
        count(try_depth, a->ap.ap_func);
        for (auto x : a->ap.ap_args) count(try_depth, x);
        return;
      }
      case LK::Lfunction: count(try_depth, as<Lfunction>(lam)->f->body); return;
      case LK::Llet: {
        auto* x = as<Llet>(lam);
        count(try_depth, x->body);
        count(try_depth, x->arg);
        return;
      }
      case LK::Lmutlet: {
        auto* x = as<Lmutlet>(lam);
        count(try_depth, x->body);
        count(try_depth, x->arg);
        return;
      }
      case LK::Lletrec: {
        auto* x = as<Lletrec>(lam);
        for (auto& rb : x->decl) count(try_depth, rb.def->body);
        count(try_depth, x->body);
        return;
      }
      case LK::Lprim:
        for (auto x : as<Lprim>(lam)->args) count(try_depth, x);
        return;
      case LK::Lswitch: {
        auto* x = as<Lswitch>(lam);
        count_default(try_depth, x->sw);
        count(try_depth, x->arg);
        for (auto& c : x->sw.sw_consts) count(try_depth, c.action);
        for (auto& c : x->sw.sw_blocks) count(try_depth, c.action);
        return;
      }
      case LK::Lstringswitch: {
        auto* x = as<Lstringswitch>(lam);
        count(try_depth, x->arg);
        for (auto& c : x->cases) count(try_depth, c.action);
        if (x->def) {
          if (x->cases.size() <= 1) {
            count(try_depth, x->def);
          } else {  // default will get replicated
            count(try_depth, x->def);
            count(try_depth, x->def);
          }
        }
        return;
      }
      case LK::Lstaticraise: {
        auto* r = as<Lstaticraise>(lam);
        incr_exit(r->i, 1, try_depth);
        for (auto x : r->args) count(try_depth, x);
        return;
      }
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(lam);
        auto* j = as<Lstaticraise>(c->handler);
        if (c->params.size() == 0 && j && j->args.size() == 0) {
          // i will be replaced by j in l1, so each occurrence of i in l1
          // increases j's ref count
          count(try_depth, c->body);
          Exit ic = get_exit(c->i);
          incr_exit(j->i, ic.count, std::max(try_depth, ic.max_depth));
          return;
        }
        count(try_depth, c->body);
        // If l1 does not contain (exit i), l2 will be removed, so don't
        // count its exits
        if (get_exit(c->i).count > 0) count(try_depth, c->handler);
        return;
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(lam);
        count(try_depth + 1, t->body);
        count(try_depth, t->handler);
        return;
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(lam);
        count(try_depth, i->cond);
        count(try_depth, i->ifso);
        count(try_depth, i->ifnot);
        return;
      }
      case LK::Lsequence: {
        auto* s = as<Lsequence>(lam);
        count(try_depth, s->l1);
        count(try_depth, s->l2);
        return;
      }
      case LK::Lwhile: {
        auto* w = as<Lwhile>(lam);
        count(try_depth, w->cond);
        count(try_depth, w->body);
        return;
      }
      case LK::Lfor: {
        auto* x = as<Lfor>(lam);
        count(try_depth, x->lo);
        count(try_depth, x->hi);
        count(try_depth, x->body);
        return;
      }
      case LK::Lassign: count(try_depth, as<Lassign>(lam)->e); return;
      case LK::Lsend: {
        auto* s = as<Lsend>(lam);
        count(try_depth, s->met);
        count(try_depth, s->obj);
        for (auto x : s->args) count(try_depth, x);
        return;
      }
      case LK::Levent: count(try_depth, as<Levent>(lam)->l); return;
      case LK::Lifused: count(try_depth, as<Lifused>(lam)->l); return;
    }
  }

  void count_default(long try_depth, const LambdaSwitch& sw) {
    if (!sw.sw_failaction) return;
    long nconsts = static_cast<long>(sw.sw_consts.size());
    long nblocks = static_cast<long>(sw.sw_blocks.size());
    if (nconsts < sw.sw_numconsts && nblocks < sw.sw_numblocks) {
      // default action will occur twice in native code
      count(try_depth, sw.sw_failaction);
      count(try_depth, sw.sw_failaction);
    } else {  // default action will occur once
      if (!(nconsts < sw.sw_numconsts || nblocks < sw.sw_numblocks))
        throw std::logic_error("Simplif.simplify_exits: count_default");
      count(try_depth, sw.sw_failaction);
    }
  }

  Lam simplif(long try_depth, Lam lam) {
    auto sf = [&](Lam l) { return simplif(try_depth, l); };
    switch (lam->kind) {
      case LK::Lvar:
      case LK::Lmutvar:
      case LK::Lconst: return lam;
      case LK::Lapply: {
        auto* a = as<Lapply>(lam);
        LambdaApply ap = a->ap;
        ap.ap_args = map_list(a->ap.ap_args, sf);
        ap.ap_func = sf(a->ap.ap_func);
        return lapply(ap);
      }
      case LK::Lfunction: return lfunction_node(map_lfunction(sf, as<Lfunction>(lam)->f));
      case LK::Llet: {
        auto* x = as<Llet>(lam);
        Lam e2 = sf(x->body);
        Lam e1 = sf(x->arg);
        return llet(x->str, x->k, x->id, e1, e2);
      }
      case LK::Lmutlet: {
        auto* x = as<Lmutlet>(lam);
        Lam e2 = sf(x->body);
        Lam e1 = sf(x->arg);
        return lmutlet(x->k, x->id, e1, e2);
      }
      case LK::Lletrec: {
        auto* x = as<Lletrec>(lam);
        std::vector<RecBinding> decl;
        for (auto& rb : x->decl) {
          const LFunction* d = rb.def;
          decl.push_back({rb.id, lfunction_(d->kind, d->params, d->return_, sf(d->body), d->attr, d->loc)});
        }
        return lletrec(slice(decl), sf(x->body));
      }
      case LK::Lprim: {
        auto* p = as<Lprim>(lam);
        Slice<Lam> ll = map_list(p->args, sf);
        // Simplify Obj.with_tag
        if (p->p.kind == PK::Pccall && p->p.ccall && p->p.ccall->prim_name == "caml_obj_with_tag" &&
            ll.size() == 2) {
          auto* c = as<Lconst>(ll[0]);
          if (c && c->c->kind == StructuredConstant::Kind::Const_int) {
            long tag = c->c->i;
            if (auto* mb = as<Lprim>(ll[1]); mb && mb->p.kind == PK::Pmakeblock) {
              Primitive q = mb->p;
              q.n = tag;
              return lprim(q, mb->args, mb->loc);
            }
            if (auto* cb = as<Lconst>(ll[1]); cb && cb->c->kind == StructuredConstant::Kind::Const_block) {
              StructuredConstant k = *cb->c;
              k.i = tag;
              return lconst(make<StructuredConstant>(k));
            }
          }
        }
        return lprim(p->p, ll, p->loc);
      }
      case LK::Lswitch: {
        auto* x = as<Lswitch>(lam);
        // let new_l = .. and new_consts = .. and new_blocks = .. and new_fail = ..
        Lam new_l = sf(x->arg);
        std::vector<SwitchCase> consts, blocks;
        for (auto& c : x->sw.sw_consts) consts.push_back({c.key, sf(c.action)});
        for (auto& c : x->sw.sw_blocks) blocks.push_back({c.key, sf(c.action)});
        Lam new_fail = x->sw.sw_failaction ? sf(x->sw.sw_failaction) : nullptr;
        LambdaSwitch sw = x->sw;
        sw.sw_consts = slice(consts);
        sw.sw_blocks = slice(blocks);
        sw.sw_failaction = new_fail;
        return lswitch(new_l, sw, x->loc);
      }
      case LK::Lstringswitch: {
        auto* x = as<Lstringswitch>(lam);
        Lam d = x->def ? sf(x->def) : nullptr;
        std::vector<StringCase> cs;
        for (auto& c : x->cases) cs.push_back({c.s, sf(c.action)});
        Lam e = sf(x->arg);
        return lstringswitch(e, slice(cs), d, x->loc);
      }
      case LK::Lstaticraise: {
        auto* r = as<Lstaticraise>(lam);
        if (r->args.size() == 0) {
          auto it = subst.find(r->i);
          return it == subst.end() ? lam : it->second.second;
        }
        Slice<Lam> ls = map_list(r->args, sf);
        auto it = subst.find(r->i);
        if (it == subst.end()) return lstaticraise(r->i, ls);
        Slice<Param> xs = it->second.first;
        Lam handler = it->second.second;
        std::vector<Param> ys;
        for (auto& x : xs) ys.push_back({ident::rename(x.id), x.kind});
        if (ys.size() != ls.size()) throw std::invalid_argument("List.fold_left2");
        IdentMap<Ident::t> env;
        for (std::size_t k = xs.size(); k-- > 0;) env[xs[k].id] = ys[k].id;  // fold_right2 of Map.add
        // The evaluation order for Lstaticraise arguments is right-to-left:
        // fold_left2 inserts the first argument deepest.
        Lam r2 = rename(env, handler);
        for (std::size_t k = 0; k < ys.size(); ++k) r2 = llet(LetKind::Strict, ys[k].kind, ys[k].id, ls[k], r2);
        return r2;
      }
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(lam);
        auto* j = as<Lstaticraise>(c->handler);
        if (c->params.size() == 0 && j && j->args.size() == 0) {
          Lam h = sf(c->handler);
          subst[c->i] = {c->params, h};
          return sf(c->body);
        }
        Exit e = get_exit(c->i);
        if (e.count == 0) {
          // Discard staticcatch: not matching exit
          return sf(c->body);
        } else if (e.count == 1 && e.max_depth <= try_depth) {
          // Inline handler if there is a single occurrence and it is not
          // nested within an inner try..with
          if (e.max_depth != try_depth) throw std::logic_error("Simplif.simplify_exits: max_depth");
          Lam h = sf(c->handler);
          subst[c->i] = {c->params, h};
          return sf(c->body);
        }
        Lam h = sf(c->handler);
        Lam b = sf(c->body);
        return lstaticcatch(b, c->i, c->params, h);
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(lam);
        Lam l1 = simplif(try_depth + 1, t->body);
        return ltrywith(l1, t->exn, sf(t->handler));
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(lam);
        Lam e3 = sf(i->ifnot);
        Lam e2 = sf(i->ifso);
        Lam e1 = sf(i->cond);
        return lifthenelse(e1, e2, e3);
      }
      case LK::Lsequence: {
        auto* s = as<Lsequence>(lam);
        Lam e2 = sf(s->l2);
        Lam e1 = sf(s->l1);
        return lsequence(e1, e2);
      }
      case LK::Lwhile: {
        auto* w = as<Lwhile>(lam);
        Lam e2 = sf(w->body);
        Lam e1 = sf(w->cond);
        return lwhile(e1, e2);
      }
      case LK::Lfor: {
        auto* x = as<Lfor>(lam);
        Lam e3 = sf(x->body);
        Lam e2 = sf(x->hi);
        Lam e1 = sf(x->lo);
        return lfor(x->id, e1, e2, x->dir, e3);
      }
      case LK::Lassign: {
        auto* a = as<Lassign>(lam);
        return lassign(a->id, sf(a->e));
      }
      case LK::Lsend: {
        auto* s = as<Lsend>(lam);
        Slice<Lam> el = map_list(s->args, sf);
        Lam o = sf(s->obj);
        Lam m = sf(s->met);
        return lsend(s->k, m, o, el, s->loc);
      }
      case LK::Levent: {
        auto* e = as<Levent>(lam);
        return levent(sf(e->l), e->ev);
      }
      case LK::Lifused: {
        auto* u = as<Lifused>(lam);
        return lifused(u->id, sf(u->l));
      }
    }
    throw std::logic_error("Simplif.simplify_exits");
  }
};

Lam simplify_exits(Lam lam) {
  SimplifyExits s;
  s.count(0, lam);
  return s.simplif(0, lam);
}

// Compile-time beta-reduction of functions immediately applied.
std::optional<Slice<Lam>> exact_application(const LFunction* lf, Slice<Lam> args) {
  return find_exact_application(lf->kind, static_cast<long>(lf->params.size()), args);
}

Lam beta_reduce(Slice<Param> params, Lam body, Slice<Lam> args) {
  if (params.size() != args.size()) throw std::invalid_argument("List.fold_left2");
  Lam l = body;
  for (std::size_t k = 0; k < params.size(); ++k) l = llet(LetKind::Strict, params[k].kind, params[k].id, args[k], l);
  return l;
}

// ---- Simplification of lets ----------------------------------------------------
struct SimplifyLets {
  // Disable optimisations for bytecode compilation with -g flag
  bool optimize = clflags::native_code || !clflags::debug;

  // occ: Hashtbl.add shadows, find returns the latest binding
  std::deque<long> refs;  // the int refs
  IdentMap<std::vector<long*>> occ;
  // bv: Ident.Map of int refs.  Passed down by reference and restored after
  // each binding's scope (count never escapes a scope early).
  using Bv = IdentMap<long*>;

  long count_var(Ident::t v) {
    auto it = occ.find(v);
    return it == occ.end() || it->second.empty() ? 0 : *it->second.back();
  }

  // Entering a [let]: returns the binding of v in bv it shadows (to restore)
  std::optional<long*> bind_var(Bv& bv, Ident::t v) {
    refs.push_back(0);
    long* r = &refs.back();
    occ[v].push_back(r);
    std::optional<long*> old;
    auto it = bv.find(v);
    if (it != bv.end()) {
      old = it->second;
      it->second = r;
    } else {
      bv.emplace(v, r);
    }
    return old;
  }
  static void unbind_var(Bv& bv, Ident::t v, std::optional<long*> old) {
    if (old)
      bv[v] = *old;
    else
      bv.erase(v);
  }

  void use_var(Bv& bv, Ident::t v, long n) {
    auto it = bv.find(v);
    if (it != bv.end()) {
      *it->second += n;
      return;
    }
    // v is not locally bound, therefore this is a use under a lambda or
    // within a loop.  Increase use count by 2 -- enough so that single-use
    // optimizations will not apply.
    auto jt = occ.find(v);
    if (jt != occ.end() && !jt->second.empty()) *jt->second.back() += 2;
    // Not a let-bound variable, ignore
  }

  void count(Bv& bv, Lam lam) {
    switch (lam->kind) {
      case LK::Lconst: return;
      case LK::Lvar: use_var(bv, as<Lvar>(lam)->id, 1); return;
      case LK::Lmutvar: return;
      case LK::Lapply: {
        auto* a = as<Lapply>(lam);
        Lam ll = a->ap.ap_func;
        auto no_opt = [&] {
          count(bv, ll);
          for (auto x : a->ap.ap_args) count(bv, x);
        };
        if (auto* lf = as<Lfunction>(ll); lf && optimize) {
          auto exact_args = exact_application(lf->f, a->ap.ap_args);
          if (!exact_args)
            no_opt();
          else
            count(bv, beta_reduce(lf->f->params, lf->f->body, *exact_args));
          return;
        }
        no_opt();
        return;
      }
      case LK::Lfunction: count_lfunction(as<Lfunction>(lam)->f); return;
      case LK::Llet: {
        auto* x = as<Llet>(lam);
        if (auto* w = as<Lvar>(x->arg); w && optimize) {
          // v will be replaced by w in l2, so each occurrence of v in l2
          // increases w's refcount
          auto old = bind_var(bv, x->id);
          count(bv, x->body);
          unbind_var(bv, x->id, old);
          use_var(bv, w->id, count_var(x->id));
          return;
        }
        auto old = bind_var(bv, x->id);
        count(bv, x->body);
        unbind_var(bv, x->id, old);
        // If v is unused, l1 will be removed, so don't count its variables
        if (x->str == LetKind::Strict || count_var(x->id) > 0) count(bv, x->arg);
        return;
      }
      case LK::Lmutlet: {
        auto* x = as<Lmutlet>(lam);
        count(bv, x->arg);
        count(bv, x->body);
        return;
      }
      case LK::Lletrec: {
        auto* x = as<Lletrec>(lam);
        for (auto& rb : x->decl) count_lfunction(rb.def);
        count(bv, x->body);
        return;
      }
      case LK::Lprim:
        for (auto x : as<Lprim>(lam)->args) count(bv, x);
        return;
      case LK::Lswitch: {
        auto* x = as<Lswitch>(lam);
        count_default(bv, x->sw);
        count(bv, x->arg);
        for (auto& c : x->sw.sw_consts) count(bv, c.action);
        for (auto& c : x->sw.sw_blocks) count(bv, c.action);
        return;
      }
      case LK::Lstringswitch: {
        auto* x = as<Lstringswitch>(lam);
        count(bv, x->arg);
        for (auto& c : x->cases) count(bv, c.action);
        if (x->def) {
          if (x->cases.size() <= 1) {
            count(bv, x->def);
          } else {
            count(bv, x->def);
            count(bv, x->def);
          }
        }
        return;
      }
      case LK::Lstaticraise:
        for (auto x : as<Lstaticraise>(lam)->args) count(bv, x);
        return;
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(lam);
        count(bv, c->body);
        count(bv, c->handler);
        return;
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(lam);
        count(bv, t->body);
        count(bv, t->handler);
        return;
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(lam);
        count(bv, i->cond);
        count(bv, i->ifso);
        count(bv, i->ifnot);
        return;
      }
      case LK::Lsequence: {
        auto* s = as<Lsequence>(lam);
        count(bv, s->l1);
        count(bv, s->l2);
        return;
      }
      case LK::Lwhile: {
        auto* w = as<Lwhile>(lam);
        Bv e1, e2;
        count(e1, w->cond);
        count(e2, w->body);
        return;
      }
      case LK::Lfor: {
        auto* x = as<Lfor>(lam);
        count(bv, x->lo);
        count(bv, x->hi);
        Bv e;
        count(e, x->body);
        return;
      }
      case LK::Lassign:
        // Lalias-bound variables are never assigned, so don't increase v's
        // refcount
        count(bv, as<Lassign>(lam)->e);
        return;
      case LK::Lsend: {
        auto* s = as<Lsend>(lam);
        count(bv, s->met);
        count(bv, s->obj);
        for (auto x : s->args) count(bv, x);
        return;
      }
      case LK::Levent: count(bv, as<Levent>(lam)->l); return;
      case LK::Lifused: {
        auto* u = as<Lifused>(lam);
        if (count_var(u->id) > 0) count(bv, u->l);
        return;
      }
    }
  }

  void count_lfunction(const LFunction* fn) {
    Bv e;
    count(e, fn->body);
  }

  void count_default(Bv& bv, const LambdaSwitch& sw) {
    if (!sw.sw_failaction) return;
    long nconsts = static_cast<long>(sw.sw_consts.size());
    long nblocks = static_cast<long>(sw.sw_blocks.size());
    if (nconsts < sw.sw_numconsts && nblocks < sw.sw_numblocks) {
      // default action will occur twice in native code
      count(bv, sw.sw_failaction);
      count(bv, sw.sw_failaction);
    } else {  // default action will occur once
      if (!(nconsts < sw.sw_numconsts || nblocks < sw.sw_numblocks))
        throw std::logic_error("Simplif.simplify_lets: count_default");
      count(bv, sw.sw_failaction);
    }
  }

  // Second pass: remove Lalias bindings of unused variables, and substitute
  // the bindings of variables used exactly once.
  IdentMap<Lam> subst;  // Hashtbl.add shadows; never removed

  // This (small) optimisation is always legal, it may uncover some tail
  // call later on.
  Lam mklet(LetKind str, ValueKind kind, Ident::t v, Lam e1, Lam e2) {
    if (auto* w = as<Lvar>(e2); w && optimize && ident::same(v, w->id)) return e1;
    return llet(str, kind, v, e1, e2);
  }
  Lam mkmutlet(ValueKind kind, Ident::t v, Lam e1, Lam e2) {
    if (auto* w = as<Lmutvar>(e2); w && optimize && ident::same(v, w->id)) return e1;
    return lmutlet(kind, v, e1, e2);
  }

  Lam simplif(Lam lam) {
    auto sf = [&](Lam l) { return simplif(l); };
    switch (lam->kind) {
      case LK::Lvar: {
        auto it = subst.find(as<Lvar>(lam)->id);
        return it == subst.end() ? lam : it->second;
      }
      case LK::Lmutvar:
      case LK::Lconst: return lam;
      case LK::Lapply: {
        auto* a = as<Lapply>(lam);
        auto no_opt = [&] {
          LambdaApply ap = a->ap;
          ap.ap_args = map_list(a->ap.ap_args, sf);
          ap.ap_func = sf(a->ap.ap_func);
          return lapply(ap);
        };
        if (auto* lf = as<Lfunction>(a->ap.ap_func); lf && optimize) {
          auto exact_args = exact_application(lf->f, a->ap.ap_args);
          if (!exact_args) return no_opt();
          return simplif(beta_reduce(lf->f->params, lf->f->body, *exact_args));
        }
        return no_opt();
      }
      case LK::Lfunction: {
        const LFunction* f = as<Lfunction>(lam)->f;
        Lam b = simplif(f->body);
        if (auto* inner = as<Lfunction>(b)) {
          const LFunction* g = inner->f;
          if (g->kind == FunctionKind::Curried && f->kind == FunctionKind::Curried && optimize &&
              f->attr.may_fuse_arity && g->attr.may_fuse_arity &&
              static_cast<long>(f->params.size() + g->params.size()) <= max_arity()) {
            // The return type of the merged function taking [params @
            // params'] is the type returned after applying [params'].
            std::vector<Param> ps(f->params.begin(), f->params.end());
            ps.insert(ps.end(), g->params.begin(), g->params.end());
            return lfunction(f->kind, slice(ps), g->return_, g->body, g->attr, g->loc);
          }
        }
        return lfunction(f->kind, f->params, f->return_, b, f->attr, f->loc);
      }
      case LK::Llet: {
        auto* x = as<Llet>(lam);
        if (as<Lvar>(x->arg) && optimize) {
          subst[x->id] = simplif(x->arg);
          return simplif(x->body);
        }
        if (x->str == LetKind::Strict && optimize) {
          auto* mb = as<Lprim>(x->arg);
          if (mb && mb->p.kind == PK::Pmakeblock && mb->p.n == 0 && mb->p.mut == MutableFlag::Mutable &&
              mb->args.size() == 1) {
            Lam slinit = simplif(mb->args[0]);
            Lam slbody = simplif(x->body);
            try {
              ValueKind kind;
              if (!mb->p.shape.some) {
                kind = ValueKind::gen();
              } else if (mb->p.shape.kinds.size() == 1) {
                kind = mb->p.shape.kinds[0];
              } else {
                throw std::logic_error("Simplif.simplify_lets: kind_ref");
              }
              return mkmutlet(kind, x->id, slinit, eliminate_ref(x->id, slbody));
            } catch (const RealReference&) {
              return mklet(LetKind::Strict, x->k, x->id, lprim(mb->p, slice({slinit}), mb->loc), slbody);
            }
          }
        }
        if (x->str == LetKind::Alias) {
          long c = count_var(x->id);
          if (c == 0) return simplif(x->body);
          if (c == 1 && optimize) {
            subst[x->id] = simplif(x->arg);
            return simplif(x->body);
          }
          Lam e2 = simplif(x->body);
          Lam e1 = simplif(x->arg);
          return llet(LetKind::Alias, x->k, x->id, e1, e2);
        }
        if (x->str == LetKind::StrictOpt) {
          if (count_var(x->id) == 0) return simplif(x->body);
        }
        Lam e2 = simplif(x->body);
        Lam e1 = simplif(x->arg);
        return mklet(x->str, x->k, x->id, e1, e2);
      }
      case LK::Lmutlet: {
        auto* x = as<Lmutlet>(lam);
        Lam e2 = simplif(x->body);
        Lam e1 = simplif(x->arg);
        return mkmutlet(x->k, x->id, e1, e2);
      }
      case LK::Lletrec: {
        auto* x = as<Lletrec>(lam);
        std::vector<RecBinding> decl;
        for (auto& rb : x->decl) decl.push_back({rb.id, map_lfunction(sf, rb.def)});
        return lletrec(slice(decl), simplif(x->body));
      }
      case LK::Lprim: {
        auto* p = as<Lprim>(lam);
        return lprim(p->p, map_list(p->args, sf), p->loc);
      }
      case LK::Lswitch: {
        auto* x = as<Lswitch>(lam);
        Lam new_l = simplif(x->arg);
        std::vector<SwitchCase> consts, blocks;
        for (auto& c : x->sw.sw_consts) consts.push_back({c.key, simplif(c.action)});
        for (auto& c : x->sw.sw_blocks) blocks.push_back({c.key, simplif(c.action)});
        Lam new_fail = x->sw.sw_failaction ? simplif(x->sw.sw_failaction) : nullptr;
        LambdaSwitch sw = x->sw;
        sw.sw_consts = slice(consts);
        sw.sw_blocks = slice(blocks);
        sw.sw_failaction = new_fail;
        return lswitch(new_l, sw, x->loc);
      }
      case LK::Lstringswitch: {
        auto* x = as<Lstringswitch>(lam);
        Lam d = x->def ? simplif(x->def) : nullptr;
        std::vector<StringCase> cs;
        for (auto& c : x->cases) cs.push_back({c.s, simplif(c.action)});
        Lam e = simplif(x->arg);
        return lstringswitch(e, slice(cs), d, x->loc);
      }
      case LK::Lstaticraise: {
        auto* r = as<Lstaticraise>(lam);
        return lstaticraise(r->i, map_list(r->args, sf));
      }
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(lam);
        Lam h = simplif(c->handler);
        Lam b = simplif(c->body);
        return lstaticcatch(b, c->i, c->params, h);
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(lam);
        Lam h = simplif(t->handler);
        Lam b = simplif(t->body);
        return ltrywith(b, t->exn, h);
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(lam);
        Lam e3 = simplif(i->ifnot);
        Lam e2 = simplif(i->ifso);
        Lam e1 = simplif(i->cond);
        return lifthenelse(e1, e2, e3);
      }
      case LK::Lsequence: {
        auto* s = as<Lsequence>(lam);
        if (auto* u = as<Lifused>(s->l1)) {
          if (count_var(u->id) > 0) {
            Lam e2 = simplif(s->l2);
            Lam e1 = simplif(u->l);
            return lsequence(e1, e2);
          }
          return simplif(s->l2);
        }
        Lam e2 = simplif(s->l2);
        Lam e1 = simplif(s->l1);
        return lsequence(e1, e2);
      }
      case LK::Lwhile: {
        auto* w = as<Lwhile>(lam);
        Lam e2 = simplif(w->body);
        Lam e1 = simplif(w->cond);
        return lwhile(e1, e2);
      }
      case LK::Lfor: {
        auto* x = as<Lfor>(lam);
        Lam e3 = simplif(x->body);
        Lam e2 = simplif(x->hi);
        Lam e1 = simplif(x->lo);
        return lfor(x->id, e1, e2, x->dir, e3);
      }
      case LK::Lassign: {
        auto* a = as<Lassign>(lam);
        return lassign(a->id, simplif(a->e));
      }
      case LK::Lsend: {
        auto* s = as<Lsend>(lam);
        Slice<Lam> el = map_list(s->args, sf);
        Lam o = simplif(s->obj);
        Lam m = simplif(s->met);
        return lsend(s->k, m, o, el, s->loc);
      }
      case LK::Levent: {
        auto* e = as<Levent>(lam);
        return levent(simplif(e->l), e->ev);
      }
      case LK::Lifused: {
        auto* u = as<Lifused>(lam);
        return count_var(u->id) > 0 ? simplif(u->l) : lambda_unit();
      }
    }
    throw std::logic_error("Simplif.simplify_lets");
  }
};

Lam simplify_lets(Lam lam) {
  SimplifyLets s;
  SimplifyLets::Bv bv;
  s.count(bv, lam);
  return s.simplif(lam);
}

// ---- Simplify local let-bound functions ------------------------------------------
// If all occurrences are fully-applied function calls in the same "tail
// scope", replace the function by a staticcatch handler (on that scope).
struct Slot {
  const LFunction* func;
  Lam function_scope;
  Lam scope;  // nullptr = None
};

struct SimplifyLocalFunctions {
  std::deque<Slot> slot_store;
  IdentMap<std::vector<Slot*>> slots;  // Hashtbl: add shadows, remove the latest
  IdentMap<long> static_id;            // function id -> static id
  // LamTbl (physical keys): scope -> static functions on that scope, in
  // insertion order (find_all returns them most recent first)
  std::unordered_map<Lam, std::vector<std::pair<long, const LFunction*>>> static_;
  std::size_t static_length = 0;
  Lam current_scope;
  Lam current_function_scope;

  Slot* find_slot(Ident::t id) {
    auto it = slots.find(id);
    return it == slots.end() || it->second.empty() ? nullptr : it->second.back();
  }
  void remove_slot(Ident::t id) {
    auto it = slots.find(id);
    if (it != slots.end() && !it->second.empty()) {
      it->second.pop_back();
      if (it->second.empty()) slots.erase(it);
    }
  }

  static bool enabled(const FunctionAttribute& a) {
    using IK = InlineAttribute::Kind;
    switch (a.local) {
      case LocalAttribute::Always_local: return true;
      case LocalAttribute::Default_local:
        return a.inline_.kind == IK::Never_inline || a.inline_.kind == IK::Default_inline;
      case LocalAttribute::Never_local: return false;
    }
    return false;
  }

  void check_static(const LFunction* lf) {
    if (lf->attr.local == LocalAttribute::Always_local)
      location::prerr_warning(to_location(lf->loc),
                              warnings::Warning::with_s(warnings::Warning::K::Inlining_impossible,
                                                        "This function cannot be compiled into a static continuation"));
  }

  void tail(Lam lam) {
    if (auto* x = as<Llet>(lam)) {
      if (auto* fn = as<Lfunction>(x->arg); fn && enabled(fn->f->attr)) {
        const LFunction* lf = fn->f;
        slot_store.push_back(Slot{lf, current_function_scope, nullptr});
        slots[x->id].push_back(&slot_store.back());
        tail(x->body);
        Slot* s = find_slot(x->id);
        if (s && s->scope) {
          Lam scope = s->scope;
          long st = next_raise_count();
          // Do not move higher than current lambda
          Lam sc = scope == current_scope ? x->body : scope;
          static_id[x->id] = st;
          static_[sc].push_back({st, lf});
          ++static_length;
          // The body of the function will become an handler in that "scope".
          with_scope(scope, lf->body);
        } else {
          check_static(lf);
          // note: if scope = None, the function is unused
          function_definition(lf);
        }
        return;
      }
    }
    if (auto* a = as<Lapply>(lam)) {
      if (auto* f = as<Lvar>(a->ap.ap_func)) {
        Ident::t id = f->id;
        if (Slot* s = find_slot(id)) {
          if (!exact_application(s->func, a->ap.ap_args)) {
            // Wrong arity
            remove_slot(id);
          } else if (s->scope && s->scope != current_scope) {
            // Different "tail scope"
            remove_slot(id);
          } else if (s->function_scope != current_function_scope) {
            // Non local function
            remove_slot(id);
          } else if (!s->scope) {
            // First use of the function: remember the current tail scope
            s->scope = current_scope;
          }
        }
        for (auto x : a->ap.ap_args) non_tail(x);
        return;
      }
    }
    if (auto* v = as<Lvar>(lam)) {
      remove_slot(v->id);
      return;
    }
    if (auto* fn = as<Lfunction>(lam)) {
      check_static(fn->f);
      function_definition(fn->f);
      return;
    }
    shallow_iter([this](Lam l) { tail(l); }, [this](Lam l) { non_tail(l); }, lam);
  }
  void non_tail(Lam lam) { with_scope(lam, lam); }
  void function_definition(const LFunction* lf) {
    Lam old_function_scope = current_function_scope;
    current_function_scope = lf->body;
    non_tail(lf->body);
    current_function_scope = old_function_scope;
  }
  void with_scope(Lam scope, Lam lam) {
    Lam old_scope = current_scope;
    current_scope = scope;
    tail(lam);
    current_scope = old_scope;
  }

  Lam rewrite(Lam lam0) {
    Lam lam = nullptr;
    if (auto* x = as<Llet>(lam0); x && static_id.count(x->id)) {
      lam = rewrite(x->body);
    } else if (auto* a = as<Lapply>(lam0); a && as<Lvar>(a->ap.ap_func) && static_id.count(as<Lvar>(a->ap.ap_func)->id)) {
      Ident::t id = as<Lvar>(a->ap.ap_func)->id;
      long st = static_id.at(id);
      Slot* slot = find_slot(id);
      if (!slot) throw std::out_of_range("Simplif.simplify_local_functions: Not_found");
      auto exact_args = exact_application(slot->func, a->ap.ap_args);
      if (!exact_args) throw std::logic_error("Simplif.simplify_local_functions: exact_application");
      lam = lstaticraise(st, map_list(*exact_args, [this](Lam l) { return rewrite(l); }));
    } else {
      lam = shallow_map([this](Lam l) { return rewrite(l); }, lam0);
    }
    // List.fold_right over find_all (most recent first): the oldest binding
    // is applied first, innermost
    auto it = static_.find(lam0);
    if (it != static_.end())
      for (auto& [st, lf] : it->second) {
        Lam h = rewrite(lf->body);
        lam = lstaticcatch(lam, st, lf->params, h);
      }
    return lam;
  }
};

Lam simplify_local_functions(Lam lam) {
  SimplifyLocalFunctions s;
  // We keep track of the current "tail scope", identified by the outermost
  // lambda for which the current lambda is in tail position.
  s.current_scope = lam;
  // PR11383: We will only apply the transformation if we don't have to
  // move code across function boundaries
  s.current_function_scope = lam;
  s.tail(lam);
  if (s.static_length == 0) return lam;
  return s.rewrite(lam);
}

struct ExitSplit {};

}  // namespace

// Split a function with default parameters into a wrapper and an inner
// function.  The wrapper fills in missing optional parameters with their
// default value and tail-calls the inner function.
Slice<RecBinding> split_default_wrapper(Ident::t fun_id, FunctionKind kind, Slice<Param> params, ValueKind return_,
                                        Lam body, const FunctionAttribute& attr,
                                        const ScopedLocation& loc) {
  auto mem_assoc_params = [&](Ident::t x) {
    for (auto& p : params)
      if (ident::same(p.id, x)) return true;
    return false;
  };
  using Map = std::vector<std::pair<Ident::t, Ident::t>>;  // (optparam, id), most recent first
  std::function<std::pair<Lam, RecBinding>(const Map&, Lam)> aux =
      [&](const Map& map, Lam b) -> std::pair<Lam, RecBinding> {
    auto map_mem = [&](Ident::t x) {
      for (auto& [p, _] : map)
        if (ident::same(p, x)) return true;
      return false;
    };
    if (auto* x = as<Llet>(b); x && x->str == LetKind::Strict) {
      if (auto* ite = as<Lifthenelse>(x->arg)) {
        auto* isint = as<Lprim>(ite->cond);
        if (isint && isint->p.kind == PK::Pisint && isint->args.size() == 1) {
          if (auto* opt = as<Lvar>(isint->args[0])) {
            Ident::t optparam = opt->id;
            if (ident::name(optparam) == "*opt*" && mem_assoc_params(optparam) && !map_mem(optparam)) {
              Map map2;
              map2.push_back({optparam, x->id});
              map2.insert(map2.end(), map.begin(), map.end());
              auto [wrapper_body, inner] = aux(map2, x->body);
              return {llet(LetKind::Strict, x->k, x->id, x->arg, wrapper_body), inner};
            }
          }
        }
      }
    }
    if (map.empty()) throw ExitSplit{};
    // Check that those *opt* identifiers don't appear in the remaining
    // body.  This should not appear, but let's be on the safe side.
    IdentSet fv = free_variables(b);
    for (auto& [id, _] : map)
      if (fv.count(id)) throw ExitSplit{};

    Ident::t inner_id = Ident::create_local(std::string(ident::name(fun_id)) + "_inner");
    auto map_param = [&](Ident::t p) {
      for (auto& [q, id] : map)
        if (ident::same(q, p)) return id;
      return p;
    };
    std::vector<Lam> args;
    for (auto& p : params) args.push_back(lvar(map_param(p.id)));
    LambdaApply ap;
    ap.ap_func = lvar(inner_id);
    ap.ap_args = slice(args);
    ap.ap_loc = ScopedLocation{};
    ap.ap_tailcall = TailcallAttribute::Default_tailcall;
    ap.ap_inlined = InlineAttribute{};
    ap.ap_specialised = SpecialiseAttribute::Default_specialise;
    Lam wrapper_body = lapply(ap);
    std::vector<Ident::t> inner_params;
    for (auto& p : params) inner_params.push_back(map_param(p.id));
    std::vector<Ident::t> new_ids;
    for (Ident::t id : inner_params) new_ids.push_back(ident::rename(id));
    IdentMap<Ident::t> subst;
    for (std::size_t k = 0; k < inner_params.size(); ++k) subst[inner_params[k]] = new_ids[k];
    Lam body2 = rename(subst, b);
    std::vector<Param> ps;
    for (Ident::t id : new_ids) ps.push_back({id, ValueKind::gen()});
    const LFunction* inner_fun = lfunction_(FunctionKind::Curried, slice(ps), return_, body2, attr, loc);
    return {wrapper_body, RecBinding{inner_id, inner_fun}};
  };
  // (the usual function, whose body does not start with an optional
  // parameter's default: aux raises Exit before making anything -- decided
  // here without a C++ exception)
  auto starts_with_default = [&](Lam b) {
    auto* x = as<Llet>(b);
    if (!x || x->str != LetKind::Strict) return false;
    auto* ite = as<Lifthenelse>(x->arg);
    if (!ite) return false;
    auto* isint = as<Lprim>(ite->cond);
    if (!isint || isint->p.kind != PK::Pisint || isint->args.size() != 1) return false;
    auto* opt = as<Lvar>(isint->args[0]);
    return opt && ident::name(opt->id) == "*opt*" && mem_assoc_params(opt->id);
  };
  if (!starts_with_default(body))
    return slice({RecBinding{fun_id, lfunction_(kind, params, return_, body, attr, loc)}});
  try {
    auto [wbody, inner] = aux({}, body);
    FunctionAttribute sattr = default_stub_attribute();
    return slice({RecBinding{fun_id, lfunction_(kind, params, return_, wbody, sattr, loc)}, inner});
  } catch (const ExitSplit&) {
    return slice({RecBinding{fun_id, lfunction_(kind, params, return_, body, attr, loc)}});
  }
}

// The entry point: simplification + rewriting of tail-modulo-cons calls.
// (+ emission of tailcall annotations: warnings only, not run.)
// Tail call info in annotation files (and the Wrong_tailcall_expectation
// warning)
static void emit_tail_infos(bool is_tail, Lam lambda) {
  auto list = [](bool t, Slice<Lam> l) {
    for (Lam x : l) emit_tail_infos(t, x);
  };
  // emit_tail_infos_lfunction: entering a function resets [is_tail]
  auto lfunction = [](const LFunction* lf) { emit_tail_infos(true, lf->body); };
  switch (lambda->kind) {
    case LK::Lvar:
    case LK::Lmutvar:
    case LK::Lconst: return;
    case LK::Lapply: {
      const LambdaApply& ap = as<Lapply>(lambda)->ap;
      // Note: is_tail may over-approximate tail-callness (see simplif.ml)
      if (ap.ap_tailcall != TailcallAttribute::Default_tailcall) {
        bool expect_tail = ap.ap_tailcall == TailcallAttribute::Tailcall_expectation_true;
        if (is_tail != expect_tail) {
          warnings::Warning w = warnings::Warning::make(warnings::Warning::K::Wrong_tailcall_expectation);
          w.b = expect_tail;
          location::prerr_warning(to_location(ap.ap_loc), w);
        }
      }
      emit_tail_infos(false, ap.ap_func);
      list(false, ap.ap_args);
      return;
    }
    case LK::Lfunction: lfunction(as<Lfunction>(lambda)->f); return;
    case LK::Llet: {
      auto* x = as<Llet>(lambda);
      emit_tail_infos(false, x->arg);
      emit_tail_infos(is_tail, x->body);
      return;
    }
    case LK::Lmutlet: {
      auto* x = as<Lmutlet>(lambda);
      emit_tail_infos(false, x->arg);
      emit_tail_infos(is_tail, x->body);
      return;
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(lambda);
      for (auto& b : x->decl) lfunction(b.def);
      emit_tail_infos(is_tail, x->body);
      return;
    }
    case LK::Lprim: {
      auto* x = as<Lprim>(lambda);
      using PK_ = Primitive::K;
      if ((x->p.kind == PK_::Pbytes_to_string || x->p.kind == PK_::Pbytes_of_string) && x->args.size() == 1) {
        emit_tail_infos(is_tail, x->args[0]);
      } else if ((x->p.kind == PK_::Psequand || x->p.kind == PK_::Psequor) && x->args.size() == 2) {
        emit_tail_infos(false, x->args[0]);
        emit_tail_infos(is_tail, x->args[1]);
      } else {
        list(false, x->args);
      }
      return;
    }
    case LK::Lswitch: {
      auto* x = as<Lswitch>(lambda);
      emit_tail_infos(false, x->arg);
      for (auto& c : x->sw.sw_consts) emit_tail_infos(is_tail, c.action);
      for (auto& c : x->sw.sw_blocks) emit_tail_infos(is_tail, c.action);
      if (x->sw.sw_failaction) emit_tail_infos(is_tail, x->sw.sw_failaction);
      return;
    }
    case LK::Lstringswitch: {
      auto* x = as<Lstringswitch>(lambda);
      emit_tail_infos(false, x->arg);
      for (auto& c : x->cases) emit_tail_infos(is_tail, c.action);
      if (x->def) emit_tail_infos(is_tail, x->def);
      return;
    }
    case LK::Lstaticraise: list(false, as<Lstaticraise>(lambda)->args); return;
    case LK::Lstaticcatch: {
      auto* x = as<Lstaticcatch>(lambda);
      emit_tail_infos(is_tail, x->body);
      emit_tail_infos(is_tail, x->handler);
      return;
    }
    case LK::Ltrywith: {
      auto* x = as<Ltrywith>(lambda);
      emit_tail_infos(false, x->body);
      emit_tail_infos(is_tail, x->handler);
      return;
    }
    case LK::Lifthenelse: {
      auto* x = as<Lifthenelse>(lambda);
      emit_tail_infos(false, x->cond);
      emit_tail_infos(is_tail, x->ifso);
      emit_tail_infos(is_tail, x->ifnot);
      return;
    }
    case LK::Lsequence: {
      auto* x = as<Lsequence>(lambda);
      emit_tail_infos(false, x->l1);
      emit_tail_infos(is_tail, x->l2);
      return;
    }
    case LK::Lwhile: {
      auto* x = as<Lwhile>(lambda);
      emit_tail_infos(false, x->cond);
      emit_tail_infos(false, x->body);
      return;
    }
    case LK::Lfor: {
      auto* x = as<Lfor>(lambda);
      emit_tail_infos(false, x->lo);
      emit_tail_infos(false, x->hi);
      emit_tail_infos(false, x->body);
      return;
    }
    case LK::Lassign: emit_tail_infos(false, as<Lassign>(lambda)->e); return;
    case LK::Lsend: {
      auto* x = as<Lsend>(lambda);
      emit_tail_infos(false, x->met);
      emit_tail_infos(false, x->obj);
      list(false, x->args);
      return;
    }
    case LK::Levent: emit_tail_infos(is_tail, as<Levent>(lambda)->l); return;
    case LK::Lifused: emit_tail_infos(is_tail, as<Lifused>(lambda)->l); return;
  }
}

Lam simplify_lambda(Lam lam) {
  if (clflags::native_code || !clflags::debug) lam = simplify_local_functions(lam);
  lam = simplify_exits(lam);
  lam = simplify_lets(lam);
  lam = tmc::rewrite(lam);
  if (clflags::annotations || warnings::is_active(51)) emit_tail_infos(true, lam);
  return lam;
}

}  // namespace cppcaml::typing::simplif
