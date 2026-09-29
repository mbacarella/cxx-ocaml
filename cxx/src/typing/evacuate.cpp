// Evacuation of the typing zone; see evacuate.hpp.
#include "cppcaml/typing/evacuate.hpp"

#include <functional>
#include <vector>

namespace cppcaml::typing::evacuate {

namespace L = lambda;

template <class T>
static T* mut(const T* p) {
  return const_cast<T*>(p);
}

std::string_view Evacuator::str(std::string_view s) {
  if (!owned(s.data())) return s;
  auto& by = strs_[reinterpret_cast<std::uintptr_t>(s.data())];
  auto [it, fresh] = by.try_emplace(s.size());
  if (fresh) it->second = zone().str(s);
  return it->second;
}

const void* Evacuator::identity(const void* p) {
  if (!owned(p)) return p;
  auto [it, fresh] = memo_.try_emplace(p, nullptr);
  if (fresh) it->second = fresh_identity();
  return it->second;
}

Ident::t Evacuator::ident(Ident::t id) {
  if (!owned(id)) return id;
  auto [it, fresh] = memo_.try_emplace(id, nullptr);
  if (fresh) {
    Ident* c = make<Ident>(*id);
    c->name_ = str(id->name_);
    it->second = c;
  }
  return static_cast<Ident::t>(it->second);
}

Path::t Evacuator::path(Path::t p) {
  if (!owned(p)) return p;
  auto [it, fresh] = memo_.try_emplace(p, nullptr);
  if (fresh) {
    Path* c = make<Path>(*p);
    c->id = ident(p->id);
    c->p1 = path(p->p1);
    c->p2 = path(p->p2);
    c->s = str(p->s);
    it->second = c;
  }
  return static_cast<Path::t>(it->second);
}

const PrimitiveDescription* Evacuator::prim_desc(const PrimitiveDescription* d) {
  if (!owned(d)) return d;
  auto [it, fresh] = memo_.try_emplace(d, nullptr);
  if (fresh) {
    auto* c = make<PrimitiveDescription>(*d);
    c->prim_name = str(d->prim_name);
    c->prim_native_name = str(d->prim_native_name);
    c->prim_native_repr_args = slice_copy(d->prim_native_repr_args);
    it->second = c;
  }
  return static_cast<const PrimitiveDescription*>(it->second);
}

template <class T>
Slice<T> Evacuator::slice_copy(Slice<T> s) {
  if (s.empty() || !owned(s.begin())) return s;
  auto [it, fresh] = memo_.try_emplace(s.begin(), nullptr);
  if (fresh) it->second = slice(std::vector<T>(s.begin(), s.end())).begin();
  return Slice<T>{static_cast<const T*>(it->second), s.size()};
}

Location Evacuator::location(const Location& l) {
  Location r = l;
  // the positions' file names are permanent handles; their identities go
  r.loc_start.obj = static_cast<const Position*>(identity(l.loc_start.obj));
  r.loc_end.obj = static_cast<const Position*>(identity(l.loc_end.obj));
  r.obj = static_cast<const Location*>(identity(l.obj));
  return r;
}

debuginfo::scopes Evacuator::scopes(debuginfo::scopes s) {
  if (!owned(s)) return s;
  auto [it, fresh] = memo_.try_emplace(s, nullptr);
  if (fresh) {
    auto* c = make<debuginfo::Scopes>(*s);
    c->str = str(s->str);
    c->str_fun = str(s->str_fun);
    it->second = c;
  }
  return static_cast<debuginfo::scopes>(it->second);
}

debuginfo::ScopedLocation Evacuator::scoped_location(const debuginfo::ScopedLocation& l) {
  debuginfo::ScopedLocation r = l;
  if (!l.known()) return r;
  // the location copied once (it is shared) when it or an identity it
  // holds is in the dying zone
  const Location& o = l.loc();
  if (owned(l.locp) || owned(o.obj) || owned(o.loc_start.obj) || owned(o.loc_end.obj)) {
    auto [it, fresh] = locs_.try_emplace(l.locp, nullptr);
    if (fresh) it->second = make<Location>(location(o));
    r.locp = it->second;
  }
  r.sc = scopes(l.sc);
  return r;
}

const L::StructuredConstant* Evacuator::constant(const L::StructuredConstant* c) {
  if (!owned(c)) return c;
  auto [it, fresh] = memo_.try_emplace(c, nullptr);
  if (fresh) {
    auto* n = make<L::StructuredConstant>(*c);
    n->box = identity(c->box);
    n->s = str(c->s);
    std::vector<const L::StructuredConstant*> fs;
    for (auto* f : c->fields) fs.push_back(constant(f));
    n->fields = c->fields.empty() ? c->fields : slice(fs);
    std::vector<std::string_view> fl;
    for (auto s : c->floats) fl.push_back(str(s));
    n->floats = c->floats.empty() ? c->floats : slice(fl);
    it->second = n;
  }
  return static_cast<const L::StructuredConstant*>(it->second);
}

L::Primitive Evacuator::primitive(const L::Primitive& p) {
  L::Primitive r = p;
  r.id = ident(p.id);
  r.shape.kinds = slice_copy(p.shape.kinds);
  r.repr.extension = path(p.repr.extension);
  r.repr.obj = identity(p.repr.obj);
  r.ccall = prim_desc(p.ccall);
  return r;
}

const L::LFunction* Evacuator::lfunction(const L::LFunction* f) {
  if (!f) return f;
  const L::LFunction* g = f;
  if (owned(f)) {
    auto [it, fresh] = memo_.try_emplace(f, nullptr);
    if (!fresh) return static_cast<const L::LFunction*>(it->second);
    g = make<L::LFunction>(*f);
    it->second = g;
  } else if (!seen_.insert(reinterpret_cast<L::lambda>(f)).second) {
    return f;  // (an LFunction outside the zone, already rewritten)
  }
  auto* m = mut(g);
  std::vector<L::Param> ps;
  for (const L::Param& x : f->params) ps.push_back({ident(x.id), x.kind});
  m->params = f->params.empty() ? f->params : slice(ps);
  m->loc = scoped_location(f->loc);
  lambda(f->body);
  return g;
}

void Evacuator::lambda(L::lambda l) {
  if (!l || !seen_.insert(l).second) return;
  auto lams = [&](Slice<L::lambda> s) {
    for (L::lambda x : s) lambda(x);
    return slice_copy(s);
  };
  switch (l->kind) {
    case L::LK::Lvar: mut(L::as<L::Lvar>(l))->id = ident(L::as<L::Lvar>(l)->id); break;
    case L::LK::Lmutvar: mut(L::as<L::Lmutvar>(l))->id = ident(L::as<L::Lmutvar>(l)->id); break;
    case L::LK::Lconst: mut(L::as<L::Lconst>(l))->c = constant(L::as<L::Lconst>(l)->c); break;
    case L::LK::Lapply: {
      auto* n = mut(L::as<L::Lapply>(l));
      lambda(n->ap.ap_func);
      n->ap.ap_args = lams(n->ap.ap_args);
      n->ap.ap_loc = scoped_location(n->ap.ap_loc);
      break;
    }
    case L::LK::Lfunction: {
      auto* n = mut(L::as<L::Lfunction>(l));
      n->f = lfunction(n->f);
      break;
    }
    case L::LK::Llet: {
      auto* n = mut(L::as<L::Llet>(l));
      n->id = ident(n->id);
      lambda(n->arg);
      lambda(n->body);
      break;
    }
    case L::LK::Lmutlet: {
      auto* n = mut(L::as<L::Lmutlet>(l));
      n->id = ident(n->id);
      lambda(n->arg);
      lambda(n->body);
      break;
    }
    case L::LK::Lletrec: {
      auto* n = mut(L::as<L::Lletrec>(l));
      std::vector<L::RecBinding> bs;
      for (const L::RecBinding& b : n->decl) bs.push_back({ident(b.id), lfunction(b.def)});
      n->decl = n->decl.empty() ? n->decl : slice(bs);
      lambda(n->body);
      break;
    }
    case L::LK::Lprim: {
      auto* n = mut(L::as<L::Lprim>(l));
      n->p = primitive(n->p);
      n->args = lams(n->args);
      n->loc = scoped_location(n->loc);
      break;
    }
    case L::LK::Lswitch: {
      auto* n = mut(L::as<L::Lswitch>(l));
      lambda(n->arg);
      for (const L::SwitchCase& c : n->sw.sw_consts) lambda(c.action);
      for (const L::SwitchCase& c : n->sw.sw_blocks) lambda(c.action);
      n->sw.sw_consts = slice_copy(n->sw.sw_consts);
      n->sw.sw_blocks = slice_copy(n->sw.sw_blocks);
      lambda(n->sw.sw_failaction);
      n->loc = scoped_location(n->loc);
      break;
    }
    case L::LK::Lstringswitch: {
      auto* n = mut(L::as<L::Lstringswitch>(l));
      lambda(n->arg);
      std::vector<L::StringCase> cs;
      for (const L::StringCase& c : n->cases) {
        lambda(c.action);
        cs.push_back({str(c.s), c.action});
      }
      n->cases = n->cases.empty() ? n->cases : slice(cs);
      lambda(n->def);
      n->loc = scoped_location(n->loc);
      break;
    }
    case L::LK::Lstaticraise: {
      auto* n = mut(L::as<L::Lstaticraise>(l));
      n->args = lams(n->args);
      break;
    }
    case L::LK::Lstaticcatch: {
      auto* n = mut(L::as<L::Lstaticcatch>(l));
      lambda(n->body);
      std::vector<L::Param> ps;
      for (const L::Param& x : n->params) ps.push_back({ident(x.id), x.kind});
      n->params = n->params.empty() ? n->params : slice(ps);
      lambda(n->handler);
      break;
    }
    case L::LK::Ltrywith: {
      auto* n = mut(L::as<L::Ltrywith>(l));
      lambda(n->body);
      n->exn = ident(n->exn);
      lambda(n->handler);
      break;
    }
    case L::LK::Lifthenelse: {
      auto* n = L::as<L::Lifthenelse>(l);
      lambda(n->cond);
      lambda(n->ifso);
      lambda(n->ifnot);
      break;
    }
    case L::LK::Lsequence: {
      auto* n = L::as<L::Lsequence>(l);
      lambda(n->l1);
      lambda(n->l2);
      break;
    }
    case L::LK::Lwhile: {
      auto* n = L::as<L::Lwhile>(l);
      lambda(n->cond);
      lambda(n->body);
      break;
    }
    case L::LK::Lfor: {
      auto* n = mut(L::as<L::Lfor>(l));
      n->id = ident(n->id);
      lambda(n->lo);
      lambda(n->hi);
      lambda(n->body);
      break;
    }
    case L::LK::Lassign: {
      auto* n = mut(L::as<L::Lassign>(l));
      n->id = ident(n->id);
      lambda(n->e);
      break;
    }
    case L::LK::Lsend: {
      auto* n = mut(L::as<L::Lsend>(l));
      lambda(n->met);
      lambda(n->obj);
      n->args = lams(n->args);
      n->loc = scoped_location(n->loc);
      break;
    }
    case L::LK::Levent: {
      auto* n = mut(L::as<L::Levent>(l));
      lambda(n->l);
      if (n->ev) {
        // (the native back end reads an event's location only: its type and
        // environment are the typing phase's)
        auto* e = make<L::LambdaEvent>(*n->ev);
        e->lev_loc = scoped_location(e->lev_loc);
        if (owned(e->lev_after_type)) e->lev_after_type = nullptr;
        if (owned(e->lev_env)) e->lev_env = nullptr;
        if (owned(e->lev_repr)) e->lev_repr = make<L::IntRef>(*e->lev_repr);
        n->ev = e;
      }
      break;
    }
    case L::LK::Lifused: {
      auto* n = mut(L::as<L::Lifused>(l));
      n->id = ident(n->id);
      lambda(n->l);
      break;
    }
  }
}

std::vector<std::string> Evacuator::leftovers(L::lambda root) {
  std::vector<std::string> out;
  std::unordered_set<L::lambda> seen;
  auto bad_id = [&](Ident::t id) { return owned(id) || (id && owned(id->name_.data())); };
  auto bad_loc = [&](const debuginfo::ScopedLocation& l) {
    return l.known() && (owned(l.sc) || owned(l.locp) || owned(l.loc().loc_start.pos_fname.data()));
  };
  std::function<void(L::lambda)> go = [&](L::lambda l) {
    if (!l || !seen.insert(l).second) return;
    auto flag = [&](const char* what) { out.push_back(std::to_string(static_cast<int>(l->kind)) + ": " + what); };
    if (owned(l)) flag("node");
    switch (l->kind) {
      case L::LK::Lvar: if (bad_id(L::as<L::Lvar>(l)->id)) flag("Lvar id"); break;
      case L::LK::Lmutvar: if (bad_id(L::as<L::Lmutvar>(l)->id)) flag("Lmutvar id"); break;
      case L::LK::Lconst: if (owned(L::as<L::Lconst>(l)->c)) flag("Lconst"); break;
      case L::LK::Lapply: {
        auto* n = L::as<L::Lapply>(l);
        if (owned(n->ap.ap_args.begin())) flag("Lapply args");
        if (bad_loc(n->ap.ap_loc)) flag("Lapply loc");
        go(n->ap.ap_func);
        for (auto x : n->ap.ap_args) go(x);
        break;
      }
      case L::LK::Lfunction: {
        auto* f = L::as<L::Lfunction>(l)->f;
        if (owned(f)) flag("LFunction");
        for (auto& p : f->params) if (bad_id(p.id)) flag("param");
        if (owned(f->params.begin())) flag("params");
        go(f->body);
        break;
      }
      case L::LK::Llet: { auto* n = L::as<L::Llet>(l); if (bad_id(n->id)) flag("Llet id"); go(n->arg); go(n->body); break; }
      case L::LK::Lmutlet: { auto* n = L::as<L::Lmutlet>(l); if (bad_id(n->id)) flag("Lmutlet id"); go(n->arg); go(n->body); break; }
      case L::LK::Lletrec: {
        auto* n = L::as<L::Lletrec>(l);
        if (owned(n->decl.begin())) flag("Lletrec decl");
        for (auto& b : n->decl) { if (bad_id(b.id) || owned(b.def)) flag("rec binding"); go(b.def->body); }
        go(n->body);
        break;
      }
      case L::LK::Lprim: {
        auto* n = L::as<L::Lprim>(l);
        if (bad_id(n->p.id) || owned(n->p.ccall) || owned(n->p.repr.extension)) flag("Lprim prim");
        if (owned(n->args.begin())) flag("Lprim args");
        if (bad_loc(n->loc)) flag("Lprim loc");
        for (auto x : n->args) go(x);
        break;
      }
      case L::LK::Lswitch: {
        auto* n = L::as<L::Lswitch>(l);
        go(n->arg);
        for (auto& c : n->sw.sw_consts) go(c.action);
        for (auto& c : n->sw.sw_blocks) go(c.action);
        go(n->sw.sw_failaction);
        break;
      }
      case L::LK::Lstringswitch: {
        auto* n = L::as<L::Lstringswitch>(l);
        go(n->arg);
        for (auto& c : n->cases) { if (owned(c.s.data())) flag("case string"); go(c.action); }
        go(n->def);
        break;
      }
      case L::LK::Lstaticraise: for (auto x : L::as<L::Lstaticraise>(l)->args) go(x); break;
      case L::LK::Lstaticcatch: {
        auto* n = L::as<L::Lstaticcatch>(l);
        for (auto& p : n->params) if (bad_id(p.id)) flag("catch param");
        go(n->body);
        go(n->handler);
        break;
      }
      case L::LK::Ltrywith: { auto* n = L::as<L::Ltrywith>(l); if (bad_id(n->exn)) flag("exn"); go(n->body); go(n->handler); break; }
      case L::LK::Lifthenelse: { auto* n = L::as<L::Lifthenelse>(l); go(n->cond); go(n->ifso); go(n->ifnot); break; }
      case L::LK::Lsequence: { auto* n = L::as<L::Lsequence>(l); go(n->l1); go(n->l2); break; }
      case L::LK::Lwhile: { auto* n = L::as<L::Lwhile>(l); go(n->cond); go(n->body); break; }
      case L::LK::Lfor: { auto* n = L::as<L::Lfor>(l); if (bad_id(n->id)) flag("for id"); go(n->lo); go(n->hi); go(n->body); break; }
      case L::LK::Lassign: { auto* n = L::as<L::Lassign>(l); if (bad_id(n->id)) flag("assign id"); go(n->e); break; }
      case L::LK::Lsend: { auto* n = L::as<L::Lsend>(l); go(n->met); go(n->obj); for (auto x : n->args) go(x); break; }
      case L::LK::Levent: go(L::as<L::Levent>(l)->l); break;
      case L::LK::Lifused: { auto* n = L::as<L::Lifused>(l); if (bad_id(n->id)) flag("ifused id"); go(n->l); break; }
    }
  };
  go(root);
  return out;
}

void Evacuator::program(L::Program& p) {
  p.module_ident = ident(p.module_ident);
  L::IdentSet g;
  for (Ident::t id : p.required_globals) g.insert(ident(id));
  p.required_globals = std::move(g);
  lambda(p.code);
}

}  // namespace cppcaml::typing::evacuate
