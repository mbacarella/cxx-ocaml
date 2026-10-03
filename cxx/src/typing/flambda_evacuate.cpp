// Evacuation of a flambda program (see flambda_evacuate.hpp).
#include "cppcaml/typing/flambda_evacuate.hpp"

#include <string>
#include <unordered_map>

#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::flambda_evacuate {

using namespace flambda;

namespace {

class Evacuator {
 public:
  explicit Evacuator(const std::vector<const Zone*>& dying) : dying_(dying) { memo_.reserve(1 << 16); }

  Program program(const Program& p) { return Program{set(p.imported_symbols), body(p.program_body)}; }

 private:
  bool dying(const void* p) const {
    if (!p) return false;
    for (const Zone* z : dying_)
      if (z->owns(static_cast<const char*>(p))) return true;
    return false;
  }
  [[noreturn]] static void leak(const char* what) {
    misc::fatal_error(std::string("Flambda_evacuate: ") + what + " in a dying zone");
  }

  // one copy per object
  template <class T, class F>
  const T* memo(const T* p, F copy) {
    if (!dying(p)) return p;
    if (auto it = memo_.find(p); it != memo_.end()) return static_cast<const T*>(it->second);
    const T* c = copy(*p);
    memo_.emplace(p, c);
    return c;
  }

  // (one copy per string, as per object: a view of another length is a
  // copy of its own)
  std::string_view str(std::string_view s) {
    if (!dying(s.data())) return s;
    auto it = strs_.find(s.data());
    if (it != strs_.end() && it->second.size() == s.size()) return it->second;
    std::string_view c = zone().str(s);
    if (it == strs_.end()) strs_.emplace(s.data(), c);
    return c;
  }
  template <class T, class F>
  Slice<T> slice(Slice<T> s, F elem) {
    if (s.empty()) return {};
    if (!dying(s.p)) return s;
    if (auto it = memo_.find(s.p); it != memo_.end()) return {static_cast<const T*>(it->second), s.n};
    T* c = static_cast<T*>(zone().alloc(sizeof(T) * s.n, alignof(T)));
    for (std::size_t k = 0; k < s.n; ++k) new (c + k) T(elem(s.p[k]));
    memo_.emplace(s.p, c);
    return {c, s.n};
  }
  template <class T>
  Slice<T> plain_slice(Slice<T> s) {
    return slice(s, [](const T& x) { return x; });
  }

  // the identifiers: never copied
  variable::t id(variable::t v) {
    if (dying(v) || (v && dying(v->name.data()))) leak("a variable");
    return v;
  }
  symbol::t id(symbol::t s) {
    if (dying(s)) leak("a symbol");
    return s;
  }
  unit_id::t id(unit_id::t u) {
    if (dying(u)) leak("a set of closures id");
    return u;
  }
  long id(long n) { return n; }
  const debuginfo::t& dbg(const debuginfo::t& d) {
    if (dying(d.p)) leak("a debuginfo list");
    return d;
  }

  // Map / Set: their trees' nodes (shared between maps) copied once
  template <class K, class Cmp>
  OSet<K, Cmp> set(const OSet<K, Cmp>& s) {
    return OSet<K, Cmp>(set_node<K, Cmp>(s.root()));
  }
  template <class K, class Cmp>
  const typename OSet<K, Cmp>::Node* set_node(const typename OSet<K, Cmp>::Node* n) {
    using Node = typename OSet<K, Cmp>::Node;
    return memo(n, [&](const Node& x) {
      return make<Node>(set_node<K, Cmp>(x.l), id(x.v), set_node<K, Cmp>(x.r), x.h);
    });
  }
  template <class K, class V, class Cmp, class F>
  OMap<K, V, Cmp> map(const OMap<K, V, Cmp>& m, F val) {
    return OMap<K, V, Cmp>(map_node<K, V, Cmp>(m.root(), val));
  }
  template <class K, class V, class Cmp, class F>
  const typename OMap<K, V, Cmp>::Node* map_node(const typename OMap<K, V, Cmp>::Node* n, F& val) {
    using Node = typename OMap<K, V, Cmp>::Node;
    return memo(n, [&](const Node& x) {
      auto l = map_node<K, V, Cmp>(x.l, val);
      auto r = map_node<K, V, Cmp>(x.r, val);
      return make<Node>(l, id(x.v), val(x.d), r, x.h);
    });
  }

  projection::t proj(projection::t p) {
    return memo(p, [&](const projection::T& x) { return make<projection::T>(x); });
  }
  SpecialisedTo spec(const SpecialisedTo& s) { return {id(s.var), proj(s.projection)}; }
  variable::Map<SpecialisedTo> spec_map(const variable::Map<SpecialisedTo>& m) {
    return map(m, [&](const SpecialisedTo& s) { return spec(s); });
  }

  const clambda::Primitive* prim(const clambda::Primitive* p) {
    return memo(p, [&](const clambda::Primitive& x) {
      clambda::Primitive c = x;
      c.sym = str(x.sym);
      c.shape.kinds = plain_slice(x.shape.kinds);
      if (dying(x.ccall) || dying(x.repr.extension) || dying(x.repr.obj)) leak("a primitive's description");
      return make<clambda::Primitive>(c);
    });
  }
  allocated_const::t allocated(allocated_const::t a) {
    return memo(a, [&](const AllocatedConst& x) {
      AllocatedConst c = x;
      c.floats = plain_slice(x.floats);
      c.s = str(x.s);
      return make<AllocatedConst>(c);
    });
  }

  const FunctionDeclaration* fun_decl(const FunctionDeclaration* d) {
    return memo(d, [&](const FunctionDeclaration& x) {
      FunctionDeclaration c = x;
      id(x.closure_origin);
      c.params = slice(x.params, [&](const Parameter& p) { return Parameter{id(p.var)}; });
      c.body = expr(x.body);
      c.free_variables = set(x.free_variables);
      c.free_symbols = set(x.free_symbols);
      dbg(x.dbg);
      return make<FunctionDeclaration>(c);
    });
  }
  const FunctionDeclarations* fun_decls(const FunctionDeclarations* d) {
    return memo(d, [&](const FunctionDeclarations& x) {
      FunctionDeclarations c = x;
      id(x.set_of_closures_id);
      id(x.set_of_closures_origin);
      c.funs = map(x.funs, [&](const FunctionDeclaration* f) { return fun_decl(f); });
      return make<FunctionDeclarations>(c);
    });
  }
  const SetOfClosures* set_of_closures(const SetOfClosures* s) {
    return memo(s, [&](const SetOfClosures& x) {
      SetOfClosures c = x;
      c.function_decls = fun_decls(x.function_decls);
      c.free_vars = spec_map(x.free_vars);
      c.specialised_args = spec_map(x.specialised_args);
      c.direct_call_surrogates = map(x.direct_call_surrogates, [&](variable::t v) { return id(v); });
      return make<SetOfClosures>(c);
    });
  }

  named nam(named n) {
    return memo(n, [&](const Named& x) -> named {
      switch (x.kind) {
        case NK::Symbol: {
          NSymbol c = static_cast<const NSymbol&>(x);
          id(c.sym);
          return make<NSymbol>(c);
        }
        case NK::Const: return make<NConst>(static_cast<const NConst&>(x));
        case NK::Allocated_const: {
          NAllocated_const c = static_cast<const NAllocated_const&>(x);
          c.c = allocated(c.c);
          return make<NAllocated_const>(c);
        }
        case NK::Read_mutable: {
          NRead_mutable c = static_cast<const NRead_mutable&>(x);
          id(c.var);
          return make<NRead_mutable>(c);
        }
        case NK::Read_symbol_field: {
          NRead_symbol_field c = static_cast<const NRead_symbol_field&>(x);
          id(c.sym);
          return make<NRead_symbol_field>(c);
        }
        case NK::Set_of_closures: {
          NSet_of_closures c = static_cast<const NSet_of_closures&>(x);
          c.set = set_of_closures(c.set);
          return make<NSet_of_closures>(c);
        }
        case NK::Project_closure: return make<NProject_closure>(static_cast<const NProject_closure&>(x));
        case NK::Move_within_set_of_closures:
          return make<NMove_within_set_of_closures>(static_cast<const NMove_within_set_of_closures&>(x));
        case NK::Project_var: return make<NProject_var>(static_cast<const NProject_var&>(x));
        case NK::Prim: {
          NPrim c = static_cast<const NPrim&>(x);
          c.prim = prim(c.prim);
          c.args = slice(c.args, [&](variable::t v) { return id(v); });
          dbg(c.dbg);
          return make<NPrim>(c);
        }
        case NK::Expr: {
          NExpr c = static_cast<const NExpr&>(x);
          c.expr = expr(c.expr);
          return make<NExpr>(c);
        }
      }
      misc::fatal_error("Flambda_evacuate.named");
    });
  }

  Slice<variable::t> vars(Slice<variable::t> s) {
    return slice(s, [&](variable::t v) { return id(v); });
  }
  Slice<SwitchCase> cases(Slice<SwitchCase> s) {
    return slice(s, [&](const SwitchCase& c) { return SwitchCase{c.key, expr(c.action)}; });
  }

  t expr(t e) {
    if (!dying(e)) return e;
    if (auto it = memo_.find(e); it != memo_.end()) return static_cast<t>(it->second);
    // a chain of [Let]s copied from its end (no recursion along it)
    if (e->kind == EK::Let) {
      std::vector<const Let*> chain;
      t cur = e;
      while (dying(cur) && cur->kind == EK::Let && !memo_.count(cur)) {
        chain.push_back(static_cast<const Let*>(cur));
        cur = static_cast<const Let*>(cur)->body;
      }
      t body = expr(cur);
      for (std::size_t k = chain.size(); k-- > 0;) {
        Let c = *chain[k];
        id(c.var);
        c.defining_expr = nam(c.defining_expr);
        c.body = body;
        c.free_vars_of_defining_expr = set(c.free_vars_of_defining_expr);
        c.free_vars_of_body = set(c.free_vars_of_body);
        body = make<Let>(c);
        memo_.emplace(chain[k], body);
      }
      return body;
    }
    t c = copy_expr(*e);
    memo_.emplace(e, c);
    return c;
  }
  t copy_expr(const Expr& x) {
    switch (x.kind) {
      case EK::Var: {
        Var c = static_cast<const Var&>(x);
        id(c.var);
        return make<Var>(c);
      }
      case EK::Let: misc::fatal_error("Flambda_evacuate.copy_expr");
      case EK::Let_mutable: {
        Let_mutable c = static_cast<const Let_mutable&>(x);
        c.body = expr(c.body);
        return make<Let_mutable>(c);
      }
      case EK::Apply: {
        Apply c = static_cast<const Apply&>(x);
        c.args = vars(c.args);
        dbg(c.dbg);
        return make<Apply>(c);
      }
      case EK::Send: {
        Send c = static_cast<const Send&>(x);
        c.args = vars(c.args);
        dbg(c.dbg);
        return make<Send>(c);
      }
      case EK::Assign: return make<Assign>(static_cast<const Assign&>(x));
      case EK::If_then_else: {
        If_then_else c = static_cast<const If_then_else&>(x);
        c.ifso = expr(c.ifso);
        c.ifnot = expr(c.ifnot);
        return make<If_then_else>(c);
      }
      case EK::Switch: {
        Switch c = static_cast<const Switch&>(x);
        c.numconsts = set(c.numconsts);
        c.consts = cases(c.consts);
        c.numblocks = set(c.numblocks);
        c.blocks = cases(c.blocks);
        c.failaction = expr(c.failaction);
        return make<Switch>(c);
      }
      case EK::String_switch: {
        String_switch c = static_cast<const String_switch&>(x);
        c.cases = slice(c.cases, [&](const StringCase& s) { return StringCase{str(s.s), expr(s.action)}; });
        c.def = expr(c.def);
        return make<String_switch>(c);
      }
      case EK::Static_raise: {
        Static_raise c = static_cast<const Static_raise&>(x);
        c.args = vars(c.args);
        return make<Static_raise>(c);
      }
      case EK::Static_catch: {
        Static_catch c = static_cast<const Static_catch&>(x);
        c.vars = slice(c.vars, [&](const CatchVar& v) { return CatchVar{id(v.var), v.kind}; });
        c.body = expr(c.body);
        c.handler = expr(c.handler);
        return make<Static_catch>(c);
      }
      case EK::Try_with: {
        Try_with c = static_cast<const Try_with&>(x);
        c.body = expr(c.body);
        c.handler = expr(c.handler);
        return make<Try_with>(c);
      }
      case EK::While: {
        While c = static_cast<const While&>(x);
        c.cond = expr(c.cond);
        c.body = expr(c.body);
        return make<While>(c);
      }
      case EK::For: {
        For c = static_cast<const For&>(x);
        c.body = expr(c.body);
        return make<For>(c);
      }
      case EK::Proved_unreachable: return make<Proved_unreachable>(static_cast<const Proved_unreachable&>(x));
    }
    misc::fatal_error("Flambda_evacuate.expr");
  }

  constant_defining_value cdv(constant_defining_value d) {
    return memo(d, [&](const ConstantDefiningValue& x) {
      ConstantDefiningValue c = x;
      c.c = allocated(x.c);
      c.fields = slice(x.fields, [&](const BlockField& f) {
        if (f.sym) id(f.sym);
        return f;
      });
      c.set = set_of_closures(x.set);
      return make<ConstantDefiningValue>(c);
    });
  }

  program_body body(program_body p) {
    // the program's spine copied from its end
    std::vector<program_body> spine;
    program_body cur = p;
    while (dying(cur) && !memo_.count(cur) && cur->kind != ProgramBody::Kind::End) {
      spine.push_back(cur);
      cur = cur->body;
    }
    program_body rest = cur;
    if (dying(cur) && !memo_.count(cur)) {  // End
      rest = make<ProgramBody>(*cur);
      memo_.emplace(cur, rest);
    } else if (dying(cur)) {
      rest = static_cast<program_body>(memo_.at(cur));
    }
    for (std::size_t k = spine.size(); k-- > 0;) {
      ProgramBody c = *spine[k];
      c.def = cdv(c.def);
      c.defs = slice(c.defs, [&](const SymbolBinding& b) { return SymbolBinding{id(b.sym), cdv(b.def)}; });
      c.fields = slice(c.fields, [&](t f) { return expr(f); });
      c.expr = expr(c.expr);
      c.body = rest;
      rest = make<ProgramBody>(c);
      memo_.emplace(spine[k], rest);
    }
    return rest;
  }

  const std::vector<const Zone*>& dying_;
  std::unordered_map<const void*, const void*> memo_;  // old object -> its copy
  std::unordered_map<const char*, std::string_view> strs_;
};

}  // namespace

Program evacuate(const Program& program, const std::vector<const Zone*>& dying) {
  return Evacuator(dying).program(program);
}

}  // namespace cppcaml::typing::flambda_evacuate
