// Port of lambda/translobj.ml (TYPECHECKER.md stage 10).  See translobj.hpp.
//
// The consts table is a Hashtbl keyed by structural equality on
// structured constants; its only observable order is the Ident.Map fold of
// transl_label_init_general, so a vector with structural lookup is exact.
// Misc.protect_refs is a scope guard restoring the refs on exit (normal or
// exceptional), as protect_refs does.
#include "cppcaml/typing/translobj.hpp"

#include <map>
#include <stdexcept>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::translobj {

namespace L = ::cppcaml::typing::lambda;
using Lam = L::lambda;
using VK = L::ValueKind;
using namespace lambda;

Lam oo_prim(std::string_view name) { return transl_prim("CamlinternalOO", name); }

// ---- Share blocks ----------------------------------------------------------------
namespace {
struct ConstEntry {
  const StructuredConstant* c;
  Ident::t id;
};
std::vector<ConstEntry> consts;

const StructuredConstant* mk_const_int(long n) { return const_int(n); }
}  // namespace

Lam share(const StructuredConstant* c) {
  if (c->kind == StructuredConstant::Kind::Const_block && !c->fields.empty()) {
    for (const ConstEntry& e : consts)
      if (equal_structured_constant(e.c, c)) return lvar(e.id);
    Ident::t id = Ident::create_local(OCAML_LIT("shared"));
    consts.push_back({c, id});
    return lvar(id);
  }
  return lconst(c);
}

// ---- Collect labels --------------------------------------------------------------
namespace {
bool cache_required = false;
Lam method_cache_ = nullptr;  // lambda_unit (set lazily: a node needs the zone)
long method_count = 0;
struct TableEntry {
  Lam obj;
  std::vector<std::pair<Lam, Slice<Lam>>> r;  // (tag * lambda list) list ref
};
std::vector<TableEntry> method_table;  // most recent first, as the OCaml list

Lam method_cache() { return method_cache_ ? method_cache_ : lambda_unit(); }

Lam meth_tag(std::string_view s) { return lconst(mk_const_int(btype::hash_variant(s))); }

std::pair<Lam, Slice<Lam>> next_cache(Lam tag) {
  long n = method_count;
  ++method_count;
  return {tag, slice<Lam>({method_cache(), lconst(mk_const_int(n))})};
}

bool is_path(Lam l) {
  switch (l->kind) {
    case LK::Lvar:
    case LK::Lconst:
      return true;
    case LK::Lprim: {
      auto* p = as<Lprim>(l);
      if (p->p.kind == Primitive::K::Pgetglobal && p->args.empty()) return true;
      if (p->p.kind == Primitive::K::Pfield && p->args.size() == 1) return is_path(p->args[0]);
      if ((p->p.kind == Primitive::K::Parrayrefu || p->p.kind == Primitive::K::Parrayrefs) && p->args.size() == 2)
        return is_path(p->args[0]) && is_path(p->args[1]);
      return false;
    }
    default:
      return false;
  }
}
}  // namespace

std::pair<Lam, Slice<Lam>> meth(Lam obj, std::string_view lab) {
  Lam tag = meth_tag(lab);
  if (!(cache_required && clflags::native_code)) return {tag, {}};
  if (!is_path(obj)) return next_cache(tag);
  // List.assoc obj !method_table (structural)
  for (TableEntry& e : method_table) {
    if (!equal_lambda(e.obj, obj)) continue;
    for (auto& [t, v] : e.r)
      if (equal_lambda(t, tag)) return {tag, v};
    auto p = next_cache(tag);
    e.r.insert(e.r.begin(), p);
    return p;
  }
  auto p = next_cache(tag);
  method_table.insert(method_table.begin(), TableEntry{obj, {p}});
  return p;
}

void reset_labels() {
  consts.clear();
  method_count = 0;
  method_table.clear();
}

// ---- Insert labels ---------------------------------------------------------------
namespace {
Lam int_(long n) { return lconst(mk_const_int(n)); }

const PrimitiveDescription* prim_makearray() {
  // Primitive.simple ~name:"caml_array_make" ~arity:2 ~alloc:true
  static const PrimitiveDescription* p = [] {
    auto* d = make<PrimitiveDescription>();
    d->prim_name = zstr("caml_array_make");
    d->prim_arity = 2;
    d->prim_alloc = true;
    d->prim_native_name = zstr("");
    d->prim_native_repr_args = slice<NativeRepr>({NativeRepr{}, NativeRepr{}});
    d->prim_native_repr_res = NativeRepr{};
    return d;
  }();
  return p;
}

Lam transl_label_init_general(const std::function<Lam()>& f) {
  Lam expr = f();
  // Hashtbl.fold into an Ident.Map, then Ident.Map.fold (increasing ids,
  // each binding wrapping the previous: the largest id outermost)
  IdentMap<const StructuredConstant*> all_consts;
  for (const ConstEntry& e : consts) all_consts[e.id] = e.c;
  for (auto& [id, c] : all_consts) expr = llet(LetKind::Alias, VK::gen(), id, lconst(c), expr);
  reset_labels();
  return expr;
}

Lam transl_label_init_flambda(const std::function<Lam()>& f) {
  Ident::t method_cache_id = Ident::create_local(OCAML_LIT("method_cache"));
  method_cache_ = lvar(method_cache_id);
  Lam expr = f();
  if (method_count != 0) {
    Primitive pr = prim(Primitive::K::Pccall);
    pr.ccall = prim_makearray();
    Lam a1 = int_(0);
    Lam a0 = int_(method_count);
    expr = llet(LetKind::Strict, VK::gen(), method_cache_id,
                lprim(pr, slice<Lam>({a0, a1}), {}), expr);
  }
  return transl_label_init_general([&] { return expr; });
}
}  // namespace

std::pair<long, Lam> transl_store_label_init(Ident::t glob, long size,
                                                        const std::function<Lam()>& f) {
  if (!clflags::native_code) throw std::logic_error("Translobj.transl_store_label_init: bytecode");
  {
    Primitive pf = prim(Primitive::K::Pfield);
    pf.n = size;
    pf.ptr = ImmediateOrPointer::Pointer;
    pf.mut = MutableFlag::Mutable;
    Primitive pg = prim(Primitive::K::Pgetglobal);
    pg.id = glob;
    method_cache_ = lprim(pf, slice<Lam>({lprim(pg, {}, {})}), {});
  }
  Lam expr = f();
  if (method_count != 0) {
    Primitive ps = prim(Primitive::K::Psetfield);
    ps.n = size;
    ps.ptr = ImmediateOrPointer::Pointer;
    ps.init = InitializationOrAssignment::Root_initialization;
    Primitive pg = prim(Primitive::K::Pgetglobal);
    pg.id = glob;
    Primitive pc = prim(Primitive::K::Pccall);
    pc.ccall = prim_makearray();
    Lam a1 = int_(0);
    Lam a0 = int_(method_count);
    Lam mk = lprim(pc, slice<Lam>({a0, a1}), {});
    Lam gl = lprim(pg, {}, {});
    expr = lsequence(lprim(ps, slice<Lam>({gl, mk}), {}), expr);
    size = size + 1;
  }
  return {size, transl_label_init_general([&] { return expr; })};
}

Lam transl_label_init(const std::function<Lam()>& f) {
  if (clflags::native_code) return transl_label_init_flambda(f);
  return transl_label_init_general(f);
}

// ---- Share classes ---------------------------------------------------------------
namespace {
bool wrapping = false;
env::t top_env = nullptr;  // Env.empty (set lazily)
bool top_env_set = false;
std::vector<Ident::t> classes;  // most recent first, as the OCaml list

env::t get_top_env() { return top_env_set ? top_env : env::empty(); }

template <class T>
struct ProtectRef {  // one Misc.R (r, v) of Misc.protect_refs
  T& r;
  T saved;
  ProtectRef(T& ref, T v) : r(ref), saved(ref) { r = v; }
  ~ProtectRef() { r = saved; }
  ProtectRef(const ProtectRef&) = delete;
  ProtectRef& operator=(const ProtectRef&) = delete;
};
}  // namespace

IdentSet method_ids;

std::pair<env::t, bool> oo_add_class(Ident::t id) {
  classes.insert(classes.begin(), id);
  return {get_top_env(), cache_required};
}

std::pair<Lam, typedtree::RecursiveBindingKind> oo_wrap_gen(
    env::t env, bool req, const std::function<std::pair<Lam, typedtree::RecursiveBindingKind>()>& f) {
  if (wrapping) {
    if (cache_required) return f();
    ProtectRef<bool> g(cache_required, true);
    return f();
  }
  ProtectRef<bool> g1(wrapping, true);
  // Misc.R (top_env, env): top_env and whether it was set travel together
  ProtectRef<env::t> g2(top_env, env);
  ProtectRef<bool> g3(top_env_set, true);
  cache_required = req;
  classes.clear();
  method_ids.clear();
  auto [lam, other] = f();
  // List.fold_left over !classes (most recent first): the oldest class outermost
  for (Ident::t id : classes) {
    Primitive p = prim(Primitive::K::Pmakeblock);
    p.n = 0;
    p.mut = MutableFlag::Mutable;
    Lam u3 = lambda_unit(), u2 = lambda_unit(), u1 = lambda_unit();
    lam = llet(LetKind::StrictOpt, VK::gen(), id, lprim(p, slice<Lam>({u1, u2, u3}), {}), lam);
  }
  return {lam, other};
}

Lam oo_wrap(env::t env, bool req, const std::function<Lam()>& f) {
  return oo_wrap_gen(env, req, [&] { return std::pair{f(), typedtree::RecursiveBindingKind::Dynamic}; }).first;
}

void reset() {
  consts.clear();
  cache_required = false;
  method_cache_ = nullptr;
  method_count = 0;
  method_table.clear();
  wrapping = false;
  top_env = nullptr;
  top_env_set = false;
  classes.clear();
  method_ids.clear();
}

}  // namespace cppcaml::typing::translobj
