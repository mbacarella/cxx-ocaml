// Port of typing/value_rec_check.ml: the static check of recursive
// definitions ("A practical mode system for recursive definitions",
// Reynaud, Scherer and Yallop, POPL 2021).
//
// Two criteria decide whether `let rec x1 = e1 and ... and xn = en` is
// valid: whether each ei has a static size (classify_expression), and how
// each ei uses the recursive variables (the mode judgments below).
//
// A judgment `G |- e : m` is a term_judg (Mode -> Env), a binding judgment
// `G |- ... : m -| G'` a bind_judg (Mode -> Env -> Env).  As in the OCaml
// code, building a judgment does part of its work eagerly (the recursive
// calls outside `fun mode -> ..`, and the Typeopt queries, which expand
// types) and the rest when it is applied to a mode; the port keeps that
// staging and its evaluation order (list literals right to left).
#include "cppcaml/typing/value_rec_check.hpp"

#include <functional>
#include <map>

#include "cppcaml/typing/typeopt.hpp"

namespace cppcaml::typing::value_rec_check {

using namespace types;
namespace tt = typedtree;
using tt::as;
using XK = tt::ExpressionDesc::Kind;
using PK = tt::PatternDesc::Kind;
using MK = tt::ModuleExprDesc::Kind;
using CK = tt::ModuleCoercion::Kind;
using SK = tt::StructureItemDesc::Kind;
using ClK = tt::ClassExprDesc::Kind;
using CfK = tt::ClassFieldDesc::Kind;
using Sd = tt::RecursiveBindingKind;

// ---- static or dynamic size -------------------------------------------------------------
static bool is_ref(const ValueDescription* vd) {
  return vd->val_kind.kind == ValueKind::Kind::Val_prim && vd->val_kind.prim->prim_name == "%makemutable" &&
         vd->val_kind.prim->prim_arity == 1;
}

// See the note on abstracted arguments in the documentation for
// Typedtree.Texp_apply
static bool is_abstracted_arg(const tt::LabeledArg& a) { return a.arg.omitted; }

struct IdentLess {
  bool operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b) < 0; }
};
// Ident.tbl of sizes (Ident.add / Ident.find_same)
using SizeEnv = std::map<Ident::t, Sd, IdentLess>;

static Sd classify_expression(const SizeEnv& env, const tt::Expression* e);

// We use a non-recursive classification, classifying each binding with
// respect to the old environment (before all definitions), even if the
// bindings are recursive.
static SizeEnv classify_value_bindings(RecFlag, const SizeEnv& env, Slice<const tt::ValueBinding*> bindings) {
  const SizeEnv& old_env = env;
  SizeEnv out = env;
  for (auto* vb : bindings) {
    if (auto* v = as<tt::Tpat_var>(vb->vb_pat->pat_desc)) {
      Sd size = classify_expression(old_env, vb->vb_expr);
      out[v->id] = size;
    }
    // Note: we don't try to compute any size for complex patterns
  }
  return out;
}

static Sd classify_path(const SizeEnv& env, Path::t p) {
  if (p->kind == Path::Kind::Pident) {
    for (auto& [id, sd] : env)
      if (ident::same(id, p->id)) return sd;
    // an identifier will be missing from the map if either:
    // - it is a non-local identifier (bound outside the letrec-binding we
    //   are analyzing)
    // - or it is bound by a complex (let p = e in ...) local binding
    // - or it is bound within a module (let module M = ... in ...) that we
    //   are not traversing for size computation
    return Sd::Dynamic;
  }
  // local modules could have such paths to local definitions;
  // classify_expression could be extend to compute module shapes more
  // precisely
  return Sd::Dynamic;
}

static Sd classify_module_expression(const SizeEnv& env, const tt::ModuleExpr* mexp) {
  const tt::ModuleExprDesc* d = mexp->mod_desc;
  switch (d->kind) {
    case MK::Tmod_ident: return classify_path(env, as<tt::Tmod_ident>(d)->path);
    case MK::Tmod_structure:
    case MK::Tmod_functor: return Sd::Static;
    case MK::Tmod_apply:
    case MK::Tmod_apply_unit: return Sd::Dynamic;
    case MK::Tmod_constraint: {
      auto* c = as<tt::Tmod_constraint>(d);
      switch (c->coercion->kind) {
        case CK::Tcoerce_none: return classify_module_expression(env, c->me);
        case CK::Tcoerce_structure:
        case CK::Tcoerce_functor: return Sd::Static;
        case CK::Tcoerce_primitive: throw std::logic_error("letrec: primitive coercion on a module");
        case CK::Tcoerce_alias: throw std::logic_error("letrec: alias coercion on a module");
      }
      break;
    }
    case MK::Tmod_unpack: return classify_expression(env, as<tt::Tmod_unpack>(d)->exp);
  }
  throw std::logic_error("classify_module_expression");
}

// We need to keep track of the size of expressions bound by local
// declarations, to be able to predict the size of variables (see
// value_rec_check.ml).
static Sd classify_expression(const SizeEnv& env, const tt::Expression* e) {
  const tt::ExpressionDesc* d = e->exp_desc;
  switch (d->kind) {
    // binding and variable cases
    case XK::Texp_let: {
      auto* l = as<tt::Texp_let>(d);
      SizeEnv env2 = classify_value_bindings(l->rec, env, l->vbs);
      return classify_expression(env2, l->body);
    }
    case XK::Texp_ident: return classify_path(env, as<tt::Texp_ident>(d)->path);
    // non-binding cases
    case XK::Texp_sequence: return classify_expression(env, as<tt::Texp_sequence>(d)->e2);
    case XK::Texp_struct_item: return classify_expression(env, as<tt::Texp_struct_item>(d)->body);
    case XK::Texp_construct: {
      auto* c = as<tt::Texp_construct>(d);
      if (c->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_unboxed && c->args.size() == 1)
        return classify_expression(env, c->args[0]);
      return Sd::Static;
    }
    case XK::Texp_record: {
      auto* r = as<tt::Texp_record>(d);
      if (r->representation.kind == RecordRepresentation::Kind::Record_unboxed && r->fields.size() == 1 &&
          !r->fields[0].def.kept)
        return classify_expression(env, r->fields[0].def.exp);
      return Sd::Static;
    }
    case XK::Texp_variant:
    case XK::Texp_tuple:
    case XK::Texp_atomic_loc:
    case XK::Texp_extension_constructor:
    case XK::Texp_constant: return Sd::Static;
    case XK::Texp_for:
    case XK::Texp_setfield:
    case XK::Texp_while:
    case XK::Texp_setinstvar:
      // Unit-returning expressions
      return Sd::Static;
    case XK::Texp_unreachable: return Sd::Static;
    case XK::Texp_apply: {
      auto* a = as<tt::Texp_apply>(d);
      if (auto* i = as<tt::Texp_ident>(a->fn->exp_desc); i && is_ref(i->vd)) return Sd::Static;
      for (auto& arg : a->args)
        if (is_abstracted_arg(arg)) return Sd::Static;
      return Sd::Dynamic;
    }
    case XK::Texp_array: return Sd::Static;
    case XK::Texp_pack: return classify_module_expression(env, as<tt::Texp_pack>(d)->me);
    case XK::Texp_function: return Sd::Static;
    case XK::Texp_lazy: {
      const tt::Expression* le = as<tt::Texp_lazy>(d)->exp;
      // The code below was copied (in part) from translcore.ml
      switch (typeopt::classify_lazy_argument(le)) {
        case typeopt::LazyArgument::Constant_or_function:
          // A constant expr (of type <> float if [Config.flat_float_array]
          // is true) gets compiled as itself.
          return classify_expression(env, le);
        case typeopt::LazyArgument::Float_that_cannot_be_shortcut:
        case typeopt::LazyArgument::Identifier_forward_value:
          // Forward blocks
          return Sd::Static;
        case typeopt::LazyArgument::Identifier_other: return classify_expression(env, le);
        case typeopt::LazyArgument::Other:
          // other cases compile to a lazy block holding a function
          return Sd::Static;
      }
      throw std::logic_error("classify_expression: Texp_lazy");
    }
    case XK::Texp_new:
    case XK::Texp_instvar:
    case XK::Texp_object:
    case XK::Texp_match:
    case XK::Texp_ifthenelse:
    case XK::Texp_send:
    case XK::Texp_field:
    case XK::Texp_assert:
    case XK::Texp_try:
    case XK::Texp_override:
    case XK::Texp_letop: return Sd::Dynamic;
  }
  throw std::logic_error("classify_expression");
}

// ---- usage of recursive variables -------------------------------------------------------
// Ignore < Delay < Guard < Return < Dereference (the enum order is the rank)
enum class Mode { Ignore, Delay, Guard, Return, Dereference };
static int rank(Mode m) { return static_cast<int>(m); }
// the more conservative (highest-ranking) mode of the two
static Mode join(Mode m, Mode m2) { return rank(m) >= rank(m2) ? m : m2; }
// If x is used with the mode m in e[x], and e[x] is used with mode m' in
// e'[e[x]], then x is used with mode m'[m] = compose m' m in e'[e[x]].
static Mode compose(Mode m2, Mode m) {
  if (m2 == Mode::Ignore || m == Mode::Ignore) return Mode::Ignore;
  switch (m2) {
    case Mode::Dereference: return Mode::Dereference;
    case Mode::Delay: return Mode::Delay;
    case Mode::Guard: return m == Mode::Return ? Mode::Guard : m;
    case Mode::Return: return m;
    case Mode::Ignore: break;
  }
  return Mode::Ignore;
}

// A "t" maps each rec-bound variable to an access status (Ident.Map)
using VEnv = std::map<Ident::t, Mode, IdentLess>;
namespace venv {
static Mode find(Ident::t id, const VEnv& tbl) {
  auto it = tbl.find(id);
  return it == tbl.end() ? Mode::Ignore : it->second;
}
static VEnv join(const VEnv& x, const VEnv& y) {
  VEnv r = x;
  for (auto& [id, v2] : y) {
    auto it = r.find(id);
    if (it == r.end()) r.emplace(id, v2);
    else it->second = value_rec_check::join(it->second, v2);
  }
  return r;
}
static VEnv join_list(const std::vector<VEnv>& li) {
  VEnv r;
  for (auto& e : li) r = join(r, e);
  return r;
}
static VEnv compose(Mode m, const VEnv& env) {
  VEnv r;
  for (auto& [id, v] : env) r.emplace(id, value_rec_check::compose(m, v));
  return r;
}
// Ident.Map.equal Mode.equal
static bool equal(const VEnv& a, const VEnv& b) {
  if (a.size() != b.size()) return false;
  for (auto ia = a.begin(), ib = b.begin(); ia != a.end(); ++ia, ++ib)
    if (ident::compare(ia->first, ib->first) != 0 || ia->second != ib->second) return false;
  return true;
}
static VEnv single(Ident::t id, Mode mode) { return VEnv{{id, mode}}; }
// the identifiers of li that are dereferenced or returned in env
static std::vector<Ident::t> unguarded(const VEnv& env, const std::vector<Ident::t>& li) {
  std::vector<Ident::t> r;
  for (auto* id : li)
    if (rank(find(id, env)) > rank(Mode::Guard)) r.push_back(id);
  return r;
}
// the identifiers of li that are used in env (not ignored)
static std::vector<Ident::t> dependent(const VEnv& env, const std::vector<Ident::t>& li) {
  std::vector<Ident::t> r;
  for (auto* id : li)
    if (rank(find(id, env)) > rank(Mode::Ignore)) r.push_back(id);
  return r;
}
static VEnv remove(Ident::t id, VEnv env) {
  env.erase(id);
  return env;
}
static std::pair<Mode, VEnv> take(Ident::t id, const VEnv& env) { return {find(id, env), remove(id, env)}; }
static VEnv remove_list(const std::vector<Ident::t>& l, VEnv env) {
  for (auto* id : l) env.erase(id);
  return env;
}
}  // namespace venv

static VEnv remove_pat(const tt::Pattern* pat, const VEnv& env) {
  return venv::remove_list(tt::pat_bound_idents(pat), env);
}
static VEnv remove_patlist(const std::vector<const tt::Pattern*>& pats, VEnv env) {
  // List.fold_right remove_pat pats env
  for (std::size_t k = pats.size(); k-- > 0;) env = remove_pat(pats[k], env);
  return env;
}

using TermJudg = std::function<VEnv(Mode)>;
using BindJudg = std::function<VEnv(Mode, const VEnv&)>;

template <class A, class F>
static TermJudg option_j(F f, A o) {
  return [f, o](Mode m) { return o ? f(o)(m) : VEnv{}; };
}
// list f li m = List.fold_left (fun env item -> Env.join env (f item m)) Env.empty li
template <class A, class F>
static TermJudg list_j(F f, std::vector<A> li) {
  return [f, li](Mode m) {
    VEnv env;
    for (auto& item : li) env = venv::join(env, f(item)(m));
    return env;
  };
}
static TermJudg single_j(Ident::t id) {
  return [id](Mode m) { return venv::single(id, m); };
}
static TermJudg remove_ids(std::vector<Ident::t> ids, TermJudg f) {
  return [ids, f](Mode m) { return venv::remove_list(ids, f(m)); };
}
static TermJudg join_j(std::vector<TermJudg> li) {
  return [li](Mode m) {
    std::vector<VEnv> envs;
    for (auto& f : li) envs.push_back(f(m));
    return venv::join_list(envs);
  };
}
static VEnv empty_env(Mode) { return VEnv{}; }
static const TermJudg empty_j = empty_env;
// judg << m: evaluate [judg] in the composed mode m'[m]
static TermJudg operator<<(TermJudg f, Mode inner_mode) {
  return [f, inner_mode](Mode outer_mode) { return f(compose(outer_mode, inner_mode)); };
}
// binder >> judg: the inner environment is the one [judg] returns in the
// ambient mode
static TermJudg operator>>(BindJudg binder, TermJudg term) {
  return [binder, term](Mode mode) { return binder(mode, term(mode)); };
}

static TermJudg expression(const tt::Expression* exp);
static TermJudg function_body(const tt::FunctionBody* body);
static TermJudg binding_op(const tt::BindingOp* bop);
static TermJudg class_structure(const tt::ClassStructure* cs);
static TermJudg class_field(const tt::ClassField* cf);
static TermJudg class_field_kind(const tt::ClassFieldKind& cfk);
static TermJudg modexp(const tt::ModuleExpr* mexp);
static TermJudg path(Path::t pth);
static TermJudg structure(const tt::Structure* s);
static BindJudg structure_item(const tt::StructureItem* s);
static BindJudg module_binding(Ident::t id, const tt::ModuleExpr* mexp);
static BindJudg open_declaration(const tt::OpenDeclaration* od);
static BindJudg recursive_module_bindings(std::vector<std::pair<Ident::t, const tt::ModuleExpr*>> m_bindings);
static TermJudg class_expr(const tt::ClassExpr* ce);
static TermJudg extension_constructor(const tt::TExtensionConstructor* ec);
static BindJudg value_bindings(RecFlag rec_flag, Slice<const tt::ValueBinding*> bindings);
static std::function<std::pair<VEnv, Mode>(Mode)> case_(const tt::Case* c);
static Mode pattern(const tt::Pattern* pat, const VEnv& env);
static bool is_destructuring_pattern(const tt::Pattern* pat);

// Case environments (case_env c m = fst (case c m))
static TermJudg case_env(const tt::Case* c) {
  return [c](Mode m) { return case_(c)(m).first; };
}

// Expression judgment: G |- e : m, where m is an input and G an output.
static TermJudg expression(const tt::Expression* exp) {
  const tt::ExpressionDesc* d = exp->exp_desc;
  switch (d->kind) {
    case XK::Texp_ident: return path(as<tt::Texp_ident>(d)->path);
    case XK::Texp_let: {
      //  G  |- <bindings> : m -| G'
      //  G' |- body : m
      //  -------------------------------
      //  G |- let <bindings> in body : m
      auto* l = as<tt::Texp_let>(d);
      TermJudg body = expression(l->body);
      return value_bindings(l->rec, l->vbs) >> body;
    }
    case XK::Texp_match: {
      //  (Gi; mi |- pi -> ei : m)^i
      //  G |- e : sum(mi)^i
      //  ----------------------------------------------
      //  G + sum(Gi)^i |- match e with (pi -> ei)^i : m
      auto* mt = as<tt::Texp_match>(d);
      return [mt](Mode mode) {
        std::vector<VEnv> pat_envs, eff_envs;
        Mode pm = Mode::Ignore, em = Mode::Ignore;
        std::vector<std::pair<VEnv, Mode>> pcs;
        for (auto* c : mt->comp_cases) pcs.push_back(case_(c)(mode));
        for (auto& [e, m] : pcs) {
          pat_envs.push_back(e);
          pm = join(pm, m);
        }
        VEnv env_e = expression(mt->exp)(pm);
        std::vector<std::pair<VEnv, Mode>> ecs;
        for (auto* c : mt->eff_cases) ecs.push_back(case_(c)(mode));
        for (auto& [e, m] : ecs) {
          eff_envs.push_back(e);
          em = join(em, m);
        }
        VEnv eff_e = expression(mt->exp)(em);
        std::vector<VEnv> first{env_e};
        first.insert(first.end(), pat_envs.begin(), pat_envs.end());
        std::vector<VEnv> all{venv::join_list(first), eff_e};
        all.insert(all.end(), eff_envs.begin(), eff_envs.end());
        return venv::join_list(all);
      };
    }
    case XK::Texp_for: {
      //  G1 |- low: m[Dereference]
      //  G2 |- high: m[Dereference]
      //  G3 |- body: m[Guard]
      //  ---
      //  G1 + G2 + G3 |- for _ = low to high do body done: m
      auto* f = as<tt::Texp_for>(d);
      TermJudg b = expression(f->body) << Mode::Guard;
      TermJudg h = expression(f->hi) << Mode::Dereference;
      TermJudg lo = expression(f->lo) << Mode::Dereference;
      return join_j({lo, h, b});
    }
    case XK::Texp_constant: return empty_j;
    case XK::Texp_new:
      //  G |- c: m[Dereference]
      //  -----------------------
      //  G |- new c: m
      return path(as<tt::Texp_new>(d)->path) << Mode::Dereference;
    case XK::Texp_instvar: {
      auto* iv = as<tt::Texp_instvar>(d);
      TermJudg p = path(iv->path);
      TermJudg s = path(iv->self_path) << Mode::Dereference;
      return join_j({s, p});
    }
    case XK::Texp_apply: {
      auto* a = as<tt::Texp_apply>(d);
      if (auto* i = as<tt::Texp_ident>(a->fn->exp_desc);
          i && a->args.size() == 1 && !a->args[0].arg.omitted && is_ref(i->vd))
        //  G |- e: m[Guard]
        //  ------------------
        //  G |- ref e: m
        return expression(a->args[0].arg.arg) << Mode::Guard;
      // [args] may contain omitted arguments.  The arguments before the
      // first omitted argument are passed to the function immediately, so
      // they are dereferenced.  The arguments after the first omitted one
      // are stored in a closure, so guarded.  The function itself is called
      // immediately (dereferenced) if there is at least one argument before
      // the first omitted one.
      std::vector<const tt::Expression*> applied, delayed;
      bool has_omitted_arg = false;
      for (auto& arg : a->args) {
        if (arg.arg.omitted) has_omitted_arg = true;
        else (has_omitted_arg ? delayed : applied).push_back(arg.arg.arg);
      }
      Mode function_mode = applied.empty() ? Mode::Guard : Mode::Dereference;
      TermJudg dl = list_j(expression, delayed) << Mode::Guard;
      TermJudg ap = list_j(expression, applied) << Mode::Dereference;
      TermJudg fn = expression(a->fn) << function_mode;
      return join_j({fn, ap, dl});
    }
    case XK::Texp_tuple: {
      std::vector<const tt::Expression*> es;
      for (auto& x : as<tt::Texp_tuple>(d)->el) es.push_back(x.exp);
      return list_j(expression, es) << Mode::Guard;
    }
    case XK::Texp_atomic_loc: return expression(as<tt::Texp_atomic_loc>(d)->exp) << Mode::Guard;
    case XK::Texp_array: {
      auto* ar = as<tt::Texp_array>(d);
      Mode array_mode = Mode::Guard;
      switch (typeopt::array_kind(exp)) {
        case typeopt::ArrayKind::Pfloatarray:
          // (flat) float arrays unbox their elements
          array_mode = Mode::Dereference;
          break;
        case typeopt::ArrayKind::Pgenarray:
          // This is counted as a use, because constructing a generic array
          // involves inspecting to decide whether to unbox (PR#6939).
          array_mode = Mode::Dereference;
          break;
        case typeopt::ArrayKind::Paddrarray:
        case typeopt::ArrayKind::Pintarray:
          // non-generic, non-float arrays act as constructors
          array_mode = Mode::Guard;
          break;
      }
      return list_j(expression, std::vector<const tt::Expression*>(ar->el.begin(), ar->el.end())) << array_mode;
    }
    case XK::Texp_construct: {
      auto* c = as<tt::Texp_construct>(d);
      const ConstructorTag& tag = c->cstr->cstr_tag;
      TermJudg access_constructor =
          tag.kind == ConstructorTag::Kind::Cstr_extension ? path(tag.ext_path) << Mode::Dereference : empty_j;
      Mode m2 = tag.kind == ConstructorTag::Kind::Cstr_unboxed ? Mode::Return : Mode::Guard;
      TermJudg args = list_j(expression, std::vector<const tt::Expression*>(c->args.begin(), c->args.end())) << m2;
      return join_j({access_constructor, args});
    }
    case XK::Texp_variant:
      //  G |- e: m[Guard]
      //  ------------------   -----------
      //  G |- `A e: m         [] |- `A: m
      return option_j(expression, as<tt::Texp_variant>(d)->arg) << Mode::Guard;
    case XK::Texp_record: {
      auto* r = as<tt::Texp_record>(d);
      Mode field_mode = Mode::Guard;
      switch (r->representation.kind) {
        case RecordRepresentation::Kind::Record_float: field_mode = Mode::Dereference; break;
        case RecordRepresentation::Kind::Record_unboxed: field_mode = Mode::Return; break;
        default: field_mode = Mode::Guard; break;
      }
      auto field = [](const tt::RecordField& f) -> TermJudg { return f.def.kept ? empty_j : expression(f.def.exp); };
      TermJudg eo = option_j(expression, r->extended_expression) << Mode::Dereference;
      TermJudg fs = list_j(field, std::vector<tt::RecordField>(r->fields.begin(), r->fields.end())) << field_mode;
      return join_j({fs, eo});
    }
    case XK::Texp_ifthenelse: {
      //  Gc |- c: m[Dereference]
      //  G1 |- e1: m
      //  G2 |- e2: m
      //  ---
      //  Gc + G1 + G2 |- if c then e1 else e2: m
      auto* i = as<tt::Texp_ifthenelse>(d);
      TermJudg ifnot = option_j(expression, i->else_);
      TermJudg ifso = expression(i->then_);
      TermJudg cond = expression(i->cond) << Mode::Dereference;
      return join_j({cond, ifso, ifnot});
    }
    case XK::Texp_setfield: {
      //  G1 |- e1: m[Dereference]
      //  G2 |- e2: m[Dereference]
      //  ---
      //  G1 + G2 |- e1.x <- e2: m
      auto* s = as<tt::Texp_setfield>(d);
      TermJudg e2 = expression(s->value) << Mode::Dereference;
      TermJudg e1 = expression(s->exp) << Mode::Dereference;
      return join_j({e1, e2});
    }
    case XK::Texp_sequence: {
      //  G1 |- e1: m[Guard]
      //  G2 |- e2: m
      //  --------------------
      //  G1 + G2 |- e1; e2: m
      auto* s = as<tt::Texp_sequence>(d);
      TermJudg e2 = expression(s->e2);
      TermJudg e1 = expression(s->e1) << Mode::Guard;
      return join_j({e1, e2});
    }
    case XK::Texp_while: {
      //  G1 |- cond: m[Dereference]
      //  G2 |- body: m[Guard]
      //  ---------------------------------
      //  G1 + G2 |- while cond do body done: m
      auto* w = as<tt::Texp_while>(d);
      TermJudg b = expression(w->body) << Mode::Guard;
      TermJudg c = expression(w->cond) << Mode::Dereference;
      return join_j({c, b});
    }
    case XK::Texp_send:
      //  G |- e: m[Dereference]
      //  ----------------------
      //  G |- e#x: m
      return join_j({expression(as<tt::Texp_send>(d)->obj) << Mode::Dereference});
    case XK::Texp_field: return expression(as<tt::Texp_field>(d)->exp) << Mode::Dereference;
    case XK::Texp_setinstvar: {
      //  G |- e: m[Dereference]
      //  ----------------------
      //  G |- x <- e: m
      auto* s = as<tt::Texp_setinstvar>(d);
      TermJudg e = expression(s->value) << Mode::Dereference;
      TermJudg p = path(s->self_path) << Mode::Dereference;
      return join_j({p, e});
    }
    case XK::Texp_assert:
      // `assert e` is treated just as if `assert` was a function.
      return expression(as<tt::Texp_assert>(d)->exp) << Mode::Dereference;
    case XK::Texp_pack: return modexp(as<tt::Texp_pack>(d)->me);
    case XK::Texp_object: return class_structure(as<tt::Texp_object>(d)->cs);
    case XK::Texp_try: {
      //  G |- e: m      (Gi; _ |- pi -> ei : m)^i
      //  --------------------------------------------
      //  G + sum(Gi)^i |- try e with (pi -> ei)^i : m
      // Contrarily to match, the patterns p do not inspect the value of e,
      // so their mode does not influence the mode of e.
      auto* t = as<tt::Texp_try>(d);
      TermJudg eff = list_j(case_env, std::vector<const tt::Case*>(t->eff_cases.begin(), t->eff_cases.end()));
      TermJudg exn = list_j(case_env, std::vector<const tt::Case*>(t->exn_cases.begin(), t->exn_cases.end()));
      TermJudg e = expression(t->exp);
      return join_j({e, exn, eff});
    }
    case XK::Texp_override: {
      //  G |- pth : m   (Gi |- ei : m[Dereference])^i
      //  ----------------------------------------------------
      //  G + sum(Gi)^i |- {< (xi = ei)^i >} (at path pth) : m
      auto* o = as<tt::Texp_override>(d);
      auto field = [](const tt::OverrideField& f) { return expression(f.exp); };
      TermJudg fs =
          list_j(field, std::vector<tt::OverrideField>(o->fields.begin(), o->fields.end())) << Mode::Dereference;
      TermJudg p = path(o->self_path) << Mode::Dereference;
      return join_j({p, fs});
    }
    case XK::Texp_function: {
      //  G      |-{body} b  : m[Delay]
      //  (Hj    |-{def}  Pj : m[Delay])^j
      //  H  := sum(Hj)^j
      //  ps := sum(pat(Pj))^j
      //  -----------------------------------
      //  G + H - ps |- fun (Pj)^j -> b : m
      auto* f = as<tt::Texp_function>(d);
      std::vector<const tt::Pattern*> patterns;
      for (auto* p : f->params) patterns.push_back(p->fp_kind.pat);
      // Optional argument defaults: G |-{def} P : m
      std::vector<TermJudg> defaults;
      for (auto* p : f->params)
        defaults.push_back(p->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_optional_default
                               ? expression(p->fp_kind.default_)
                               : empty_j);
      TermJudg body = function_body(f->body);
      std::vector<TermJudg> all{body};
      all.insert(all.end(), defaults.begin(), defaults.end());
      TermJudg fj = join_j(all) << Mode::Delay;
      return [fj, patterns](Mode m) { return remove_patlist(patterns, fj(m)); };
    }
    case XK::Texp_lazy: {
      //  G |- e: m[Delay]
      //  ----------------  (modulo some subtle compiler optimizations)
      //  G |- lazy e: m
      const tt::Expression* e = as<tt::Texp_lazy>(d)->exp;
      Mode lazy_mode =
          typeopt::classify_lazy_argument(e) == typeopt::LazyArgument::Other ? Mode::Delay : Mode::Return;
      return expression(e) << lazy_mode;
    }
    case XK::Texp_letop: {
      auto* l = as<tt::Texp_letop>(d);
      TermJudg body = case_env(l->body) << Mode::Delay;
      std::vector<const tt::BindingOp*> bops{l->let_};
      bops.insert(bops.end(), l->ands.begin(), l->ands.end());
      TermJudg ops = list_j(binding_op, bops) << Mode::Dereference;
      return join_j({ops, body});
    }
    case XK::Texp_unreachable: return empty_j;
    case XK::Texp_extension_constructor:
      return path(as<tt::Texp_extension_constructor>(d)->path) << Mode::Dereference;
    case XK::Texp_struct_item: {
      auto* s = as<tt::Texp_struct_item>(d);
      TermJudg e = expression(s->body);
      return structure_item(s->item) >> e;
    }
  }
  throw std::logic_error("Value_rec_check.expression");
}

// Function bodies: G |-{body} b : m
static TermJudg function_body(const tt::FunctionBody* body) {
  if (body->kind == tt::FunctionBody::Kind::Tfunction_body) return expression(body->body);
  // Contrarily to match, the values that are pattern-matched are bound
  // locally, so the pattern modes do not influence the final environment.
  std::vector<TermJudg> cs;
  for (auto* c : body->cases) cs.push_back([c](Mode mode) { return case_(c)(mode).first; });
  return join_j(cs);
}

static TermJudg binding_op(const tt::BindingOp* bop) {
  TermJudg e = expression(bop->bop_exp);
  TermJudg p = path(bop->bop_op_path);
  return join_j({p, e});
}

static TermJudg class_structure(const tt::ClassStructure* cs) {
  return list_j(class_field, std::vector<const tt::ClassField*>(cs->cstr_fields.begin(), cs->cstr_fields.end()));
}

static TermJudg class_field(const tt::ClassField* cf) {
  const tt::ClassFieldDesc* d = cf->cf_desc;
  switch (d->kind) {
    case CfK::Tcf_inherit: return class_expr(as<tt::Tcf_inherit>(d)->ce) << Mode::Dereference;
    case CfK::Tcf_val: return class_field_kind(as<tt::Tcf_val>(d)->kind_);
    case CfK::Tcf_method: return class_field_kind(as<tt::Tcf_method>(d)->kind_);
    case CfK::Tcf_constraint: return empty_j;
    case CfK::Tcf_initializer: return expression(as<tt::Tcf_initializer>(d)->exp) << Mode::Dereference;
    case CfK::Tcf_attribute: return empty_j;
  }
  throw std::logic_error("Value_rec_check.class_field");
}

static TermJudg class_field_kind(const tt::ClassFieldKind& cfk) {
  if (cfk.is_virtual) return empty_j;
  return expression(cfk.exp) << Mode::Dereference;
}

static TermJudg modexp(const tt::ModuleExpr* mexp) {
  const tt::ModuleExprDesc* d = mexp->mod_desc;
  switch (d->kind) {
    case MK::Tmod_ident: return path(as<tt::Tmod_ident>(d)->path);
    case MK::Tmod_structure: return structure(as<tt::Tmod_structure>(d)->str);
    case MK::Tmod_functor: return modexp(as<tt::Tmod_functor>(d)->body) << Mode::Delay;
    case MK::Tmod_apply: {
      auto* a = as<tt::Tmod_apply>(d);
      TermJudg p = modexp(a->arg) << Mode::Dereference;
      TermJudg f = modexp(a->fn) << Mode::Dereference;
      return join_j({f, p});
    }
    case MK::Tmod_apply_unit: return modexp(as<tt::Tmod_apply_unit>(d)->fn) << Mode::Dereference;
    case MK::Tmod_constraint: {
      auto* c = as<tt::Tmod_constraint>(d);
      std::function<TermJudg(const tt::ModuleCoercion*, const std::function<TermJudg(Mode)>&)> coercion =
          [&](const tt::ModuleCoercion* coe, const std::function<TermJudg(Mode)>& k) -> TermJudg {
        switch (coe->kind) {
          case CK::Tcoerce_none: return k(Mode::Return);
          case CK::Tcoerce_structure:
          case CK::Tcoerce_functor:
            // These coercions perform a shallow copy of the input module, by
            // creating a new module with fields obtained by accessing the
            // same fields in the input module.
            return k(Mode::Dereference);
          case CK::Tcoerce_primitive:
            // This corresponds to 'external' declarations, and the coercion
            // ignores its argument
            return k(Mode::Ignore);
          case CK::Tcoerce_alias: {
            // Alias coercions ignore their arguments, but they evaluate
            // their alias module 'pth' under another coercion.
            Path::t pth = coe->alias_path;
            return coercion(coe->alias_coercion, [pth](Mode m) { return path(pth) << m; });
          }
        }
        throw std::logic_error("Value_rec_check.modexp");
      };
      const tt::ModuleExpr* me = c->me;
      return coercion(c->coercion, [me](Mode m) { return modexp(me) << m; });
    }
    case MK::Tmod_unpack: return expression(as<tt::Tmod_unpack>(d)->exp);
  }
  throw std::logic_error("Value_rec_check.modexp");
}

// G |- pth : m
static TermJudg path(Path::t pth) {
  switch (pth->kind) {
    case Path::Kind::Pident: return single_j(pth->id);
    case Path::Kind::Pdot: return path(pth->p1) << Mode::Dereference;
    case Path::Kind::Papply: {
      TermJudg p = path(pth->p2) << Mode::Dereference;
      TermJudg f = path(pth->p1) << Mode::Dereference;
      return join_j({f, p});
    }
    case Path::Kind::Pextra_ty: return path(pth->p1);
  }
  throw std::logic_error("Value_rec_check.path");
}

// G |- struct ... end : m
static TermJudg structure(const tt::Structure* s) {
  return [s](Mode m) {
    VEnv env;
    for (std::size_t k = s->str_items.size(); k-- > 0;) env = structure_item(s->str_items[k])(m, env);
    return env;
  };
}

static std::vector<Ident::t> signature_ids(const Signature& sg) {
  std::vector<Ident::t> r;
  for (auto* it : sg) r.push_back(signature_item_id(it));
  return r;
}

// G |- <structure item> : m -| G', where G is an output and m, G' are inputs
static BindJudg structure_item(const tt::StructureItem* s) {
  return [s](Mode m, const VEnv& env) -> VEnv {
    const tt::StructureItemDesc* d = s->str_desc;
    switch (d->kind) {
      case SK::Tstr_eval: {
        // The expression `e` is treated in the same way as let _ = e
        TermJudg judg_e = expression(as<tt::Tstr_eval>(d)->exp) << Mode::Guard;
        return venv::join(judg_e(m), env);
      }
      case SK::Tstr_value: {
        auto* v = as<tt::Tstr_value>(d);
        return value_bindings(v->rec, v->vbs)(m, env);
      }
      case SK::Tstr_module: {
        const tt::ModuleBinding* mb = as<tt::Tstr_module>(d)->mb;
        return module_binding(mb->mb_id, mb->mb_expr)(m, env);
      }
      case SK::Tstr_recmodule: {
        std::vector<std::pair<Ident::t, const tt::ModuleExpr*>> bindings;
        for (auto* mb : as<tt::Tstr_recmodule>(d)->mbs) bindings.push_back({mb->mb_id, mb->mb_expr});
        return recursive_module_bindings(bindings)(m, env);
      }
      case SK::Tstr_primitive:
      case SK::Tstr_type: return env;
      case SK::Tstr_typext: {
        auto exts = as<tt::Tstr_typext>(d)->ext->tyext_constructors;
        std::vector<Ident::t> ext_ids;
        for (auto* e : exts) ext_ids.push_back(e->ext_id);
        VEnv rest = venv::remove_list(ext_ids, env);
        VEnv es = list_j(extension_constructor, std::vector<const tt::TExtensionConstructor*>(exts.begin(), exts.end()))(m);
        return venv::join(es, rest);
      }
      case SK::Tstr_exception: {
        const tt::TExtensionConstructor* ext = as<tt::Tstr_exception>(d)->exn->tyexn_constructor;
        VEnv rest = venv::remove(ext->ext_id, env);
        return venv::join(extension_constructor(ext)(m), rest);
      }
      case SK::Tstr_modtype:
      case SK::Tstr_class_type:
      case SK::Tstr_attribute: return env;
      case SK::Tstr_open: return open_declaration(as<tt::Tstr_open>(d)->od)(m, env);
      case SK::Tstr_class: {
        auto classes = as<tt::Tstr_class>(d)->classes;
        std::vector<Ident::t> class_ids;
        for (auto& c : classes) class_ids.push_back(c.decl->ci_id_class);
        auto class_declaration = [class_ids](const tt::ClassDeclarationItem& c) -> TermJudg {
          const tt::ClassExpr* ci_expr = c.decl->ci_expr;
          return [class_ids, ci_expr](Mode m2) { return venv::remove_list(class_ids, class_expr(ci_expr)(m2)); };
        };
        VEnv rest = venv::remove_list(class_ids, env);
        VEnv cs = list_j(class_declaration, std::vector<tt::ClassDeclarationItem>(classes.begin(), classes.end()))(m);
        return venv::join(cs, rest);
      }
      case SK::Tstr_include: {
        const tt::IncludeDeclaration* incl = as<tt::Tstr_include>(d)->incl;
        std::vector<Ident::t> included_ids = signature_ids(incl->incl_type);
        VEnv rest = venv::remove_list(included_ids, env);
        return venv::join(modexp(incl->incl_mod)(m), rest);
      }
    }
    throw std::logic_error("Value_rec_check.structure_item");
  };
}

// G |- module M = E : m -| G
static BindJudg module_binding(Ident::t id, const tt::ModuleExpr* mexp) {
  //  GE |- E: m[mM + Guard]
  //  -------------------------------------
  //  GE + G |- module M = E : m -| M:mM, G
  return [id, mexp](Mode m, const VEnv& env0) {
    TermJudg judg_E;
    VEnv env = env0;
    if (!id) {
      judg_E = modexp(mexp) << Mode::Guard;
    } else {
      auto [mM, env2] = venv::take(id, env0);
      judg_E = modexp(mexp) << join(mM, Mode::Guard);
      env = env2;
    }
    return venv::join(judg_E(m), env);
  };
}

static BindJudg open_declaration(const tt::OpenDeclaration* od) {
  return [od](Mode m, const VEnv& env) {
    TermJudg judg_E = modexp(od->open_expr);
    std::vector<Ident::t> bound_ids = signature_ids(od->open_bound_items);
    VEnv rest = venv::remove_list(bound_ids, env);
    return venv::join(judg_E(m), rest);
  };
}

static BindJudg recursive_module_bindings(std::vector<std::pair<Ident::t, const tt::ModuleExpr*>> m_bindings) {
  return [m_bindings](Mode m, const VEnv& env) {
    std::vector<Ident::t> mids;
    for (auto& [mid, _] : m_bindings)
      if (mid) mids.push_back(mid);
    auto binding = [&mids, &env](const std::pair<Ident::t, const tt::ModuleExpr*>& b) -> TermJudg {
      auto [mid, mexp] = b;
      TermJudg judg_E =
          !mid ? modexp(mexp) << Mode::Guard : modexp(mexp) << join(venv::find(mid, env), Mode::Guard);
      std::vector<Ident::t> ms = mids;
      return [ms, judg_E](Mode m2) { return venv::remove_list(ms, judg_E(m2)); };
    };
    VEnv rest = venv::remove_list(mids, env);
    return venv::join(list_j(binding, m_bindings)(m), rest);
  };
}

static TermJudg class_expr(const tt::ClassExpr* ce) {
  const tt::ClassExprDesc* d = ce->cl_desc;
  switch (d->kind) {
    case ClK::Tcl_ident: return path(as<tt::Tcl_ident>(d)->path) << Mode::Dereference;
    case ClK::Tcl_structure: return class_structure(as<tt::Tcl_structure>(d)->cs);
    case ClK::Tcl_fun: {
      auto* f = as<tt::Tcl_fun>(d);
      std::vector<Ident::t> ids;
      for (auto& a : f->args) ids.push_back(a.id);
      return remove_ids(ids, class_expr(f->ce) << Mode::Delay);
    }
    case ClK::Tcl_apply: {
      auto* a = as<tt::Tcl_apply>(d);
      auto arg = [](const tt::LabeledArg& x) -> TermJudg { return x.arg.omitted ? empty_j : expression(x.arg.arg); };
      TermJudg args = list_j(arg, std::vector<tt::LabeledArg>(a->args.begin(), a->args.end())) << Mode::Dereference;
      TermJudg c = class_expr(a->ce) << Mode::Dereference;
      return join_j({c, args});
    }
    case ClK::Tcl_let: {
      auto* l = as<tt::Tcl_let>(d);
      TermJudg c = class_expr(l->ce);
      return value_bindings(l->rec, l->vbs) >> c;
    }
    case ClK::Tcl_constraint: return class_expr(as<tt::Tcl_constraint>(d)->ce);
    case ClK::Tcl_open: return class_expr(as<tt::Tcl_open>(d)->ce);
  }
  throw std::logic_error("Value_rec_check.class_expr");
}

static TermJudg extension_constructor(const tt::TExtensionConstructor* ec) {
  if (ec->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_decl) return empty_j;
  return path(ec->ext_kind.path);
}

// G |- let (rec?) (pi = ei)^i : m -| G'
static BindJudg value_bindings(RecFlag rec_flag, Slice<const tt::ValueBinding*> bindings) {
  std::vector<const tt::ValueBinding*> bs(bindings.begin(), bindings.end());
  return [rec_flag, bs](Mode mode, const VEnv& bound_env) {
    std::vector<const tt::Pattern*> all_bound_pats;
    for (auto* vb : bs) all_bound_pats.push_back(vb->vb_pat);
    VEnv outer_env = remove_patlist(all_bound_pats, bound_env);
    VEnv bindings_env;
    if (rec_flag == RecFlag::Nonrecursive) {
      //  (Gi, pi:_ |- ei : m[mbody_i])^i   (pi : mbody_i -| D)^i
      //  ------------------------------------------------------------
      //  Sum(Gi) + (D - (pi)^i) |- let (pi=ei)^i : m -| D
      auto binding_env = [&bound_env](const tt::ValueBinding* vb) -> TermJudg {
        return [vb, &bound_env](Mode m) {
          Mode m2 = compose(m, pattern(vb->vb_pat, bound_env));
          return remove_pat(vb->vb_pat, expression(vb->vb_expr)(m2));
        };
      };
      bindings_env = list_j(binding_env, bs)(mode);
    } else {
      //  (Gi, (xj : mdef_ij)^j |- ei : m[mbody_i])^i   (xi : mbody_i -| D)^i
      //  G'i = Gi + mdef_ij[G'j]
      //  -------------------------------------------------------------------
      //  Sum(G'i) + (D - (pi)^i) |- let rec (xi=ei)^i : m -| D
      // The (G'i)^i, the transitive dependencies, are the smallest solution
      // of the equations, computed as a least fixpoint.
      std::vector<VEnv> env;
      std::vector<std::vector<Mode>> mdef;
      for (auto* vb : bs) {
        Mode mbody_i = pattern(vb->vb_pat, bound_env);
        // Gi, (x_j:mdef_ij)^j
        VEnv rhs_env_i = expression(vb->vb_expr)(compose(mode, mbody_i));
        // (mdef_ij)^j (for a fixed i)
        std::vector<Mode> mutual_modes;
        for (auto* vbj : bs) mutual_modes.push_back(pattern(vbj->vb_pat, rhs_env_i));
        // Gi
        env.push_back(remove_patlist(all_bound_pats, rhs_env_i));
        mdef.push_back(mutual_modes);
      }
      for (;;) {
        // Gi, (mdef_ij)^j => Gi + Sum_j mdef_ij[Gj]
        std::vector<VEnv> env2;
        for (std::size_t i = 0; i < env.size(); ++i) {
          std::vector<VEnv> composed;
          for (std::size_t j = 0; j < env.size(); ++j) composed.push_back(venv::compose(mdef[i][j], env[j]));
          env2.push_back(venv::join(env[i], venv::join_list(composed)));
        }
        bool same = true;
        for (std::size_t i = 0; i < env.size(); ++i) same = same && venv::equal(env[i], env2[i]);
        if (same) {
          env = env2;
          break;
        }
        env = env2;
      }
      bindings_env = venv::join_list(env);
    }
    return venv::join(bindings_env, outer_env);
  };
}

// G; m' |- (p -> e) : m, with outputs G, m' and input m (m' is the mode
// under which the scrutinee of p is placed)
static std::function<std::pair<VEnv, Mode>(Mode)> case_(const tt::Case* c) {
  //  Ge |- e : m    Gg |- g : m[Dereference]
  //  G := Ge+Gg     p : mp -| G
  //  ----------------------------------------
  //  G - p; m[mp] |- (p (when g)? -> e) : m
  TermJudg rhs = expression(c->c_rhs);
  TermJudg guard = option_j(expression, c->c_guard) << Mode::Dereference;
  TermJudg judg = join_j({guard, rhs});
  const tt::Pattern* lhs = c->c_lhs;
  return [judg, lhs](Mode m) {
    VEnv env = judg(m);
    Mode pm = compose(m, pattern(lhs, env));
    return std::make_pair(remove_pat(lhs, env), pm);
  };
}

// p : m -| G, with output m and input G (m is the mode under which the
// scrutinee of p is placed)
static Mode pattern(const tt::Pattern* pat, const VEnv& env) {
  //  mp := | Dereference if p is destructuring
  //        | Guard       otherwise
  //  me := sum{G(x), x in vars(p)}
  //  --------------------------------------------
  //  p : (mp + me) -| G
  Mode m_pat = is_destructuring_pattern(pat) ? Mode::Dereference : Mode::Guard;
  Mode m_env = Mode::Ignore;
  for (auto* id : tt::pat_bound_idents(pat)) m_env = join(m_env, venv::find(id, env));
  return join(m_pat, m_env);
}

static bool is_destructuring_pattern(const tt::Pattern* pat) {
  const tt::PatternDesc* d = pat->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_var: return false;
    case PK::Tpat_alias: return is_destructuring_pattern(as<tt::Tpat_alias>(d)->pat);
    case PK::Tpat_constant:
    case PK::Tpat_tuple:
    case PK::Tpat_construct:
    case PK::Tpat_variant:
    case PK::Tpat_record:
    case PK::Tpat_array:
    case PK::Tpat_lazy: return true;
    case PK::Tpat_value: return is_destructuring_pattern(as<tt::Tpat_value>(d)->pat);
    case PK::Tpat_exception: return false;
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      return is_destructuring_pattern(o->p1) || is_destructuring_pattern(o->p2);
    }
  }
  throw std::logic_error("Value_rec_check.is_destructuring_pattern");
}

std::optional<tt::RecursiveBindingKind> is_valid_recursive_expression(const std::vector<Ident::t>& idlist,
                                                                      const tt::Expression* expr) {
  // Fast path: functions can never have invalid recursive references
  if (expr->exp_desc->kind == XK::Texp_function) return Sd::Static;
  Sd rkind = classify_expression(SizeEnv{}, expr);
  bool is_valid;
  if (rkind == Sd::Static) {
    // The expression has known size or is constant
    VEnv ty = expression(expr)(Mode::Return);
    is_valid = venv::unguarded(ty, idlist).empty();
  } else {
    // The expression has unknown size
    VEnv ty = expression(expr)(Mode::Return);
    is_valid = venv::unguarded(ty, idlist).empty() && venv::dependent(ty, idlist).empty();
  }
  if (is_valid) return rkind;
  return std::nullopt;
}

// A class declaration may contain let-bindings.  This prevents the unsafe
// creations of objects of this class in the let-binding: for example,
// `class a = let x = new a in object ... end` is forbidden, but
// `class a = let x () = new a in object ... end` is allowed.
bool is_valid_class_expr(const std::vector<Ident::t>& idlist, const tt::ClassExpr* ce) {
  std::function<VEnv(Mode, const tt::ClassExpr*)> class_expr_ = [&](Mode mode, const tt::ClassExpr* c) -> VEnv {
    const tt::ClassExprDesc* d = c->cl_desc;
    switch (d->kind) {
      case ClK::Tcl_ident:
      case ClK::Tcl_structure:
      case ClK::Tcl_fun:
      case ClK::Tcl_apply: return VEnv{};
      case ClK::Tcl_let: {
        auto* l = as<tt::Tcl_let>(d);
        VEnv inner = class_expr_(mode, l->ce);
        return value_bindings(l->rec, l->vbs)(mode, inner);
      }
      case ClK::Tcl_constraint: return class_expr_(mode, as<tt::Tcl_constraint>(d)->ce);
      case ClK::Tcl_open: return class_expr_(mode, as<tt::Tcl_open>(d)->ce);
    }
    throw std::logic_error("Value_rec_check.is_valid_class_expr");
  };
  return venv::unguarded(class_expr_(Mode::Return, ce), idlist).empty();
}

}  // namespace cppcaml::typing::value_rec_check
