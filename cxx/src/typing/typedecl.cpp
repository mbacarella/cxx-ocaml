// Port of typing/typedecl.ml, part 1: typing of type declarations
// (transl_declaration, transl_type_decl) and the checks on them
// (constraints, coherence, well-foundedness, regularity).  Shapes (cmt-only,
// and pure here) are not computed.  Warnings are not emitted.
#include "typedecl_internal.hpp"
#include "cppcaml/typing/location.hpp"

#include <map>
#include <set>

namespace cppcaml::typing::typedecl {

using namespace types;
using pt::as;

[[noreturn]] void raise_error(const Error& e) { throw e; }

// Some true / Some false / None
std::optional<bool> get_unboxed_from_attributes(const pt::TypeDeclaration* sdecl) {
  bool unboxed = builtin_attributes::has_attribute("unboxed", sdecl->ptype_attributes);
  bool boxed = builtin_attributes::has_attribute("boxed", sdecl->ptype_attributes);
  if (boxed && unboxed) raise_error(Error(sdecl->ptype_loc, EK::Boxed_and_unboxed));
  if (boxed) return false;
  if (unboxed) return true;
  return std::nullopt;
}

env::t add_type_attrs(bool check, Ident::t id, const TypeDeclaration* decl, env::t env) {
  return builtin_attributes::warning_scope(
      builtin_attributes::ast_attributes(decl->type_attributes), [&] { return env::add_type(check, id, decl, env); },
      false);
}

// Add a dummy type declaration to the environment, with the given arity.
// The [type_kind] is [Type_abstract], but there is a generic [type_manifest]
// for abbreviations, to allow polymorphic expansion, except if
// [abstract_abbrevs] is given along with a reason for not allowing expansion.
env::t enter_type(const std::optional<TypeOrigin>& abstract_abbrevs, RecFlag rec_flag, env::t env,
                  const pt::TypeDeclaration* sdecl, Ident::t id, Uid uid) {
  bool needed;
  if (rec_flag == RecFlag::Nonrecursive) {
    if (sdecl->ptype_kind.kind == pt::TypeKind::Kind::Ptype_variant)
      for (auto* cd : sdecl->ptype_kind.constructors)
        if (cd->pcd_res) raise_error(Error(cd->pcd_loc, EK::Nonrec_gadt));
    needed = btype::is_row_name(ident::name(id));
  } else {
    needed = true;
  }
  long arity = static_cast<long>(sdecl->ptype_params.size());
  if (!needed) return env;
  TypeOrigin abstract_source{};
  TypeExpr* type_manifest = nullptr;
  if (sdecl->ptype_manifest) {
    if (!abstract_abbrevs) type_manifest = ctype::newvar();
    else abstract_source = *abstract_abbrevs;
  }
  std::vector<TypeExpr*> params;
  for (std::size_t k = 0; k < sdecl->ptype_params.size(); ++k) params.push_back(btype::newgenvar());
  auto* kind = make<TypeKind>();
  kind->kind = TypeKind::Kind::Type_abstract;
  kind->origin = abstract_source;
  auto* decl = make<TypeDeclaration>();
  decl->type_params = slice(params);
  decl->type_arity = arity;
  decl->type_kind = kind;
  decl->type_private = sdecl->ptype_private;
  decl->type_manifest = type_manifest;
  decl->type_variance = slice(variance::unknown_signature(false, arity));
  decl->type_separability = slice(default_separability(arity));
  decl->type_is_newtype = false;
  decl->type_expansion_scope = btype::lowest_level;
  decl->type_loc = sdecl->ptype_loc;
  decl->type_attributes = parsetree::types_attributes(sdecl->ptype_attributes);
  decl->type_immediate = TypeImmediacy::Unknown;
  decl->type_unboxed_default = false;
  decl->type_uid = uid;
  return add_type_attrs(true, id, decl, env);
}

// Types.Separability.default_signature (Config.flat_float_array)
std::vector<Separability> default_separability(long arity) {
  return std::vector<Separability>(static_cast<std::size_t>(arity > 0 ? arity : 0), Separability::Deepsep);
}

// Determine if a type's values are represented by floats at run-time.
static bool is_float(env::t env, TypeExpr* ty) {
  TypeExpr* t2 = typedecl_unboxed::get_unboxed_type_representation(env, ty);
  if (!t2) return false;
  auto* tc = as<Tconstr>(get_desc(t2));
  return tc && path::same(tc->path, predef::paths().float_);
}

// Determine if a type definition defines a fixed type. (PW)
bool is_fixed_type(const pt::TypeDeclaration* sd) {
  std::function<bool(const pt::CoreType*)> has_row_var = [&](const pt::CoreType* sty) {
    const pt::CoreTypeDesc* d = sty->ptyp_desc;
    if (auto* a = as<pt::Ptyp_alias>(d)) return has_row_var(a->ty);
    if (d->kind == pt::CoreTypeDesc::Kind::Ptyp_class) return true;
    if (auto* o = as<pt::Ptyp_object>(d)) return o->closed == ClosedFlag::Open;
    if (auto* v = as<pt::Ptyp_variant>(d))
      return v->closed == ClosedFlag::Open || (v->closed == ClosedFlag::Closed && v->has_labels);
    return false;
  };
  if (!sd->ptype_manifest) return false;
  return sd->ptype_kind.kind == pt::TypeKind::Kind::Ptype_abstract && sd->ptype_private == PrivateFlag::Private &&
         has_row_var(sd->ptype_manifest);
}

// Set the row variable to a fixed type in a private row type declaration
// (requires [is_fixed_type decl])
void set_private_row(env::t env, const Location& loc, Path::t p, const TypeDeclaration* decl) {
  if (!decl->type_manifest) throw std::logic_error("set_private_row");
  TypeExpr* tm = ctype::expand_head(env, decl->type_manifest);
  TypeExpr* rv;
  const TypeDesc* d = get_desc(tm);
  if (auto* v = as<Tvariant>(d)) {
    const RowDesc* row = v->row;
    RowDescRepr r = row_repr(row);
    static const FixedExplanation fixed_private{FixedExplanation::Kind::Fixed_private};
    set_type_desc(tm, tvariant(create_row(slice(r.fields), r.more, r.closed, &fixed_private, r.name)));
    if (btype::static_row(row)) {
      // the syntax hinted at the existence of a row variable, but there is
      // in fact no row variable to make private
      Error e(loc, EK::Invalid_private_row_declaration);
      e.ty = tm;
      raise_error(e);
    }
    rv = r.more;
  } else if (auto* o = as<Tobject>(d)) {
    TypeExpr* r = ctype::flatten_fields(o->fields).second;
    if (!btype::is_Tvar(r)) {
      // a syntactically open object was closed by a constraint
      Error e(loc, EK::Invalid_private_row_declaration);
      e.ty = tm;
      raise_error(e);
    }
    rv = r;
  } else {
    throw std::logic_error("set_private_row: not a row");
  }
  set_type_desc(rv, tconstr(p, decl->type_params, make<MemoRef>(mnil())));
}

// Translate one type declaration
std::vector<tt::TypeParam> make_params(env::t env, Slice<pt::TypeParam> params) {
  std::vector<tt::TypeParam> out;
  for (auto& p : params) {
    try {
      out.push_back({typetexp::transl_type_param(env, p.ty), p.variance, p.injectivity});
    } catch (const typetexp::AlreadyBound&) {
      raise_error(Error(p.ty->ptyp_loc, EK::Repeated_parameter));
    }
  }
  return out;
}

static const pt::CoreType* force_poly(const pt::CoreType* t) {
  if (t->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly) return t;
  return make<pt::CoreType>(make<pt::Ptyp_poly>(pt::Ptyp_poly{{pt::CoreTypeDesc::Kind::Ptyp_poly}, {}, t}),
                            t->ptyp_loc, pt::LocationStack{}, pt::Attributes{});
}

std::pair<std::vector<const tt::TLabelDeclaration*>, std::vector<const LabelDeclaration*>> transl_labels(
    env::t env, const typetexp::ty_var_env::PolyUnivars* univars, bool closed,
    Slice<const pt::LabelDeclaration*> lbls) {
  if (lbls.empty()) throw std::logic_error("transl_labels");
  std::set<std::string_view> all_labels;
  for (auto* l : lbls) {
    if (all_labels.count(l->pld_name.txt)) {
      Error e(l->pld_name.loc, EK::Duplicate_label);
      e.name = std::string(l->pld_name.txt);
      raise_error(e);
    }
    all_labels.insert(l->pld_name.txt);
  }
  std::vector<const tt::TLabelDeclaration*> tl;
  for (auto* l : lbls) {
    builtin_attributes::warning_scope(l->pld_attributes, [&] {
      const pt::CoreType* arg = force_poly(l->pld_type);
      const tt::CoreType* cty = typetexp::transl_simple_type(env, univars, closed, arg);
      bool is_atomic = builtin_attributes::has_attribute("atomic", l->pld_attributes);
      bool is_mutable = l->pld_mutable == MutableFlag::Mutable;
      if (is_atomic && !is_mutable) {
        Error e(l->pld_loc, EK::Atomic_field_must_be_mutable);
        e.name = std::string(l->pld_name.txt);
        raise_error(e);
      }
      // (record fields right to left: ld_uid before ld_id)
      Uid uid = uid::mk(env::get_current_unit());
      Ident::t id = Ident::create_local(l->pld_name.txt);
      tl.push_back(make<tt::TLabelDeclaration>(id, l->pld_name, uid, l->pld_mutable,
                                               is_atomic ? AtomicFlag::Atomic : AtomicFlag::Nonatomic, cty,
                                               l->pld_loc, l->pld_attributes));
      return 0;
    });
  }
  std::vector<const LabelDeclaration*> tl2;
  for (auto* ld : tl) {
    TypeExpr* ty = ld->ld_type->ctyp_type;
    if (auto* p = as<Tpoly>(get_desc(ty)); p && p->vars.empty()) ty = p->body;
    tl2.push_back(make<LabelDeclaration>(ld->ld_id, ld->ld_mutable, ld->ld_atomic, ty, ld->ld_loc,
                                         parsetree::types_attributes(ld->ld_attributes), ld->ld_uid));
  }
  return {tl, tl2};
}

std::pair<ConstructorArguments, tt::TConstructorArguments> transl_constructor_arguments(
    env::t env, const typetexp::ty_var_env::PolyUnivars* univars, bool closed, const pt::ConstructorArguments& a) {
  if (a.kind == pt::ConstructorArguments::Kind::Pcstr_tuple) {
    std::vector<const tt::CoreType*> l;
    for (auto* sty : a.tuple) l.push_back(typetexp::transl_simple_type(env, univars, closed, sty));
    std::vector<TypeExpr*> tys;
    for (auto* c : l) tys.push_back(c->ctyp_type);
    ConstructorArguments args{ConstructorArguments::Kind::Cstr_tuple, slice(tys)};
    tt::TConstructorArguments targs{false, slice(l)};
    return {args, targs};
  }
  auto [lbls, lbls2] = transl_labels(env, univars, closed, a.record);
  ConstructorArguments args{ConstructorArguments::Kind::Cstr_record, {}, slice(lbls2)};
  tt::TConstructorArguments targs{true, {}, slice(lbls)};
  return {args, targs};
}

MadeConstructor make_constructor(env::t env, const Location& loc, Path::t type_path,
                                 const std::vector<TypeExpr*>& type_params, Slice<pt::StrLoc> svars,
                                 const pt::ConstructorArguments& sargs, const pt::CoreType* sret_type) {
  if (!sret_type) {
    auto [args, targs] = transl_constructor_arguments(env, nullptr, true, sargs);
    return {targs, nullptr, args, nullptr};
  }
  // if it's a generalized constructor we must first narrow and then widen
  // so as to not introduce any new constraints
  return typetexp::ty_var_env::with_local_scope([&]() -> MadeConstructor {
    bool closed = !svars.empty();
    struct R {
      tt::TConstructorArguments targs;
      const tt::CoreType* tret_type;
      ConstructorArguments args;
      TypeExpr* ret_type;
      typetexp::ty_var_env::PolyUnivars univars;
    };
    R r = ctype::with_local_level_generalize_if(closed, [&] {
      typetexp::ty_var_env::reset();
      std::vector<std::string_view> names;
      for (auto& v : svars) names.push_back(v.txt);
      typetexp::ty_var_env::PolyUnivars univar_list = typetexp::ty_var_env::make_poly_univars(names);
      const typetexp::ty_var_env::PolyUnivars* univars = closed ? &univar_list : nullptr;
      auto [args, targs] = transl_constructor_arguments(env, univars, closed, sargs);
      const tt::CoreType* tret_type = typetexp::transl_simple_type(env, univars, closed, sret_type);
      TypeExpr* ret_type = tret_type->ctyp_type;
      // TODO add back type_path as a parameter ? (typedecl.ml)
      auto* tc = as<Tconstr>(get_desc(ret_type));
      if (!(tc && path::same(type_path, tc->path))) {
        // Expansion is not helpful here -- the restriction on GADT return
        // types is purely syntactic.
        TypeExpr* expected = ctype::newconstr(type_path, slice(type_params));
        Error e(sret_type->ptyp_loc, EK::Constraint_failed);
        e.env = env;
        e.trace = et::UnificationError{{ctype::unexpanded_diff(ret_type, expected)}};
        raise_error(e);
      }
      return R{targs, tret_type, args, ret_type, univar_list};
    });
    if (closed) {
      typetexp::ty_var_env::instance_poly_univars(env, loc, r.univars);
      auto set_level = [&](TypeExpr* t) { ctype::enforce_current_level(env, t); };
      btype::iter_type_expr_cstr_args(set_level, r.args);
      set_level(r.ret_type);
    }
    return {r.targs, r.tret_type, r.args, r.ret_type};
  });
}

static std::vector<TypeExpr*> param_types(const std::vector<tt::TypeParam>& params) {
  std::vector<TypeExpr*> out;
  for (auto& p : params) out.push_back(p.ty->ctyp_type);
  return out;
}

static const tt::TTypeDeclaration* transl_declaration(env::t env, const pt::TypeDeclaration* sdecl, Ident::t id,
                                                      Uid uid) {
  // Bind type parameters
  typetexp::ty_var_env::reset();
  std::vector<tt::TypeParam> tparams = make_params(env, sdecl->ptype_params);
  std::vector<TypeExpr*> params = param_types(tparams);
  std::vector<tt::TypeConstraintItem> constraints;
  for (auto& c : sdecl->ptype_constraints) {
    // (transl sty, transl sty', loc): right to left
    const tt::CoreType* cty2 = typetexp::transl_simple_type(env, nullptr, false, c.t2);
    const tt::CoreType* cty = typetexp::transl_simple_type(env, nullptr, false, c.t1);
    constraints.push_back({cty, cty2, c.loc});
  }
  std::optional<bool> unboxed_attr = get_unboxed_from_attributes(sdecl);
  using PK = pt::TypeKind::Kind;
  const pt::TypeKind& sk = sdecl->ptype_kind;
  if (unboxed_attr && *unboxed_attr) {
    auto bad = [&](const char* msg) {
      Error e(sdecl->ptype_loc, EK::Bad_unboxed_attribute);
      e.name = msg;
      raise_error(e);
    };
    switch (sk.kind) {
      case PK::Ptype_abstract:
      case PK::Ptype_external: bad("it is abstract");
      case PK::Ptype_open: bad("extensible variant types cannot be unboxed");
      case PK::Ptype_record:
        if (sk.labels.empty()) bad("it has no fields");
        if (sk.labels.size() > 1) bad("it has more than one field");
        if (sk.labels[0]->pld_mutable == MutableFlag::Mutable) bad("it is mutable");
        break;
      case PK::Ptype_variant: {
        if (sk.constructors.empty()) bad("it has no constructor");
        if (sk.constructors.size() > 1) bad("it has more than one constructor");
        const pt::ConstructorArguments& a = sk.constructors[0]->pcd_args;
        if (a.kind == pt::ConstructorArguments::Kind::Pcstr_tuple) {
          if (a.tuple.empty()) bad("its constructor has no argument");
          if (a.tuple.size() > 1) bad("its constructor has more than one argument");
        } else {
          if (a.record.empty()) bad("its constructor has no fields");
          if (a.record.size() > 1) bad("its constructor has more than one field");
          if (a.record[0]->pld_mutable == MutableFlag::Mutable) bad("it is mutable");
        }
        break;
      }
    }
  }
  bool unbox = false, unboxed_default = false;
  {
    bool unboxable = false;
    if (sk.kind == PK::Ptype_variant && sk.constructors.size() == 1) {
      const pt::ConstructorArguments& a = sk.constructors[0]->pcd_args;
      unboxable = (a.kind == pt::ConstructorArguments::Kind::Pcstr_tuple && a.tuple.size() == 1) ||
                  (a.kind == pt::ConstructorArguments::Kind::Pcstr_record && a.record.size() == 1 &&
                   a.record[0]->pld_mutable == MutableFlag::Immutable);
    } else if (sk.kind == PK::Ptype_record && sk.labels.size() == 1 &&
               sk.labels[0]->pld_mutable == MutableFlag::Immutable) {
      unboxable = true;
    }
    if (unboxable) {
      unbox = unboxed_attr ? *unboxed_attr : clflags::unboxed_types;
      unboxed_default = !unboxed_attr;
    }  // else: Not unboxable, mark as boxed
  }
  tt::TTypeKind tkind{};
  auto* kind = make<TypeKind>();
  const TypeKind* abstract_kind = nullptr;  // typedecl.ml's `Type_abstract Definition` literal
  switch (sk.kind) {
    case PK::Ptype_abstract:
      tkind.kind = tt::TTypeKind::Kind::Ttype_abstract;
      abstract_kind = TYPE_ABSTRACT_LIT(Definition);
      break;
    case PK::Ptype_external:
      tkind.kind = tt::TTypeKind::Kind::Ttype_external;
      tkind.external = sk.external;
      kind->kind = TypeKind::Kind::Type_external;
      kind->external = sk.external;
      break;
    case PK::Ptype_variant: {
      bool gadt = false;
      for (auto* c : sk.constructors)
        if (c->pcd_res) gadt = true;
      if (gadt && !constraints.empty())
        location::prerr_warning(constraints[0].loc, warnings::Warning::make(warnings::Warning::K::Constraint_on_gadt));
      std::set<std::string_view> all_constrs;
      for (auto* c : sk.constructors) {
        if (all_constrs.count(c->pcd_name.txt)) {
          Error e(sdecl->ptype_loc, EK::Duplicate_constructor);
          e.name = std::string(c->pcd_name.txt);
          raise_error(e);
        }
        all_constrs.insert(c->pcd_name.txt);
      }
      long nonconst = 0;
      for (auto* c : sk.constructors)
        if (!(c->pcd_args.kind == pt::ConstructorArguments::Kind::Pcstr_tuple && c->pcd_args.tuple.empty()))
          ++nonconst;
      if (nonconst > max_tag + 1) raise_error(Error(sdecl->ptype_loc, EK::Too_many_constructors));
      std::vector<const tt::TConstructorDeclaration*> tcstrs;
      std::vector<const ConstructorDeclaration*> cstrs;
      for (auto* scstr : sk.constructors) {
        builtin_attributes::warning_scope(scstr->pcd_attributes, [&] {
          Ident::t name = Ident::create_local(scstr->pcd_name.txt);
          MadeConstructor mc = make_constructor(env, scstr->pcd_loc, Path::pident(id), params, scstr->pcd_vars,
                                                scstr->pcd_args, scstr->pcd_res);
          Uid cuid = uid::mk(env::get_current_unit());
          tcstrs.push_back(make<tt::TConstructorDeclaration>(name, scstr->pcd_name, cuid, scstr->pcd_vars, mc.targs,
                                                             mc.tret_type, scstr->pcd_loc, scstr->pcd_attributes));
          cstrs.push_back(make<ConstructorDeclaration>(name, mc.args, mc.ret_type, scstr->pcd_loc,
                                                       parsetree::types_attributes(scstr->pcd_attributes), cuid));
          return 0;
        });
      }
      tkind.kind = tt::TTypeKind::Kind::Ttype_variant;
      tkind.constructors = slice(tcstrs);
      kind->kind = TypeKind::Kind::Type_variant;
      kind->constructors = slice(cstrs);
      kind->variant_repr = unbox ? VariantRepresentation::Variant_unboxed : VariantRepresentation::Variant_regular;
      break;
    }
    case PK::Ptype_record: {
      auto [lbls, lbls2] = transl_labels(env, nullptr, true, sk.labels);
      RecordRepresentation rep{};
      if (unbox) {
        rep.kind = RecordRepresentation::Kind::Record_unboxed;
        rep.unboxed_inlined = false;
        static const char record_unboxed_false = 0;  // a static structured constant
        rep.obj = &record_unboxed_false;
      } else {
        bool all_float = true;
        for (auto* l : lbls2) all_float = all_float && is_float(env, l->ld_type) && l->ld_atomic == AtomicFlag::Nonatomic;
        rep.kind = all_float ? RecordRepresentation::Kind::Record_float : RecordRepresentation::Kind::Record_regular;
      }
      tkind.kind = tt::TTypeKind::Kind::Ttype_record;
      tkind.labels = slice(lbls);
      kind->kind = TypeKind::Kind::Type_record;
      kind->labels = slice(lbls2);
      kind->record_repr = rep;
      break;
    }
    case PK::Ptype_open:
      tkind.kind = tt::TTypeKind::Kind::Ttype_open;
      kind->kind = TypeKind::Kind::Type_open;
      break;
  }
  const tt::CoreType* tman = nullptr;
  TypeExpr* man = nullptr;
  if (sdecl->ptype_manifest) {
    bool no_row = !is_fixed_type(sdecl);
    tman = typetexp::transl_simple_type(env, nullptr, no_row, sdecl->ptype_manifest);
    man = tman->ctyp_type;
  }
  long arity = static_cast<long>(params.size());
  auto* decl = make<TypeDeclaration>();
  decl->type_params = slice(params);
  decl->type_arity = arity;
  decl->type_kind = abstract_kind ? abstract_kind : kind;
  decl->type_private = sdecl->ptype_private;
  decl->type_manifest = man;
  decl->type_variance = slice(variance::unknown_signature(false, arity));
  decl->type_separability = slice(default_separability(arity));
  decl->type_is_newtype = false;
  decl->type_expansion_scope = btype::lowest_level;
  decl->type_loc = sdecl->ptype_loc;
  decl->type_attributes = parsetree::types_attributes(sdecl->ptype_attributes);
  decl->type_immediate = TypeImmediacy::Unknown;
  decl->type_unboxed_default = unboxed_default;
  decl->type_uid = uid;
  // Check constraints
  for (auto& c : constraints) {
    try {
      ctype::unify(env, c.t1->ctyp_type, c.t2->ctyp_type);
    } catch (const ctype::Unify& u) {
      Error e(c.loc, EK::Inconsistent_constraint);
      e.env = env;
      e.trace = u.err;
      raise_error(e);
    }
  }
  // Add abstract row
  if (is_fixed_type(sdecl)) {
    Path::t p;
    try {
      p = env::find_type_by_name(Longident::lident(zborrow(std::string(ident::name(id)) + "#row")), env).first;
    } catch (const env::NotFound&) {
      throw std::logic_error("transl_declaration: #row");
    }
    set_private_row(env, sdecl->ptype_loc, p, decl);
  }
  return make<tt::TTypeDeclaration>(id, sdecl->ptype_name, slice(tparams), decl, slice(constraints), tkind,
                                    sdecl->ptype_private, tman, sdecl->ptype_loc, sdecl->ptype_attributes);
}

// ---- check that all constraints are enforced ---------------------------------------------
namespace {
struct TypeSet {  // Btype.TypeSet, by the id of the representative
  std::set<long> s;
  bool mem(TypeExpr* t) const { return s.count(get_id(t)) != 0; }
  void add(TypeExpr* t) { s.insert(get_id(t)); }
};
}  // namespace

static void check_constraints_rec(env::t env, const Location& loc, TypeSet& visited, TypeExpr* ty) {
  if (visited.mem(ty)) return;
  visited.add(ty);
  const TypeDesc* d = btype::get_constr_desc(ty);
  if (auto* tc = as<Tconstr>(d)) {
    const TypeDeclaration* decl;
    try {
      decl = env::find_type(tc->path, env);
    } catch (const env::NotFound&) {
      Error e(loc, EK::Unavailable_type_constructor);
      e.path = tc->path;
      raise_error(e);
    }
    std::vector<TypeExpr*> ps(decl->type_params.begin(), decl->type_params.end());
    TypeExpr* ty2 = ctype::newconstr(tc->path, slice(ctype::instance_list(ps)));
    // We don't expand the error trace (constraint errors).
    try {
      ctype::matches(false, env, ty, ty2);
    } catch (const ctype::MatchesFailure& m) {
      Error e(loc, EK::Constraint_failed);
      e.env = m.env;
      e.trace = m.err;
      raise_error(e);
    }
    for (TypeExpr* a : tc->args) check_constraints_rec(env, loc, visited, a);
    return;
  }
  if (auto* f = as<Tfunctor>(d)) {
    for (auto& c : f->pack->pack_constraints) check_constraints_rec(env, loc, visited, c.ty);
    auto [env2, ty2] = ctype::open_tfunctor(env, loc, f->id, f->pack, f->body);
    check_constraints_rec(env2, loc, visited, ty2);
    return;
  }
  if (auto* p = as<Tpoly>(d)) {
    check_constraints_rec(env, loc, visited, ctype::instance_poly(p->vars, p->body));
    return;
  }
  btype::iter_type_expr([&](TypeExpr* t) { check_constraints_rec(env, loc, visited, t); }, ty);
}

static void check_constraints_labels(env::t env, TypeSet& visited, Slice<const LabelDeclaration*> l,
                                     Slice<const pt::LabelDeclaration*> pl) {
  auto get_loc = [&](std::string_view name) -> Location {
    for (auto* pld : pl)
      if (name == pld->pld_name.txt) return pld->pld_type->ptyp_loc;
    throw std::logic_error("check_constraints_labels");
  };
  for (auto* ld : l) check_constraints_rec(env, get_loc(ident::name(ld->ld_id)), visited, ld->ld_type);
}

static void check_constraints(env::t env, const pt::TypeDeclaration* sdecl, const TypeDeclaration* decl) {
  TypeSet visited;
  if (sdecl->ptype_params.size() != decl->type_params.size()) throw std::invalid_argument("List.iter2");
  for (std::size_t k = 0; k < decl->type_params.size(); ++k)
    check_constraints_rec(env, sdecl->ptype_params[k].ty->ptyp_loc, visited, decl->type_params[k]);
  const TypeKind* k = decl->type_kind;
  if (k->kind == TypeKind::Kind::Type_variant) {
    if (sdecl->ptype_kind.kind != pt::TypeKind::Kind::Ptype_variant) throw std::logic_error("check_constraints");
    Slice<const pt::ConstructorDeclaration*> pl = sdecl->ptype_kind.constructors;
    // String.Map: the last constructor of a name wins
    std::map<std::string_view, const pt::ConstructorDeclaration*> pl_index;
    for (auto* x : pl) pl_index[x->pcd_name.txt] = x;
    for (auto* cd : k->constructors) {
      auto it = pl_index.find(ident::name(cd->cd_id));
      if (it == pl_index.end()) throw std::logic_error("check_constraints: constructor");
      const pt::ConstructorDeclaration* pcd = it->second;
      if (cd->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple &&
          pcd->pcd_args.kind == pt::ConstructorArguments::Kind::Pcstr_tuple) {
        if (cd->cd_args.tuple.size() != pcd->pcd_args.tuple.size()) throw std::invalid_argument("List.iter2");
        for (std::size_t j = 0; j < cd->cd_args.tuple.size(); ++j)
          check_constraints_rec(env, pcd->pcd_args.tuple[j]->ptyp_loc, visited, cd->cd_args.tuple[j]);
      } else if (cd->cd_args.kind == ConstructorArguments::Kind::Cstr_record &&
                 pcd->pcd_args.kind == pt::ConstructorArguments::Kind::Pcstr_record) {
        check_constraints_labels(env, visited, cd->cd_args.record, pcd->pcd_args.record);
      } else {
        throw std::logic_error("check_constraints: arguments");
      }
      if (pcd->pcd_res && cd->cd_res) check_constraints_rec(env, pcd->pcd_res->ptyp_loc, visited, cd->cd_res);
    }
  } else if (k->kind == TypeKind::Kind::Type_record) {
    if (sdecl->ptype_kind.kind != pt::TypeKind::Kind::Ptype_record) throw std::logic_error("check_constraints");
    check_constraints_labels(env, visited, k->labels, sdecl->ptype_kind.labels);
  }
  if (decl->type_manifest) {
    if (!sdecl->ptype_manifest) throw std::logic_error("check_constraints: manifest");
    check_constraints_rec(env, sdecl->ptype_manifest->ptyp_loc, visited, decl->type_manifest);
  }
}

// If both a variant/record definition and a type equation are given, need
// to check that the equation refers to a type of the same kind with the
// same constructors and labels.
void check_coherence(env::t env, const Location& loc, Path::t dpath, const TypeDeclaration* decl) {
  TypeKind::Kind k = decl->type_kind->kind;
  if (!((k == TypeKind::Kind::Type_variant || k == TypeKind::Kind::Type_record || k == TypeKind::Kind::Type_open) &&
        decl->type_manifest))
    return;
  TypeExpr* ty = decl->type_manifest;
  auto* tc = as<Tconstr>(btype::get_constr_desc(ty));
  if (!tc) {
    Error e(loc, EK::Definition_mismatch);
    e.ty = ty;
    e.env = env;
    raise_error(e);
  }
  std::optional<includecore::TypeMismatch> err;
  try {
    const TypeDeclaration* decl2 = env::find_type(tc->path, env);
    if (tc->args.size() != decl->type_params.size()) {
      err = includecore::TypeMismatch{includecore::TypeMismatch::Kind::Arity};
    } else {
      bool eq = true;
      try {
        ctype::equal(env, false, tc->args, decl->type_params);
      } catch (const ctype::Equality& q) {
        includecore::TypeMismatch m{includecore::TypeMismatch::Kind::Constraint};
        m.err = q.err;
        err = m;
        eq = false;
      }
      if (eq) {
        subst::t s = subst::unsafe::add_type_path(dpath, tc->path, subst::identity());
        const TypeDeclaration* d = subst::type_declaration(s, decl);
        err = includecore::type_declarations(true, loc, env, true, std::string(path::last(tc->path)), decl2, dpath, d);
      }
    }
  } catch (const env::NotFound&) {
    Error e(loc, EK::Unavailable_type_constructor);
    e.path = tc->path;
    raise_error(e);
  }
  if (err) {
    Error e(loc, EK::Definition_mismatch);
    e.ty = ty;
    e.env = env;
    e.mismatch = err;
    raise_error(e);
  }
}

static void check_abbrev(env::t env, const pt::TypeDeclaration* sdecl, Ident::t id, const TypeDeclaration* decl) {
  check_coherence(env, sdecl->ptype_loc, Path::pident(id), decl);
}

// ---- well-foundedness (see the notes of typedecl.ml) ---------------------------------------
// The reaching trace is a vector in the order steps were taken (the OCaml
// list reversed).
using Trace = ReachingTypePath;
static Trace cons_trace(const Trace& t, ReachingTypeStep s) {
  Trace r = t;
  r.push_back(s);
  return r;
}
static ReachingTypeStep contains(TypeExpr* a, TypeExpr* b) {
  return {ReachingTypeStep::Kind::Contains, a, b};
}

namespace {
struct Reach {
  env::t abs_env, final_env;
  const std::function<bool(Path::t)>& is_decl_path;
  const Location& loc;
  Path::t ty_path;
  TypeSet visited;
  std::vector<std::pair<Path::t, std::vector<TypeExpr*>>> visited_paths;  // Path.Map, lists head first

  [[noreturn]] void raise_error_(const Trace& trace) {
    Path::t path = ty_path;
    bool rec_abbrev = false;
    if (!trace.empty() && trace.back().kind == ReachingTypeStep::Kind::Expands_to) {
      auto* tc = as<Tconstr>(btype::get_constr_desc(trace.back().t1));
      rec_abbrev = tc && path::same(tc->path, path);
    }
    Error e(loc, rec_abbrev ? EK::Recursive_abbrev : EK::Cycle_in_def);
    e.name = path::name(path);
    e.env = abs_env;
    e.reaching_path = trace;
    raise_error(e);
  }
  void unguarded(const Trace& trace, TypeExpr* ty2) {
    if (visited.mem(ty2)) return;
    auto* tc = as<Tconstr>(btype::get_constr_desc(ty2));
    if (tc) {
      for (auto& [p, tys] : visited_paths)
        if (path::same(p, tc->path)) {
          for (TypeExpr* ty3 : tys) {
            std::vector<TypeExpr*> a{ty2}, b{ty3};
            if (ctype::is_equal(abs_env, false, slice(a), slice(b))) return;
          }
          break;
        }
    }
    if (tc && path::same(tc->path, ty_path)) raise_error_(trace);
    unguarded_no_self(trace, ty2);
  }
  void unguarded_no_self(const Trace& trace, TypeExpr* ty2) {
    visited.add(ty2);
    if (auto* tc = as<Tconstr>(btype::get_constr_desc(ty2)); tc && is_decl_path(tc->path)) {
      bool found = false;
      for (auto& [p, tys] : visited_paths)
        if (path::same(p, tc->path)) {
          tys.insert(tys.begin(), ty2);
          found = true;
          break;
        }
      if (!found) visited_paths.push_back({tc->path, {ty2}});
    }
    reachable(trace, ty2);
  }
  void rectypes_guarded(const Trace& trace, TypeExpr* ty2) {
    if (clflags::recursive_types) return;
    unguarded(trace, ty2);
  }
  ctype::FindTypeExpansion restrict_type_expansion(Path::t root_path_to_expand) {
    return [this, root_path_to_expand](Path::t path, env::t) {
      bool idp = is_decl_path(path);
      bool should_not_expand = path::same(path, ty_path) || (idp && !path::same(path, root_path_to_expand));
      // Always expand private abbreviations
      if (should_not_expand) return env::find_type_expansion(path, abs_env);
      return env::find_type_expansion_opt(path, final_env);
    };
  }
  // We must use get_desc here (not get_constr_desc).
  void reachable(const Trace& trace, TypeExpr* ty) {
    const TypeDesc* d = get_desc(ty);
    switch (d->kind) {
      case DescKind::Tobject:
      case DescKind::Tfield:
      case DescKind::Tnil:
      case DescKind::Tvariant:
      case DescKind::Tvar:
      case DescKind::Tunivar: return;
      case DescKind::Tarrow: {
        auto* a = as<Tarrow>(d);
        rectypes_guarded(cons_trace(trace, contains(ty, a->t1)), a->t1);
        rectypes_guarded(cons_trace(trace, contains(ty, a->t2)), a->t2);
        return;
      }
      case DescKind::Ttuple:
        for (auto& x : as<Ttuple>(d)->elems) rectypes_guarded(cons_trace(trace, contains(ty, x.ty)), x.ty);
        return;
      case DescKind::Tconstr: {
        auto* tc = as<Tconstr>(d);
        if (ctype::is_contractive(final_env, tc->path)) {
          for (TypeExpr* t : tc->args) rectypes_guarded(cons_trace(trace, contains(ty, t)), t);
          return;
        }
        TypeExpr* ty2;
        try {
          // Expansion can trigger unification, so we need to use an
          // abstract environment to avoid any cycles.
          ty2 = ctype::try_expand_once_gen_nolink(restrict_type_expansion(tc->path), abs_env, ty);
        } catch (const ctype::CannotExpand&) {
          // Abstract
          ReachingTypeStep ca{ReachingTypeStep::Kind::Considered_abstract};
          ca.path = tc->path;
          Trace t2 = cons_trace(trace, ca);
          for (TypeExpr* t : tc->args) unguarded(cons_trace(t2, contains(ty, t)), t);
          return;
        }
        unguarded(cons_trace(trace, {ReachingTypeStep::Kind::Expands_to, ty, ty2}), ty2);
        return;
      }
      case DescKind::Tpoly: {
        TypeExpr* t = as<Tpoly>(d)->body;
        unguarded(cons_trace(trace, contains(ty, t)), t);
        return;
      }
      case DescKind::Tfunctor: {
        auto* f = as<Tfunctor>(d);
        rectypes_guarded(cons_trace(trace, contains(ty, f->body)), f->body);
        for (auto& c : f->pack->pack_constraints) rectypes_guarded(cons_trace(trace, contains(ty, c.ty)), c.ty);
        return;
      }
      case DescKind::Tpackage:
        for (auto& c : as<Tpackage>(d)->pack->pack_constraints)
          rectypes_guarded(cons_trace(trace, contains(ty, c.ty)), c.ty);
        return;
      default: throw std::runtime_error("Tsubst");  // failwith "Tsubst"
    }
  }
};
}  // namespace

static void is_reachable(const Trace& trace, const std::function<bool(Path::t)>& is_decl_path, env::t abs_env,
                         env::t final_env, const Location& loc, TypeExpr* from_ty, Path::t ty_path) {
  Snapshot snap = btype::snapshot();
  try {
    ctype::wrap_trace_gadt_instances(final_env, [&] {
      Reach r{abs_env, final_env, is_decl_path, loc, ty_path, {}, {}};
      r.unguarded(trace, from_ty);
    });
  } catch (const ctype::Escape&) {
    // Will be detected by check_regularity
    btype::backtrack(snap);
  }
}

// Given a new type declaration, we check that accepting the declaration
// does not introduce ill-founded types (no cycle through the "root" of the
// declaration).
void check_well_founded_decl(env::t abs_env, env::t final_env, const std::function<bool(Path::t)>& is_decl_path,
                             const Location& loc, Path::t path, const TypeDeclaration* decl) {
  const TypeDeclaration* declaration = ctype::generic_instance_declaration(decl);
  for (std::size_t i = 0; i < declaration->type_params.size(); ++i) {
    TypeExpr* from_ty = declaration->type_params[i];
    ReachingTypeStep s{ReachingTypeStep::Kind::Parameter, from_ty};
    s.path = path;
    s.n = static_cast<long>(i);
    is_reachable({s}, is_decl_path, abs_env, final_env, loc, from_ty, path);
  }
  if (TypeExpr* from_ty = declaration->type_manifest) {
    TypeExpr* ty = ctype::newconstr(path, declaration->type_params);
    is_reachable({ReachingTypeStep{ReachingTypeStep::Kind::Expands_to, ty, from_ty}}, is_decl_path, abs_env,
                 final_env, loc, from_ty, path);
  }
}

// Check for non-regular abbreviations
static void check_regularity(env::t abs_env, env::t env, const Location& loc, Path::t path,
                             const TypeDeclaration* decl, const std::function<bool(Path::t)>& to_check) {
  // to_check is true for potentially mutually recursive paths.
  if (decl->type_params.empty()) return;
  TypeSet visited;
  std::function<void(const std::vector<TypeExpr*>&, const std::vector<Path::t>&, const Trace&, TypeExpr*)>
      check_regular;
  auto check_subtype = [&](const std::vector<TypeExpr*>& args, const std::vector<Path::t>& prev_exp,
                           const Trace& trace, TypeExpr* outer_ty, TypeExpr* inner_ty) {
    check_regular(args, prev_exp, cons_trace(trace, contains(outer_ty, inner_ty)), inner_ty);
  };
  check_regular = [&](const std::vector<TypeExpr*>& args, const std::vector<Path::t>& prev_exp, const Trace& trace,
                      TypeExpr* ty) {
    if (visited.mem(ty)) return;
    visited.add(ty);
    const TypeDesc* d = btype::get_constr_desc(ty);
    if (auto* tc = as<Tconstr>(d)) {
      if (path::same(path, tc->path)) {
        if (!ctype::is_equal(abs_env, false, slice(args), tc->args)) {
          Error e(loc, EK::Non_regular);
          e.path = path;
          e.ty = ty;
          e.ty2 = ctype::newconstr(path, slice(args));
          e.reaching_path = trace;
          raise_error(e);
        }
      } else if (to_check(tc->path)) {
        // Attempt to expand a type abbreviation if [to_check path'] holds
        // and we haven't expanded this type constructor before.
        bool seen = false;
        for (Path::t p : prev_exp) seen = seen || path::same(p, tc->path);
        if (!seen) {
          try {
            // Attempt expansion
            env::TypeExpansion te = env::find_type_expansion(tc->path, env);
            auto [params, body] = ctype::instance_parameterized_type(te.params, te.body);
            try {
              if (tc->args.size() != params.size()) throw std::invalid_argument("List.iter2");
              for (std::size_t k = 0; k < params.size(); ++k) ctype::unify(abs_env, tc->args[k], params[k]);
            } catch (const ctype::Unify& u) {
              Error e(loc, EK::Constraint_failed);
              e.env = abs_env;
              e.trace = u.err;
              raise_error(e);
            }
            std::vector<Path::t> prev2{tc->path};
            prev2.insert(prev2.end(), prev_exp.begin(), prev_exp.end());
            check_regular(args, prev2, cons_trace(trace, {ReachingTypeStep::Kind::Expands_to, ty, body}), body);
          } catch (const env::NotFound&) {
          }
        }
      }
      for (TypeExpr* a : tc->args) check_subtype(args, prev_exp, trace, ty, a);
      return;
    }
    if (auto* p = as<Tpoly>(d)) {
      check_regular(args, prev_exp, trace, ctype::instance_poly(p->vars, p->body, true));
      return;
    }
    btype::iter_type_expr([&](TypeExpr* t) { check_subtype(args, prev_exp, trace, ty, t); }, ty);
  };
  if (decl->type_manifest) {
    auto [args, body] = ctype::instance_parameterized_type(decl->type_params, decl->type_manifest, true);
    for (TypeExpr* a : args) check_regular(args, {}, {}, a);
    check_regular(args, {}, {}, body);
  }
}

// Force recursion to go through id for private types
const TypeDeclaration* name_recursion(const pt::TypeDeclaration* sdecl, Ident::t id, const TypeDeclaration* decl) {
  if (decl->type_kind->kind == TypeKind::Kind::Type_abstract && decl->type_manifest &&
      decl->type_private == PrivateFlag::Private && is_fixed_type(sdecl)) {
    TypeExpr* ty = decl->type_manifest;
    TypeExpr* ty2 = btype::newty2(get_level(ty), get_desc(ty));
    if (btype::deep_occur(ty, ty2)) {
      const TypeDesc* td = tconstr(Path::pident(id), decl->type_params, make<MemoRef>(mnil()));
      link_type(ty, btype::newty2(get_level(ty), td));
      TypeDeclaration* d = make<TypeDeclaration>(*decl);
      d->type_manifest = ty2;
      return d;
    }
  }
  return decl;
}

// Update a temporary definition to share recursion
static void update_type(env::t temp_env, env::t env, Ident::t id, const Location& loc) {
  Path::t path = Path::pident(id);
  const TypeDeclaration* decl = env::find_type(path, temp_env);
  TypeExpr* ty = decl->type_manifest;
  if (!ty) return;
  // Since this function is called after generalizing declarations, ty is at
  // the generic level: unify without instantiating, but generalize again.
  ctype::with_local_level_generalize([&] {
    std::vector<TypeExpr*> params;
    for (std::size_t k = 0; k < decl->type_params.size(); ++k) params.push_back(ctype::newvar());
    try {
      ctype::unify(env, ctype::newconstr(path, slice(params)), ty);
    } catch (const ctype::Unify& u) {
      Error e(loc, EK::Type_clash);
      e.env = env;
      e.trace = u.err;
      raise_error(e);
    }
    return 0;
  });
}

static env::t add_types_to_env(const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls, env::t env) {
  // List.fold_right2
  for (std::size_t k = decls.size(); k-- > 0;) env = add_type_attrs(true, decls[k].first, decls[k].second, env);
  return env;
}

// Translate a set of type declarations, mutually recursive or not
// Warn on definitions of type "type foo = ()" which redefine a different
// unit type and are likely a mistake.
static void check_redefined_unit(const pt::TypeDeclaration* td) {
  if (!td->ptype_manifest && td->ptype_kind.kind == pt::TypeKind::Kind::Ptype_variant &&
      td->ptype_kind.constructors.size() == 1 && td->ptype_kind.constructors[0]->pcd_name.txt == "()")
    location::prerr_warning(td->ptype_loc, warnings::Warning::with_s(warnings::Warning::K::Redefining_unit,
                                                                      std::string(td->ptype_name.txt)));
}

static void check_duplicates(const std::vector<const pt::TypeDeclaration*>& sdecl_list) {
  std::map<std::string_view, std::string_view> labels, constrs;
  auto warn = [](const Location& loc, const char* kind, std::string_view name, std::string_view n1,
                 std::string_view n2) {
    warnings::Warning w = warnings::Warning::make(warnings::Warning::K::Duplicate_definitions);
    w.s = kind;
    w.s2 = std::string(name);
    w.s3 = std::string(n1);
    w.s4 = std::string(n2);
    location::prerr_warning(loc, w);
  };
  for (auto* sdecl : sdecl_list) {
    if (sdecl->ptype_kind.kind == pt::TypeKind::Kind::Ptype_variant) {
      for (auto* pcd : sdecl->ptype_kind.constructors) {
        auto it = constrs.find(pcd->pcd_name.txt);
        if (it != constrs.end()) warn(pcd->pcd_loc, "constructor", pcd->pcd_name.txt, it->second, sdecl->ptype_name.txt);
        else constrs[pcd->pcd_name.txt] = sdecl->ptype_name.txt;
      }
    } else if (sdecl->ptype_kind.kind == pt::TypeKind::Kind::Ptype_record) {
      for (auto* ld : sdecl->ptype_kind.labels) {
        auto it = labels.find(ld->pld_name.txt);
        if (it != labels.end()) warn(ld->pld_loc, "label", ld->pld_name.txt, it->second, sdecl->ptype_name.txt);
        else labels[ld->pld_name.txt] = sdecl->ptype_name.txt;
      }
    }
  }
}

TranslTypeDeclResult transl_type_decl(env::t env, RecFlag rec_flag, Slice<const pt::TypeDeclaration*> sdecl_list0) {
  for (auto* sd : sdecl_list0) check_redefined_unit(sd);
  // Add dummy types for fixed rows
  std::vector<const pt::TypeDeclaration*> sdecl_list;
  for (auto* sdecl : sdecl_list0) {
    if (!is_fixed_type(sdecl)) continue;
    auto* d = make<pt::TypeDeclaration>(*sdecl);
    Location nloc = sdecl->ptype_name.loc;
    nloc.loc_ghost = true;
    d->ptype_name = pt::StrLoc{zborrow(std::string(sdecl->ptype_name.txt) + "#row"), nloc};
    d->ptype_kind = pt::TypeKind{pt::TypeKind::Kind::Ptype_abstract};
    d->ptype_manifest = nullptr;
    d->ptype_loc.loc_ghost = true;
    sdecl_list.push_back(d);
  }
  sdecl_list.insert(sdecl_list.end(), sdecl_list0.begin(), sdecl_list0.end());
  // Create identifiers.
  long scope = ctype::create_scope();
  std::vector<std::pair<Ident::t, Uid>> ids_list;
  for (auto* sdecl : sdecl_list) {
    // (Ident.create_scoped .., Uid.mk ..): right to left
    Uid uid = uid::mk(env::get_current_unit());
    ids_list.push_back({Ident::create_scoped(static_cast<int>(scope), sdecl->ptype_name.txt), uid});
  }
  // Translate declarations, using a temporary environment where
  // abbreviations expand to a generic type variable.
  struct R {
    std::vector<const tt::TTypeDeclaration*> tdecls;
    env::t temp_env;
  };
  R r = ctype::with_local_level_generalize([&] {
    // Enter types.
    env::t temp_env = env;
    for (std::size_t k = 0; k < sdecl_list.size(); ++k)
      temp_env = enter_type(std::nullopt, rec_flag, temp_env, sdecl_list[k], ids_list[k].first, ids_list[k].second);
    // Translate each declaration.
    using Slot = std::shared_ptr<std::vector<Uid>>;
    auto current_slot = std::make_shared<Slot>();
    bool warn_unused = warnings::is_active(34);
    std::vector<Slot> slots;
    for (std::size_t k = 0; k < sdecl_list.size(); ++k) {
      Slot slot;
      if (rec_flag == RecFlag::Recursive && warn_unused) {
        // See typecore.ml for a description of the algorithm used to
        // detect unused declarations in a set of recursive definitions.
        slot = std::make_shared<std::vector<Uid>>();
        const TypeDeclaration* td = env::find_type(Path::pident(ids_list[k].first), temp_env);
        Uid tuid = td->type_uid;
        env::set_type_used_callback(td, [current_slot, slot, tuid](std::function<void()> old_callback) {
          if (*current_slot) {
            (*current_slot)->insert((*current_slot)->begin(), tuid);
          } else {
            std::vector<Uid> l = *slot;  // get_ref slot
            slot->clear();
            for (const Uid& u : l) env::mark_type_used(u);
            old_callback();
          }
        });
      }
      slots.push_back(slot);
    }
    std::vector<const tt::TTypeDeclaration*> tdecls;
    for (std::size_t k = 0; k < sdecl_list.size(); ++k) {
      *current_slot = slots[k];
      tdecls.push_back(builtin_attributes::warning_scope(sdecl_list[k]->ptype_attributes, [&] {
        return transl_declaration(temp_env, sdecl_list[k], ids_list[k].first, ids_list[k].second);
      }));
    }
    *current_slot = nullptr;
    // Check for duplicates
    check_duplicates(sdecl_list);
    return R{tdecls, temp_env};
  });
  // Copy the type declarations to remove spurious expansions
  std::vector<const tt::TTypeDeclaration*> tdecls;
  std::vector<std::pair<Ident::t, const TypeDeclaration*>> decls;
  for (auto* td : r.tdecls) {
    const TypeDeclaration* decl = subst::type_declaration(subst::identity(), td->typ_type);
    auto* t2 = make<tt::TTypeDeclaration>(*td);
    t2->typ_type = decl;
    tdecls.push_back(t2);
    decls.push_back({td->typ_id, decl});
  }
  // Build the final env.
  env::t new_env = add_types_to_env(decls, env);
  // Check for ill-formed abbrevs
  std::vector<std::pair<Ident::t, Location>> id_loc_list;
  for (std::size_t k = 0; k < ids_list.size(); ++k) id_loc_list.push_back({ids_list[k].first, sdecl_list[k]->ptype_loc});
  auto assoc = [&](Ident::t id) -> const Location& {
    for (auto& [i, l] : id_loc_list)
      if (ident::same(i, id)) return l;
    throw std::logic_error("List.assoc");
  };
  // [check_abbrev_regularity] and error messages cannot use the new
  // environment (non-termination): a completely abstract version of the
  // temporary environment is used (#12334, #12368)
  env::t abs_env = env;
  {
    TypeOrigin rcr{TypeOrigin::Kind::Rec_check_regularity};
    for (std::size_t k = 0; k < sdecl_list.size(); ++k)
      abs_env = enter_type(rcr, rec_flag, abs_env, sdecl_list[k], ids_list[k].first, ids_list[k].second);
  }
  std::function<bool(Path::t)> to_check = [&](Path::t p) {
    if (p->kind != Path::Kind::Pident) return false;
    for (auto& [i, l] : id_loc_list)
      if (ident::same(i, p->id)) return true;
    return false;
  };
  for (auto& [id, decl] : decls) check_well_founded_decl(abs_env, new_env, to_check, assoc(id), Path::pident(id), decl);
  for (auto* td : tdecls)  // check_abbrev_regularity
    check_regularity(abs_env, new_env, assoc(td->typ_id), Path::pident(td->typ_id), td->typ_type, to_check);
  // Update temporary definitions (for well-founded recursive types)
  if (rec_flag == RecFlag::Recursive)
    for (std::size_t k = 0; k < ids_list.size(); ++k)
      update_type(r.temp_env, new_env, ids_list[k].first, sdecl_list[k]->ptype_loc);
  // Check that all type variables are closed
  for (std::size_t k = 0; k < sdecl_list.size(); ++k) {
    const TypeDeclaration* decl = tdecls[k]->typ_type;
    if (TypeExpr* var = ctype::closed_type_decl(decl)) {
      // (Typing_recovery.erroneous_type_check is for merlin)
      Error e(sdecl_list[k]->ptype_loc, EK::Unbound_type_var);
      e.ty = var;
      std::vector<tt::TypeParam> ps(tdecls[k]->typ_params.begin(), tdecls[k]->typ_params.end());
      e.params = param_types(ps);
      e.decl = decl;
      raise_error(e);
    }
  }
  // Check that constraints are enforced
  for (std::size_t k = 0; k < sdecl_list.size(); ++k) check_constraints(new_env, sdecl_list[k], decls[k].second);
  // Add type properties to declarations
  try {
    std::vector<std::pair<Ident::t, const TypeDeclaration*>> d2;
    for (std::size_t k = 0; k < decls.size(); ++k)
      d2.push_back({decls[k].first, name_recursion(sdecl_list[k], decls[k].first, decls[k].second)});
    d2 = typedecl_variance::update_decls(env, slice(sdecl_list), d2);
    d2 = typedecl_immediacy::update_decls(env, d2);
    d2 = typedecl_separability::update_decls(env, d2);
    decls = d2;
  } catch (const typedecl_variance::Error& v) {
    Error e(v.loc, EK::Variance);
    e.variance = v;
    raise_error(e);
  } catch (const typedecl_immediacy::Error& i) {
    Error e(i.loc, EK::Immediacy);
    e.sub = static_cast<int>(i.violation);
    raise_error(e);
  } catch (const typedecl_separability::Error& s) {
    Error e(s.loc, EK::Separability);
    e.name = s.evar.some ? std::string(s.evar.v) : std::string();
    raise_error(e);
  }
  // Compute the final environment with variance and immediacy
  env::t final_env = add_types_to_env(decls, env);
  // Check re-exportation
  for (std::size_t k = 0; k < sdecl_list.size(); ++k) check_abbrev(final_env, sdecl_list[k], decls[k].first, decls[k].second);
  // Keep original declaration
  std::vector<const tt::TTypeDeclaration*> final_decls;
  for (std::size_t k = 0; k < tdecls.size(); ++k) {
    auto* t2 = make<tt::TTypeDeclaration>(*tdecls[k]);
    // Using [Subst] reverts expansions
    t2->typ_type = subst::type_declaration(subst::identity(), decls[k].second);
    final_decls.push_back(t2);
  }
  return {final_decls, final_env};
}

// Check the well-formedness conditions on type abbreviations defined within
// recursive modules.
void check_recmod_typedecl(env::t abs_env, env::t env, const Location& loc, const std::vector<Ident::t>& recmod_ids,
                           Path::t path, const TypeDeclaration* decl) {
  // recmod_ids is the list of recursively-defined module idents.
  std::function<bool(Path::t)> to_check = [&](Path::t p) { return path::exists_free(recmod_ids, p); };
  check_well_founded_decl(abs_env, env, to_check, loc, path, decl);
  check_regularity(abs_env, env, loc, path, decl, to_check);
  // additional coherence check, as one might build an incoherent signature,
  // and use it to build an incoherent module, cf. #7851
  check_coherence(env, loc, path, decl);
}

}  // namespace cppcaml::typing::typedecl
