// Port of typing/btype.ml.  See btype.hpp.
#include "cppcaml/typing/btype.hpp"

#include <algorithm>
#include <climits>
#include <stdexcept>

namespace cppcaml::typing::btype {

using namespace types;

// ---- TypePairs ------------------------------------------------------------
void TypePairs::add(TypeExpr* a, TypeExpr* b) {
  auto p = std::make_pair(repr(a), repr(b));
  if (set_.count(p)) return;
  set_.insert(p);
  elems_.push_back(p);
}
bool TypePairs::mem(TypeExpr* a, TypeExpr* b) const {
  return set_.count(std::make_pair(repr(a), repr(b))) != 0;
}
void TypePairs::iter(FnRef<void(TypeExpr*, TypeExpr*)> f) const {
  for (auto& [a, b] : elems_) f(a, b);
}

// ---- leveled type pool --------------------------------------------------------
struct Pool {
  long level;
  std::vector<TypeExpr*> pool;  // most recent last (OCaml list reversed)
  Pool* next;
};
static Pool g_dummy{LONG_MAX, {}, nullptr};
static Pool g_base{0, {}, &g_dummy};
static Pool* g_pool_stack = &g_base;

static Pool* pool_of_level(long level, Pool* pool) {
  while (level < pool->level) pool = pool->next;
  return pool;
}

std::vector<TypeExpr*> with_new_pool(long level, FnRef<void()> f) {
  Pool pool{level, {}, g_pool_stack};
  Pool* saved = g_pool_stack;
  g_pool_stack = &pool;
  struct Restore {
    Pool*& stack;
    Pool* saved;
    ~Restore() { stack = saved; }
  } restore{g_pool_stack, saved};
  f();
  // OCaml returns pool.pool, whose head is the most recently added node.
  std::vector<TypeExpr*> r = std::move(pool.pool);
  std::reverse(r.begin(), r.end());
  return r;
}

void add_to_pool(long level, TypeExpr* ty) {
  if (level >= generic_level || level <= lowest_level) return;
  pool_of_level(level, g_pool_stack)->pool.push_back(ty);
}

// ---- type creators --------------------------------------------------------
TypeExpr* newty3(long level, long scope, const TypeDesc* desc) {
  TypeExpr* ty = proto_newty3(level, scope, desc);
  add_to_pool(level, ty);
  return ty;
}
TypeExpr* newty2(long level, const TypeDesc* desc) {
  return newty3(level, ident::lowest_scope, desc);
}
TypeExpr* newgenty(const TypeDesc* desc) { return newty2(generic_level, desc); }
TypeExpr* newgenmono(TypeExpr* ty) { return newgenty(tpoly(ty, {})); }
TypeExpr* newgenvar(OptStr name) { return newgenty(tvar(name)); }
TypeExpr* newgenstub(long scope) { return newty3(generic_level, scope, TVAR_NONE_LIT()); }

// ---- checks -----------------------------------------------------------------
bool is_Tvar(TypeExpr* ty) { return get_desc(ty)->kind == DescKind::Tvar; }
bool is_Tunivar(TypeExpr* ty) { return get_desc(ty)->kind == DescKind::Tunivar; }
bool is_Tconstr(TypeExpr* ty) { return get_desc(ty)->kind == DescKind::Tconstr; }
bool is_Tpoly(TypeExpr* ty) { return get_desc(ty)->kind == DescKind::Tpoly; }
bool is_poly_Tpoly(TypeExpr* ty) {
  auto* p = as<Tpoly>(get_desc(ty));
  return p && !p->vars.empty();
}
bool type_kind_is_abstract(const TypeDeclaration* decl) {
  return decl->type_kind->kind == TypeKind::Kind::Type_abstract;
}
TypeOrigin type_origin(const TypeDeclaration* decl) {
  if (decl->type_kind->kind == TypeKind::Kind::Type_abstract) return decl->type_kind->origin;
  return TypeOrigin{};
}

const TypeDesc* get_constr_desc(TypeExpr* ty) {
  if (const PathArgs* a = get_abbrev(ty))
    return tconstr(a->path, a->args, make<MemoRef>(mnil()));
  return get_desc(ty);
}

// ---- poly types -------------------------------------------------------------
bool tpoly_is_mono(TypeExpr* ty) {
  auto* p = as<Tpoly>(get_desc(ty));
  if (!p) throw std::logic_error("Btype.tpoly_is_mono");
  return p->vars.empty();
}
std::pair<TypeExpr*, Slice<TypeExpr*>> tpoly_get_poly(TypeExpr* ty) {
  auto* p = as<Tpoly>(get_desc(ty));
  if (!p) throw std::logic_error("Btype.tpoly_get_poly");
  return {p->body, p->vars};
}
TypeExpr* tpoly_get_mono(TypeExpr* ty) {
  auto* p = as<Tpoly>(get_desc(ty));
  if (!p || !p->vars.empty()) throw std::logic_error("Btype.tpoly_get_mono");
  return p->body;
}
TypeExpr* tpoly_get_mono_opt(TypeExpr* ty) {
  auto* p = as<Tpoly>(get_desc(ty));
  if (!p) throw std::logic_error("Btype.tpoly_get_mono_opt");
  return p->vars.empty() ? p->body : nullptr;
}

// ---- rows ---------------------------------------------------------------------
const FixedExplanation* merge_fixed_explanation(const FixedExplanation* f1,
                                                const FixedExplanation* f2) {
  using FK = FixedExplanation::Kind;
  for (FK k : {FK::Univar, FK::Fixed_private, FK::Reified, FK::Rigid}) {
    if (f1 && f1->kind == k) return f1;
    if (f2 && f2->kind == k) return f2;
  }
  return nullptr;
}

const FixedExplanation* fixed_explanation(const RowDesc* row) {
  if (const FixedExplanation* x = row_fixed(row)) return x;
  TypeExpr* ty = row_more(row);
  const TypeDesc* d = get_desc(ty);
  switch (d->kind) {
    case DescKind::Tvar:
    case DescKind::Tnil:
      return nullptr;
    case DescKind::Tunivar:
      return make<FixedExplanation>(FixedExplanation::Kind::Univar, ty);
    case DescKind::Tconstr:
      return make<FixedExplanation>(FixedExplanation::Kind::Reified, nullptr,
                                    as<Tconstr>(d)->path);
    default:
      throw std::logic_error("Btype.fixed_explanation");
  }
}

bool is_fixed(const RowDesc* row) { return row_fixed(row) != nullptr; }
bool has_fixed_explanation(const RowDesc* row) { return fixed_explanation(row) != nullptr; }

bool static_row(const RowDesc* row) {
  if (!row_closed(row)) return false;
  for (auto& e : row_fields(row))
    if (row_field_repr(e.field).kind == RowFieldView::Kind::Reither) return false;
  return true;
}

long hash_variant(std::string_view s) {
  long accu = 0;
  for (unsigned char c : s) accu = 223 * accu + c;
  // reduce to 31 bits (OCaml ints wrap at 63 bits; the mask makes that moot
  // for the low 31 bits, which is all that is kept)
  accu &= (1L << 31) - 1;
  return accu > 0x3FFFFFFF ? accu - (1L << 31) : accu;
}

TypeExpr* proxy(TypeExpr* ty) {
  const TypeDesc* d = get_desc(ty);
  if (auto* v = as<Tvariant>(d); v && !static_row(v->row)) return row_more(v->row);
  if (auto* o = as<Tobject>(d)) {
    TypeExpr* t = o->fields;
    for (;;) {
      const TypeDesc* dd = get_desc(t);
      switch (dd->kind) {
        case DescKind::Tfield: t = as<Tfield>(dd)->rest; continue;
        case DescKind::Tvar:
        case DescKind::Tunivar:
        case DescKind::Tconstr:
        case DescKind::Tnil:
          return t;
        default:
          throw std::logic_error("Btype.proxy");
      }
    }
  }
  return ty;
}

TypeExpr* row_of_type(TypeExpr* t) {
  const TypeDesc* d = get_desc(t);
  if (auto* o = as<Tobject>(d)) {
    TypeExpr* x = o->fields;
    for (;;) {
      auto* f = as<Tfield>(get_desc(x));
      if (!f) return x;
      x = f->rest;
    }
  }
  if (auto* v = as<Tvariant>(d)) return row_more(v->row);
  return t;
}

bool has_constr_row(TypeExpr* t) { return !is_Tconstr(t) && is_Tconstr(row_of_type(t)); }

bool is_row_name(std::string_view s) {
  return s.size() > 4 && s.substr(s.size() - 4) == "#row";
}

bool is_constr_row(bool allow_ident, TypeExpr* t) {
  auto* c = as<Tconstr>(get_desc(t));
  if (!c) return false;
  if (c->path->kind == Path::Kind::Pident && allow_ident)
    return is_row_name(ident::name(c->path->id));
  if (c->path->kind == Path::Kind::Pdot) return is_row_name(c->path->s);
  return false;
}

void set_static_row_name(const TypeDeclaration* decl, Path::t path) {
  if (decl->type_private != PrivateFlag::Public || !decl->type_manifest) return;
  TypeExpr* ty = decl->type_manifest;
  auto* v = as<Tvariant>(get_desc(ty));
  if (v && static_row(v->row)) {
    const RowDesc* row = set_row_name(v->row, make<PathArgs>(path, decl->type_params));
    set_type_desc(ty, tvariant(row));
  }
}

// ---- traversal --------------------------------------------------------------
void iter_row(FnRef<void(TypeExpr*)> f, const RowDesc* row) {
  for (auto& e : row_fields(row)) {
    auto v = row_field_repr(e.field);
    if (v.kind == RowFieldView::Kind::Rpresent && v.present) f(v.present);
    else if (v.kind == RowFieldView::Kind::Reither)
      for (TypeExpr* t : v.arg_types) f(t);
  }
  switch (get_desc(row_more(row))->kind) {
    case DescKind::Tvar:
    case DescKind::Tunivar:
    case DescKind::Tsubst:
    case DescKind::Tconstr:
    case DescKind::Tnil:
      if (const PathArgs* nm = row_name(row))
        for (TypeExpr* t : nm->args) f(t);
      return;
    default:
      throw std::logic_error("Btype.fold_row");
  }
}

void iter_type_desc(FnRef<void(TypeExpr*)> f, const TypeDesc* d) {
  switch (d->kind) {
    case DescKind::Tvar: return;
    case DescKind::Tarrow: { auto* a = as<Tarrow>(d); f(a->t1); f(a->t2); return; }
    case DescKind::Ttuple: for (auto& e : as<Ttuple>(d)->elems) f(e.ty); return;
    case DescKind::Tconstr: for (TypeExpr* t : as<Tconstr>(d)->args) f(t); return;
    case DescKind::Tobject: {
      auto* o = as<Tobject>(d);
      f(o->fields);
      if (o->name->contents)
        for (TypeExpr* t : o->name->contents->args) f(t);
      return;
    }
    case DescKind::Tvariant: {
      auto* v = as<Tvariant>(d);
      iter_row(f, v->row);
      f(row_more(v->row));
      return;
    }
    case DescKind::Tfield: { auto* x = as<Tfield>(d); f(x->ty); f(x->rest); return; }
    case DescKind::Tnil:
    case DescKind::Tunivar:
      return;
    case DescKind::Tpoly: {
      auto* p = as<Tpoly>(d);
      f(p->body);
      for (TypeExpr* t : p->vars) f(t);
      return;
    }
    case DescKind::Tpackage:
      for (auto& c : as<Tpackage>(d)->pack->pack_constraints) f(c.ty);
      return;
    case DescKind::Tfunctor: {
      auto* x = as<Tfunctor>(d);
      for (auto& c : x->pack->pack_constraints) f(c.ty);
      f(x->body);
      return;
    }
    case DescKind::Tlink:
    case DescKind::Texpand:
      throw std::logic_error("Btype.fold_type_desc");
    case DescKind::Tsubst:
      return;
  }
}

void iter_type_expr(FnRef<void(TypeExpr*)> f, TypeExpr* ty) {
  iter_type_desc(f, get_desc(ty));
}

void iter_abbrev_memo(FnRef<void(TypeExpr*)> f, const AbbrevMemo* m) {
  for (;;) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return;
      case AbbrevMemo::Kind::Mcons:
        f(m->abbreviation);
        f(m->expansion);
        m = m->rem;
        continue;
      case AbbrevMemo::Kind::Mlink:
        m = m->link->contents;
        continue;
    }
  }
}

void iter_type_expr_cstr_args(FnRef<void(TypeExpr*)> f,
                              const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple) {
    for (TypeExpr* t : a.tuple) f(t);
  } else {
    for (auto* d : a.record) f(d->ld_type);
  }
}

ConstructorArguments map_type_expr_cstr_args(FnRef<TypeExpr*(TypeExpr*)> f,
                                             const ConstructorArguments& a) {
  ConstructorArguments r;
  r.kind = a.kind;
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple) {
    std::vector<TypeExpr*> v;
    for (TypeExpr* t : a.tuple) v.push_back(f(t));
    r.tuple = slice(v);
  } else {
    std::vector<const LabelDeclaration*> v;
    for (auto* d : a.record) {
      auto* nd = make<LabelDeclaration>(*d);
      nd->ld_type = f(d->ld_type);
      v.push_back(nd);
    }
    r.record = slice(v);
  }
  return r;
}

void iter_type_expr_kind(FnRef<void(TypeExpr*)> f, const TypeKind* k) {
  switch (k->kind) {
    case TypeKind::Kind::Type_variant:
      for (auto* cd : k->constructors) {
        iter_type_expr_cstr_args(f, cd->cd_args);
        if (cd->cd_res) f(cd->cd_res);
      }
      return;
    case TypeKind::Kind::Type_record:
      for (auto* d : k->labels) f(d->ld_type);
      return;
    default:
      return;
  }
}

const Package* map_pack(FnRef<Path::t(Path::t)> map_path,
                        FnRef<TypeExpr*(TypeExpr*)> map_type,
                        const Package* p) {
  Path::t np = map_path(p->pack_path);
  std::vector<PackConstraint> cs;
  for (auto& c : p->pack_constraints) cs.push_back({c.path, map_type(c.ty)});
  return make<Package>(np, slice(cs));
}

// ---- marking ------------------------------------------------------------------
void mark_type(TypeMark& mark, TypeExpr* ty) {
  if (try_mark_node(mark, ty))
    iter_type_expr([&](TypeExpr* t) { mark_type(mark, t); }, ty);
}
void mark_type_params(TypeMark& mark, TypeExpr* ty) {
  iter_type_expr([&](TypeExpr* t) { mark_type(mark, t); }, ty);
}

// ---- iterators ------------------------------------------------------------------
TypeIterators type_iterators_without_type_expr() {
  TypeIterators it;
  it.it_signature = [](TypeIterators& it, Signature sg) {
    for (auto* x : sg) it.it_signature_item(it, x);
  };
  it.it_signature_item = [](TypeIterators& it, const SignatureItem* x) {
    using SK = SignatureItem::Kind;
    switch (x->kind) {
      case SK::Sig_value: it.it_value_description(it, x->value); break;
      case SK::Sig_type: it.it_type_declaration(it, x->type); break;
      case SK::Sig_typext: it.it_extension_constructor(it, x->ext); break;
      case SK::Sig_module: it.it_module_declaration(it, x->md); break;
      case SK::Sig_modtype: it.it_modtype_declaration(it, x->mtd); break;
      case SK::Sig_class: it.it_class_declaration(it, x->cls); break;
      case SK::Sig_class_type: it.it_class_type_declaration(it, x->clty); break;
    }
  };
  it.it_value_description = [](TypeIterators& it, const ValueDescription* vd) {
    it.it_type_expr(it, vd->val_type);
  };
  it.it_type_declaration = [](TypeIterators& it, const TypeDeclaration* td) {
    for (TypeExpr* t : td->type_params) it.it_type_expr(it, t);
    if (td->type_manifest) it.it_type_expr(it, td->type_manifest);
    it.it_type_kind(it, td->type_kind);
  };
  it.it_extension_constructor = [](TypeIterators& it, const ExtensionConstructor* td) {
    it.it_path(td->ext_type_path);
    for (TypeExpr* t : td->ext_type_params) it.it_type_expr(it, t);
    iter_type_expr_cstr_args([&](TypeExpr* t) { it.it_type_expr(it, t); }, td->ext_args);
    if (td->ext_ret_type) it.it_type_expr(it, td->ext_ret_type);
  };
  it.it_module_declaration = [](TypeIterators& it, const ModuleDeclaration* md) {
    it.it_module_type(it, md->md_type);
  };
  it.it_modtype_declaration = [](TypeIterators& it, const ModtypeDeclaration* mtd) {
    if (mtd->mtd_type) it.it_module_type(it, mtd->mtd_type);
  };
  it.it_class_declaration = [](TypeIterators& it, const ClassDeclaration* cd) {
    for (TypeExpr* t : cd->cty_params) it.it_type_expr(it, t);
    it.it_class_type(it, cd->cty_type);
    if (cd->cty_new) it.it_type_expr(it, cd->cty_new);
    it.it_path(cd->cty_path);
  };
  it.it_class_type_declaration = [](TypeIterators& it, const ClassTypeDeclaration* ctd) {
    for (TypeExpr* t : ctd->clty_params) it.it_type_expr(it, t);
    it.it_class_type(it, ctd->clty_type);
    it.it_path(ctd->clty_path);
  };
  it.it_functor_param = [](TypeIterators& it, const FunctorParameter& p) {
    if (!p.is_unit) it.it_module_type(it, p.mty);
  };
  it.it_module_type = [](TypeIterators& it, const ModuleType* mt) {
    switch (mt->kind) {
      case ModuleType::Kind::Mty_ident:
      case ModuleType::Kind::Mty_alias: it.it_path(mt->path); break;
      case ModuleType::Kind::Mty_signature: it.it_signature(it, mt->sign); break;
      case ModuleType::Kind::Mty_functor:
        it.it_functor_param(it, mt->param);
        it.it_module_type(it, mt->res);
        break;
    }
  };
  it.it_class_type = [](TypeIterators& it, const ClassType* ct) {
    switch (ct->kind) {
      case ClassType::Kind::Cty_constr:
        it.it_path(ct->path);
        for (TypeExpr* t : ct->args) it.it_type_expr(it, t);
        it.it_class_type(it, ct->cty);
        break;
      case ClassType::Kind::Cty_signature: {
        ClassSignature* cs = ct->sign;
        it.it_type_expr(it, cs->csig_self);
        it.it_type_expr(it, cs->csig_self_row);
        cs->csig_vars.iter([&](std::string_view, const VarEntry& e) { it.it_type_expr(it, e.ty); });
        cs->csig_meths.iter([&](std::string_view, const MethEntry& e) { it.it_type_expr(it, e.ty); });
        break;
      }
      case ClassType::Kind::Cty_arrow:
        it.it_type_expr(it, ct->arg);
        it.it_class_type(it, ct->cty);
        break;
    }
  };
  it.it_type_kind = [](TypeIterators& it, const TypeKind* k) {
    iter_type_expr_kind([&](TypeExpr* t) { it.it_type_expr(it, t); }, k);
  };
  it.it_path = [](Path::t) {};
  it.it_type_expr = [](TypeIterators&, TypeExpr*) {};
  it.it_do_type_expr = [](TypeIterators&, TypeExpr*) {};
  return it;
}

TypeIterators type_iterators(TypeMark& mark) {
  TypeIterators it = type_iterators_without_type_expr();
  it.it_type_expr = [&mark](TypeIterators& it, TypeExpr* ty) {
    if (try_mark_node(mark, ty)) it.it_do_type_expr(it, ty);
  };
  it.it_do_type_expr = [](TypeIterators& it, TypeExpr* ty) {
    iter_type_expr([&](TypeExpr* t) { it.it_type_expr(it, t); }, ty);
    const TypeDesc* d = get_desc(ty);
    switch (d->kind) {
      case DescKind::Tconstr: it.it_path(as<Tconstr>(d)->path); break;
      case DescKind::Tobject:
        if (auto* nm = as<Tobject>(d)->name->contents) it.it_path(nm->path);
        break;
      case DescKind::Tfunctor: it.it_path(as<Tfunctor>(d)->pack->pack_path); break;
      case DescKind::Tpackage: it.it_path(as<Tpackage>(d)->pack->pack_path); break;
      case DescKind::Tvariant:
        if (const PathArgs* nm = row_name(as<Tvariant>(d)->row)) it.it_path(nm->path);
        break;
      default: break;
    }
  };
  return it;
}

// ---- copying --------------------------------------------------------------
const RowDesc* copy_row(FnRef<TypeExpr*(TypeExpr*)> f, bool fixed,
                        const RowDesc* row, bool keep, TypeExpr* more) {
  RowDescRepr r = row_repr(row);
  std::vector<RowFieldEntry> fields;
  for (auto& e : r.fields) {
    auto v = row_field_repr(e.field);
    const RowField* nf = nullptr;
    switch (v.kind) {
      case RowFieldView::Kind::Rpresent:
        nf = rf_present(v.present ? f(v.present) : nullptr);
        break;
      case RowFieldView::Kind::Reither: {
        const RowField* use_ext_of = keep ? e.field : nullptr;
        bool m = is_fixed(row) ? fixed : v.matched;
        std::vector<TypeExpr*> tl;
        for (TypeExpr* t : v.arg_types) tl.push_back(f(t));
        nf = rf_either(use_ext_of, v.constant, slice(tl), m);
        break;
      }
      case RowFieldView::Kind::Rabsent:
        nf = rf_absent();
        break;
    }
    fields.push_back({e.label, nf});
  }
  const PathArgs* name = nullptr;
  if (r.name) {
    std::vector<TypeExpr*> tl;
    for (TypeExpr* t : r.name->args) tl.push_back(f(t));
    name = make<PathArgs>(r.name->path, slice(tl));
  }
  const FixedExplanation* fx = fixed ? r.fixed : nullptr;
  return create_row(slice(fields), more, r.closed, fx, name);
}

Commutable* copy_commu(Commutable* c) { return is_commu_ok(c) ? commu_ok() : commu_var(); }

const TypeDesc* copy_type_desc(FnRef<TypeExpr*(TypeExpr*)> f,
                               const TypeDesc* d, bool keep_names) {
  switch (d->kind) {
    case DescKind::Tvar:
      return keep_names ? d : TVAR_NONE_LIT();
    case DescKind::Tarrow: {
      // OCaml evaluates constructor arguments right to left.
      auto* a = as<Tarrow>(d);
      Commutable* c = copy_commu(a->commu);
      TypeExpr* t2 = f(a->t2);
      TypeExpr* t1 = f(a->t1);
      return tarrow(a->label, t1, t2, c);
    }
    case DescKind::Ttuple: {
      std::vector<LabeledTy> l;
      for (auto& e : as<Ttuple>(d)->elems) l.push_back({e.label, f(e.ty)});
      return ttuple(slice(l));
    }
    case DescKind::Tconstr: {
      auto* c = as<Tconstr>(d);
      std::vector<TypeExpr*> l;
      for (TypeExpr* t : c->args) l.push_back(f(t));
      return tconstr(c->path, slice(l), make<MemoRef>(mnil()));
    }
    case DescKind::Tobject: {
      auto* o = as<Tobject>(d);
      if (const PathArgs* nm = o->name->contents) {
        std::vector<TypeExpr*> tl;  // right to left: the name's args first
        for (TypeExpr* t : nm->args) tl.push_back(f(t));
        TypeExpr* fl = f(o->fields);
        return tobject(fl, make<NameRef>(make<PathArgs>(nm->path, slice(tl))));
      }
      return tobject(f(o->fields), make<NameRef>(nullptr));
    }
    case DescKind::Tvariant:
      throw std::logic_error("Btype.copy_type_desc: Tvariant");  // too ambiguous
    case DescKind::Tfield: {
      auto* x = as<Tfield>(d);
      // the kind is kept shared, with indirections removed for performance
      TypeExpr* t2 = f(x->rest);  // right to left
      TypeExpr* t1 = f(x->ty);
      return tfield(x->label, field_kind_internal_repr(x->kind_), t1, t2);
    }
    case DescKind::Tnil:
      return tnil();
    case DescKind::Tunivar:
      return d;  // always keep the name
    case DescKind::Tpoly: {
      auto* p = as<Tpoly>(d);
      std::vector<TypeExpr*> tyl;
      for (TypeExpr* t : p->vars) tyl.push_back(f(t));
      return tpoly(f(p->body), slice(tyl));
    }
    case DescKind::Tpackage: {
      auto* p = as<Tpackage>(d)->pack;
      std::vector<PackConstraint> cs;
      for (auto& c : p->pack_constraints) cs.push_back({c.path, f(c.ty)});
      return tpackage(make<Package>(p->pack_path, slice(cs)));
    }
    default:
      // Tfunctor would break unicity of the unscoped binding; Tlink, Tsubst,
      // Texpand never reach a copy.
      throw std::logic_error("Btype.copy_type_desc");
  }
}

void redirect_desc(CopyScope& scope, TypeExpr* ty, const TypeDesc* desc) {
  ty = repr(ty);
  scope.saved_desc.emplace_back(ty, ty->desc);
  transient_expr::set_desc(ty, desc);
}

// saved_desc buffers of finished copy scopes, reused (the scopes nest)
static std::vector<std::vector<std::pair<TypeExpr*, const TypeDesc*>>> g_saved_desc_free;

void with_copy_scope(FnRef<void(CopyScope&)> f) {
  CopyScope scope;
  if (!g_saved_desc_free.empty()) {
    scope.saved_desc = std::move(g_saved_desc_free.back());
    g_saved_desc_free.pop_back();
  }
  struct Cleanup {
    CopyScope& s;
    ~Cleanup() {
      // List.iter over saved_desc, which conses: newest first
      for (auto it = s.saved_desc.rbegin(); it != s.saved_desc.rend(); ++it)
        transient_expr::set_desc(it->first, it->second);
      s.saved_desc.clear();
      g_saved_desc_free.push_back(std::move(s.saved_desc));
    }
  } cleanup{scope};
  f(scope);
}

// ---- memorization of abbreviation expansion --------------------------------
static bool lte_public(PrivateFlag p1, PrivateFlag p2) {
  return p1 == PrivateFlag::Private || p2 == PrivateFlag::Public;
}

TypeExpr* find_expans(PrivateFlag priv, Path::t p1, const AbbrevMemo* m) {
  for (;;) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return nullptr;
      case AbbrevMemo::Kind::Mcons:
        if (lte_public(priv, m->privacy) && path::same(p1, m->path)) return m->expansion;
        m = m->rem;
        continue;
      case AbbrevMemo::Kind::Mlink:
        m = m->link->contents;
        continue;
    }
  }
}

static std::vector<MemoRef*> g_memo;  // Local_store s_ref []

void cleanup_abbrev_memo() {
  for (MemoRef* r : g_memo) r->contents = mnil();
  g_memo.clear();
}

void memorize_abbrev(MemoRef* mem, PrivateFlag privacy, Path::t path,
                     TypeExpr* abbreviation, TypeExpr* expansion) {
  mem->contents = make<AbbrevMemo>(AbbrevMemo::Kind::Mcons, privacy, path, abbreviation,
                                   expansion, mem->contents);
  g_memo.push_back(mem);
}

struct ExitEx {};

static const AbbrevMemo* forget_abbrev_rec(const AbbrevMemo* mem, Path::t path) {
  switch (mem->kind) {
    case AbbrevMemo::Kind::Mnil:
      return mem;
    case AbbrevMemo::Kind::Mcons:
      if (path::same(path, mem->path)) return mem->rem;
      return make<AbbrevMemo>(AbbrevMemo::Kind::Mcons, mem->privacy, mem->path,
                              mem->abbreviation, mem->expansion,
                              forget_abbrev_rec(mem->rem, path));
    case AbbrevMemo::Kind::Mlink:
      mem->link->contents = forget_abbrev_rec(mem->link->contents, path);
      throw ExitEx{};
  }
  return mem;
}

void forget_abbrev_memo(MemoRef* mem, Path::t path) {
  try {
    mem->contents = forget_abbrev_rec(mem->contents, path);
  } catch (const ExitEx&) {
  }
}

types::Snapshot snapshot() { return types::snapshot(); }
void backtrack(types::Snapshot s) { types::backtrack(cleanup_abbrev_memo, s); }

// ---- labels -------------------------------------------------------------------
bool is_optional(const ArgLabel& l) { return l.kind == ArgLabel::Kind::Optional; }
std::string_view label_name(const ArgLabel& l) {
  return l.kind == ArgLabel::Kind::Nolabel ? std::string_view{} : l.name;
}
std::string prefixed_label_name(const ArgLabel& l) {
  switch (l.kind) {
    case ArgLabel::Kind::Nolabel: return "";
    case ArgLabel::Kind::Labelled: return "~" + std::string(l.name);
    case ArgLabel::Kind::Optional: return "?" + std::string(l.name);
  }
  return "";
}

// ---- class types ------------------------------------------------------------
ClassSignature* signature_of_class_type(const ClassType* cty) {
  for (;;) {
    switch (cty->kind) {
      case ClassType::Kind::Cty_constr:
      case ClassType::Kind::Cty_arrow: cty = cty->cty; continue;
      case ClassType::Kind::Cty_signature: return cty->sign;
    }
  }
}
const ClassType* class_body(const ClassType* cty) {
  while (cty->kind == ClassType::Kind::Cty_arrow) cty = cty->cty;
  return cty;
}
const ClassType* scrape_class_type(const ClassType* cty) {
  while (cty->kind == ClassType::Kind::Cty_constr) cty = cty->cty;
  return cty;
}
long class_type_arity(const ClassType* cty) {
  long n = 0;
  for (;;) {
    switch (cty->kind) {
      case ClassType::Kind::Cty_constr: cty = cty->cty; continue;
      case ClassType::Kind::Cty_signature: return n;
      case ClassType::Kind::Cty_arrow: ++n; cty = cty->cty; continue;
    }
  }
}
const ClassType* abbreviate_class_type(Path::t path, Slice<TypeExpr*> params,
                                       const ClassType* cty) {
  if (cty->kind == ClassType::Kind::Cty_arrow) {
    auto* c = make<ClassType>(*cty);
    c->cty = abbreviate_class_type(path, params, cty->cty);
    return c;
  }
  auto* c = make<ClassType>(ClassType::Kind::Cty_constr);
  c->path = path;
  c->args = params;
  c->cty = cty;
  return c;
}
TypeExpr* self_type(const ClassType* cty) { return signature_of_class_type(cty)->csig_self; }
TypeExpr* self_type_row(const ClassType* cty) {
  return signature_of_class_type(cty)->csig_self_row;
}

// Meths.fold (fun name .. l -> name :: l) m []: keys in DECREASING order.
std::vector<std::string_view> methods(const ClassSignature* sign) {
  std::vector<std::string_view> l;
  sign->csig_meths.iter([&](std::string_view n, const MethEntry&) { l.insert(l.begin(), n); });
  return l;
}
std::vector<std::string_view> virtual_methods(const ClassSignature* sign) {
  std::vector<std::string_view> l;
  sign->csig_meths.iter([&](std::string_view n, const MethEntry& e) {
    if (e.virt == VirtualFlag::Virtual) l.insert(l.begin(), n);
  });
  return l;
}
std::set<std::string_view> concrete_methods(const ClassSignature* sign) {
  std::set<std::string_view> s;
  sign->csig_meths.iter([&](std::string_view n, const MethEntry& e) {
    if (e.virt == VirtualFlag::Concrete) s.insert(n);
  });
  return s;
}
std::vector<std::string_view> public_methods(const ClassSignature* sign) {
  std::vector<std::string_view> l;
  sign->csig_meths.iter([&](std::string_view n, const MethEntry& e) {
    if (!e.priv.is_private) l.insert(l.begin(), n);
  });
  return l;
}
std::vector<std::string_view> instance_vars(const ClassSignature* sign) {
  std::vector<std::string_view> l;
  sign->csig_vars.iter([&](std::string_view n, const VarEntry&) { l.insert(l.begin(), n); });
  return l;
}
std::vector<std::string_view> virtual_instance_vars(const ClassSignature* sign) {
  std::vector<std::string_view> l;
  sign->csig_vars.iter([&](std::string_view n, const VarEntry& e) {
    if (e.virt == VirtualFlag::Virtual) l.insert(l.begin(), n);
  });
  return l;
}
std::set<std::string_view> concrete_instance_vars(const ClassSignature* sign) {
  std::set<std::string_view> s;
  sign->csig_vars.iter([&](std::string_view n, const VarEntry& e) {
    if (e.virt == VirtualFlag::Concrete) s.insert(n);
  });
  return s;
}
TypeExpr* method_type(std::string_view label, const ClassSignature* sign) {
  auto* e = sign->csig_meths.find_opt(label);
  if (!e) throw std::logic_error("Btype.method_type");
  return e->ty;
}
TypeExpr* instance_variable_type(std::string_view label, const ClassSignature* sign) {
  auto* e = sign->csig_vars.find_opt(label);
  if (!e) throw std::logic_error("Btype.instance_variable_type");
  return e->ty;
}

// ---- deep occurrences ---------------------------------------------------------
struct Occur {};

static void deep_occur_rec(TypeMark& mark, TypeExpr* t0, TypeExpr* ty) {
  if (get_level(ty) >= get_level(t0) && try_mark_node(mark, ty)) {
    if (eq_type(ty, t0)) throw Occur{};
    iter_type_expr([&](TypeExpr* t) { deep_occur_rec(mark, t0, t); }, ty);
    iter_abbrev(
        [&](Path::t, Slice<TypeExpr*> tyl) {
          for (TypeExpr* t : tyl) deep_occur_rec(mark, t0, t);
        },
        ty);
  }
}

bool deep_occur(TypeExpr* t0, TypeExpr* ty) {
  try {
    with_type_mark([&](TypeMark& mark) { deep_occur_rec(mark, t0, ty); });
    return false;
  } catch (const Occur&) {
    return true;
  }
}

bool deep_occur_list(TypeExpr* t0, const std::vector<TypeExpr*>& tyl) {
  try {
    with_type_mark([&](TypeMark& mark) {
      for (TypeExpr* t : tyl) deep_occur_rec(mark, t0, t);
    });
    return false;
  } catch (const Occur&) {
    return true;
  }
}

const TypeDesc* get_folded_desc(bool keep_Tvar, TypeExpr* ty) {
  const TypeDesc* desc = get_desc(ty);
  if (desc->kind == DescKind::Tsubst) return desc;
  if (desc->kind == DescKind::Tvar && keep_Tvar) return desc;
  if (const PathArgs* a = get_abbrev(ty)) {
    std::vector<TypeExpr*> args(a->args.begin(), a->args.end());
    if (!(path::contains_unscoped_ident(a->path) || deep_occur_list(ty, args)))
      return tconstr(a->path, a->args, make<MemoRef>(mnil()));
  }
  return desc;
}

}  // namespace cppcaml::typing::btype
