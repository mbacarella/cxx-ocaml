// Port of lambda/lambda.ml (and Debuginfo.Scoped_location).  See lambda.hpp.
//
// Where lambda.ml builds a constructor, tuple or record whose components have
// effects (Ident.rename in `subst ~freshen_bound_variables`, the key
// generator in `make_key`), the components are evaluated as OCaml does:
// right to left, a record's fields right to left in definition order.
#include "cppcaml/typing/lambda.hpp"

#include <algorithm>
#include <stdexcept>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/longident.hpp"

namespace cppcaml::typing {

// ---- Debuginfo.Scoped_location ----------------------------------------------
namespace debuginfo {

static std::string_view str_fun(scopes s) { return s ? s->str_fun : std::string_view("(fun)"); }
static scopes cons(Scopes::Item item, std::string str) {
  std::string_view sv = zborrow(str);
  std::string_view f = zborrow(str + ".(fun)");
  return make<Scopes>(Scopes{item, sv, f});
}
static std::string add_parens_if_symbolic(std::string_view s) {
  if (s.empty()) return "";
  char c = s[0];
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9')) return std::string(s);
  return "(" + std::string(s) + ")";
}
static std::string dot(scopes s, std::string_view x, std::string_view sep = ".") {
  std::string p = add_parens_if_symbolic(x);
  if (!s) return p;
  return std::string(s->str) + std::string(sep) + p;
}
scopes enter_anonymous_function(scopes s) {
  std::string_view str = str_fun(s);
  return make<Scopes>(Scopes{Scopes::Item::Sc_anonymous_function, str, str});
}
scopes enter_value_definition(scopes s, Ident::t id) {
  return cons(Scopes::Item::Sc_value_definition, dot(s, ident::name(id)));
}
scopes enter_module_definition(scopes s, Ident::t id) {
  return cons(Scopes::Item::Sc_module_definition, dot(s, ident::name(id)));
}
scopes enter_class_definition(scopes s, Ident::t id) {
  return cons(Scopes::Item::Sc_class_definition, dot(s, ident::name(id)));
}
scopes enter_method_definition(scopes s, std::string_view label) {
  std::string str = s && s->item == Scopes::Item::Sc_class_definition ? dot(s, label, "#") : dot(s, label);
  return cons(Scopes::Item::Sc_method_definition, str);
}
std::string string_of_scopes(scopes s) { return s ? std::string(s->str) : "<unknown>"; }

static bool is_none_loc(const Location& l) {
  // Location.is_none: l = Location.none (in_file "_none_")
  Location n = location::none();
  auto eq = [](const Position& a, const Position& b) {
    return a.pos_fname == b.pos_fname && a.pos_lnum == b.pos_lnum && a.pos_bol == b.pos_bol &&
           a.pos_cnum == b.pos_cnum;
  };
  return eq(l.loc_start, n.loc_start) && eq(l.loc_end, n.loc_end) && l.loc_ghost == n.loc_ghost;
}
ScopedLocation of_location(scopes s, const Location& loc) {
  if (is_none_loc(loc)) return {};
  return ScopedLocation{true, loc, s};
}
Location to_location(const ScopedLocation& l) { return l.known ? l.loc : location::none(); }
std::string string_of_scoped_location(const ScopedLocation& l) {
  return l.known ? string_of_scopes(l.sc) : "??";
}

}  // namespace debuginfo

namespace lambda {

using PK = Primitive::K;

bool equal_boxed_integer(BoxedInteger a, BoxedInteger b) { return a == b; }
bool equal_value_kind(const ValueKind& a, const ValueKind& b) {
  if (a.kind != b.kind) return false;
  return a.kind != ValueKind::Kind::Pboxedintval || a.bi == b.bi;
}

static bool equal_shape(const BlockShape& a, const BlockShape& b) {
  if (a.some != b.some) return false;
  if (!a.some) return true;
  if (a.kinds.size() != b.kinds.size()) return false;
  for (std::size_t k = 0; k < a.kinds.size(); ++k)
    if (!equal_value_kind(a.kinds[k], b.kinds[k])) return false;
  return true;
}
static bool equal_repr(const RecordRepresentation& a, const RecordRepresentation& b) {
  using RK = RecordRepresentation::Kind;
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case RK::Record_unboxed: return a.unboxed_inlined == b.unboxed_inlined;
    case RK::Record_inlined: return a.inlined_tag == b.inlined_tag;
    case RK::Record_extension: return path::same(a.extension, b.extension);
    default: return true;
  }
}
static bool equal_native_repr(const NativeRepr& a, const NativeRepr& b) {
  return a.kind == b.kind && (a.kind != NativeRepr::Kind::Unboxed_integer || a.bi == b.bi);
}
static bool equal_prim_desc(const PrimitiveDescription* a, const PrimitiveDescription* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->prim_name != b->prim_name || a->prim_arity != b->prim_arity || a->prim_alloc != b->prim_alloc ||
      a->prim_native_name != b->prim_native_name ||
      a->prim_native_repr_args.size() != b->prim_native_repr_args.size() ||
      !equal_native_repr(a->prim_native_repr_res, b->prim_native_repr_res))
    return false;
  for (std::size_t k = 0; k < a->prim_native_repr_args.size(); ++k)
    if (!equal_native_repr(a->prim_native_repr_args[k], b->prim_native_repr_args[k])) return false;
  return true;
}

// equal_primitive = (=): structural equality over every argument
bool equal_primitive(const Primitive& a, const Primitive& b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case PK::Pgetglobal:
    case PK::Psetglobal: return ident::same(a.id, b.id) && ident::name(a.id) == ident::name(b.id);
    case PK::Pmakeblock: return a.n == b.n && a.mut == b.mut && equal_shape(a.shape, b.shape);
    case PK::Pmakelazyblock: return a.lazy_tag == b.lazy_tag;
    case PK::Pfield: return a.n == b.n && a.ptr == b.ptr && a.mut == b.mut;
    case PK::Psetfield: return a.n == b.n && a.ptr == b.ptr && a.init == b.init;
    case PK::Psetfield_computed: return a.ptr == b.ptr && a.init == b.init;
    case PK::Pfloatfield: return a.n == b.n;
    case PK::Psetfloatfield: return a.n == b.n && a.init == b.init;
    case PK::Pduprecord: return equal_repr(a.repr, b.repr) && a.n == b.n;
    case PK::Pccall: return equal_prim_desc(a.ccall, b.ccall);
    case PK::Praise: return a.raise == b.raise;
    case PK::Pdivint:
    case PK::Pmodint: return a.safe == b.safe;
    case PK::Pintcomp: return a.icmp == b.icmp;
    case PK::Pphyscomp: return a.pcmp == b.pcmp;
    case PK::Pcompare_bints: return a.bi == b.bi;
    case PK::Poffsetint:
    case PK::Poffsetref: return a.n == b.n;
    case PK::Pfloatcomp: return a.fcmp == b.fcmp;
    case PK::Pmakearray:
    case PK::Pduparray: return a.array == b.array && a.mut == b.mut;
    case PK::Parraylength:
    case PK::Parrayrefu:
    case PK::Parraysetu:
    case PK::Parrayrefs:
    case PK::Parraysets: return a.array == b.array;
    case PK::Pbintofint:
    case PK::Pintofbint:
    case PK::Pnegbint:
    case PK::Paddbint:
    case PK::Psubbint:
    case PK::Pmulbint:
    case PK::Pandbint:
    case PK::Porbint:
    case PK::Pxorbint:
    case PK::Plslbint:
    case PK::Plsrbint:
    case PK::Pasrbint:
    case PK::Pbbswap: return a.bi == b.bi;
    case PK::Pcvtbint: return a.bi == b.bi && a.bi2 == b.bi2;
    case PK::Pdivbint:
    case PK::Pmodbint: return a.bi == b.bi && a.safe == b.safe;
    case PK::Pbintcomp: return a.bi == b.bi && a.icmp == b.icmp;
    case PK::Pbigarrayref:
    case PK::Pbigarrayset:
      return a.unsafe == b.unsafe && a.n == b.n && a.ba_kind == b.ba_kind && a.ba_layout == b.ba_layout;
    case PK::Pbigarraydim: return a.n == b.n;
    case PK::Pstring_load_16:
    case PK::Pstring_load_32:
    case PK::Pstring_load_64:
    case PK::Pbytes_load_16:
    case PK::Pbytes_load_32:
    case PK::Pbytes_load_64:
    case PK::Pbytes_set_16:
    case PK::Pbytes_set_32:
    case PK::Pbytes_set_64:
    case PK::Pbigstring_load_16:
    case PK::Pbigstring_load_32:
    case PK::Pbigstring_load_64:
    case PK::Pbigstring_set_16:
    case PK::Pbigstring_set_32:
    case PK::Pbigstring_set_64: return a.unsafe == b.unsafe;
    case PK::Pctconst: return a.ctconst == b.ctconst;
    default: return true;
  }
}

bool equal_inline_attribute(const InlineAttribute& a, const InlineAttribute& b) {
  if (a.kind != b.kind) return false;
  return a.kind != InlineAttribute::Kind::Unroll || a.unroll == b.unroll;
}
bool equal_specialise_attribute(SpecialiseAttribute a, SpecialiseAttribute b) { return a == b; }
bool equal_meth_kind(MethKind a, MethKind b) { return a == b; }

long tag_of_lazy_tag(LazyBlockTag t) { return t == LazyBlockTag::Lazy_tag ? 246 : 250; }

// ---- structured constants ----------------------------------------------------
const StructuredConstant* const_int(long n) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_int;
  c->i = n;
  return c;
}
const StructuredConstant* const_unit() {
  static const StructuredConstant* u = [] {
    ZoneScope perm(permanent_zone());
    return const_int(0);
  }();
  return u;
}

// ---- node constructors -----------------------------------------------------
template <class T>
static T* node() {
  T* n = make<T>();
  n->kind = T::K;
  return n;
}
lambda lvar(Ident::t id) {
  auto* n = node<Lvar>();
  n->id = id;
  return n;
}
lambda lmutvar(Ident::t id) {
  auto* n = node<Lmutvar>();
  n->id = id;
  return n;
}
lambda lconst(const StructuredConstant* c) {
  auto* n = node<Lconst>();
  n->c = c;
  return n;
}
lambda lapply(const LambdaApply& ap) {
  auto* n = node<Lapply>();
  n->ap = ap;
  return n;
}
lambda lfunction_node(const LFunction* f) {
  auto* n = node<Lfunction>();
  n->f = f;
  return n;
}
lambda llet(LetKind str, ValueKind k, Ident::t id, lambda arg, lambda body) {
  auto* n = node<Llet>();
  n->str = str;
  n->k = k;
  n->id = id;
  n->arg = arg;
  n->body = body;
  return n;
}
lambda lmutlet(ValueKind k, Ident::t id, lambda arg, lambda body) {
  auto* n = node<Lmutlet>();
  n->k = k;
  n->id = id;
  n->arg = arg;
  n->body = body;
  return n;
}
lambda lletrec(Slice<RecBinding> decl, lambda body) {
  auto* n = node<Lletrec>();
  n->decl = decl;
  n->body = body;
  return n;
}
lambda lprim(const Primitive& p, Slice<lambda> args, const ScopedLocation& loc) {
  auto* n = node<Lprim>();
  n->p = p;
  n->args = args;
  n->loc = loc;
  return n;
}
lambda lswitch(lambda arg, const LambdaSwitch& sw, const ScopedLocation& loc) {
  auto* n = node<Lswitch>();
  n->arg = arg;
  n->sw = sw;
  n->loc = loc;
  return n;
}
lambda lstringswitch(lambda arg, Slice<StringCase> cases, lambda def, const ScopedLocation& loc) {
  auto* n = node<Lstringswitch>();
  n->arg = arg;
  n->cases = cases;
  n->def = def;
  n->loc = loc;
  return n;
}
lambda lstaticraise(long i, Slice<lambda> args) {
  auto* n = node<Lstaticraise>();
  n->i = i;
  n->args = args;
  return n;
}
lambda lstaticcatch(lambda body, long i, Slice<Param> params, lambda handler) {
  auto* n = node<Lstaticcatch>();
  n->body = body;
  n->i = i;
  n->params = params;
  n->handler = handler;
  return n;
}
lambda ltrywith(lambda body, Ident::t exn, lambda handler) {
  auto* n = node<Ltrywith>();
  n->body = body;
  n->exn = exn;
  n->handler = handler;
  return n;
}
lambda lifthenelse(lambda c, lambda a, lambda b) {
  auto* n = node<Lifthenelse>();
  n->cond = c;
  n->ifso = a;
  n->ifnot = b;
  return n;
}
lambda lsequence(lambda a, lambda b) {
  auto* n = node<Lsequence>();
  n->l1 = a;
  n->l2 = b;
  return n;
}
lambda lwhile(lambda c, lambda b) {
  auto* n = node<Lwhile>();
  n->cond = c;
  n->body = b;
  return n;
}
lambda lfor(Ident::t id, lambda lo, lambda hi, parsetree::DirectionFlag dir, lambda body) {
  auto* n = node<Lfor>();
  n->id = id;
  n->lo = lo;
  n->hi = hi;
  n->dir = dir;
  n->body = body;
  return n;
}
lambda lassign(Ident::t id, lambda e) {
  auto* n = node<Lassign>();
  n->id = id;
  n->e = e;
  return n;
}
lambda lsend(MethKind k, lambda met, lambda obj, Slice<lambda> args, const ScopedLocation& loc) {
  auto* n = node<Lsend>();
  n->k = k;
  n->met = met;
  n->obj = obj;
  n->args = args;
  n->loc = loc;
  return n;
}
lambda levent(lambda l, const LambdaEvent* ev) {
  auto* n = node<Levent>();
  n->l = l;
  n->ev = ev;
  return n;
}
lambda lifused(Ident::t id, lambda l) {
  auto* n = node<Lifused>();
  n->id = id;
  n->l = l;
  return n;
}

lambda lambda_unit() { return lconst(const_unit()); }

lambda dummy_constant() { return lconst(const_int(0xBBBB / 2)); }

lambda lambda_of_const(const typedtree::Constant& c) {
  using CK = typedtree::Constant::Kind;
  using SK = StructuredConstant::Kind;
  auto* sc = make<StructuredConstant>();
  switch (c.kind) {
    case CK::Const_int: sc->kind = SK::Const_int; sc->i = c.i; break;
    case CK::Const_char: sc->kind = SK::Const_char; sc->i = c.i; break;
    case CK::Const_float: sc->kind = SK::Const_float; sc->s = c.s; break;
    case CK::Const_int32: sc->kind = SK::Const_int32; sc->boxed = c.boxed; break;
    case CK::Const_int64: sc->kind = SK::Const_int64; sc->boxed = c.boxed; break;
    case CK::Const_nativeint: sc->kind = SK::Const_nativeint; sc->boxed = c.boxed; break;
    case CK::Const_string: sc->kind = SK::Const_immstring; sc->s = c.s; break;
  }
  return lconst(sc);
}

long max_arity() { return clflags::native_code ? 126 : 0x3fffffffffffffffL; }

const LFunction* lfunction_(FunctionKind kind, Slice<Param> params, ValueKind return_, lambda body,
                            const FunctionAttribute& attr, const ScopedLocation& loc) {
  if (static_cast<long>(params.size()) > max_arity()) throw std::logic_error("Lambda.lfunction'");
  return make<LFunction>(LFunction{kind, params, return_, body, attr, loc});
}
lambda lfunction(FunctionKind kind, Slice<Param> params, ValueKind return_, lambda body,
                 const FunctionAttribute& attr, const ScopedLocation& loc) {
  return lfunction_node(lfunction_(kind, params, return_, body, attr, loc));
}

FunctionAttribute default_function_attribute() { return FunctionAttribute{}; }
FunctionAttribute default_stub_attribute() {
  FunctionAttribute a;
  a.stub = true;
  return a;
}

// ---- make_key ---------------------------------------------------------------
namespace {
struct NotSimple {};
// Ident.make_key_generator: Local {name = ""; stamp = 1, 0, -1, ...}
struct KeyGen {
  int c = 1;
  Ident::t operator()(Ident::t id) {
    if (id->kind != Ident::Kind::Local && id->kind != Ident::Kind::Scoped)
      throw std::logic_error("Ident.make_key_generator");
    int stamp = c--;
    return Ident::make_raw(Ident::Kind::Local, "", stamp, 0, nullptr);
  }
};
struct MakeKey {
  int count = 0;
  KeyGen make_key;
  static constexpr int max_raw = 32;
  using Env = IdentMap<lambda>;

  std::vector<lambda> recs(const Env& env, Slice<lambda> es) {
    std::vector<lambda> out;
    for (lambda e : es) out.push_back(rec(env, e));
    return out;
  }
  lambda opt(const Env& env, lambda e) { return e ? rec(env, e) : nullptr; }
  lambda rec(const Env& env, lambda e) {
    ++count;
    if (count > max_raw) throw NotSimple{};
    switch (e->kind) {
      case LK::Lvar:
      case LK::Lmutvar: {
        Ident::t id = e->kind == LK::Lvar ? as<Lvar>(e)->id : as<Lmutvar>(e)->id;
        for (auto& [k, v] : env)
          if (ident::same(k, id)) return v;
        return e;
      }
      case LK::Lconst: return e;
      case LK::Lapply: {
        auto* a = as<Lapply>(e);
        LambdaApply ap = a->ap;
        // {ap with ap_func = ..; ap_args = ..; ap_loc = Loc_unknown}: right to left
        ap.ap_loc = {};
        ap.ap_args = slice(recs(env, a->ap.ap_args));
        ap.ap_func = rec(env, a->ap.ap_func);
        return lapply(ap);
      }
      case LK::Llet: {
        auto* l = as<Llet>(e);
        if (l->str == LetKind::Alias) {  // ignore aliases -> substitute
          lambda ex = rec(env, l->arg);
          Env env2 = env;
          env2[l->id] = ex;
          return rec(env2, l->body);
        }
        if (auto* v = as<Lvar>(l->body); v && ident::same(v->id, l->id)) return rec(env, l->arg);
        lambda ex = rec(env, l->arg);
        Ident::t y = make_key(l->id);
        Env env2 = env;
        env2[l->id] = lvar(y);
        return llet(l->str, l->k, y, ex, rec(env2, l->body));
      }
      case LK::Lmutlet: {
        auto* l = as<Lmutlet>(e);
        lambda ex = rec(env, l->arg);
        Ident::t y = make_key(l->id);
        Env env2 = env;
        env2[l->id] = lmutvar(y);
        return lmutlet(l->k, y, ex, rec(env2, l->body));
      }
      case LK::Lprim: {
        auto* p = as<Lprim>(e);
        return lprim(p->p, slice(recs(env, p->args)), {});
      }
      case LK::Lswitch: {
        auto* s = as<Lswitch>(e);
        // Lswitch (tr_rec env e, tr_sw env sw, loc): tr_sw first; in it the
        // record fields right to left (failaction, blocks, consts)
        LambdaSwitch sw = s->sw;
        sw.sw_failaction = opt(env, s->sw.sw_failaction);
        std::vector<SwitchCase> blocks, consts;
        for (auto& c : s->sw.sw_blocks) blocks.push_back({c.key, rec(env, c.action)});
        for (auto& c : s->sw.sw_consts) consts.push_back({c.key, rec(env, c.action)});
        sw.sw_blocks = slice(blocks);
        sw.sw_consts = slice(consts);
        lambda arg = rec(env, s->arg);
        return lswitch(arg, sw, s->loc);
      }
      case LK::Lstringswitch: {
        auto* s = as<Lstringswitch>(e);
        lambda d = opt(env, s->def);
        std::vector<StringCase> cases;
        for (auto& c : s->cases) cases.push_back({c.s, rec(env, c.action)});
        lambda arg = rec(env, s->arg);
        return lstringswitch(arg, slice(cases), d, {});
      }
      case LK::Lstaticraise: {
        auto* r = as<Lstaticraise>(e);
        return lstaticraise(r->i, slice(recs(env, r->args)));
      }
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(e);
        lambda h = rec(env, c->handler);
        lambda b = rec(env, c->body);
        return lstaticcatch(b, c->i, c->params, h);
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(e);
        lambda h = rec(env, t->handler);
        lambda b = rec(env, t->body);
        return ltrywith(b, t->exn, h);
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(e);
        lambda c3 = rec(env, i->ifnot);
        lambda c2 = rec(env, i->ifso);
        lambda c1 = rec(env, i->cond);
        return lifthenelse(c1, c2, c3);
      }
      case LK::Lsequence: {
        auto* s = as<Lsequence>(e);
        lambda b = rec(env, s->l2);
        lambda a = rec(env, s->l1);
        return lsequence(a, b);
      }
      case LK::Lassign: {
        auto* a = as<Lassign>(e);
        return lassign(a->id, rec(env, a->e));
      }
      case LK::Lsend: {
        auto* s = as<Lsend>(e);
        std::vector<lambda> args = recs(env, s->args);
        lambda obj = rec(env, s->obj);
        lambda met = rec(env, s->met);
        return lsend(s->k, met, obj, slice(args), {});
      }
      case LK::Lifused: {
        auto* u = as<Lifused>(e);
        return lifused(u->id, rec(env, u->l));
      }
      case LK::Lletrec:
      case LK::Lfunction:
      case LK::Lfor:
      case LK::Lwhile:
      case LK::Levent: throw NotSimple{};
    }
    throw NotSimple{};
  }
};
}  // namespace

std::optional<lambda> make_key(lambda e) {
  MakeKey mk;
  try {
    return mk.rec({}, e);
  } catch (const NotSimple&) {
    return std::nullopt;
  }
}

// ---- naming ----------------------------------------------------------------
lambda name_lambda(LetKind str, lambda arg, const std::function<lambda(Ident::t)>& fn) {
  if (auto* v = as<Lvar>(arg)) return fn(v->id);
  Ident::t id = Ident::create_local("let");
  return llet(str, ValueKind::gen(), id, arg, fn(id));
}

lambda name_lambda_list(Slice<lambda> args, const std::function<lambda(Slice<lambda>)>& fn) {
  std::function<lambda(std::vector<lambda>, std::size_t)> name_list = [&](std::vector<lambda> names,
                                                                          std::size_t k) -> lambda {
    if (k == args.size()) return fn(slice(names));  // fn (List.rev names): names kept in order
    lambda arg = args[k];
    if (as<Lvar>(arg)) {
      names.push_back(arg);
      return name_list(names, k + 1);
    }
    Ident::t id = Ident::create_local("let");
    names.push_back(lvar(id));
    return llet(LetKind::Strict, ValueKind::gen(), id, arg, name_list(names, k + 1));
  };
  return name_list({}, 0);
}

// ---- iteration ---------------------------------------------------------------
void shallow_iter(const std::function<void(lambda)>& tail, const std::function<void(lambda)>& f, lambda l) {
  switch (l->kind) {
    case LK::Lvar:
    case LK::Lmutvar:
    case LK::Lconst: return;
    case LK::Lapply: {
      auto* a = as<Lapply>(l);
      f(a->ap.ap_func);
      for (lambda x : a->ap.ap_args) f(x);
      return;
    }
    case LK::Lfunction: f(as<Lfunction>(l)->f->body); return;
    case LK::Llet: {
      auto* x = as<Llet>(l);
      f(x->arg);
      tail(x->body);
      return;
    }
    case LK::Lmutlet: {
      auto* x = as<Lmutlet>(l);
      f(x->arg);
      tail(x->body);
      return;
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(l);
      tail(x->body);
      for (auto& rb : x->decl) f(lfunction_node(rb.def));
      return;
    }
    case LK::Lprim: {
      auto* p = as<Lprim>(l);
      if ((p->p.kind == PK::Psequand || p->p.kind == PK::Psequor) && p->args.size() == 2) {
        f(p->args[0]);
        tail(p->args[1]);
        return;
      }
      for (lambda x : p->args) f(x);
      return;
    }
    case LK::Lswitch: {
      auto* s = as<Lswitch>(l);
      f(s->arg);
      for (auto& c : s->sw.sw_consts) tail(c.action);
      for (auto& c : s->sw.sw_blocks) tail(c.action);
      if (s->sw.sw_failaction) tail(s->sw.sw_failaction);
      return;
    }
    case LK::Lstringswitch: {
      auto* s = as<Lstringswitch>(l);
      f(s->arg);
      for (auto& c : s->cases) tail(c.action);
      if (s->def) tail(s->def);
      return;
    }
    case LK::Lstaticraise:
      for (lambda x : as<Lstaticraise>(l)->args) f(x);
      return;
    case LK::Lstaticcatch: {
      auto* c = as<Lstaticcatch>(l);
      tail(c->body);
      tail(c->handler);
      return;
    }
    case LK::Ltrywith: {
      auto* t = as<Ltrywith>(l);
      f(t->body);
      tail(t->handler);
      return;
    }
    case LK::Lifthenelse: {
      auto* i = as<Lifthenelse>(l);
      f(i->cond);
      tail(i->ifso);
      tail(i->ifnot);
      return;
    }
    case LK::Lsequence: {
      auto* s = as<Lsequence>(l);
      f(s->l1);
      tail(s->l2);
      return;
    }
    case LK::Lwhile: {
      auto* w = as<Lwhile>(l);
      f(w->cond);
      f(w->body);
      return;
    }
    case LK::Lfor: {
      auto* x = as<Lfor>(l);
      f(x->lo);
      f(x->hi);
      f(x->body);
      return;
    }
    case LK::Lassign: f(as<Lassign>(l)->e); return;
    case LK::Lsend: {
      auto* s = as<Lsend>(l);
      f(s->met);
      f(s->obj);
      for (lambda x : s->args) f(x);
      return;
    }
    case LK::Levent: tail(as<Levent>(l)->l); return;
    case LK::Lifused: tail(as<Lifused>(l)->l); return;
  }
}

void iter_head_constructor(const std::function<void(lambda)>& f, lambda l) { shallow_iter(f, f, l); }

bool is_evaluated(lambda l) {
  return l->kind == LK::Lconst || l->kind == LK::Lvar || l->kind == LK::Lfunction;
}

// ---- free variables -----------------------------------------------------------
static void fv(lambda l, IdentSet& out);
static void fv_list(Slice<lambda> es, IdentSet& out) {
  for (lambda e : es) fv(e, out);
}
static IdentSet fv_of(lambda l) {
  IdentSet s;
  fv(l, s);
  return s;
}
static void fv(lambda l, IdentSet& out) {
  switch (l->kind) {
    case LK::Lvar: out.insert(as<Lvar>(l)->id); return;
    case LK::Lmutvar: out.insert(as<Lmutvar>(l)->id); return;
    case LK::Lconst: return;
    case LK::Lapply: {
      auto* a = as<Lapply>(l);
      fv(a->ap.ap_func, out);
      fv_list(a->ap.ap_args, out);
      return;
    }
    case LK::Lfunction: {
      auto* f = as<Lfunction>(l)->f;
      IdentSet b = fv_of(f->body);
      for (auto& p : f->params) b.erase(p.id);
      out.insert(b.begin(), b.end());
      return;
    }
    case LK::Llet:
    case LK::Lmutlet: {
      Ident::t id;
      lambda arg, body;
      if (auto* x = as<Llet>(l)) { id = x->id; arg = x->arg; body = x->body; }
      else { auto* y = as<Lmutlet>(l); id = y->id; arg = y->arg; body = y->body; }
      fv(arg, out);
      IdentSet b = fv_of(body);
      b.erase(id);
      out.insert(b.begin(), b.end());
      return;
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(l);
      IdentSet s = fv_of(x->body);
      for (auto& rb : x->decl) fv(lfunction_node(rb.def), s);
      for (auto& rb : x->decl) s.erase(rb.id);
      out.insert(s.begin(), s.end());
      return;
    }
    case LK::Lprim: fv_list(as<Lprim>(l)->args, out); return;
    case LK::Lswitch: {
      auto* s = as<Lswitch>(l);
      fv(s->arg, out);
      for (auto& c : s->sw.sw_consts) fv(c.action, out);
      for (auto& c : s->sw.sw_blocks) fv(c.action, out);
      if (s->sw.sw_failaction) fv(s->sw.sw_failaction, out);
      return;
    }
    case LK::Lstringswitch: {
      auto* s = as<Lstringswitch>(l);
      fv(s->arg, out);
      for (auto& c : s->cases) fv(c.action, out);
      if (s->def) fv(s->def, out);
      return;
    }
    case LK::Lstaticraise: fv_list(as<Lstaticraise>(l)->args, out); return;
    case LK::Lstaticcatch: {
      auto* c = as<Lstaticcatch>(l);
      IdentSet h = fv_of(c->handler);
      for (auto& p : c->params) h.erase(p.id);
      out.insert(h.begin(), h.end());
      fv(c->body, out);
      return;
    }
    case LK::Ltrywith: {
      auto* t = as<Ltrywith>(l);
      IdentSet h = fv_of(t->handler);
      h.erase(t->exn);
      out.insert(h.begin(), h.end());
      fv(t->body, out);
      return;
    }
    case LK::Lifthenelse: {
      auto* i = as<Lifthenelse>(l);
      fv(i->cond, out);
      fv(i->ifso, out);
      fv(i->ifnot, out);
      return;
    }
    case LK::Lsequence: {
      auto* s = as<Lsequence>(l);
      fv(s->l1, out);
      fv(s->l2, out);
      return;
    }
    case LK::Lwhile: {
      auto* w = as<Lwhile>(l);
      fv(w->cond, out);
      fv(w->body, out);
      return;
    }
    case LK::Lfor: {
      auto* x = as<Lfor>(l);
      fv(x->lo, out);
      fv(x->hi, out);
      IdentSet b = fv_of(x->body);
      b.erase(x->id);
      out.insert(b.begin(), b.end());
      return;
    }
    case LK::Lassign: {
      auto* a = as<Lassign>(l);
      out.insert(a->id);
      fv(a->e, out);
      return;
    }
    case LK::Lsend: {
      auto* s = as<Lsend>(l);
      fv(s->met, out);
      fv(s->obj, out);
      fv_list(s->args, out);
      return;
    }
    case LK::Levent: fv(as<Levent>(l)->l, out); return;
    case LK::Lifused: fv(as<Lifused>(l)->l, out); return;
  }
}
IdentSet free_variables(lambda l) { return fv_of(l); }

// ---- static failures --------------------------------------------------------------
static long g_raise_count = 0;
long next_raise_count() { return ++g_raise_count; }
void reset() { g_raise_count = 0; }

lambda staticfail() { return lstaticraise(0, {}); }

static bool is_staticfail0(lambda l) {
  auto* r = as<Lstaticraise>(l);
  return r && r->i == 0 && r->args.empty();
}
bool is_guarded(lambda l) {
  if (auto* i = as<Lifthenelse>(l)) return is_staticfail0(i->ifnot);
  if (auto* x = as<Llet>(l)) return is_guarded(x->body);
  if (auto* e = as<Levent>(l)) return is_guarded(e->l);
  return false;
}
lambda patch_guarded(lambda patch, lambda l) {
  if (auto* i = as<Lifthenelse>(l); i && is_staticfail0(i->ifnot)) return lifthenelse(i->cond, i->ifso, patch);
  if (auto* x = as<Llet>(l)) return llet(x->str, x->k, x->id, x->arg, patch_guarded(patch, x->body));
  if (auto* e = as<Levent>(l)) return levent(patch_guarded(patch, e->l), e->ev);
  throw std::logic_error("Lambda.patch_guarded");
}

// ---- access paths ---------------------------------------------------------------
static lambda transl_address(const ScopedLocation& loc, const env::Address* a) {
  if (a->is_ident) {
    if (ident::global(a->id)) {
      Primitive p = prim(PK::Pgetglobal);
      p.id = a->id;
      return lprim(p, {}, loc);
    }
    return lvar(a->id);
  }
  Primitive p = prim(PK::Pfield);
  p.n = a->pos;
  p.ptr = ImmediateOrPointer::Pointer;
  p.mut = MutableFlag::Immutable;
  return lprim(p, slice(std::vector<lambda>{transl_address(loc, a->parent)}), loc);
}
template <class Find>
static lambda transl_path(Find find, const ScopedLocation& loc, env::t env, Path::t path) {
  const env::Address* a;
  try {
    a = find(path, env);
  } catch (const env::NotFound&) {
    throw std::logic_error("Cannot find address for: " + path::name(path));
  }
  return transl_address(loc, a);
}
lambda transl_module_path(const ScopedLocation& loc, env::t env, Path::t path) {
  return transl_path(env::find_module_address, loc, env, path);
}
lambda transl_value_path(const ScopedLocation& loc, env::t env, Path::t path) {
  return transl_path(env::find_value_address, loc, env, path);
}
lambda transl_extension_path(const ScopedLocation& loc, env::t env, Path::t path) {
  return transl_path(env::find_constructor_address, loc, env, path);
}
lambda transl_class_path(const ScopedLocation& loc, env::t env, Path::t path) {
  return transl_path(env::find_class_address, loc, env, path);
}

lambda transl_prim(std::string_view modname, std::string_view field) {
  Ident::t mod_ident = Ident::create_persistent(modname);
  env::t e = env::add_persistent_structure(mod_ident, env::initial());
  env::OpenResult r = env::open_pers_signature(modname, e);
  if (r.kind != env::OpenResult::Kind::Ok)
    throw std::logic_error("Module " + std::string(modname) + " unavailable.");
  std::pair<Path::t, const ValueDescription*> found;
  try {
    found = env::find_value_by_name(Longident::lident(field), r.env);
  } catch (const env::NotFound&) {
    throw std::logic_error("Primitive " + std::string(modname) + "." + std::string(field) + " not found.");
  }
  return transl_value_path({}, r.env, found.first);
}

lambda make_atomic_loc(const ScopedLocation& loc, lambda arg, lambda field) {
  Primitive p = prim(PK::Pmakeblock);
  p.n = 0;
  p.mut = MutableFlag::Immutable;
  p.shape = BlockShape{true, slice(std::vector<ValueKind>{ValueKind::gen(), ValueKind::intval()})};
  return lprim(p, slice(std::vector<lambda>{arg, field}), loc);
}

// ---- substitution -------------------------------------------------------------------
namespace {
struct Substs {
  const UpdateEnv& update_env;
  bool freshen;
  const IdentMap<lambda>& s;
  using L = IdentMap<Ident::t>;

  Ident::t bind(Ident::t id, L& l) {
    Ident::t id2 = freshen ? ident::rename(id) : id;
    l[id] = id2;
    return id2;
  }
  // List.fold_right over the params: the last one is bound first
  Slice<Param> bind_many(Slice<Param> ids, L& l) {
    std::vector<Param> out(ids.size());
    for (std::size_t k = ids.size(); k-- > 0;) out[k] = Param{bind(ids[k].id, l), ids[k].kind};
    return slice(out);
  }
  static const Ident::t* find(const L& l, Ident::t id) {
    auto it = l.find(id);
    return it == l.end() ? nullptr : &it->second;
  }
  lambda subst(const L& l, lambda lam) {
    switch (lam->kind) {
      case LK::Lvar:
      case LK::Lmutvar: {
        bool mut = lam->kind == LK::Lmutvar;
        Ident::t id = mut ? as<Lmutvar>(lam)->id : as<Lvar>(lam)->id;
        if (const Ident::t* id2 = find(l, id)) return mut ? lmutvar(*id2) : lvar(*id2);
        auto it = s.find(id);
        return it == s.end() ? lam : it->second;
      }
      case LK::Lconst: return lam;
      case LK::Lapply: {
        auto* a = as<Lapply>(lam);
        LambdaApply ap = a->ap;
        ap.ap_args = list(l, a->ap.ap_args);  // record fields right to left: ap_args, ap_func
        ap.ap_func = subst(l, a->ap.ap_func);
        return lapply(ap);
      }
      case LK::Lfunction: return lfunction_node(subst_lfun(l, as<Lfunction>(lam)->f));
      case LK::Llet: {
        auto* x = as<Llet>(lam);
        L l2 = l;
        Ident::t id = bind(x->id, l2);
        lambda body = subst(l2, x->body);
        lambda arg = subst(l, x->arg);
        return llet(x->str, x->k, id, arg, body);
      }
      case LK::Lmutlet: {
        auto* x = as<Lmutlet>(lam);
        L l2 = l;
        Ident::t id = bind(x->id, l2);
        lambda body = subst(l2, x->body);
        lambda arg = subst(l, x->arg);
        return lmutlet(x->k, id, arg, body);
      }
      case LK::Lletrec: {
        auto* x = as<Lletrec>(lam);
        L l2 = l;
        std::vector<RecBinding> decl(x->decl.size());
        for (std::size_t k = x->decl.size(); k-- > 0;) decl[k] = RecBinding{bind(x->decl[k].id, l2), x->decl[k].def};
        lambda body = subst(l2, x->body);
        for (auto& rb : decl) rb.def = subst_lfun(l2, rb.def);
        return lletrec(slice(decl), body);
      }
      case LK::Lprim: {
        auto* p = as<Lprim>(lam);
        return lprim(p->p, list(l, p->args), p->loc);
      }
      case LK::Lswitch: {
        auto* x = as<Lswitch>(lam);
        LambdaSwitch sw = x->sw;
        sw.sw_failaction = x->sw.sw_failaction ? subst(l, x->sw.sw_failaction) : nullptr;
        sw.sw_blocks = cases(l, x->sw.sw_blocks);
        sw.sw_consts = cases(l, x->sw.sw_consts);
        lambda arg = subst(l, x->arg);
        return lswitch(arg, sw, x->loc);
      }
      case LK::Lstringswitch: {
        auto* x = as<Lstringswitch>(lam);
        lambda d = x->def ? subst(l, x->def) : nullptr;
        std::vector<StringCase> cs;
        for (auto& c : x->cases) cs.push_back({c.s, subst(l, c.action)});
        lambda arg = subst(l, x->arg);
        return lstringswitch(arg, slice(cs), d, x->loc);
      }
      case LK::Lstaticraise: {
        auto* r = as<Lstaticraise>(lam);
        return lstaticraise(r->i, list(l, r->args));
      }
      case LK::Lstaticcatch: {
        auto* c = as<Lstaticcatch>(lam);
        L l2 = l;
        Slice<Param> params = bind_many(c->params, l2);
        lambda handler = subst(l2, c->handler);
        lambda body = subst(l, c->body);
        return lstaticcatch(body, c->i, params, handler);
      }
      case LK::Ltrywith: {
        auto* t = as<Ltrywith>(lam);
        L l2 = l;
        Ident::t exn = bind(t->exn, l2);
        lambda handler = subst(l2, t->handler);
        lambda body = subst(l, t->body);
        return ltrywith(body, exn, handler);
      }
      case LK::Lifthenelse: {
        auto* i = as<Lifthenelse>(lam);
        lambda e3 = subst(l, i->ifnot);
        lambda e2 = subst(l, i->ifso);
        lambda e1 = subst(l, i->cond);
        return lifthenelse(e1, e2, e3);
      }
      case LK::Lsequence: {
        auto* x = as<Lsequence>(lam);
        lambda e2 = subst(l, x->l2);
        lambda e1 = subst(l, x->l1);
        return lsequence(e1, e2);
      }
      case LK::Lwhile: {
        auto* w = as<Lwhile>(lam);
        lambda e2 = subst(l, w->body);
        lambda e1 = subst(l, w->cond);
        return lwhile(e1, e2);
      }
      case LK::Lfor: {
        auto* x = as<Lfor>(lam);
        L l2 = l;
        Ident::t v = bind(x->id, l2);
        lambda body = subst(l2, x->body);
        lambda hi = subst(l, x->hi);
        lambda lo = subst(l, x->lo);
        return lfor(v, lo, hi, x->dir, body);
      }
      case LK::Lassign: {
        auto* a = as<Lassign>(lam);
        if (s.count(a->id)) throw std::logic_error("Lambda.subst: Lassign");
        const Ident::t* id2 = find(l, a->id);
        return lassign(id2 ? *id2 : a->id, subst(l, a->e));
      }
      case LK::Lsend: {
        auto* x = as<Lsend>(lam);
        Slice<lambda> args = list(l, x->args);
        lambda obj = subst(l, x->obj);
        lambda met = subst(l, x->met);
        return lsend(x->k, met, obj, args, x->loc);
      }
      case LK::Levent: {
        auto* e = as<Levent>(lam);
        env::t old_env = e->ev->lev_env;
        // Ident.Map.merge over (l, s) in key order, then folded: the updates
        // apply in Ident order
        std::vector<Ident::t> keys;
        for (auto& [k, _] : l) keys.push_back(k);
        for (auto& [k, _] : s)
          if (!l.count(k)) keys.push_back(k);
        std::sort(keys.begin(), keys.end(), IdentLess{});
        env::t new_env = old_env;
        for (Ident::t id : keys) {
          auto bound = l.find(id);
          const ValueDescription* vd = nullptr;
          try {
            vd = env::find_value(Path::pident(id), old_env);
          } catch (const env::NotFound&) {
            vd = nullptr;
          }
          if (bound != l.end()) {
            if (ident::equal(id, bound->second)) continue;
            if (vd) new_env = env::add_value(bound->second, vd, new_env);
          } else {
            if (vd) new_env = update_env(id, vd, new_env);
          }
        }
        auto* ev = make<LambdaEvent>(*e->ev);
        ev->lev_env = new_env;
        return levent(subst(l, e->l), ev);
      }
      case LK::Lifused: {
        auto* u = as<Lifused>(lam);
        const Ident::t* id2 = find(l, u->id);
        return lifused(id2 ? *id2 : u->id, subst(l, u->l));
      }
    }
    throw std::logic_error("Lambda.subst");
  }
  Slice<lambda> list(const L& l, Slice<lambda> li) {
    std::vector<lambda> out;
    for (lambda x : li) out.push_back(subst(l, x));
    return slice(out);
  }
  Slice<SwitchCase> cases(const L& l, Slice<SwitchCase> cs) {
    std::vector<SwitchCase> out;
    for (auto& c : cs) out.push_back({c.key, subst(l, c.action)});
    return slice(out);
  }
  const LFunction* subst_lfun(const L& l, const LFunction* lf) {
    L l2 = l;
    Slice<Param> params = bind_many(lf->params, l2);
    lambda body = subst(l2, lf->body);
    return make<LFunction>(LFunction{lf->kind, params, lf->return_, body, lf->attr, lf->loc});
  }
};
}  // namespace

lambda subst(const UpdateEnv& update_env, bool freshen_bound_variables, const IdentMap<lambda>& s, lambda lt) {
  Substs st{update_env, freshen_bound_variables, s};
  return st.subst({}, lt);
}

lambda rename(const IdentMap<Ident::t>& idmap, lambda lt) {
  UpdateEnv update_env = [&](Ident::t oldid, const ValueDescription* vd, env::t e) {
    return env::add_value(idmap.at(oldid), vd, e);
  };
  IdentMap<lambda> s;
  for (auto& [k, v] : idmap) s[k] = lvar(v);
  return subst(update_env, false, s, lt);
}

const LFunction* duplicate_function(const LFunction* f) {
  UpdateEnv id = [](Ident::t, const ValueDescription*, env::t e) { return e; };
  IdentMap<lambda> empty;
  Substs st{id, true, empty};
  return st.subst_lfun({}, f);
}

const LFunction* map_lfunction(const std::function<lambda(lambda)>& f, const LFunction* lf) {
  lambda body = f(lf->body);
  return make<LFunction>(LFunction{lf->kind, lf->params, lf->return_, body, lf->attr, lf->loc});
}

// shallow_map: the constructors' arguments are evaluated right to left
lambda shallow_map(const std::function<lambda(lambda)>& f, lambda lam) {
  auto map_list = [&](Slice<lambda> l) {
    std::vector<lambda> out;
    for (lambda x : l) out.push_back(f(x));
    return slice(out);
  };
  switch (lam->kind) {
    case LK::Lvar:
    case LK::Lmutvar:
    case LK::Lconst: return lam;
    case LK::Lapply: {
      auto* a = as<Lapply>(lam);
      LambdaApply ap = a->ap;
      // Lapply { ap_func = f ap_func; ap_args = List.map f ap_args; .. }
      ap.ap_args = map_list(a->ap.ap_args);
      ap.ap_func = f(a->ap.ap_func);
      return lapply(ap);
    }
    case LK::Lfunction: return lfunction_node(map_lfunction(f, as<Lfunction>(lam)->f));
    case LK::Llet: {
      auto* x = as<Llet>(lam);
      lambda e2 = f(x->body);
      lambda e1 = f(x->arg);
      return llet(x->str, x->k, x->id, e1, e2);
    }
    case LK::Lmutlet: {
      auto* x = as<Lmutlet>(lam);
      lambda e2 = f(x->body);
      lambda e1 = f(x->arg);
      return lmutlet(x->k, x->id, e1, e2);
    }
    case LK::Lletrec: {
      auto* x = as<Lletrec>(lam);
      lambda e2 = f(x->body);
      std::vector<RecBinding> decl;
      for (auto& rb : x->decl) decl.push_back({rb.id, map_lfunction(f, rb.def)});
      return lletrec(slice(decl), e2);
    }
    case LK::Lprim: {
      auto* p = as<Lprim>(lam);
      return lprim(p->p, map_list(p->args), p->loc);
    }
    case LK::Lswitch: {
      auto* x = as<Lswitch>(lam);
      // { sw_numconsts; sw_consts; sw_numblocks; sw_blocks; sw_failaction }:
      // right to left
      LambdaSwitch sw = x->sw;
      sw.sw_failaction = x->sw.sw_failaction ? f(x->sw.sw_failaction) : nullptr;
      std::vector<SwitchCase> blocks, consts;
      for (auto& c : x->sw.sw_blocks) blocks.push_back({c.key, f(c.action)});
      for (auto& c : x->sw.sw_consts) consts.push_back({c.key, f(c.action)});
      sw.sw_blocks = slice(blocks);
      sw.sw_consts = slice(consts);
      lambda e = f(x->arg);
      return lswitch(e, sw, x->loc);
    }
    case LK::Lstringswitch: {
      auto* x = as<Lstringswitch>(lam);
      lambda d = x->def ? f(x->def) : nullptr;
      std::vector<StringCase> cs;
      for (auto& c : x->cases) cs.push_back({c.s, f(c.action)});
      lambda e = f(x->arg);
      return lstringswitch(e, slice(cs), d, x->loc);
    }
    case LK::Lstaticraise: {
      auto* r = as<Lstaticraise>(lam);
      return lstaticraise(r->i, map_list(r->args));
    }
    case LK::Lstaticcatch: {
      auto* c = as<Lstaticcatch>(lam);
      lambda h = f(c->handler);
      lambda b = f(c->body);
      return lstaticcatch(b, c->i, c->params, h);
    }
    case LK::Ltrywith: {
      auto* t = as<Ltrywith>(lam);
      lambda e2 = f(t->handler);
      lambda e1 = f(t->body);
      return ltrywith(e1, t->exn, e2);
    }
    case LK::Lifthenelse: {
      auto* i = as<Lifthenelse>(lam);
      lambda e3 = f(i->ifnot);
      lambda e2 = f(i->ifso);
      lambda e1 = f(i->cond);
      return lifthenelse(e1, e2, e3);
    }
    case LK::Lsequence: {
      auto* s = as<Lsequence>(lam);
      lambda e2 = f(s->l2);
      lambda e1 = f(s->l1);
      return lsequence(e1, e2);
    }
    case LK::Lwhile: {
      auto* w = as<Lwhile>(lam);
      lambda e2 = f(w->body);
      lambda e1 = f(w->cond);
      return lwhile(e1, e2);
    }
    case LK::Lfor: {
      auto* x = as<Lfor>(lam);
      lambda e3 = f(x->body);
      lambda e2 = f(x->hi);
      lambda e1 = f(x->lo);
      return lfor(x->id, e1, e2, x->dir, e3);
    }
    case LK::Lassign: {
      auto* a = as<Lassign>(lam);
      return lassign(a->id, f(a->e));
    }
    case LK::Lsend: {
      auto* s = as<Lsend>(lam);
      Slice<lambda> el = map_list(s->args);
      lambda o = f(s->obj);
      lambda m = f(s->met);
      return lsend(s->k, m, o, el, s->loc);
    }
    case LK::Levent: {
      auto* e = as<Levent>(lam);
      return levent(f(e->l), e->ev);
    }
    case LK::Lifused: {
      auto* u = as<Lifused>(lam);
      return lifused(u->id, f(u->l));
    }
  }
  throw std::logic_error("Lambda.shallow_map");
}

lambda map(const std::function<lambda(lambda)>& f, lambda l) {
  std::function<lambda(lambda)> g = [&](lambda lam) { return f(shallow_map(g, lam)); };
  return g(l);
}

// ---- binding ------------------------------------------------------------------
lambda bind_with_value_kind(LetKind str, Ident::t var, ValueKind k, lambda exp, lambda body) {
  if (auto* v = as<Lvar>(exp); v && ident::same(var, v->id)) return body;
  return llet(str, k, var, exp, body);
}
lambda bind(LetKind str, Ident::t var, lambda exp, lambda body) {
  return bind_with_value_kind(str, var, ValueKind::gen(), exp, body);
}

// ---- comparisons -------------------------------------------------------------------
PhysicalComparison negate_physical_comparison(PhysicalComparison c) {
  return c == PhysicalComparison::CPeq ? PhysicalComparison::CPneq : PhysicalComparison::CPeq;
}
IntegerComparison negate_integer_comparison(IntegerComparison c) {
  using C = IntegerComparison;
  switch (c) {
    case C::Ceq: return C::Cne;
    case C::Cne: return C::Ceq;
    case C::Clt: return C::Cge;
    case C::Cle: return C::Cgt;
    case C::Cgt: return C::Cle;
    case C::Cge: return C::Clt;
  }
  return c;
}
IntegerComparison swap_integer_comparison(IntegerComparison c) {
  using C = IntegerComparison;
  switch (c) {
    case C::Ceq: return C::Ceq;
    case C::Cne: return C::Cne;
    case C::Clt: return C::Cgt;
    case C::Cle: return C::Cge;
    case C::Cgt: return C::Clt;
    case C::Cge: return C::Cle;
  }
  return c;
}
FloatComparison negate_float_comparison(FloatComparison c) {
  using C = FloatComparison;
  switch (c) {
    case C::CFeq: return C::CFneq;
    case C::CFneq: return C::CFeq;
    case C::CFlt: return C::CFnlt;
    case C::CFnlt: return C::CFlt;
    case C::CFgt: return C::CFngt;
    case C::CFngt: return C::CFgt;
    case C::CFle: return C::CFnle;
    case C::CFnle: return C::CFle;
    case C::CFge: return C::CFnge;
    case C::CFnge: return C::CFge;
  }
  return c;
}
FloatComparison swap_float_comparison(FloatComparison c) {
  using C = FloatComparison;
  switch (c) {
    case C::CFeq: return C::CFeq;
    case C::CFneq: return C::CFneq;
    case C::CFlt: return C::CFgt;
    case C::CFnlt: return C::CFngt;
    case C::CFle: return C::CFge;
    case C::CFnle: return C::CFnge;
    case C::CFgt: return C::CFlt;
    case C::CFngt: return C::CFnlt;
    case C::CFge: return C::CFle;
    case C::CFnge: return C::CFnle;
  }
  return c;
}

std::string_view raise_kind(RaiseKind k) {
  switch (k) {
    case RaiseKind::Raise_regular: return "raise";
    case RaiseKind::Raise_reraise: return "reraise";
    case RaiseKind::Raise_notrace: return "raise_notrace";
  }
  return "raise";
}

std::optional<InlineAttribute> merge_inline_attributes(const InlineAttribute& a, const InlineAttribute& b) {
  if (a.kind == InlineAttribute::Kind::Default_inline) return b;
  if (b.kind == InlineAttribute::Kind::Default_inline) return a;
  if (equal_inline_attribute(a, b)) return a;
  return std::nullopt;
}

bool function_is_curried(const LFunction* f) { return f->kind == FunctionKind::Curried; }

std::optional<Slice<lambda>> find_exact_application(FunctionKind kind, long arity, Slice<lambda> args) {
  if (kind == FunctionKind::Curried) {
    if (arity != static_cast<long>(args.size())) return std::nullopt;
    return args;
  }
  if (args.size() == 1) {
    if (auto* p = as<Lprim>(args[0]); p && p->p.kind == PK::Pmakeblock) {
      if (arity != static_cast<long>(p->args.size())) return std::nullopt;
      return p->args;
    }
    if (auto* c = as<Lconst>(args[0]); c && c->c->kind == StructuredConstant::Kind::Const_block) {
      if (arity != static_cast<long>(c->c->fields.size())) return std::nullopt;
      std::vector<lambda> out;
      for (auto* f : c->c->fields) out.push_back(lconst(f));
      return slice(out);
    }
  }
  return std::nullopt;
}

// ---- structural equality ---------------------------------------------------------
bool equal_structured_constant(const StructuredConstant* a, const StructuredConstant* b) {
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  using SK = StructuredConstant::Kind;
  switch (a->kind) {
    case SK::Const_int:
    case SK::Const_char: return a->i == b->i;
    case SK::Const_float:
    case SK::Const_immstring: return a->s == b->s;
    case SK::Const_int32:
    case SK::Const_int64:
    case SK::Const_nativeint: return a->boxed == b->boxed;
    case SK::Const_block:
      if (a->i != b->i || a->fields.size() != b->fields.size()) return false;
      for (std::size_t k = 0; k < a->fields.size(); ++k)
        if (!equal_structured_constant(a->fields[k], b->fields[k])) return false;
      return true;
    case SK::Const_float_array:
      if (a->floats.size() != b->floats.size()) return false;
      for (std::size_t k = 0; k < a->floats.size(); ++k)
        if (a->floats[k] != b->floats[k]) return false;
      return true;
  }
  return false;
}

static bool equal_ident(Ident::t a, Ident::t b) {
  // structural (=) on Ident.t
  if (a == b) return true;
  if (a->kind != b->kind) return false;
  return ident::name(a) == ident::name(b) && a->stamp_ == b->stamp_ && a->scope_ == b->scope_;
}
static bool equal_pos(const Position& a, const Position& b) {
  return a.pos_fname == b.pos_fname && a.pos_lnum == b.pos_lnum && a.pos_bol == b.pos_bol && a.pos_cnum == b.pos_cnum;
}
static bool equal_sloc(const ScopedLocation& a, const ScopedLocation& b) {
  if (a.known != b.known) return false;
  if (!a.known) return true;
  if (!(equal_pos(a.loc.loc_start, b.loc.loc_start) && equal_pos(a.loc.loc_end, b.loc.loc_end) &&
        a.loc.loc_ghost == b.loc.loc_ghost))
    return false;
  if (a.sc == b.sc) return true;
  if (!a.sc || !b.sc) return false;
  return a.sc->item == b.sc->item && a.sc->str == b.sc->str && a.sc->str_fun == b.sc->str_fun;
}
static bool equal_list(Slice<lambda> a, Slice<lambda> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (!equal_lambda(a[k], b[k])) return false;
  return true;
}
static bool equal_opt(lambda a, lambda b) {
  if (!a || !b) return a == b;
  return equal_lambda(a, b);
}
static bool equal_cases(Slice<SwitchCase> a, Slice<SwitchCase> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (a[k].key != b[k].key || !equal_lambda(a[k].action, b[k].action)) return false;
  return true;
}
static bool equal_params(Slice<Param> a, Slice<Param> b) {
  if (a.size() != b.size()) return false;
  for (std::size_t k = 0; k < a.size(); ++k)
    if (!equal_ident(a[k].id, b[k].id) || !equal_value_kind(a[k].kind, b[k].kind)) return false;
  return true;
}
static bool equal_attr(const FunctionAttribute& a, const FunctionAttribute& b) {
  return equal_inline_attribute(a.inline_, b.inline_) && a.specialise == b.specialise && a.local == b.local &&
         a.poll == b.poll && a.is_a_functor == b.is_a_functor && a.stub == b.stub &&
         a.tmc_candidate == b.tmc_candidate && a.may_fuse_arity == b.may_fuse_arity;
}
static bool equal_lfun(const LFunction* a, const LFunction* b) {
  return a->kind == b->kind && equal_params(a->params, b->params) && equal_value_kind(a->return_, b->return_) &&
         equal_lambda(a->body, b->body) && equal_attr(a->attr, b->attr) && equal_sloc(a->loc, b->loc);
}
bool equal_lambda(lambda a, lambda b) {
  if (a == b) return true;
  if (!a || !b || a->kind != b->kind) return false;
  switch (a->kind) {
    case LK::Lvar: return equal_ident(as<Lvar>(a)->id, as<Lvar>(b)->id);
    case LK::Lmutvar: return equal_ident(as<Lmutvar>(a)->id, as<Lmutvar>(b)->id);
    case LK::Lconst: return equal_structured_constant(as<Lconst>(a)->c, as<Lconst>(b)->c);
    case LK::Lapply: {
      auto &x = as<Lapply>(a)->ap, &y = as<Lapply>(b)->ap;
      return equal_lambda(x.ap_func, y.ap_func) && equal_list(x.ap_args, y.ap_args) &&
             equal_sloc(x.ap_loc, y.ap_loc) && x.ap_tailcall == y.ap_tailcall &&
             equal_inline_attribute(x.ap_inlined, y.ap_inlined) && x.ap_specialised == y.ap_specialised;
    }
    case LK::Lfunction: return equal_lfun(as<Lfunction>(a)->f, as<Lfunction>(b)->f);
    case LK::Llet: {
      auto *x = as<Llet>(a), *y = as<Llet>(b);
      return x->str == y->str && equal_value_kind(x->k, y->k) && equal_ident(x->id, y->id) &&
             equal_lambda(x->arg, y->arg) && equal_lambda(x->body, y->body);
    }
    case LK::Lmutlet: {
      auto *x = as<Lmutlet>(a), *y = as<Lmutlet>(b);
      return equal_value_kind(x->k, y->k) && equal_ident(x->id, y->id) && equal_lambda(x->arg, y->arg) &&
             equal_lambda(x->body, y->body);
    }
    case LK::Lletrec: {
      auto *x = as<Lletrec>(a), *y = as<Lletrec>(b);
      if (x->decl.size() != y->decl.size()) return false;
      for (std::size_t k = 0; k < x->decl.size(); ++k)
        if (!equal_ident(x->decl[k].id, y->decl[k].id) || !equal_lfun(x->decl[k].def, y->decl[k].def)) return false;
      return equal_lambda(x->body, y->body);
    }
    case LK::Lprim: {
      auto *x = as<Lprim>(a), *y = as<Lprim>(b);
      return equal_primitive(x->p, y->p) && equal_list(x->args, y->args) && equal_sloc(x->loc, y->loc);
    }
    case LK::Lswitch: {
      auto *x = as<Lswitch>(a), *y = as<Lswitch>(b);
      return equal_lambda(x->arg, y->arg) && x->sw.sw_numconsts == y->sw.sw_numconsts &&
             equal_cases(x->sw.sw_consts, y->sw.sw_consts) && x->sw.sw_numblocks == y->sw.sw_numblocks &&
             equal_cases(x->sw.sw_blocks, y->sw.sw_blocks) && equal_opt(x->sw.sw_failaction, y->sw.sw_failaction) &&
             equal_sloc(x->loc, y->loc);
    }
    case LK::Lstringswitch: {
      auto *x = as<Lstringswitch>(a), *y = as<Lstringswitch>(b);
      if (x->cases.size() != y->cases.size()) return false;
      for (std::size_t k = 0; k < x->cases.size(); ++k)
        if (x->cases[k].s != y->cases[k].s || !equal_lambda(x->cases[k].action, y->cases[k].action)) return false;
      return equal_lambda(x->arg, y->arg) && equal_opt(x->def, y->def) && equal_sloc(x->loc, y->loc);
    }
    case LK::Lstaticraise: {
      auto *x = as<Lstaticraise>(a), *y = as<Lstaticraise>(b);
      return x->i == y->i && equal_list(x->args, y->args);
    }
    case LK::Lstaticcatch: {
      auto *x = as<Lstaticcatch>(a), *y = as<Lstaticcatch>(b);
      return equal_lambda(x->body, y->body) && x->i == y->i && equal_params(x->params, y->params) &&
             equal_lambda(x->handler, y->handler);
    }
    case LK::Ltrywith: {
      auto *x = as<Ltrywith>(a), *y = as<Ltrywith>(b);
      return equal_lambda(x->body, y->body) && equal_ident(x->exn, y->exn) && equal_lambda(x->handler, y->handler);
    }
    case LK::Lifthenelse: {
      auto *x = as<Lifthenelse>(a), *y = as<Lifthenelse>(b);
      return equal_lambda(x->cond, y->cond) && equal_lambda(x->ifso, y->ifso) && equal_lambda(x->ifnot, y->ifnot);
    }
    case LK::Lsequence: {
      auto *x = as<Lsequence>(a), *y = as<Lsequence>(b);
      return equal_lambda(x->l1, y->l1) && equal_lambda(x->l2, y->l2);
    }
    case LK::Lwhile: {
      auto *x = as<Lwhile>(a), *y = as<Lwhile>(b);
      return equal_lambda(x->cond, y->cond) && equal_lambda(x->body, y->body);
    }
    case LK::Lfor: {
      auto *x = as<Lfor>(a), *y = as<Lfor>(b);
      return equal_ident(x->id, y->id) && equal_lambda(x->lo, y->lo) && equal_lambda(x->hi, y->hi) &&
             x->dir == y->dir && equal_lambda(x->body, y->body);
    }
    case LK::Lassign: {
      auto *x = as<Lassign>(a), *y = as<Lassign>(b);
      return equal_ident(x->id, y->id) && equal_lambda(x->e, y->e);
    }
    case LK::Lsend: {
      auto *x = as<Lsend>(a), *y = as<Lsend>(b);
      return x->k == y->k && equal_lambda(x->met, y->met) && equal_lambda(x->obj, y->obj) &&
             equal_list(x->args, y->args) && equal_sloc(x->loc, y->loc);
    }
    case LK::Levent: {
      auto *x = as<Levent>(a), *y = as<Levent>(b);
      return equal_lambda(x->l, y->l) && x->ev == y->ev;
    }
    case LK::Lifused: {
      auto *x = as<Lifused>(a), *y = as<Lifused>(b);
      return equal_ident(x->id, y->id) && equal_lambda(x->l, y->l);
    }
  }
  return false;
}

}  // namespace lambda
}  // namespace cppcaml::typing
