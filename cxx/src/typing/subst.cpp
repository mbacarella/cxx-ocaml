// Port of typing/subst.ml.  See subst.hpp.  Record literals evaluate their
// fields right to left in definition order (cxx/PORTING.md, "Evaluation
// order"), so the effectful copies below are made in that order explicitly.
#include "cppcaml/typing/subst.hpp"

#include <algorithm>

#include "cppcaml/typing/clflags.hpp"

namespace cppcaml::typing::subst {

using namespace types;
using namespace btype;

static const S g_identity{};
t identity() { return &g_identity; }

static S* copy(t s) { return make<S>(*s); }

t add_type(Ident::t id, Path::t p, t s) {
  S* r = copy(s);
  r->types = s->types.add(Path::pident(id), make<TypeReplacement>(true, p));
  return r;
}
t add_module(Ident::t id, Path::t p, t s) {
  S* r = copy(s);
  r->modules = s->modules.add(Path::pident(id), p);
  return r;
}
static t add_modtype_gen(Path::t p, const ModuleType* ty, t s) {
  S* r = copy(s);
  r->modtypes = s->modtypes.add(p, ty);
  return r;
}
t add_modtype_path(Path::t p, Path::t p2, t s) {
  auto* mt = make<ModuleType>(ModuleType::Kind::Mty_ident);
  mt->path = p2;
  return add_modtype_gen(p, mt, s);
}
t add_modtype(Ident::t id, Path::t p, t s) { return add_modtype_path(Path::pident(id), p, s); }
t for_saving(t s) {
  S* r = copy(s);
  r->for_saving = true;
  return r;
}
t change_locs(t s, const Location& l) {
  S* r = copy(s);
  r->loc = make<Location>(l);
  return r;
}

static const bool& keep_locs = clflags::keep_locs;
static const bool& keep_docs = clflags::keep_docs;

static Location loc(t s, const Location& x) {
  if (s->loc) return *s->loc;
  if (s->for_saving && !keep_locs) return location::none();
  return x;
}

static bool is_not_doc(const Attribute* a) {
  std::string_view n = a->attr_name;
  return !(n == "ocaml.doc" || n == "ocaml.text" || n == "doc" || n == "text");
}

// remove_loc: Ast_mapper.default_mapper with every location Location.none.
// The mapper rebuilds the payload's records and lists, and keeps what holds
// no location (a longident, an integer constant, a label) as it is.
static bool is_position(const OValue* x) {
  return x->kind == OValue::Kind::Block && x->tag == 0 && x->fields.size() == 4 &&
         x->fields[0]->kind == OValue::Kind::String;
}
static bool is_location(const OValue* x) {
  if (x->kind != OValue::Kind::Block) return false;
  if (x->loc_rec || x->loc_val) return true;
  return x->tag == 0 && x->fields.size() == 3 && is_position(x->fields[0]) && is_position(x->fields[1]) &&
         x->fields[2]->kind == OValue::Kind::Int;
}
static bool is_location_list(const OValue* x) {
  for (; x->kind == OValue::Kind::Block; x = x->fields[1])
    if (x->tag != 0 || x->fields.size() != 2 || !is_location(x->fields[0])) return false;
  return x->kind == OValue::Kind::Int && x->i == 0;
}
static bool holds_location(const OValue* x) {
  if (x->kind != OValue::Kind::Block || is_position(x)) return false;
  if (is_location(x)) return true;
  for (const OValue* f : x->fields)
    if (holds_location(f)) return true;
  return false;
}
static const OValue* remove_loc(const OValue* x) {
  if (!holds_location(x)) return x;
  if (is_location(x)) {
    static const Location none = location::none();
    auto* n = make<OValue>();
    n->kind = OValue::Kind::Block;
    n->loc_val = &none;
    return n;
  }
  std::vector<const OValue*> fs;
  for (const OValue* f : x->fields) fs.push_back(remove_loc(f));
  // an expression, pattern or core type ({_desc; _loc; _loc_stack;
  // _attributes}): Ast_helper's rebuilt record has an empty location stack
  if (x->tag == 0 && fs.size() == 4 && is_location(x->fields[1]) && is_location_list(x->fields[2])) {
    static const OValue nil{};
    fs[2] = &nil;
  }
  auto* r = make<OValue>();
  r->kind = OValue::Kind::Block;
  r->tag = x->tag;
  r->fields = slice(fs);
  return r;
}
// the mapper's attribute: { attr_name = map_loc; attr_payload; attr_loc }
static const Attribute* remove_loc(const Attribute* a) {
  auto* r = make<Attribute>(*a);
  r->attr_payload = a->attr_payload ? remove_loc(a->attr_payload) : nullptr;
  r->attr_name_loc = location::none();
  r->attr_loc = location::none();
  r->name_obj = nullptr;
  return r;
}

static Attributes attrs(t s, Attributes x) {
  if (s->for_saving && !keep_docs) {
    std::vector<const Attribute*> v;
    for (auto* a : x)
      if (is_not_doc(a)) v.push_back(a);
    x = slice(v);  // List.filter always builds a new list
  }
  if (s->for_saving && !keep_locs) {
    std::vector<const Attribute*> v;
    for (auto* a : x) v.push_back(remove_loc(a));
    x = slice(v);
  }
  return x;
}

Path::t module_path(t s, Path::t path) {
  if (auto* p = s->modules.find_opt(path)) return *p;
  switch (path->kind) {
    case Path::Kind::Pident: return path;
    case Path::Kind::Pdot: return Path::pdot(module_path(s, path->p1), path->s);
    case Path::Kind::Papply: {
      // Papply (module_path s p1, module_path s p2): right to left
      Path::t a = module_path(s, path->p2);
      Path::t f = module_path(s, path->p1);
      return Path::papply(f, a);
    }
    case Path::Kind::Pextra_ty: throw std::logic_error("Subst.module_path");
  }
  return path;
}

Path::t modtype_path(t s, Path::t path) {
  if (auto* m = s->modtypes.find_opt(path)) {
    if ((*m)->kind == ModuleType::Kind::Mty_ident) return (*m)->path;
    throw ModuleTypePathSubstitutedAway(path, *m);
  }
  switch (path->kind) {
    case Path::Kind::Pdot: return Path::pdot(module_path(s, path->p1), path->s);
    case Path::Kind::Pident: return path;
    default: throw std::logic_error("Subst.modtype_path");
  }
}

// For values, extension constructors, classes and class types
static Path::t value_path(t s, Path::t path) {
  switch (path->kind) {
    case Path::Kind::Pident: return path;
    case Path::Kind::Pdot: return Path::pdot(module_path(s, path->p1), path->s);
    default: throw std::logic_error("Subst.value_path");
  }
}

Path::t type_path(t s, Path::t path) {
  if (auto* r = s->types.find_opt(path)) {
    if ((*r)->is_path) return (*r)->path;
    throw std::logic_error("Subst.type_path: Type_function");
  }
  switch (path->kind) {
    case Path::Kind::Pident: return path;
    case Path::Kind::Pdot: return Path::pdot(module_path(s, path->p1), path->s);
    case Path::Kind::Papply: throw std::logic_error("Subst.type_path");
    case Path::Kind::Pextra_ty:
      if (path->extra == Path::Extra::Pcstr_ty)
        return Path::pextra_ty(type_path(s, path->p1), path->extra, path->s);
      return Path::pextra_ty(value_path(s, path->p1), path->extra, path->s);
  }
  return path;
}

static bool to_subst_by_type_function(t s, Path::t p) {
  auto* r = s->types.find_opt(p);
  return r && !(*r)->is_path;
}

// ---- special type ids for saved signatures ---------------------------------
static long g_new_id = -1;
void reset_for_saving() { g_new_id = -1; }

static TypeExpr* newpersty(const TypeDesc* desc) {
  --g_new_id;
  return create_expr(desc, generic_level, lowest_level, g_new_id);
}

// ensure that all occurrences of 'Tvar None' are physically shared
static const TypeDesc* tvar_none() {
  static const TypeDesc* d = [] {
    ZoneScope perm(permanent_zone());
    return tvar(OptStr::none());
  }();
  return d;
}
static const TypeDesc* tunivar_none() {
  static const TypeDesc* d = [] {
    ZoneScope perm(permanent_zone());
    return tunivar(OptStr::none());
  }();
  return d;
}
static const TypeDesc* norm(const TypeDesc* d) {
  if (auto* v = as<Tvar>(d); v && !v->name.some) return tvar_none();
  if (auto* u = as<Tunivar>(d); u && !u->name.some) return tunivar_none();
  return d;
}

static TypeExpr* apply_type_function(Slice<TypeExpr*> params, Slice<TypeExpr*> args,
                                     TypeExpr* body) {
  TypeExpr* result = nullptr;
  with_copy_scope([&](CopyScope& copy_scope) {
    for (std::size_t k = 0; k < params.size() && k < args.size(); ++k)
      redirect_desc(copy_scope, params[k], btype::scoped_tsubst(args[k], nullptr));
    std::function<TypeExpr*(TypeExpr*)> copy = [&](TypeExpr* ty) -> TypeExpr* {
      const TypeDesc* d = get_desc(ty);
      if (auto* x = as<Tsubst>(d)) return x->ty;
      if (auto* v = as<Tvariant>(d)) {
        const RowDesc* row = v->row;
        TypeExpr* t = newgenstub(get_scope(ty));
        redirect_desc(copy_scope, ty, btype::scoped_tsubst(t, nullptr));
        TypeExpr* more = row_more(row);
        const TypeDesc* mored = get_desc(more);
        const TypeDesc* desc2;
        auto* ms = as<Tsubst>(mored);
        if (ms && ms->row) {
          // This variant type has been already copied
          redirect_desc(copy_scope, ty, btype::scoped_tsubst(ms->row, nullptr));
          desc2 = tlink(ms->row);
        } else {
          TypeExpr* more2;
          if (ms) more2 = ms->ty;
          else if (mored->kind == DescKind::Tconstr || mored->kind == DescKind::Tnil)
            more2 = copy(more);
          else if (mored->kind == DescKind::Tvar || mored->kind == DescKind::Tunivar)
            more2 = newgenty(mored);
          else
            throw std::logic_error("Subst.apply_type_function");
          if (auto* c = as<Tconstr>(get_desc(more2)); c && !is_fixed(row)) {  // PR#6163
            RowDescRepr r = row_repr(row);
            row = create_row(slice(r.fields), r.more, r.closed,
                             make<FixedExplanation>(FixedExplanation::Kind::Reified,
                                                    nullptr, c->path),
                             r.name);
          }
          // Register new type first for recursion
          redirect_desc(copy_scope, more, btype::scoped_tsubst(more2, t));
          desc2 = tvariant(copy_row(copy, true, row, false, more2));
        }
        transient_expr::set_stub_desc(t, desc2);
        return t;
      }
      if (auto* f = as<Tfunctor>(d)) {
        TypeExpr* t = newgenstub(get_scope(ty));
        redirect_desc(copy_scope, ty, btype::scoped_tsubst(t, nullptr));
        std::vector<PackConstraint> cs;
        for (auto& c : f->pack->pack_constraints) cs.push_back({c.path, copy(c.ty)});
        const Package* pack2 = make<Package>(f->pack->pack_path, slice(cs));
        const TypeDesc* desc2 = tfunctor(f->label, f->id, pack2, copy(f->body));
        transient_expr::set_stub_desc(t, desc2);
        return t;
      }
      TypeExpr* t = newgenstub(get_scope(ty));
      redirect_desc(copy_scope, ty, btype::scoped_tsubst(t, nullptr));
      const TypeDesc* desc2 = copy_type_desc(copy, d);
      transient_expr::set_stub_desc(t, desc2);
      return t;
    };
    result = copy(body);
  });
  return result;
}

static const Package* package(CopyScope& cs, t s, const Package* p);

// Similar to [Ctype.nondep_type_rec].
static TypeExpr* typexp(CopyScope& copy_scope, t s, TypeExpr* ty) {
  const TypeDesc* desc = get_folded_desc(false, ty);
  switch (desc->kind) {
    case DescKind::Tvar:
    case DescKind::Tunivar:
      if (s->for_saving || get_id(ty) < 0) {
        TypeExpr* ty2 = s->for_saving ? newpersty(norm(desc)) : newty2(get_level(ty), desc);
        redirect_desc(copy_scope, ty, btype::scoped_tsubst(ty2, nullptr));
        return ty2;
      }
      return ty;
    case DescKind::Tsubst:
      return as<Tsubst>(desc)->ty;
    default:
      break;
  }
  if (auto* f = as<Tfield>(desc);
      f && !s->for_saving && f->label == dummy_method &&
      field_kind_repr(f->kind_) != FieldKindView::Fabsent && get_level(ty) < generic_level)
    return ty;  // do not copy the type of self when it is not generalized
  TypeExpr* tm = row_of_type(ty);
  bool has_fixed_row = !is_Tconstr(ty) && is_constr_row(false, tm);
  // Make a stub
  TypeExpr* ty2 = s->for_saving ? newpersty(tvar_none()) : newgenstub(get_scope(ty));
  if (get_desc(ty) == desc) redirect_desc(copy_scope, ty, btype::scoped_tsubst(ty2, nullptr));
  auto rec = [&](TypeExpr* x) { return typexp(copy_scope, s, x); };
  const TypeDesc* desc2;
  if (has_fixed_row) {
    auto* c = as<Tconstr>(get_desc(tm));  // PR#7348
    if (!c || c->path->kind != Path::Kind::Pdot) throw std::logic_error("Subst.typexp");
    std::string_view i = c->path->s;
    std::string_view i2 = i.substr(0, i.size() - 4);
    desc2 = tconstr(type_path(s, Path::pdot(c->path->p1, i2)), c->args, make<MemoRef>(mnil()));
  } else {
    switch (desc->kind) {
      case DescKind::Tconstr: {
        auto* c = as<Tconstr>(desc);
        std::vector<TypeExpr*> args;
        for (TypeExpr* a : c->args) args.push_back(rec(a));
        auto* r = s->types.find_opt(c->path);
        if (!r || (*r)->is_path)
          desc2 = tconstr(type_path(s, c->path), slice(args), make<MemoRef>(mnil()));
        else
          desc2 = tlink(apply_type_function((*r)->params, slice(args), (*r)->body));
        break;
      }
      case DescKind::Tpackage:
        desc2 = tpackage(package(copy_scope, s, as<Tpackage>(desc)->pack));
        break;
      case DescKind::Tfunctor: {
        auto* f = as<Tfunctor>(desc);
        ident::Unscoped* us2 = ident::Unscoped::refresh(f->id);
        t s2 = add_module(Ident::of_unscoped(f->id), Path::pident(Ident::of_unscoped(us2)), s);
        // Tfunctor (lbl, us', package .., typexp .. s' ty): right to left
        TypeExpr* body = typexp(copy_scope, s2, f->body);
        const Package* pk = package(copy_scope, s, f->pack);
        desc2 = tfunctor(f->label, us2, pk, body);
        break;
      }
      case DescKind::Tobject: {
        auto* o = as<Tobject>(desc);
        TypeExpr* t1 = rec(o->fields);
        const PathArgs* name2 = nullptr;
        if (const PathArgs* nm = o->name->contents) {
          if (!to_subst_by_type_function(s, nm->path)) {
            // Some (type_path s p, List.map ..): the pair right to left
            std::vector<TypeExpr*> tl;
            for (TypeExpr* x : nm->args) tl.push_back(rec(x));
            name2 = make<PathArgs>(type_path(s, nm->path), slice(tl));
          }
        }
        desc2 = tobject(t1, make<NameRef>(name2));
        break;
      }
      case DescKind::Tvariant: {
        const RowDesc* row = as<Tvariant>(desc)->row;
        TypeExpr* more = row_more(row);
        const TypeDesc* mored = get_desc(more);
        auto* ms = as<Tsubst>(mored);
        if (ms && ms->row) {
          // This variant type has been already copied
          redirect_desc(copy_scope, ty, btype::scoped_tsubst(ms->row, nullptr));
          desc2 = tlink(ms->row);
          break;
        }
        bool dup = s->for_saving || get_level(more) == generic_level || static_row(row) ||
                   is_Tconstr(more);
        TypeExpr* more2;
        if (ms) more2 = ms->ty;
        else if (mored->kind == DescKind::Tconstr || mored->kind == DescKind::Tnil)
          more2 = rec(more);
        else if (mored->kind == DescKind::Tunivar || mored->kind == DescKind::Tvar) {
          if (s->for_saving) more2 = newpersty(norm(mored));
          else if (dup && is_Tvar(more)) more2 = newgenty(mored);
          else more2 = more;
        } else {
          throw std::logic_error("Subst.typexp: row_more");
        }
        // Register new type first for recursion
        redirect_desc(copy_scope, more, btype::scoped_tsubst(more2, ty2));
        const RowDesc* row2 = copy_row(rec, true, row, !dup, more2);
        if (const PathArgs* nm = row_name(row2)) {
          const PathArgs* name =
              to_subst_by_type_function(s, nm->path)
                  ? nullptr
                  : make<PathArgs>(type_path(s, nm->path), nm->args, nm->tail);
          desc2 = tvariant(set_row_name(row2, name));
        } else {
          desc2 = tvariant(row2);
        }
        break;
      }
      case DescKind::Tfield:
        if (field_kind_repr(as<Tfield>(desc)->kind_) == FieldKindView::Fabsent) {
          desc2 = tlink(rec(as<Tfield>(desc)->rest));
          break;
        }
        desc2 = copy_type_desc(rec, desc);
        break;
      default:
        desc2 = copy_type_desc(rec, desc);
        break;
    }
  }
  transient_expr::set_stub_desc(ty2, desc2);
  return ty2;
}

static const Package* package(CopyScope& copy_scope, t s, const Package* p) {
  // record {pack_path; pack_constraints}: right to left
  std::vector<PackConstraint> cs;
  for (auto& c : p->pack_constraints) cs.push_back({c.path, typexp(copy_scope, s, c.ty)});
  Path::t pp = modtype_path(s, p->pack_path);
  return make<Package>(pp, slice(cs));
}

// Always make a copy of the type. If this is not done, type levels might not
// be correct.
TypeExpr* type_expr(t s, TypeExpr* ty) {
  TypeExpr* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = typexp(cs, s, ty); });
  return r;
}

static std::vector<TypeExpr*> map_typexp(CopyScope& cs, t s, Slice<TypeExpr*> l) {
  std::vector<TypeExpr*> v;
  for (TypeExpr* x : l) v.push_back(typexp(cs, s, x));
  return v;
}

static const LabelDeclaration* label_declaration(CopyScope& cs, t s, const LabelDeclaration* l) {
  TypeExpr* ty = typexp(cs, s, l->ld_type);
  return make<LabelDeclaration>(l->ld_id, l->ld_mutable, l->ld_atomic, ty, loc(s, l->ld_loc),
                                attrs(s, l->ld_attributes), l->ld_uid);
}

static ConstructorArguments constructor_arguments(CopyScope& cs, t s,
                                                  const ConstructorArguments& a) {
  ConstructorArguments r;
  r.kind = a.kind;
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple) {
    r.tuple = slice(map_typexp(cs, s, a.tuple));
  } else {
    std::vector<const LabelDeclaration*> v;
    for (auto* l : a.record) v.push_back(label_declaration(cs, s, l));
    r.record = slice(v);
  }
  return r;
}

static const ConstructorDeclaration* constructor_declaration(CopyScope& cs, t s,
                                                             const ConstructorDeclaration* c) {
  // fields right to left: cd_res before cd_args
  TypeExpr* res = c->cd_res ? typexp(cs, s, c->cd_res) : nullptr;
  ConstructorArguments args = constructor_arguments(cs, s, c->cd_args);
  return make<ConstructorDeclaration>(c->cd_id, args, res, loc(s, c->cd_loc),
                                      attrs(s, c->cd_attributes), c->cd_uid);
}

static const TypeDeclaration* type_declaration2(CopyScope& cs, t s, const TypeDeclaration* d) {
  // definition order right to left: manifest, kind, params
  TypeExpr* manifest = d->type_manifest ? typexp(cs, s, d->type_manifest) : nullptr;
  const TypeKind* kind = d->type_kind;
  switch (kind->kind) {
    case TypeKind::Kind::Type_variant: {
      auto* k = make<TypeKind>(*kind);
      std::vector<const ConstructorDeclaration*> v;
      for (auto* c : kind->constructors) v.push_back(constructor_declaration(cs, s, c));
      k->constructors = slice(v);
      kind = k;
      break;
    }
    case TypeKind::Kind::Type_record: {
      auto* k = make<TypeKind>(*kind);
      std::vector<const LabelDeclaration*> v;
      for (auto* l : kind->labels) v.push_back(label_declaration(cs, s, l));
      k->labels = slice(v);
      kind = k;
      break;
    }
    case TypeKind::Kind::Type_abstract:
    case TypeKind::Kind::Type_external:
      // `Type_abstract r -> Type_abstract r`: a new block (its origin shared)
      kind = make<TypeKind>(*kind);
      break;
    case TypeKind::Kind::Type_open:  // an immediate
      break;
  }
  auto params = map_typexp(cs, s, d->type_params);
  return make<TypeDeclaration>(slice(params), d->type_arity, kind, d->type_private, manifest,
                               d->type_variance, d->type_separability, false, lowest_level,
                               loc(s, d->type_loc), attrs(s, d->type_attributes),
                               d->type_immediate, d->type_unboxed_default, d->type_uid);
}

const TypeDeclaration* type_declaration(t s, const TypeDeclaration* d) {
  const TypeDeclaration* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = type_declaration2(cs, s, d); });
  return r;
}

static ClassSignature* class_signature(CopyScope& cs, t s, const ClassSignature* sign) {
  // definition order right to left: meths, vars, dummy, self_row, self
  auto meths = sign->csig_meths.map([&](const MethEntry& e) {
    return MethEntry{e.priv, e.virt, typexp(cs, s, e.ty)};
  });
  auto vars = sign->csig_vars.map([&](const VarEntry& e) {
    return VarEntry{e.mut, e.virt, typexp(cs, s, e.ty)};
  });
  FieldKind* dummy = field_kind_internal_repr(sign->csig_dummy_method);
  TypeExpr* self_row = typexp(cs, s, sign->csig_self_row);
  TypeExpr* self = typexp(cs, s, sign->csig_self);
  return make<ClassSignature>(self, self_row, dummy, vars, meths);
}

static const ClassType* class_type2(CopyScope& cs, t s, const ClassType* c) {
  auto* r = make<ClassType>(c->kind);
  switch (c->kind) {
    case ClassType::Kind::Cty_constr: {
      r->path = type_path(s, c->path);
      r->args = slice(map_typexp(cs, s, c->args));
      r->cty = class_type2(cs, s, c->cty);
      break;
    }
    case ClassType::Kind::Cty_signature:
      r->sign = class_signature(cs, s, c->sign);
      break;
    case ClassType::Kind::Cty_arrow: {
      // Cty_arrow (l, typexp ty, class_type cty): right to left
      r->cty = class_type2(cs, s, c->cty);
      r->label = c->label;
      r->arg = typexp(cs, s, c->arg);
      break;
    }
  }
  return r;
}

static const ClassDeclaration* class_declaration2(CopyScope& cs, t s, const ClassDeclaration* d) {
  // definition order: params, type, path, new, variance, loc, attrs, uid;
  // evaluated right to left
  TypeExpr* nw = d->cty_new ? typexp(cs, s, d->cty_new) : nullptr;
  Path::t p = type_path(s, d->cty_path);
  const ClassType* ct = class_type2(cs, s, d->cty_type);
  auto params = map_typexp(cs, s, d->cty_params);
  return make<ClassDeclaration>(slice(params), ct, p, nw, d->cty_variance, loc(s, d->cty_loc),
                                attrs(s, d->cty_attributes), d->cty_uid);
}

const ClassDeclaration* class_declaration(t s, const ClassDeclaration* d) {
  const ClassDeclaration* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = class_declaration2(cs, s, d); });
  return r;
}

static const ClassTypeDeclaration* cltype_declaration2(CopyScope& cs, t s,
                                                       const ClassTypeDeclaration* d) {
  // definition order: params, type, path, hash_type, variance, ..; right to left
  const TypeDeclaration* hash = type_declaration2(cs, s, d->clty_hash_type);
  Path::t p = type_path(s, d->clty_path);
  const ClassType* ct = class_type2(cs, s, d->clty_type);
  auto params = map_typexp(cs, s, d->clty_params);
  return make<ClassTypeDeclaration>(slice(params), ct, p, hash, d->clty_variance,
                                    loc(s, d->clty_loc), attrs(s, d->clty_attributes),
                                    d->clty_uid);
}

const ClassTypeDeclaration* cltype_declaration(t s, const ClassTypeDeclaration* d) {
  const ClassTypeDeclaration* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = cltype_declaration2(cs, s, d); });
  return r;
}

const ClassType* class_type(t s, const ClassType* c) {
  const ClassType* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = class_type2(cs, s, c); });
  return r;
}

static const ValueDescription* value_description2(CopyScope& cs, t s, const ValueDescription* d) {
  TypeExpr* ty = typexp(cs, s, d->val_type);
  return make<ValueDescription>(ty, d->val_kind, loc(s, d->val_loc),
                                attrs(s, d->val_attributes), d->val_uid);
}

const ValueDescription* value_description(t s, const ValueDescription* d) {
  const ValueDescription* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = value_description2(cs, s, d); });
  return r;
}

static const ExtensionConstructor* extension_constructor2(CopyScope& cs, t s,
                                                          const ExtensionConstructor* e) {
  // definition order: type_path, params, args, ret_type, private, loc, attrs,
  // uid; right to left
  TypeExpr* ret = e->ext_ret_type ? typexp(cs, s, e->ext_ret_type) : nullptr;
  ConstructorArguments args = constructor_arguments(cs, s, e->ext_args);
  auto params = map_typexp(cs, s, e->ext_type_params);
  Path::t p = type_path(s, e->ext_type_path);
  return make<ExtensionConstructor>(p, slice(params), args, ret, e->ext_private,
                                    s->for_saving ? location::none() : e->ext_loc,
                                    attrs(s, e->ext_attributes), e->ext_uid);
}

const ExtensionConstructor* extension_constructor(t s, const ExtensionConstructor* e) {
  const ExtensionConstructor* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = extension_constructor2(cs, s, e); });
  return r;
}

// For every binding k |-> d of m1, add k |-> f d to m2.
template <class V, class F>
static PathMap<V> merge_path_maps(F&& f, PathMap<V> m1, PathMap<V> m2) {
  m1.iter([&](Path::t k, const V& d) { m2 = m2.add(k, f(d)); });
  return m2;
}

static const TypeReplacement* type_replacement(t s, const TypeReplacement* r) {
  if (r->is_path) return make<TypeReplacement>(true, type_path(s, r->path));
  const TypeReplacement* out = nullptr;
  with_copy_scope([&](CopyScope& cs) {
    auto params = map_typexp(cs, s, r->params);
    TypeExpr* body = typexp(cs, s, r->body);
    out = make<TypeReplacement>(false, nullptr, slice(params), body);
  });
  return out;
}

// ============================================================================
// Lazy

namespace lazy {

using LSig = lazy::Signature;

static std::pair<std::vector<const SignatureItem*>, subst::t> rename_bound_idents(
    Scoping scoping, subst::t s, Slice<const SignatureItem*> sg) {
  auto rename = [&](Ident::t id) -> Ident::t {
    switch (scoping.kind) {
      case Scoping::Kind::Keep: return Ident::create_scoped(ident::scope(id), ident::name(id));
      case Scoping::Kind::Make_local: return ident::rename(id);
      case Scoping::Kind::Rescope: return Ident::create_scoped(scoping.scope, ident::name(id));
    }
    return id;
  };
  using K = typing::SignatureItem::Kind;
  std::vector<const SignatureItem*> out;  // accumulated reversed (consed), as sg
  for (auto* it : sg) {
    auto* n = make<SignatureItem>(*it);
    switch (it->kind) {
      case K::Sig_type:
        n->id = rename(it->id);
        s = add_type(it->id, Path::pident(n->id), s);
        break;
      case K::Sig_module:
        n->id = rename(it->id);
        s = add_module(it->id, Path::pident(n->id), s);
        break;
      case K::Sig_modtype:
        n->id = rename(it->id);
        s = add_modtype(it->id, Path::pident(n->id), s);
        break;
      case K::Sig_class:
      case K::Sig_class_type:
        // cheat and pretend they are types cf. PR#6650
        n->id = rename(it->id);
        s = add_type(it->id, Path::pident(n->id), s);
        break;
      case K::Sig_value:
        // scope doesn't matter for value identifiers.
        n->id = ident::rename(it->id);
        break;
      case K::Sig_typext:
        n->id = rename(it->id);
        break;
    }
    out.insert(out.begin(), n);
  }
  return {std::move(out), s};
}

const Modtype* of_modtype(const ModuleType* m);

const ModuleDecl* of_module_decl(const ModuleDeclaration* md) {
  return make<ModuleDecl>(of_modtype(md->md_type), md->md_attributes, md->md_loc, md->md_uid);
}

static LSig create_forced(SigPrime p) { return LazyBacktrack<SigThunk, SigPrime>::create_forced(p); }

const Modtype* of_modtype(const ModuleType* m) {
  using MK = ModuleType::Kind;
  using LK = Modtype::Kind;
  switch (m->kind) {
    case MK::Mty_ident: {
      auto* r = make<Modtype>(LK::MtyL_ident);
      r->path = m->path;
      return r;
    }
    case MK::Mty_signature: {
      auto* r = make<Modtype>(LK::MtyL_signature);
      SigPrime p;
      p.eager = true;
      p.eager_sg = m->sign;
      r->sign = create_forced(p);
      return r;
    }
    case MK::Mty_functor: {
      auto* r = make<Modtype>(LK::MtyL_functor);
      if (m->param.is_unit) {
        r->res = of_modtype(m->res);
      } else {
        // MtyL_functor (Named (id, lazy_modtype arg), lazy_modtype res): right to left
        r->res = of_modtype(m->res);
        r->param.is_unit = false;
        r->param.id = m->param.id;
        r->param.some_obj = m->param.some_obj;
        r->param.mty = of_modtype(m->param.mty);
      }
      return r;
    }
    case MK::Mty_alias: {
      auto* r = make<Modtype>(LK::MtyL_alias);
      r->path = m->path;
      return r;
    }
  }
  return nullptr;
}

const ModtypeDecl* of_modtype_decl(const ModtypeDeclaration* d) {
  const Modtype* mt = d->mtd_type ? of_modtype(d->mtd_type) : nullptr;
  return make<ModtypeDecl>(mt, d->mtd_attributes, d->mtd_loc, d->mtd_uid);
}

LSig of_signature(typing::Signature sg) {
  SigPrime p;
  p.eager = true;
  p.eager_sg = sg;
  return create_forced(p);
}

LSig of_signature_items(Slice<const SignatureItem*> sg) {
  SigPrime p;
  p.eager = false;
  p.lazy_sg = sg;
  return create_forced(p);
}

const SignatureItem* of_signature_item(const typing::SignatureItem* it) {
  auto* n = make<SignatureItem>(it->kind, it->id, it->vis, it->rec, it->value, it->type,
                                it->ext, it->ext_status, it->presence);
  n->cls = it->cls;
  n->clty = it->clty;
  if (it->kind == SignatureItem::K::Sig_module) n->md = of_module_decl(it->md);
  if (it->kind == SignatureItem::K::Sig_modtype) n->mtd = of_modtype_decl(it->mtd);
  return n;
}

const ModuleDecl* module_decl(Scoping sc, subst::t s, const ModuleDecl* md) {
  const Modtype* mt = modtype(sc, s, md->mdl_type);
  return make<ModuleDecl>(mt, attrs(s, md->mdl_attributes), loc(s, md->mdl_loc), md->mdl_uid);
}

LSig signature(Scoping scoping, subst::t s, LSig sg);

const Modtype* modtype(Scoping sc, subst::t s, const Modtype* m) {
  using LK = Modtype::Kind;
  switch (m->kind) {
    case LK::MtyL_ident: {
      if (auto* mt = s->modtypes.find_opt(m->path)) return of_modtype(*mt);
      auto* r = make<Modtype>(LK::MtyL_ident);
      switch (m->path->kind) {
        case Path::Kind::Pident: r->path = m->path; break;
        case Path::Kind::Pdot:
          r->path = Path::pdot(module_path(s, m->path->p1), m->path->s);
          break;
        default: throw std::logic_error("Subst.modtype");
      }
      return r;
    }
    case LK::MtyL_signature: {
      auto* r = make<Modtype>(LK::MtyL_signature);
      r->sign = signature(sc, s, m->sign);
      return r;
    }
    case LK::MtyL_functor: {
      auto* r = make<Modtype>(LK::MtyL_functor);
      if (m->param.is_unit) {
        r->res = modtype(sc, s, m->res);
      } else if (!m->param.id) {
        // MtyL_functor (Named (None, f arg), f res): right to left
        r->res = modtype(sc, s, m->res);
        r->param.is_unit = false;
        r->param.mty = modtype(sc, s, m->param.mty);
      } else {
        Ident::t id2 = ident::rename(m->param.id);
        r->res = modtype(sc, add_module(m->param.id, Path::pident(id2), s), m->res);
        r->param.is_unit = false;
        r->param.id = id2;
        r->param.some_obj = fresh_identity();
        r->param.mty = modtype(sc, s, m->param.mty);
      }
      return r;
    }
    case LK::MtyL_alias: {
      auto* r = make<Modtype>(LK::MtyL_alias);
      r->path = module_path(s, m->path);
      return r;
    }
  }
  return m;
}

const ModtypeDecl* modtype_decl(Scoping sc, subst::t s, const ModtypeDecl* d) {
  // record right to left: loc/attrs are pure, then the type
  const Modtype* mt = d->mtdl_type ? modtype(sc, s, d->mtdl_type) : nullptr;
  return make<ModtypeDecl>(mt, attrs(s, d->mtdl_attributes), loc(s, d->mtdl_loc), d->mtdl_uid);
}

const ModuleType* force_modtype(const Modtype* m);

const ModuleDeclaration* force_module_decl(const ModuleDecl* md) {
  const ModuleType* mt = force_modtype(md->mdl_type);
  return make<ModuleDeclaration>(mt, md->mdl_attributes, md->mdl_loc, md->mdl_uid);
}

const ModuleType* force_modtype(const Modtype* m) {
  using LK = Modtype::Kind;
  using MK = ModuleType::Kind;
  switch (m->kind) {
    case LK::MtyL_ident: {
      auto* r = make<ModuleType>(MK::Mty_ident);
      r->path = m->path;
      return r;
    }
    case LK::MtyL_signature: {
      auto* r = make<ModuleType>(MK::Mty_signature);
      r->sign = force_signature(m->sign);
      return r;
    }
    case LK::MtyL_functor: {
      auto* r = make<ModuleType>(MK::Mty_functor);
      // `let param = .. in Mty_functor (param, force_modtype res)`
      if (!m->param.is_unit) {
        r->param.is_unit = false;
        r->param.id = m->param.id;
        r->param.some_obj = m->param.some_obj;
        r->param.mty = force_modtype(m->param.mty);
      }
      r->res = force_modtype(m->res);
      return r;
    }
    case LK::MtyL_alias: {
      auto* r = make<ModuleType>(MK::Mty_alias);
      r->path = m->path;
      return r;
    }
  }
  return nullptr;
}

const ModtypeDeclaration* force_modtype_decl(const ModtypeDecl* d) {
  const ModuleType* mt = d->mtdl_type ? force_modtype(d->mtdl_type) : nullptr;
  return make<ModtypeDeclaration>(mt, d->mtdl_attributes, d->mtdl_loc, d->mtdl_uid);
}

LSig signature(Scoping scoping, subst::t s, LSig sg) {
  if (sg->is_thunk()) {
    const SigThunk& th = sg->thunk;
    Scoping sc = scoping.kind == Scoping::Kind::Keep ? th.scoping : scoping;
    subst::t s2 = compose(th.s, s);
    return LazyBacktrack<SigThunk, SigPrime>::create(SigThunk{sc, s2, th.sg});
  }
  return LazyBacktrack<SigThunk, SigPrime>::create(SigThunk{scoping, s, sg->done});
}

static Slice<const SignatureItem*> lazy_signature2(const SigPrime& p) {
  if (!p.eager) return p.lazy_sg;
  std::vector<const SignatureItem*> v;
  for (auto* it : p.eager_sg) v.push_back(of_signature_item(it));
  return slice(v);
}

static const SignatureItem* subst_lazy_signature_item2(CopyScope& cs, Scoping scoping,
                                                       subst::t s, const SignatureItem* comp) {
  using K = typing::SignatureItem::Kind;
  auto* n = make<SignatureItem>(*comp);
  switch (comp->kind) {
    case K::Sig_value: n->value = value_description2(cs, s, comp->value); break;
    case K::Sig_type: n->type = type_declaration2(cs, s, comp->type); break;
    case K::Sig_typext: n->ext = extension_constructor2(cs, s, comp->ext); break;
    case K::Sig_module: n->md = module_decl(scoping, s, comp->md); break;
    case K::Sig_modtype: n->mtd = modtype_decl(scoping, s, comp->mtd); break;
    case K::Sig_class: n->cls = class_declaration2(cs, s, comp->cls); break;
    case K::Sig_class_type: n->clty = cltype_declaration2(cs, s, comp->clty); break;
  }
  return n;
}

static SigPrime force_signature_once2(const SigThunk& th) {
  Slice<const SignatureItem*> sg = lazy_signature2(th.sg);
  // Components of signature may be mutually recursive (e.g. type declarations
  // or class and type declarations), so first build global renaming
  // substitution...
  auto [sg2, s2] = rename_bound_idents(th.scoping, th.s, sg);
  // ... then apply it to each signature component in turn
  SigPrime out;
  out.eager = false;
  with_copy_scope([&, &sg2 = sg2, &s2 = s2](CopyScope& cs) {
    // List.rev_map: sg2 is reversed, rev_map walks it in order and conses,
    // restoring the source order -- the components are processed in
    // REVERSED source order.
    std::vector<const SignatureItem*> v(sg2.size());
    for (std::size_t k = 0; k < sg2.size(); ++k)
      v[sg2.size() - 1 - k] = subst_lazy_signature_item2(cs, th.scoping, s2, sg2[k]);
    out.lazy_sg = slice(v);
  });
  return out;
}

Slice<const SignatureItem*> force_signature_once(LSig sg) {
  return lazy_signature2(sg->force(force_signature_once2));
}

typing::Signature force_signature(LSig sg) {
  std::vector<const typing::SignatureItem*> v;
  for (auto* it : force_signature_once(sg)) v.push_back(force_signature_item(it));
  return slice(v);
}

const typing::SignatureItem* force_signature_item(const SignatureItem* it) {
  auto* n = make<typing::SignatureItem>(it->kind, it->id, it->vis, it->rec, it->value, it->type,
                                        it->ext, it->ext_status, it->presence);
  n->cls = it->cls;
  n->clty = it->clty;
  if (it->kind == SignatureItem::K::Sig_module) n->md = force_module_decl(it->md);
  if (it->kind == SignatureItem::K::Sig_modtype) n->mtd = force_modtype_decl(it->mtd);
  return n;
}

const SignatureItem* signature_item(Scoping sc, subst::t s, const SignatureItem* it) {
  const SignatureItem* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = subst_lazy_signature_item2(cs, sc, s, it); });
  return r;
}

}  // namespace lazy

const ModuleType* modtype(Scoping sc, t s, const ModuleType* m) {
  return lazy::force_modtype(lazy::modtype(sc, s, lazy::of_modtype(m)));
}

t compose(t s1, t s2) {
  if (s1 == identity()) return s2;
  if (s2 == identity()) return s1;
  // record fields right to left: loc, for_saving, modtypes, modules, types
  const Location* l = s2->loc ? s2->loc : s1->loc;
  bool fs = s1->for_saving || s2->for_saving;
  auto modtypes = merge_path_maps(
      [&](const ModuleType* m) { return modtype(Scoping::keep(), s2, m); }, s1->modtypes,
      s2->modtypes);
  auto modules =
      merge_path_maps([&](Path::t p) { return module_path(s2, p); }, s1->modules, s2->modules);
  auto types = merge_path_maps(
      [&](const TypeReplacement* r) { return type_replacement(s2, r); }, s1->types, s2->types);
  return make<S>(types, modules, modtypes, fs, l);
}

Signature signature(Scoping sc, t s, Signature sg) {
  return lazy::force_signature(lazy::signature(sc, s, lazy::of_signature(sg)));
}

const SignatureItem* signature_item(Scoping sc, t s, const SignatureItem* it) {
  return lazy::force_signature_item(lazy::signature_item(sc, s, lazy::of_signature_item(it)));
}

const ModtypeDeclaration* modtype_declaration(Scoping sc, t s, const ModtypeDeclaration* d) {
  return lazy::force_modtype_decl(lazy::modtype_decl(sc, s, lazy::of_modtype_decl(d)));
}

const ModuleDeclaration* module_declaration(Scoping sc, t s, const ModuleDeclaration* d) {
  return lazy::force_module_decl(lazy::module_decl(sc, s, lazy::of_module_decl(d)));
}

namespace unsafe {
t add_modtype_path(Path::t p, const ModuleType* mty, t s) { return add_modtype_gen(p, mty, s); }
t add_modtype(Ident::t id, const ModuleType* mty, t s) {
  return add_modtype_gen(Path::pident(id), mty, s);
}
t add_type_path(Path::t id, Path::t p, t s) {
  S* r = copy(s);
  r->types = s->types.add(id, make<TypeReplacement>(true, p));
  return r;
}
t add_type_function(Path::t id, Slice<TypeExpr*> params, TypeExpr* body, t s) {
  S* r = copy(s);
  r->types = s->types.add(id, make<TypeReplacement>(false, nullptr, params, body));
  return r;
}
t add_module_path(Path::t id, Path::t p, t s) {
  S* r = copy(s);
  r->modules = s->modules.add(id, p);
  return r;
}
}  // namespace unsafe

}  // namespace cppcaml::typing::subst
