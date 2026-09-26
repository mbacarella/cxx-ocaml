// Port of lambda/translmod.ml (TYPECHECKER.md stage 10): the bytecode
// paths.  See translmod.hpp.  (The native entry points -- transl_store_*,
// the *_flambda wrappers other than the one transl_implementation calls, and
// the toplevel's transl_toplevel_* -- are not ported.)
//
// Evaluation order follows OCaml's: a constructor's (or tuple's) arguments
// right to left, so where translmod.ml writes `Llet (..., transl_x, body)`
// with `body` a `let` above, the body is translated first and the bound
// expression after -- the order ident stamps are drawn in.
//
// The structure-field list (`fields`, an OCaml list threaded through
// transl_structure) is a persistent cons list here: prepending is O(1) as in
// OCaml.  Translcore's forward reference takes a Slice (head first).
#include "cppcaml/typing/translmod.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/translattribute.hpp"
#include "cppcaml/typing/translclass.hpp"
#include "cppcaml/typing/translcore.hpp"
#include "cppcaml/typing/translobj.hpp"
#include "cppcaml/typing/translprim.hpp"
#include "cppcaml/typing/value_rec_compiler.hpp"

namespace cppcaml::typing {

// SHARED-HEADER ADDITION NEEDED (translcore.hpp): OCaml's
// `transl_let ~scopes ~in_structure rec_flag pat_expr_list` is applied
// partially by Tstr_value -- the bindings are translated *before* the rest
// of the structure (which gives the body) -- so Translcore must expose the
// staged form.  Translcore's transl_let(sc, in_structure, rec, vbs, body)
// is this function applied to body.
namespace translcore {
std::function<lambda::lambda(lambda::lambda)> transl_let(scopes sc, bool in_structure, RecFlag rec_flag,
                                                         Slice<const typedtree::ValueBinding*> pat_expr_list);
}

namespace translmod {

namespace L = ::cppcaml::typing::lambda;
namespace tt = ::cppcaml::typing::typedtree;
using Lam = L::lambda;
using VK = L::ValueKind;
using L::LetKind;
using L::Primitive;
using L::ScopedLocation;
using L::StructuredConstant;
using debuginfo::scopes;
using MC = tt::ModuleCoercion;

std::vector<const PrimitiveDescription*> primitive_declarations;  // OCaml list order: newest first

namespace {

[[noreturn]] void fatal_error(const char* s) { throw std::logic_error(s); }

// ---- small constructors ------------------------------------------------------------------
Primitive pfield(long n) {
  Primitive p = L::prim(Primitive::K::Pfield);
  p.n = n;
  p.ptr = L::ImmediateOrPointer::Pointer;
  p.mut = MutableFlag::Mutable;
  return p;
}
Primitive pmakeblock(long tag, MutableFlag mut) {
  Primitive p = L::prim(Primitive::K::Pmakeblock);
  p.n = tag;
  p.mut = mut;
  return p;
}
Primitive pglobal(Primitive::K k, Ident::t id) {
  Primitive p = L::prim(k);
  p.id = id;
  return p;
}
L::LambdaApply mk_apply(const ScopedLocation& loc, Lam func, Slice<Lam> args) {
  L::LambdaApply ap;
  ap.ap_loc = loc;
  ap.ap_func = func;
  ap.ap_args = args;
  ap.ap_tailcall = L::TailcallAttribute::Default_tailcall;
  ap.ap_inlined = L::InlineAttribute{};
  ap.ap_specialised = L::SpecialiseAttribute::Default_specialise;
  return ap;
}
const StructuredConstant* const_block(long tag, const std::vector<const StructuredConstant*>& fields) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_block;
  c->i = tag;
  c->fields = slice(fields);
  return c;
}
const StructuredConstant* const_immstring(std::string_view s) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_immstring;
  c->s = zborrow(s);
  return c;
}

// ---- the fields list ---------------------------------------------------------------------
struct FNode {
  Ident::t id;
  const FNode* next;
};
using Fields = const FNode*;
Fields cons(Ident::t id, Fields l) { return make<FNode>(FNode{id, l}); }
// List.rev_append ids fields
Fields rev_append(const std::vector<Ident::t>& ids, Fields l) {
  for (Ident::t id : ids) l = cons(id, l);
  return l;
}
std::vector<Ident::t> to_vector(Fields l) {  // in list order
  std::vector<Ident::t> v;
  for (; l; l = l->next) v.push_back(l->id);
  return v;
}
Fields of_slice(Slice<Ident::t> s) {
  Fields l = nullptr;
  for (std::size_t k = s.size(); k-- > 0;) l = cons(s[k], l);
  return l;
}

using NextFn = std::function<Lam(Fields)>;

// Keep track of the root path (from the root of the namespace to the
// currently compiled module expression).  Useful for naming extensions.
// (Path.t option: nullptr = None)
Path::t global_path(Ident::t glob) { return Path::pident(glob); }
Path::t functor_path(Path::t path, Ident::t param) {
  if (!path) return nullptr;
  return Path::papply(path, Path::pident(param));
}
Path::t field_path(Path::t path, Ident::t field) {
  if (!path) return nullptr;
  return Path::pdot(path, ident::name(field));
}

// ---- type extensions -----------------------------------------------------------------------
Lam transl_type_extension(scopes sc, env::t env, Path::t rootpath, const tt::TTypeExtension* tyext, Lam body) {
  // List.fold_right: the last constructor first (innermost)
  auto& cs = tyext->tyext_constructors;
  for (std::size_t k = cs.size(); k-- > 0;) {
    const tt::TExtensionConstructor* ext = cs[k];
    Lam lam = translcore::transl_extension_constructor(sc, env, field_path(rootpath, ext->ext_id), ext);
    body = L::llet(LetKind::Strict, VK::gen(), ext->ext_id, lam, body);
  }
  return body;
}

// ---- coercions ---------------------------------------------------------------------------
using GetField = std::function<Lam(long)>;
Lam apply_coercion(const ScopedLocation& loc, LetKind strict, const MC* restr, Lam arg);
Lam apply_coercion_result(const ScopedLocation& loc, LetKind strict, Lam funct, std::vector<L::Param> params,
                          std::vector<Lam> args, const MC* cc_res);
Lam wrap_id_pos_list(const ScopedLocation& loc, const std::vector<tt::IdPosCoercion>& id_pos_list,
                     const GetField& get_field, Lam lam);

Lam apply_coercion_field(const ScopedLocation& loc, const GetField& get_field, const tt::PosCoercion& pc) {
  return apply_coercion(loc, LetKind::Alias, pc.cc, get_field(pc.pos));
}

Lam apply_coercion(const ScopedLocation& loc, LetKind strict, const MC* restr, Lam arg) {
  switch (restr->kind) {
    case MC::Kind::Tcoerce_none:
      return arg;
    case MC::Kind::Tcoerce_structure:
      return L::name_lambda(strict, arg, [&](Ident::t id) -> Lam {
        GetField get_field = [&](long pos) -> Lam {
          if (pos < 0) return L::lambda_unit();
          return L::lprim(pfield(pos), slice<Lam>({L::lvar(id)}), loc);
        };
        std::vector<Lam> fs;
        for (const tt::PosCoercion& pc : restr->pos_cc) fs.push_back(apply_coercion_field(loc, get_field, pc));
        Lam lam = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(fs), loc);
        return wrap_id_pos_list(loc, std::vector<tt::IdPosCoercion>(restr->id_pos_cc.begin(), restr->id_pos_cc.end()),
                                get_field, lam);
      });
    case MC::Kind::Tcoerce_functor: {
      Ident::t param = Ident::create_local(OCAML_LIT("funarg"));
      Lam carg = apply_coercion(loc, LetKind::Alias, restr->arg, L::lvar(param));
      return apply_coercion_result(loc, strict, arg, {L::Param{param, VK::gen()}}, {carg}, restr->res);
    }
    case MC::Kind::Tcoerce_primitive: {
      const tt::PrimitiveCoercion* p = restr->prim;
      return translprim::transl_primitive(loc, p->pc_desc, p->pc_env, p->pc_type, nullptr);
    }
    case MC::Kind::Tcoerce_alias: {
      Lam lam = L::transl_module_path(loc, restr->alias_env, restr->alias_path);
      return L::name_lambda(strict, arg,
                            [&](Ident::t) { return apply_coercion(loc, LetKind::Alias, restr->alias_coercion, lam); });
    }
  }
  fatal_error("Translmod.apply_coercion");
}

// params / args: in application order (OCaml's reversed accumulators, reversed)
Lam apply_coercion_result(const ScopedLocation& loc, LetKind strict, Lam funct, std::vector<L::Param> params,
                          std::vector<Lam> args, const MC* cc_res) {
  if (cc_res->kind == MC::Kind::Tcoerce_functor) {
    Ident::t param = Ident::create_local(OCAML_LIT("funarg"));
    Lam arg = apply_coercion(loc, LetKind::Alias, cc_res->arg, L::lvar(param));
    params.push_back(L::Param{param, VK::gen()});
    args.push_back(arg);
    return apply_coercion_result(loc, strict, funct, std::move(params), std::move(args), cc_res->res);
  }
  return L::name_lambda(strict, funct, [&](Ident::t id) -> Lam {
    L::FunctionAttribute attr = L::default_function_attribute();
    attr.is_a_functor = true;
    attr.stub = true;
    attr.may_fuse_arity = true;
    Lam body = apply_coercion(loc, LetKind::Strict, cc_res, L::lapply(mk_apply(loc, L::lvar(id), slice(args))));
    return L::lfunction(L::FunctionKind::Curried, slice(params), VK::gen(), body, attr, loc);
  });
}

Lam wrap_id_pos_list(const ScopedLocation& loc, const std::vector<tt::IdPosCoercion>& id_pos_list,
                     const GetField& get_field, Lam lam) {
  L::IdentSet fv = L::free_variables(lam);
  L::IdentMap<Ident::t> s;
  for (const tt::IdPosCoercion& ipc : id_pos_list) {
    if (!fv.count(ipc.id)) continue;
    Ident::t id2 = Ident::create_local(ident::name(ipc.id));
    Lam rhs = apply_coercion(loc, LetKind::Alias, ipc.cc, get_field(ipc.pos));
    L::IdentSet fv_rhs = L::free_variables(rhs);
    lam = L::llet(LetKind::Alias, VK::gen(), id2, rhs, lam);
    fv.insert(fv_rhs.begin(), fv_rhs.end());
    s[ipc.id] = id2;
  }
  if (s.empty()) return lam;  // s == Ident.Map.empty
  return L::rename(s, lam);
}

// Compose two coercions
// apply_coercion c1 (apply_coercion c2 e) behaves like
// apply_coercion (compose_coercions c1 c2) e.
const MC* compose_coercions(const MC* c1, const MC* c2) {
  if (c1->kind == MC::Kind::Tcoerce_none) return c2;
  if (c2->kind == MC::Kind::Tcoerce_none) return c1;
  if (c1->kind == MC::Kind::Tcoerce_structure && c2->kind == MC::Kind::Tcoerce_structure) {
    const Slice<tt::PosCoercion>& v2 = c2->pos_cc;
    auto at = [&](long i) -> const tt::PosCoercion& {
      if (i < 0 || static_cast<std::size_t>(i) >= v2.size()) throw std::out_of_range("index out of bounds");
      return v2[i];
    };
    std::vector<tt::IdPosCoercion> ids1;
    for (const tt::IdPosCoercion& x : c1->id_pos_cc) {
      if (x.pos < 0) {
        ids1.push_back(x);
      } else {
        const tt::PosCoercion& p2 = at(x.pos);
        ids1.push_back(tt::IdPosCoercion{x.id, p2.pos, compose_coercions(x.cc, p2.cc)});
      }
    }
    std::vector<tt::PosCoercion> pcs;
    for (const tt::PosCoercion& pc : c1->pos_cc) {
      if (pc.cc->kind == MC::Kind::Tcoerce_primitive || pc.cc->kind == MC::Kind::Tcoerce_alias) {
        // These cases do not take an argument (the position is -1),
        // so they do not need adjusting.
        pcs.push_back(pc);
      } else {
        const tt::PosCoercion& p2 = at(pc.pos);
        pcs.push_back(tt::PosCoercion{p2.pos, compose_coercions(pc.cc, p2.cc)});
      }
    }
    for (const tt::IdPosCoercion& x : c2->id_pos_cc) ids1.push_back(x);
    auto* r = make<MC>();
    r->kind = MC::Kind::Tcoerce_structure;
    r->pos_cc = slice(pcs);
    r->id_pos_cc = slice(ids1);
    return r;
  }
  if (c1->kind == MC::Kind::Tcoerce_functor && c2->kind == MC::Kind::Tcoerce_functor) {
    const MC* res = compose_coercions(c1->res, c2->res);
    const MC* arg = compose_coercions(c2->arg, c1->arg);
    auto* r = make<MC>();
    r->kind = MC::Kind::Tcoerce_functor;
    r->arg = arg;
    r->res = res;
    return r;
  }
  if (c2->kind == MC::Kind::Tcoerce_alias) {
    const MC* cc = compose_coercions(c1, c2->alias_coercion);
    auto* r = make<MC>();
    r->kind = MC::Kind::Tcoerce_alias;
    r->alias_env = c2->alias_env;
    r->alias_path = c2->alias_path;
    r->alias_coercion = cc;
    return r;
  }
  fatal_error("Translmod.compose_coercions");
}

// ---- primitive declarations ------------------------------------------------------------
void record_primitive(const ValueDescription* vd) {
  if (vd->val_kind.kind == ValueKind::Kind::Val_prim) {
    const PrimitiveDescription* p = vd->val_kind.prim;
    translprim::check_primitive_arity(vd->val_loc, p);
    primitive_declarations.insert(primitive_declarations.begin(), p);
  }
}

// ---- "module rec" -------------------------------------------------------------------------
Lam mod_prim(std::string_view name) { return L::transl_prim("CamlinternalMod", name); }

Lam undefined_location(const Location& loc) {
  // Location.get_pos_info loc.loc_start
  const Position& p = loc.loc_start;
  return L::lconst(const_block(
      0, {const_immstring(p.pos_fname), L::const_int(p.pos_lnum), L::const_int(p.pos_cnum - p.pos_bol)}));
}

struct InitializationFailure {
  UnsafeInfo info;
};
UnsafeInfo unsafe(UnsafeInfo::Reason r, const Location& loc, Path::t path) {
  UnsafeInfo u;
  u.unnamed = false;
  u.reason = r;
  u.loc = loc;
  u.path = path;
  return u;
}

std::vector<const StructuredConstant*> init_shape_struct(Path::t path, env::t env, const Signature& sg,
                                                        std::size_t k);

const StructuredConstant* init_shape_mod(Path::t path, const Location& loc, env::t env, const ModuleType* mty) {
  const ModuleType* m = mtype::scrape(env, mty);
  switch (m->kind) {
    case ModuleType::Kind::Mty_ident:
    case ModuleType::Kind::Mty_alias:
      throw InitializationFailure{unsafe(UnsafeInfo::Reason::Unsafe_module_binding, loc, path)};
    case ModuleType::Kind::Mty_signature:
      return const_block(0, {const_block(0, init_shape_struct(path, env, m->sign, 0))});
    case ModuleType::Kind::Mty_functor:
      // can we do better?
      throw InitializationFailure{unsafe(UnsafeInfo::Reason::Unsafe_functor, loc, path)};
  }
  fatal_error("Translmod.init_shape_mod");
}

// `x :: init_shape_struct ...`: the tail is evaluated first (right to left)
std::vector<const StructuredConstant*> cons_const(const StructuredConstant* x,
                                                 std::vector<const StructuredConstant*> rest) {
  rest.insert(rest.begin(), x);
  return rest;
}

std::vector<const StructuredConstant*> init_shape_struct(Path::t path, env::t env, const Signature& sg,
                                                        std::size_t k) {
  if (k == sg.size()) return {};
  const SignatureItem* it = sg[k];
  switch (it->kind) {
    case SignatureItem::Kind::Sig_value: {
      const ValueDescription* vd = it->value;
      if (vd->val_kind.kind == ValueKind::Kind::Val_reg) {
        Path::t new_path = Path::pdot(path, ident::name(it->id));
        const TypeDesc* d = types::get_desc(ctype::expand_head(env, vd->val_type));
        const StructuredConstant* init_v;
        if (d->kind == DescKind::Tarrow || d->kind == DescKind::Tfunctor) {
          init_v = L::const_int(0);  // camlinternalMod.Function
        } else if (auto* c = as<Tconstr>(d); c && path::same(c->path, predef::paths().lazy_t)) {
          init_v = L::const_int(1);  // camlinternalMod.Lazy
        } else {
          throw InitializationFailure{unsafe(UnsafeInfo::Reason::Unsafe_non_function, vd->val_loc, new_path)};
        }
        // (translmod.ml threads new_path into the rest of the signature)
        return cons_const(init_v, init_shape_struct(new_path, env, sg, k + 1));
      }
      if (vd->val_kind.kind == ValueKind::Kind::Val_prim) return init_shape_struct(path, env, sg, k + 1);
      fatal_error("Translmod.init_shape_struct: assert false");
    }
    case SignatureItem::Kind::Sig_type:
      return init_shape_struct(path, env::add_type(false, it->id, it->type, env), sg, k + 1);
    case SignatureItem::Kind::Sig_typext: {
      Path::t new_path = Path::pdot(path, ident::name(it->id));
      throw InitializationFailure{unsafe(UnsafeInfo::Reason::Unsafe_typext, it->ext->ext_loc, new_path)};
    }
    case SignatureItem::Kind::Sig_module: {
      const ModuleDeclaration* md = it->md;
      if (it->presence == ModulePresence::Mp_present) {
        auto rest = init_shape_struct(
            path, env::add_module_declaration(false, it->id, ModulePresence::Mp_present, md, env), sg, k + 1);
        const StructuredConstant* x =
            init_shape_mod(Path::pdot(path, ident::name(it->id)), md->md_loc, env, md->md_type);
        return cons_const(x, std::move(rest));
      }
      return init_shape_struct(path, env::add_module_declaration(false, it->id, ModulePresence::Mp_absent, md, env),
                               sg, k + 1);
    }
    case SignatureItem::Kind::Sig_modtype:
      return init_shape_struct(path, env::add_modtype(it->id, it->mtd, env), sg, k + 1);
    case SignatureItem::Kind::Sig_class:
      return cons_const(L::const_int(2) /* camlinternalMod.Class */, init_shape_struct(path, env, sg, k + 1));
    case SignatureItem::Kind::Sig_class_type:
      return init_shape_struct(path, env, sg, k + 1);
  }
  fatal_error("Translmod.init_shape_struct");
}

struct InitOk {
  Lam loc;
  Lam shape;
};
using InitResult = std::variant<InitOk, UnsafeInfo>;  // (lambda * lambda, unsafe_info) result

InitResult init_shape(Ident::t id, const tt::ModuleExpr* modl) {
  try {
    const StructuredConstant* shape =
        init_shape_mod(Path::pident(id), modl->mod_loc, modl->mod_env, modl->mod_type);
    Lam lshape = L::lconst(shape);
    return InitOk{undefined_location(modl->mod_loc), lshape};
  } catch (const InitializationFailure& f) {
    return f.info;
  }
}

// Reorder bindings to honor dependencies.
struct BindingStatus {  // Undefined | Inprogress of int option | Defined
  enum class Kind { Undefined, Inprogress, Defined };
  Kind kind = Kind::Undefined;
  std::optional<std::size_t> parent;
};

struct IdOrIgnoreLoc {  // Id of Ident.t | Ignore_loc of scoped_location
  Ident::t id = nullptr;  // nullptr: Ignore_loc
  ScopedLocation loc;
};

struct RecInput {
  IdOrIgnoreLoc id;
  Location loc;
  InitResult init;
  Lam rhs;
};
struct RecOutput {
  IdOrIgnoreLoc id;
  std::optional<InitOk> init;
  Lam rhs;
};

std::vector<std::pair<Ident::t, UnsafeInfo>> extract_unsafe_cycle(const std::vector<RecInput>& b,
                                                                  const std::vector<BindingStatus>& status,
                                                                  std::size_t cycle_start) {
  auto info = [&](std::size_t i) -> std::pair<Ident::t, UnsafeInfo> {
    if (auto* r = std::get_if<UnsafeInfo>(&b[i].init)) {
      if (!b[i].id.id) fatal_error("Translmod.extract_unsafe_cycle: assert false");
      return {b[i].id.id, *r};
    }
    fatal_error("Translmod.extract_unsafe_cycle: assert false");
  };
  std::vector<std::pair<Ident::t, UnsafeInfo>> l;  // the OCaml list, head first
  std::size_t stop = cycle_start, i = cycle_start;
  for (;;) {
    const BindingStatus& st = status[i];
    if (st.kind != BindingStatus::Kind::Inprogress || !st.parent)
      fatal_error("Translmod.extract_unsafe_cycle: assert false");
    i = *st.parent;
    l.insert(l.begin(), info(i));
    if (i == stop) return l;
  }
}

std::vector<RecOutput> reorder_rec_bindings(const std::vector<RecInput>& b) {
  std::size_t num_bindings = b.size();
  std::vector<L::IdentSet> fv;
  for (const RecInput& r : b) fv.push_back(L::free_variables(r.rhs));
  std::vector<BindingStatus> status(num_bindings);
  std::vector<RecOutput> res;  // in emission order (List.rev !res)
  auto is_unsafe = [&](std::size_t i) { return std::holds_alternative<UnsafeInfo>(b[i].init); };
  auto init_res = [&](std::size_t i) -> std::optional<InitOk> {
    if (auto* ok = std::get_if<InitOk>(&b[i].init)) return *ok;
    return std::nullopt;
  };
  std::function<void(std::optional<std::size_t>, std::size_t)> emit_binding =
      [&](std::optional<std::size_t> parent, std::size_t i) {
        switch (status[i].kind) {
          case BindingStatus::Kind::Defined:
            return;
          case BindingStatus::Kind::Inprogress: {
            status[i] = {BindingStatus::Kind::Inprogress, parent};
            auto cycle = extract_unsafe_cycle(b, status, i);
            Error e(b[i].loc, Error::Kind::Circular_dependency);
            e.cycle = std::move(cycle);
            throw e;
          }
          case BindingStatus::Kind::Undefined:
            if (is_unsafe(i)) {
              status[i] = {BindingStatus::Kind::Inprogress, parent};
              for (std::size_t j = 0; j < num_bindings; ++j) {
                Ident::t idj = b[j].id.id;
                if (idj && fv[i].count(idj)) emit_binding(i, j);
              }
            }
            res.push_back(RecOutput{b[i].id, init_res(i), b[i].rhs});
            status[i] = {BindingStatus::Kind::Defined, std::nullopt};
            return;
        }
      };
  for (std::size_t i = 0; i < num_bindings; ++i) {
    switch (status[i].kind) {
      case BindingStatus::Kind::Undefined:
        emit_binding(std::nullopt, i);
        break;
      case BindingStatus::Kind::Inprogress:
        fatal_error("Translmod.reorder_rec_bindings: assert false");
      case BindingStatus::Kind::Defined:
        break;
    }
  }
  return res;
}

// Generate lambda-code for a reordered list of bindings
Lam eval_rec_bindings(const std::vector<RecOutput>& bindings, Lam cont) {
  std::size_t n = bindings.size();
  std::function<Lam(std::size_t)> patch_forwards = [&](std::size_t k) -> Lam {
    if (k == n) return cont;
    const RecOutput& b = bindings[k];
    if (!b.id.id || !b.init) return patch_forwards(k + 1);
    Lam rest = patch_forwards(k + 1);
    Lam f = mod_prim("update_mod");
    return L::lsequence(
        L::lapply(mk_apply({}, f, slice<Lam>({b.init->shape, L::lvar(b.id.id), b.rhs}))), rest);
  };
  std::function<Lam(std::size_t)> bind_strict = [&](std::size_t k) -> Lam {
    if (k == n) return patch_forwards(0);
    const RecOutput& b = bindings[k];
    if (!b.id.id && !b.init) {
      Lam rest = bind_strict(k + 1);
      return L::lsequence(L::lprim(L::prim(Primitive::K::Pignore), slice<Lam>({b.rhs}), b.id.loc), rest);
    }
    if (b.id.id && !b.init) {
      Lam rest = bind_strict(k + 1);
      return L::llet(LetKind::Strict, VK::gen(), b.id.id, b.rhs, rest);
    }
    return bind_strict(k + 1);
  };
  std::function<Lam(std::size_t)> bind_inits = [&](std::size_t k) -> Lam {
    if (k == n) return bind_strict(0);
    const RecOutput& b = bindings[k];
    if (!b.id.id || !b.init) return bind_inits(k + 1);
    Lam rest = bind_inits(k + 1);
    Lam f = mod_prim("init_mod");
    return L::llet(LetKind::Strict, VK::gen(), b.id.id,
                   L::lapply(mk_apply({}, f, slice<Lam>({b.init->loc, b.init->shape}))), rest);
  };
  return bind_inits(0);
}

using CompileRhs = std::function<Lam(Ident::t, const tt::ModuleExpr*)>;
Lam compile_recmodule(scopes sc, const CompileRhs& compile_rhs, Slice<const tt::ModuleBinding*> bindings, Lam cont) {
  std::vector<RecInput> in;
  for (const tt::ModuleBinding* mb : bindings) {
    const tt::ModuleExpr* modl = mb->mb_expr;
    IdOrIgnoreLoc idl;
    InitResult shape;
    if (!mb->mb_id) {
      idl.loc = debuginfo::of_location(sc, mb->mb_name.loc);
      UnsafeInfo u;
      u.unnamed = true;
      shape = u;
    } else {
      idl.id = mb->mb_id;
      shape = init_shape(mb->mb_id, modl);
    }
    Lam rhs = compile_rhs(mb->mb_id, modl);
    in.push_back(RecInput{idl, modl->mod_loc, shape, rhs});
  }
  return eval_rec_bindings(reorder_rec_bindings(in), cont);
}

// ---- classes in a structure --------------------------------------------------------------
std::pair<std::vector<Ident::t>, std::vector<value_rec_compiler::RecBinding>> transl_class_bindings(
    scopes sc, Slice<tt::ClassDeclarationItem> cl_list) {
  std::vector<Ident::t> ids;
  for (const tt::ClassDeclarationItem& c : cl_list) ids.push_back(c.decl->ci_id_class);
  Slice<Ident::t> sids = slice(ids);
  std::vector<value_rec_compiler::RecBinding> out;
  for (const tt::ClassDeclarationItem& c : cl_list) {
    const tt::TClassDeclaration* ci = c.decl;
    auto [def, rkind] = translclass::transl_class(sc, sids, ci->ci_id_class, c.names, ci->ci_expr, ci->ci_virt);
    out.push_back(value_rec_compiler::RecBinding{ci->ci_id_class, rkind, def});
  }
  return {ids, out};
}

// ---- functors ------------------------------------------------------------------------------
L::InlineAttribute merge_inline_attributes(const L::InlineAttribute& a1, const L::InlineAttribute& a2,
                                           const ScopedLocation& loc) {
  if (auto a = L::merge_inline_attributes(a1, a2)) return *a;
  throw Error(debuginfo::to_location(loc), Error::Kind::Conflicting_inline_attributes);
}

struct FunctorParam {
  Ident::t param;
  ScopedLocation loc;
  const MC* arg_coercion;
};
struct Merged {
  std::vector<FunctorParam> params;  // in merge order (the OCaml accumulator reversed)
  const tt::ModuleExpr* body;
  Path::t path;
  const MC* coercion;
  L::InlineAttribute inline_attribute;
};

Merged merge_functors(scopes sc, const tt::ModuleExpr* mexp, const MC* coercion, Path::t root_path) {
  Merged m{{}, mexp, root_path, coercion, L::InlineAttribute{}};
  for (;;) {
    auto* f = tt::as<tt::Tmod_functor>(m.body->mod_desc);
    if (!f) return m;
    L::InlineAttribute inline_attribute2 = translattribute::get_inline_attribute(m.body->mod_attributes);
    const MC* arg_coercion;
    const MC* res_coercion;
    switch (m.coercion->kind) {
      case MC::Kind::Tcoerce_none:
        arg_coercion = tt::tcoerce_none();
        res_coercion = tt::tcoerce_none();
        break;
      case MC::Kind::Tcoerce_functor:
        arg_coercion = m.coercion->arg;
        res_coercion = m.coercion->res;
        break;
      default:
        fatal_error("Translmod.merge_functors: bad coercion");
    }
    ScopedLocation loc = debuginfo::of_location(sc, m.body->mod_loc);
    Path::t path;
    Ident::t param;
    if (f->param.is_unit) {
      path = nullptr;
      param = Ident::create_local(OCAML_LIT("*"));
    } else if (!f->param.id) {
      Ident::t id = Ident::create_local(OCAML_LIT("_"));
      path = functor_path(m.path, id);
      param = id;
    } else {
      path = functor_path(m.path, f->param.id);
      param = f->param.id;
    }
    m.inline_attribute = merge_inline_attributes(m.inline_attribute, inline_attribute2, loc);
    m.params.push_back(FunctorParam{param, loc, arg_coercion});
    m.body = f->body;
    m.coercion = res_coercion;
    m.path = path;
  }
}

Lam transl_module(scopes sc, const MC* cc, Path::t rootpath, const tt::ModuleExpr* mexp);
Lam transl_structure(scopes sc, const ScopedLocation& loc, Fields fields, const MC* cc, Path::t rootpath,
                     env::t final_env, Slice<const tt::StructureItem*> items, std::size_t k);
Lam transl_struct_item(scopes sc, Fields fields, Path::t rootpath, const tt::StructureItem* item,
                       const NextFn& next);

Lam compile_functor(scopes sc, const tt::ModuleExpr* mexp, const MC* coercion, Path::t root_path,
                    const ScopedLocation& floc) {
  Merged m = merge_functors(sc, mexp, coercion, root_path);
  if (m.params.empty()) fatal_error("Translmod.compile_functor: assert false");  // cf. [transl_module]
  Lam body = transl_module(sc, m.coercion, m.path, m.body);
  std::vector<L::Param> params;  // built by prepending: the first-merged ends first
  // List.fold_left over functor_params_rev: the last-merged parameter first
  for (std::size_t k = m.params.size(); k-- > 0;) {
    const FunctorParam& fp = m.params[k];
    Ident::t param2 = ident::rename(fp.param);
    Lam arg = apply_coercion(fp.loc, LetKind::Alias, fp.arg_coercion, L::lvar(param2));
    params.insert(params.begin(), L::Param{param2, VK::gen()});
    body = L::llet(LetKind::Alias, VK::gen(), fp.param, arg, body);
  }
  L::FunctionAttribute attr;
  attr.inline_ = m.inline_attribute;
  attr.specialise = L::SpecialiseAttribute::Default_specialise;
  attr.local = L::LocalAttribute::Default_local;
  attr.poll = L::PollAttribute::Default_poll;
  attr.is_a_functor = true;
  attr.stub = false;
  attr.tmc_candidate = false;
  attr.may_fuse_arity = true;
  return L::lfunction(L::FunctionKind::Curried, slice(params), VK::gen(), body, attr, floc);
}

Lam transl_apply(scopes sc, const ScopedLocation& loc, const MC* cc, env::t mod_env, const tt::ModuleExpr* funct,
                 Lam translated_arg) {
  L::InlineAttribute inlined_attribute = translattribute::get_inlined_attribute_on_module(funct);
  // `oo_wrap mod_env true (apply_coercion loc Strict cc) (Lapply {...})`:
  // the argument (and so the functor's translation) is evaluated before
  // oo_wrap runs
  L::LambdaApply ap = mk_apply(loc, nullptr, slice<Lam>({translated_arg}));
  ap.ap_inlined = inlined_attribute;
  ap.ap_func = transl_module(sc, tt::tcoerce_none(), nullptr, funct);
  Lam x = L::lapply(ap);
  return translobj::oo_wrap(mod_env, true, [&] { return apply_coercion(loc, LetKind::Strict, cc, x); });
}

Lam transl_struct(scopes sc, const ScopedLocation& loc, Fields fields, const MC* cc, Path::t rootpath,
                  const tt::Structure* str) {
  return transl_structure(sc, loc, fields, cc, rootpath, str->str_final_env, str->str_items, 0);
}

Lam transl_module(scopes sc, const MC* cc, Path::t rootpath, const tt::ModuleExpr* mexp) {
  ScopedLocation loc = debuginfo::of_location(sc, mexp->mod_loc);
  const tt::ModuleExprDesc* d = mexp->mod_desc;
  switch (d->kind) {
    case tt::ModuleExprDesc::Kind::Tmod_ident: {
      auto* x = tt::as<tt::Tmod_ident>(d);
      return apply_coercion(loc, LetKind::Strict, cc, L::transl_module_path(loc, mexp->mod_env, x->path));
    }
    case tt::ModuleExprDesc::Kind::Tmod_structure:
      return transl_struct(sc, loc, nullptr, cc, rootpath, tt::as<tt::Tmod_structure>(d)->str);
    case tt::ModuleExprDesc::Kind::Tmod_functor:
      return translobj::oo_wrap(mexp->mod_env, true, [&] { return compile_functor(sc, mexp, cc, rootpath, loc); });
    case tt::ModuleExprDesc::Kind::Tmod_apply: {
      auto* x = tt::as<tt::Tmod_apply>(d);
      Lam translated_arg = transl_module(sc, x->coercion, nullptr, x->arg);
      return transl_apply(sc, loc, cc, mexp->mod_env, x->fn, translated_arg);
    }
    case tt::ModuleExprDesc::Kind::Tmod_apply_unit:
      return transl_apply(sc, loc, cc, mexp->mod_env, tt::as<tt::Tmod_apply_unit>(d)->fn, L::lambda_unit());
    case tt::ModuleExprDesc::Kind::Tmod_constraint: {
      auto* x = tt::as<tt::Tmod_constraint>(d);
      return transl_module(sc, compose_coercions(cc, x->coercion), rootpath, x->me);
    }
    case tt::ModuleExprDesc::Kind::Tmod_unpack:
      return apply_coercion(loc, LetKind::Strict, cc, translcore::transl_exp(sc, tt::as<tt::Tmod_unpack>(d)->exp));
  }
  fatal_error("Translmod.transl_module");
}

// The function transl_structure is called by the bytecode compiler.
// Some effort is made to compile in top to bottom order, in order to display
// warning by increasing locations.
Lam transl_structure(scopes sc, const ScopedLocation& loc, Fields fields, const MC* cc, Path::t rootpath,
                     env::t final_env, Slice<const tt::StructureItem*> items, std::size_t k) {
  if (k < items.size()) {
    return transl_struct_item(sc, fields, rootpath, items[k], [&, k](Fields fields2) {
      return transl_structure(sc, loc, fields2, cc, rootpath, final_env, items, k + 1);
    });
  }
  Lam body;
  std::vector<Ident::t> rev_fields = to_vector(fields);  // List.rev fields
  std::reverse(rev_fields.begin(), rev_fields.end());
  switch (cc->kind) {
    case MC::Kind::Tcoerce_none: {
      std::vector<Lam> vs;
      for (Ident::t id : rev_fields) vs.push_back(L::lvar(id));
      body = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(vs), loc);
      break;
    }
    case MC::Kind::Tcoerce_structure: {
      // Do not ignore id_pos_list !
      const std::vector<Ident::t>& v = rev_fields;
      GetField get_field = [&](long pos) -> Lam {
        if (pos < 0) return L::lambda_unit();
        if (static_cast<std::size_t>(pos) >= v.size()) throw std::out_of_range("index out of bounds");
        return L::lvar(v[pos]);
      };
      L::IdentSet ids(rev_fields.begin(), rev_fields.end());
      std::vector<Lam> fs;
      for (const tt::PosCoercion& pc : cc->pos_cc) {
        if (pc.cc->kind == MC::Kind::Tcoerce_primitive) {
          const tt::PrimitiveCoercion* p = pc.cc->prim;
          fs.push_back(translprim::transl_primitive(debuginfo::of_location(sc, p->pc_loc), p->pc_desc, p->pc_env,
                                                    p->pc_type, nullptr));
        } else {
          fs.push_back(apply_coercion(loc, LetKind::Strict, pc.cc, get_field(pc.pos)));
        }
      }
      Lam lam = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(fs), loc);
      std::vector<tt::IdPosCoercion> id_pos_list;
      for (const tt::IdPosCoercion& x : cc->id_pos_cc)
        if (!ids.count(x.id)) id_pos_list.push_back(x);
      body = wrap_id_pos_list(loc, id_pos_list, get_field, lam);
      break;
    }
    default:
      fatal_error("Translmod.transl_structure");
  }
  // This debugging event provides information regarding the structure
  // items. It is ignored by the OCaml debugger but is used by
  // Js_of_ocaml to preserve variable names.
  if (clflags::debug && !clflags::native_code) {
    auto* ev = make<L::LambdaEvent>();
    ev->lev_loc = loc;
    ev->lev_kind = L::EventKind::Lev_pseudo;
    ev->lev_repr = nullptr;
    ev->lev_env = final_env;
    return L::levent(body, ev);
  }
  return body;
}

// include / open: bind the components of the module value [mid]
Lam rebind_idents(scopes sc, Ident::t mid, const Location& ploc, const std::vector<Ident::t>& ids, std::size_t pos,
                  Fields newfields, const NextFn& next) {
  if (pos == ids.size()) return next(newfields);
  Ident::t id = ids[pos];
  Lam body = rebind_idents(sc, mid, ploc, ids, pos + 1, cons(id, newfields), next);
  return L::llet(LetKind::Alias, VK::gen(), id,
                 L::lprim(pfield(static_cast<long>(pos)), slice<Lam>({L::lvar(mid)}), debuginfo::of_location(sc, ploc)),
                 body);
}

Lam transl_struct_item(scopes sc, Fields fields, Path::t rootpath, const tt::StructureItem* item,
                       const NextFn& next) {
  const tt::StructureItemDesc* d = item->str_desc;
  using K = tt::StructureItemDesc::Kind;
  switch (d->kind) {
    case K::Tstr_eval: {
      Lam body = next(fields);
      return L::lsequence(translcore::transl_exp(sc, tt::as<tt::Tstr_eval>(d)->exp), body);
    }
    case K::Tstr_value: {
      auto* x = tt::as<tt::Tstr_value>(d);
      // Translate bindings first
      std::function<Lam(Lam)> mk_lam_let = translcore::transl_let(sc, true, x->rec, x->vbs);
      Fields ext_fields = rev_append(tt::let_bound_idents(x->vbs), fields);
      // Then, translate remainder of struct
      Lam body = next(ext_fields);
      return mk_lam_let(body);
    }
    case K::Tstr_primitive:
      record_primitive(tt::as<tt::Tstr_primitive>(d)->pd->prim_val);
      return next(fields);
    case K::Tstr_type:
      return next(fields);
    case K::Tstr_typext: {
      const tt::TTypeExtension* tyext = tt::as<tt::Tstr_typext>(d)->ext;
      std::vector<Ident::t> ids;
      for (const tt::TExtensionConstructor* ext : tyext->tyext_constructors) ids.push_back(ext->ext_id);
      Lam body = next(rev_append(ids, fields));
      return transl_type_extension(sc, item->str_env, rootpath, tyext, body);
    }
    case K::Tstr_exception: {
      const tt::TExtensionConstructor* ext = tt::as<tt::Tstr_exception>(d)->exn->tyexn_constructor;
      Ident::t id = ext->ext_id;
      Path::t path = field_path(rootpath, id);
      Lam body = next(cons(id, fields));
      return L::llet(LetKind::Strict, VK::gen(), id,
                     translcore::transl_extension_constructor(sc, item->str_env, path, ext), body);
    }
    case K::Tstr_module: {
      const tt::ModuleBinding* mb = tt::as<tt::Tstr_module>(d)->mb;
      if (mb->mb_presence == ModulePresence::Mp_absent) return next(fields);
      Ident::t id = mb->mb_id;
      // Translate module first
      scopes subscopes = id ? debuginfo::enter_module_definition(sc, id) : sc;
      Lam module_body =
          transl_module(subscopes, tt::tcoerce_none(), id ? field_path(rootpath, id) : nullptr, mb->mb_expr);
      module_body = translattribute::add_inline_attribute(module_body, mb->mb_loc, mb->mb_attributes);
      // Translate remainder second
      Lam body = next(id ? cons(id, fields) : fields);
      if (!id)
        return L::lsequence(L::lprim(L::prim(Primitive::K::Pignore), slice<Lam>({module_body}),
                                     debuginfo::of_location(sc, mb->mb_name.loc)),
                            body);
      return L::llet(translcore::pure_module(mb->mb_expr), VK::gen(), id, module_body, body);
    }
    case K::Tstr_recmodule: {
      Slice<const tt::ModuleBinding*> bindings = tt::as<tt::Tstr_recmodule>(d)->mbs;
      std::vector<Ident::t> ids;
      for (const tt::ModuleBinding* mb : bindings)
        if (mb->mb_id) ids.push_back(mb->mb_id);
      Lam body = next(rev_append(ids, fields));
      return compile_recmodule(
          sc,
          [&](Ident::t id, const tt::ModuleExpr* modl) -> Lam {
            if (!id) return transl_module(sc, tt::tcoerce_none(), nullptr, modl);
            return transl_module(debuginfo::enter_module_definition(sc, id), tt::tcoerce_none(),
                                 field_path(rootpath, id), modl);
          },
          bindings, body);
    }
    case K::Tstr_class: {
      auto [ids, class_bindings] = transl_class_bindings(sc, tt::as<tt::Tstr_class>(d)->classes);
      Lam body = next(rev_append(ids, fields));
      return value_rec_compiler::compile_letrec(slice(class_bindings), body);
    }
    case K::Tstr_include: {
      const tt::IncludeDeclaration* incl = tt::as<tt::Tstr_include>(d)->incl;
      std::vector<Ident::t> ids = types::bound_value_identifiers(incl->incl_type);
      const tt::ModuleExpr* modl = incl->incl_mod;
      Ident::t mid = Ident::create_local(OCAML_LIT("include"));
      Lam body = rebind_idents(sc, mid, incl->incl_loc, ids, 0, fields, next);
      Lam m = transl_module(sc, tt::tcoerce_none(), nullptr, modl);
      return L::llet(translcore::pure_module(modl), VK::gen(), mid, m, body);
    }
    case K::Tstr_open: {
      const tt::OpenDeclaration* od = tt::as<tt::Tstr_open>(d)->od;
      LetKind pure = translcore::pure_module(od->open_expr);
      // this optimization shouldn't be needed because Simplif would
      // actually remove the [Llet] when it's not used.
      // But since [scan_used_globals] runs before Simplif, we need to do it.
      if (od->open_bound_items.empty() && pure == LetKind::Alias) return next(fields);
      std::vector<Ident::t> ids = types::bound_value_identifiers(od->open_bound_items);
      Ident::t mid = Ident::create_local(OCAML_LIT("open"));
      Lam body = rebind_idents(sc, mid, od->open_loc, ids, 0, fields, next);
      Lam m = transl_module(sc, tt::tcoerce_none(), nullptr, od->open_expr);
      return L::llet(pure, VK::gen(), mid, m, body);
    }
    case K::Tstr_modtype:
    case K::Tstr_class_type:
    case K::Tstr_attribute:
      return next(fields);
  }
  fatal_error("Translmod.transl_struct_item");
}

// ---- compilation units ---------------------------------------------------------------------
// Introduce dependencies on modules referenced only by "external".
L::IdentSet scan_used_globals(Lam lam) {
  L::IdentSet globals;
  std::function<void(Lam)> scan = [&](Lam l) {
    L::iter_head_constructor(scan, l);
    if (auto* p = L::as<L::Lprim>(l)) {
      if ((p->p.kind == Primitive::K::Pgetglobal || p->p.kind == Primitive::K::Psetglobal) &&
          !ident::is_predef(p->p.id))
        globals.insert(p->p.id);
    }
  };
  scan(lam);
  return globals;
}

L::IdentSet required_globals(bool flambda, Lam body) {
  L::IdentSet globals = scan_used_globals(body);
  auto add_global = [&](Ident::t id, L::IdentSet req) {
    if (!flambda && globals.count(id)) return req;
    req.insert(id);
    return req;
  };
  L::IdentSet required = flambda ? globals : L::IdentSet{};
  for (Path::t path : translprim::get_used_primitives()) required = add_global(path::head(path), required);
  std::vector<Ident::t> rg = env::get_required_globals();
  for (std::size_t k = rg.size(); k-- > 0;) required = add_global(rg[k], required);
  env::reset_required_globals();
  translprim::clear_used_primitives();
  return required;
}

long module_block_size(std::size_t n_component_names, const MC* coercion) {
  switch (coercion->kind) {
    case MC::Kind::Tcoerce_none:
      return static_cast<long>(n_component_names);
    case MC::Kind::Tcoerce_structure:
      return static_cast<long>(coercion->pos_cc.size());
    default:
      fatal_error("Translmod.module_block_size: assert false");
  }
}

L::Program transl_implementation_flambda(std::string_view module_name, const tt::Structure* str, const MC* cc) {
  translobj::reset_labels();
  primitive_declarations.clear();
  translprim::clear_used_primitives();
  Ident::t module_id = Ident::create_persistent(module_name);
  scopes sc = debuginfo::enter_module_definition(debuginfo::empty_scopes, module_id);
  Lam body = translobj::transl_label_init(
      [&] { return transl_struct(sc, ScopedLocation{}, nullptr, cc, global_path(module_id), str); });
  long size = module_block_size(types::bound_value_identifiers(str->str_type).size(), cc);
  L::Program p;
  p.module_ident = module_id;
  p.main_module_block_size = size;
  p.required_globals = required_globals(true, body);
  p.code = body;
  return p;
}

// toplevel_name: the toplevel's unique names (set_toplevel_unique_name is
// the toplevel's; the batch compiler never adds one)
std::vector<std::pair<Ident::t, std::string>> aliased_idents;

}  // namespace

L::Program transl_implementation(std::string_view module_name, const tt::Structure* str, const MC* cc) {
  L::Program implementation = transl_implementation_flambda(module_name, str, cc);
  implementation.code = L::lprim(pglobal(Primitive::K::Psetglobal, implementation.module_ident),
                                 slice<Lam>({implementation.code}), ScopedLocation{});
  return implementation;
}

std::string toplevel_name(Ident::t id) {
  // Ident.find_same: the most recent binding of the same ident
  for (auto it = aliased_idents.rbegin(); it != aliased_idents.rend(); ++it)
    if (ident::same(it->first, id)) return it->second;
  return std::string(ident::name(id));
}

Lam transl_package(Slice<Ident::t> component_names, Ident::t target_name, const MC* coercion) {
  std::vector<Lam> cs;
  for (Ident::t id : component_names)
    cs.push_back(id ? L::lprim(pglobal(Primitive::K::Pgetglobal, id), {}, ScopedLocation{})
                    : L::lconst(L::const_unit()));
  Lam components = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(cs), ScopedLocation{});
  return L::lprim(pglobal(Primitive::K::Psetglobal, target_name),
                  slice<Lam>({apply_coercion(ScopedLocation{}, LetKind::Strict, coercion, components)}),
                  ScopedLocation{});
}

void reset() {
  primitive_declarations.clear();
  // (transl_store_subst: native only)
  aliased_idents.clear();
  env::reset_required_globals();
  translprim::clear_used_primitives();
}

void install_forward_refs() {
  translcore::transl_module = [](scopes sc, const MC* cc, Path::t rootpath, const tt::ModuleExpr* me) {
    return transl_module(sc, cc, rootpath, me);
  };
  translcore::transl_struct_item = [](scopes sc, Slice<Ident::t> fields, Path::t rootpath,
                                      const tt::StructureItem* item,
                                      const std::function<Lam(Slice<Ident::t>)>& next) {
    return transl_struct_item(sc, of_slice(fields), rootpath, item,
                              [&](Fields f) { return next(slice(to_vector(f))); });
  };
  // Translclass's `transl_object := ...`
  translcore::transl_object = [](scopes sc, Ident::t id, Slice<std::string_view> meths, const tt::ClassExpr* cl) {
    return translclass::transl_class(sc, {}, id, meths, cl, VirtualFlag::Concrete).first;
  };
}

}  // namespace translmod
}  // namespace cppcaml::typing
