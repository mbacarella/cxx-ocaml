// Port of lambda/value_rec_compiler.ml (TYPECHECKER.md stage 10): the
// compilation of generic recursive definitions -- sizing, function lifting,
// then pre-allocation / functions / backpatching.  Misc.fatal_error is a
// std::logic_error.

#include "cppcaml/typing/value_rec_compiler.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cppcaml::typing::value_rec_compiler {

using namespace lambda;
namespace L = cppcaml::typing::lambda;
using lam_t = cppcaml::typing::lambda::lambda;
using ValueKind = cppcaml::typing::lambda::ValueKind;
using RecBinding = cppcaml::typing::value_rec_compiler::RecBinding;
using PK = Primitive::K;

namespace {

[[noreturn]] void fatal_error(const char* s) { throw std::logic_error(s); }

// Primitive.simple ~name ~arity ~alloc
const PrimitiveDescription* simple(std::string_view name, long arity, bool alloc) {
  ZoneScope perm(permanent_zone());
  std::vector<NativeRepr> reprs(static_cast<std::size_t>(arity));
  return make<PrimitiveDescription>(
      PrimitiveDescription{zstr(name), arity, alloc, zstr(""), slice(reprs), NativeRepr{}});
}

// Allocation and backpatching primitives
const PrimitiveDescription* alloc_prim() {
  static const PrimitiveDescription* d = simple("caml_alloc_dummy", 1, true);
  return d;
}
const PrimitiveDescription* alloc_float_record_prim() {
  static const PrimitiveDescription* d = simple("caml_alloc_dummy_float", 1, true);
  return d;
}
const PrimitiveDescription* alloc_lazy_prim() {
  static const PrimitiveDescription* d = simple("caml_alloc_dummy_lazy", 1, true);
  return d;
}
const PrimitiveDescription* update_prim() {
  static const PrimitiveDescription* d = simple("caml_update_dummy", 2, true);
  return d;
}
const PrimitiveDescription* update_lazy_prim() {
  static const PrimitiveDescription* d = simple("caml_update_dummy_lazy", 2, true);
  return d;
}

Primitive pccall(const PrimitiveDescription* d) {
  Primitive p = prim(PK::Pccall);
  p.ccall = d;
  return p;
}

// ---- 1. Sizing ---------------------------------------------------------------

struct BlockSize {  // Regular_block of int | Float_record of int | Lazy_block
  enum class Kind { Regular_block, Float_record, Lazy_block };
  Kind kind;
  long n = 0;
};

struct Size {  // Unreachable | Constant | Function | Block of block_size
  enum class Kind { Unreachable, Constant, Function, Block };
  Kind kind;
  BlockSize block{BlockSize::Kind::Regular_block};
};
Size sz(Size::Kind k) { return Size{k}; }
Size block(BlockSize b) { return Size{Size::Kind::Block, b}; }

// binding_size = (lambda_with_env, size) Lazy_backtrack.t; the env
// (binding_size Ident.Map.t) is a persistent list, newest binding first
struct EnvNode;
using SizeEnv = std::shared_ptr<const EnvNode>;
struct BindingSize {
  bool forced = false;
  Size value{Size::Kind::Unreachable};
  lam_t lambda = nullptr;
  SizeEnv env;
};
struct EnvNode {
  Ident::t id;
  std::shared_ptr<BindingSize> size;
  SizeEnv next;
};
SizeEnv env_add(Ident::t id, std::shared_ptr<BindingSize> s, SizeEnv env) {
  return std::make_shared<const EnvNode>(EnvNode{id, std::move(s), std::move(env)});
}
std::shared_ptr<BindingSize> env_find_opt(Ident::t id, SizeEnv env) {
  for (const EnvNode* n = env.get(); n; n = n->next.get())
    if (ident::compare(n->id, id) == 0) return n->size;
  return nullptr;
}

[[noreturn]] Size dynamic_size() { fatal_error("letrec: No size found for Static binding"); }

Size join_sizes(const Size& size1, const Size& size2) {
  if (size1.kind == Size::Kind::Unreachable) return size2;
  if (size2.kind == Size::Kind::Unreachable) return size1;
  dynamic_size();
}

std::optional<BlockSize> find_size_of_alloc_prim(const PrimitiveDescription* prim, Slice<lam_t> args) {
  auto same_as = [&](const PrimitiveDescription* other) { return prim->prim_name == other->prim_name; };
  std::optional<long> int_arg;
  if (args.size() == 1)
    if (auto* c = as<Lconst>(args[0]); c && c->c->kind == StructuredConstant::Kind::Const_int) int_arg = c->c->i;
  if (same_as(alloc_prim())) {
    if (int_arg) return BlockSize{BlockSize::Kind::Regular_block, *int_arg};
    return std::nullopt;
  }
  if (same_as(alloc_float_record_prim())) {
    if (int_arg) return BlockSize{BlockSize::Kind::Float_record, *int_arg};
    return std::nullopt;
  }
  if (same_as(alloc_lazy_prim())) return BlockSize{BlockSize::Kind::Lazy_block};
  return std::nullopt;
}

Size compute_expression_size(const SizeEnv& env, lam_t lam);

Size force(const std::shared_ptr<BindingSize>& b) {
  if (!b->forced) {
    b->value = compute_expression_size(b->env, b->lambda);
    b->forced = true;
  }
  return b->value;
}

Size compute_and_join_sizes(const SizeEnv& env, std::initializer_list<lam_t> branches) {
  Size size = sz(Size::Kind::Unreachable);
  for (lam_t branch : branches) size = join_sizes(size, compute_expression_size(env, branch));
  return size;
}

Size size_of_primitive(const SizeEnv& env, const Primitive& p, Slice<lam_t> args) {
  using BK = BlockSize::Kind;
  switch (p.kind) {
    case PK::Pignore: case PK::Psetfield: case PK::Psetfield_computed: case PK::Psetfloatfield:
    case PK::Poffsetint: case PK::Poffsetref: case PK::Pbytessetu: case PK::Pbytessets: case PK::Parraysetu:
    case PK::Parraysets: case PK::Pcheckbound: case PK::Pbigarrayset: case PK::Pbytes_set_16:
    case PK::Pbytes_set_32: case PK::Pbytes_set_64: case PK::Pbigstring_set_16: case PK::Pbigstring_set_32:
    case PK::Pbigstring_set_64: case PK::Ppoll:
      // Unit-returning primitives
      return sz(Size::Kind::Constant);
    case PK::Pduprecord:
      switch (p.repr.kind) {
        case RecordRepresentation::Kind::Record_regular:
        case RecordRepresentation::Kind::Record_inlined:
        case RecordRepresentation::Kind::Record_extension: return block({BK::Regular_block, p.n});
        case RecordRepresentation::Kind::Record_float: return block({BK::Float_record, p.n});
        case RecordRepresentation::Kind::Record_unboxed: fatal_error("size_of_primitive");
      }
      fatal_error("size_of_primitive");
    case PK::Pmakeblock: return block({BK::Regular_block, static_cast<long>(args.size())});
    case PK::Pmakelazyblock: return block({BK::Lazy_block});
    case PK::Pmakearray: {
      long size = static_cast<long>(args.size());
      if (p.array == ArrayKind::Pfloatarray) return block({BK::Float_record, size});
      return block({BK::Regular_block, size});
    }
    case PK::Pduparray:
      // The size has to be recovered from the size of the argument
      if (args.size() == 1) return compute_expression_size(env, args[0]);
      fatal_error("size_of_primitive");
    case PK::Praise: return sz(Size::Kind::Unreachable);
    case PK::Pctconst: return sz(Size::Kind::Constant);
    case PK::Pccall:
      if (std::optional<BlockSize> s = find_size_of_alloc_prim(p.ccall, args)) return block(*s);
      dynamic_size();
    default: dynamic_size();
  }
}

Size compute_expression_size(const SizeEnv& env, lam_t lam) {
  switch (lam->kind) {
    case LK::Lvar: {
      std::shared_ptr<BindingSize> b = env_find_opt(static_cast<const Lvar*>(lam)->id, env);
      if (!b) dynamic_size();
      return force(b);
    }
    case LK::Lmutvar: dynamic_size();
    case LK::Lconst: return sz(Size::Kind::Constant);
    case LK::Lapply: dynamic_size();
    case LK::Lfunction: return sz(Size::Kind::Function);
    case LK::Llet: {
      auto* l = static_cast<const Llet*>(lam);
      auto b = std::make_shared<BindingSize>();
      b->lambda = l->arg;
      b->env = env;
      return compute_expression_size(env_add(l->id, b, env), l->body);
    }
    case LK::Lmutlet: return compute_expression_size(env, static_cast<const Lmutlet*>(lam)->body);
    case LK::Lletrec: {
      auto* l = static_cast<const Lletrec*>(lam);
      SizeEnv env_acc = env;
      for (const L::RecBinding& rb : l->decl) {
        auto b = std::make_shared<BindingSize>();
        b->forced = true;
        b->value = sz(Size::Kind::Function);
        env_acc = env_add(rb.id, b, env_acc);
      }
      return compute_expression_size(env_acc, l->body);
    }
    case LK::Lprim: {
      auto* l = static_cast<const Lprim*>(lam);
      return size_of_primitive(env, l->p, l->args);
    }
    case LK::Lswitch: {
      auto* l = static_cast<const Lswitch*>(lam);
      Size size = sz(Size::Kind::Unreachable);
      for (const SwitchCase& c : l->sw.sw_consts) size = join_sizes(size, compute_expression_size(env, c.action));
      for (const SwitchCase& c : l->sw.sw_blocks) size = join_sizes(size, compute_expression_size(env, c.action));
      if (l->sw.sw_failaction) size = join_sizes(size, compute_expression_size(env, l->sw.sw_failaction));
      return size;
    }
    case LK::Lstringswitch: {
      auto* l = static_cast<const Lstringswitch*>(lam);
      Size size = sz(Size::Kind::Unreachable);
      for (const StringCase& c : l->cases) size = join_sizes(size, compute_expression_size(env, c.action));
      if (l->def) size = join_sizes(size, compute_expression_size(env, l->def));
      return size;
    }
    case LK::Lstaticraise: return sz(Size::Kind::Unreachable);
    case LK::Lstaticcatch: {
      auto* l = static_cast<const Lstaticcatch*>(lam);
      return compute_and_join_sizes(env, {l->body, l->handler});
    }
    case LK::Ltrywith: {
      auto* l = static_cast<const Ltrywith*>(lam);
      return compute_and_join_sizes(env, {l->body, l->handler});
    }
    case LK::Lifthenelse: {
      auto* l = static_cast<const Lifthenelse*>(lam);
      return compute_and_join_sizes(env, {l->ifso, l->ifnot});
    }
    case LK::Lsequence: return compute_expression_size(env, static_cast<const Lsequence*>(lam)->l2);
    case LK::Lwhile:
    case LK::Lfor:
    case LK::Lassign: return sz(Size::Kind::Constant);
    case LK::Lsend: dynamic_size();
    case LK::Levent: return compute_expression_size(env, static_cast<const Levent*>(lam)->l);
    case LK::Lifused: return sz(Size::Kind::Constant);
  }
  fatal_error("compute_expression_size");
}

Size compute_static_size(lam_t lam) { return compute_expression_size(nullptr, lam); }

const LFunction* lfunction_with_body(const LFunction* f, lam_t body) {
  return lfunction_(f->kind, f->params, f->return_, body, f->attr, f->loc);
}

// ---- 2. Function Lifting --------------------------------------------------------

struct LiftedFunction {
  const LFunction* lfun;
  long free_vars_block_size;
};

template <class A>
struct SplitResult {  // Unreachable | Reachable of lifted_function * 'a
  bool reachable = false;
  LiftedFunction func{};
  A v{};
};
template <class A>
SplitResult<A> reachable(LiftedFunction f, A v) {
  return SplitResult<A>{true, f, std::move(v)};
}

// The closure blocks are immutable.
const MutableFlag lifted_block_mut = MutableFlag::Immutable;

const ScopedLocation no_loc{};

Primitive pfield(long i) {
  Primitive p = prim(PK::Pfield);
  p.n = i;
  p.ptr = ImmediateOrPointer::Pointer;
  p.mut = lifted_block_mut;
  return p;
}
Primitive pmakeblock() {
  Primitive p = prim(PK::Pmakeblock);
  p.n = 0;
  p.mut = lifted_block_mut;
  return p;
}

// (let+) : res f
template <class F>
SplitResult<lam_t> map_res(const SplitResult<lam_t>& r, F&& f) {
  if (!r.reachable) return {};
  return reachable<lam_t>(r.func, f(r.v));
}

SplitResult<lam_t> split_static_function(Ident::t block_var, const IdentSet& local_idents, lam_t lam);

template <class Arm>
SplitResult<std::vector<Arm>> rebuild_arms(Ident::t block_var, const IdentSet& local_idents, Slice<Arm> arms,
                                           std::size_t from) {
  if (from >= arms.size()) return {};
  SplitResult<std::vector<Arm>> res = rebuild_arms(block_var, local_idents, arms, from + 1);
  SplitResult<lam_t> lam_res = split_static_function(block_var, local_idents, arms[from].action);
  std::vector<Arm> rest(arms.begin() + static_cast<long>(from) + 1, arms.end());
  if (!lam_res.reachable && !res.reachable) return {};
  if (lam_res.reachable && !res.reachable) {
    Arm a = arms[from];
    a.action = lam_res.v;
    rest.insert(rest.begin(), a);
    return reachable(lam_res.func, rest);
  }
  if (!lam_res.reachable && res.reachable) {
    std::vector<Arm> r = res.v;
    r.insert(r.begin(), arms[from]);
    return reachable(res.func, r);
  }
  fatal_error("letrec: multiple functions");
}

SplitResult<lam_t> split_static_function(Ident::t block_var, const IdentSet& local_idents, lam_t lam) {
  switch (lam->kind) {
    case LK::Lvar: {
      // Eta-expand
      Ident::t v = static_cast<const Lvar*>(lam)->id;
      Ident::t param = Ident::create_local("let_rec_param");
      lam_t ap_func = lprim(pfield(0), slice({lvar(block_var)}), no_loc);
      LambdaApply ap;
      ap.ap_func = ap_func;
      ap.ap_args = slice({lvar(param)});
      ap.ap_loc = no_loc;
      lam_t body = lapply(ap);
      const LFunction* wrapper = lfunction_(FunctionKind::Curried, slice({Param{param, ValueKind::gen()}}),
                                            ValueKind::gen(), body, default_stub_attribute(), no_loc);
      LiftedFunction lifted{wrapper, 1};
      return reachable<lam_t>(lifted, lprim(pmakeblock(), slice({lvar(v)}), no_loc));
    }
    case LK::Lfunction: {
      const LFunction* lfun = static_cast<const Lfunction*>(lam)->f;
      IdentSet free_vars = free_variables(lfun->body);
      IdentSet local_free_vars;
      for (Ident::t id : free_vars)
        if (local_idents.count(id)) local_free_vars.insert(id);
      long i = 0;
      IdentMap<lam_t> subst_map;
      std::vector<lam_t> block_fields;
      for (Ident::t var : local_free_vars) {  // Ident.Set.fold: increasing order
        lam_t access = lprim(pfield(i), slice({lvar(block_var)}), no_loc);
        subst_map[var] = access;
        block_fields.push_back(lvar(var));
        i++;
      }
      const LFunction* new_fun = lfunction_with_body(
          lfun, L::subst([](Ident::t, const ValueDescription*, env::t env) { return env; }, false, subst_map,
                              lfun->body));
      LiftedFunction lifted{new_fun, i};
      return reachable<lam_t>(lifted, lprim(pmakeblock(), slice(block_fields), no_loc));
    }
    case LK::Llet: {
      auto* l = static_cast<const Llet*>(lam);
      IdentSet li = local_idents;
      li.insert(l->id);
      return map_res(split_static_function(block_var, li, l->body),
                     [&](lam_t body) { return llet(l->str, l->k, l->id, l->arg, body); });
    }
    case LK::Lmutlet: {
      auto* l = static_cast<const Lmutlet*>(lam);
      IdentSet li = local_idents;
      li.insert(l->id);
      return map_res(split_static_function(block_var, li, l->body),
                     [&](lam_t body) { return lmutlet(l->k, l->id, l->arg, body); });
    }
    case LK::Lletrec: {
      auto* l = static_cast<const Lletrec*>(lam);
      IdentSet li = local_idents;
      for (const L::RecBinding& b : l->decl) li.insert(b.id);
      return map_res(split_static_function(block_var, li, l->body),
                     [&](lam_t body) { return lletrec(l->decl, body); });
    }
    case LK::Lprim:
      if (static_cast<const Lprim*>(lam)->p.kind == PK::Praise) return {};
      break;
    case LK::Lstaticraise: return {};
    case LK::Lswitch: {
      auto* l = static_cast<const Lswitch*>(lam);
      SplitResult<std::vector<SwitchCase>> consts_res =
          rebuild_arms(block_var, local_idents, l->sw.sw_consts, 0);
      SplitResult<std::vector<SwitchCase>> blocks_res =
          rebuild_arms(block_var, local_idents, l->sw.sw_blocks, 0);
      std::optional<SplitResult<lam_t>> fail_res;
      if (l->sw.sw_failaction) fail_res = split_static_function(block_var, local_idents, l->sw.sw_failaction);
      bool fail_reach = fail_res && fail_res->reachable;
      int n = consts_res.reachable + blocks_res.reachable + fail_reach;
      if (n == 0) return {};
      if (n > 1) fatal_error("letrec: multiple functions");
      LambdaSwitch sw = l->sw;
      if (consts_res.reachable) {
        sw.sw_consts = slice(consts_res.v);
        return reachable<lam_t>(consts_res.func, lswitch(l->arg, sw, l->loc));
      }
      if (blocks_res.reachable) {
        sw.sw_blocks = slice(blocks_res.v);
        return reachable<lam_t>(blocks_res.func, lswitch(l->arg, sw, l->loc));
      }
      sw.sw_failaction = fail_res->v;
      return reachable<lam_t>(fail_res->func, lswitch(l->arg, sw, l->loc));
    }
    case LK::Lstringswitch: {
      auto* l = static_cast<const Lstringswitch*>(lam);
      SplitResult<std::vector<StringCase>> arms_res = rebuild_arms(block_var, local_idents, l->cases, 0);
      std::optional<SplitResult<lam_t>> fail_res;
      if (l->def) fail_res = split_static_function(block_var, local_idents, l->def);
      bool fail_reach = fail_res && fail_res->reachable;
      if (!arms_res.reachable && !fail_reach) return {};
      if (arms_res.reachable && !fail_reach)
        return reachable<lam_t>(arms_res.func, lstringswitch(l->arg, slice(arms_res.v), l->def, l->loc));
      if (!arms_res.reachable && fail_reach)
        return reachable<lam_t>(fail_res->func, lstringswitch(l->arg, l->cases, fail_res->v, l->loc));
      fatal_error("letrec: multiple functions");
    }
    case LK::Lstaticcatch: {
      auto* l = static_cast<const Lstaticcatch*>(lam);
      SplitResult<lam_t> body_res = split_static_function(block_var, local_idents, l->body);
      IdentSet li = local_idents;
      for (const Param& p : l->params) li.insert(p.id);
      SplitResult<lam_t> handler_res = split_static_function(block_var, li, l->handler);
      if (!body_res.reachable && !handler_res.reachable) return {};
      if (body_res.reachable && !handler_res.reachable)
        return reachable<lam_t>(body_res.func, lstaticcatch(body_res.v, l->i, l->params, l->handler));
      if (!body_res.reachable && handler_res.reachable)
        return reachable<lam_t>(handler_res.func, lstaticcatch(l->body, l->i, l->params, handler_res.v));
      fatal_error("letrec: multiple functions");
    }
    case LK::Ltrywith: {
      auto* l = static_cast<const Ltrywith*>(lam);
      SplitResult<lam_t> body_res = split_static_function(block_var, local_idents, l->body);
      IdentSet li = local_idents;
      li.insert(l->exn);
      SplitResult<lam_t> handler_res = split_static_function(block_var, li, l->handler);
      if (!body_res.reachable && !handler_res.reachable) return {};
      if (body_res.reachable && !handler_res.reachable)
        return reachable<lam_t>(body_res.func, ltrywith(body_res.v, l->exn, l->handler));
      if (!body_res.reachable && handler_res.reachable)
        return reachable<lam_t>(handler_res.func, ltrywith(l->body, l->exn, handler_res.v));
      fatal_error("letrec: multiple functions");
    }
    case LK::Lifthenelse: {
      auto* l = static_cast<const Lifthenelse*>(lam);
      SplitResult<lam_t> ifso_res = split_static_function(block_var, local_idents, l->ifso);
      SplitResult<lam_t> ifnot_res = split_static_function(block_var, local_idents, l->ifnot);
      if (!ifso_res.reachable && !ifnot_res.reachable) return {};
      if (ifso_res.reachable && !ifnot_res.reachable)
        return reachable<lam_t>(ifso_res.func, lifthenelse(l->cond, ifso_res.v, l->ifnot));
      if (!ifso_res.reachable && ifnot_res.reachable)
        return reachable<lam_t>(ifnot_res.func, lifthenelse(l->cond, l->ifso, ifnot_res.v));
      fatal_error("letrec: multiple functions");
    }
    case LK::Lsequence: {
      auto* l = static_cast<const Lsequence*>(lam);
      return map_res(split_static_function(block_var, local_idents, l->l2),
                     [&](lam_t e2) { return lsequence(l->l1, e2); });
    }
    case LK::Levent: {
      auto* l = static_cast<const Levent*>(lam);
      return map_res(split_static_function(block_var, local_idents, l->l),
                     [&](lam_t e) { return levent(e, l->ev); });
    }
    default: break;
  }
  fatal_error("letrec binding is not a static function");
}

// ---- 3. Compilation --------------------------------------------------------------

struct StaticBinding {
  Ident::t id;
  BlockSize size;
  lam_t lam;
};
struct FunctionBinding {
  Ident::t id;
  const LFunction* lfun;
};
struct DynamicBinding {
  Ident::t id;
  lam_t lam;
};

lam_t compile_indirect(lam_t newval) {
  lam_t indirect = transl_prim("CamlinternalLazy", "indirect");
  LambdaApply ap;
  ap.ap_func = indirect;
  ap.ap_args = slice({newval});
  ap.ap_loc = no_loc;
  return lapply(ap);
}

lam_t compile_alloc(const BlockSize& size) {
  auto alloc = [](const PrimitiveDescription* p, long n) {
    return lprim(pccall(p), slice({lconst(const_int(n))}), no_loc);
  };
  switch (size.kind) {
    case BlockSize::Kind::Regular_block: return alloc(alloc_prim(), size.n);
    case BlockSize::Kind::Float_record: return alloc(alloc_float_record_prim(), size.n);
    case BlockSize::Kind::Lazy_block: return lprim(pccall(alloc_lazy_prim()), slice({lambda_unit()}), no_loc);
  }
  fatal_error("compile_alloc");
}

lam_t compile_update(const BlockSize& size, lam_t dummy, lam_t newval) {
  const PrimitiveDescription* p;
  if (size.kind != BlockSize::Kind::Lazy_block) {
    p = update_prim();
  } else {
    p = update_lazy_prim();
    auto* lp = as<Lprim>(newval);
    if (!(lp && lp->p.kind == PK::Pmakelazyblock)) newval = compile_indirect(newval);
  }
  return lprim(pccall(p), slice({dummy, newval}), no_loc);
}

}  // namespace

lam_t compile_letrec(Slice<RecBinding> input_bindings, lam_t body) {
  IdentMap<lam_t> subst_for_constants;
  for (const RecBinding& b : input_bindings) subst_for_constants[b.id] = dummy_constant();
  // the three lists, reversed (newest first)
  std::vector<StaticBinding> static_rev;
  std::vector<FunctionBinding> functions_rev;
  std::vector<DynamicBinding> dynamic_rev;
  for (const RecBinding& b : input_bindings) {
    if (b.kind == typedtree::RecursiveBindingKind::Dynamic) {
      dynamic_rev.insert(dynamic_rev.begin(), DynamicBinding{b.id, b.def});
      continue;
    }
    Size size = compute_static_size(b.def);
    switch (size.kind) {
      case Size::Kind::Constant:
      case Size::Kind::Unreachable: {
        lam_t def = L::subst([](Ident::t, const ValueDescription*, env::t env) { return env; }, false,
                                  subst_for_constants, b.def);
        dynamic_rev.insert(dynamic_rev.begin(), DynamicBinding{b.id, def});
        break;
      }
      case Size::Kind::Block: static_rev.insert(static_rev.begin(), StaticBinding{b.id, size.block, b.def}); break;
      case Size::Kind::Function: {
        if (auto* lf = as<Lfunction>(b.def)) {
          functions_rev.insert(functions_rev.begin(), FunctionBinding{b.id, lf->f});
          break;
        }
        Ident::t ctx_id = Ident::create_local("letrec_function_context");
        SplitResult<lam_t> r = split_static_function(ctx_id, IdentSet{}, b.def);
        if (!r.reachable) fatal_error("letrec: no function for binding");
        functions_rev.insert(functions_rev.begin(), FunctionBinding{b.id, r.func.lfun});
        static_rev.insert(static_rev.begin(),
                          StaticBinding{ctx_id, BlockSize{BlockSize::Kind::Regular_block, r.func.free_vars_block_size},
                                        r.v});
        break;
      }
    }
  }
  lam_t body_with_patches = body;
  for (const StaticBinding& s : static_rev)
    body_with_patches = lsequence(compile_update(s.size, lvar(s.id), s.lam), body_with_patches);
  lam_t body_with_functions = body_with_patches;
  if (!functions_rev.empty()) {
    std::vector<L::RecBinding> function_bindings;
    for (std::size_t k = functions_rev.size(); k-- > 0;)
      function_bindings.push_back(L::RecBinding{functions_rev[k].id, functions_rev[k].lfun});
    body_with_functions = lletrec(slice(function_bindings), body_with_patches);
  }
  lam_t body_with_dynamic_values = body_with_functions;
  for (const DynamicBinding& d : dynamic_rev)
    body_with_dynamic_values = llet(LetKind::Strict, ValueKind::gen(), d.id, d.lam, body_with_dynamic_values);
  lam_t body_with_pre_allocations = body_with_dynamic_values;
  for (const StaticBinding& s : static_rev) {
    lam_t alloc = compile_alloc(s.size);
    body_with_pre_allocations = llet(LetKind::Strict, ValueKind::gen(), s.id, alloc, body_with_pre_allocations);
  }
  return body_with_pre_allocations;
}

}  // namespace cppcaml::typing::value_rec_compiler
