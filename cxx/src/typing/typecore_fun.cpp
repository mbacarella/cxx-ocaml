// Port of typing/typecore.ml, part 5: functions (type_function,
// type_moddep_fun), record label access, format strings, labeled
// expressions, arguments, applications, constructors and statements
// ("type_function" to "type_statement").
#include "ast_helper.hpp"
#include "cppcaml/typing/camlinternal_format.hpp"
#include "cppcaml/typing/subst.hpp"
#include "typecore_cases.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using XK = tt::ExpressionDesc::Kind;
using SXK = pt::ExpressionDesc::Kind;
using SPK = pt::PatternDesc::Kind;
namespace ah = ast_helper;

bool could_be_functor(env::t env, TypeExpr* ty) {
  DescKind k = get_desc(ctype::expand_head(env, ty))->kind;
  return k == DescKind::Tvar || k == DescKind::Tfunctor;
}

static TypeFunctionResult type_moddep_fun(env::t env, const pt::StrLoc& name, const pt::PackageType* pack_param,
                                          Slice<const pt::FunctionParam*> rest, const ArgLabel& arg_label,
                                          bool first, const InFunction& in_function, TypeExpr* ty_expected,
                                          const Location& pparam_loc, const Location& loc,
                                          const pt::TypeConstraint* body_constraint, const pt::FunctionBody* body);

// Typecheck parameters one at a time followed by the body.  Later
// parameters are checked in the scope of earlier ones.
TypeFunctionResult type_function(env::t env, Slice<const pt::FunctionParam*> params_suffix,
                                 const pt::TypeConstraint* body_constraint, const pt::FunctionBody* body,
                                 TypeExpr* ty_expected, bool first, const InFunction& in_function) {
  const TypeExpected& ty_fun = in_function.ty_fun;
  // The "rest of the function" extends from the start of the first
  // parameter to the end of the overall function.
  Location loc = loc_rest_of_function(in_function.loc, first, params_suffix, body);
  if (!params_suffix.empty()) {
    const pt::FunctionParam* p = params_suffix[0];
    Slice<const pt::FunctionParam*> rest(params_suffix.begin() + 1, params_suffix.size() - 1);
    const pt::FunctionParamDesc& pd = p->pparam_desc;
    if (pd.kind == pt::FunctionParamDesc::Kind::Pparam_newtype) {
      // Check everything else in the scope of (type a).
      auto [r, exp_type] = type_newtype<TypeFunctionResult>(env, pd.newtype, [&](env::t env2) {
        // mimic the typing of Pexp_newtype by minting a new type var, like
        // [type_exp].
        TypeFunctionResult r2 =
            type_function(env2, rest, body_constraint, body, ctype::newvar(), false, in_function);
        return std::make_pair(r2, r2.exp_type);
      });
      with_explanation(ty_fun.explanation, [&] {
        TypeExpr* ie = ctype::instance(ty_expected);
        unify_exp_types(loc, env, exp_type, ie);
      });
      std::vector<pt::StrLoc> newtypes{pd.newtype};
      newtypes.insert(newtypes.end(), r.newtypes.begin(), r.newtypes.end());
      return {exp_type, r.params, r.body, newtypes, r.contains_gadt};
    }
    if (!pd.default_ && is_unpack(pd.pat) && could_be_functor(env, ty_expected) && !is_optional(pd.label)) {
      auto* u = as<pt::Ppat_unpack>(pd.pat->ppat_desc);
      pt::StrLoc name{u->name.txt.v, u->name.loc};
      return type_moddep_fun(env, name, u->pack, rest, pd.label, first, in_function, ty_expected, p->pparam_loc,
                             loc, body_constraint, body);
    }
    const ArgLabel& arg_label = pd.label;
    const pt::Pattern* pat = pd.pat;
    bool has_poly = check_poly_constraint(pat, env, arg_label);
    SplitFunctionTy sft = split_function_ty(env, ty_expected, arg_label, has_poly, first, in_function);
    TypeExpr* ty_param = sft.filtered_arrow.ty_param;
    TypeExpr* ty_ret = sft.filtered_arrow.ty_ret;
    // [ty_arg_internal] is the type of the parameter viewed internally to
    // the function (not optional for optional arguments with defaults).
    TypeExpr* ty_arg_internal = sft.ty_arg_mono;
    const tt::Expression* default_arg = nullptr;
    if (pd.default_) {
      if (!is_optional(arg_label)) throw std::logic_error("type_function: default");
      TypeExpr* ty_default = ctype::newvar();
      try {
        ctype::unify(env, type_option(ty_default), sft.ty_arg_mono);
      } catch (const ctype::Unify&) {
        throw std::logic_error("type_function: default unify");
      }
      // Issue#12668: Retain type-directed disambiguation of
      // ?x:(y : Variant.t = Constr)
      const pt::Expression* dflt = pd.default_;
      if (auto* c = as<pt::Ppat_constraint>(pat->ppat_desc)) {
        Location gloc = dflt->pexp_loc;
        gloc.loc_ghost = true;
        gloc = location::distinct_record(gloc);  // {default.pexp_loc with loc_ghost = true}
        dflt = ah::exp_constraint(gloc, dflt, c->ty);
      }
      default_arg = type_expect(env, dflt, mk_expected(ty_default));
      ty_arg_internal = ty_default;
    }
    struct R5 {
      const tt::Pattern* pat;
      std::vector<TypeFunctionResultParam> params;
      const tt::FunctionBody* body;
      std::vector<pt::StrLoc> newtypes;
      bool contains_gadt;
    };
    struct Unit {};
    // Check everything else in the scope of the parameter.
    std::vector<std::pair<UntypedCase, Unit>> caselist{{UntypedCase{pat, false, false}, Unit{}}};
    TypeBody<Unit, R5> tb = [&](const Unit&, const tt::Pattern* tpat, env::t, env::t ext_env,
                                const std::optional<ContinuationVar>&, TypeExpr* ty_exp2, TypeExpr*,
                                bool param_contains_gadt) {
      TypeFunctionResult r = type_function(ext_env, rest, body_constraint, body, ty_exp2, false, in_function);
      return R5{tpat, r.params, r.body, r.newtypes, param_contains_gadt || r.contains_gadt};
    };
    auto [results, partial] = map_half_typed_cases<Unit, R5>(nullptr, nullptr, tt::PatternCategory::Value, env,
                                                             ty_arg_internal, ty_ret, pat->ppat_loc, caselist, tb, true);
    // The result must be a singleton because we passed a singleton list.
    if (results.size() != 1) throw std::logic_error("type_function: cases");
    const R5& r = results[0];
    TypeExpr* exp_type = ctype::instance(newgenty(tarrow(arg_label, ty_param, ty_ret, commu_ok())));
    with_explanation(ty_fun.explanation, [&] {
      TypeExpr* ie = ctype::instance(ty_expected);
      unify_exp_types(loc, env, exp_type, ie);
    });
    auto not_nolabel_function = [env](TypeExpr* ty) {
      // [list_labels] does expansion and is potentially expensive; only
      // call this when necessary.
      auto [ls, tvar] = list_labels(env, ty);
      for (auto& l : ls)
        if (l.kind == ArgLabel::Kind::Nolabel) return false;
      return !tvar;
    };
    if (is_optional(arg_label) && not_nolabel_function(ty_ret))
      prerr_warning(r.pat->pat_loc, WK::Unerasable_optional_argument);
    tt::FunctionParamKind fp_kind{tt::FunctionParamKind::Kind::Tparam_pat, r.pat};
    Ident::t fp_param;
    if (!default_arg) {
      fp_param = name_pattern(OCAML_LIT("param"), {r.pat});
    } else {
      fp_param = Ident::create_local(OCAML_LIT("*opt*"));
      fp_kind = tt::FunctionParamKind{tt::FunctionParamKind::Kind::Tparam_optional_default, r.pat, default_arg};
    }
    auto* param =
        make<tt::FunctionParam>(arg_label, fp_param, partial, fp_kind, slice(r.newtypes), p->pparam_loc);
    std::vector<TypeFunctionResultParam> params{{param, has_poly}};
    params.insert(params.end(), r.params.begin(), r.params.end());
    return {exp_type, params, r.body, {}, r.contains_gadt};
  }
  TypeExpr* exp_type;
  tt::FunctionBody* tbody = make<tt::FunctionBody>();
  if (body->kind == pt::FunctionBody::Kind::Pfunction_body) {
    const tt::Expression* b;
    if (!body_constraint) {
      b = type_expect(env, body->body, mk_expected(ty_expected));
    } else {
      const Location& body_loc = body->body->pexp_loc;
      Constrained<const tt::Expression*> r = type_constraint_expect(expression_constraint(body->body), env, body_loc,
                                                                    body_loc, *body_constraint, ty_expected);
      tt::Expression* b2 = make<tt::Expression>(*r.ret);
      std::vector<tt::ExpExtraItem> extra{{r.extra, body_loc, {}}};
      extra.insert(extra.end(), r.ret->exp_extra.begin(), r.ret->exp_extra.end());
      b2->exp_extra = slice(extra);
      b2->exp_type = r.ty;
      b = b2;
    }
    exp_type = b->exp_type;
    tbody->kind = tt::FunctionBody::Kind::Tfunction_body;
    tbody->body = b;
  } else {
    Slice<const pt::Case*> cases = body->cases;
    pt::Attributes attributes = body->attrs;
    auto type_cases_expect = [&](env::t env2, TypeExpr* te) {
      return type_function_cases_expect(env2, te, loc, cases, attributes, first, in_function);
    };
    FunctionCases cp;
    const tt::ExpExtra* exp_extra = nullptr;
    if (!body_constraint) {
      FunctionCasesResult r = type_cases_expect(env, ty_expected);
      cp = {r.cases, r.partial};
      exp_type = r.ty_fun;
    } else {
      // The typing of function case coercions/constraints is analogous to
      // the typing of expression coercions/constraints.
      ConstraintArg<FunctionCases> arg;
      arg.is_self = [](const FunctionCases&) { return false; };
      arg.type_with_constraint = [&](env::t env2, TypeExpr* ty) {
        FunctionCasesResult r = type_cases_expect(env2, ty);
        return FunctionCases{r.cases, r.partial};
      };
      arg.type_without_constraint = [&](env::t env2) {
        // The analogy to [type_exp] for expressions.
        FunctionCasesResult r = type_cases_expect(env2, ctype::newvar());
        return std::make_pair(FunctionCases{r.cases, r.partial}, r.ty_fun);
      };
      Constrained<FunctionCases> r = type_constraint_expect(arg, env, loc, loc, *body_constraint, ty_expected);
      cp = r.ret;
      exp_type = r.ty;
      exp_extra = make<tt::ExpExtra>(r.extra);
    }
    tbody->kind = tt::FunctionBody::Kind::Tfunction_cases;
    tbody->cases = cp.first;
    tbody->partial = cp.second;
    tbody->param = name_cases(OCAML_LIT("param"), cp.first);
    tbody->loc = loc;
    tbody->exp_extra = exp_extra;
    tbody->attributes = attributes;
  }
  // [No_gadt]: this value only says whether [params] (here, empty) contains
  // a GADT.
  return {exp_type, {}, tbody, {}, false};
}

static TypeFunctionResult type_moddep_fun(env::t env, const pt::StrLoc& name, const pt::PackageType* pack_param,
                                          Slice<const pt::FunctionParam*> rest, const ArgLabel& arg_label,
                                          bool first, const InFunction& in_function, TypeExpr* ty_expected,
                                          const Location& pparam_loc, const Location& loc,
                                          const pt::TypeConstraint* body_constraint, const pt::FunctionBody* body) {
  auto type_pack = [&](const pt::PackageType* pack) {
    const pt::CoreType* t = ah::typ_package(pack->ppt_loc, pack);
    const tt::CoreType* cpack = typetexp::transl_simple_type(env, nullptr, false, t);
    auto* tp = as<Tpackage>(get_desc(cpack->ctyp_type));
    if (!tp) throw std::logic_error("type_moddep_fun: package");
    return std::make_pair(cpack, tp->pack);
  };
  struct IdEty {
    ident::Unscoped* id;
    TypeExpr* ety;
  };
  std::optional<IdEty> id_expected_typ_opt;
  const tt::CoreType* cpack = nullptr;
  const Package* pack;
  std::optional<ctype::FunctorView> split = split_function_mty(env, ty_expected, arg_label, first, in_function);
  if (!split && !pack_param) {
    raise_error(err(loc, env, EK::Cannot_infer_signature));
  } else if (!split) {
    std::tie(cpack, pack) = type_pack(pack_param);
  } else if (pack_param) {
    std::tie(cpack, pack) = type_pack(pack_param);
    try {
      // unify env (newty (Tfunctor .. pack ..)) (newty (Tfunctor .. pack' ..)): right to left
      TypeExpr* t2 = ctype::newty(tfunctor(arg_label, split->id, split->pack, ctype::newvar()));
      TypeExpr* t1 = ctype::newty(tfunctor(arg_label, split->id, pack, ctype::newvar()));
      ctype::unify(env, t1, t2);
    } catch (const ctype::Unify& u) {
      Error e = err(loc, env, EK::Expr_type_clash);
      e.trace = u.err;
      raise_error(e);
    }
    id_expected_typ_opt = IdEty{split->id, split->ty};
  } else {
    // (the -principal warning is not emitted)
    id_expected_typ_opt = IdEty{split->id, split->ty};
    pack = split->pack;
  }
  {
    std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>> cstrs;
    for (auto& c : pack->pack_constraints)
      cstrs.push_back({std::vector<std::string_view>(c.path.begin(), c.path.end()), c.ty});
    check_package_closed(pparam_loc, env, ctype::newty(tpackage(pack)), cstrs);
  }
  const ModuleType* mty = ctype::modtype_of_package(env, pparam_loc, pack);
  Uid pv_uid = uid::mk(env::get_current_unit());
  auto* arg_md = make<ModuleDeclaration>(mty, Attributes{}, pparam_loc, pv_uid);
  auto [r, s_ident] = ctype::with_local_level([&] {
    Ident::t s_ident = Ident::create_scoped(static_cast<int>(ctype::get_current_level()), name.txt);
    env::t new_env = env::add_module_declaration(true, s_ident, ModulePresence::Mp_present, arg_md, env);
    TypeExpr* expected_res;
    if (id_expected_typ_opt)
      expected_res = ctype::with_local_level_generalize_structure_if_principal([&] {
        return ctype::instance_funct(Ident::of_unscoped(id_expected_typ_opt->id), Path::pident(s_ident), false,
                                     id_expected_typ_opt->ety);
      });
    else
      expected_res = ctype::newvar();
    TypeFunctionResult r2 =
        type_function(new_env, rest, body_constraint, body, expected_res, false, in_function);
    return std::make_pair(r2, s_ident);
  });
  ident::Unscoped* ident = ident::Unscoped::create(name.txt);
  TypeExpr* exp_type;
  if (TypeExpr* res_ty =
          ctype::instance_funct_opt(s_ident, Path::pident(Ident::of_unscoped(ident)), false, r.exp_type)) {
    exp_type = newgenty(tfunctor(arg_label, ident, pack, res_ty));
  } else {
    TypeExpr* pck_ty = newgenmono(newgenty(tpackage(pack)));
    exp_type = newgenty(tarrow(arg_label, pck_ty, r.exp_type, commu_ok()));
  }
  try {
    ctype::unify(env, ty_expected, exp_type);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Expr_type_clash);
    e.trace = u.err;
    raise_error(e);
  }
  const tt::PackageType* pp = nullptr;
  if (cpack) {
    auto* tp = as<tt::Ttyp_package>(cpack->ctyp_desc);
    if (!tp) throw std::logic_error("type_moddep_fun: cpack");
    pp = tp->pack;
  }
  tt::PatExtra px{tt::PatExtra::Kind::Tpat_unpack};
  px.pack = pp;
  std::vector<tt::PatExtraItem> pat_extra{{px, pparam_loc, {}}};
  auto* pattern = make<tt::Pattern>(
      make<tt::Tpat_var>(tt::Tpat_var{{tt::PatternDesc::Kind::Tpat_var}, s_ident, name, pv_uid}), pparam_loc,
      slice(pat_extra), ctype::newty(tpackage(pack)), env, tt::Attributes{});
  auto* param = make<tt::FunctionParam>(arg_label, s_ident, tt::Partial::Total,
                                        tt::FunctionParamKind{tt::FunctionParamKind::Kind::Tparam_pat, pattern},
                                        slice(r.newtypes), pparam_loc);
  std::vector<TypeFunctionResultParam> params{{param, false}};
  params.insert(params.end(), r.params.begin(), r.params.end());
  return {exp_type, params, r.body, {}, r.contains_gadt};
}

LabelAccess type_label_access(env::t env, const pt::Expression* srecord, env::LabelUsage usage,
                              const pt::LidLoc& lid) {
  const tt::Expression* record = ctype::with_local_level_generalize_structure_if_principal(
      [&] { return type_exp_r(Recarg::Allowed, env, srecord); });
  TypeExpr* ty_exp = record->exp_type;
  std::optional<ExpectedTypePath> expected_type;
  RecordExtraction re = extract_concrete_record(env, ty_exp);
  switch (re.kind) {
    case RecordExtraction::Kind::Record_type: expected_type = ExpectedTypePath{re.p0, re.p, is_principal(ty_exp)}; break;
    case RecordExtraction::Kind::Maybe_a_record_type: break;
    case RecordExtraction::Kind::Not_a_record_type: {
      Error e = err(record->exp_loc, env, EK::Expr_not_a_record_type);
      e.ty = ty_exp;
      raise_error(e);
    }
  }
  env::LookupAllLabels labels = env::lookup_all_labels(true, lid.loc, usage, lid.txt, env);
  const LabelDescription* label = wrap_disambiguate("This expression has", mk_expected(ty_exp), [&] {
    return disambiguate_label(usage, lid, env, expected_type, labels);
  });
  return {record, label, expected_type};
}

SolvedField solve_Pexp_field(env::LabelUsage label_usage, env::t env, const pt::Expression* sexp,
                             const pt::Expression* srecord, const pt::LidLoc& lid) {
  LabelAccess la = type_label_access(env, srecord, label_usage, lid);
  ctype::InstancedLabel il = ctype::instance_label(false, la.label);
  unify_exp(sexp, env, la.record, il.res);
  return {la.record, la.label, il.arg};
}

// Typing format strings for printing or reading (the Printf, Format and
// Scanf formats): the string's CamlinternalFormatBasics value, as syntax.
const pt::Expression* type_format(const Location& loc0, std::string_view str, env::t env) {
  Location loc = loc0;
  loc.loc_ghost = true;
  loc = location::distinct_record(loc);  // {loc with loc_ghost = true}
  namespace cf = camlinternal_format;
  auto mk_exp_loc = [&](const pt::ExpressionDesc* d) { return ah::exp_mk(d, loc); };
  auto mk_lid_loc = [&](Longident::t lid) { return pt::LidLoc{lid, loc}; };
  auto construct = [&](Longident::t lid, const pt::Expression* arg) {
    return mk_exp_loc(make<pt::Pexp_construct>(pt::Pexp_construct{{SXK::Pexp_construct}, mk_lid_loc(lid), arg}));
  };
  auto tuple = [&](const std::vector<const pt::Expression*>& es) {
    std::vector<pt::LabeledExpression> el;
    for (auto* e : es) el.push_back({OptStr::none(), e});
    return mk_exp_loc(make<pt::Pexp_tuple>(pt::Pexp_tuple{{SXK::Pexp_tuple}, slice(el)}));
  };
  auto mk_constr = [&](std::string_view name, const std::vector<const pt::Expression*>& args) {
    // the constructor names are typecore.ml's literals: one string each
    Longident::t lid =
        Longident::ldot(Longident::lident(ocaml_literal("typing/typecore.ml", "CamlinternalFormatBasics")),
                        location::none(), ocaml_literal("typing/typecore.ml", name), location::none());
    const pt::Expression* arg = args.empty() ? nullptr : args.size() == 1 ? args[0] : tuple(args);
    return construct(lid, arg);
  };
  auto mk_cst = [&](const pt::ConstantDesc& cst) {
    return mk_exp_loc(make<pt::Pexp_constant>(pt::Pexp_constant{{SXK::Pexp_constant}, pt::Constant{cst, loc}}));
  };
  auto mk_int = [&](long n) {
    pt::ConstantDesc c{pt::ConstantDesc::Kind::Pconst_integer};
    c.s = zborrow(std::to_string(n));
    return mk_cst(c);
  };
  auto mk_string = [&](std::string_view s) {
    pt::ConstantDesc c{pt::ConstantDesc::Kind::Pconst_string};
    c.s = s;
    c.str_loc = loc;
    return mk_cst(c);
  };
  auto mk_char = [&](char ch) {
    pt::ConstantDesc c{pt::ConstantDesc::Kind::Pconst_char};
    c.c = ch;
    return mk_cst(c);
  };
  std::function<const pt::Expression*(const cf::FmtValue*)> conv = [&](const cf::FmtValue* v) -> const pt::Expression* {
    switch (v->kind) {
      case cf::FmtValue::Kind::Constr: {
        std::vector<const pt::Expression*> args;
        for (auto* a : v->args) args.push_back(conv(a));
        return mk_constr(v->name, args);
      }
      case cf::FmtValue::Kind::Tuple: {
        std::vector<const pt::Expression*> args;
        for (auto* a : v->args) args.push_back(conv(a));
        return tuple(args);
      }
      case cf::FmtValue::Kind::None: return construct(Longident::lident(ocaml_literal("typing/typecore.ml", "None")), nullptr);
      case cf::FmtValue::Kind::Some: return construct(Longident::lident(ocaml_literal("typing/typecore.ml", "Some")), conv(v->args[0]));
      case cf::FmtValue::Kind::Int: return mk_int(v->i);
      case cf::FmtValue::Kind::String: return mk_string(v->s);
      case cf::FmtValue::Kind::Char: return mk_char(v->c);
    }
    throw std::logic_error("type_format");
  };
  try {
    bool legacy_behavior = !clflags::strict_formats;
    const cf::FmtValue* fmt = cf::fmt_ebb_of_string(legacy_behavior, str);
    return mk_constr("Format", {conv(fmt), mk_string(str)});
  } catch (const cf::Failure& f) {
    Error e = err(loc, env, EK::Invalid_format);
    e.name = f.msg;
    raise_error(e);
  }
}

LabelExp type_label_exp(bool create, env::t env, const Location& loc, TypeExpr* ty_expected,
                        const pt::LidLoc& lid, const LabelDescription* label, const pt::Expression* sarg) {
  // Here also ty_expected may be at generic_level
  bool separate = clflags::principal || env::has_local_constraints(env);
  bool is_poly = is_poly_Tpoly(label->lbl_arg);
  struct VA {
    std::vector<TypeExpr*> vars;
    const tt::Expression* arg;
  };
  // raise level to check univars
  VA va = ctype::with_local_level_generalize_if(
      is_poly,
      [&] {
        auto [vars, ty_arg] = ctype::with_local_level_generalize_structure_if(separate, [&] {
          ctype::InstancedLabel il = ctype::with_local_level_generalize_structure_if(
              separate, [&] { return ctype::instance_label(true, label); });
          try {
            TypeExpr* b = ctype::instance(ty_expected);
            TypeExpr* a = ctype::instance(il.res);
            ctype::unify(env, a, b);
          } catch (const ctype::Unify& u) {
            Error e = err(lid.loc, env, EK::Label_mismatch);
            e.lid = lid.txt;
            e.trace = u.err;
            raise_error(e);
          }
          // Instantiate so that we can generalize internal nodes
          TypeExpr* ty_arg2 = ctype::instance(il.arg);
          return std::make_pair(il.vars, ty_arg2);
        });
        if (label->lbl_private == PrivateFlag::Private) {
          if (create) {
            Error e = err(loc, env, EK::Private_type);
            e.ty = ty_expected;
            raise_error(e);
          }
          Error e = err(lid.loc, env, EK::Private_label);
          e.lid = lid.txt;
          e.ty = ty_expected;
          raise_error(e);
        }
        TypeExpr* ia = ctype::instance(ty_arg);
        return VA{vars, type_argument(env, sarg, ty_arg, ia)};
      },
      [&](const VA& va) { may_lower_contravariant(env, va.arg); });
  if (is_poly) check_univars(env, "field value", va.arg, label->lbl_arg, va.vars);
  tt::Expression* a2 = make<tt::Expression>(*va.arg);
  a2->exp_type = ctype::instance(va.arg->exp_type);
  return {lid, label, a2};
}

static std::pair<tt::Pattern*, tt::Expression*> var_pair(env::t env, std::string_view name, TypeExpr* ty) {
  Ident::t id = Ident::create_local(name);
  auto* desc = make<ValueDescription>(ty, ValueKind{}, location::none(), Attributes{}, uid::mk(env::get_current_unit()));
  env::t exp_env = env::add_value(id, desc, env);
  auto* pat = make<tt::Pattern>(
      make<tt::Tpat_var>(tt::Tpat_var{{tt::PatternDesc::Kind::Tpat_var}, id, pt::StrLoc{zborrow(name), location::none()},
                                      desc->val_uid}),
      location::none(), Slice<tt::PatExtraItem>{}, ty, env, tt::Attributes{});
  auto* exp = make<tt::Expression>(
      make<tt::Texp_ident>(tt::Texp_ident{{XK::Texp_ident}, Path::pident(id),
                                          pt::LidLoc{Longident::lident(zborrow(name)), location::none()}, desc}),
      location::none(), Slice<tt::ExpExtraItem>{}, ty, exp_env, tt::Attributes{});
  return {pat, exp};
}

const tt::Expression* type_argument_x(Explanation explanation, Recarg recarg, env::t env, const pt::Expression* sarg,
                                      TypeExpr* ty_expected_, TypeExpr* ty_expected) {
  // ty_expected' may be generic
  auto no_labels = [&](TypeExpr* ty) {
    auto [ls, tvar] = list_labels(env, ty);
    if (tvar) return false;
    for (auto& l : ls)
      if (l.kind != ArgLabel::Kind::Nolabel) return false;
    return true;
  };
  struct MayCoerce {
    bool safe_expect;
    long lv;
  };
  std::optional<MayCoerce> may_coerce;
  if (is_inferred(sarg)) {
    auto work = [&]() -> std::optional<MayCoerce> {
      TypeExpr* te = ctype::expand_head(env, ty_expected_);
      auto* a = as<Tarrow>(get_desc(te));
      if (a && a->label.kind == ArgLabel::Kind::Nolabel) return MayCoerce{no_labels(a->t2), get_level(te)};
      return std::nullopt;
    };
    // Need to be careful not to expand local constraints here
    if (env::has_local_constraints(env)) {
      Snapshot snap = btype::snapshot();
      struct B {
        Snapshot s;
        ~B() { btype::backtrack(s); }
      } b{snap};
      may_coerce = work();
    } else {
      may_coerce = work();
    }
  }
  if (!may_coerce) {
    const tt::Expression* texp = type_expect_r(recarg, env, sarg, mk_expected(ty_expected_, explanation));
    unify_exp(sarg, env, texp, ty_expected);
    return texp;
  }
  // apply omittable arguments when expected type is ""
  // we must be very careful about not breaking the semantics
  const tt::Expression* texp0 =
      ctype::with_local_level_generalize_structure_if_principal([&] { return type_exp(env, sarg); });
  std::vector<tt::LabeledArg> args;  // make_args
  TypeExpr* ty_fun2;
  bool simple_res;
  {
    TypeExpr* ty_fun = texp0->exp_type;
    for (;;) {
      const TypeDesc* d = get_desc(ctype::expand_head(env, ty_fun));
      auto* a = as<Tarrow>(d);
      if (a && is_optional(a->label)) {
        TypeExpr* it = ctype::instance(tpoly_get_mono(a->t1));
        const tt::Expression* ty = option_none(env, it, sarg->pexp_loc);
        args.push_back({a->label, tt::ApplyArg{false, ty}});
        ty_fun = a->t2;
        continue;
      }
      if (a && (a->label.kind == ArgLabel::Kind::Nolabel || clflags::classic)) {
        ty_fun2 = ty_fun;
        simple_res = no_labels(a->t2);
      } else if (d->kind == DescKind::Tvar) {
        ty_fun2 = ty_fun;
        simple_res = false;
      } else {
        args.clear();
        ty_fun2 = texp0->exp_type;
        simple_res = false;
      }
      break;
    }
  }
  tt::Expression* texp = make<tt::Expression>(*texp0);
  texp->exp_type = ctype::instance(texp0->exp_type);
  if (!(simple_res || may_coerce->safe_expect)) {
    unify_exp(sarg, env, texp, ty_expected);
    return texp;
  }
  bool warn = clflags::principal && (may_coerce->lv != generic_level || get_level(ty_fun2) != generic_level);
  TypeExpr* ty_fun = ctype::instance(ty_fun2);
  auto* ea = as<Tarrow>(get_desc(ctype::expand_head(env, ty_expected)));
  if (!(ea && ea->label.kind == ArgLabel::Kind::Nolabel)) throw std::logic_error("type_argument_");
  TypeExpr* ty_arg = ea->t1;
  TypeExpr* ty_res = ea->t2;
  {
    tt::Expression* t2 = make<tt::Expression>(*texp);
    t2->exp_type = ty_fun;
    unify_exp(sarg, env, t2, ty_expected);
  }
  if (args.empty()) return texp;
  // eta-expand to avoid side effects
  auto [eta_pat, eta_var] = var_pair(env, OCAML_LIT("eta"), ty_arg);
  auto func = [&, eta_pat = eta_pat, eta_var = eta_var](const tt::Expression* texp1) {
    std::vector<tt::LabeledArg> a2 = args;
    a2.push_back({ArgLabel::nolabel(), tt::ApplyArg{false, eta_var}});
    tt::Expression* e = make<tt::Expression>(*texp1);
    e->exp_type = ty_res;
    e->exp_desc = make<tt::Texp_apply>(tt::Texp_apply{{XK::Texp_apply}, texp1, slice(a2)});
    std::vector<const tt::Case*> cases{make<tt::Case>(eta_pat, nullptr, nullptr, e)};
    Location cases_loc = texp1->exp_loc;
    cases_loc.loc_ghost = true;
    cases_loc = location::distinct_record(cases_loc);  // {texp.exp_loc with loc_ghost = true}
    Ident::t param = name_cases(OCAML_LIT("param"), slice(cases));
    tt::FunctionBody* fb = make<tt::FunctionBody>();
    fb->kind = tt::FunctionBody::Kind::Tfunction_cases;
    fb->cases = slice(cases);
    fb->partial = tt::Partial::Total;
    fb->param = param;
    fb->loc = cases_loc;
    tt::Expression* f = make<tt::Expression>(*texp1);
    f->exp_type = ty_fun;
    f->exp_desc = make<tt::Texp_function>(tt::Texp_function{{XK::Texp_function}, {}, fb});
    return f;
  };
  {
    std::vector<std::string> ls;
    for (auto& a : args) ls.push_back(string_of_label(a.label));
    prerr_warning(texp->exp_loc, warnings::Warning::with_l(WK::Eliminated_optional_arguments, ls));
  }
  if (warn)
    prerr_warning(texp->exp_loc, warnings::Warning::with_s(WK::Non_principal_labels, "eliminated optional argument"));
  // let-expand to have side effects
  auto [let_pat, let_var] = var_pair(env, OCAML_LIT("arg"), texp->exp_type);
  auto* vb = make<tt::ValueBinding>(let_pat, texp, tt::RecursiveBindingKind::Dynamic, tt::Attributes{},
                                    location::none());
  std::vector<const tt::ValueBinding*> vbs{vb};
  tt::Expression* r = make<tt::Expression>(*texp);
  r->exp_type = ty_fun;
  r->exp_desc = make<tt::Texp_let>(tt::Texp_let{{XK::Texp_let}, RecFlag::Nonrecursive, slice(vbs), func(let_var)});
  return re(r);
}

const tt::Expression* type_argument(env::t env, const pt::Expression* sexp, TypeExpr* t1, TypeExpr* t2) {
  return type_argument_x(std::nullopt, Recarg::Rejected, env, sexp, t1, t2);
}

static UntypedArg type_apply_arg(env::t env, const Location& app_loc, const UntypedArg& a) {
  if (a.omitted) return a;
  using UK = UntypedApplyArg::Kind;
  auto typed = [&](const tt::Expression* e) {
    UntypedArg r = a;
    r.arg = UntypedApplyArg{UK::Typed_arg};
    r.arg.targ = e;
    return r;
  };
  const UntypedApplyArg& arg = a.arg;
  switch (arg.kind) {
    case UK::Unknown_arg: {
      const tt::Expression* e = type_expect(env, arg.sarg, mk_expected(arg.ty_arg));
      if (is_optional(a.label)) unify_exp(arg.sarg, env, e, type_option(ctype::newvar()));
      return typed(e);
    }
    case UK::Known_arg: {
      auto [ty_arg2, vars] = tpoly_get_poly(arg.ty_arg);
      if (vars.empty()) {
        TypeExpr* ty_arg02 = tpoly_get_mono(arg.ty_arg0);
        if (arg.wrapped_in_some) {
          // (type_argument env sarg (extract ..) (extract ..)): right to left
          TypeExpr* e0 = extract_option_type(env, ty_arg02);
          TypeExpr* e1 = extract_option_type(env, ty_arg2);
          return typed(option_some(env, type_argument(env, arg.sarg, e1, e0)));
        }
        return typed(type_argument(env, arg.sarg, ty_arg2, ty_arg02));
      }
      if (clflags::principal && get_level(arg.ty_arg) < generic_level && ctype::is_really_poly(env, arg.ty_arg))
        prerr_warning(app_loc, not_principal("applying a higher-rank function here"));
      struct R {
        const tt::Expression* arg;
        std::vector<TypeExpr*> vars;
      };
      R r = ctype::with_local_level_generalize(
          [&] {
            bool separate = clflags::principal || env::has_local_constraints(env);
            auto [vs, ty_arg3] = ctype::with_local_level_generalize_structure_if(
                separate, [&] { return ctype::instance_poly_fixed(vars, ty_arg2); });
            auto [ty_arg03, vars0] = tpoly_get_poly(arg.ty_arg0);
            auto [vs0, ty_arg04] = ctype::instance_poly_fixed(vars0, ty_arg03);
            if (vs.size() != vs0.size()) throw std::invalid_argument("List.iter2");
            for (std::size_t k = 0; k < vs.size(); ++k) ctype::unify_var(env, vs[k], vs0[k]);
            const tt::Expression* e = type_argument(env, arg.sarg, ty_arg3, ty_arg04);
            return R{e, vs0};
          },
          [&](const R& r) { may_lower_contravariant(env, r.arg); });
      check_univars(env, "argument", r.arg, arg.ty_arg, r.vars);
      tt::Expression* e2 = make<tt::Expression>(*r.arg);
      e2->exp_type = ctype::instance(r.arg->exp_type);
      return typed(e2);
    }
    case UK::Typed_arg: return a;
    case UK::Eliminated_optional_arg:
      return typed(option_none(env, ctype::instance(arg.ty_arg), location::none()));
  }
  throw std::logic_error("type_apply_arg");
}

std::pair<Slice<tt::LabeledArg>, TypeExpr*> type_application(
    env::t env, const Location& app_loc, const tt::Expression* funct,
    const std::vector<std::pair<ArgLabel, const pt::Expression*>>& sargs) {
  struct FilterArrowMonoFailed {};
  auto filter_arrow_mono = [&](TypeExpr* t, const ArgLabel& l) {
    auto r = ctype::filter_arrow(env, false, t, l, false);
    if (!r.ok) throw FilterArrowMonoFailed{};
    TypeExpr* tp = tpoly_get_mono_opt(r.value.ty_param);
    if (!tp) throw FilterArrowMonoFailed{};
    return ctype::FilteredArrow{tp, r.value.ty_ret};
  };
  auto is_ignore = [&]() {
    if (!is_prim("%ignore", funct)) return false;
    try {
      filter_arrow_mono(ctype::instance(funct->exp_type), ArgLabel::nolabel());
      return true;
    } catch (const FilterArrowMonoFailed&) {
      return false;
    }
  };
  // Special case for ignore: avoid discarding warning
  if (sargs.size() == 1 && sargs[0].first.kind == ArgLabel::Kind::Nolabel && is_ignore()) {
    ctype::FilteredArrow fa = ctype::with_local_level_generalize_structure_if_principal(
        [&] { return filter_arrow_mono(ctype::instance(funct->exp_type), ArgLabel::nolabel()); });
    const tt::Expression* exp = type_expect(env, sargs[0].second, mk_expected(fa.ty_param));
    check_partial_application(false, exp);
    std::vector<tt::LabeledArg> args{{ArgLabel::nolabel(), tt::ApplyArg{false, exp}}};
    return {slice(args), fa.ty_ret};
  }
  TypeExpr* ty = funct->exp_type;
  bool ignore_labels = clflags::classic;
  if (!ignore_labels) {
    auto [ls, tvar] = list_labels(env, ty);
    if (!tvar) {
      std::vector<ArgLabel> labels;
      for (auto& l : ls)
        if (!is_optional(l)) labels.push_back(l);
      bool all_nolabel = true;
      for (auto& s : sargs)
        if (s.first.kind != ArgLabel::Kind::Nolabel) all_nolabel = false;
      bool any_label = false;
      for (auto& l : labels)
        if (l.kind != ArgLabel::Kind::Nolabel) any_label = true;
      ignore_labels = labels.size() == sargs.size() && all_nolabel && any_label;
      if (ignore_labels) {
        std::vector<std::string> ls2;
        for (auto& l : labels)
          if (l.kind != ArgLabel::Kind::Nolabel) ls2.push_back(string_of_label(l));
        prerr_warning(funct->exp_loc, warnings::Warning::with_l(WK::Labels_omitted, ls2));
      }
    }
  }
  TypeExpr* ity = ctype::instance(ty);
  CollectedArgs ca = collect_apply_args(env, funct, ignore_labels, ty, ity, sargs);
  std::vector<UntypedArg> args;
  for (auto& a : ca.args) args.push_back(type_apply_arg(env, app_loc, a));
  auto [ty_ret, args2] = type_omitted_parameters_and_build_result_type(ca.ty_ret, args);
  std::vector<tt::LabeledArg> out;
  for (auto& a : args2) out.push_back({a.label, a.omitted ? tt::ApplyArg{true, nullptr} : tt::ApplyArg{false, a.arg.targ}});
  return {slice(out), ctype::instance(ty_ret)};
}

const tt::Expression* type_construct(env::t env, const pt::Expression* sexp, const pt::LidLoc& lid,
                                     const pt::Expression* sarg, const TypeExpected& ty_expected_explained) {
  TypeExpr* ty_expected = ty_expected_explained.ty;
  Explanation explanation = ty_expected_explained.explanation;
  std::optional<ExpectedTypePath> expected_type;
  VariantExtraction ve = extract_concrete_variant(env, ty_expected);
  switch (ve.kind) {
    case VariantExtraction::Kind::Variant_type:
      expected_type = ExpectedTypePath{ve.p0, ve.p, is_principal(ty_expected)};
      break;
    case VariantExtraction::Kind::Maybe_a_variant_type: break;
    case VariantExtraction::Kind::Not_a_variant_type: {
      Error e = err(sexp->pexp_loc, env, EK::Wrong_expected_kind);
      e.sort = wrong_kind_sort_of_constructor(lid.txt);
      e.ctx = WrongKindContext{false, explanation};
      e.ty = ty_expected;
      raise_error(e);
    }
  }
  env::LookupAllCstrs constrs =
      env::lookup_all_constructors(true, lid.loc, env::ConstructorUsage::Positive, lid.txt, env);
  const ConstructorDescription* constr =
      wrap_disambiguate("This variant expression is expected to have", ty_expected_explained, [&] {
        return disambiguate_constructor(env::ConstructorUsage::Positive, lid, env, expected_type, constrs);
      });
  std::vector<const pt::Expression*> sargs;
  if (sarg) {
    auto* t = as<pt::Pexp_tuple>(sarg->pexp_desc);
    if (t && (constr->cstr_arity > 1 || builtin_attributes::explicit_arity(sexp->pexp_attributes))) {
      for (auto& x : t->el) {
        if (x.label.some) raise_error(err(sexp->pexp_loc, env, EK::Constructor_labeled_arg));
        sargs.push_back(x.exp);
      }
    } else {
      sargs.push_back(sarg);
    }
  }
  if (static_cast<long>(sargs.size()) != constr->cstr_arity) {
    Error e = err(sexp->pexp_loc, env, EK::Constructor_arity_mismatch);
    e.lid = lid.txt;
    e.n1 = constr->cstr_arity;
    e.n2 = static_cast<long>(sargs.size());
    raise_error(e);
  }
  bool separate = clflags::principal || env::has_local_constraints(env);
  struct R {
    std::vector<TypeExpr*> ty_args;
    TypeExpr* ty_res;
    tt::Expression* texp;
  };
  R r = ctype::with_local_level_generalize_structure_if(separate, [&] {
    R r2 = ctype::with_local_level_generalize_structure_if(separate, [&] {
      ctype::InstancedConstructor ic = ctype::instance_constructor(ctype::ExistentialTreatment{}, constr);
      tt::Expression* texp = re(make<tt::Expression>(
          make<tt::Texp_construct>(tt::Texp_construct{{XK::Texp_construct}, lid, constr, {}}), sexp->pexp_loc,
          Slice<tt::ExpExtraItem>{}, ic.res, env, sexp->pexp_attributes));
      return R{ic.args, ic.res, texp};
    });
    with_explanation(explanation, [&] {
      TypeExpr* ie = ctype::instance(ty_expected);
      tt::Expression* t2 = make<tt::Expression>(*r2.texp);
      t2->exp_type = ctype::instance(r2.ty_res);
      unify_exp(sexp, env, t2, ie);
    });
    return r2;
  });
  std::vector<TypeExpr*> all{r.ty_res};
  all.insert(all.end(), r.ty_args.begin(), r.ty_args.end());
  std::vector<TypeExpr*> inst = ctype::instance_list(all);
  TypeExpr* ty_res = inst[0];
  std::vector<TypeExpr*> ty_args0(inst.begin() + 1, inst.end());
  tt::Expression* texp = make<tt::Expression>(*r.texp);
  texp->exp_type = ty_res;
  if (!separate) unify_exp(sexp, env, texp, ctype::instance(ty_expected));
  Recarg recarg = Recarg::Rejected;
  if (constr->cstr_inlined) {
    bool ok = false;
    if (sargs.size() == 1) {
      const pt::ExpressionDesc* d = sargs[0]->pexp_desc;
      if (d->kind == SXK::Pexp_ident) ok = true;
      else if (auto* rr = as<pt::Pexp_record>(d))
        ok = !rr->base || rr->base->pexp_desc->kind == SXK::Pexp_ident;
    }
    if (!ok) raise_error(err(sexp->pexp_loc, env, EK::Inlined_record_expected));
    recarg = Recarg::Required;
  }
  std::vector<const tt::Expression*> args;
  for (std::size_t k = 0; k < sargs.size(); ++k)
    args.push_back(type_argument_x(std::nullopt, recarg, env, sargs[k], r.ty_args[k], ty_args0[k]));
  if (constr->cstr_private == PrivateFlag::Private) {
    if (constr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension) {
      Error e = err(sexp->pexp_loc, env, EK::Private_constructor);
      e.cstr = constr;
      e.ty = ty_res;
      raise_error(e);
    }
    Error e = err(sexp->pexp_loc, env, EK::Private_type);
    e.ty = ty_res;
    raise_error(e);
  }
  // NOTE: shouldn't we call "re" on this final expression? -- AF
  texp->exp_desc = make<tt::Texp_construct>(tt::Texp_construct{{XK::Texp_construct}, lid, constr, slice(args)});
  return texp;
}

// Typing of statements (expressions whose values are discarded)
const tt::Expression* type_statement(Explanation explanation, env::t env, const pt::Expression* sexp) {
  // Raise the current level to detect non-returning functions
  return ctype::with_local_level_generalize(
      [&] { return type_exp(env, sexp); },
      [&](const tt::Expression* exp) {
        const tt::Expression* subexp = final_subexpression(exp);
        TypeExpr* ty = ctype::expand_head(env, exp->exp_type);
        // (-typing-recovery is never on in batch ocamlc: has_recovery_errors is false)
        if (is_Tvar(ty) && get_level(ty) > ctype::get_current_level() &&
            subexp->exp_desc->kind != XK::Texp_while)
          prerr_warning(subexp->exp_loc, WK::Nonreturning_statement);
        if (clflags::strict_sequence) {
          TypeExpr* expected_ty = ctype::instance(predef::type_unit());
          with_explanation(explanation, [&] { unify_exp(sexp, env, exp, expected_ty); });
        } else {
          check_partial_application(true, exp);
          ctype::enforce_current_level(env, ty);
        }
      });
}

}  // namespace cppcaml::typing::typecore
