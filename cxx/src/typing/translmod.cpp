// Port of lambda/translmod.ml (cxx/PORTING.md stage 10): the bytecode
// paths and the native compiler's transl_store_implementation.  See
// translmod.hpp.  (transl_store_phrases / transl_store_package, the
// *_flambda wrappers other than the one transl_implementation calls, and the
// toplevel's transl_toplevel_* are not ported.)
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

// ---- the native compiler: transl_store_* -------------------------------------------------
// A variant of transl_structure used to compile toplevel structure definitions
// for the native-code compiler.  Store the defined values in the fields of the
// global as soon as they are defined, in order to reduce register pressure.
// Also rewrites the defining expressions so that they refer to earlier fields
// of the structure through the fields of the global, not by their names.
// "map" is a table from defined idents to (pos in global block, coercion).
// "prim" is a list of (pos in global block, primitive declaration).

std::vector<Ident::t> defined_idents(Slice<const tt::StructureItem*> items, std::size_t k = 0);
std::vector<Ident::t> all_idents(Slice<const tt::StructureItem*> items, std::size_t k = 0);

void append(std::vector<Ident::t>& v, const std::vector<Ident::t>& w) { v.insert(v.end(), w.begin(), w.end()); }

std::vector<Ident::t> defined_idents(Slice<const tt::StructureItem*> items, std::size_t k) {
  using K = tt::StructureItemDesc::Kind;
  std::vector<Ident::t> r;
  for (; k < items.size(); ++k) {
    const tt::StructureItemDesc* d = items[k]->str_desc;
    switch (d->kind) {
      case K::Tstr_value: append(r, tt::let_bound_idents(tt::as<tt::Tstr_value>(d)->vbs)); break;
      case K::Tstr_typext:
        for (auto* ext : tt::as<tt::Tstr_typext>(d)->ext->tyext_constructors) r.push_back(ext->ext_id);
        break;
      case K::Tstr_exception: r.push_back(tt::as<tt::Tstr_exception>(d)->exn->tyexn_constructor->ext_id); break;
      case K::Tstr_module: {
        const tt::ModuleBinding* mb = tt::as<tt::Tstr_module>(d)->mb;
        if (mb->mb_id && mb->mb_presence == ModulePresence::Mp_present) r.push_back(mb->mb_id);
        break;
      }
      case K::Tstr_recmodule:
        for (auto* mb : tt::as<tt::Tstr_recmodule>(d)->mbs)
          if (mb->mb_id) r.push_back(mb->mb_id);
        break;
      case K::Tstr_open: append(r, types::bound_value_identifiers(tt::as<tt::Tstr_open>(d)->od->open_bound_items)); break;
      case K::Tstr_class:
        for (auto& ci : tt::as<tt::Tstr_class>(d)->classes) r.push_back(ci.decl->ci_id_class);
        break;
      case K::Tstr_include: append(r, types::bound_value_identifiers(tt::as<tt::Tstr_include>(d)->incl->incl_type)); break;
      default: break;
    }
  }
  return r;
}

// the structure of a module expression that is a structure, possibly
// constrained (nullptr otherwise)
const tt::Structure* structure_of(const tt::ModuleExpr* me, bool through_constraint) {
  if (auto* st = tt::as<tt::Tmod_structure>(me->mod_desc)) return st->str;
  if (!through_constraint) return nullptr;
  if (auto* c = tt::as<tt::Tmod_constraint>(me->mod_desc))
    if (auto* st = tt::as<tt::Tmod_structure>(c->me->mod_desc)) return st->str;
  return nullptr;
}

// second level idents (module M = struct ... let id = ... end),
// and all sub-levels idents
std::vector<Ident::t> more_idents(Slice<const tt::StructureItem*> items, std::size_t k = 0) {
  using K = tt::StructureItemDesc::Kind;
  std::vector<Ident::t> r;
  for (; k < items.size(); ++k) {
    const tt::StructureItemDesc* d = items[k]->str_desc;
    switch (d->kind) {
      case K::Tstr_open:
        if (auto* st = structure_of(tt::as<tt::Tstr_open>(d)->od->open_expr, false)) append(r, all_idents(st->str_items));
        break;
      case K::Tstr_include:
        if (auto* st = structure_of(tt::as<tt::Tstr_include>(d)->incl->incl_mod, true)) append(r, all_idents(st->str_items));
        break;
      case K::Tstr_module: {
        const tt::ModuleBinding* mb = tt::as<tt::Tstr_module>(d)->mb;
        if (mb->mb_presence == ModulePresence::Mp_present)
          if (auto* st = structure_of(mb->mb_expr, true)) append(r, all_idents(st->str_items));
        break;
      }
      default: break;
    }
  }
  return r;
}

std::vector<Ident::t> all_idents(Slice<const tt::StructureItem*> items, std::size_t k) {
  using K = tt::StructureItemDesc::Kind;
  std::vector<Ident::t> r;
  for (; k < items.size(); ++k) {
    const tt::StructureItemDesc* d = items[k]->str_desc;
    switch (d->kind) {
      case K::Tstr_value: append(r, tt::let_bound_idents(tt::as<tt::Tstr_value>(d)->vbs)); break;
      case K::Tstr_typext:
        for (auto* ext : tt::as<tt::Tstr_typext>(d)->ext->tyext_constructors) r.push_back(ext->ext_id);
        break;
      case K::Tstr_exception: r.push_back(tt::as<tt::Tstr_exception>(d)->exn->tyexn_constructor->ext_id); break;
      case K::Tstr_recmodule:
        for (auto* mb : tt::as<tt::Tstr_recmodule>(d)->mbs)
          if (mb->mb_id) r.push_back(mb->mb_id);
        break;
      case K::Tstr_open: {
        const tt::OpenDeclaration* od = tt::as<tt::Tstr_open>(d)->od;
        append(r, types::bound_value_identifiers(od->open_bound_items));
        if (auto* st = structure_of(od->open_expr, false)) append(r, all_idents(st->str_items));
        break;
      }
      case K::Tstr_class:
        for (auto& ci : tt::as<tt::Tstr_class>(d)->classes) r.push_back(ci.decl->ci_id_class);
        break;
      case K::Tstr_include: {
        const tt::IncludeDeclaration* incl = tt::as<tt::Tstr_include>(d)->incl;
        append(r, types::bound_value_identifiers(incl->incl_type));
        if (auto* st = structure_of(incl->incl_mod, true)) append(r, all_idents(st->str_items));
        break;
      }
      case K::Tstr_module: {
        const tt::ModuleBinding* mb = tt::as<tt::Tstr_module>(d)->mb;
        if (!mb->mb_id || mb->mb_presence != ModulePresence::Mp_present) break;
        r.push_back(mb->mb_id);
        if (auto* st = structure_of(mb->mb_expr, true)) append(r, all_idents(st->str_items));
        break;
      }
      default: break;
    }
  }
  return r;
}

// In the native toplevel, this reference is threaded through successive
// calls of transl_store_structure
L::IdentPMap<Lam> transl_store_subst;  // Ident.Map: persistent

// field_of_str loc str (pos, cc)
std::function<Lam(const tt::PosCoercion&)> field_of_str(const ScopedLocation& loc, const tt::Structure* str) {
  std::vector<Ident::t> ids = defined_idents(str->str_items);
  return [loc, ids](const tt::PosCoercion& pc) -> Lam {
    const MC* cc = pc.cc;
    switch (cc->kind) {
      case MC::Kind::Tcoerce_primitive:
        return translprim::transl_primitive(loc, cc->prim->pc_desc, cc->prim->pc_env, cc->prim->pc_type, nullptr);
      case MC::Kind::Tcoerce_alias: {
        Lam lam = L::transl_module_path(loc, cc->alias_env, cc->alias_path);
        return apply_coercion(loc, LetKind::Alias, cc->alias_coercion, lam);
      }
      default:
        if (pc.pos < 0 || static_cast<std::size_t>(pc.pos) >= ids.size()) throw std::out_of_range("index out of bounds");
        return apply_coercion(loc, LetKind::Strict, cc, L::lvar(ids[pc.pos]));
    }
  };
}

Lam lambda_subst(const L::IdentMap<Lam>& subst, Lam lam) {
  return L::subst([](Ident::t, const ValueDescription*, env::t env) { return env; }, false, subst, lam);
}
Lam lambda_subst(const L::IdentPMap<Lam>& subst, Lam lam) {
  return L::subst([](Ident::t, const ValueDescription*, env::t env) { return env; }, false, subst, lam);
}

// Ident.tbl: find_same finds the latest binding of the same ident (by
// name, then along the name's bindings, newest first)
struct IdentTbl {
  std::vector<std::pair<Ident::t, std::pair<long, const MC*>>> v;
  std::unordered_map<std::string_view, std::vector<std::size_t>> by_name;
  void add(Ident::t id, std::pair<long, const MC*> data) {
    by_name[ident::name(id)].push_back(v.size());
    v.push_back({id, data});
  }
  const std::pair<long, const MC*>* find_same(Ident::t id) const {
    auto it = by_name.find(ident::name(id));
    if (it == by_name.end()) return nullptr;
    for (auto k = it->second.rbegin(); k != it->second.rend(); ++k)
      if (ident::same(v[*k].first, id)) return &v[*k].second;
    return nullptr;
  }
};
struct AliasEntry {
  long pos;
  env::t env;
  Path::t path;
  const MC* cc;
};

struct StoreCtx {
  Ident::t glob;
  const IdentTbl& map;

  Primitive getglobal() const { return pglobal(Primitive::K::Pgetglobal, glob); }

  Lam store_ident(const ScopedLocation& loc, Ident::t id) const {
    const std::pair<long, const MC*>* e = map.find_same(id);
    if (!e) fatal_error("Translmod.store_ident");
    Lam init_val = apply_coercion(loc, LetKind::Alias, e->second, L::lvar(id));
    Primitive p = L::prim(Primitive::K::Psetfield);
    p.n = e->first;
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.init = L::InitializationOrAssignment::Root_initialization;
    return L::lprim(p, slice<Lam>({L::lprim(getglobal(), {}, loc), init_val}), loc);
  }
  Lam store_idents(const ScopedLocation& loc, const std::vector<Ident::t>& ids) const {
    return L::make_sequence([&](Ident::t id) { return store_ident(loc, id); }, ids);
  }
  L::IdentPMap<Lam> add_ident(bool may_coerce, Ident::t id, L::IdentPMap<Lam> subst) const {
    const std::pair<long, const MC*>* e = map.find_same(id);
    if (!e) fatal_error("Translmod.add_ident: assert false");
    if (e->second->kind == MC::Kind::Tcoerce_none) {
      Primitive f = L::prim(Primitive::K::Pfield);
      f.n = e->first;
      f.ptr = L::ImmediateOrPointer::Pointer;
      f.mut = MutableFlag::Immutable;
      return subst.add(id, L::lprim(f, slice<Lam>({L::lprim(getglobal(), {}, ScopedLocation{})}), ScopedLocation{}));
    }
    if (may_coerce) return subst;
    fatal_error("Translmod.add_ident: assert false");
  }
  // List.fold_right (add_ident may_coerce) idlist subst
  L::IdentPMap<Lam> add_idents(bool may_coerce, const std::vector<Ident::t>& ids, L::IdentPMap<Lam> subst) const {
    for (std::size_t k = ids.size(); k-- > 0;) subst = add_ident(may_coerce, ids[k], std::move(subst));
    return subst;
  }

  // transl_store ~scopes rootpath subst cont items[k..]
  Lam transl_store(scopes sc, Path::t rootpath, const L::IdentPMap<Lam>& subst, Lam cont,
                   Slice<const tt::StructureItem*> items, std::size_t k) const {
    if (k == items.size()) {
      transl_store_subst = subst;
      return lambda_subst(subst, cont);
    }
    const tt::StructureItem* item = items[k];
    const tt::StructureItemDesc* d = item->str_desc;
    using K = tt::StructureItemDesc::Kind;
    auto rest = [&](const L::IdentPMap<Lam>& s) { return transl_store(sc, rootpath, s, cont, items, k + 1); };
    switch (d->kind) {
      case K::Tstr_eval: {
        // Lsequence (lambda_subst subst (transl_exp expr), transl_store rem): right to left
        Lam r = rest(subst);
        return L::lsequence(lambda_subst(subst, translcore::transl_exp(sc, tt::as<tt::Tstr_eval>(d)->exp)), r);
      }
      case K::Tstr_value: {
        auto* x = tt::as<tt::Tstr_value>(d);
        std::vector<Ident::t> ids = tt::let_bound_idents(x->vbs);
        Lam body = store_idents(ScopedLocation{}, ids);
        Lam lam = translcore::transl_let(sc, true, x->rec, x->vbs)(body);
        Lam r = rest(add_idents(false, ids, subst));
        return L::lsequence(lambda_subst(subst, lam), r);
      }
      case K::Tstr_primitive:
        record_primitive(tt::as<tt::Tstr_primitive>(d)->pd->prim_val);
        return rest(subst);
      case K::Tstr_type:
        return rest(subst);
      case K::Tstr_typext: {
        const tt::TTypeExtension* tyext = tt::as<tt::Tstr_typext>(d)->ext;
        std::vector<Ident::t> ids;
        for (auto* ext : tyext->tyext_constructors) ids.push_back(ext->ext_id);
        Lam body = store_idents(ScopedLocation{}, ids);
        Lam lam = transl_type_extension(sc, item->str_env, rootpath, tyext, body);
        Lam r = rest(add_idents(false, ids, subst));
        return L::lsequence(lambda_subst(subst, lam), r);
      }
      case K::Tstr_exception: {
        const tt::TExtensionConstructor* ext = tt::as<tt::Tstr_exception>(d)->exn->tyexn_constructor;
        Ident::t id = ext->ext_id;
        Path::t path = field_path(rootpath, id);
        ScopedLocation loc = debuginfo::of_location(sc, ext->ext_loc);
        Lam lam = translcore::transl_extension_constructor(sc, item->str_env, path, ext);
        Lam r = rest(add_ident(false, id, subst));
        Lam st = store_ident(loc, id);
        return L::lsequence(L::llet(LetKind::Strict, VK::gen(), id, lambda_subst(subst, lam), st), r);
      }
      case K::Tstr_module: {
        const tt::ModuleBinding* mb = tt::as<tt::Tstr_module>(d)->mb;
        if (mb->mb_presence == ModulePresence::Mp_absent) return rest(subst);
        Ident::t id = mb->mb_id;
        if (!id) {
          Lam lam = translattribute::add_inline_attribute(transl_module(sc, tt::tcoerce_none(), nullptr, mb->mb_expr),
                                                          mb->mb_loc, mb->mb_attributes);
          Lam r = rest(subst);
          ScopedLocation nloc = debuginfo::of_location(sc, mb->mb_name.loc);
          return L::lsequence(L::lprim(L::prim(Primitive::K::Pignore), slice<Lam>({lambda_subst(subst, lam)}), nloc), r);
        }
        const tt::ModuleExprDesc* md = mb->mb_expr->mod_desc;
        const tt::Structure* str = nullptr;
        const MC* map_cc = nullptr;  // the Tcoerce_structure of a constrained structure
        if (auto* st = tt::as<tt::Tmod_structure>(md)) {
          str = st->str;
        } else if (auto* c = tt::as<tt::Tmod_constraint>(md)) {
          if (auto* st2 = tt::as<tt::Tmod_structure>(c->me->mod_desc);
              st2 && c->coercion->kind == MC::Kind::Tcoerce_structure) {
            str = st2->str;
            map_cc = c->coercion;
          }
        }
        if (str) {
          ScopedLocation loc = debuginfo::of_location(sc, mb->mb_loc);
          Lam lam = transl_store(debuginfo::enter_module_definition(sc, id), field_path(rootpath, id), subst,
                                 L::lambda_unit(), str->str_items, 0);
          // Careful: see next case
          L::IdentPMap<Lam> subst2 = transl_store_subst;
          Lam r = rest(add_ident(true, id, subst2));
          Lam st = store_ident(loc, id);
          std::vector<Lam> fields;
          if (!map_cc) {
            for (Ident::t fid : defined_idents(str->str_items)) fields.push_back(L::lvar(fid));
          } else {
            auto field = field_of_str(loc, str);
            for (const tt::PosCoercion& pc : map_cc->pos_cc) fields.push_back(field(pc));  // List.map: left to right
          }
          Lam block = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(fields), loc);
          return L::lsequence(lam, L::llet(LetKind::Strict, VK::gen(), id, lambda_subst(subst2, block), L::lsequence(st, r)));
        }
        Lam lam = translattribute::add_inline_attribute(
            transl_module(debuginfo::enter_module_definition(sc, id), tt::tcoerce_none(), field_path(rootpath, id),
                          mb->mb_expr),
            mb->mb_loc, mb->mb_attributes);
        // Careful: the module value stored in the global may be different
        // from the local module value, in case a coercion is applied.  If so,
        // keep using the local module value (id) in the remainder of the
        // compilation unit (add_ident true returns subst unchanged).  If not,
        // we can use the value from the global (add_ident true adds id ->
        // Pgetglobal... to subst).
        Lam r = rest(add_ident(true, id, subst));
        Lam st = store_ident(debuginfo::of_location(sc, mb->mb_loc), id);
        return L::llet(LetKind::Strict, VK::gen(), id, lambda_subst(subst, lam), L::lsequence(st, r));
      }
      case K::Tstr_recmodule: {
        Slice<const tt::ModuleBinding*> bindings = tt::as<tt::Tstr_recmodule>(d)->mbs;
        std::vector<Ident::t> ids;
        for (auto* mb : bindings)
          if (mb->mb_id) ids.push_back(mb->mb_id);
        Lam r = rest(add_idents(true, ids, subst));
        Lam c = L::lsequence(store_idents(ScopedLocation{}, ids), r);
        return compile_recmodule(
            sc,
            [&](Ident::t id, const tt::ModuleExpr* modl) -> Lam {
              if (!id) return lambda_subst(subst, transl_module(sc, tt::tcoerce_none(), nullptr, modl));
              return lambda_subst(subst, transl_module(debuginfo::enter_module_definition(sc, id), tt::tcoerce_none(),
                                                       field_path(rootpath, id), modl));
            },
            bindings, c);
      }
      case K::Tstr_class: {
        auto [ids, class_bindings] = transl_class_bindings(sc, tt::as<tt::Tstr_class>(d)->classes);
        Lam body = store_idents(ScopedLocation{}, ids);
        Lam lam = value_rec_compiler::compile_letrec(slice(class_bindings), body);
        Lam r = rest(add_idents(false, ids, subst));
        return L::lsequence(lambda_subst(subst, lam), r);
      }
      case K::Tstr_include: {
        const tt::IncludeDeclaration* incl = tt::as<tt::Tstr_include>(d)->incl;
        const tt::ModuleExpr* modl = incl->incl_mod;
        const tt::Structure* str = nullptr;
        const MC* map_cc = nullptr;
        if (auto* st = tt::as<tt::Tmod_structure>(modl->mod_desc)) {
          str = st->str;
        } else if (auto* c = tt::as<tt::Tmod_constraint>(modl->mod_desc)) {
          if (auto* st2 = tt::as<tt::Tmod_structure>(c->me->mod_desc);
              st2 && (c->coercion->kind == MC::Kind::Tcoerce_structure ||
                      c->coercion->kind == MC::Kind::Tcoerce_none)) {
            str = st2->str;
            map_cc = c->coercion;
          }
        }
        ScopedLocation loc = debuginfo::of_location(sc, incl->incl_loc);
        if (str) {
          // It is tempting to pass rootpath instead of None in order to give
          // a more precise name to exceptions in the included structured, but
          // this would introduce a difference of behavior compared to bytecode.
          Lam lam = transl_store(sc, nullptr, subst, L::lambda_unit(), str->str_items, 0);
          L::IdentPMap<Lam> subst2 = transl_store_subst;
          auto field = field_of_str(loc, str);
          std::vector<Ident::t> ids0 = types::bound_value_identifiers(incl->incl_type);
          std::vector<tt::PosCoercion> map;
          if (map_cc && map_cc->kind == MC::Kind::Tcoerce_structure) {
            map.assign(map_cc->pos_cc.begin(), map_cc->pos_cc.end());
          } else {
            for (std::size_t i = 0; i < ids0.size(); ++i) map.push_back({static_cast<long>(i), tt::tcoerce_none()});
          }
          if (map.size() != ids0.size()) fatal_error("Translmod.transl_store: assert false");
          // loop ids args: the innermost first (right to left)
          std::function<Lam(std::size_t)> loop = [&](std::size_t i) -> Lam {
            if (i == ids0.size()) return rest(add_idents(true, ids0, subst2));
            Lam inner = loop(i + 1);
            Lam st = store_ident(loc, ids0[i]);
            return L::llet(LetKind::Alias, VK::gen(), ids0[i], lambda_subst(subst2, field(map[i])), L::lsequence(st, inner));
          };
          return L::lsequence(lam, loop(0));
        }
        std::vector<Ident::t> ids = types::bound_value_identifiers(incl->incl_type);
        Ident::t mid = Ident::create_local(OCAML_LIT("include"));
        std::function<Lam(long)> store_idents_from = [&](long pos) -> Lam {
          if (static_cast<std::size_t>(pos) == ids.size()) return rest(add_idents(true, ids, subst));
          Ident::t id = ids[pos];
          Lam inner = store_idents_from(pos + 1);
          Lam st = store_ident(loc, id);
          Primitive f = pfield(pos);
          return L::llet(LetKind::Alias, VK::gen(), id, L::lprim(f, slice<Lam>({L::lvar(mid)}), loc),
                         L::lsequence(st, inner));
        };
        Lam body = store_idents_from(0);
        return L::llet(LetKind::Strict, VK::gen(), mid,
                       lambda_subst(subst, transl_module(sc, tt::tcoerce_none(), nullptr, modl)), body);
      }
      case K::Tstr_open: {
        const tt::OpenDeclaration* od = tt::as<tt::Tstr_open>(d)->od;
        if (auto* st = tt::as<tt::Tmod_structure>(od->open_expr->mod_desc)) {
          const tt::Structure* str = st->str;
          Lam lam = transl_store(sc, rootpath, subst, L::lambda_unit(), str->str_items, 0);
          ScopedLocation loc = debuginfo::of_location(sc, od->open_loc);
          std::vector<Ident::t> ids = defined_idents(str->str_items);
          std::vector<Ident::t> ids0 = types::bound_value_identifiers(od->open_bound_items);
          L::IdentPMap<Lam> subst2 = transl_store_subst;
          std::function<Lam(long)> store_idents_from = [&](long pos) -> Lam {
            if (static_cast<std::size_t>(pos) == ids0.size()) return rest(add_idents(true, ids0, subst2));
            Ident::t id = ids0[pos];
            Lam inner = store_idents_from(pos + 1);
            Lam st2 = store_ident(loc, id);
            if (static_cast<std::size_t>(pos) >= ids.size()) throw std::out_of_range("index out of bounds");
            return L::llet(LetKind::Alias, VK::gen(), id, L::lvar(ids[pos]), L::lsequence(st2, inner));
          };
          return L::lsequence(lam, lambda_subst(subst2, store_idents_from(0)));
        }
        LetKind pure = translcore::pure_module(od->open_expr);
        // this optimization shouldn't be needed because Simplif would
        // actually remove the [Llet] when it's not used.  But since
        // [scan_used_globals] runs before Simplif, we need to do it.
        if (od->open_bound_items.empty() && pure == LetKind::Alias) return rest(subst);
        std::vector<Ident::t> ids = types::bound_value_identifiers(od->open_bound_items);
        Ident::t mid = Ident::create_local(OCAML_LIT("open"));
        ScopedLocation loc = debuginfo::of_location(sc, od->open_loc);
        std::function<Lam(long)> store_idents_from = [&](long pos) -> Lam {
          if (static_cast<std::size_t>(pos) == ids.size()) return rest(add_idents(true, ids, subst));
          Ident::t id = ids[pos];
          Lam inner = store_idents_from(pos + 1);
          Lam st = store_ident(loc, id);
          return L::llet(LetKind::Alias, VK::gen(), id, L::lprim(pfield(pos), slice<Lam>({L::lvar(mid)}), loc),
                         L::lsequence(st, inner));
        };
        Lam body = store_idents_from(0);
        return L::llet(pure, VK::gen(), mid,
                       lambda_subst(subst, transl_module(sc, tt::tcoerce_none(), nullptr, od->open_expr)), body);
      }
      case K::Tstr_modtype:
      case K::Tstr_class_type:
      case K::Tstr_attribute:
        return rest(subst);
    }
    fatal_error("Translmod.transl_store");
  }
};

Lam transl_store_structure(scopes sc, Ident::t glob, const IdentTbl& map,
                           const std::vector<std::pair<long, const tt::PrimitiveCoercion*>>& prims,
                           const std::vector<AliasEntry>& aliases, Slice<const tt::StructureItem*> str) {
  StoreCtx ctx{glob, map};
  auto setfield = [&](long pos, Lam v) {
    Primitive p = L::prim(Primitive::K::Psetfield);
    p.n = pos;
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.init = L::InitializationOrAssignment::Root_initialization;
    return L::lprim(p, slice<Lam>({L::lprim(ctx.getglobal(), {}, ScopedLocation{}), v}), ScopedLocation{});
  };
  // let aliases = make_sequence store_alias aliases
  Lam alias_seq = L::make_sequence(
      [&](const AliasEntry& a) {
        Lam path_lam = L::transl_module_path(ScopedLocation{}, a.env, a.path);
        Lam init_val = apply_coercion(ScopedLocation{}, LetKind::Strict, a.cc, path_lam);
        return setfield(a.pos, init_val);
      },
      aliases);
  // List.fold_right store_primitive prims (transl_store ...)
  // (a copy: the [] case assigns transl_store_subst while items still hold
  // the substitution they started from)
  L::IdentPMap<Lam> subst0 = transl_store_subst;
  Lam acc = ctx.transl_store(sc, global_path(glob), subst0, alias_seq, str, 0);
  for (std::size_t k = prims.size(); k-- > 0;) {
    const tt::PrimitiveCoercion* prim = prims[k].second;
    Lam v = translprim::transl_primitive(ScopedLocation{}, prim->pc_desc, prim->pc_env, prim->pc_type, nullptr);
    acc = L::lsequence(setfield(prims[k].first, v), acc);
  }
  return acc;
}

// build_ident_map restr idlist more_ids: [id -> (pos, coercion)], the
// exported primitives and aliases, and the size of the global block
struct IdentMapResult {
  IdentTbl map;
  std::vector<std::pair<long, const tt::PrimitiveCoercion*>> prims;  // OCaml list order: newest first
  std::vector<AliasEntry> aliases;                                   // likewise
  long size;
};
IdentMapResult build_ident_map(const MC* restr, const std::vector<Ident::t>& idlist,
                               const std::vector<Ident::t>& more_ids) {
  IdentMapResult r;
  auto natural_map = [&](long pos, const std::vector<Ident::t>& ids) {
    for (Ident::t id : ids) r.map.add(id, {pos++, tt::tcoerce_none()});
    return pos;
  };
  long pos;
  if (restr->kind == MC::Kind::Tcoerce_none) {
    pos = natural_map(0, idlist);
  } else if (restr->kind == MC::Kind::Tcoerce_structure) {
    // ignore id_pos_list as the ids are already bound
    std::vector<Ident::t> undef = idlist;
    pos = 0;
    for (const tt::PosCoercion& pc : restr->pos_cc) {
      if (pc.cc->kind == MC::Kind::Tcoerce_primitive) {
        r.prims.insert(r.prims.begin(), {pos, pc.cc->prim});
      } else if (pc.cc->kind == MC::Kind::Tcoerce_alias) {
        r.aliases.insert(r.aliases.begin(), AliasEntry{pos, pc.cc->alias_env, pc.cc->alias_path, pc.cc->alias_coercion});
      } else {
        if (pc.pos < 0 || static_cast<std::size_t>(pc.pos) >= idlist.size()) throw std::out_of_range("index out of bounds");
        Ident::t id = idlist[pc.pos];
        r.map.add(id, {pos, pc.cc});
        // Misc.list_remove id undef (structural equality)
        for (auto it = undef.begin(); it != undef.end(); ++it)
          if (ident::same(*it, id)) {
            undef.erase(it);
            break;
          }
      }
      ++pos;
    }
    pos = natural_map(pos, undef);
  } else {
    fatal_error("Translmod.build_ident_map");
  }
  r.size = natural_map(pos, more_ids);
  return r;
}

std::pair<long, Lam> transl_store_gen(scopes sc, std::string_view module_name, const tt::Structure* str,
                                      const MC* restr, bool topl) {
  translobj::reset_labels();
  primitive_declarations.clear();
  translprim::clear_used_primitives();
  Ident::t module_id = Ident::create_persistent(module_name);
  std::vector<Ident::t> more = more_idents(str->str_items);
  std::vector<Ident::t> defined = defined_idents(str->str_items);
  IdentMapResult m = build_ident_map(restr, defined, more);
  auto f = [&]() -> Lam {
    if (topl && str->str_items.size() == 1 &&
        str->str_items[0]->str_desc->kind == tt::StructureItemDesc::Kind::Tstr_eval) {
      if (m.size != 0) fatal_error("Translmod.transl_store_gen: assert false");
      return lambda_subst(transl_store_subst,
                          translcore::transl_exp(sc, tt::as<tt::Tstr_eval>(str->str_items[0]->str_desc)->exp));
    }
    return transl_store_structure(sc, module_id, m.map, m.prims, m.aliases, str->str_items);
  };
  return translobj::transl_store_label_init(module_id, m.size, f);
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

L::Program transl_store_implementation(std::string_view module_name, const tt::Structure* str, const MC* restr) {
  L::IdentPMap<Lam> s = transl_store_subst;
  transl_store_subst = {};
  Ident::t module_ident = Ident::create_persistent(module_name);
  scopes sc = debuginfo::enter_module_definition(debuginfo::empty_scopes, module_ident);
  auto [i, code] = transl_store_gen(sc, module_name, str, restr, false);
  transl_store_subst = s;
  L::Program p;
  p.main_module_block_size = i;
  p.code = code;
  // module_ident is not used by closure, but this allow to share the type
  // with the flambda version
  p.module_ident = module_ident;
  p.required_globals = required_globals(true, code);
  return p;
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

// transl_store_package component_names target_name coercion (the native
// -pack): the block size and the code storing the components
std::pair<long, Lam> transl_store_package(Slice<Ident::t> component_names, Ident::t target_name, const MC* coercion) {
  auto get_component = [](Ident::t id) {
    return id ? L::lprim(pglobal(Primitive::K::Pgetglobal, id), {}, ScopedLocation{}) : L::lconst(L::const_unit());
  };
  auto setfield = [&](long pos, Lam v) {
    Primitive p = L::prim(Primitive::K::Psetfield);
    p.n = pos;
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.init = L::InitializationOrAssignment::Root_initialization;
    return L::lprim(p, slice<Lam>({L::lprim(pglobal(Primitive::K::Pgetglobal, target_name), {}, ScopedLocation{}), v}),
                    ScopedLocation{});
  };
  // make_sequence fn 0 l
  auto make_sequence = [](std::size_t n, const std::function<Lam(long)>& fn) {
    Lam r = L::lambda_unit();
    for (std::size_t k = n; k-- > 0;) r = L::lsequence(fn(static_cast<long>(k)), r);
    return r;
  };
  if (coercion->kind == MC::Kind::Tcoerce_none)
    return {static_cast<long>(component_names.size()),
            make_sequence(component_names.size(),
                          [&](long pos) { return setfield(pos, get_component(component_names[static_cast<std::size_t>(pos)])); })};
  if (coercion->kind != MC::Kind::Tcoerce_structure) fatal_error("Translmod.transl_store_package");
  std::vector<Lam> cs;
  for (Ident::t id : component_names) cs.push_back(get_component(id));
  Lam components = L::lprim(pmakeblock(0, MutableFlag::Immutable), slice(cs), ScopedLocation{});
  Ident::t blk = Ident::create_local("block");
  Lam def = apply_coercion(ScopedLocation{}, LetKind::Strict, coercion, components);
  Lam body = make_sequence(coercion->pos_cc.size(), [&](long pos) {
    return setfield(pos, L::lprim(pfield(pos), slice<Lam>({L::lvar(blk)}), ScopedLocation{}));
  });
  return {static_cast<long>(coercion->pos_cc.size()), L::llet(LetKind::Strict, VK::gen(), blk, def, body)};
}

void reset() {
  primitive_declarations.clear();
  transl_store_subst = {};
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
