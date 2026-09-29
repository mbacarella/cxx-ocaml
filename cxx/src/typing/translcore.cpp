// Port of lambda/translcore.ml (cxx/PORTING.md stage 10): the translation
// of the core language to Lambda.
//
// Evaluation order is ocamlc's, since it decides the stamps of the idents
// the translation creates: constructor, tuple and application arguments
// right to left (a labelled application in the order of the function's
// type), record fields right to left in definition order, List.map left to
// right, List.fold_right from the end.  So `Lifthenelse (c, a, b)` translates
// b, then a, then c, and `transl_let ... (transl body)` translates the body
// before the bindings.  Misc.fatal_error / assert false are
// std::logic_error.

#include "cppcaml/typing/translcore.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/matching.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/translattribute.hpp"
#include "cppcaml/typing/translobj.hpp"
#include "cppcaml/typing/translprim.hpp"
#include "cppcaml/typing/typeopt.hpp"
#include "cppcaml/typing/value_rec_compiler.hpp"
#include "typecore_app.hpp"

namespace cppcaml::typing::translcore {

namespace tt = typedtree;
namespace L = cppcaml::typing::lambda;
using lam_t = L::lambda;
using ValueKind = L::ValueKind;
using L::as;
using L::FunctionAttribute;
using L::InlineAttribute;
using L::LambdaApply;
using L::Param;
using L::Primitive;
using L::ScopedLocation;
using L::SpecialiseAttribute;
using L::StructuredConstant;
using L::TailcallAttribute;
using PK = Primitive::K;
using debuginfo::of_location;
using debuginfo::to_location;

// Forward declarations -- filled in by Translmod / Translclass
std::function<lam_t(scopes, const tt::ModuleCoercion*, Path::t, const tt::ModuleExpr*)> transl_module;
std::function<lam_t(scopes, Slice<Ident::t>, Path::t, const tt::StructureItem*,
                    const std::function<lam_t(Slice<Ident::t>)>&)>
    transl_struct_item;
std::function<lam_t(scopes, Ident::t, Slice<std::string_view>, const tt::ClassExpr*)> transl_object;

namespace {

// Config.max_young_wosize
constexpr long config_max_young_wosize = 256;
// Obj.object_tag
constexpr long object_tag = 248;

[[noreturn]] void fatal_error(const std::string& s) { throw std::logic_error(s); }

constexpr long use_dup_for_constant_mutable_arrays_bigger_than = 4;

// ---- Lambda builders ------------------------------------------------------------
Primitive pfield(long n, L::ImmediateOrPointer ptr, MutableFlag mut) {
  Primitive p = L::prim(PK::Pfield);
  p.n = n;
  p.ptr = ptr;
  p.mut = mut;
  return p;
}
Primitive psetfield(long n, L::ImmediateOrPointer ptr, L::InitializationOrAssignment init) {
  Primitive p = L::prim(PK::Psetfield);
  p.n = n;
  p.ptr = ptr;
  p.init = init;
  return p;
}
Primitive pfloatfield(long n) { return [&] { Primitive p = L::prim(PK::Pfloatfield); p.n = n; return p; }(); }
Primitive psetfloatfield(long n, L::InitializationOrAssignment init) {
  Primitive p = L::prim(PK::Psetfloatfield);
  p.n = n;
  p.init = init;
  return p;
}
Primitive pmakeblock(long tag, MutableFlag mut, const std::vector<ValueKind>* shape) {
  Primitive p = L::prim(PK::Pmakeblock);
  p.n = tag;
  p.mut = mut;
  if (shape) p.shape = L::BlockShape{true, slice(*shape)};
  return p;
}
Primitive pmakearray(L::ArrayKind k, MutableFlag mut) {
  Primitive p = L::prim(PK::Pmakearray);
  p.array = k;
  p.mut = mut;
  return p;
}
Primitive pduparray(L::ArrayKind k, MutableFlag mut) {
  Primitive p = L::prim(PK::Pduparray);
  p.array = k;
  p.mut = mut;
  return p;
}
Primitive pccall(const PrimitiveDescription* d) {
  Primitive p = L::prim(PK::Pccall);
  p.ccall = d;
  return p;
}
Primitive praise(L::RaiseKind k) {
  Primitive p = L::prim(PK::Praise);
  p.raise = k;
  return p;
}

lam_t mkapply(lam_t func, std::vector<lam_t> args, const ScopedLocation& loc,
              TailcallAttribute tailcall = TailcallAttribute::Default_tailcall,
              InlineAttribute inlined = InlineAttribute{},
              SpecialiseAttribute specialised = SpecialiseAttribute::Default_specialise) {
  LambdaApply ap;
  ap.ap_func = func;
  ap.ap_args = slice(args);
  ap.ap_loc = loc;
  ap.ap_tailcall = tailcall;
  ap.ap_inlined = inlined;
  ap.ap_specialised = specialised;
  return L::lapply(ap);
}

const StructuredConstant* const_immstring(std::string_view s) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_immstring;
  c->s = zborrow(s);  // Const_immstring s: the string itself
  return c;
}
const StructuredConstant* const_block(long tag, const std::vector<const StructuredConstant*>& fields) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_block;
  c->i = tag;
  c->fields = slice(fields);
  return c;
}

// Primitive.simple ~name ~arity ~alloc
const PrimitiveDescription* simple(std::string_view name, long arity, bool alloc) {
  ZoneScope perm(permanent_zone());
  std::vector<NativeRepr> reprs(static_cast<std::size_t>(arity));
  return make<PrimitiveDescription>(
      PrimitiveDescription{zstr(name), arity, alloc, empty_native_name(), slice(reprs), NativeRepr{}});
}

const Primitive& prim_fresh_oo_id() {
  static const Primitive p = pccall(simple("caml_fresh_oo_id", 1, false));
  return p;
}

const Primitive& prim_alloc_stack() {
  static const Primitive p = pccall(simple("caml_alloc_stack", 3, true));
  return p;
}

// ---- Out_type.rewrite_double_underscore_paths --------------------------------------

std::optional<std::size_t> find_double_underscore(std::string_view s) {
  for (std::size_t i = 0; i + 1 < s.size(); i++)
    if (s[i] == '_' && s[i + 1] == '_') return i;
  return std::nullopt;
}

bool module_path_is_an_alias_of(env::t env, Path::t path, Path::t alias_of) {
  const ModuleDeclaration* md;
  try {
    md = env::find_module(path, env);
  } catch (const env::NotFound&) {
    return false;
  }
  if (md->md_type->kind != ModuleType::Kind::Mty_alias) return false;
  Path::t path2 = md->md_type->path;
  return path::same(path2, alias_of) || module_path_is_an_alias_of(env, path2, alias_of);
}

Path::t rewrite_double_underscore_paths(env::t env, Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pdot: return Path::pdot(rewrite_double_underscore_paths(env, p->p1), p->s);
    case Path::Kind::Papply:
      return Path::papply(rewrite_double_underscore_paths(env, p->p1), rewrite_double_underscore_paths(env, p->p2));
    case Path::Kind::Pextra_ty: return Path::pextra_ty(rewrite_double_underscore_paths(env, p->p1), p->extra, p->s);
    case Path::Kind::Pident: {
      std::string_view name = ident::name(p->id);
      std::optional<std::size_t> i = find_double_underscore(name);
      if (!i) return p;
      std::string rest(name.substr(*i + 2));
      if (!rest.empty() && rest[0] >= 'a' && rest[0] <= 'z') rest[0] = static_cast<char>(rest[0] - 'a' + 'A');
      Location none = location::none();
      Longident::t better_lid = Longident::ldot(Longident::lident(zstr(name.substr(0, *i))), none, zstr(rest), none);
      Path::t p2;
      try {
        p2 = env::find_module_by_name(better_lid, env).first;
      } catch (const env::NotFound&) {
        return p;
      }
      return module_path_is_an_alias_of(env, p2, p) ? p2 : p;
    }
  }
  return p;
}

// ---- To propagate structured constants ----------------------------------------------

struct NotConstant {};

const StructuredConstant* extract_constant(lam_t l) {
  if (auto* c = as<L::Lconst>(l)) return c->c;
  throw NotConstant{};
}
// whether List.map extract_constant ll would not raise Not_constant (it has
// no other effect: the translations below test first instead of catching)
static bool all_constant(const std::vector<lam_t>& ll) {
  for (lam_t l : ll)
    if (!as<L::Lconst>(l)) return false;
  return true;
}

std::string_view extract_float(const StructuredConstant* c) {
  if (c->kind == StructuredConstant::Kind::Const_float) return c->s;
  fatal_error("Translcore.extract_float");
}

// ---- Insertion of debugging events --------------------------------------------------

lam_t event_before(scopes sc, const tt::Expression* exp, lam_t lam) {
  return translprim::event_before(of_location(sc, exp->exp_loc), exp, lam);
}

lam_t event_after(scopes sc, const tt::Expression* exp, lam_t lam) {
  return translprim::event_after(of_location(sc, exp->exp_loc), exp, lam);
}

// the ((kind, params, return), body) of a function translation
struct FunInfo {
  L::FunctionKind kind;
  std::vector<Param> params;
  ValueKind return_;
  lam_t body;
};

template <class F>
FunInfo event_function(scopes sc, const tt::Expression* exp, F&& lam) {
  if (clflags::debug && !clflags::native_code) {
    L::IntRef* repr = make<L::IntRef>(L::IntRef{0});
    FunInfo r = lam(repr);
    r.body = L::levent(r.body, make<L::LambdaEvent>(L::LambdaEvent{of_location(sc, exp->exp_loc),
                                                                  L::EventKind::Lev_function, nullptr, repr,
                                                                  exp->exp_env}));
    return r;
  }
  return lam(static_cast<L::IntRef*>(nullptr));
}

// ---- Assertions ----------------------------------------------------------------------

lam_t assert_failed(const Location& loc, scopes sc, const tt::Expression* exp) {
  lam_t slot = L::transl_extension_path(debuginfo::loc_unknown(), env::initial(), predef::paths().assert_failure);
  // Location.get_pos_info
  std::string_view fname = loc.loc_start.pos_fname;
  long line = loc.loc_start.pos_lnum;
  long char_ = loc.loc_start.pos_cnum - loc.loc_start.pos_bol;
  ScopedLocation sloc = of_location(sc, exp->exp_loc);
  lam_t blk = L::lprim(
      pmakeblock(0, MutableFlag::Immutable, nullptr),
      slice({slot, L::lconst(const_block(0, {const_immstring(fname), L::const_int(line), L::const_int(char_)}))}),
      sloc);
  return L::lprim(praise(L::RaiseKind::Raise_regular), slice({event_after(sc, exp, blk)}), sloc);
}

// In cases where we're careful to preserve syntactic arity, we disable the
// arity fusion attempted by simplif.ml
FunctionAttribute function_attribute_disallowing_arity_fusion() {
  FunctionAttribute a = L::default_function_attribute();
  a.may_fuse_arity = false;
  return a;
}

// fuse_method_arity: a n-ary method is compiled as a (n+1)-ary function
std::pair<std::vector<const tt::FunctionParam*>, const tt::FunctionBody*> fuse_method_arity(
    Slice<const tt::FunctionParam*> parent_params, const tt::FunctionBody* parent_body) {
  std::vector<const tt::FunctionParam*> params(parent_params.begin(), parent_params.end());
  if (parent_body->kind == tt::FunctionBody::Kind::Tfunction_body) {
    const tt::Expression* e = parent_body->body;
    if (auto* f = tt::as<tt::Texp_function>(e->exp_desc)) {
      bool poly = false;
      for (const tt::ExpExtraItem& x : e->exp_extra)
        if (x.extra.kind == tt::ExpExtra::Kind::Texp_poly) poly = true;
      if (poly) {
        params.insert(params.end(), f->params.begin(), f->params.end());
        return {params, f->body};
      }
    }
  }
  return {params, parent_body};
}

// ---- Translation of expressions ------------------------------------------------------

void iter_exn_names(void (*f)(Ident::t), const tt::Pattern* pat) {
  for (;;) {
    if (auto* v = tt::as<tt::Tpat_var>(pat->pat_desc)) {
      f(v->id);
      return;
    }
    if (auto* a = tt::as<tt::Tpat_alias>(pat->pat_desc)) {
      f(a->id);
      pat = a->pat;
      continue;
    }
    return;
  }
}

lam_t transl_ident(const ScopedLocation& loc, env::t env, TypeExpr* ty, Path::t path, const ValueDescription* desc) {
  switch (desc->val_kind.kind) {
    case typing::ValueKind::Kind::Val_prim:
      return translprim::transl_primitive(loc, desc->val_kind.prim, env, ty, path);
    case typing::ValueKind::Kind::Val_anc: throw Error(to_location(loc), Error::Kind::Free_super_var);
    case typing::ValueKind::Kind::Val_reg:
    case typing::ValueKind::Kind::Val_self: return L::transl_value_path(loc, env, path);
    default: fatal_error("Translcore.transl_exp: bad Texp_ident");
  }
}

bool is_unreachable(const tt::Expression* e) { return e->exp_desc->kind == tt::ExpressionDesc::Kind::Texp_unreachable; }

const tt::Case* case_with_lhs(const tt::Case* c, const tt::Pattern* lhs) {
  return make<tt::Case>(tt::Case{lhs, c->c_cont, c->c_guard, c->c_rhs});
}

// forward declarations
lam_t transl_exp1(scopes sc, bool in_new_scope, const tt::Expression* e);
lam_t transl_exp0(bool in_new_scope, scopes sc, const tt::Expression* e);
std::vector<lam_t> transl_list(scopes sc, Slice<const tt::Expression*> expr_list);
std::pair<std::vector<lam_t>, std::vector<ValueKind>> transl_list_with_shape(scopes sc,
                                                                             Slice<const tt::Expression*> l);
lam_t transl_guard(scopes sc, const tt::Expression* guard, const tt::Expression* rhs);
matching::PatAction transl_case(scopes sc, Ident::t cont, const tt::Case* c);
std::vector<matching::PatAction> transl_cases(scopes sc, Ident::t cont, Slice<const tt::Case*> cases);
matching::PatAction transl_case_try(scopes sc, const tt::Case* c);
std::vector<matching::PatAction> transl_cases_try(scopes sc, Slice<const tt::Case*> cases);
lam_t transl_function(scopes sc, const tt::Expression* e, Slice<const tt::FunctionParam*> params,
                      const tt::FunctionBody* body);
FunInfo transl_function_without_attributes(scopes sc, env::t env, const Location& loc, L::IntRef* repr,
                                           const std::vector<const tt::FunctionParam*>& params,
                                           const tt::FunctionBody* body);
FunInfo transl_curried_function(scopes sc, env::t env, const Location& loc, ValueKind return_, L::IntRef* repr,
                                const std::vector<const tt::FunctionParam*>& params, const tt::FunctionBody* body);
lam_t transl_setinstvar(scopes sc, const ScopedLocation& loc, lam_t self, lam_t var, const tt::Expression* expr);
lam_t transl_record(scopes sc, const Location& loc, env::t env, Slice<tt::RecordField> fields,
                    const RecordRepresentation& repres, const tt::Expression* opt_init_expr);
std::pair<lam_t, lam_t> transl_atomic_loc(scopes sc, const tt::Expression* arg, const LabelDescription* lbl);
lam_t transl_match(scopes sc, const tt::Expression* e, const tt::Expression* arg, Slice<const tt::Case*> pat_expr_list,
                   tt::Partial partial);
struct ValCases {
  std::vector<const tt::Case*> cases;
  tt::Partial partial;
};
lam_t transl_handler(scopes sc, const tt::Expression* e, const tt::Expression* body, const ValCases* val_caselist,
                     Slice<const tt::Case*> exn_caselist, Slice<const tt::Case*> eff_caselist);
lam_t transl_letop(scopes sc, const Location& loc, env::t env, const tt::BindingOp* let_,
                   Slice<const tt::BindingOp*> ands, Ident::t param, const tt::Case* c, tt::Partial partial);

lam_t transl_exp_(scopes sc, const tt::Expression* e) { return transl_exp1(sc, false, e); }

// ~in_new_scope tracks whether we just opened a new scope.
lam_t transl_exp1(scopes sc, bool in_new_scope, const tt::Expression* e) {
  using EK = tt::ExpressionDesc::Kind;
  EK k = e->exp_desc->kind;
  // Whether classes for immediate objects must be cached
  bool eval_once = !(k == EK::Texp_function || k == EK::Texp_for || k == EK::Texp_while);
  if (eval_once) return transl_exp0(in_new_scope, sc, e);
  return translobj::oo_wrap(e->exp_env, true, [&] { return transl_exp0(in_new_scope, sc, e); });
}

lam_t transl_exp0(bool in_new_scope, scopes sc, const tt::Expression* e) {
  using EK = tt::ExpressionDesc::Kind;
  const tt::ExpressionDesc* d = e->exp_desc;
  switch (d->kind) {
    case EK::Texp_ident: {
      auto* x = static_cast<const tt::Texp_ident*>(d);
      return transl_ident(of_location(sc, e->exp_loc), e->exp_env, e->exp_type, x->path, x->vd);
    }
    case EK::Texp_constant: return L::lambda_of_const(static_cast<const tt::Texp_constant*>(d)->c);
    case EK::Texp_let: {
      auto* x = static_cast<const tt::Texp_let*>(d);
      lam_t body = event_before(sc, x->body, transl_exp_(sc, x->body));
      return transl_let(sc, false, x->rec, x->vbs, body);
    }
    case EK::Texp_function: {
      auto* x = static_cast<const tt::Texp_function*>(d);
      scopes sc2 = in_new_scope ? sc : debuginfo::enter_anonymous_function(sc);
      return transl_function(sc2, e, x->params, x->body);
    }
    case EK::Texp_apply: {
      auto* x = static_cast<const tt::Texp_apply*>(d);
      const tt::Expression* funct = x->fn;
      if (auto* id = tt::as<tt::Texp_ident>(funct->exp_desc);
          id && id->vd->val_kind.kind == typing::ValueKind::Kind::Val_prim) {
        const PrimitiveDescription* p = id->vd->val_kind.prim;
        bool all_args = true;
        for (const tt::LabeledArg& a : x->args)
          if (a.arg.omitted) all_args = false;
        if (static_cast<long>(x->args.size()) >= p->prim_arity && all_args) {
          std::size_t n = static_cast<std::size_t>(p->prim_arity);
          Slice<tt::LabeledArg> extra_args{x->args.begin() + n, x->args.size() - n};
          std::vector<const tt::Expression*> arg_exps;
          for (std::size_t i = 0; i < n; i++) arg_exps.push_back(x->args[i].arg.arg);
          std::vector<lam_t> args = transl_list(sc, slice(arg_exps));
          const tt::Expression* prim_exp = extra_args.empty() ? e : nullptr;
          lam_t lam = translprim::transl_primitive_application(of_location(sc, e->exp_loc), p, e->exp_env,
                                                               funct->exp_type, id->path, prim_exp, slice(args),
                                                               slice(arg_exps));
          if (extra_args.empty()) return lam;
          TailcallAttribute tailcall = translattribute::get_tailcall_attribute(funct);
          InlineAttribute inlined = translattribute::get_inlined_attribute(funct);
          SpecialiseAttribute specialised = translattribute::get_specialised_attribute(funct);
          return event_after(sc, e,
                             transl_apply(sc, tailcall, inlined, specialised, lam, extra_args,
                                          of_location(sc, e->exp_loc)));
        }
      }
      TailcallAttribute tailcall = translattribute::get_tailcall_attribute(funct);
      InlineAttribute inlined = translattribute::get_inlined_attribute(funct);
      SpecialiseAttribute specialised = translattribute::get_specialised_attribute(funct);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      lam_t f = transl_exp_(sc, funct);
      return event_after(sc, e, transl_apply(sc, tailcall, inlined, specialised, f, x->args, loc));
    }
    case EK::Texp_match: {
      auto* x = static_cast<const tt::Texp_match*>(d);
      if (x->eff_cases.empty()) return transl_match(sc, e, x->exp, x->comp_cases, x->partial);
      // need to separate the values from exceptions for transl_handler
      ValCases vals{{}, x->partial};
      std::vector<const tt::Case*> exns;
      for (const tt::Case* c : x->comp_cases) {
        if (is_unreachable(c->c_rhs)) continue;
        auto [val_pat, exn_pat] = tt::split_pattern(c->c_lhs);
        if (!val_pat && !exn_pat) fatal_error("Translcore: split_case");
        if (val_pat) vals.cases.push_back(case_with_lhs(c, val_pat));
        if (exn_pat) exns.push_back(case_with_lhs(c, exn_pat));
      }
      return transl_handler(sc, e, x->exp, &vals, slice(exns), x->eff_cases);
    }
    case EK::Texp_try: {
      auto* x = static_cast<const tt::Texp_try*>(d);
      if (x->eff_cases.empty()) {
        Ident::t id = typecore::name_cases(OCAML_LIT("exn"), x->exn_cases);
        std::vector<matching::PatAction> cases = transl_cases_try(sc, x->exn_cases);
        lam_t handler = matching::for_trywith(sc, e->exp_loc, L::lvar(id), slice(cases));
        return L::ltrywith(transl_exp_(sc, x->exp), id, handler);
      }
      return transl_handler(sc, e, x->exp, nullptr, x->exn_cases, x->eff_cases);
    }
    case EK::Texp_tuple: {
      auto* x = static_cast<const tt::Texp_tuple*>(d);
      std::vector<const tt::Expression*> el;
      for (const tt::LabeledExpression& le : x->el) el.push_back(le.exp);
      auto [ll, shape] = transl_list_with_shape(sc, slice(el));
      if (all_constant(ll)) {
        std::vector<const StructuredConstant*> cl;
        for (lam_t l : ll) cl.push_back(extract_constant(l));
        return L::lconst(const_block(0, cl));
      }
      return L::lprim(pmakeblock(0, MutableFlag::Immutable, &shape), slice(ll), of_location(sc, e->exp_loc));
    }
    case EK::Texp_construct: {
      auto* x = static_cast<const tt::Texp_construct*>(d);
      const ConstructorDescription* cstr = x->cstr;
      auto [ll, shape] = transl_list_with_shape(sc, x->args);
      if (cstr->cstr_inlined) {
        if (ll.size() != 1) fatal_error("Translcore: inlined constructor");
        return ll[0];
      }
      switch (cstr->cstr_tag.kind) {
        case ConstructorTag::Kind::Cstr_constant: return L::lconst(L::const_int(cstr->cstr_tag.n));
        case ConstructorTag::Kind::Cstr_unboxed:
          if (ll.size() != 1) fatal_error("Translcore: unboxed constructor");
          return ll[0];
        case ConstructorTag::Kind::Cstr_block:
          if (all_constant(ll)) {
            std::vector<const StructuredConstant*> cl;
            for (lam_t l : ll) cl.push_back(extract_constant(l));
            return L::lconst(const_block(cstr->cstr_tag.n, cl));
          }
          return L::lprim(pmakeblock(cstr->cstr_tag.n, MutableFlag::Immutable, &shape), slice(ll),
                          of_location(sc, e->exp_loc));
        case ConstructorTag::Kind::Cstr_extension: {
          lam_t lam = L::transl_extension_path(of_location(sc, e->exp_loc), e->exp_env, cstr->cstr_tag.ext_path);
          if (cstr->cstr_tag.ext_constant) return lam;
          std::vector<ValueKind> shape2{ValueKind::gen()};
          shape2.insert(shape2.end(), shape.begin(), shape.end());
          std::vector<lam_t> args{lam};
          args.insert(args.end(), ll.begin(), ll.end());
          return L::lprim(pmakeblock(0, MutableFlag::Immutable, &shape2), slice(args), of_location(sc, e->exp_loc));
        }
      }
      fatal_error("Translcore: constructor tag");
    }
    case EK::Texp_extension_constructor: {
      auto* x = static_cast<const tt::Texp_extension_constructor*>(d);
      return L::transl_extension_path(of_location(sc, e->exp_loc), e->exp_env, x->path);
    }
    case EK::Texp_variant: {
      auto* x = static_cast<const tt::Texp_variant*>(d);
      long tag = btype::hash_variant(x->label);
      if (!x->arg) return L::lconst(L::const_int(tag));
      lam_t lam = transl_exp_(sc, x->arg);
      if (as<L::Lconst>(lam)) return L::lconst(const_block(0, {L::const_int(tag), extract_constant(lam)}));
      return L::lprim(pmakeblock(0, MutableFlag::Immutable, nullptr), slice({L::lconst(L::const_int(tag)), lam}),
                      of_location(sc, e->exp_loc));
    }
    case EK::Texp_record: {
      auto* x = static_cast<const tt::Texp_record*>(d);
      return transl_record(sc, e->exp_loc, e->exp_env, x->fields, x->representation, x->extended_expression);
    }
    case EK::Texp_atomic_loc: {
      auto* x = static_cast<const tt::Texp_atomic_loc*>(d);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      auto [arg, lbl] = transl_atomic_loc(sc, x->exp, x->label);
      return L::make_atomic_loc(loc, arg, lbl);
    }
    case EK::Texp_field: {
      auto* x = static_cast<const tt::Texp_field*>(d);
      const LabelDescription* lbl = x->label;
      if (lbl->lbl_atomic == AtomicFlag::Atomic) {
        auto [arg, lbll] = transl_atomic_loc(sc, x->exp, lbl);
        ScopedLocation loc = of_location(sc, e->exp_loc);
        return L::lprim(L::prim(PK::Patomic_load), slice({arg, lbll}), loc);
      }
      lam_t targ = transl_exp_(sc, x->exp);
      using RK = RecordRepresentation::Kind;
      switch (lbl->lbl_repres.kind) {
        case RK::Record_regular:
        case RK::Record_inlined:
          return L::lprim(pfield(lbl->lbl_pos, typeopt::maybe_pointer(e), lbl->lbl_mut), slice({targ}),
                          of_location(sc, e->exp_loc));
        case RK::Record_unboxed: return targ;
        case RK::Record_float: return L::lprim(pfloatfield(lbl->lbl_pos), slice({targ}), of_location(sc, e->exp_loc));
        case RK::Record_extension:
          return L::lprim(pfield(lbl->lbl_pos + 1, typeopt::maybe_pointer(e), lbl->lbl_mut), slice({targ}),
                          of_location(sc, e->exp_loc));
      }
      fatal_error("Translcore: field");
    }
    case EK::Texp_setfield: {
      auto* x = static_cast<const tt::Texp_setfield*>(d);
      const LabelDescription* lbl = x->label;
      if (lbl->lbl_atomic == AtomicFlag::Atomic) {
        static const PrimitiveDescription* prim = simple("caml_atomic_exchange_field", 3, false);
        auto [arg, lbll] = transl_atomic_loc(sc, x->exp, lbl);
        lam_t newval = transl_exp_(sc, x->value);
        ScopedLocation loc = of_location(sc, e->exp_loc);
        return L::lprim(L::prim(PK::Pignore), slice({L::lprim(pccall(prim), slice({arg, lbll, newval}), loc)}), loc);
      }
      using RK = RecordRepresentation::Kind;
      using IA = L::InitializationOrAssignment;
      Primitive access = L::prim(PK::Pignore);
      switch (lbl->lbl_repres.kind) {
        case RK::Record_regular:
        case RK::Record_inlined:
          access = psetfield(lbl->lbl_pos, typeopt::maybe_pointer(x->value), IA::Assignment);
          break;
        case RK::Record_unboxed: fatal_error("Translcore: setfield on unboxed record");
        case RK::Record_float: access = psetfloatfield(lbl->lbl_pos, IA::Assignment); break;
        case RK::Record_extension:
          access = psetfield(lbl->lbl_pos + 1, typeopt::maybe_pointer(x->value), IA::Assignment);
          break;
      }
      // [transl_exp arg; transl_exp newval]: right to left
      lam_t newval = transl_exp_(sc, x->value);
      lam_t arg = transl_exp_(sc, x->exp);
      return L::lprim(access, slice({arg, newval}), of_location(sc, e->exp_loc));
    }
    case EK::Texp_array: {
      auto* x = static_cast<const tt::Texp_array*>(d);
      L::ArrayKind kind = typeopt::array_kind(e);
      std::vector<lam_t> ll = transl_list(sc, x->el);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      auto makearray = [&](MutableFlag mutability) { return L::lprim(pmakearray(kind, mutability), slice(ll), loc); };
      auto duparray_to_mutable = [&](lam_t array) {
        return L::lprim(pduparray(kind, MutableFlag::Mutable), slice({array}), loc);
      };
      lam_t imm_array = makearray(MutableFlag::Immutable);
      try {
        // Deactivate constant optimization if array is small enough
        if (x->mut == MutableFlag::Mutable &&
            static_cast<long>(ll.size()) <= use_dup_for_constant_mutable_arrays_bigger_than)
          throw NotConstant{};
        std::vector<const StructuredConstant*> cl;
        try {
          for (lam_t l : ll) cl.push_back(extract_constant(l));
        } catch (const NotConstant&) {
          if (kind == L::ArrayKind::Pfloatarray && x->mut == MutableFlag::Mutable)
            return duparray_to_mutable(imm_array);
          throw;
        }
        lam_t cst;
        switch (kind) {
          case L::ArrayKind::Paddrarray:
          case L::ArrayKind::Pintarray: cst = L::lconst(const_block(0, cl)); break;
          case L::ArrayKind::Pfloatarray: {
            std::vector<std::string_view> fl;
            for (const StructuredConstant* c : cl) fl.push_back(extract_float(c));
            auto* c = make<StructuredConstant>();
            c->kind = StructuredConstant::Kind::Const_float_array;
            c->floats = slice(fl);
            cst = L::lconst(c);
            break;
          }
          case L::ArrayKind::Pgenarray: throw NotConstant{};  // can this really happen?
        }
        if (x->mut == MutableFlag::Mutable) return duparray_to_mutable(cst);
        return cst;
      } catch (const NotConstant&) {
        return makearray(x->mut);
      }
    }
    case EK::Texp_ifthenelse: {
      auto* x = static_cast<const tt::Texp_ifthenelse*>(d);
      if (x->else_) {
        lam_t ifnot = event_before(sc, x->else_, transl_exp_(sc, x->else_));
        lam_t ifso = event_before(sc, x->then_, transl_exp_(sc, x->then_));
        lam_t cond = transl_exp_(sc, x->cond);
        return L::lifthenelse(cond, ifso, ifnot);
      }
      lam_t ifso = event_before(sc, x->then_, transl_exp_(sc, x->then_));
      lam_t cond = transl_exp_(sc, x->cond);
      return L::lifthenelse(cond, ifso, L::lambda_unit());
    }
    case EK::Texp_sequence: {
      auto* x = static_cast<const tt::Texp_sequence*>(d);
      lam_t l2 = event_before(sc, x->e2, transl_exp_(sc, x->e2));
      lam_t l1 = transl_exp_(sc, x->e1);
      return L::lsequence(l1, l2);
    }
    case EK::Texp_while: {
      auto* x = static_cast<const tt::Texp_while*>(d);
      lam_t body = event_before(sc, x->body, transl_exp_(sc, x->body));
      lam_t cond = transl_exp_(sc, x->cond);
      return L::lwhile(cond, body);
    }
    case EK::Texp_for: {
      auto* x = static_cast<const tt::Texp_for*>(d);
      lam_t body = event_before(sc, x->body, transl_exp_(sc, x->body));
      lam_t hi = transl_exp_(sc, x->hi);
      lam_t lo = transl_exp_(sc, x->lo);
      return L::lfor(x->id, lo, hi, x->dir, body);
    }
    case EK::Texp_send: {
      auto* x = static_cast<const tt::Texp_send*>(d);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      lam_t lam;
      switch (x->meth.kind) {
        case tt::Meth::Kind::Tmeth_val: {
          lam_t obj = transl_exp_(sc, x->obj);
          lam = L::lsend(L::MethKind::Self, L::lvar(x->meth.id), obj, {}, loc);
          break;
        }
        case tt::Meth::Kind::Tmeth_name: {
          lam_t obj = transl_exp_(sc, x->obj);
          auto [tag, cache] = translobj::meth(obj, x->meth.name);
          L::MethKind kind = cache.empty() ? L::MethKind::Public : L::MethKind::Cached;
          lam = L::lsend(kind, tag, obj, cache, loc);
          break;
        }
        case tt::Meth::Kind::Tmeth_ancestor: {
          lam_t self = L::transl_value_path(loc, e->exp_env, x->meth.path);
          lam = mkapply(L::lvar(x->meth.id), {self}, loc);
          break;
        }
      }
      return event_after(sc, e, lam);
    }
    case EK::Texp_new: {
      auto* x = static_cast<const tt::Texp_new*>(d);
      ScopedLocation loc = of_location(sc, x->lid.loc);
      lam_t func = L::lprim(pfield(0, L::ImmediateOrPointer::Pointer, MutableFlag::Mutable),
                            slice({L::transl_class_path(loc, e->exp_env, x->path)}), loc);
      return mkapply(func, {L::lambda_unit()}, loc);
    }
    case EK::Texp_instvar: {
      auto* x = static_cast<const tt::Texp_instvar*>(d);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      lam_t self = L::transl_value_path(loc, e->exp_env, x->self_path);
      lam_t var = L::transl_value_path(loc, e->exp_env, x->path);
      return L::lprim(L::prim(PK::Pfield_computed), slice({self, var}), loc);
    }
    case EK::Texp_setinstvar: {
      auto* x = static_cast<const tt::Texp_setinstvar*>(d);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      lam_t self = L::transl_value_path(loc, e->exp_env, x->self_path);
      lam_t var = L::transl_value_path(loc, e->exp_env, x->path);
      return transl_setinstvar(sc, loc, self, var, x->value);
    }
    case EK::Texp_override: {
      auto* x = static_cast<const tt::Texp_override*>(d);
      ScopedLocation loc = of_location(sc, e->exp_loc);
      lam_t self = L::transl_value_path(loc, e->exp_env, x->self_path);
      Ident::t cpy = Ident::create_local(OCAML_LIT("copy"));
      // List.fold_right: from the end
      lam_t rem = L::lvar(cpy);
      for (std::size_t k = x->fields.size(); k-- > 0;) {
        const tt::OverrideField& f = x->fields[k];
        rem = L::lsequence(transl_setinstvar(sc, debuginfo::loc_unknown(), L::lvar(cpy), L::lvar(f.id), f.exp), rem);
      }
      lam_t copy = mkapply(translobj::oo_prim("copy"), {self}, debuginfo::loc_unknown());
      return L::llet(L::LetKind::Strict, ValueKind::gen(), cpy, copy, rem);
    }
    case EK::Texp_pack: {
      auto* x = static_cast<const tt::Texp_pack*>(d);
      return transl_module(sc, tt::tcoerce_none(), nullptr, x->me);
    }
    case EK::Texp_assert: {
      auto* x = static_cast<const tt::Texp_assert*>(d);
      if (auto* c = tt::as<tt::Texp_construct>(x->exp->exp_desc); c && c->cstr->cstr_name == "false")
        return assert_failed(x->loc, sc, e);
      if (clflags::noassert) return L::lambda_unit();
      lam_t failed = assert_failed(x->loc, sc, e);
      lam_t cond = transl_exp_(sc, x->exp);
      return L::lifthenelse(cond, L::lambda_unit(), failed);
    }
    case EK::Texp_lazy: {
      const tt::Expression* le = static_cast<const tt::Texp_lazy*>(d)->exp;
      typeopt::LazySummary s = typeopt::classify_lazy_argument(le);
      if (s.kind == typeopt::LazySummary::Kind::Eager) {
        if (s.repr == typeopt::LazySummary::ForwardRepr::Shortcut) return transl_exp_(sc, le);
        Primitive p = L::prim(PK::Pmakelazyblock);
        p.lazy_tag = L::LazyBlockTag::Forward_tag;
        return L::lprim(p, slice({transl_exp_(sc, le)}), of_location(sc, le->exp_loc));
      }
      // lfunction ~kind ~params ~return ~body ~attr ~loc: ~body before ~params
      ScopedLocation loc = of_location(sc, le->exp_loc);
      FunctionAttribute attr = function_attribute_disallowing_arity_fusion();
      lam_t body = transl_exp_(sc, le);
      Ident::t param = Ident::create_local(OCAML_LIT("param"));
      lam_t fn = L::lfunction(L::FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}), ValueKind::gen(),
                              body, attr, loc);
      Primitive p = L::prim(PK::Pmakelazyblock);
      p.lazy_tag = L::LazyBlockTag::Lazy_tag;
      return L::lprim(p, slice({fn}), of_location(sc, le->exp_loc));
    }
    case EK::Texp_object: {
      auto* x = static_cast<const tt::Texp_object*>(d);
      Ident::t cl = Ident::create_local(OCAML_LIT("object"));
      auto* desc = make<tt::Tcl_structure>();
      desc->kind = tt::Tcl_structure::K;
      desc->cs = x->cs;
      auto* cty = make<typing::ClassType>();
      cty->kind = typing::ClassType::Kind::Cty_signature;
      cty->sign = const_cast<ClassSignature*>(x->cs->cstr_type);
      auto* ce = make<tt::ClassExpr>(tt::ClassExpr{desc, e->exp_loc, cty, e->exp_env, {}});
      return transl_object(sc, cl, x->meths, ce);
    }
    case EK::Texp_letop: {
      auto* x = static_cast<const tt::Texp_letop*>(d);
      return event_after(sc, e, transl_letop(sc, e->exp_loc, e->exp_env, x->let_, x->ands, x->param, x->body,
                                             x->partial));
    }
    case EK::Texp_unreachable: throw Error(e->exp_loc, Error::Kind::Unreachable_reached);
    case EK::Texp_struct_item: {
      auto* x = static_cast<const tt::Texp_struct_item*>(d);
      const tt::Expression* body = x->body;
      return transl_struct_item(sc, {}, nullptr, x->item, [&](Slice<Ident::t>) { return transl_exp_(sc, body); });
    }
  }
  fatal_error("Translcore.transl_exp0");
}

std::vector<lam_t> transl_list(scopes sc, Slice<const tt::Expression*> expr_list) {
  std::vector<lam_t> r;
  for (const tt::Expression* e : expr_list) r.push_back(transl_exp_(sc, e));
  return r;
}

std::pair<std::vector<lam_t>, std::vector<ValueKind>> transl_list_with_shape(scopes sc,
                                                                             Slice<const tt::Expression*> l) {
  std::vector<lam_t> ll;
  std::vector<ValueKind> shape;
  for (const tt::Expression* e : l) {
    ValueKind k = typeopt::value_kind(e->exp_env, e->exp_type);
    ll.push_back(transl_exp_(sc, e));
    shape.push_back(k);
  }
  return {ll, shape};
}

lam_t transl_guard(scopes sc, const tt::Expression* guard, const tt::Expression* rhs) {
  lam_t expr = event_before(sc, rhs, transl_exp_(sc, rhs));
  if (!guard) return expr;
  return event_before(sc, guard, L::lifthenelse(transl_exp_(sc, guard), expr, L::staticfail()));
}

// cont: nullptr = None
lam_t transl_cont(Ident::t cont, Ident::t c_cont, lam_t body) {
  if (cont && c_cont) return L::llet(L::LetKind::Alias, ValueKind::gen(), c_cont, L::lvar(cont), body);
  if (!cont && c_cont) fatal_error("Translcore.transl_cont");
  return body;
}

matching::PatAction transl_case(scopes sc, Ident::t cont, const tt::Case* c) {
  return {c->c_lhs, transl_cont(cont, c->c_cont ? c->c_cont->cont_id : nullptr,
                                transl_guard(sc, c->c_guard, c->c_rhs))};
}

std::vector<matching::PatAction> transl_cases(scopes sc, Ident::t cont, Slice<const tt::Case*> cases) {
  std::vector<matching::PatAction> r;
  for (const tt::Case* c : cases)
    if (!is_unreachable(c->c_rhs)) r.push_back(transl_case(sc, cont, c));
  return r;
}

matching::PatAction transl_case_try(scopes sc, const tt::Case* c) {
  iter_exn_names(translprim::add_exception_ident, c->c_lhs);
  struct Always {
    const tt::Pattern* p;
    ~Always() { iter_exn_names(translprim::remove_exception_ident, p); }
  } always{c->c_lhs};
  return {c->c_lhs, transl_guard(sc, c->c_guard, c->c_rhs)};
}

std::vector<matching::PatAction> transl_cases_try(scopes sc, Slice<const tt::Case*> cases) {
  std::vector<matching::PatAction> r;
  for (const tt::Case* c : cases)
    if (!is_unreachable(c->c_rhs)) r.push_back(transl_case_try(sc, c));
  return r;
}

// Misc.Stdlib.List.chunks_of
std::vector<std::vector<Param>> chunks_of(long n, const std::vector<Param>& l) {
  if (n <= 0) throw std::invalid_argument("chunks_of");
  std::vector<std::vector<Param>> r;
  for (std::size_t i = 0; i < l.size(); i += static_cast<std::size_t>(n)) {
    std::size_t e = std::min(l.size(), i + static_cast<std::size_t>(n));
    r.emplace_back(l.begin() + static_cast<long>(i), l.begin() + static_cast<long>(e));
  }
  return r;
}

// There are two cases in function translation:
//  - [Tupled]. It takes a tupled argument, and we can flatten it.
//  - [Curried]. It takes each argument individually.
FunInfo transl_tupled_function(scopes sc, env::t env, const Location& loc, ValueKind return_, L::IntRef* repr,
                               const std::vector<const tt::FunctionParam*>& params, const tt::FunctionBody* body) {
  // Cases are eligible for flattening if they belong to the only param.
  std::optional<std::pair<std::vector<const tt::Case*>, tt::Partial>> eligible_cases;
  if (params.empty() && body->kind == tt::FunctionBody::Kind::Tfunction_cases) {
    eligible_cases.emplace(std::vector<const tt::Case*>(body->cases.begin(), body->cases.end()), body->partial);
  } else if (params.size() == 1 && params[0]->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_pat &&
             body->kind == tt::FunctionBody::Kind::Tfunction_body) {
    const tt::Case* c = make<tt::Case>(tt::Case{params[0]->fp_kind.pat, nullptr, nullptr, body->body});
    eligible_cases.emplace(std::vector<const tt::Case*>{c}, params[0]->fp_partial);
  }
  if (eligible_cases && !eligible_cases->first.empty()) {
    const std::vector<const tt::Case*>& cases = eligible_cases->first;
    auto* tup = tt::as<tt::Tpat_tuple>(cases[0]->c_lhs->pat_desc);
    if (tup && clflags::native_code && static_cast<long>(tup->pats.size()) <= L::max_arity()) {
      try {
        long size = static_cast<long>(tup->pats.size());
        struct PatsExpr {
          std::vector<const tt::Pattern*> pats;
          const tt::Expression* guard;
          const tt::Expression* rhs;
        };
        std::vector<PatsExpr> pats_expr_list;
        for (const tt::Case* c : cases)
          pats_expr_list.push_back({matching::flatten_pattern(size, c->c_lhs), c->c_guard, c->c_rhs});
        // if the match is partial, we cannot rely on GADT equations
        std::optional<env::LocalEquations> local_equations;
        if (eligible_cases->second == tt::Partial::Partial) local_equations = env::freeze_local_equations(env);
        const env::LocalEquations* leq = local_equations ? &*local_equations : nullptr;
        // All the patterns might not share the same types. We must take the
        // union of the patterns types
        std::vector<ValueKind> kinds;
        for (const tt::Pattern* pat : pats_expr_list[0].pats) kinds.push_back(typeopt::pattern_kind(leq, pat));
        for (std::size_t c = 1; c < pats_expr_list.size(); c++)
          for (std::size_t i = 0; i < kinds.size(); i++) {
            const tt::Pattern* pat = pats_expr_list[c].pats[i];
            kinds[i] = typeopt::value_kind_union(kinds[i], typeopt::pattern_kind(leq, pat));
          }
        std::vector<Param> tparams;
        for (const ValueKind& kind : kinds) tparams.push_back(Param{Ident::create_local(OCAML_LIT("param")), kind});
        std::vector<Ident::t> ps;
        for (const Param& p : tparams) ps.push_back(p.id);
        // transl_tupled_cases
        std::vector<matching::PatsAction> tcases;
        for (const PatsExpr& pe : pats_expr_list)
          if (!is_unreachable(pe.rhs)) tcases.push_back({slice(pe.pats), transl_guard(sc, pe.guard, pe.rhs)});
        lam_t b = matching::for_tupled_function(sc, loc, slice(ps), slice(tcases), eligible_cases->second);
        return FunInfo{L::FunctionKind::Tupled, tparams, return_, b};
      } catch (const matching::CannotFlatten&) {
        return transl_curried_function(sc, env, loc, return_, repr, params, body);
      }
    }
  }
  return transl_curried_function(sc, env, loc, return_, repr, params, body);
}

FunInfo transl_function_without_attributes(scopes sc, env::t env, const Location& loc, L::IntRef* repr,
                                           const std::vector<const tt::FunctionParam*>& params,
                                           const tt::FunctionBody* body) {
  ValueKind return_ = ValueKind::gen();
  if (body->kind == tt::FunctionBody::Kind::Tfunction_body) {
    return_ = typeopt::value_kind(body->body->exp_env, body->body->exp_type);
  } else if (!body->cases.empty()) {
    const tt::Expression* c_rhs = body->cases[0]->c_rhs;
    return_ = typeopt::value_kind(c_rhs->exp_env, c_rhs->exp_type);
  }
  // (Tfunction_cases { cases = [] }: with Camlp4/ppx, a pattern matching
  // might be empty -- Pgenval)
  return transl_tupled_function(sc, env, loc, return_, repr, params, body);
}

FunInfo transl_curried_function(scopes sc, env::t env, const Location& loc, ValueKind return_, L::IntRef* repr,
                                const std::vector<const tt::FunctionParam*>& params, const tt::FunctionBody* fbody) {
  std::optional<Param> cases_param;
  lam_t body;
  if (fbody->kind == tt::FunctionBody::Kind::Tfunction_body) {
    body = event_before(sc, fbody->body, transl_exp_(sc, fbody->body));
  } else {
    ValueKind kind = ValueKind::gen();
    if (!fbody->cases.empty()) {
      const tt::Pattern* pat = fbody->cases[0]->c_lhs;
      kind = typeopt::value_kind(pat->pat_env, pat->pat_type);
      for (std::size_t i = 1; i < fbody->cases.size(); i++) {
        const tt::Pattern* p = fbody->cases[i]->c_lhs;
        kind = typeopt::value_kind_union(kind, typeopt::value_kind(p->pat_env, p->pat_type));
      }
    }
    std::vector<matching::PatAction> cases = transl_cases(sc, nullptr, fbody->cases);
    body = matching::for_function(sc, fbody->loc, repr, L::lvar(fbody->param), slice(cases), fbody->partial);
    cases_param = Param{fbody->param, kind};
  }
  // We freeze local GADTs equations to the set existing before the
  // first partial match to avoid using equations that might be only
  // valid if a match succeeds.
  std::vector<std::optional<env::LocalEquations>> param_equations;
  {
    std::optional<env::LocalEquations> local_equations;
    env::t prev_env = env;
    for (const tt::FunctionParam* fp : params) {
      if (!local_equations &&
          (fp->fp_partial == tt::Partial::Partial ||
           // in default arguments [?(pat=exp)], [exp] can raise and
           // thus even a [Total] pattern can fail.
           fp->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_optional_default))
        local_equations = env::freeze_local_equations(prev_env);
      prev_env = fp->fp_kind.pat->pat_env;
      param_equations.push_back(local_equations);
    }
  }
  // List.fold_right over the params: from the last
  std::vector<Param> lparams;  // built reversed
  if (cases_param) lparams.push_back(*cases_param);
  for (std::size_t k = params.size(); k-- > 0;) {
    const tt::FunctionParam* fp = params[k];
    Ident::t param = fp->fp_param;
    const Location& param_loc = fp->fp_loc;
    if (fp->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_pat) {
      const tt::Pattern* pat = fp->fp_kind.pat;
      const std::optional<env::LocalEquations>& leq = param_equations[k];
      ValueKind kind = typeopt::pattern_kind(leq ? &*leq : nullptr, pat);
      body = matching::for_function(sc, param_loc, nullptr, L::lvar(param), slice({matching::PatAction{pat, body}}),
                                    fp->fp_partial);
      lparams.push_back(Param{param, kind});
    } else {
      const tt::Expression* d = fp->fp_kind.default_;
      lam_t default_arg = event_before(sc, d, transl_exp_(sc, d));
      body = matching::for_optional_arg_default(sc, param_loc, fp->fp_kind.pat, default_arg, param, body);
      // The optional param is Pgenval as it's an option.
      lparams.push_back(Param{param, ValueKind::gen()});
    }
  }
  std::vector<Param> all_params(lparams.rbegin(), lparams.rend());
  // chunk params according to Lambda.max_arity
  std::vector<std::vector<Param>> chunks = chunks_of(L::max_arity(), all_params);
  if (chunks.empty()) fatal_error("attempted to translate a function with zero arguments");
  for (std::size_t k = chunks.size(); k-- > 1;) {
    FunctionAttribute attr = function_attribute_disallowing_arity_fusion();
    ScopedLocation sloc = of_location(sc, loc);
    body = L::lfunction(L::FunctionKind::Curried, slice(chunks[k]), return_, body, attr, sloc);
    // we return Pgenval (for a function) after the rightmost chunk.
    return_ = ValueKind::gen();
  }
  return FunInfo{L::FunctionKind::Curried, chunks[0], return_, body};
}

lam_t transl_function(scopes sc, const tt::Expression* e, Slice<const tt::FunctionParam*> params,
                      const tt::FunctionBody* body) {
  FunInfo fi = event_function(sc, e, [&](L::IntRef* repr) {
    auto [ps, b] = fuse_method_arity(params, body);
    return transl_function_without_attributes(sc, e->exp_env, e->exp_loc, repr, ps, b);
  });
  FunctionAttribute attr = function_attribute_disallowing_arity_fusion();
  ScopedLocation loc = of_location(sc, e->exp_loc);
  lam_t lam = L::lfunction(fi.kind, slice(fi.params), fi.return_, fi.body, attr, loc);
  // Collect attributes from the Pexp_newtype node for locally abstract types.
  std::vector<const parsetree::Attribute*> attrs(e->exp_attributes.begin(), e->exp_attributes.end());
  for (const tt::ExpExtraItem& x : e->exp_extra)
    if (x.extra.kind == tt::ExpExtra::Kind::Texp_newtype) attrs.insert(attrs.begin(), x.attrs.begin(), x.attrs.end());
  return translattribute::add_function_attributes(lam, e->exp_loc, slice(attrs));
}

// Decides whether a pattern binding should introduce a new scope.
lam_t transl_bound_exp(scopes sc, bool in_structure, const tt::Pattern* pat, const tt::Expression* expr) {
  bool should_introduce_scope = expr->exp_desc->kind == tt::ExpressionDesc::Kind::Texp_function || in_structure;
  if (should_introduce_scope) {
    std::vector<Ident::t> ids = tt::pat_bound_idents(pat);
    if (!ids.empty()) return transl_scoped_exp(debuginfo::enter_value_definition(sc, ids[0]), expr);
  }
  return transl_exp_(sc, expr);
}

lam_t transl_setinstvar(scopes sc, const ScopedLocation& loc, lam_t self, lam_t var, const tt::Expression* expr) {
  lam_t v = transl_exp_(sc, expr);
  Primitive p = L::prim(PK::Psetfield_computed);
  p.ptr = typeopt::maybe_pointer(expr);
  p.init = L::InitializationOrAssignment::Assignment;
  return L::lprim(p, slice({self, var, v}), loc);
}

lam_t transl_record(scopes sc, const Location& loc, env::t env, Slice<tt::RecordField> fields,
                    const RecordRepresentation& repres, const tt::Expression* opt_init_expr) {
  using RK = RecordRepresentation::Kind;
  using IA = L::InitializationOrAssignment;
  long size = static_cast<long>(fields.size());
  // Determine if there are "enough" fields (only relevant if this is a
  // functional-style record update
  bool no_init = opt_init_expr == nullptr;
  if (no_init || size < config_max_young_wosize) {
    // Allocate new record with given fields (and remaining fields taken from
    // init_expr if any
    Ident::t init_id = Ident::create_local(OCAML_LIT("init"));
    std::vector<lam_t> ll;
    std::vector<ValueKind> shape;
    for (long i = 0; i < size; i++) {  // Array.mapi
      const tt::RecordLabelDefinition& definition = fields[i].def;
      if (definition.kept) {
        TypeExpr* typ = definition.ty;
        ValueKind field_kind = typeopt::value_kind(env, typ);
        Primitive access = L::prim(PK::Pignore);
        switch (repres.kind) {
          case RK::Record_regular:
          case RK::Record_inlined: access = pfield(i, typeopt::maybe_pointer_type(env, typ), definition.mut); break;
          case RK::Record_unboxed: fatal_error("Translcore.transl_record: Kept on an unboxed record");
          case RK::Record_extension:
            access = pfield(i + 1, typeopt::maybe_pointer_type(env, typ), definition.mut);
            break;
          case RK::Record_float: access = pfloatfield(i); break;
        }
        ll.push_back(L::lprim(access, slice({L::lvar(init_id)}), of_location(sc, loc)));
        shape.push_back(field_kind);
      } else {
        const tt::Expression* expr = definition.exp;
        ValueKind field_kind = typeopt::value_kind(expr->exp_env, expr->exp_type);
        ll.push_back(transl_exp_(sc, expr));
        shape.push_back(field_kind);
      }
    }
    MutableFlag mut = MutableFlag::Immutable;
    for (const tt::RecordField& f : fields)
      if (f.label->lbl_mut == MutableFlag::Mutable) mut = MutableFlag::Mutable;
    lam_t lam;
    // translcore.ml raises Not_constant for a mutable record, a field that
    // is not a constant, an extension record (tested first here)
    if (mut != MutableFlag::Mutable && repres.kind != RK::Record_extension && all_constant(ll)) {
      std::vector<const StructuredConstant*> cl;
      for (lam_t l : ll) cl.push_back(extract_constant(l));
      switch (repres.kind) {
        case RK::Record_regular: lam = L::lconst(const_block(0, cl)); break;
        case RK::Record_inlined: lam = L::lconst(const_block(repres.inlined_tag, cl)); break;
        case RK::Record_unboxed:
          if (cl.size() != 1) fatal_error("Translcore.transl_record: unboxed");
          lam = L::lconst(cl[0]);
          break;
        case RK::Record_float: {
          std::vector<std::string_view> fl;
          for (const StructuredConstant* c : cl) fl.push_back(extract_float(c));
          auto* c = make<StructuredConstant>();
          c->kind = StructuredConstant::Kind::Const_float_array;
          c->floats = slice(fl);
          lam = L::lconst(c);
          break;
        }
        case RK::Record_extension: break;
      }
    } else {
      ScopedLocation sloc = of_location(sc, loc);
      switch (repres.kind) {
        case RK::Record_regular: lam = L::lprim(pmakeblock(0, mut, &shape), slice(ll), sloc); break;
        case RK::Record_inlined: lam = L::lprim(pmakeblock(repres.inlined_tag, mut, &shape), slice(ll), sloc); break;
        case RK::Record_unboxed:
          if (ll.size() != 1) fatal_error("Translcore.transl_record: unboxed");
          lam = ll[0];
          break;
        case RK::Record_float: lam = L::lprim(pmakearray(L::ArrayKind::Pfloatarray, mut), slice(ll), sloc); break;
        case RK::Record_extension: {
          lam_t slot = L::transl_extension_path(sloc, env, repres.extension);
          std::vector<ValueKind> shape2{ValueKind::gen()};
          shape2.insert(shape2.end(), shape.begin(), shape.end());
          std::vector<lam_t> args{slot};
          args.insert(args.end(), ll.begin(), ll.end());
          lam = L::lprim(pmakeblock(0, mut, &shape2), slice(args), sloc);
          break;
        }
      }
    }
    if (!opt_init_expr) return lam;
    return L::llet(L::LetKind::Strict, ValueKind::gen(), init_id, transl_exp_(sc, opt_init_expr), lam);
  }
  // Take a shallow copy of the init record, then mutate the fields of the copy
  Ident::t copy_id = Ident::create_local(OCAML_LIT("newrecord"));
  lam_t cont = L::lvar(copy_id);
  for (const tt::RecordField& f : fields) {  // Array.fold_left update_field
    if (f.def.kept) continue;
    const LabelDescription* lbl = f.label;
    const tt::Expression* expr = f.def.exp;
    Primitive upd = L::prim(PK::Pignore);
    switch (repres.kind) {
      case RK::Record_regular:
      case RK::Record_inlined: upd = psetfield(lbl->lbl_pos, typeopt::maybe_pointer(expr), IA::Assignment); break;
      case RK::Record_unboxed: fatal_error("Translcore.transl_record: update of an unboxed record");
      case RK::Record_float: upd = psetfloatfield(lbl->lbl_pos, IA::Assignment); break;
      case RK::Record_extension:
        upd = psetfield(lbl->lbl_pos + 1, typeopt::maybe_pointer(expr), IA::Assignment);
        break;
    }
    lam_t v = transl_exp_(sc, expr);
    cont = L::lsequence(L::lprim(upd, slice({L::lvar(copy_id), v}), of_location(sc, loc)), cont);
  }
  Primitive dup = L::prim(PK::Pduprecord);
  dup.repr = repres;
  dup.n = size;
  lam_t init = L::lprim(dup, slice({transl_exp_(sc, opt_init_expr)}), of_location(sc, loc));
  return L::llet(L::LetKind::Strict, ValueKind::gen(), copy_id, init, cont);
}

std::pair<lam_t, lam_t> transl_atomic_loc(scopes sc, const tt::Expression* arg, const LabelDescription* lbl) {
  lam_t a = transl_exp_(sc, arg);
  long offset = 0;
  switch (lbl->lbl_repres.kind) {
    case RecordRepresentation::Kind::Record_regular:
    case RecordRepresentation::Kind::Record_inlined: offset = 0; break;
    case RecordRepresentation::Kind::Record_float:
      fatal_error("Translcore.transl_atomic_loc: atomic field in float record");
    case RecordRepresentation::Kind::Record_unboxed:
      fatal_error("Translcore.transl_atomic_loc: atomic field in unboxed record");
    case RecordRepresentation::Kind::Record_extension: offset = 1; break;
  }
  return {a, L::lconst(L::const_int(lbl->lbl_pos + offset))};
}

lam_t transl_match(scopes sc, const tt::Expression* e, const tt::Expression* arg, Slice<const tt::Case*> pat_expr_list,
                   tt::Partial partial) {
  struct StaticHandler {
    long lbl;
    std::vector<Param> ids_kinds;
    lam_t rhs;
  };
  std::vector<matching::PatAction> val_cases, exn_cases;
  std::vector<StaticHandler> static_handlers;
  for (const tt::Case* c : pat_expr_list) {  // List.fold_left rewrite_case
    if (is_unreachable(c->c_rhs)) continue;
    auto [val_pat, exn_pat] = tt::split_pattern(c->c_lhs);
    if (!val_pat && !exn_pat) fatal_error("Translcore.transl_match");
    if (val_pat && !exn_pat) {
      val_cases.push_back(transl_case(sc, nullptr, case_with_lhs(c, val_pat)));
    } else if (!val_pat && exn_pat) {
      exn_cases.push_back(transl_case_try(sc, case_with_lhs(c, exn_pat)));
    } else {
      if (c->c_guard) fatal_error("Translcore.transl_match: guard");
      long lbl = L::next_raise_count();
      // Simplif doesn't like it if binders are not uniq, so we make sure to
      // use different names in the value and the exception branches.
      std::vector<tt::BoundIdent> ids_full = tt::pat_bound_idents_full(val_pat);
      std::vector<Ident::t> ids;
      for (const tt::BoundIdent& b : ids_full) ids.push_back(b.id);
      std::vector<Param> ids_kinds;
      for (const tt::BoundIdent& b : ids_full) ids_kinds.push_back(Param{b.id, typeopt::value_kind(val_pat->pat_env, b.ty)});
      std::vector<Ident::t> vids;
      for (Ident::t id : ids) vids.push_back(ident::rename(id));
      std::vector<std::pair<Ident::t, Ident::t>> alpha;
      for (std::size_t i = 0; i < ids.size(); i++) alpha.emplace_back(ids[i], vids[i]);
      const tt::Pattern* pv = tt::alpha_pat(alpha, val_pat);
      // Also register the names of the exception so Re-raise happens.
      iter_exn_names(translprim::add_exception_ident, exn_pat);
      lam_t rhs;
      {
        struct Always {
          const tt::Pattern* p;
          ~Always() { iter_exn_names(translprim::remove_exception_ident, p); }
        } always{exn_pat};
        rhs = event_before(sc, c->c_rhs, transl_exp_(sc, c->c_rhs));
      }
      auto static_raise = [&](const std::vector<Ident::t>& is) {
        std::vector<lam_t> a;
        for (Ident::t id : is) a.push_back(L::lvar(id));
        return L::lstaticraise(lbl, slice(a));
      };
      val_cases.push_back({pv, static_raise(vids)});
      exn_cases.push_back({exn_pat, static_raise(ids)});
      static_handlers.push_back({lbl, ids_kinds, rhs});
    }
  }
  // staticcatch (try (exit <val-exit> <scrutinees>) with <exn-patterns> ->
  // <exn-actions>) with <val-exit> <val-ids> -> match <val-ids> with ...
  auto static_catch = [&](const std::vector<lam_t>& scrutinees, const std::vector<Param>& val_ids, lam_t handler) {
    std::vector<const tt::Pattern*> pats;
    for (const matching::PatAction& pa : exn_cases) pats.push_back(pa.pat);
    Ident::t id = typecore::name_pattern(OCAML_LIT("exn"), pats);
    long static_exception_id = L::next_raise_count();
    lam_t h = matching::for_trywith(sc, e->exp_loc, L::lvar(id), slice(exn_cases));
    return L::lstaticcatch(L::ltrywith(L::lstaticraise(static_exception_id, slice(scrutinees)), id, h),
                           static_exception_id, slice(val_ids), handler);
  };
  lam_t classic;
  if (auto* tup = tt::as<tt::Texp_tuple>(arg->exp_desc)) {
    std::vector<const tt::Expression*> argl;
    for (const tt::LabeledExpression& le : tup->el) argl.push_back(le.exp);
    if (exn_cases.empty()) {
      if (!static_handlers.empty()) fatal_error("Translcore.transl_match: static handlers");
      std::vector<lam_t> l = transl_list(sc, slice(argl));
      classic = matching::for_multiple_match(sc, e->exp_loc, slice(l), slice(val_cases), partial);
    } else {
      std::vector<Param> val_ids;
      for (const tt::Expression* a : argl) {
        ValueKind k = typeopt::value_kind(a->exp_env, a->exp_type);
        val_ids.push_back(Param{typecore::name_pattern(OCAML_LIT("val"), {}), k});
      }
      std::vector<lam_t> lvars;
      for (const Param& p : val_ids) lvars.push_back(L::lvar(p.id));
      lam_t handler = matching::for_multiple_match(sc, e->exp_loc, slice(lvars), slice(val_cases), partial);
      std::vector<lam_t> scrutinees = transl_list(sc, slice(argl));
      classic = static_catch(scrutinees, val_ids, handler);
    }
  } else if (exn_cases.empty()) {
    if (!static_handlers.empty()) fatal_error("Translcore.transl_match: static handlers");
    lam_t a = transl_exp_(sc, arg);
    classic = matching::for_function(sc, e->exp_loc, nullptr, a, slice(val_cases), partial);
  } else {
    std::vector<const tt::Pattern*> pats;
    for (const matching::PatAction& pa : val_cases) pats.push_back(pa.pat);
    Ident::t val_id = typecore::name_pattern(OCAML_LIT("val"), pats);
    ValueKind k = typeopt::value_kind(arg->exp_env, arg->exp_type);
    lam_t handler = matching::for_function(sc, e->exp_loc, nullptr, L::lvar(val_id), slice(val_cases), partial);
    std::vector<Param> val_ids{Param{val_id, k}};
    std::vector<lam_t> scrutinees{transl_exp_(sc, arg)};
    classic = static_catch(scrutinees, val_ids, handler);
  }
  for (const StaticHandler& h : static_handlers)
    classic = L::lstaticcatch(classic, h.lbl, slice(h.ids_kinds), h.rhs);
  return classic;
}

lam_t transl_handler(scopes sc, const tt::Expression* e, const tt::Expression* body, const ValCases* val_caselist,
                     Slice<const tt::Case*> exn_caselist, Slice<const tt::Case*> eff_caselist) {
  FunctionAttribute dattr = L::default_function_attribute();
  ScopedLocation unk = debuginfo::loc_unknown();
  lam_t val_fun;
  if (!val_caselist) {
    Ident::t param = Ident::create_local(OCAML_LIT("param"));
    val_fun = L::lfunction(L::FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}), ValueKind::gen(),
                           L::lvar(param), dattr, unk);
  } else {
    std::vector<matching::PatAction> val_cases = transl_cases(sc, nullptr, slice(val_caselist->cases));
    Ident::t param = typecore::name_cases(OCAML_LIT("param"), slice(val_caselist->cases));
    lam_t b = matching::for_function(sc, e->exp_loc, nullptr, L::lvar(param), slice(val_cases), val_caselist->partial);
    val_fun = L::lfunction(L::FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}), ValueKind::gen(), b,
                           dattr, unk);
  }
  lam_t exn_fun;
  {
    std::vector<matching::PatAction> exn_cases = transl_cases(sc, nullptr, exn_caselist);
    Ident::t param = typecore::name_cases(OCAML_LIT("exn"), exn_caselist);
    lam_t b = matching::for_trywith(sc, e->exp_loc, L::lvar(param), slice(exn_cases));
    exn_fun = L::lfunction(L::FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}), ValueKind::gen(), b,
                           dattr, unk);
  }
  lam_t eff_fun;
  {
    Ident::t param = typecore::name_cases(OCAML_LIT("eff"), eff_caselist);
    Ident::t cont = Ident::create_local(OCAML_LIT("k"));
    std::vector<matching::PatAction> eff_cases = transl_cases(sc, cont, eff_caselist);
    lam_t b = matching::for_handler(sc, e->exp_loc, L::lvar(param), L::lvar(cont), slice(eff_cases));
    eff_fun = L::lfunction(L::FunctionKind::Curried,
                           slice({Param{param, ValueKind::gen()}, Param{cont, ValueKind::gen()}}), ValueKind::gen(), b,
                           dattr, unk);
  }
  lam_t body_fun, arg;
  lam_t tb = transl_exp_(sc, body);
  auto* ap = as<L::Lapply>(tb);
  if (ap && ap->ap.ap_args.size() == 1 && L::is_evaluated(ap->ap.ap_func) && L::is_evaluated(ap->ap.ap_args[0])) {
    body_fun = ap->ap.ap_func;
    arg = ap->ap.ap_args[0];
  } else {
    Ident::t param = Ident::create_local(OCAML_LIT("param"));
    body_fun = L::lfunction(L::FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}), ValueKind::gen(), tb,
                            dattr, unk);
    arg = L::lconst(L::const_int(0));
  }
  lam_t alloc_stack = L::lprim(prim_alloc_stack(), slice({val_fun, exn_fun, eff_fun}), unk);
  return L::lprim(L::prim(PK::Prunstack), slice({alloc_stack, body_fun, arg}), of_location(sc, e->exp_loc));
}

lam_t transl_letop(scopes sc, const Location& loc, env::t env, const tt::BindingOp* let_,
                   Slice<const tt::BindingOp*> ands, Ident::t param, const tt::Case* c, tt::Partial partial) {
  std::function<lam_t(lam_t, std::size_t)> loop = [&](lam_t prev_lam, std::size_t i) -> lam_t {
    if (i >= ands.size()) return prev_lam;
    const tt::BindingOp* and_ = ands[i];
    Ident::t left_id = Ident::create_local(OCAML_LIT("left"));
    Ident::t right_id = Ident::create_local(OCAML_LIT("right"));
    lam_t op = transl_ident(of_location(sc, and_->bop_op_name.loc), env, and_->bop_op_type, and_->bop_op_path,
                            and_->bop_op_val);
    lam_t exp = transl_exp_(sc, and_->bop_exp);
    lam_t lam = L::bind(L::LetKind::Strict, right_id, exp,
                        mkapply(op, {L::lvar(left_id), L::lvar(right_id)}, of_location(sc, and_->bop_loc)));
    lam_t rest = loop(lam, i + 1);
    return L::bind(L::LetKind::Strict, left_id, prev_lam, rest);
  };
  lam_t op = transl_ident(of_location(sc, let_->bop_op_name.loc), env, let_->bop_op_type, let_->bop_op_path,
                          let_->bop_op_val);
  lam_t exp = loop(transl_exp_(sc, let_->bop_exp), 0);
  lam_t func;
  {
    FunInfo fi = event_function(sc, c->c_rhs, [&](L::IntRef* repr) {
      Location l = c->c_rhs->exp_loc;
      Location ghost_loc = l;
      ghost_loc.loc_ghost = true;
      auto* fb = make<tt::FunctionBody>();
      fb->kind = tt::FunctionBody::Kind::Tfunction_cases;
      fb->cases = slice({c});
      fb->param = param;
      fb->partial = partial;
      fb->loc = ghost_loc;
      fb->exp_extra = nullptr;
      return transl_function_without_attributes(sc, env, l, repr, {}, fb);
    });
    FunctionAttribute attr = function_attribute_disallowing_arity_fusion();
    ScopedLocation floc = of_location(sc, c->c_rhs->exp_loc);
    func = L::lfunction(fi.kind, slice(fi.params), fi.return_, fi.body, attr, floc);
  }
  return mkapply(op, {exp, func}, of_location(sc, loc));
}

}  // namespace

// ---- exported ---------------------------------------------------------------------------

lam_t transl_extension_constructor(scopes sc, env::t env, Path::t path, const tt::TExtensionConstructor* ext) {
  // Printtyp.wrap_printing_env env ~error:true: cmi loading disabled
  if (path) env::without_cmis([&] { path = rewrite_double_underscore_paths(env, path); });
  std::string name;
  if (!path)
    name = std::string(ident::name(ext->ext_id));
  else if (!clflags::for_package)
    name = path::name(path);
  else
    name = *clflags::for_package + "." + path::name(path);
  ScopedLocation loc = of_location(sc, ext->ext_loc);
  if (ext->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_decl) {
    lam_t oo = L::lprim(prim_fresh_oo_id(), slice({L::lconst(L::const_int(0))}), loc);
    return L::lprim(pmakeblock(object_tag, MutableFlag::Immutable, nullptr),
                    slice({L::lconst(const_immstring(name)), oo}), loc);
  }
  return L::transl_extension_path(loc, env, ext->ext_kind.path);
}

L::LetKind pure_module(const tt::ModuleExpr* m) {
  switch (m->mod_desc->kind) {
    case tt::ModuleExprDesc::Kind::Tmod_ident: return L::LetKind::Alias;
    case tt::ModuleExprDesc::Kind::Tmod_constraint:
      return pure_module(static_cast<const tt::Tmod_constraint*>(m->mod_desc)->me);
    default: return L::LetKind::Strict;
  }
}

lam_t transl_exp(scopes sc, const tt::Expression* e) { return transl_exp_(sc, e); }

// Like transl_exp, but used when a new scope was just introduced.
lam_t transl_scoped_exp(scopes sc, const tt::Expression* e) { return transl_exp1(sc, true, e); }

lam_t transl_apply(scopes sc, TailcallAttribute tailcall, InlineAttribute inlined, SpecialiseAttribute specialised,
                   lam_t lam, Slice<tt::LabeledArg> sargs, const ScopedLocation& loc) {
  auto lapply = [&](lam_t funct, const std::vector<lam_t>& args) -> lam_t {
    const L::Lsend* s = as<L::Lsend>(funct);
    if (!s)
      if (auto* ev = as<L::Levent>(funct)) s = as<L::Lsend>(ev->l);
    if (s) {
      std::vector<lam_t> a(s->args.begin(), s->args.end());
      a.insert(a.end(), args.begin(), args.end());
      return L::lsend(s->k, s->met, s->obj, slice(a), loc);
    }
    if (auto* ap = as<L::Lapply>(funct)) {
      LambdaApply ap2 = ap->ap;
      std::vector<lam_t> a(ap2.ap_args.begin(), ap2.ap_args.end());
      a.insert(a.end(), args.begin(), args.end());
      ap2.ap_args = slice(a);
      ap2.ap_loc = loc;
      return L::lapply(ap2);
    }
    return mkapply(funct, args, loc, tailcall, inlined, specialised);
  };
  // (arg, optional) with arg an Omitted () (nullptr) or Arg lambda
  struct TArg {
    lam_t arg;  // nullptr = Omitted ()
    bool optional;
  };
  auto rev_map_fst = [](const std::vector<TArg>& args) {
    std::vector<lam_t> r;
    for (std::size_t k = args.size(); k-- > 0;) r.push_back(args[k].arg);
    return r;
  };
  // Build a function application.  Particular care is required for
  // out-of-order partial applications (see translcore.ml).  `args` is
  // newest-first, as the OCaml list.
  std::function<lam_t(lam_t, std::vector<TArg>, std::vector<TArg>)> build_apply =
      [&](lam_t lam, std::vector<TArg> args, std::vector<TArg> l) -> lam_t {
    std::size_t i = 0;
    // (Arg arg, optional) :: l -> build_apply lam ((arg, optional) :: args) l
    while (i < l.size() && l[i].arg) {
      args.insert(args.begin(), l[i]);
      i++;
    }
    if (i >= l.size()) return lapply(lam, rev_map_fst(args));
    // (Omitted (), optional) :: l: out-of-order partial application; we
    // will need to build a closure
    bool optional = l[i].optional;
    std::vector<TArg> rest(l.begin() + static_cast<long>(i) + 1, l.end());
    std::vector<std::pair<Ident::t, lam_t>> defs;  // newest first
    auto protect = [&](std::string_view name, lam_t lam) -> lam_t {
      if (as<L::Lvar>(lam) || as<L::Lconst>(lam)) return lam;
      Ident::t id = Ident::create_local(name);
      defs.insert(defs.begin(), {id, lam});
      return L::lvar(id);
    };
    // If all arguments in [args] were optional, delay their application
    // until after this one is received
    std::vector<TArg> args1, args2;
    bool all_opt = true;
    for (const TArg& a : args)
      if (!a.optional) all_opt = false;
    if (all_opt)
      args2 = args;
    else
      args1 = args;
    lam_t lam2 = args1.empty() ? lam : lapply(lam, rev_map_fst(args1));
    // Evaluate the function, applied to the arguments in [args]
    lam_t handle = protect("func", lam2);
    // Evaluate the arguments whose applications was delayed
    for (TArg& a : args2) a.arg = protect("arg", a.arg);
    // Evaluate the remaining arguments
    for (TArg& a : rest)
      if (a.arg) a.arg = protect("arg", a.arg);
    Ident::t id_arg = Ident::create_local(OCAML_LIT("param"));
    // Process remaining arguments and build closure
    std::vector<TArg> nargs{TArg{L::lvar(id_arg), optional}};
    nargs.insert(nargs.end(), args2.begin(), args2.end());
    lam_t inner = build_apply(handle, nargs, rest);
    lam_t body;
    auto* lf = as<L::Lfunction>(inner);
    if (lf && lf->f->kind == L::FunctionKind::Curried && static_cast<long>(lf->f->params.size()) < L::max_arity()) {
      std::vector<Param> ps{Param{id_arg, ValueKind::gen()}};
      ps.insert(ps.end(), lf->f->params.begin(), lf->f->params.end());
      body = L::lfunction(L::FunctionKind::Curried, slice(ps), lf->f->return_, lf->f->body, lf->f->attr, lf->f->loc);
    } else {
      body = L::lfunction(L::FunctionKind::Curried, slice({Param{id_arg, ValueKind::gen()}}), ValueKind::gen(), inner,
                          L::default_stub_attribute(), loc);
    }
    // Wrap "protected" definitions, starting from the left, so that
    // evaluation is right-to-left.
    for (std::size_t k = defs.size(); k-- > 0;)
      body = L::llet(L::LetKind::Strict, ValueKind::gen(), defs[k].first, defs[k].second, body);
    return body;
  };
  std::vector<TArg> l;
  for (const tt::LabeledArg& a : sargs)  // List.map: left to right
    l.push_back(TArg{a.arg.omitted ? nullptr : transl_exp_(sc, a.arg.arg), btype::is_optional(a.label)});
  return build_apply(lam, {}, l);
}

/*
  Notice: transl_let consumes (ie compiles) its pat_expr_list argument, and
  returns a function that will take the body of the lambda-let construct.
  This complication allows choosing any compilation order for the bindings
  and body of let constructs.
*/
std::function<lam_t(lam_t)> transl_let(scopes sc, bool in_structure, RecFlag rec_flag,
                                       Slice<const tt::ValueBinding*> pat_expr_list) {
  if (rec_flag == RecFlag::Nonrecursive) {
    std::vector<lam_t> lams;
    for (const tt::ValueBinding* vb : pat_expr_list) {
      lam_t lam = transl_bound_exp(sc, in_structure, vb->vb_pat, vb->vb_expr);
      lams.push_back(translattribute::add_function_attributes(lam, vb->vb_loc, vb->vb_attributes));
    }
    // fun body -> Matching.for_let ~scopes pat.pat_loc lam pat (mk_body body):
    // the innermost binding first
    return [sc, lams, pat_expr_list](lam_t body) {
      for (std::size_t k = pat_expr_list.size(); k-- > 0;) {
        const tt::Pattern* pat = pat_expr_list[k]->vb_pat;
        body = matching::for_let(sc, pat->pat_loc, lams[k], pat, body);
      }
      return body;
    };
  }
  std::vector<Ident::t> idlist;
  for (const tt::ValueBinding* vb : pat_expr_list) {
    auto* v = tt::as<tt::Tpat_var>(vb->vb_pat->pat_desc);
    if (!v) fatal_error("Translcore.transl_let: recursive binding of a non-variable");
    idlist.push_back(v->id);
  }
  std::vector<value_rec_compiler::RecBinding> lam_bds;
  for (std::size_t i = 0; i < pat_expr_list.size(); i++) {  // List.map2
    const tt::ValueBinding* vb = pat_expr_list[i];
    lam_t def = transl_bound_exp(sc, in_structure, vb->vb_pat, vb->vb_expr);
    def = translattribute::add_function_attributes(def, vb->vb_loc, vb->vb_attributes);
    lam_bds.push_back(value_rec_compiler::RecBinding{idlist[i], vb->vb_rec_kind, def});
  }
  Slice<value_rec_compiler::RecBinding> bds = slice(lam_bds);
  return [bds](lam_t body) { return value_rec_compiler::compile_letrec(bds, body); };
}

lam_t transl_let(scopes sc, bool in_structure, RecFlag rec_flag, Slice<const tt::ValueBinding*> pat_expr_list,
                 lam_t body) {
  return transl_let(sc, in_structure, rec_flag, pat_expr_list)(body);
}

}  // namespace cppcaml::typing::translcore
