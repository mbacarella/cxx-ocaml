// Port of lambda/translclass.ml (TYPECHECKER.md stage 10): the translation
// of class and object expressions.  See translclass.hpp, and translclass.ml's
// header comment for the compilation scheme.
//
// Evaluation order follows OCaml's (a constructor's / list's elements right
// to left, `let ... and ...` left to right) wherever it draws ident stamps
// or shares a constant (Translobj.share allocates a "shared" ident on first
// use): e.g. narrow_args' method lists are shared last to first.
// OCaml's `=` on lambda terms (`obj_init = lambda_unit`, the rebind test)
// is Lambda.equal_lambda.
#include "cppcaml/typing/translclass.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/matching.hpp"
#include "cppcaml/typing/translcore.hpp"
#include "cppcaml/typing/translobj.hpp"
#include "cppcaml/typing/typeopt.hpp"

namespace cppcaml::typing::translclass {

namespace L = ::cppcaml::typing::lambda;
namespace tt = ::cppcaml::typing::typedtree;
using Lam = L::lambda;
using VK = L::ValueKind;
using L::LetKind;
using L::LK;
using L::Primitive;
using L::ScopedLocation;
using L::StructuredConstant;
using debuginfo::scopes;
using RBK = tt::RecursiveBindingKind;
using translobj::oo_prim;

// Clflags.afl_instrument (native only; not in the port's Clflags)
constexpr bool afl_instrument = false;

namespace {

struct NotFound {};  // Not_found
struct Exit {};      // Exit

[[noreturn]] void assert_false(const char* where) { throw std::logic_error(std::string(where) + ": assert false"); }

bool lam_is_unit(Lam l) { return L::equal_lambda(l, L::lambda_unit()); }

Primitive pfield(long n) {
  Primitive p = L::prim(Primitive::K::Pfield);
  p.n = n;
  p.ptr = L::ImmediateOrPointer::Pointer;
  p.mut = MutableFlag::Mutable;
  return p;
}
Primitive pmakeblock_imm() {
  Primitive p = L::prim(Primitive::K::Pmakeblock);
  p.n = 0;
  p.mut = MutableFlag::Immutable;
  return p;
}
Lam makeblock(const std::vector<Lam>& l) { return L::lprim(pmakeblock_imm(), slice(l), ScopedLocation{}); }

// ---- translclass.ml's own constructors -----------------------------------------------------
Lam lfunction(const std::vector<L::Param>& params, Lam body) {
  if (params.empty()) return body;
  if (auto* f = L::as<L::Lfunction>(body)) {
    const L::LFunction* lf = f->f;
    if (lf->kind == L::FunctionKind::Curried && lf->attr.may_fuse_arity &&
        static_cast<long>(params.size() + lf->params.size()) <= L::max_arity()) {
      std::vector<L::Param> ps(params);
      ps.insert(ps.end(), lf->params.begin(), lf->params.end());
      return L::lfunction(L::FunctionKind::Curried, slice(ps), VK::gen(), lf->body, lf->attr, lf->loc);
    }
  }
  return L::lfunction(L::FunctionKind::Curried, slice(params), VK::gen(), body, L::default_function_attribute(),
                      ScopedLocation{});
}
Lam lfunction1(Ident::t id, Lam body) { return lfunction({L::Param{id, VK::gen()}}, body); }

L::LambdaApply apply_record(Lam func, Slice<Lam> args) {
  L::LambdaApply ap;
  ap.ap_loc = ScopedLocation{};
  ap.ap_func = func;
  ap.ap_args = args;
  ap.ap_tailcall = L::TailcallAttribute::Default_tailcall;
  ap.ap_inlined = L::InlineAttribute{};
  ap.ap_specialised = L::SpecialiseAttribute::Default_specialise;
  return ap;
}

Lam lapply_(L::LambdaApply ap) {
  if (auto* a = L::as<L::Lapply>(ap.ap_func)) {
    std::vector<Lam> args(a->ap.ap_args.begin(), a->ap.ap_args.end());
    args.insert(args.end(), ap.ap_args.begin(), ap.ap_args.end());
    ap.ap_func = a->ap.ap_func;
    ap.ap_args = slice(args);
  }
  return L::lapply(ap);
}

Lam mkappl(Lam func, const std::vector<Lam>& args) { return L::lapply(apply_record(func, slice(args))); }

Lam lsequence_(Lam l1, Lam l2) {
  if (lam_is_unit(l2)) return l1;
  return L::lsequence(l1, l2);
}

Lam lfield(Ident::t v, long i) { return L::lprim(pfield(i), slice<Lam>({L::lvar(v)}), ScopedLocation{}); }

const StructuredConstant* const_immstring(std::string_view s) {
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_immstring;
  c->s = zborrow(s);
  return c;
}

Lam transl_label(std::string_view l) { return translobj::share(const_immstring(l)); }

// translclass.ml's "" literals (bind_id_as_val's name, new_variable's
// label): a string literal is one static object, and ocamlopt (which built
// the reference ocamlc.opt) merges equal immutable string constants of a
// compilation unit, so both sites are the same object.
std::string_view static_literal_empty() {
  static std::string_view s = [] {
    ZoneScope perm(permanent_zone());
    return zone().str("");
  }();
  return s;
}

template <class Strs>
Lam transl_meth_list(const Strs& lst) {
  if (lst.empty()) return L::lconst(L::const_int(0));
  std::vector<const StructuredConstant*> fs;
  for (std::string_view lab : lst) fs.push_back(const_immstring(lab));
  auto* c = make<StructuredConstant>();
  c->kind = StructuredConstant::Kind::Const_block;
  c->i = 0;
  c->fields = slice(fs);
  return translobj::share(c);
}

Lam set_inst_var(scopes sc, Ident::t obj, Ident::t id, const tt::Expression* expr) {
  Lam e = translcore::transl_exp(sc, expr);
  Primitive p = L::prim(Primitive::K::Psetfield_computed);
  p.ptr = typeopt::maybe_pointer(expr);
  p.init = L::InitializationOrAssignment::Assignment;
  return L::lprim(p, slice<Lam>({L::lvar(obj), L::lvar(id), e}), ScopedLocation{});
}

Lam transl_val(Ident::t tbl, bool create, std::string_view name) {
  return mkappl(oo_prim(create ? "new_variable" : "get_variable"), {L::lvar(tbl), transl_label(name)});
}

using NameId = std::pair<std::string_view, Ident::t>;

Lam transl_vals(Ident::t tbl, bool create, LetKind strict, const std::vector<NameId>& vals, Lam rem) {
  for (std::size_t k = vals.size(); k-- > 0;)
    rem = L::llet(strict, VK::gen(), vals[k].second, transl_val(tbl, create, vals[k].first), rem);
  return rem;
}

struct MethGet {  // (string * Ident.t * lambda)
  std::string_view nm;
  Ident::t id;
  Lam def;
};
struct Super {  // (instance_vars, method_getters)
  std::vector<NameId> vals;
  std::vector<MethGet> meths;
};

std::vector<MethGet> meths_super(Ident::t tbl, const StrMap<Ident::t>& meths, const std::vector<NameId>& inh_meths) {
  std::vector<MethGet> rem;  // List.fold_right: built from the end, prepending
  for (std::size_t k = inh_meths.size(); k-- > 0;) {
    const auto& [nm, id] = inh_meths[k];
    if (const Ident::t* m = meths.find_opt(nm))
      rem.insert(rem.begin(), MethGet{nm, id, mkappl(oo_prim("get_method"), {L::lvar(tbl), L::lvar(*m)})});
  }
  return rem;
}

Lam bind_super(Ident::t tbl, const Super& sup, Lam cl_init) {
  Lam body = cl_init;
  for (std::size_t k = sup.meths.size(); k-- > 0;)
    body = L::llet(LetKind::StrictOpt, VK::gen(), sup.meths[k].id, sup.meths[k].def, body);
  return transl_vals(tbl, false, LetKind::StrictOpt, sup.vals, body);
}

struct InhEntry {  // (Path.t * lambda * Ident.t)
  Path::t path;
  Lam path_lam;
  Ident::t obj_init;
};
using InhList = std::vector<InhEntry>;  // OCaml list order, head first
struct InhInit {                         // (Ident.t option * inh list)
  Ident::t envs;                         // nullptr = None
  InhList l;
};

struct Created {
  InhInit inh_init;
  Lam obj_init;
  bool has_init;
};

Lam create_object(Ident::t cl, Lam obj, InhInit* inh_out, const std::function<Created(Ident::t)>& init) {
  Ident::t obj2 = Ident::create_local("self");
  Created c = init(obj2);
  *inh_out = std::move(c.inh_init);
  if (lam_is_unit(c.obj_init)) {
    return mkappl(oo_prim(c.has_init ? "create_object_and_run_initializers" : "create_object_opt"),
                  {obj, L::lvar(cl)});
  }
  Lam tail = !c.has_init ? L::lvar(obj2)
                         : mkappl(oo_prim("run_initializers_opt"), {obj, L::lvar(obj2), L::lvar(cl)});
  Lam seq = L::lsequence(c.obj_init, tail);
  return L::llet(LetKind::Strict, VK::gen(), obj2, mkappl(oo_prim("create_object_opt"), {obj, L::lvar(cl)}), seq);
}

Ident::t name_pattern(std::string_view dflt, const tt::Pattern* p) {
  if (auto* v = tt::as<tt::Tpat_var>(p->pat_desc)) return v->id;
  if (auto* a = tt::as<tt::Tpat_alias>(p->pat_desc)) return a->id;
  return Ident::create_local(dflt);
}

using ParamList = std::vector<std::pair<Ident::t, const tt::Expression*>>;  // head first
ParamList append_vals(Slice<tt::IdentExpression> vals, const ParamList& params) {  // vals @ params
  ParamList r;
  for (const tt::IdentExpression& v : vals) r.push_back({v.id, v.exp});
  r.insert(r.end(), params.begin(), params.end());
  return r;
}

// the `build params rem` of the Tcl_fun cases
Lam build_fun(scopes sc, const tt::Pattern* pat, tt::Partial partial, Lam obj_init) {
  std::vector<L::Param> params;
  Lam rem = obj_init;
  if (auto* f = L::as<L::Lfunction>(obj_init); f && f->f->kind == L::FunctionKind::Curried) {
    params.assign(f->f->params.begin(), f->f->params.end());
    rem = f->f->body;
  }
  Ident::t param = name_pattern("param", pat);
  Lam body = matching::for_function(sc, pat->pat_loc, nullptr, L::lvar(param),
                                    slice<matching::PatAction>({matching::PatAction{pat, rem}}), partial);
  params.insert(params.begin(), L::Param{param, VK::gen()});
  return L::lfunction(L::FunctionKind::Curried, slice(params), VK::gen(), body, L::default_function_attribute(),
                      debuginfo::of_location(sc, pat->pat_loc));
}

Lam transl_apply_default(scopes sc, Lam lam, Slice<tt::LabeledArg> args) {
  return translcore::transl_apply(sc, L::TailcallAttribute::Default_tailcall, L::InlineAttribute{},
                                  L::SpecialiseAttribute::Default_specialise, lam, args, ScopedLocation{});
}

using ObjInitFn = std::function<Lam(Ident::t)>;

// [build_object_init] returns an expression that creates and initialises new
// objects (see translclass.ml).
std::pair<InhInit, Lam> build_object_init(scopes sc, Ident::t cl_table, Lam obj, const ParamList& params,
                                          InhInit inh_init, const ObjInitFn& obj_init, const tt::ClassExpr* cl) {
  const tt::ClassExprDesc* d = cl->cl_desc;
  using K = tt::ClassExprDesc::Kind;
  switch (d->kind) {
    case K::Tcl_ident: {
      auto* x = tt::as<tt::Tcl_ident>(d);
      // The object initialiser for the class in [path], specialised
      // to the class being defined
      Ident::t obj_init_id = Ident::create_local("obj_init");
      std::vector<Lam> args;
      if (inh_init.envs)
        args.push_back(L::lprim(pfield(static_cast<long>(inh_init.l.size()) + 1),
                                slice<Lam>({L::lvar(inh_init.envs)}), ScopedLocation{}));
      ScopedLocation loc = debuginfo::of_location(sc, cl->cl_loc);
      Lam path_lam = L::transl_class_path(loc, cl->cl_env, x->path);
      inh_init.l.insert(inh_init.l.begin(), InhEntry{x->path, path_lam, obj_init_id});
      args.push_back(obj);
      return {std::move(inh_init), mkappl(L::lvar(obj_init_id), args)};
    }
    case K::Tcl_structure: {
      const tt::ClassStructure* str = tt::as<tt::Tcl_structure>(d)->cs;
      InhInit out;
      Lam lam = create_object(cl_table, obj, &out, [&](Ident::t objid) -> Created {
        // [obj] will be bound to the allocated object
        InhInit ii = inh_init;
        Lam oi = obj_init(objid);
        bool has_init = false;
        auto& fields = str->cstr_fields;
        for (std::size_t k = fields.size(); k-- > 0;) {  // List.fold_right
          const tt::ClassFieldDesc* fd = fields[k]->cf_desc;
          switch (fd->kind) {
            case tt::ClassFieldDesc::Kind::Tcf_inherit: {
              auto* inh = tt::as<tt::Tcf_inherit>(fd);
              // Reset [params]. The current ones will be bound outside the structure.
              auto [ii2, oi2] = build_object_init(sc, cl_table, L::lvar(objid), {}, std::move(ii),
                                                  [](Ident::t) { return L::lambda_unit(); }, inh->ce);
              ii = std::move(ii2);
              // Since [obj] is bound to a concrete object,
              // only the side-effects of [obj_init'] are relevant.
              oi = lsequence_(oi2, oi);
              has_init = true;
              break;
            }
            case tt::ClassFieldDesc::Kind::Tcf_val: {
              auto* v = tt::as<tt::Tcf_val>(fd);
              if (!v->kind_.is_virtual) oi = lsequence_(set_inst_var(sc, objid, v->id, v->kind_.exp), oi);
              break;
            }
            case tt::ClassFieldDesc::Kind::Tcf_initializer:
              has_init = true;
              break;
            default:
              break;
          }
        }
        // Set the instance variables associated to the class parameters and
        // let bindings to their expected value.
        for (std::size_t k = params.size(); k-- > 0;) {
          Ident::t id = params[k].first;
          oi = lsequence_(L::lifused(id, set_inst_var(sc, objid, id, params[k].second)), oi);
        }
        return Created{std::move(ii), oi, has_init};
      });
      return {std::move(out), lam};
    }
    case K::Tcl_fun: {
      auto* x = tt::as<tt::Tcl_fun>(d);
      // [vals] maps all pattern variables to idents for use inside methods
      auto [ii, oi] =
          build_object_init(sc, cl_table, obj, append_vals(x->args, params), std::move(inh_init), obj_init, x->ce);
      return {std::move(ii), build_fun(sc, x->pat, x->partial, oi)};
    }
    case K::Tcl_apply: {
      auto* x = tt::as<tt::Tcl_apply>(d);
      auto [ii, oi] = build_object_init(sc, cl_table, obj, params, std::move(inh_init), obj_init, x->ce);
      return {std::move(ii), transl_apply_default(sc, oi, x->args)};
    }
    case K::Tcl_let: {
      auto* x = tt::as<tt::Tcl_let>(d);
      auto [ii, oi] =
          build_object_init(sc, cl_table, obj, append_vals(x->vals, params), std::move(inh_init), obj_init, x->ce);
      return {std::move(ii), translcore::transl_let(sc, false, x->rec, x->vbs, oi)};
    }
    case K::Tcl_open:
      // Class local opens are restricted to paths only, so no code is generated
      return build_object_init(sc, cl_table, obj, params, std::move(inh_init), obj_init,
                               tt::as<tt::Tcl_open>(d)->ce);
    case K::Tcl_constraint:
      return build_object_init(sc, cl_table, obj, params, std::move(inh_init), obj_init,
                               tt::as<tt::Tcl_constraint>(d)->ce);
  }
  assert_false("Translclass.build_object_init");
}

using SubstEnvFn = std::function<Lam(Ident::t, const InhList&, Lam)>;

// The manual specifies that toplevel lets *must* be evaluated outside of the
// class. This piece of code makes sure we skip them.
std::pair<InhList, Lam> build_object_init_0(scopes sc, Ident::t cl_table, const ParamList& params,
                                            const tt::ClassExpr* cl, const ObjInitFn& copy_env,
                                            const SubstEnvFn& subst_env, bool top, Slice<Ident::t> ids) {
  if (auto* x = tt::as<tt::Tcl_let>(cl->cl_desc))
    return build_object_init_0(sc, cl_table, append_vals(x->vals, params), x->ce, copy_env, subst_env, top, ids);
  if (auto* x = tt::as<tt::Tcl_open>(cl->cl_desc))
    return build_object_init_0(sc, cl_table, params, x->ce, copy_env, subst_env, top, ids);
  Ident::t self = Ident::create_local("self");
  Ident::t env = Ident::create_local("env");
  Lam obj = ids.empty() ? L::lambda_unit() : L::lvar(self);
  Ident::t envs = top ? nullptr : env;
  auto [ii, obj_init] = build_object_init(sc, cl_table, obj, params, InhInit{envs, {}}, copy_env, cl);
  if (!ids.empty()) obj_init = lfunction1(self, obj_init);
  Lam body = subst_env(env, ii.l, obj_init);
  return {std::move(ii.l), lfunction1(env, body)};
}

Lam bind_method(Ident::t tbl, std::string_view lab, Ident::t id, Lam cl_init) {
  Lam lbl = transl_label(lab);
  return L::llet(LetKind::Strict, VK::gen(), id, mkappl(oo_prim("get_method_label"), {L::lvar(tbl), lbl}), cl_init);
}

Lam bind_methods(Ident::t tbl, const StrMap<Ident::t>& meths, const std::vector<NameId>& vals, Lam cl_init) {
  std::vector<NameId> methl = meths.bindings();  // Meths.fold (prepending): decreasing keys
  std::reverse(methl.begin(), methl.end());
  std::size_t len = methl.size(), nvals = vals.size();
  if (len < 2 && nvals == 0) {
    // Meths.fold (bind_method tbl) meths cl_init: increasing keys, the last outermost
    meths.iter([&](std::string_view lab, Ident::t id) { cl_init = bind_method(tbl, lab, id, cl_init); });
    return cl_init;
  }
  if (len == 0 && nvals < 2) return transl_vals(tbl, true, LetKind::Strict, vals, cl_init);
  Ident::t ids = Ident::create_local("ids");
  long i = static_cast<long>(len + nvals);
  std::string_view getter;
  std::vector<Lam> names;
  if (nvals == 0) {
    getter = "get_method_labels";
  } else {
    getter = "new_methods_variables";
    std::vector<std::string_view> vn;
    for (auto& v : vals) vn.push_back(v.first);
    names.push_back(transl_meth_list(vn));
  }
  // Llet's arguments right to left: the body (fold_right) first
  std::vector<NameId> all(methl);
  all.insert(all.end(), vals.begin(), vals.end());
  Lam body = cl_init;
  for (std::size_t k = all.size(); k-- > 0;) {
    --i;
    body = L::llet(LetKind::StrictOpt, VK::gen(), all[k].second, lfield(ids, i), body);
  }
  std::vector<std::string_view> mn;
  for (auto& m : methl) mn.push_back(m.first);
  std::vector<Lam> args{L::lvar(tbl), transl_meth_list(mn)};
  args.insert(args.end(), names.begin(), names.end());
  return L::llet(LetKind::Strict, VK::gen(), ids, mkappl(oo_prim(getter), args), body);
}

Lam output_methods(Ident::t tbl, const std::vector<Lam>& methods, Lam lam) {
  if (methods.empty()) return lam;
  if (methods.size() == 2) return lsequence_(mkappl(oo_prim("set_method"), {L::lvar(tbl), methods[0], methods[1]}), lam);
  return lsequence_(mkappl(oo_prim("set_methods"), {L::lvar(tbl), makeblock(methods)}), lam);
}

const tt::ClassExpr* ignore_cstrs(const tt::ClassExpr* cl) {
  for (;;) {
    if (auto* x = tt::as<tt::Tcl_constraint>(cl->cl_desc)) {
      cl = x->ce;
    } else if (auto* y = tt::as<tt::Tcl_apply>(cl->cl_desc)) {
      cl = y->ce;
    } else {
      return cl;
    }
  }
}

template <class Strs>
long index(std::string_view a, const Strs& l) {
  long i = 0;
  for (std::string_view b : l) {
    if (b == a) return i;
    ++i;
  }
  throw NotFound{};
}

using MsubstFn = std::function<std::vector<Lam>(bool, Lam)>;

// Build the class initialisation code (see translclass.ml).
std::pair<InhList, Lam> build_class_init(scopes sc, Ident::t cla, bool cstr, const Super& super, InhList inh_init,
                                         Lam cl_init, const MsubstFn& msubst, bool top, const tt::ClassExpr* cl) {
  const tt::ClassExprDesc* d = cl->cl_desc;
  using K = tt::ClassExprDesc::Kind;
  switch (d->kind) {
    case K::Tcl_ident: {
      if (inh_init.empty()) assert_false("Translclass.build_class_init");
      InhEntry e = inh_init.front();
      inh_init.erase(inh_init.begin());
      // The methods and variables for this class are fully registered
      // in the table. If we are in an inheritance context, we can now
      // bind everything.
      Lam body = bind_super(cla, super, cl_init);
      // Load the [class_init] field of the class, and apply it to our
      // current table and the class' environment.
      std::vector<Lam> args{L::lvar(cla)};
      if (top) args.push_back(L::lprim(pfield(2), slice<Lam>({e.path_lam}), ScopedLocation{}));
      Lam f = L::lprim(pfield(1), slice<Lam>({e.path_lam}), ScopedLocation{});
      return {std::move(inh_init), L::llet(LetKind::Strict, VK::gen(), e.obj_init, mkappl(f, args), body)};
    }
    case K::Tcl_structure: {
      const tt::ClassStructure* str = tt::as<tt::Tcl_structure>(d)->cs;
      cl_init = bind_super(cla, super, cl_init);
      std::vector<Lam> methods;
      std::vector<NameId> values;
      auto& fields = str->cstr_fields;
      for (std::size_t k = fields.size(); k-- > 0;) {  // List.fold_right
        const tt::ClassFieldDesc* fd = fields[k]->cf_desc;
        switch (fd->kind) {
          case tt::ClassFieldDesc::Kind::Tcf_inherit: {
            auto* inh = tt::as<tt::Tcf_inherit>(fd);
            cl_init = output_methods(cla, methods, cl_init);
            // Build the initialisation code for the inherited class, plus its
            // wrappers.  Make sure the wrappers bind the inherited methods
            // and variables.
            Super sup;
            std::vector<NameId> inh_meths(inh->meths.begin(), inh->meths.end());
            sup.meths = meths_super(cla, str->cstr_meths, inh_meths);
            sup.vals.assign(inh->vals.begin(), inh->vals.end());
            auto [ii, ci] = build_class_init(sc, cla, false, sup, std::move(inh_init), cl_init, msubst, top, inh->ce);
            inh_init = std::move(ii);
            cl_init = ci;
            methods.clear();
            break;
          }
          case tt::ClassFieldDesc::Kind::Tcf_val: {
            auto* v = tt::as<tt::Tcf_val>(fd);
            // If this is an override, the variable is the same as the one
            // from the earlier definition, and must not be bound again.
            if (!v->inherited) values.insert(values.begin(), NameId{v->name.txt, v->id});
            break;
          }
          case tt::ClassFieldDesc::Kind::Tcf_method: {
            auto* m = tt::as<tt::Tcf_method>(fd);
            if (m->kind_.is_virtual) break;
            scopes sc2 = debuginfo::enter_method_definition(sc, m->name.txt);
            std::vector<Lam> met_code = msubst(true, translcore::transl_scoped_exp(sc2, m->kind_.exp));
            if (clflags::native_code && met_code.size() == 1) {
              // Force correct naming of method for profiles
              Ident::t met = Ident::create_local("method_" + std::string(m->name.txt));
              met_code = {L::llet(LetKind::Strict, VK::gen(), met, met_code[0], L::lvar(met))};
            }
            const Ident::t* mid = str->cstr_meths.find_opt(m->name.txt);
            if (!mid) throw NotFound{};
            std::vector<Lam> nm{L::lvar(*mid)};
            nm.insert(nm.end(), met_code.begin(), met_code.end());
            nm.insert(nm.end(), methods.begin(), methods.end());
            methods = std::move(nm);
            break;
          }
          case tt::ClassFieldDesc::Kind::Tcf_initializer: {
            auto* ini = tt::as<tt::Tcf_initializer>(fd);
            std::vector<Lam> args{L::lvar(cla)};
            std::vector<Lam> ms = msubst(false, translcore::transl_exp(sc, ini->exp));
            args.insert(args.end(), ms.begin(), ms.end());
            cl_init = L::lsequence(mkappl(oo_prim("add_initializer"), args), cl_init);
            break;
          }
          case tt::ClassFieldDesc::Kind::Tcf_constraint:
          case tt::ClassFieldDesc::Kind::Tcf_attribute:
            break;
        }
      }
      // In order of execution at runtime:
      // - Bind the method and variable indices for the current class
      //   ([bind_methods])
      // - Run the code for setting up the individual fields ([cl_init], plus
      //   [output_methods] for the remaining unset methods)
      // - If we are in an inheritance context, bind the inherited variables
      //   and methods for use in the child ([bind_super] at the top of this
      //   branch)
      cl_init = output_methods(cla, methods, cl_init);
      return {std::move(inh_init), bind_methods(cla, str->cstr_meths, values, cl_init)};
    }
    case K::Tcl_fun:
    case K::Tcl_let: {
      Slice<tt::IdentExpression> vals;
      const tt::ClassExpr* ce;
      if (auto* x = tt::as<tt::Tcl_fun>(d)) {
        vals = x->args;
        ce = x->ce;
      } else {
        auto* y = tt::as<tt::Tcl_let>(d);
        vals = y->vals;
        ce = y->ce;
      }
      auto [ii, ci] = build_class_init(sc, cla, cstr, super, std::move(inh_init), cl_init, msubst, top, ce);
      // Create anonymous instance variables and define them in the table
      std::vector<NameId> vs;
      for (const tt::IdentExpression& v : vals) vs.push_back(NameId{static_literal_empty(), v.id});  // bind_id_as_val
      return {std::move(ii), transl_vals(cla, true, LetKind::StrictOpt, vs, ci)};
    }
    case K::Tcl_apply:
      return build_class_init(sc, cla, cstr, super, std::move(inh_init), cl_init, msubst, top,
                              tt::as<tt::Tcl_apply>(d)->ce);
    case K::Tcl_constraint: {
      auto* x = tt::as<tt::Tcl_constraint>(d);
      // MethSet.elements concr_meths (sorted, distinct)
      std::vector<std::string_view> concr(x->concrete_meths.begin(), x->concrete_meths.end());
      std::sort(concr.begin(), concr.end());
      concr.erase(std::unique(concr.begin(), concr.end()), concr.end());
      std::vector<std::string_view> virt_meths;
      for (std::string_view lab : x->meths)
        if (!std::binary_search(concr.begin(), concr.end(), lab)) virt_meths.push_back(lab);
      // [Lvar cla; transl_meth_list vals; ... virt_meths; ... concr_meths]: right to left
      Lam l_concr = transl_meth_list(concr);
      Lam l_virt = transl_meth_list(virt_meths);
      Lam l_vals = transl_meth_list(x->vals);
      std::vector<Lam> narrow_args{L::lvar(cla), l_vals, l_virt, l_concr};
      const tt::ClassExpr* cl2 = ignore_cstrs(x->ce);
      auto* ci = tt::as<tt::Tcl_ident>(cl2->cl_desc);
      if (ci && !inh_init.empty()) {
        InhEntry e = inh_init.front();
        inh_init.erase(inh_init.begin());
        if (!path::same(ci->path, e.path)) assert_false("Translclass.build_class_init");
        Ident::t inh = Ident::create_local("inh");
        long ofs = static_cast<long>(x->vals.size()) + 1;
        const std::vector<NameId>& valids = super.vals;
        const std::vector<MethGet>& methids = super.meths;
        for (const MethGet& m : methids)
          cl_init = L::llet(LetKind::StrictOpt, VK::gen(), m.id, lfield(inh, index(m.nm, concr) + ofs), cl_init);
        for (const NameId& v : valids)
          cl_init = L::llet(LetKind::StrictOpt, VK::gen(), v.second, lfield(inh, index(v.first, x->vals) + 1), cl_init);
        Lam body = L::llet(LetKind::StrictOpt, VK::gen(), e.obj_init, lfield(inh, 0), cl_init);
        std::vector<Lam> args(narrow_args);
        args.push_back(e.path_lam);
        args.push_back(L::lconst(L::const_int(top ? 1 : 0)));
        return {std::move(inh_init), L::llet(LetKind::Strict, VK::gen(), inh, mkappl(oo_prim("inherits"), args), body)};
      }
      auto core = [&](Lam ci0) {
        return build_class_init(sc, cla, true, super, std::move(inh_init), ci0, msubst, top, cl2);
      };
      // Skip narrowing if we're not directly under [inherit]
      if (cstr) return core(cl_init);
      auto [ii, ci2] = core(L::lsequence(mkappl(oo_prim("widen"), {L::lvar(cla)}), cl_init));
      return {std::move(ii), L::lsequence(mkappl(oo_prim("narrow"), narrow_args), ci2)};
    }
    case K::Tcl_open:
      return build_class_init(sc, cla, cstr, super, std::move(inh_init), cl_init, msubst, top,
                              tt::as<tt::Tcl_open>(d)->ce);
  }
  assert_false("Translclass.build_class_init");
}

using LamKind = std::pair<Lam, RBK>;
using WrapFn = std::function<LamKind(LamKind)>;

std::pair<env::t, WrapFn> build_class_lets(scopes sc, const tt::ClassExpr* cl) {
  if (auto* x = tt::as<tt::Tcl_let>(cl->cl_desc)) {
    auto [env, wrap] = build_class_lets(sc, x->ce);
    return {env, [sc, x, wrap = std::move(wrap)](LamKind lk) -> LamKind {
              auto [lam, rkind] = wrap(lk);
              return {translcore::transl_let(sc, false, x->rec, x->vbs, lam), rkind};
            }};
  }
  if (auto* x = tt::as<tt::Tcl_open>(cl->cl_desc)) return build_class_lets(sc, x->ce);
  return {cl->cl_env, [](LamKind lk) { return lk; }};
}

L::IdentSet get_class_meths(const tt::ClassExpr* cl) {
  const tt::ClassExprDesc* d = cl->cl_desc;
  using K = tt::ClassExprDesc::Kind;
  switch (d->kind) {
    case K::Tcl_structure: {
      L::IdentSet s;
      tt::as<tt::Tcl_structure>(d)->cs->cstr_meths.iter([&](std::string_view, Ident::t id) { s.insert(id); });
      return s;
    }
    case K::Tcl_ident:
      return {};
    case K::Tcl_fun:
      return get_class_meths(tt::as<tt::Tcl_fun>(d)->ce);
    case K::Tcl_let:
      return get_class_meths(tt::as<tt::Tcl_let>(d)->ce);
    case K::Tcl_apply:
      return get_class_meths(tt::as<tt::Tcl_apply>(d)->ce);
    case K::Tcl_open:
      return get_class_meths(tt::as<tt::Tcl_open>(d)->ce);
    case K::Tcl_constraint:
      return get_class_meths(tt::as<tt::Tcl_constraint>(d)->ce);
  }
  assert_false("Translclass.get_class_meths");
}

// ---- rebinding a known class -----------------------------------------------------------------
struct Rebind {
  Path::t path;
  Lam path_lam;
  Lam obj_init;
};

Rebind transl_class_rebind_(scopes sc, Lam obj_init, const tt::ClassExpr* cl, VirtualFlag vf) {
  const tt::ClassExprDesc* d = cl->cl_desc;
  using K = tt::ClassExprDesc::Kind;
  switch (d->kind) {
    case K::Tcl_ident: {
      auto* x = tt::as<tt::Tcl_ident>(d);
      if (vf == VirtualFlag::Concrete) {
        try {
          if (!env::find_class(x->path, cl->cl_env)->cty_new) throw Exit{};
        } catch (const env::NotFound&) {
          throw Exit{};
        }
      }
      ScopedLocation cl_loc = debuginfo::of_location(sc, cl->cl_loc);
      Lam path_lam = L::transl_class_path(cl_loc, cl->cl_env, x->path);
      return {x->path, path_lam, obj_init};
    }
    case K::Tcl_fun: {
      auto* x = tt::as<tt::Tcl_fun>(d);
      Rebind r = transl_class_rebind_(sc, obj_init, x->ce, vf);
      r.obj_init = build_fun(sc, x->pat, x->partial, r.obj_init);
      return r;
    }
    case K::Tcl_apply: {
      auto* x = tt::as<tt::Tcl_apply>(d);
      Rebind r = transl_class_rebind_(sc, obj_init, x->ce, vf);
      r.obj_init = transl_apply_default(sc, r.obj_init, x->args);
      return r;
    }
    case K::Tcl_let: {
      auto* x = tt::as<tt::Tcl_let>(d);
      Rebind r = transl_class_rebind_(sc, obj_init, x->ce, vf);
      r.obj_init = translcore::transl_let(sc, false, x->rec, x->vbs, r.obj_init);
      return r;
    }
    case K::Tcl_structure:
      throw Exit{};
    case K::Tcl_constraint: {
      auto* x = tt::as<tt::Tcl_constraint>(d);
      Rebind r = transl_class_rebind_(sc, obj_init, x->ce, vf);
      for (const ClassType* cty = cl->cl_type;;) {  // check_constraint cl.cl_type
        if (cty->kind == ClassType::Kind::Cty_constr && path::same(r.path, cty->path)) break;
        if (cty->kind == ClassType::Kind::Cty_arrow) {
          cty = cty->cty;
          continue;
        }
        throw Exit{};
      }
      return r;
    }
    case K::Tcl_open:
      return transl_class_rebind_(sc, obj_init, tt::as<tt::Tcl_open>(d)->ce, vf);
  }
  assert_false("Translclass.transl_class_rebind");
}

Rebind transl_class_rebind_0(scopes sc, Ident::t self, Lam obj_init, const tt::ClassExpr* cl, VirtualFlag vf) {
  if (auto* x = tt::as<tt::Tcl_let>(cl->cl_desc)) {
    Rebind r = transl_class_rebind_0(sc, self, obj_init, x->ce, vf);
    r.obj_init = translcore::transl_let(sc, false, x->rec, x->vbs, r.obj_init);
    return r;
  }
  Rebind r = transl_class_rebind_(sc, obj_init, cl, vf);
  r.obj_init = lfunction1(self, r.obj_init);
  return r;
}

Lam transl_class_rebind(scopes sc, const tt::ClassExpr* cl, VirtualFlag vf) {
  try {
    Ident::t obj_init = Ident::create_local("obj_init");
    Ident::t self = Ident::create_local("self");
    Lam obj_init0 = lapply_(apply_record(L::lvar(obj_init), slice<Lam>({L::lvar(self)})));
    Rebind r = transl_class_rebind_0(sc, self, obj_init0, cl, vf);
    Lam path_lam = r.path_lam;
    Lam obj_init2 = r.obj_init;
    bool id = L::equal_lambda(obj_init2, lfunction1(self, obj_init0));
    if (id) return path_lam;
    Ident::t cla = Ident::create_local("class");
    Ident::t new_init = Ident::create_local("new_init");
    Ident::t env_init = Ident::create_local("env_init");
    Ident::t table = Ident::create_local("table");
    Ident::t envs = Ident::create_local("envs");
    Lam f_envs = lfunction1(envs, mkappl(L::lvar(new_init), {mkappl(L::lvar(env_init), {L::lvar(envs)})}));
    Lam f_table = lfunction1(
        table, L::llet(LetKind::Strict, VK::gen(), env_init, mkappl(lfield(cla, 1), {L::lvar(table)}), f_envs));
    Lam block = makeblock({mkappl(L::lvar(new_init), {lfield(cla, 0)}), f_table, lfield(cla, 2)});
    return L::llet(LetKind::Strict, VK::gen(), new_init, lfunction1(obj_init, obj_init2),
                   L::llet(LetKind::Alias, VK::gen(), cla, path_lam, block));
  } catch (const Exit&) {
    return L::lambda_unit();
  }
}

// ---- Rewrite a closure using builtins. Improves native code size. --------------------------------
bool mem_id(Ident::t id, const std::vector<Ident::t>& l) {
  for (Ident::t x : l)
    if (ident::same(x, id)) return true;
  return false;
}

bool const_path(const std::vector<Ident::t>& local, Lam l) {
  if (auto* v = L::as<L::Lvar>(l)) return !mem_id(v->id, local);
  if (L::as<L::Lconst>(l)) return true;
  if (auto* f = L::as<L::Lfunction>(l); f && f->f->kind == L::FunctionKind::Curried) {
    L::IdentSet fv = L::free_variables(f->f->body);
    for (Ident::t x : local)
      if (fv.count(x)) return false;
    return true;
  }
  return false;
}

using Builtin = std::pair<std::string, std::vector<Lam>>;

Builtin builtin_meths_(std::vector<Ident::t> self, Ident::t env, Ident::t env2, Lam body) {
  std::vector<Ident::t> local = self;
  local.insert(local.begin(), env);  // env :: self
  auto cp = [&](Lam p) { return const_path(local, p); };
  auto conv = [&](Lam l) -> Builtin {
    // (* Lvar s when List.mem s self ->  "_self", [] *)
    if (cp(l)) return {"const", {l}};
    if (auto* p = L::as<L::Lprim>(l)) {
      if (p->p.kind == Primitive::K::Parrayrefu && p->args.size() == 2) {
        auto* s = L::as<L::Lvar>(p->args[0]);
        auto* n = L::as<L::Lvar>(p->args[1]);
        if (s && n && mem_id(s->id, self)) return {"var", {L::lvar(n->id)}};
      }
      if (p->p.kind == Primitive::K::Pfield && p->args.size() == 1) {
        auto* e = L::as<L::Lvar>(p->args[0]);
        if (e && ident::same(e->id, env)) return {"env", {L::lvar(env2), L::lconst(L::const_int(p->p.n))}};
      }
    }
    if (auto* s = L::as<L::Lsend>(l); s && s->k == L::MethKind::Self && s->args.empty()) {
      auto* o = L::as<L::Lvar>(s->obj);
      if (o && mem_id(o->id, self)) return {"meth", {s->met}};
    }
    throw NotFound{};
  };
  if (auto* x = L::as<L::Llet>(body)) {
    auto* s = L::as<L::Lvar>(x->arg);
    if (s && mem_id(s->id, self)) {
      std::vector<Ident::t> self2 = self;
      self2.insert(self2.begin(), x->id);
      return builtin_meths_(self2, env, env2, x->body);
    }
  }
  if (auto* a = L::as<L::Lapply>(body)) {
    Lam f = a->ap.ap_func;
    auto& args = a->ap.ap_args;
    if (args.size() == 1 && cp(f)) {
      auto [s, as] = conv(args[0]);
      as.insert(as.begin(), f);
      return {"app_" + s, as};
    }
    if (args.size() == 2 && cp(f) && cp(args[1])) {
      auto [s, as] = conv(args[0]);
      as.insert(as.begin(), f);
      as.push_back(args[1]);
      return {"app_" + s + "_const", as};
    }
    if (args.size() == 2 && cp(f) && cp(args[0])) {
      auto [s, as] = conv(args[1]);
      as.insert(as.begin(), args[0]);
      as.insert(as.begin(), f);
      return {"app_const_" + s, as};
    }
  }
  if (auto* sd = L::as<L::Lsend>(body)) {
    auto* obj = L::as<L::Lvar>(sd->obj);
    if (sd->k == L::MethKind::Self && sd->args.size() == 1 && obj && mem_id(obj->id, self)) {
      if (auto* n = L::as<L::Lvar>(sd->met)) {
        auto [s, as] = conv(sd->args[0]);
        as.insert(as.begin(), L::lvar(n->id));
        return {"meth_app_" + s, as};
      }
    }
    if (sd->k == L::MethKind::Self && sd->args.empty() && obj && mem_id(obj->id, self))
      return {"get_meth", {sd->met}};
    if (sd->k == L::MethKind::Public && sd->args.empty()) {
      auto [s, as] = conv(sd->obj);
      as.insert(as.begin(), sd->met);
      return {"send_" + s, as};
    }
    if (sd->k == L::MethKind::Cached && sd->args.size() == 2) {
      auto [s, as] = conv(sd->obj);
      as.insert(as.begin(), sd->met);
      return {"send_" + s, as};
    }
  }
  if (auto* f = L::as<L::Lfunction>(body)) {
    if (f->f->kind == L::FunctionKind::Curried && f->f->params.size() == 1) {
      Ident::t x = f->f->params[0].id;
      std::vector<Ident::t> self2 = self;
      Lam b = f->f->body;
      for (;;) {  // enter
        if (auto* p = L::as<L::Lprim>(b); p && p->p.kind == Primitive::K::Parraysetu && p->args.size() == 3) {
          auto* s = L::as<L::Lvar>(p->args[0]);
          auto* n = L::as<L::Lvar>(p->args[1]);
          auto* x2 = L::as<L::Lvar>(p->args[2]);
          if (s && n && x2 && ident::same(x, x2->id) && mem_id(s->id, self2)) return {"set_var", {L::lvar(n->id)}};
        }
        if (auto* lt = L::as<L::Llet>(b)) {
          auto* s = L::as<L::Lvar>(lt->arg);
          if (s && mem_id(s->id, self2)) {
            self2.insert(self2.begin(), lt->id);
            b = lt->body;
            continue;
          }
        }
        throw NotFound{};
      }
    }
    throw NotFound{};
  }
  auto [s, as] = conv(body);
  return {"get_" + s, as};
}

// module M: the CamlinternalOO.impl constructor numbers
std::vector<Lam> builtin_meths(std::vector<Ident::t> self, Ident::t env, Ident::t env2, Lam body) {
  auto [builtin, args] = builtin_meths_(std::move(self), env, env2, body);
  static const char* const tags[] = {
      "get_const",      "get_var",        "get_env",         "get_meth",       "set_var",
      "app_const",      "app_var",        "app_env",         "app_meth",       "app_const_const",
      "app_const_var",  "app_const_env",  "app_const_meth",  "app_var_const",  "app_env_const",
      "app_meth_const", "meth_app_const", "meth_app_var",    "meth_app_env",   "meth_app_meth",
      "send_const",     "send_var",       "send_env",        "send_meth"};
  long tag = -1;
  for (long k = 0; k < static_cast<long>(std::size(tags)); ++k)
    if (builtin == tags[k]) tag = k;
  if (tag < 0) assert_false("Translclass.builtin_meths");
  args.insert(args.begin(), L::lconst(L::const_int(tag)));
  return args;
}

L::IdentSet free_methods(Lam l) {
  L::IdentSet fv;
  std::function<void(Lam)> free = [&](Lam l) {
    L::iter_head_constructor(free, l);
    switch (l->kind) {
      case LK::Lsend: {
        auto* s = L::as<L::Lsend>(l);
        if (s->k == L::MethKind::Self)
          if (auto* m = L::as<L::Lvar>(s->met)) fv.insert(m->id);
        break;
      }
      case LK::Lfunction:
        for (const L::Param& p : L::as<L::Lfunction>(l)->f->params) fv.erase(p.id);
        break;
      case LK::Llet:
        fv.erase(L::as<L::Llet>(l)->id);
        break;
      case LK::Lmutlet:
        fv.erase(L::as<L::Lmutlet>(l)->id);
        break;
      case LK::Lletrec:
        for (const L::RecBinding& b : L::as<L::Lletrec>(l)->decl) fv.erase(b.id);
        break;
      case LK::Lstaticcatch:
        for (const L::Param& p : L::as<L::Lstaticcatch>(l)->params) fv.erase(p.id);
        break;
      case LK::Ltrywith:
        fv.erase(L::as<L::Ltrywith>(l)->exn);
        break;
      case LK::Lfor:
        fv.erase(L::as<L::Lfor>(l)->id);
        break;
      default:
        break;
    }
  };
  free(l);
  return fv;
}

// ---- Class translation ---------------------------------------------------------------------
LamKind transl_class_(scopes sc, Slice<Ident::t> ids, Ident::t cl_id, Slice<std::string_view> pub_meths_in,
                      const tt::ClassExpr* cl, VirtualFlag vflag) {
  // First check if it is not only a rebind
  Lam rebind = transl_class_rebind(sc, cl, vflag);
  if (!lam_is_unit(rebind)) return {rebind, RBK::Dynamic};

  // Prepare for heavy environment handling
  sc = debuginfo::enter_class_definition(sc, cl_id);
  Ident::t tables = Ident::create_local(std::string(ident::name(cl_id)) + "_tables");
  auto [top_env, req] = translobj::oo_add_class(tables);
  bool top = !req;
  // The manual specifies that toplevel lets *must* be evaluated outside of the class
  auto [cl_env, llets] = build_class_lets(sc, cl);
  std::vector<Ident::t> new_ids = top ? std::vector<Ident::t>{} : env::diff(top_env, cl_env);
  Ident::t env2 = Ident::create_local("env");
  L::IdentSet meth_ids = get_class_meths(cl);
  auto subst = [&](Ident::t env, Lam lam, long i0, std::vector<Ident::t>& new_ids2) {
    L::IdentSet fv = L::free_variables(lam);
    for (Ident::t id : new_ids2) fv.erase(id);
    // We need to handle method ids specially, as they do not appear
    // in the typing environment (PR#3576, PR#4560)
    // very hacky: we add and remove free method ids on the fly,
    // depending on the visit order...
    L::IdentSet u = free_methods(lam);
    u.insert(translobj::method_ids.begin(), translobj::method_ids.end());
    L::IdentSet diff;
    for (Ident::t id : u)
      if (!meth_ids.count(id)) diff.insert(id);
    translobj::method_ids = diff;
    L::IdentSet nids = translobj::method_ids;
    nids.insert(new_ids.begin(), new_ids.end());
    L::IdentSet inter;
    for (Ident::t id : fv)
      if (nids.count(id)) inter.insert(id);
    new_ids2.insert(new_ids2.end(), inter.begin(), inter.end());
    long i = i0 - 1;
    L::IdentMap<Lam> s;
    for (Ident::t id : new_ids2) {
      ++i;
      s[id] = lfield(env, i);
    }
    return s;
  };
  std::vector<Ident::t> new_ids_meths;
  L::UpdateEnv no_env_update = [](Ident::t, const ValueDescription*, env::t env) { return env; };
  MsubstFn msubst = [&](bool arr, Lam lam) -> std::vector<Lam> {
    auto* f = L::as<L::Lfunction>(lam);
    if (!f || f->f->kind != L::FunctionKind::Curried || f->f->params.empty() ||
        !L::equal_value_kind(f->f->params[0].kind, VK::gen()))
      assert_false("Translclass.msubst");
    Ident::t self = f->f->params[0].id;
    std::vector<L::Param> args(f->f->params.begin() + 1, f->f->params.end());
    Lam body = f->f->body;
    Ident::t env = Ident::create_local("env");
    Lam body2 = new_ids.empty() ? body : L::subst(no_env_update, false, subst(env, body, 0, new_ids_meths), body);
    try {
      // Doesn't seem to improve size for bytecode
      // if not !Clflags.native_code then raise Not_found;
      if (!arr || clflags::debug) throw NotFound{};
      return builtin_meths({self}, env, env2, lfunction(args, body2));
    } catch (const NotFound&) {
      std::vector<L::Param> ps{L::Param{self, VK::gen()}};
      ps.insert(ps.end(), args.begin(), args.end());
      Lam b = body2;
      if (L::free_variables(body2).count(env)) {
        Primitive pfc = L::prim(Primitive::K::Pfield_computed);
        b = L::llet(LetKind::Alias, VK::gen(), env,
                    L::lprim(pfc, slice<Lam>({L::lvar(self), L::lvar(env2)}), ScopedLocation{}), body2);
      }
      return {lfunction(ps, b)};
    }
  };
  std::vector<Ident::t> new_ids_init;
  Ident::t env1 = Ident::create_local("env");
  Ident::t env1p = Ident::create_local("env'");
  ObjInitFn copy_env = [&](Ident::t self) -> Lam {
    if (top) return L::lambda_unit();
    Primitive p = L::prim(Primitive::K::Psetfield_computed);
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.init = L::InitializationOrAssignment::Assignment;
    return L::lifused(env2,
                      L::lprim(p, slice<Lam>({L::lvar(self), L::lvar(env2), L::lvar(env1p)}), ScopedLocation{}));
  };
  SubstEnvFn subst_env = [&](Ident::t envs, const InhList& l, Lam lam) -> Lam {
    if (top) return lam;
    // must be called only once!
    lam = L::subst(no_env_update, false, subst(env1, lam, 1, new_ids_init), lam);
    Lam inner = L::llet(LetKind::Alias, VK::gen(), env1p, new_ids_init.empty() ? L::lvar(env1) : lfield(env1, 0), lam);
    return L::llet(LetKind::Alias, VK::gen(), env1, l.empty() ? L::lvar(envs) : lfield(envs, 0), inner);
  };

  // Now we start compiling the class
  Ident::t cla = Ident::create_local("class");
  auto [inh_init, obj_init] = build_object_init_0(sc, cla, {}, cl, copy_env, subst_env, top, ids);
  InhList inh_init_r(inh_init.rbegin(), inh_init.rend());
  auto [inh_left, cl_init] = build_class_init(sc, cla, true, Super{}, std::move(inh_init_r), obj_init, msubst, top, cl);
  if (!inh_left.empty()) assert_false("Translclass.transl_class");
  Ident::t table = Ident::create_local("table");
  Ident::t class_init = Ident::create_local(std::string(ident::name(cl_id)) + "_init");
  Ident::t env_init = Ident::create_local("env_init");
  Ident::t obj_init_id = Ident::create_local("obj_init");
  // Sort methods by hash
  std::vector<std::string_view> pub_meths(pub_meths_in.begin(), pub_meths_in.end());
  std::stable_sort(pub_meths.begin(), pub_meths.end(), [](std::string_view s, std::string_view s2) {
    return btype::hash_variant(s) < btype::hash_variant(s2);
  });
  // Check for hash conflicts
  std::vector<long> tags;
  for (std::string_view s : pub_meths) tags.push_back(btype::hash_variant(s));
  for (std::size_t k = 0; k < tags.size(); ++k) {
    std::string_view name2;
    for (std::size_t j = 0; j < tags.size(); ++j)  // List.assoc tag rev_map
      if (tags[j] == tags[k]) {
        name2 = pub_meths[j];
        break;
      }
    if (name2 != pub_meths[k]) throw Error(cl->cl_loc, std::string(pub_meths[k]), std::string(name2));
  }
  auto ltable = [&](Ident::t table, Lam lam) {
    return L::llet(LetKind::Strict, VK::gen(), table, mkappl(oo_prim("create_table"), {transl_meth_list(pub_meths)}),
                   lam);
  };
  auto ldirect = [&](Ident::t obj_init) {
    return L::llet(LetKind::Strict, VK::gen(), obj_init, cl_init,
                   L::lsequence(mkappl(oo_prim("init_class"), {L::lvar(cla)}),
                                mkappl(L::lvar(obj_init), {L::lambda_unit()})));
  };
  // Simplest case: an object defined at toplevel (ids=[])
  if (top && ids.empty()) return llets({ltable(cla, ldirect(obj_init_id)), RBK::Dynamic});

  bool concrete = vflag == VirtualFlag::Concrete;
  auto lclass = [&](const std::function<LamKind(const L::IdentSet&)>& mk_lam_and_kind) -> LamKind {
    Lam cl_init2 =
        llets({L::lfunction(L::FunctionKind::Curried, slice<L::Param>({L::Param{cla, VK::gen()}}), VK::gen(),
                            cl_init, L::default_function_attribute(), ScopedLocation{}),
               RBK::Dynamic /* Placeholder, real kind is computed in [lbody] below */})
            .first;
    auto [lam, rkind] = mk_lam_and_kind(L::free_variables(cl_init2));
    return {L::llet(LetKind::Strict, VK::gen(), class_init, cl_init2, lam), rkind};
  };
  auto lbody = [&](const L::IdentSet& fv) -> LamKind {
    bool nonrec = true;
    for (Ident::t id : ids)
      if (fv.count(id)) nonrec = false;
    if (nonrec)
      // Not recursive: can use make_class directly
      return {mkappl(oo_prim("make_class"), {transl_meth_list(pub_meths), L::lvar(class_init)}), RBK::Dynamic};
    // Recursive: need to have an actual allocation for let rec compilation
    // to work, so hardcode make_class
    Lam blk = makeblock({mkappl(L::lvar(env_init), {L::lambda_unit()}), L::lvar(class_init), L::lambda_unit()});
    Lam seq = L::lsequence(mkappl(oo_prim("init_class"), {L::lvar(table)}), blk);
    return {ltable(table, L::llet(LetKind::Strict, VK::gen(), env_init,
                                  mkappl(L::lvar(class_init), {L::lvar(table)}), seq)),
            RBK::Static};
  };
  auto lbody_virt = [&](Lam lenvs) -> LamKind {
    // Virtual classes only need to provide the [class_init] and [env]
    // fields. [obj_init] is filled with a dummy [lambda_unit] value.
    Lam f = L::lfunction(L::FunctionKind::Curried, slice<L::Param>({L::Param{cla, VK::gen()}}), VK::gen(), cl_init,
                         L::default_function_attribute(), ScopedLocation{});
    return {makeblock({L::lambda_unit(), f, lenvs}), RBK::Static};
  };
  // Still easy: a class defined at toplevel
  if (top && concrete) return lclass(lbody);
  if (top) return llets(lbody_virt(L::lambda_unit()));

  // Now for the hard stuff: prepare for table caching
  Ident::t envs = Ident::create_local("envs");
  Ident::t cached = Ident::create_local("cached");
  Lam lenvs = new_ids_meths.empty() && new_ids_init.empty() && inh_init.empty() ? L::lambda_unit() : L::lvar(envs);
  Lam lenv;
  {
    Lam menv;
    if (new_ids_meths.empty()) {
      menv = L::lambda_unit();
    } else {
      std::vector<Lam> vs;
      for (Ident::t id : new_ids_meths) vs.push_back(L::lvar(id));
      menv = makeblock(vs);
    }
    if (new_ids_init.empty()) {
      lenv = menv;
    } else {
      std::vector<Lam> vs{menv};
      for (Ident::t id : new_ids_init) vs.push_back(L::lvar(id));
      lenv = makeblock(vs);
    }
  }
  std::vector<Lam> linh_envs;
  for (auto it = inh_init.rbegin(); it != inh_init.rend(); ++it)
    linh_envs.push_back(L::lprim(pfield(2), slice<Lam>({it->path_lam}), ScopedLocation{}));
  auto make_envs = [&](LamKind lk) -> LamKind {
    Lam e = lenv;
    if (!linh_envs.empty()) {
      std::vector<Lam> vs{lenv};
      vs.insert(vs.end(), linh_envs.begin(), linh_envs.end());
      e = makeblock(vs);
    }
    return {L::llet(LetKind::StrictOpt, VK::gen(), envs, e, lk.first), lk.second};
  };
  auto def_ids = [&](Ident::t cla, Lam lam) {
    Lam lbl = transl_label(static_literal_empty());
    return L::llet(LetKind::StrictOpt, VK::gen(), env2, mkappl(oo_prim("new_variable"), {L::lvar(cla), lbl}), lam);
  };
  std::vector<Lam> inh_keys;
  for (const InhEntry& e : inh_init)  // inh_paths
    if (mem_id(path::head(e.path), new_ids))
      inh_keys.push_back(L::lprim(pfield(1), slice<Lam>({e.path_lam}), ScopedLocation{}));
  auto lclass2 = [&](Lam lam) {
    Lam f = L::lfunction(L::FunctionKind::Curried, slice<L::Param>({L::Param{cla, VK::gen()}}), VK::gen(),
                         def_ids(cla, cl_init), L::default_function_attribute(), ScopedLocation{});
    return L::llet(LetKind::Strict, VK::gen(), class_init, f, lam);
  };
  auto lset = [&](Ident::t cached, long i, Lam lam) {
    Primitive p = L::prim(Primitive::K::Psetfield);
    p.n = i;
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.init = L::InitializationOrAssignment::Assignment;
    return L::lprim(p, slice<Lam>({L::lvar(cached), lam}), ScopedLocation{});
  };
  auto ldirect2 = [&]() {
    Lam seq = L::lsequence(mkappl(oo_prim("init_class"), {L::lvar(cla)}), lset(cached, 0, L::lvar(env_init)));
    return ltable(cla, L::llet(LetKind::Strict, VK::gen(), env_init, def_ids(cla, cl_init), seq));
  };
  auto lclass_virt = [&]() {
    return lset(cached, 0,
                L::lfunction(L::FunctionKind::Curried, slice<L::Param>({L::Param{cla, VK::gen()}}), VK::gen(),
                             def_ids(cla, cl_init), L::default_function_attribute(), ScopedLocation{}));
  };
  Lam lupdate_cache;
  if (ids.empty()) {
    lupdate_cache = ldirect2();
  } else if (!concrete) {
    lupdate_cache = lclass_virt();
  } else {
    Lam a = mkappl(oo_prim("make_class_store"), {transl_meth_list(pub_meths), L::lvar(class_init), L::lvar(cached)});
    lupdate_cache = lclass2(a);
  }
  Lam lcheck_cache;
  if (clflags::native_code && afl_instrument) {
    // When afl-fuzz instrumentation is enabled, ignore the cache
    // so that the program's behaviour does not change between runs
    lcheck_cache = lupdate_cache;
  } else {
    lcheck_cache = L::lifthenelse(lfield(cached, 0), L::lambda_unit(), lupdate_cache);
  }
  auto lcache = [&](LamKind lk) -> LamKind {
    Lam lam = L::lsequence(lcheck_cache, lk.first);
    if (inh_keys.empty()) {
      lam = L::llet(LetKind::Alias, VK::gen(), cached, L::lvar(tables), lam);
    } else {
      lam = L::llet(LetKind::Strict, VK::gen(), cached,
                    mkappl(oo_prim("lookup_tables"), {L::lvar(tables), makeblock(inh_keys)}), lam);
    }
    return {lam, lk.second};
  };
  LamKind inner;
  if (ids.empty()) {
    inner = {mkappl(lfield(cached, 0), {lenvs}), RBK::Dynamic};
  } else if (concrete) {
    inner = {makeblock({mkappl(lfield(cached, 0), {lenvs}), lfield(cached, 1), lenvs}), RBK::Static};
  } else {
    inner = {makeblock({L::lambda_unit(), lfield(cached, 0), lenvs}), RBK::Static};
  }
  return llets(lcache(make_envs(inner)));
}

}  // namespace

// Wrapper for class compilation
LamKind transl_class(scopes sc, Slice<Ident::t> ids, Ident::t cl_id, Slice<std::string_view> pub_meths,
                     const tt::ClassExpr* cl, VirtualFlag vflag) {
  return translobj::oo_wrap_gen(cl->cl_env, false, [&] { return transl_class_(sc, ids, cl_id, pub_meths, cl, vflag); });
}

}  // namespace cppcaml::typing::translclass
