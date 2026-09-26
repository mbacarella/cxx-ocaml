// Port of typing/typecore.ml, part 3: collecting the arguments of function
// applications, the generalization criterion (is_nonexpansive), type
// approximations for recursive definitions, and the checks and helpers used
// by type_expect ("final_subexpression" to "type_exp").
#include <algorithm>

#include "ast_helper.hpp"
#include "cppcaml/typing/value_rec_check.hpp"
#include "typecore_exp.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using PK = tt::PatternDesc::Kind;
using XK = tt::ExpressionDesc::Kind;
using SXK = pt::ExpressionDesc::Kind;
using SPK = pt::PatternDesc::Kind;
namespace ah = ast_helper;

const tt::Expression* final_subexpression(const tt::Expression* exp) {
  for (;;) {
    const tt::ExpressionDesc* d = exp->exp_desc;
    if (auto* l = as<tt::Texp_let>(d)) exp = l->body;
    else if (auto* s = as<tt::Texp_sequence>(d)) exp = s->e2;
    else if (auto* t = as<tt::Texp_try>(d)) exp = t->exp;
    else if (auto* i = as<tt::Texp_ifthenelse>(d)) exp = i->then_;
    else if (auto* m = as<tt::Texp_match>(d); m && !m->comp_cases.empty()) exp = m->comp_cases[0]->c_rhs;
    else if (auto* si = as<tt::Texp_struct_item>(d)) exp = si->body;
    else return exp;
  }
}

bool is_prim(std::string_view name, const tt::Expression* funct) {
  if (auto* i = as<tt::Texp_ident>(funct->exp_desc))
    return i->vd->val_kind.kind == ValueKind::Kind::Val_prim && i->vd->val_kind.prim->prim_name == name;
  return false;
}

// ---- collecting arguments for function applications ------------------------------------
static TypeExpr* remaining_function_type_for_error(TypeExpr* ty_ret, const std::vector<UntypedArg>& rev_args) {
  for (auto& a : rev_args) {
    bool elim = !a.omitted && a.arg.kind == UntypedApplyArg::Kind::Eliminated_optional_arg;
    if (a.omitted || elim)
      ty_ret = newty2(a.arg.level, tarrow(a.label, a.arg.ty_arg, ty_ret, commu_ok()));
  }
  return ty_ret;
}

// [rev_args] is the arguments typed until now, in reverse order of
// appearance.  Not all arguments have a location attached.
static Location previous_arg_loc(const std::vector<UntypedArg>& rev_args, const tt::Expression* funct) {
  for (auto& a : rev_args) {
    if (a.omitted) continue;
    using UK = UntypedApplyArg::Kind;
    if (a.arg.kind == UK::Known_arg || a.arg.kind == UK::Unknown_arg) return a.arg.sarg->pexp_loc;
    if (a.arg.kind == UK::Typed_arg) return a.arg.targ->exp_loc;
  }
  return funct->exp_loc;
}

static Location beginning_function_loc(const std::vector<UntypedArg>& rev_args, const tt::Expression* funct) {
  Location prev = previous_arg_loc(rev_args, funct);
  return Location{funct->exp_loc.loc_start, prev.loc_end, prev.loc_ghost && funct->exp_loc.loc_ghost};
}

[[noreturn]] static void dependent_app_error(env::t env, const ctype::FilterArrowFailure& err_,
                                             const std::vector<UntypedArg>& rev_args,
                                             const tt::Expression* funct, const pt::Expression* sarg,
                                             const Package* pack, const Package* pack0) {
  // check that the type of the argument is indeed a package
  TypeExpr* t0 = newgenty(tpackage(pack0));
  TypeExpr* t = newgenty(tpackage(pack));
  type_argument_forward(env, sarg, t, t0, std::nullopt);
  // if it is a package of the expected type then we say that could not
  // extract a path from it
  Location loc = beginning_function_loc(rev_args, funct);
  if (err_.kind != ctype::FilterArrowFailure::Kind::Unification_error)
    throw std::logic_error("dependent_app_error");
  Error e = err(loc, env, EK::Cannot_unify_tfunctor_to_tarrow);
  e.trace = err_.err;
  raise_error(e);
}

// Given the module expression [M] and package type [(module S with cstrs)],
// this returns the module expression [(M : S with cstrs)].
static const pt::ModuleExpr* module_with_package_type_constraint(const pt::ModuleExpr* me,
                                                                 const pt::PackageType* optyp) {
  if (!optyp) return me;
  const Location& loc = optyp->ppt_loc;
  auto* path = make<pt::ModuleType>(
      make<pt::Pmty_ident>(pt::Pmty_ident{{pt::ModuleTypeDesc::Kind::Pmty_ident}, optyp->ppt_path}), loc,
      pt::Attributes{});
  std::vector<const pt::WithConstraint*> cstrs;
  for (auto& [lid, t] : optyp->ppt_constraints) {
    // Ast_helper.Type.mk ~loc ~manifest:t (Location.map Longident.last lid)
    pt::TypeKind k{};
    k.kind = pt::TypeKind::Kind::Ptype_abstract;
    auto* td = make<pt::TypeDeclaration>(pt::StrLoc{longident::last(lid.txt), lid.loc}, Slice<pt::TypeParam>{},
                                         Slice<pt::TypeConstraintDecl>{}, k, PrivateFlag::Public, t,
                                         pt::Attributes{}, loc);
    pt::WithConstraint* w = make<pt::WithConstraint>(pt::WithConstraint::Kind::Pwith_type);
    w->lid = lid;
    w->decl = td;
    cstrs.push_back(w);
  }
  auto* mty = make<pt::ModuleType>(
      make<pt::Pmty_with>(pt::Pmty_with{{pt::ModuleTypeDesc::Kind::Pmty_with}, path, slice(cstrs)}), loc,
      pt::Attributes{});
  return make<pt::ModuleExpr>(
      make<pt::Pmod_constraint>(pt::Pmod_constraint{{pt::ModuleExprDesc::Kind::Pmod_constraint}, me, mty}), loc,
      optyp->ppt_attrs);
}

static const pt::Pexp_pack* extract_packing(const pt::Expression* sarg) {
  return as<pt::Pexp_pack>(sarg->pexp_desc);
}

static UntypedArg collect_arrow_arg(const ArgLabel& l, const tt::Expression*, bool optional,
                                    const std::vector<std::pair<ArgLabel, const pt::Expression*>>& sargs,
                                    TypeExpr* ty_arg, TypeExpr* ty_arg0, long lv,
                                    const std::optional<std::pair<const pt::Expression*, ArgLabel>>& arg_opt) {
  // (the -principal warnings are not emitted)
  using UK = UntypedApplyArg::Kind;
  if (arg_opt) {
    bool wrapped_in_some = optional && !is_optional(arg_opt->second);
    UntypedApplyArg a{UK::Known_arg, arg_opt->first, ty_arg, ty_arg0, wrapped_in_some};
    return UntypedArg{l, false, a};
  }
  bool has_nolabel = false;
  for (auto& s : sargs)
    if (s.first.kind == ArgLabel::Kind::Nolabel) has_nolabel = true;
  if (optional && has_nolabel) {
    UntypedApplyArg a{UK::Eliminated_optional_arg, nullptr, ty_arg};
    a.level = lv;
    return UntypedArg{l, false, a};
  }
  // No argument was given for this parameter, we abstract over it.
  UntypedApplyArg a{UK::Eliminated_optional_arg, nullptr, ty_arg};
  a.level = lv;
  return UntypedArg{l, true, a};
}

static std::pair<const tt::ModuleExpr*, const tt::Expression*> type_tfunctor_module_arg(
    env::t env, const pt::Expression* sarg, const pt::ModuleExpr* me0, const pt::PackageType* optyp,
    const Package* pack, const Package* pack0) {
  const pt::ModuleExpr* me = module_with_package_type_constraint(me0, optyp);
  // We expanded the code here to prevent a principality warning because the
  // expected signature is not closed.
  auto [modl, pack2] = type_package(env, me, pack);
  tt::Expression* texp = mkexp(make<tt::Texp_pack>(tt::Texp_pack{{XK::Texp_pack}, modl}), ctype::newty(tpackage(pack2)),
                               sarg->pexp_loc, env);
  texp->exp_attributes = sarg->pexp_attributes;
  unify_exp(sarg, env, texp, ctype::newty(tpackage(pack0)));
  return {modl, texp};
}

struct FunctorArg {
  UntypedArg arg;
  TypeExpr* ty_ret;
  TypeExpr* ty_ret0;
};

static FunctorArg collect_functor_module_arg(env::t env, const pt::Expression* sarg,
                                             const std::vector<UntypedArg>& rev_args,
                                             const tt::Expression* funct, const pt::ModuleExpr* me,
                                             const pt::PackageType* optyp, const ctype::Tfunctor_& tfun,
                                             const ctype::Tfunctor_& tfun0, const ArgLabel& l) {
  auto [modl, texp] = type_tfunctor_module_arg(env, sarg, me, optyp, tfun.pack, tfun0.pack);
  UntypedApplyArg a{UntypedApplyArg::Kind::Typed_arg};
  a.targ = texp;
  UntypedArg arg{l, false, a};
  if (Path::t path = tt::path_of_module(modl)) {
    TypeExpr* ty_ret = ctype::with_level(generic_level, [&] {
      return ctype::instance_funct(Ident::of_unscoped(tfun.id_us), path, false, tfun.ty);
    });
    TypeExpr* ty_ret0 = ctype::instance_funct(Ident::of_unscoped(tfun0.id_us), path, false, tfun0.ty);
    return {arg, ty_ret, ty_ret0};
  }
  const tt::ModuleExpr* me2 = tt::remove_module_constraint(modl);
  try {
    TypeExpr* ty = ctype::instance_funct_nondep(env, l, tfun, me2->mod_type);
    TypeExpr* ty0 = ctype::instance_funct_nondep(env, l, tfun0, me2->mod_type);
    return {arg, ty, ty0};
  } catch (const ctype::Unify& u) {
    Error e = err(beginning_function_loc(rev_args, funct), env, EK::Cannot_unify_tfunctor_to_tarrow);
    e.trace = u.err;
    raise_error(e);
  }
}

static CollectedArgs collect_unknown_apply_args(env::t env, const tt::Expression* funct, TypeExpr* ty_fun0,
                                                std::vector<UntypedArg> rev_args,
                                                std::vector<std::pair<ArgLabel, const pt::Expression*>> sargs) {
  auto labels_match = [](const ArgLabel& param, const ArgLabel& arg) {
    return param == arg || (clflags::classic && arg.kind == ArgLabel::Kind::Nolabel && !is_optional(param));
  };
  auto has_label = [&](const ArgLabel& l, TypeExpr* ty_fun) {
    auto [ls, is_ret_tvar] = ctype::arrow_labels(env, ty_fun);
    return is_ret_tvar || std::find(ls.begin(), ls.end(), l) != ls.end();
  };
  TypeExpr* ty_fun = ty_fun0;
  for (auto& [lbl, sarg] : sargs) {
    // (-typing-recovery is never on in batch ocamlc)
    TypeExpr* tf = ctype::expand_head(env, ty_fun);
    const TypeDesc* d = get_desc(tf);
    enum { Arrow, Functor } arg_kind;
    TypeExpr* ty_arg = nullptr;
    TypeExpr* ty_res = nullptr;
    ArgLabel fl;
    ident::Unscoped* fid = nullptr;
    const Package* fpack = nullptr;
    auto* a = as<Tarrow>(d);
    auto* fu = as<Tfunctor>(d);
    if (d->kind == DescKind::Tvar) {
      TypeExpr* ta = ctype::newvar();
      TypeExpr* ty_param = ctype::newmono(ta);
      TypeExpr* tr = ctype::newvar();
      // (Ignored_extra_argument warning not emitted)
      ctype::unify(env, tf, ctype::newty(tarrow(lbl, ty_param, tr, commu_var())));
      arg_kind = Arrow;
      ty_arg = ta;
      ty_res = tr;
    } else if (a && labels_match(a->label, lbl)) {
      arg_kind = Arrow;
      ty_arg = tpoly_get_mono(a->t1);
      ty_res = a->t2;
    } else if (fu && labels_match(fu->label, lbl)) {
      arg_kind = Functor;
      fl = fu->label;
      fid = fu->id;
      fpack = fu->pack;
      ty_res = fu->body;
    } else {
      TypeExpr* tyf = (a || fu) ? ctype::newty(d) : tf;
      TypeExpr* tr = remaining_function_type_for_error(tyf, rev_args);
      DescKind rk = get_desc(tr)->kind;
      if (rk == DescKind::Tarrow || rk == DescKind::Tfunctor) {
        if (clflags::classic || !has_label(lbl, tyf)) {
          Error e = err(sarg->pexp_loc, env, EK::Apply_wrong_label);
          e.label = lbl;
          e.ty = tr;
          e.flag = false;
          raise_error(e);
        }
        raise_error(err(funct->exp_loc, env, EK::Incoherent_label_order));
      }
      Error e = err(funct->exp_loc, env, EK::Apply_non_function);
      e.texp = funct;
      e.ty = ctype::expand_head(env, funct->exp_type);
      e.ty2 = ctype::expand_head(env, tr);
      e.loc2 = previous_arg_loc(rev_args, funct);
      e.loc3 = sarg->pexp_loc;
      raise_error(e);
    }
    UntypedApplyArg arg{};
    if (arg_kind == Arrow) {
      arg = UntypedApplyArg{UntypedApplyArg::Kind::Unknown_arg, sarg, ty_arg};
    } else if (const pt::Pexp_pack* pk = extract_packing(sarg)) {
      auto [modl, texp] = type_tfunctor_module_arg(env, sarg, pk->me, pk->pack, fpack, fpack);
      arg = UntypedApplyArg{UntypedApplyArg::Kind::Typed_arg};
      arg.targ = texp;
      if (Path::t path = tt::path_of_module(modl)) {
        ty_res = ctype::instance_funct(Ident::of_unscoped(fid), path, false, ty_res);
      } else {
        const tt::ModuleExpr* me = tt::remove_module_constraint(modl);
        try {
          ty_res = ctype::instance_funct_nondep(env, fl, ctype::Tfunctor_{fid, fpack, ty_res}, me->mod_type);
        } catch (const ctype::Unify& u) {
          Error e = err(beginning_function_loc(rev_args, funct), env, EK::Cannot_unify_tfunctor_to_tarrow);
          e.trace = u.err;
          raise_error(e);
        }
      }
    } else {
      auto r = ctype::filter_arrow(env, true, tf, fl, false);
      if (!r.ok) dependent_app_error(env, r.error, rev_args, funct, sarg, fpack, fpack);
      arg = UntypedApplyArg{UntypedApplyArg::Kind::Unknown_arg, sarg, r.value.ty_param};
      ty_res = r.value.ty_ret;
    }
    rev_args.insert(rev_args.begin(), UntypedArg{lbl, false, arg});
    ty_fun = ty_res;
  }
  std::reverse(rev_args.begin(), rev_args.end());
  return {ty_fun, rev_args};
}

CollectedArgs collect_apply_args(env::t env, const tt::Expression* funct, bool ignore_labels,
                                 TypeExpr* ty_fun, TypeExpr* ty_fun0,
                                 const std::vector<std::pair<ArgLabel, const pt::Expression*>>& sargs0) {
  TypeSet visited;
  std::vector<UntypedArg> rev_args;
  std::vector<std::pair<ArgLabel, const pt::Expression*>> sargs = sargs0;
  for (;;) {
    if (sargs.empty()) return collect_unknown_apply_args(env, funct, ty_fun0, rev_args, sargs);
    TypeExpr* ty_fun2 = ctype::expand_head(env, ty_fun);
    long lv = get_level(ty_fun2);
    // (the -principal warnings of may_warn are not emitted)
    TypeExpr* e0 = ctype::expand_head(env, ty_fun0);
    const TypeDesc* d = get_desc(ty_fun2);
    const TypeDesc* d0 = get_desc(e0);
    auto* a = as<Tarrow>(d);
    auto* a0 = as<Tarrow>(d0);
    auto* f = as<Tfunctor>(d);
    auto* f0 = as<Tfunctor>(d0);
    bool is_arrow = a && a0 && is_commu_ok(a->commu);
    bool is_functor = f && f0;
    if (!is_arrow && !is_functor)
      // We're not looking at a *known* function type anymore.
      return collect_unknown_apply_args(env, funct, ty_fun0, rev_args, sargs);
    ArgLabel l = is_arrow ? a->label : f->label;
    std::string_view name = label_name(l);
    bool optional = is_optional(l);
    std::vector<std::pair<ArgLabel, const pt::Expression*>> remaining_sargs;
    std::optional<std::pair<const pt::Expression*, ArgLabel>> arg_opt;
    bool terminate = false;
    TypeSet new_visited;
    if (ignore_labels) {
      // No reordering is allowed, process arguments in order
      auto [l2, sarg] = sargs[0];
      std::vector<std::pair<ArgLabel, const pt::Expression*>> rest(sargs.begin() + 1, sargs.end());
      if (name == label_name(l2) || (!optional && l2.kind == ArgLabel::Kind::Nolabel)) {
        remaining_sargs = rest;
        arg_opt = std::make_pair(sarg, l2);
      } else {
        bool in_rest = false;
        for (auto& r : rest)
          if (name == label_name(r.first)) in_rest = true;
        bool has_nolabel = false;
        for (auto& s : sargs)
          if (s.first.kind == ArgLabel::Kind::Nolabel) has_nolabel = true;
        if (optional && !in_rest && has_nolabel) {
          remaining_sargs = sargs;
          if (visited.mem(ty_fun)) {
            new_visited = visited;
            terminate = true;
          } else {
            new_visited = visited;
            new_visited.add(ty_fun);
          }
        } else {
          Error e = err(sarg->pexp_loc, env, EK::Apply_wrong_label);
          e.label = l2;
          e.ty = ty_fun2;
          e.flag = optional;
          raise_error(e);
        }
      }
    } else {
      // Arguments can be commuted, try to fetch the argument corresponding
      // to the first parameter.
      auto x = extract_label(name, sargs);
      if (x) {
        auto& [l2, sarg, commuted, rem] = *x;
        (void)commuted;  // (the -principal and Nonoptional_label warnings are not emitted)
        remaining_sargs = rem;
        arg_opt = std::make_pair(sarg, l2);
      } else {
        remaining_sargs = sargs;
        new_visited = visited;
        if (visited.mem(ty_fun)) terminate = true;
        else new_visited.add(ty_fun);
      }
    }
    visited = new_visited;
    if (terminate) return collect_unknown_apply_args(env, funct, ty_fun0, rev_args, remaining_sargs);
    if (is_arrow) {
      UntypedArg arg = collect_arrow_arg(l, funct, optional, sargs, a->t1, a0->t1, lv, arg_opt);
      rev_args.insert(rev_args.begin(), arg);
      ty_fun = a->t2;
      ty_fun0 = a0->t2;
      sargs = remaining_sargs;
      continue;
    }
    ctype::Tfunctor_ tfun{f->id, f->pack, f->body};
    ctype::Tfunctor_ tfun0{f0->id, f0->pack, f0->body};
    // (the "applying a dependent function" -principal warning is not emitted)
    FunctorArg fa;
    const pt::Pexp_pack* packing = arg_opt ? extract_packing(arg_opt->first) : nullptr;
    if (arg_opt && packing) {
      fa = collect_functor_module_arg(env, arg_opt->first, rev_args, funct, packing->me, packing->pack, tfun,
                                      tfun0, l);
    } else {
      // (filter_arrow .. ty_fun', filter_arrow .. ty_fun0): right to left
      auto r0 = ctype::filter_arrow(env, true, ty_fun0, l, false);
      auto r = ctype::filter_arrow(env, true, ty_fun2, l, false);
      if (r.ok && r0.ok) {
        UntypedArg arg = collect_arrow_arg(l, funct, optional, sargs, r.value.ty_param, r0.value.ty_param, lv,
                                           arg_opt);
        fa = FunctorArg{arg, r.value.ty_ret, r0.value.ty_ret};
      } else {
        const ctype::FilterArrowFailure& er = !r.ok ? r.error : r0.error;
        if (arg_opt) dependent_app_error(env, er, rev_args, funct, arg_opt->first, tfun.pack, tfun0.pack);
        Error e = err(beginning_function_loc(rev_args, funct), env, EK::Cannot_omit_tfunctor_argument);
        e.us = tfun.id_us;
        e.ty = remaining_function_type_for_error(ty_fun2, rev_args);
        raise_error(e);
      }
    }
    rev_args.insert(rev_args.begin(), fa.arg);
    ty_fun = fa.ty_ret;
    ty_fun0 = fa.ty_ret0;
    sargs = remaining_sargs;
  }
}

std::pair<TypeExpr*, std::vector<UntypedArg>> type_omitted_parameters_and_build_result_type(
    TypeExpr* ty_ret, const std::vector<UntypedArg>& args) {
  std::vector<UntypedArg> out;  // head first
  for (auto it = args.rbegin(); it != args.rend(); ++it) {
    if (!it->omitted) {
      out.insert(out.begin(), *it);
    } else {
      ty_ret = newty2(it->arg.level, tarrow(it->label, it->arg.ty_arg, ty_ret, commu_ok()));
      out.insert(out.begin(), *it);
    }
  }
  return {ty_ret, out};
}

// ---- generalization criterion for expressions ----------------------------------------------
static bool is_nonexpansive_mod(const tt::ModuleExpr* mexp);
static bool is_nonexpansive_opt(const tt::Expression* e) { return !e || is_nonexpansive(e); }
static bool is_nonexpansive_arg(const tt::ApplyArg& a) { return a.omitted || is_nonexpansive(a.arg); }

static bool is_nonexpansive_struct_item(const tt::StructureItem* item) {
  using TK = tt::StructureItemDesc::Kind;
  const tt::StructureItemDesc* d = item->str_desc;
  switch (d->kind) {
    case TK::Tstr_eval:
    case TK::Tstr_primitive:
    case TK::Tstr_type:
    case TK::Tstr_modtype:
    case TK::Tstr_class_type: return true;
    case TK::Tstr_value:
      for (auto* vb : as<tt::Tstr_value>(d)->vbs)
        if (!is_nonexpansive(vb->vb_expr)) return false;
      return true;
    case TK::Tstr_module: return is_nonexpansive_mod(as<tt::Tstr_module>(d)->mb->mb_expr);
    case TK::Tstr_open: return is_nonexpansive_mod(as<tt::Tstr_open>(d)->od->open_expr);
    case TK::Tstr_include: return is_nonexpansive_mod(as<tt::Tstr_include>(d)->incl->incl_mod);
    case TK::Tstr_recmodule:
      for (auto* mb : as<tt::Tstr_recmodule>(d)->mbs)
        if (!is_nonexpansive_mod(mb->mb_expr)) return false;
      return true;
    case TK::Tstr_exception:
      // Text_decl: true would be unsound
      return as<tt::Tstr_exception>(d)->exn->tyexn_constructor->ext_kind.kind ==
             tt::TExtensionConstructorKind::Kind::Text_rebind;
    case TK::Tstr_typext:
      for (auto* c : as<tt::Tstr_typext>(d)->ext->tyext_constructors)
        if (c->ext_kind.kind == tt::TExtensionConstructorKind::Kind::Text_decl) return false;
      return true;
    case TK::Tstr_class: return false;  // could be more precise
    case TK::Tstr_attribute: return true;
  }
  return false;
}

static bool is_nonexpansive_mod(const tt::ModuleExpr* mexp) {
  using MK = tt::ModuleExprDesc::Kind;
  const tt::ModuleExprDesc* d = mexp->mod_desc;
  switch (d->kind) {
    case MK::Tmod_ident:
    case MK::Tmod_functor: return true;
    case MK::Tmod_unpack: return is_nonexpansive(as<tt::Tmod_unpack>(d)->exp);
    case MK::Tmod_constraint: return is_nonexpansive_mod(as<tt::Tmod_constraint>(d)->me);
    case MK::Tmod_structure:
      for (auto* it : as<tt::Tmod_structure>(d)->str->str_items)
        if (!is_nonexpansive_struct_item(it)) return false;
      return true;
    case MK::Tmod_apply:
    case MK::Tmod_apply_unit: return false;
  }
  return false;
}

bool is_nonexpansive(const tt::Expression* exp) {
  const tt::ExpressionDesc* d = exp->exp_desc;
  switch (d->kind) {
    case XK::Texp_ident:
    case XK::Texp_constant:
    case XK::Texp_unreachable:
    case XK::Texp_function: return true;
    case XK::Texp_array: return as<tt::Texp_array>(d)->el.empty();
    case XK::Texp_let: {
      auto* l = as<tt::Texp_let>(d);
      for (auto* vb : l->vbs)
        if (!is_nonexpansive(vb->vb_expr)) return false;
      return is_nonexpansive(l->body);
    }
    case XK::Texp_apply: {
      auto* a = as<tt::Texp_apply>(d);
      if (!a->args.empty() && a->args[0].arg.omitted) {
        if (!is_nonexpansive(a->fn)) return false;
        for (std::size_t k = 1; k < a->args.size(); ++k)
          if (!is_nonexpansive_arg(a->args[k].arg)) return false;
        return true;
      }
      if (auto* i = as<tt::Texp_ident>(a->fn->exp_desc); i && i->vd->val_kind.kind == ValueKind::Kind::Val_prim) {
        std::string_view n = i->vd->val_kind.prim->prim_name;
        if ((n == "%raise" || n == "%reraise" || n == "%raise_notrace" || n == "%identity") &&
            a->args.size() == 1 && a->args[0].label.kind == ArgLabel::Kind::Nolabel && !a->args[0].arg.omitted)
          return is_nonexpansive(a->args[0].arg.arg);
        return false;
      }
      return false;
    }
    case XK::Texp_match: {
      auto* m = as<tt::Texp_match>(d);
      // Not sure this is necessary (see typecore.ml)
      auto contains_exception_pat = [](const tt::Pattern* p) {
        return tt::exists_general_pattern([](const tt::Pattern* q) { return q->pat_desc->kind == PK::Tpat_exception; }, p);
      };
      if (!is_nonexpansive(m->exp)) return false;
      for (auto* c : m->comp_cases)
        if (!(is_nonexpansive_opt(c->c_guard) && is_nonexpansive(c->c_rhs) && !contains_exception_pat(c->c_lhs)))
          return false;
      return true;
    }
    case XK::Texp_tuple:
      for (auto& x : as<tt::Texp_tuple>(d)->el)
        if (!is_nonexpansive(x.exp)) return false;
      return true;
    case XK::Texp_construct:
      for (auto* e : as<tt::Texp_construct>(d)->args)
        if (!is_nonexpansive(e)) return false;
      return true;
    case XK::Texp_variant: return is_nonexpansive_opt(as<tt::Texp_variant>(d)->arg);
    case XK::Texp_record: {
      auto* r = as<tt::Texp_record>(d);
      for (auto& f : r->fields)
        if (!f.def.kept && !(f.label->lbl_mut == MutableFlag::Immutable && is_nonexpansive(f.def.exp))) return false;
      return is_nonexpansive_opt(r->extended_expression);
    }
    case XK::Texp_atomic_loc: return is_nonexpansive(as<tt::Texp_atomic_loc>(d)->exp);
    case XK::Texp_field: return is_nonexpansive(as<tt::Texp_field>(d)->exp);
    case XK::Texp_ifthenelse: {
      auto* i = as<tt::Texp_ifthenelse>(d);
      return is_nonexpansive(i->then_) && is_nonexpansive_opt(i->else_);
    }
    case XK::Texp_sequence: return is_nonexpansive(as<tt::Texp_sequence>(d)->e2);  // PR#4354
    case XK::Texp_new: return class_type_arity(as<tt::Texp_new>(d)->decl->cty_type) > 0;
    // Note: nonexpansive only means no _observable_ side effects
    case XK::Texp_lazy: return is_nonexpansive(as<tt::Texp_lazy>(d)->exp);
    case XK::Texp_object: {
      auto* o = as<tt::Texp_object>(d);
      long count = 0;
      for (auto* f : o->cs->cstr_fields) {
        using CK = tt::ClassFieldDesc::Kind;
        const tt::ClassFieldDesc* fd = f->cf_desc;
        bool ok = true;
        switch (fd->kind) {
          case CK::Tcf_method: break;
          case CK::Tcf_val: {
            auto* v = as<tt::Tcf_val>(fd);
            count++;
            if (!v->kind_.is_virtual) ok = is_nonexpansive(v->kind_.exp);
            break;
          }
          case CK::Tcf_initializer: ok = is_nonexpansive(as<tt::Tcf_initializer>(fd)->exp); break;
          case CK::Tcf_constraint: break;
          case CK::Tcf_inherit: ok = false; break;
          case CK::Tcf_attribute: break;
        }
        if (!ok) return false;
      }
      bool b = true;
      o->cs->cstr_type->csig_vars.iter([&](std::string_view, const VarEntry& e) {
        count--;
        b = b && e.mut == MutableFlag::Immutable;
      });
      return b && count == 0;
    }
    case XK::Texp_pack: return is_nonexpansive_mod(as<tt::Texp_pack>(d)->me);
    // Computations which raise exceptions are nonexpansive (see GPR#1142)
    case XK::Texp_assert: return is_nonexpansive(as<tt::Texp_assert>(d)->exp);
    case XK::Texp_struct_item: {
      auto* si = as<tt::Texp_struct_item>(d);
      return is_nonexpansive_struct_item(si->item) && is_nonexpansive(si->body);
    }
    default: return false;
  }
}

bool maybe_expansive(const tt::Expression* e) { return !is_nonexpansive(e); }

Slice<const tt::ValueBinding*> annotate_recursive_bindings(env::t env, Slice<const tt::ValueBinding*> valbinds) {
  std::vector<Ident::t> ids = tt::let_bound_idents(valbinds);
  std::vector<const tt::ValueBinding*> out;
  for (auto* vb : valbinds) {
    auto k = value_rec_check::is_valid_recursive_expression(ids, vb->vb_expr);
    if (!k) raise_error(err(vb->vb_expr->exp_loc, env, EK::Illegal_letrec_expr));  // log_or_raise
    tt::ValueBinding* v = make<tt::ValueBinding>(*vb);
    v->vb_rec_kind = *k;
    out.push_back(v);
  }
  return slice(out);
}

// The "rest of the function" extends from the start of the first parameter
// to the end of the overall function (see typecore.ml).
Location loc_rest_of_function(const Location& loc_function, bool first,
                              Slice<const pt::FunctionParam*> params_suffix, const pt::FunctionBody* body) {
  if (!params_suffix.empty()) {
    if (first) return loc_function;
    return Location{params_suffix[0]->pparam_loc.loc_start, loc_function.loc_end, true};
  }
  if (body->kind == pt::FunctionBody::Kind::Pfunction_body) return body->body->pexp_loc;
  return body->loc;
}

// ---- approximate the type of an expression, for better recursion --------------------------
TypeExpr* approx_type(env::t env, const pt::CoreType* sty) {
  const pt::CoreTypeDesc* d = sty->ptyp_desc;
  if (auto* a = as<pt::Ptyp_arrow>(d)) {
    if (a->t1->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly) {
      if (is_optional(a->label)) return ctype::newvar();
      // Polymorphic types will only unify with types that match all of
      // their polymorphic parts, so we need to fully translate the type here
      const tt::CoreType* arg_ty = typetexp::transl_simple_type(env, nullptr, false, a->t1);
      TypeExpr* r = approx_type(env, a->t2);
      return ctype::newty(tarrow(a->label, arg_ty->ctyp_type, r, commu_ok()));
    }
    TypeExpr* ty1 = is_optional(a->label) ? type_option(ctype::newvar()) : ctype::newvar();
    // (Tarrow (p, newmono ty1, approx_type env sty, commu_ok)): right to left
    TypeExpr* r = approx_type(env, a->t2);
    TypeExpr* m = ctype::newmono(ty1);
    return ctype::newty(tarrow(a->label, m, r, commu_ok()));
  }
  if (auto* t = as<pt::Ptyp_tuple>(d)) {
    std::vector<LabeledTy> l;
    for (auto& x : t->tl) l.push_back({x.label, approx_type(env, x.ty)});
    return ctype::newty(ttuple(slice(l)));
  }
  if (auto* c = as<pt::Ptyp_constr>(d)) {
    auto [path, decl] = env::lookup_type(false, c->lid.loc, c->lid.txt, env);
    if (static_cast<long>(c->args.size()) != decl->type_arity) return ctype::newvar();
    std::vector<TypeExpr*> tyl;
    for (auto* a : c->args) tyl.push_back(approx_type(env, a));
    return ctype::newconstr(path, slice(tyl));
  }
  return ctype::newvar();
}

static void type_pattern_approx(env::t env, const pt::Pattern* spat, TypeExpr* ty_expected) {
  if (auto* c = as<pt::Ppat_constraint>(spat->ppat_desc)) {
    TypeExpr* inferred_ty;
    if (c->ty->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly)
      inferred_ty = typetexp::transl_simple_type(env, nullptr, false, c->ty)->ctyp_type;
    else
      inferred_ty = approx_type(env, c->ty);
    unify_pat_types(spat->ppat_loc, env, inferred_ty, ty_expected);
  }
}

static TypeExpr* type_approx_fun_one_param(env::t env, const ArgLabel& label, const pt::Expression* dflt,
                                           const pt::Pattern* spato, TypeExpr* ty_expected, bool first,
                                           const std::pair<Location, TypeExpr*>& in_function) {
  // [spato] is [None] when approximating a [Pfunction_cases], the
  // parameter is implicit in that case.
  bool has_poly = spato ? check_poly_constraint(spato, env, label) : false;
  auto r = ctype::filter_arrow(env, false, ty_expected, label, has_poly);
  if (!r.ok) {
    // (Error.log_or_raise: batch ocamlc raises)
    raise_error(error_of_filter_arrow_failure(in_function.first, env, std::nullopt, first, in_function.second,
                                              r.error));
  }
  TypeExpr* ty_param = r.value.ty_param;
  if (spato) {
    if (label.kind == ArgLabel::Kind::Optional) {
      if (!dflt) {
        TypeExpr* var = ctype::newmono(type_option(ctype::newvar()));
        unify_pat_types(spato->ppat_loc, env, ty_param, var);
      } else {
        TypeExpr* ty_opt_param = ctype::newvar();
        TypeExpr* ty_pat_param = ctype::newmono(type_option(ty_opt_param));
        unify_pat_types(spato->ppat_loc, env, ty_param, ty_pat_param);
        ty_param = ctype::newmono(ty_opt_param);
      }
    }
    if (!(has_poly || !tpoly_is_mono(ty_param))) ty_param = tpoly_get_mono(ty_param);
    type_pattern_approx(env, spato, ty_param);
  }
  return r.value.ty_ret;
}

static TypeExpr* type_approx_constraint(env::t env, const pt::TypeConstraint& c, const Location& loc,
                                        TypeExpr* ty_expected) {
  if (c.kind == pt::TypeConstraint::Kind::Pconstraint) {
    TypeExpr* ty_constrain = approx_type(env, c.ty);
    try {
      ctype::unify(env, ty_constrain, ty_expected);
    } catch (const ctype::Unify& u) {
      Error e = err(loc, env, EK::Expr_type_clash);
      e.trace = u.err;
      raise_error(e);
    }
    return ty_constrain;
  }
  TypeExpr* ty_constrain = c.from ? approx_type(env, c.from) : ctype::newvar();
  TypeExpr* ty_coerce = approx_type(env, c.ty);
  try {
    ctype::unify(env, ty_coerce, ty_expected);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Expr_type_clash);
    e.trace = u.err;
    raise_error(e);
  }
  return ty_constrain;
}

bool is_unpack(const pt::Pattern* pat) {
  auto* u = as<pt::Ppat_unpack>(pat->ppat_desc);
  return u && u->name.txt.some;
}

static void type_approx_function(env::t env, Slice<const pt::FunctionParam*> params, const pt::TypeConstraint* c,
                                 const pt::FunctionBody* body, TypeExpr* ty_expected,
                                 const std::pair<Location, TypeExpr*>& in_function, bool first);

static void type_tuple_approx(env::t env, const Location& loc, TypeExpr* ty_expected,
                              Slice<pt::LabeledExpression> l) {
  std::vector<LabeledTy> labeled_tys;
  for (auto& x : l) labeled_tys.push_back({x.label, ctype::newvar()});
  TypeExpr* ty = ctype::newty(ttuple(slice(labeled_tys)));
  try {
    ctype::unify(env, ty, ty_expected);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Expr_type_clash);
    e.trace = u.err;
    raise_error(e);
  }
  for (std::size_t k = 0; k < l.size(); ++k) type_approx(env, l[k].exp, labeled_tys[k].ty);
}

void type_approx(env::t env, const pt::Expression* sexp, TypeExpr* ty_expected) {
  const Location& loc = sexp->pexp_loc;
  const pt::ExpressionDesc* d = sexp->pexp_desc;
  switch (d->kind) {
    case SXK::Pexp_let: type_approx(env, as<pt::Pexp_let>(d)->body, ty_expected); return;
    case SXK::Pexp_function: {
      auto* f = as<pt::Pexp_function>(d);
      type_approx_function(env, f->params, f->constraint, f->body, ty_expected, {loc, ty_expected}, true);
      return;
    }
    case SXK::Pexp_match: {
      auto* m = as<pt::Pexp_match>(d);
      if (!m->cases.empty()) type_approx(env, m->cases[0]->pc_rhs, ty_expected);
      return;
    }
    case SXK::Pexp_try: type_approx(env, as<pt::Pexp_try>(d)->exp, ty_expected); return;
    case SXK::Pexp_tuple: type_tuple_approx(env, sexp->pexp_loc, ty_expected, as<pt::Pexp_tuple>(d)->el); return;
    case SXK::Pexp_ifthenelse: type_approx(env, as<pt::Pexp_ifthenelse>(d)->then_, ty_expected); return;
    case SXK::Pexp_sequence: type_approx(env, as<pt::Pexp_sequence>(d)->e2, ty_expected); return;
    case SXK::Pexp_constraint: {
      auto* c = as<pt::Pexp_constraint>(d);
      pt::TypeConstraint tc{pt::TypeConstraint::Kind::Pconstraint, c->ty};
      TypeExpr* t = type_approx_constraint(env, tc, loc, ty_expected);
      type_approx(env, c->exp, t);
      return;
    }
    case SXK::Pexp_coerce: {
      auto* c = as<pt::Pexp_coerce>(d);
      pt::TypeConstraint tc{pt::TypeConstraint::Kind::Pcoerce, c->to, c->from};
      type_approx_constraint(env, tc, loc, ty_expected);
      return;
    }
    case SXK::Pexp_pack: {
      auto* p = as<pt::Pexp_pack>(d);
      if (!p->pack) return;
      pt::TypeConstraint tc{pt::TypeConstraint::Kind::Pconstraint, ah::typ_package(loc, p->pack)};
      type_approx_constraint(env, tc, loc, ty_expected);
      return;
    }
    default: return;
  }
}

static void type_approx_function(env::t env, Slice<const pt::FunctionParam*> params, const pt::TypeConstraint* c,
                                 const pt::FunctionBody* body, TypeExpr* ty_expected,
                                 const std::pair<Location, TypeExpr*>& in_function, bool first) {
  Location loc = loc_rest_of_function(in_function.first, first, params, body);
  // We can approximate types up to the first newtype parameter or potential
  // dependent module argument, whereupon we give up.
  if (!params.empty()) {
    const pt::FunctionParamDesc& pd = params[0]->pparam_desc;
    if (pd.kind == pt::FunctionParamDesc::Kind::Pparam_newtype) return;
    if (!pd.default_ && is_unpack(pd.pat) && !is_optional(pd.label)) return;
    TypeExpr* ty_res = type_approx_fun_one_param(env, pd.label, pd.default_, pd.pat, ty_expected, first, in_function);
    Slice<const pt::FunctionParam*> rest(params.begin() + 1, params.size() - 1);
    type_approx_function(env, rest, c, body, ty_res, in_function, false);
    return;
  }
  // In the [Pconstraint] case, we override the [ty_expected] that gets
  // passed to the approximating of the rest of the type.
  TypeExpr* te = c ? type_approx_constraint(env, *c, loc, ty_expected) : ty_expected;
  if (body->kind == pt::FunctionBody::Kind::Pfunction_body) {
    type_approx(env, body->body, te);
  } else if (!body->cases.empty()) {
    TypeExpr* ty_res = type_approx_fun_one_param(env, ArgLabel::nolabel(), nullptr, nullptr, te, first, in_function);
    type_approx(env, body->cases[0]->pc_rhs, ty_res);
  }
  // Pfunction_cases []: not reachable
}

// ---- checks --------------------------------------------------------------------------------
// Check that all univars are safe in a type.  Both exp.exp_type and
// ty_expected should already be generalized.
void check_univars(env::t env, std::string_view kind, const tt::Expression* exp, TypeExpr* ty_expected,
                   const std::vector<TypeExpr*>& vars0) {
  TypeExpr* pty = ctype::instance(ty_expected);
  auto [exp_ty, vars] = ctype::with_local_level_generalize([&] {
    auto* p = as<Tpoly>(get_desc(pty));
    if (!p) throw std::logic_error("check_univars");
    // Enforce scoping for type_let (see typecore.ml)
    auto [_, ty2] = ctype::instance_poly_fixed(p->vars, p->body, true);
    auto [vs, ety] = ctype::instance_parameterized_type(slice(vars0), exp->exp_type);
    unify_exp_types(exp->exp_loc, env, ety, ty2);
    return std::make_pair(ety, vs);
  });
  auto [ty, errs] = ctype::polyfy(env, exp_ty, vars);
  if (!errs.empty()) {
    TypeExpr* te = ctype::instance(ty_expected);
    auto diff = ctype::expanded_diff(env, ty, te);
    auto explanation = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::Univar);
    explanation.univar.is_var_mismatch = false;
    explanation.univar.quantification = errs;
    Error e = err(exp->exp_loc, env, EK::Less_general);
    e.name = std::string(kind);
    e.trace = et::UnificationError{{diff, explanation}};
    raise_error(e);
  }
}

// [check_statement] implements the [non-unit-statement] warning (not
// emitted); its type expansion is kept.
void check_statement(const tt::Expression* exp) { ctype::expand_head(exp->exp_env, exp->exp_type); }

// [check_partial_application] implements the [ignored-partial-application]
// warning (not emitted): the type expansions it performs, and the delayed
// check it schedules, are kept.
void check_partial_application(bool statement, const tt::Expression* exp) {
  auto check_statement_ = [=] {
    if (statement) check_statement(exp);
  };
  auto doit = [=] {
    const TypeDesc* ty = get_desc(ctype::expand_head(exp->exp_env, exp->exp_type));
    if (ty->kind == DescKind::Tarrow || ty->kind == DescKind::Tfunctor) {
      std::function<void(const tt::Expression*)> check = [&](const tt::Expression* e) {
        for (auto& x : e->exp_extra)
          if (x.extra.kind == tt::ExpExtra::Kind::Texp_constraint) {
            check_statement_();
            return;
          }
        const tt::ExpressionDesc* d = e->exp_desc;
        switch (d->kind) {
          case XK::Texp_match: {
            auto* m = as<tt::Texp_match>(d);
            for (auto* c : m->comp_cases) check(c->c_rhs);
            for (auto* c : m->eff_cases) check(c->c_rhs);
            return;
          }
          case XK::Texp_try: {
            auto* t = as<tt::Texp_try>(d);
            check(t->exp);
            for (auto* c : t->exn_cases) check(c->c_rhs);
            for (auto* c : t->eff_cases) check(c->c_rhs);
            return;
          }
          case XK::Texp_ifthenelse: {
            auto* i = as<tt::Texp_ifthenelse>(d);
            if (!i->else_) {
              check_statement_();
              return;
            }
            check(i->then_);
            check(i->else_);
            return;
          }
          case XK::Texp_let: check(as<tt::Texp_let>(d)->body); return;
          case XK::Texp_sequence: check(as<tt::Texp_sequence>(d)->e2); return;
          case XK::Texp_struct_item: check(as<tt::Texp_struct_item>(d)->body); return;
          case XK::Texp_apply:
          case XK::Texp_send:
          case XK::Texp_new:
          case XK::Texp_letop: return;  // (Ignored_partial_application warning)
          default: check_statement_(); return;
        }
      };
      check(exp);
    } else {
      check_statement_();
    }
  };
  const TypeDesc* ty = get_desc(ctype::expand_head(exp->exp_env, exp->exp_type));
  if (ty->kind == DescKind::Tvar) {
    // The type of [exp] is not known.  Delay the check until after
    // typechecking.
    add_delayed_check(doit);
  } else {
    doit();
  }
}

bool pattern_needs_partial_application_check(const tt::Pattern* p) {
  for (auto& x : p->pat_extra)
    if (x.extra.kind == tt::PatExtra::Kind::Tpat_constraint) return false;
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any:
    case PK::Tpat_exception: return true;
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      return pattern_needs_partial_application_check(o->p1) && pattern_needs_partial_application_check(o->p2);
    }
    case PK::Tpat_value: return pattern_needs_partial_application_check(as<tt::Tpat_value>(d)->pat);
    default: return false;
  }
}

// Check that a type is generalizable at some level
bool generalizable(long level, TypeExpr* ty0) {
  bool r = true;
  with_type_mark([&](TypeMark& mark) {
    struct Exit {};
    std::function<void(TypeExpr*)> check = [&](TypeExpr* ty) {
      if (try_mark_node(mark, ty)) {
        if (get_level(ty) <= level) throw Exit{};
        iter_type_expr(check, ty);
      }
    };
    try {
      check(ty0);
    } catch (const Exit&) {
      r = false;
    }
  });
  return r;
}

// Hack to allow coercion of self.  Will clean-up later.
std::vector<std::pair<Path::t, std::shared_ptr<std::vector<Location>>>> self_coercion;

// Helpers for type_cases
bool contains_variant_either(TypeExpr* ty0) {
  bool r = false;
  with_type_mark([&](TypeMark& mark) {
    struct Exit {};
    std::function<void(TypeExpr*)> loop = [&](TypeExpr* ty) {
      if (!try_mark_node(mark, ty)) return;
      if (auto* v = as<Tvariant>(get_desc(ty))) {
        if (!is_fixed(v->row))
          for (auto& e : row_fields(v->row))
            if (row_field_repr(e.field).kind == RowFieldView::Kind::Reither) throw Exit{};
        iter_row(loop, v->row);
      } else {
        iter_type_expr(loop, ty);
      }
    };
    try {
      loop(ty0);
    } catch (const Exit&) {
      r = true;
    }
  });
  return r;
}

static void shallow_iter_ppat(const std::function<void(const pt::Pattern*)>& f, const pt::Pattern* p) {
  const pt::PatternDesc* d = p->ppat_desc;
  switch (d->kind) {
    case SPK::Ppat_any:
    case SPK::Ppat_var:
    case SPK::Ppat_constant:
    case SPK::Ppat_interval:
    case SPK::Ppat_extension:
    case SPK::Ppat_type:
    case SPK::Ppat_unpack: return;
    case SPK::Ppat_construct:
      if (auto* a = as<pt::Ppat_construct>(d)->arg) f(a->pat);
      return;
    case SPK::Ppat_array:
      for (auto* q : as<pt::Ppat_array>(d)->pats) f(q);
      return;
    case SPK::Ppat_or: {
      auto* o = as<pt::Ppat_or>(d);
      f(o->p1);
      f(o->p2);
      return;
    }
    case SPK::Ppat_effect: {
      auto* e = as<pt::Ppat_effect>(d);
      f(e->eff);
      f(e->cont);
      return;
    }
    case SPK::Ppat_variant:
      if (auto* a = as<pt::Ppat_variant>(d)->arg) f(a);
      return;
    case SPK::Ppat_tuple:
      for (auto& x : as<pt::Ppat_tuple>(d)->pl) f(x.pat);
      return;
    case SPK::Ppat_exception: f(as<pt::Ppat_exception>(d)->pat); return;
    case SPK::Ppat_alias: f(as<pt::Ppat_alias>(d)->pat); return;
    case SPK::Ppat_open: f(as<pt::Ppat_open>(d)->pat); return;
    case SPK::Ppat_constraint: f(as<pt::Ppat_constraint>(d)->pat); return;
    case SPK::Ppat_lazy: f(as<pt::Ppat_lazy>(d)->pat); return;
    case SPK::Ppat_record:
      for (auto& x : as<pt::Ppat_record>(d)->fields) f(x.second);
      return;
  }
}

bool exists_ppat(const std::function<bool(const pt::Pattern*)>& f, const pt::Pattern* p) {
  struct Found {};
  std::function<void(const pt::Pattern*)> loop = [&](const pt::Pattern* q) {
    if (f(q)) throw Found{};
    shallow_iter_ppat(loop, q);
  };
  try {
    loop(p);
  } catch (const Found&) {
    return true;
  }
  return false;
}

bool contains_polymorphic_variant(const pt::Pattern* p) {
  return exists_ppat([](const pt::Pattern* q) {
    return q->ppat_desc->kind == SPK::Ppat_variant || q->ppat_desc->kind == SPK::Ppat_type;
  }, p);
}

bool contains_gadt(const tt::Pattern* p) {
  return tt::exists_general_pattern([](const tt::Pattern* q) {
    auto* c = as<tt::Tpat_construct>(q->pat_desc);
    return c && c->cstr->cstr_generalized;
  }, p);
}

// When typing [let rec p = e ...], we require [p] to be "variable-like"
bool is_var_pat(const pt::Pattern* p) {
  const pt::PatternDesc* d = p->ppat_desc;
  if (d->kind == SPK::Ppat_var) return true;
  if (auto* c = as<pt::Ppat_constraint>(d)) return is_var_pat(c->pat);
  if (auto* o = as<pt::Ppat_open>(d)) return is_var_pat(o->pat);
  return false;
}

// (see typecore.ml) conservatively assume that any constructor might be a
// GADT constructor.
bool may_contain_gadts(const pt::Pattern* p) {
  return exists_ppat([](const pt::Pattern* q) { return q->ppat_desc->kind == SPK::Ppat_construct; }, p);
}

bool turn_let_into_match(const pt::Pattern* p) {
  return exists_ppat([](const pt::Pattern* q) {
    const pt::PatternDesc* d = q->ppat_desc;
    if (d->kind == SPK::Ppat_construct) return true;
    if (auto* t = as<pt::Ppat_tuple>(d)) {
      if (t->closed == ClosedFlag::Open) return true;
      for (auto& x : t->pl)
        if (x.label.some) return true;
    }
    return false;
  }, p);
}

bool may_contain_modules(const pt::Pattern* p) {
  return exists_ppat([](const pt::Pattern* q) { return q->ppat_desc->kind == SPK::Ppat_unpack; }, p);
}

void check_absent_variant(env::t env, const tt::Pattern* p) {
  tt::iter_general_pattern([&](const tt::Pattern* pat) {
    auto* v = as<tt::Tpat_variant>(pat->pat_desc);
    if (!v) return;
    const RowDesc* row = v->row->contents;
    bool found = false;
    for (auto& e : row_fields(row))
      if (e.label == v->label && row_field_repr(e.field).kind != RowFieldView::Kind::Rabsent) found = true;
    if (found || (!is_fixed(row) && !static_row(row)))  // same as Ctype.poly
      return;
    std::vector<TypeExpr*> ty_arg;
    if (v->arg) ty_arg.push_back(ctype::duplicate_type(v->arg->pat_type));
    std::vector<RowFieldEntry> fields{{v->label, rf_either(nullptr, v->arg == nullptr, slice(ty_arg), true)}};
    TypeExpr* more = ctype::newvar();
    const RowDesc* row2 = create_row(slice(fields), more, false, nullptr, nullptr);
    // Should fail.  (unify_pat env {pat with ..} (duplicate_type ..)):
    // right to left
    TypeExpr* dup = ctype::duplicate_type(pat->pat_type);
    tt::Pattern* q = make<tt::Pattern>(*pat);
    q->pat_type = ctype::newty(tvariant(row2));
    unify_pat(env, q, dup);
  }, p);
}

// To find reasonable names for let-bound and lambda-bound idents
Ident::t name_pattern(std::string_view dflt, const std::vector<const tt::Pattern*>& pats) {
  for (auto* p : pats) {
    if (auto* v = as<tt::Tpat_var>(p->pat_desc)) return v->id;
    if (auto* a = as<tt::Tpat_alias>(p->pat_desc)) return a->id;
  }
  return Ident::create_local(dflt);
}
Ident::t name_cases(std::string_view dflt, Slice<const tt::Case*> lst) {
  std::vector<const tt::Pattern*> pats;
  for (auto* c : lst) pats.push_back(c->c_lhs);
  return name_pattern(dflt, pats);
}

// If [is_inferred e] is true, [e] will be typechecked without using the
// "expected type" provided by the context.
bool is_inferred(const pt::Expression* sexp) {
  const pt::ExpressionDesc* d = sexp->pexp_desc;
  switch (d->kind) {
    case SXK::Pexp_ident:
    case SXK::Pexp_apply:
    case SXK::Pexp_field:
    case SXK::Pexp_constraint:
    case SXK::Pexp_coerce:
    case SXK::Pexp_send:
    case SXK::Pexp_new: return true;
    case SXK::Pexp_pack: return as<pt::Pexp_pack>(d)->pack != nullptr;
    case SXK::Pexp_sequence: return is_inferred(as<pt::Pexp_sequence>(d)->e2);
    case SXK::Pexp_ifthenelse: {
      auto* i = as<pt::Pexp_ifthenelse>(d);
      return i->else_ && is_inferred(i->then_) && is_inferred(i->else_);
    }
    case SXK::Pexp_struct_item: {
      // traverse at least `local open`s, `M.(exp)`, cf #14629
      auto* s = as<pt::Pexp_struct_item>(d);
      if (s->item->pstr_desc->kind == pt::StructureItemDesc::Kind::Pstr_open) return is_inferred(s->body);
      return false;
    }
    default: return false;
  }
}

// check if the type of %apply or %revapply matches the type expected by the
// specialized typing rule for those primitives.
bool check_apply_prim_type(ApplyPrim prim, TypeExpr* typ) {
  auto* ab = as<Tarrow>(get_desc(typ));
  if (!(ab && ab->label.kind == ArgLabel::Kind::Nolabel && tpoly_is_mono(ab->t1))) return false;
  TypeExpr* a = tpoly_get_mono(ab->t1);
  auto* cd = as<Tarrow>(get_desc(ab->t2));
  if (!(cd && cd->label.kind == ArgLabel::Kind::Nolabel && tpoly_is_mono(cd->t1))) return false;
  TypeExpr* c = tpoly_get_mono(cd->t1);
  TypeExpr* d = cd->t2;
  TypeExpr* f = prim == ApplyPrim::Apply ? a : c;
  TypeExpr* x = prim == ApplyPrim::Apply ? c : a;
  TypeExpr* res = d;
  auto* ff = as<Tarrow>(get_desc(f));
  if (!(ff && ff->label.kind == ArgLabel::Kind::Nolabel && tpoly_is_mono(ff->t1))) return false;
  TypeExpr* fl = tpoly_get_mono(ff->t1);
  TypeExpr* fr = ff->t2;
  return is_Tvar(fl) && is_Tvar(fr) && is_Tvar(x) && is_Tvar(res) && eq_type(fl, x) && eq_type(fr, res);
}

// lower the level of function arguments to the level of the application
void lower_args(long outer_level, env::t env0, TypeExpr* ty_fun0) {
  auto lower = [&](env::t env, TypeExpr* ty) {
    try {
      ctype::unify_var(env, ctype::newvar2(outer_level), ty);
    } catch (const ctype::Unify&) {
      throw std::logic_error("lower_args");
    }
  };
  std::function<void(env::t, TypeSet, TypeExpr*)> lower_args_ = [&](env::t env, TypeSet seen, TypeExpr* ty_fun) {
    TypeExpr* ty = ctype::expand_head(env, ty_fun);
    if (seen.mem(ty)) return;
    const TypeDesc* d = get_desc(ty);
    if (auto* a = as<Tarrow>(d)) {
      lower(env, a->t1);
      seen.add(ty);
      lower_args_(env, seen, a->t2);
    } else if (auto* f = as<Tfunctor>(d)) {
      for (auto& c : f->pack->pack_constraints) lower(env, c.ty);
      auto [env2, ty_fun2] = ctype::open_tfunctor(env, location::none(), f->id, f->pack, f->body);
      seen.add(ty);
      lower_args_(env2, seen, ty_fun2);
    }
  };
  TypeExpr* ty = ctype::instance(ty_fun0);
  ctype::wrap_trace_gadt_instances(env0, [&] { lower_args_(env0, TypeSet{}, ty); });
}

void enforce_syntactic_arity(const Location& loc, env::t env0, TypeExpr* exp_type,
                             const std::vector<TypeFunctionResultParam>& result_params,
                             const tt::FunctionBody* body) {
  // Require that the n-ary function is known to have at least n arrows in
  // the type (see typecore.ml).  Assert that [ty] is a function, and return
  // its return type.
  auto filter_ty_ret_exn = [&](const ArgLabel& arg_label, std::pair<env::t, TypeExpr*> ety) {
    auto r = ctype::filter_arity(ety.first, ety.second, arg_label);
    if (r.ok) return r.value;
    et::UnificationError trace;
    using FK = ctype::FilterArrowFailure::Kind;
    switch (r.error.kind) {
      case FK::Unification_error: trace = r.error.err; break;
      case FK::Not_a_function: {
        // newty (Tarrow (arg_label, newmono (newvar ()), newvar (), commu_ok)): right to left
        TypeExpr* ret = ctype::newvar();
        TypeExpr* arg = ctype::newmono(ctype::newvar());
        TypeExpr* tarr = ctype::newty(tarrow(arg_label, arg, ret, commu_ok()));
        // We go to some trouble to try to generate a unification error to
        // help the error printing code's heuristic.
        try {
          ctype::unify(ety.first, tarr, ety.second);
          throw std::logic_error("unification unexpectedly succeeded");
        } catch (const ctype::Unify& u) {
          trace = u.err;
        }
        break;
      }
      case FK::Label_mismatch:
        throw std::logic_error("Label_mismatch not expected as this point");
    }
    long syntactic_arity = static_cast<long>(result_params.size()) +
                           (body->kind == tt::FunctionBody::Kind::Tfunction_body ? 0 : 1);
    Error e = err(loc, env0, EK::Function_arity_type_clash);
    e.n1 = syntactic_arity;
    e.ty = exp_type;
    e.trace = trace;
    raise_error(e);
  };
  std::pair<env::t, TypeExpr*> env_ret_ty{env0, exp_type};
  for (auto& p : result_params) env_ret_ty = filter_ty_ret_exn(p.param->fp_arg_label, env_ret_ty);
  if (body->kind == tt::FunctionBody::Kind::Tfunction_cases) filter_ty_ret_exn(ArgLabel::nolabel(), env_ret_ty);
}

// Generalize expressions
void may_lower_contravariant(env::t env, const tt::Expression* exp) {
  if (maybe_expansive(exp)) ctype::lower_contravariant(env, exp->exp_type);
}

// value binding elaboration
const pt::Expression* vb_exp_constraint(const pt::ValueBinding* vb) {
  const pt::Expression* expr = vb->pvb_expr;
  const pt::ValueConstraint* ct = vb->pvb_constraint;
  if (!ct) return expr;
  if (ct->kind == pt::ValueConstraint::Kind::Pvc_coercion) {
    Location loc = expr->pexp_loc;
    loc.loc_ghost = true;
    return ah::exp_coerce(loc, expr, ct->ground, ct->coercion);
  }
  if (ct->locally_abstract_univars.empty()) {
    if (ct->typ->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly) return expr;
    Location loc = expr->pexp_loc;
    loc.loc_ghost = true;
    return ah::exp_constraint(loc, expr, ct->typ);
  }
  Location loc = expr->pexp_loc;
  loc.loc_start = vb->pvb_pat->ppat_loc.loc_start;
  loc.loc_ghost = true;
  const pt::Expression* e = ah::exp_constraint(loc, expr, ct->typ);
  // List.fold_right (Exp.newtype ~loc) vars expr
  for (std::size_t k = ct->locally_abstract_univars.size(); k-- > 0;)
    e = ah::exp_newtype(loc, ct->locally_abstract_univars[k], e);
  return e;
}

std::pair<pt::Attributes, const pt::Pattern*> vb_pat_constraint(const pt::ValueBinding* vb) {
  const pt::Pattern* pat = vb->pvb_pat;
  const pt::Expression* exp = vb->pvb_expr;
  const pt::ValueConstraint* ct = vb->pvb_constraint;
  auto ghost = [](Location l) {
    l.loc_ghost = true;
    return l;
  };
  if (ct) {
    if (ct->kind == pt::ValueConstraint::Kind::Pvc_coercion)
      return {vb->pvb_attributes, ah::pat_constraint(ghost(pat->ppat_loc), pat, ct->coercion)};
    if (ct->locally_abstract_univars.empty())
      return {vb->pvb_attributes, ah::pat_constraint(ghost(pat->ppat_loc), pat, ct->typ)};
    const pt::CoreType* varified = ah::varify_constructors(ct->locally_abstract_univars, ct->typ);
    const pt::CoreType* t = ah::typ_poly(ct->typ->ptyp_loc, ct->locally_abstract_univars, varified);
    Location loc = pat->ppat_loc;
    loc.loc_end = ct->typ->ptyp_loc.loc_end;
    loc.loc_ghost = true;
    return {vb->pvb_attributes, ah::pat_constraint(loc, pat, t)};
  }
  SPK pk = pat->ppat_desc->kind;
  if (pk == SPK::Ppat_any || pk == SPK::Ppat_constraint) return {vb->pvb_attributes, pat};
  if (clflags::principal) {
    const pt::CoreType* sty = nullptr;
    if (auto* c = as<pt::Pexp_coerce>(exp->pexp_desc)) sty = c->to;
    else if (auto* c2 = as<pt::Pexp_constraint>(exp->pexp_desc)) sty = c2->ty;
    // propagate type annotation to pattern, to allow it to be generalized
    // in -principal mode
    if (sty) return {vb->pvb_attributes, ah::pat_constraint(ghost(pat->ppat_loc), pat, sty)};
  }
  return {vb->pvb_attributes, pat};
}

// Performs the relaxed value restriction on a list of typed value bindings
void do_relaxed_value_restriction(
    env::t env, const std::vector<std::pair<const tt::Pattern*, TypeExpr*>>& pat_list,
    const std::vector<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>>& exp_list) {
  if (pat_list.size() != exp_list.size()) throw std::invalid_argument("List.iter2");
  for (std::size_t k = 0; k < pat_list.size(); ++k) {
    if (maybe_expansive(exp_list[k].first)) {
      ctype::lower_contravariant(env, pat_list[k].first->pat_type);
      if (exp_list[k].second) ctype::lower_contravariant(env, exp_list[k].first->exp_type);
    }
  }
}

void check_let_univars(
    env::t env, const std::vector<std::pair<const tt::Pattern*, TypeExpr*>>& pat_list,
    const std::vector<std::pair<const tt::Expression*, std::optional<std::vector<TypeExpr*>>>>& exp_list) {
  if (pat_list.size() != exp_list.size()) throw std::invalid_argument("List.iter2");
  for (std::size_t k = 0; k < pat_list.size(); ++k)
    if (exp_list[k].second) check_univars(env, "definition", exp_list[k].first, pat_list[k].second, *exp_list[k].second);
}

}  // namespace cppcaml::typing::typecore

namespace cppcaml::typing::ast_helper {

// Typ.varify_constructors
const pt::CoreType* varify_constructors(Slice<pt::StrLoc> var_names0, const pt::CoreType* t) {
  std::vector<std::string_view> var_names;
  for (auto& v : var_names0) var_names.push_back(v.txt);
  auto mem = [&](std::string_view v) { return std::find(var_names.begin(), var_names.end(), v) != var_names.end(); };
  auto check_variable = [&](const Location& loc, std::string_view v) {
    if (mem(v)) throw typecore::VariableInScope(loc, v);
  };
  using K = pt::CoreTypeDesc::Kind;
  std::function<const pt::CoreType*(const pt::CoreType*)> loop;
  auto loop_package_type = [&](const pt::PackageType* p) {
    std::vector<std::pair<pt::LidLoc, const pt::CoreType*>> cs;
    for (auto& [l, ty] : p->ppt_constraints) cs.push_back({l, loop(ty)});
    return make<pt::PackageType>(pt::PackageType{p->ppt_path, slice(cs), p->ppt_loc, p->ppt_attrs});
  };
  loop = [&](const pt::CoreType* ty) -> const pt::CoreType* {
    const pt::CoreTypeDesc* d = ty->ptyp_desc;
    const pt::CoreTypeDesc* nd = d;
    switch (d->kind) {
      case K::Ptyp_any: break;
      case K::Ptyp_var: check_variable(ty->ptyp_loc, as<pt::Ptyp_var>(d)->name); break;
      case K::Ptyp_arrow: {
        auto* a = as<pt::Ptyp_arrow>(d);
        // Ptyp_arrow (label, loop t1, loop t2): right to left
        const pt::CoreType* t2 = loop(a->t2);
        const pt::CoreType* t1 = loop(a->t1);
        nd = make<pt::Ptyp_arrow>(pt::Ptyp_arrow{{K::Ptyp_arrow}, a->label, t1, t2});
        break;
      }
      case K::Ptyp_tuple: {
        std::vector<pt::LabeledCoreType> tl;
        for (auto& x : as<pt::Ptyp_tuple>(d)->tl) tl.push_back({x.label, loop(x.ty)});
        nd = make<pt::Ptyp_tuple>(pt::Ptyp_tuple{{K::Ptyp_tuple}, slice(tl)});
        break;
      }
      case K::Ptyp_constr: {
        auto* c = as<pt::Ptyp_constr>(d);
        if (c->lid.txt->kind == Longident::Kind::Lident && c->args.empty() && mem(c->lid.txt->s)) {
          nd = make<pt::Ptyp_var>(pt::Ptyp_var{{K::Ptyp_var}, c->lid.txt->s});
        } else {
          std::vector<const pt::CoreType*> args;
          for (auto* a : c->args) args.push_back(loop(a));
          nd = make<pt::Ptyp_constr>(pt::Ptyp_constr{{K::Ptyp_constr}, c->lid, slice(args)});
        }
        break;
      }
      case K::Ptyp_object: {
        auto* o = as<pt::Ptyp_object>(d);
        std::vector<const pt::ObjectField*> fs;
        for (auto* f : o->fields) {
          const pt::ObjectFieldDesc* fd;
          if (auto* ot = as<pt::Otag>(f->pof_desc))
            fd = make<pt::Otag>(pt::Otag{{pt::ObjectFieldDesc::Kind::Otag}, ot->label, loop(ot->ty)});
          else
            fd = make<pt::Oinherit>(pt::Oinherit{{pt::ObjectFieldDesc::Kind::Oinherit}, loop(as<pt::Oinherit>(f->pof_desc)->ty)});
          fs.push_back(make<pt::ObjectField>(fd, f->pof_loc, f->pof_attributes));
        }
        nd = make<pt::Ptyp_object>(pt::Ptyp_object{{K::Ptyp_object}, slice(fs), o->closed});
        break;
      }
      case K::Ptyp_class: {
        auto* c = as<pt::Ptyp_class>(d);
        std::vector<const pt::CoreType*> args;
        for (auto* a : c->args) args.push_back(loop(a));
        nd = make<pt::Ptyp_class>(pt::Ptyp_class{{K::Ptyp_class}, c->lid, slice(args)});
        break;
      }
      case K::Ptyp_alias: {
        auto* a = as<pt::Ptyp_alias>(d);
        check_variable(a->name.loc, a->name.txt);
        nd = make<pt::Ptyp_alias>(pt::Ptyp_alias{{K::Ptyp_alias}, loop(a->ty), a->name});
        break;
      }
      case K::Ptyp_variant: {
        auto* v = as<pt::Ptyp_variant>(d);
        std::vector<const pt::RowField*> fs;
        for (auto* f : v->fields) {
          const pt::RowFieldDesc* fd;
          if (auto* rt = as<pt::Rtag>(f->prf_desc)) {
            std::vector<const pt::CoreType*> tys;
            for (auto* x : rt->types) tys.push_back(loop(x));
            fd = make<pt::Rtag>(pt::Rtag{{pt::RowFieldDesc::Kind::Rtag}, rt->label, rt->constant, slice(tys)});
          } else {
            fd = make<pt::Rinherit>(pt::Rinherit{{pt::RowFieldDesc::Kind::Rinherit}, loop(as<pt::Rinherit>(f->prf_desc)->ty)});
          }
          fs.push_back(make<pt::RowField>(fd, f->prf_loc, f->prf_attributes));
        }
        nd = make<pt::Ptyp_variant>(pt::Ptyp_variant{{K::Ptyp_variant}, slice(fs), v->closed, v->has_labels, v->labels});
        break;
      }
      case K::Ptyp_poly: {
        auto* p = as<pt::Ptyp_poly>(d);
        for (auto& v : p->vars) check_variable(ty->ptyp_loc, v.txt);
        nd = make<pt::Ptyp_poly>(pt::Ptyp_poly{{K::Ptyp_poly}, p->vars, loop(p->ty)});
        break;
      }
      case K::Ptyp_package:
        nd = make<pt::Ptyp_package>(pt::Ptyp_package{{K::Ptyp_package}, loop_package_type(as<pt::Ptyp_package>(d)->pack)});
        break;
      case K::Ptyp_open: {
        auto* o = as<pt::Ptyp_open>(d);
        nd = make<pt::Ptyp_open>(pt::Ptyp_open{{K::Ptyp_open}, o->lid, loop(o->ty)});
        break;
      }
      case K::Ptyp_extension: break;
      case K::Ptyp_functor: {
        auto* f = as<pt::Ptyp_functor>(d);
        // Ptyp_functor (label, name, loop_package_type ptyp, loop codomain): right to left
        const pt::CoreType* cod = loop(f->ty);
        const pt::PackageType* pk = loop_package_type(f->pack);
        nd = make<pt::Ptyp_functor>(pt::Ptyp_functor{{K::Ptyp_functor}, f->label, f->name, pk, cod});
        break;
      }
    }
    pt::CoreType* r = make<pt::CoreType>(*ty);
    r->ptyp_desc = nd;
    return r;
  };
  return loop(t);
}

}  // namespace cppcaml::typing::ast_helper
