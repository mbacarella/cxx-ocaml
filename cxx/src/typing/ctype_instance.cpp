// Port of typing/ctype.ml, part 2: instantiation ("Instantiation" to
// "Abbreviation expansion").
#include <algorithm>
#include <climits>

#include "cppcaml/typing/ctype.hpp"
#include "ctype_internal.hpp"
#include "cppcaml/typing/datarepr.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;
using namespace internal;

static const AbbrevMemo* find_repr(Path::t p1, const AbbrevMemo* m) {
  for (;;) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return nullptr;
      case AbbrevMemo::Kind::Mcons:
        if (m->privacy == PrivateFlag::Public && path::same(p1, m->path)) return m;
        m = m->rem;
        continue;
      case AbbrevMemo::Kind::Mlink:
        m = m->link->contents;
        continue;
    }
  }
}

// `abbreviations = ref (ref Mnil)`: the memo copy attaches to Tconstr nodes
MemoRef* abbreviations = [] {
  ZoneScope perm(permanent_zone());
  return make<MemoRef>(mnil());
}();

UnscopedMapping empty_unscoped_mapping() { return {{}, [](TypeExpr*) { return true; }}; }

UnscopedMapping compute_new_closed(ident::Unscoped* us, ident::Unscoped* us2,
                                   const std::vector<std::pair<Ident::t, Path::t>>& id_map0,
                                          TypeExpr* ty) {
  Ident::t id = Ident::of_unscoped(us);
  Ident::t id2 = Ident::of_unscoped(us2);
  std::vector<std::pair<Ident::t, Path::t>> id_map{{id, Path::pident(id2)}};
  for (auto& e : id_map0)
    if (!ident::same(e.first, id)) id_map.push_back(e);
  std::vector<Ident::t> ids;
  for (auto& e : id_map) ids.push_back(e.first);
  auto tyset = std::make_shared<TypeSet>(type_subexpressions_with_free_occurrences(ids, ty));
  return {id_map, [tyset](TypeExpr* t) { return !tyset->mem(t); }};
}

TypeExpr* copy(CopyScope& copy_scope, TypeExpr* ty, const Partial* partial, bool keep_names,
               std::optional<long> scope, const UnscopedMapping* unscoped0) {
  static const UnscopedMapping empty = empty_unscoped_mapping();
  const UnscopedMapping& unscoped = unscoped0 ? *unscoped0 : empty;
  auto copy_ = [&](TypeExpr* t) {
    return copy(copy_scope, t, partial, keep_names, scope, &unscoped);
  };
  const TypeDesc* desc = get_desc(ty);
  if (auto* s = as<Tsubst>(desc)) return s->ty;
  long level = get_level(ty);
  if (level != generic_level && !partial && unscoped.closed(ty)) return ty;
  // We only forget types that are non generic and do not contain free univars
  long forget;
  if (level == generic_level || !partial) {
    forget = generic_level;
  } else if (!is_Tpoly(ty) && partial->free_univars(ty).s.empty() && unscoped.closed(ty)) {
    forget = partial->keep ? level : current_level;
  } else {
    forget = generic_level;
  }
  if (forget != generic_level) return newty2(forget, TVAR_NONE_LIT());
  long ty_scope = scope ? std::max(*scope, get_scope(ty)) : get_scope(ty);
  const PathArgs* ty_expand = get_abbrev(ty);
  TypeExpr* t = newstub(ty_scope);
  redirect_desc(copy_scope, ty, btype::scoped_tsubst(t, nullptr));
  const TypeDesc* desc2 = nullptr;
  switch (desc->kind) {
    case DescKind::Tconstr: {
      auto* c = as<Tconstr>(desc);
      Path::t p = path::subst(unscoped.map, c->path);
      MemoRef* abbrevs = proper_abbrevs(c->args, abbreviations);
      const AbbrevMemo* found = find_repr(p, abbrevs->contents);
      if (found && !eq_type(found->abbreviation, t)) {
        desc2 = tlink(found->abbreviation);
      } else {
        // One must allocate a new reference, so that abbreviations belonging
        // to different branches of a type are independent; a reference
        // containing a Mcons must be shared.  (Constructor arguments run
        // right to left: the ref, then the argument copies.)
        const AbbrevMemo* cur = abbreviations->contents;
        MemoRef* memo = make<MemoRef>(
            cur->kind == AbbrevMemo::Kind::Mcons
                ? make<AbbrevMemo>(AbbrevMemo::Kind::Mlink, PrivateFlag::Public, nullptr, nullptr,
                                   nullptr, nullptr, abbreviations)
                : cur);
        std::vector<TypeExpr*> tl;
        for (TypeExpr* a : c->args) tl.push_back(copy_(a));
        desc2 = tconstr(p, slice(tl), memo);
      }
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
      // If the row variable is not generic, we must keep it
      bool keep = get_level(more) != generic_level && !partial;
      TypeExpr* more2;
      if (ms) more2 = ms->ty;
      else if (mored->kind == DescKind::Tconstr || mored->kind == DescKind::Tnil) more2 = copy_(more);
      else if (mored->kind == DescKind::Tvar || mored->kind == DescKind::Tunivar)
        more2 = keep ? more : newty(mored);
      else throw std::logic_error("Ctype.copy: row_more");
      if (auto* c = as<Tconstr>(get_desc(more2)); c && !is_fixed(row)) {  // PR#6163
        RowDescRepr r = row_repr(row);
        row = create_row(slice(r.fields), r.more, r.closed,
                         make<FixedExplanation>(FixedExplanation::Kind::Reified, nullptr, c->path),
                         r.name);
      }
      // Open row if partial for pattern and contains Reither
      if (partial && !partial->keep) {
        auto not_reither = [](const RowFieldEntry& e) {
          return row_field_repr(e.field).kind != RowFieldView::Kind::Reither;
        };
        std::vector<RowFieldEntry> fields = row_fields(row);
        if (row_closed(row) && !is_fixed(row) && partial->free_univars(ty).s.empty() &&
            !std::all_of(fields.begin(), fields.end(), not_reither)) {
          more2 = newvar();
          std::vector<RowFieldEntry> kept;
          for (auto& e : fields)
            if (not_reither(e)) kept.push_back(e);
          row = create_row(slice(kept), more2, false, nullptr, nullptr);
        }
      }
      // Register new type first for recursion
      redirect_desc(copy_scope, more, btype::scoped_tsubst(more2, t));
      // Return a new copy
      row = subst_row_name_path(unscoped.map, row);
      desc2 = tvariant(copy_row(copy_, true, row, keep, more2));
      break;
    }
    case DescKind::Tobject: {
      auto* o = as<Tobject>(desc);
      if (partial) {
        desc2 = tobject(copy_(o->fields), make<NameRef>(nullptr));
      } else if (const PathArgs* nm = o->name->contents) {
        // Tobject (copy ty, ref (Some (p, List.map copy tl))): right to left
        Path::t p = path::subst(unscoped.map, nm->path);
        std::vector<TypeExpr*> tl;
        for (TypeExpr* a : nm->args) tl.push_back(copy_(a));
        NameRef* name = make<NameRef>(make<PathArgs>(p, slice(tl)));
        desc2 = tobject(copy_(o->fields), name);
      } else {
        desc2 = copy_type_desc(copy_, desc, keep_names);
      }
      break;
    }
    case DescKind::Tfunctor: {
      auto* fu = as<Tfunctor>(desc);
      auto psubst = [&](Path::t p) { return path::subst(unscoped.map, p); };
      const Package* pack2 = map_pack(psubst, copy_, fu->pack);
      ident::Unscoped* us2 = ident::Unscoped::refresh(fu->id);
      UnscopedMapping nm = compute_new_closed(fu->id, us2, unscoped.map, fu->body);
      TypeExpr* ty2 = copy(copy_scope, fu->body, partial, keep_names, scope, &nm);
      desc2 = tfunctor(fu->label, us2, pack2, ty2);
      break;
    }
    case DescKind::Tpackage: {
      auto psubst = [&](Path::t p) { return path::subst(unscoped.map, p); };
      desc2 = tpackage(map_pack(psubst, copy_, as<Tpackage>(desc)->pack));
      break;
    }
    default:
      desc2 = copy_type_desc(copy_, desc, keep_names);
  }
  if (ty_expand) {
    std::vector<TypeExpr*> args;
    for (TypeExpr* a : ty_expand->args) args.push_back(copy_(a));
    TypeExpr* t2 = new_scoped_ty(ty_scope, desc2);
    desc2 = texpand(t2, ty_expand->path, slice(args));
  }
  transient_expr::set_stub_desc(t, desc2);
  return t;
}

// ---- variants of instantiation ---------------------------------------------------
TypeExpr* instance(TypeExpr* sch, std::optional<bool> partial) {
  std::optional<Partial> p;
  if (partial) p = Partial{compute_univars(sch), *partial};
  TypeExpr* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = copy(cs, sch, p ? &*p : nullptr); });
  return r;
}

TypeExpr* generic_instance(TypeExpr* sch) {
  return with_level(generic_level, [&] { return instance(sch); });
}

std::vector<TypeExpr*> instance_list(const std::vector<TypeExpr*>& schl) {
  std::vector<TypeExpr*> r;
  with_copy_scope([&](CopyScope& cs) {
    for (TypeExpr* t : schl) r.push_back(copy(cs, t));
  });
  return r;
}

TypeExpr* subst_unscoped(ident::Unscoped* us1, ident::Unscoped* us2, TypeExpr* ty) {
  std::vector<std::pair<Ident::t, Path::t>> id_map{
      {Ident::of_unscoped(us1), Path::pident(Ident::of_unscoped(us2))}};
  auto tyset = std::make_shared<TypeSet>(
      type_subexpressions_with_free_occurrences({id_map[0].first}, ty));
  UnscopedMapping u{id_map, [tyset](TypeExpr* t) { return !tyset->mem(t); }};
  TypeExpr* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = copy(cs, ty, nullptr, false, std::nullopt, &u); });
  return r;
}

// Misc.find_first_mono
static long find_first_mono(const std::function<bool(long)>& p) {
  if (p(0)) return 0;
  const long max_int = LONG_MAX >> 1;  // OCaml's max_int on 64 bits
  long low = 0, jump = 1, high = max_int;
  for (;;) {
    if (low + 1 == high) return high;
    if (jump < 1) {
      jump = 1;
      continue;
    }
    if (jump >= high - low) {
      jump = (high - low) / 2;
      continue;
    }
    if (p(low + jump)) {
      high = low + jump;
      jump = jump / 2;
    } else {
      low = low + jump;
      jump = std::max(jump, 2 * jump);  // avoid overflows
    }
  }
}

// Create unique names to new type constructors (existentials, local
// constraints).
std::string_view get_new_abstract_name(env::t env, std::string_view s) {
  auto name = [&](long index) {
    if (index == 0 && !s.empty() && s.back() != '$') return std::string(s);
    return std::string(s) + std::to_string(index);
  };
  long index = find_first_mono([&](long i) {
    try {
      env::find_type_by_name(Longident::lident(name(i)), env);
      return false;
    } catch (const env::NotFound&) {
      return true;
    }
  });
  // `name 0` is s itself
  if (index == 0 && !s.empty() && s.back() != '$') return zborrow(s);
  return zborrow(name(index));
}

const TypeDeclaration* new_local_type(TypeOrigin origin, const Location& loc, TypeExpr* manifest,
                                      long scope) {
  auto* kind = make<TypeKind>();
  kind->kind = TypeKind::Kind::Type_abstract;
  kind->origin = origin;
  long expansion_scope = manifest ? scope : lowest_level;
  return make<TypeDeclaration>(Slice<TypeExpr*>{}, 0L, kind, PrivateFlag::Public, manifest,
                               Slice<variance::t>{}, Slice<Separability>{}, true, expansion_scope,
                               loc, Attributes{}, TypeImmediacy::Unknown, false,
                               uid::mk(env::get_current_unit()));
}

// Misc.letter_of_int
static std::string letter_of_int(long n) {
  std::string letter(1, static_cast<char>('a' + n % 26));
  long num = n / 26;
  return num == 0 ? letter : letter + std::to_string(num);
}

static std::string existential_name(long& name_counter, TypeExpr* ty) {
  std::string name;
  if (auto* v = as<Tvar>(get_desc(ty)); v && v->name.some) {
    name = std::string(v->name.v);
  } else {
    name = letter_of_int(name_counter);
    ++name_counter;
  }
  return "$" + name;
}

InstancedConstructor instance_constructor(ExistentialTreatment et,
                                          const ConstructorDescription* cstr) {
  InstancedConstructor r;
  with_copy_scope([&](CopyScope& copy_scope) {
    long name_counter = 0;
    auto copy_existential = [&](TypeExpr* existential) -> TypeExpr* {
      if (!et.make_abstract) return copy(copy_scope, existential);
      PatternEnv* penv = et.make_abstract;
      env::t env = penv->env;
      long fresh_constr_scope = penv->equations_scope;
      TypeOrigin origin;
      origin.kind = TypeOrigin::Kind::Existential;
      origin.existential = cstr->cstr_name;
      origin.obj = fresh_identity();  // `Existential cstr_name`: one block per call
      const TypeDeclaration* decl = new_local_type(origin);
      std::string name = existential_name(name_counter, existential);
      Ident::t id = penv->enter_type(fresh_constr_scope, get_new_abstract_name(env, name), decl);
      TypeExpr* to_unify = newty(tconstr(Path::pident(id), {}, make<MemoRef>(mnil())));
      TypeExpr* tv = copy(copy_scope, existential);
      if (!is_Tvar(tv)) throw std::logic_error("Ctype.instance_constructor");
      link_type(tv, to_unify);
      return tv;
    };
    for (TypeExpr* e : cstr->cstr_existentials) r.existentials.push_back(copy_existential(e));
    r.res = copy(copy_scope, cstr->cstr_res);
    for (TypeExpr* a : cstr->cstr_args) r.args.push_back(copy(copy_scope, a));
  });
  return r;
}

std::pair<std::vector<TypeExpr*>, TypeExpr*> instance_parameterized_type(
    Slice<TypeExpr*> sch_args, TypeExpr* sch, bool keep_names, std::optional<long> scope) {
  std::pair<std::vector<TypeExpr*>, TypeExpr*> r;
  with_copy_scope([&](CopyScope& cs) {
    // Only raise scope in body, not in parameters
    for (TypeExpr* t : sch_args) r.first.push_back(copy(cs, t, nullptr, keep_names));
    r.second = copy(cs, sch, nullptr, false, scope);
  });
  return r;
}

const TypeKind* internal::map_kind(const std::function<TypeExpr*(TypeExpr*)>& f,
                                   const TypeKind* k) {
  switch (k->kind) {
    case TypeKind::Kind::Type_variant: {
      auto* r = make<TypeKind>(*k);
      std::vector<const ConstructorDeclaration*> cl;
      for (auto* c : k->constructors) {
        // {c with cd_args = ..; cd_res = ..}: definition order right to left
        TypeExpr* res = c->cd_res ? f(c->cd_res) : nullptr;
        ConstructorArguments args = map_type_expr_cstr_args(f, c->cd_args);
        auto* n = make<ConstructorDeclaration>(*c);
        n->cd_args = args;
        n->cd_res = res;
        cl.push_back(n);
      }
      r->constructors = slice(cl);
      return r;
    }
    case TypeKind::Kind::Type_record: {
      auto* r = make<TypeKind>(*k);
      std::vector<const LabelDeclaration*> fl;
      for (auto* l : k->labels) {
        auto* n = make<LabelDeclaration>(*l);
        n->ld_type = f(l->ld_type);
        fl.push_back(n);
      }
      r->labels = slice(fl);
      return r;
    }
    case TypeKind::Kind::Type_abstract:
    case TypeKind::Kind::Type_external:
      // `Type_abstract r -> Type_abstract r` / `Type_external name -> ..`: a new block
      return make<TypeKind>(*k);
    default:  // Type_open: an immediate
      return k;
  }
}

const TypeDeclaration* instance_declaration(const TypeDeclaration* decl) {
  const TypeDeclaration* r = nullptr;
  with_copy_scope([&](CopyScope& cs) {
    auto f = [&](TypeExpr* t) { return copy(cs, t); };
    // {decl with type_params; type_manifest; type_kind}: definition order
    // is params, arity, kind, private, manifest -> right to left: manifest,
    // kind, params
    TypeExpr* manifest = decl->type_manifest ? f(decl->type_manifest) : nullptr;
    const TypeKind* kind = map_kind(f, decl->type_kind);
    std::vector<TypeExpr*> params;
    for (TypeExpr* p : decl->type_params) params.push_back(f(p));
    auto* d = make<TypeDeclaration>(*decl);
    d->type_params = slice(params);
    d->type_manifest = manifest;
    d->manifest_obj.reset();  // a new Some block
    d->type_kind = kind;
    r = d;
  });
  return r;
}

const TypeDeclaration* generic_instance_declaration(const TypeDeclaration* decl) {
  return with_level(generic_level, [&] { return instance_declaration(decl); });
}

std::pair<std::vector<TypeExpr*>, const ClassType*> instance_class(Slice<TypeExpr*> params,
                                                                   const ClassType* cty) {
  std::pair<std::vector<TypeExpr*>, const ClassType*> r;
  with_copy_scope([&](CopyScope& cs) {
    std::function<const ClassType*(const ClassType*)> copy_class_type =
        [&](const ClassType* c) -> const ClassType* {
      auto* n = make<ClassType>(c->kind);
      switch (c->kind) {
        case ClassType::Kind::Cty_constr: {
          std::vector<TypeExpr*> tyl;
          for (TypeExpr* t : c->args) tyl.push_back(copy(cs, t));
          n->path = c->path;
          n->args = slice(tyl);
          n->cty = copy_class_type(c->cty);
          break;
        }
        case ClassType::Kind::Cty_signature: {
          const ClassSignature* sign = c->sign;
          // record fields right to left: meths, vars, dummy, self_row, self
          auto meths = sign->csig_meths.map([&](const MethEntry& e) {
            return MethEntry{e.priv, e.virt, copy(cs, e.ty)};
          });
          auto vars = sign->csig_vars.map([&](const VarEntry& e) {
            return VarEntry{e.mut, e.virt, copy(cs, e.ty)};
          });
          FieldKind* dummy = field_kind_internal_repr(sign->csig_dummy_method);
          TypeExpr* self_row = copy(cs, sign->csig_self_row);
          TypeExpr* self = copy(cs, sign->csig_self);
          n->sign = make<ClassSignature>(self, self_row, dummy, vars, meths);
          break;
        }
        case ClassType::Kind::Cty_arrow: {
          // Cty_arrow (l, copy ty, copy_class_type cty): right to left
          n->cty = copy_class_type(c->cty);
          n->label = c->label;
          n->arg = copy(cs, c->arg);
          break;
        }
      }
      return n;
    };
    for (TypeExpr* p : params) r.first.push_back(copy(cs, p));
    r.second = copy_class_type(cty);
  });
  return r;
}

// ---- instantiation for types with free universal variables ----------------------
// [copy_sep] (see the long comment in ctype.ml).  Returns nullptr when the
// output would have been the input.
static TypeExpr* copy_sep(CopyScope& copy_scope, bool fixed, TypeHash<TypeExpr*>& visited,
                          const std::vector<std::pair<Ident::t, Path::t>>& id_map, TypeExpr* sch) {
  auto free = compute_univars(sch);
  std::vector<std::function<void()>> delayed_copies;  // consed: run newest first
  auto add_delayed_copy = [&](UnscopedMapping unscoped, TypeExpr* t, TypeExpr* ty) {
    delayed_copies.insert(delayed_copies.begin(), [&copy_scope, unscoped, t, ty]() {
      transient_expr::set_stub_desc(
          t, tlink(copy(copy_scope, ty, nullptr, false, std::nullopt, &unscoped)));
    });
  };
  std::function<TypeExpr*(bool, const UnscopedMapping&, TypeExpr*)> copy_rec =
      [&](bool may_share, const UnscopedMapping& unscoped, TypeExpr* ty) -> TypeExpr* {
    auto copy_shared = [&](TypeExpr* t) { return copy_rec(true, unscoped, t); };
    TypeSet univars = free(ty);
    if (is_Tvar(ty) || (may_share && univars.s.empty() && unscoped.closed(ty))) {
      if (get_level(ty) != generic_level) return ty;
      TypeExpr* t = newstub(get_scope(ty));
      add_delayed_copy(unscoped, t, ty);
      return t;
    }
    if (TypeExpr** v = visited.find_opt(ty)) return *v;
    TypeExpr* t = newstub(get_scope(ty));
    visited.add(ty, t);
    const TypeDesc* d = get_desc(ty);
    const TypeDesc* desc2;
    switch (d->kind) {
      case DescKind::Tvariant: {
        const RowDesc* row = as<Tvariant>(d)->row;
        TypeExpr* more = row_more(row);
        // We shall really check the level on the row variable
        bool keep = is_Tvar(more) && get_level(more) != generic_level;
        // In that case we should keep the original, but we still call copy
        // to correct the levels
        if (keep) {
          add_delayed_copy(unscoped, t, ty);
          desc2 = TVAR_NONE_LIT();
          break;
        }
        TypeExpr* more2 = copy_rec(false, unscoped, more);
        bool fixed2 = fixed && (is_Tvar(more) || is_Tunivar(more));
        const RowDesc* row2 = copy_row(copy_shared, fixed2, row, keep, more2);
        desc2 = tvariant(subst_row_name_path(unscoped.map, row2));
        break;
      }
      case DescKind::Tfield: {
        auto* f = as<Tfield>(d);
        // the kind is kept shared; arguments right to left
        TypeExpr* t2 = copy_rec(false, unscoped, f->rest);
        TypeExpr* t1 = copy_shared(f->ty);
        desc2 = tfield(f->label, field_kind_internal_repr(f->kind_), t1, t2);
        break;
      }
      case DescKind::Tconstr: {
        auto* c = as<Tconstr>(d);
        std::vector<TypeExpr*> tl;
        for (TypeExpr* a : c->args) tl.push_back(copy_shared(a));
        desc2 = tconstr(path::subst(unscoped.map, c->path), slice(tl), make<MemoRef>(mnil()));
        break;
      }
      case DescKind::Tpackage: {
        auto psubst = [&](Path::t p) { return path::subst(unscoped.map, p); };
        desc2 = tpackage(map_pack(psubst, copy_shared, as<Tpackage>(d)->pack));
        break;
      }
      case DescKind::Tobject: {
        auto* o = as<Tobject>(d);
        if (const PathArgs* nm = o->name->contents) {
          Path::t p = path::subst(unscoped.map, nm->path);
          std::vector<TypeExpr*> tl;
          for (TypeExpr* a : nm->args) tl.push_back(copy_shared(a));
          NameRef* name = make<NameRef>(make<PathArgs>(p, slice(tl)));
          desc2 = tobject(copy_shared(o->fields), name);
        } else {
          desc2 = copy_type_desc(copy_shared, d);
        }
        break;
      }
      case DescKind::Tfunctor: {
        auto* fu = as<Tfunctor>(d);
        auto psubst = [&](Path::t p) { return path::subst(unscoped.map, p); };
        const Package* pack2 = map_pack(psubst, copy_shared, fu->pack);
        ident::Unscoped* us2 = ident::Unscoped::refresh(fu->id);
        UnscopedMapping nm = compute_new_closed(fu->id, us2, unscoped.map, fu->body);
        TypeExpr* ty2 = copy_rec(true, nm, fu->body);
        desc2 = tfunctor(fu->label, us2, pack2, ty2);
        break;
      }
      default:
        desc2 = copy_type_desc(copy_shared, d);
    }
    transient_expr::set_stub_desc(t, desc2);
    return t;
  };
  std::vector<Ident::t> ids;
  for (auto& e : id_map) ids.push_back(e.first);
  auto tyset = std::make_shared<TypeSet>(type_subexpressions_with_free_occurrences(ids, sch));
  auto closed = [tyset](TypeExpr* t) { return !tyset->mem(t); };
  if (free(sch).s.empty() && closed(sch) && get_level(sch) != generic_level) return nullptr;
  UnscopedMapping u{id_map, closed};
  TypeExpr* ty = copy_rec(true, u, sch);
  for (auto& force : delayed_copies) force();
  return ty;
}

static std::pair<std::vector<TypeExpr*>, TypeExpr*> instance_poly2(
    CopyScope& copy_scope, bool keep_names, bool fixed, Slice<TypeExpr*> univars, TypeExpr* sch) {
  // In order to compute univars below, [sch] should not contain [Tsubst]
  auto copy_var = [&](TypeExpr* ty) {
    auto* u = as<Tunivar>(get_desc(ty));
    if (!u) throw std::logic_error("Ctype.instance_poly'");
    return keep_names ? newty(tvar(u->name)) : newvar();
  };
  std::vector<TypeExpr*> vars;
  for (TypeExpr* u : univars) vars.push_back(copy_var(u));
  TypeHash<TypeExpr*> visited;
  for (std::size_t k = 0; k < univars.size(); ++k) visited.add(univars[k], vars[k]);
  TypeExpr* ty = copy_sep(copy_scope, fixed, visited, {}, sch);
  return {vars, ty ? ty : sch};
}

std::pair<std::vector<TypeExpr*>, TypeExpr*> instance_poly_fixed(Slice<TypeExpr*> univars,
                                                                 TypeExpr* sch, bool keep_names) {
  std::pair<std::vector<TypeExpr*>, TypeExpr*> r;
  with_copy_scope([&](CopyScope& cs) { r = instance_poly2(cs, keep_names, true, univars, sch); });
  return r;
}

TypeExpr* instance_poly(Slice<TypeExpr*> univars, TypeExpr* sch, bool keep_names) {
  TypeExpr* r = nullptr;
  with_copy_scope(
      [&](CopyScope& cs) { r = instance_poly2(cs, keep_names, false, univars, sch).second; });
  return r;
}

TypeExpr* maybe_instance_poly(TypeExpr* ty) {
  if (auto* p = as<Tpoly>(get_desc(ty))) return instance_poly(p->vars, p->body, true);
  return ty;
}

TypeExpr* instance_funct_opt(Ident::t id_in, Path::t p_out, bool fixed, TypeExpr* sch) {
  TypeHash<TypeExpr*> visited;
  TypeExpr* r = nullptr;
  with_copy_scope([&](CopyScope& cs) { r = copy_sep(cs, fixed, visited, {{id_in, p_out}}, sch); });
  return r;
}

TypeExpr* instance_funct(Ident::t id_in, Path::t p_out, bool fixed, TypeExpr* sch) {
  TypeExpr* r = instance_funct_opt(id_in, p_out, fixed, sch);
  return r ? r : sch;
}

std::pair<env::t, TypeExpr*> open_tfunctor(env::t env, const Location& loc, ident::Unscoped* us,
                                           const Package* pack, TypeExpr* ty) {
  const ModuleType* mty = modtype_of_package(env, loc, pack);
  Ident::t id = Ident::create_scoped(ident::lowest_scope, ident::Unscoped::name_of(us));
  env::t env2 = env::add_module(id, ModulePresence::Mp_present, mty, env);
  TypeExpr* ty2 = instance_funct(Ident::of_unscoped(us), Path::pident(id), false, ty);
  return {env2, ty2};
}

InstancedLabel instance_label(bool fixed, const LabelDescription* lbl) {
  InstancedLabel r;
  with_copy_scope([&](CopyScope& cs) {
    if (auto* p = as<Tpoly>(get_desc(lbl->lbl_arg))) {
      auto [vars, ty] = instance_poly2(cs, false, fixed, p->vars, p->body);
      r.vars = vars;
      r.arg = ty;
    } else {
      r.arg = copy(cs, lbl->lbl_arg);
    }
    // call [copy] after [instance_poly] to avoid introducing [Tsubst]
    r.res = copy(cs, lbl->lbl_res);
  });
  return r;
}

// ---- instantiation with parameter substitution -------------------------------------
std::function<void(const Uenv&, TypeExpr*, TypeExpr*)> unify_var_ref;

TypeExpr* subst(env::t env, long level, PrivateFlag priv, MemoRef* abbrev, TypeExpr* oty,
                Slice<TypeExpr*> params, Slice<TypeExpr*> args, TypeExpr* body,
                std::optional<long> scope) {
  if (params.size() != args.size()) throw CannotSubst{};
  return with_level(level, [&]() -> TypeExpr* {
    TypeExpr* body0 = newvar();  // Stub
    std::function<void()> undo_abbrev = [] {};  // No abbreviation added
    if (oty) {
      auto* c = as<Tconstr>(get_desc(oty));
      if (!c) throw std::logic_error("Ctype.subst");
      MemoRef* ab = proper_abbrevs(c->args, abbrev);
      memorize_abbrev(ab, priv, c->path, oty, body0);
      Path::t p = c->path;
      undo_abbrev = [ab, p] { forget_abbrev_memo(ab, p); };
    }
    abbreviations = abbrev;
    auto [params2, body2] = instance_parameterized_type(params, body, false, scope);
    abbreviations = make<MemoRef>(mnil());
    Uenv uenv = Uenv::expression(env, true);
    try {
      unify_var_ref(uenv, body0, body2);
      for (std::size_t k = 0; k < params2.size(); ++k) unify_var_ref(uenv, params2[k], args[k]);
      return body2;
    } catch (const Unify&) {
      undo_abbrev();
      throw CannotSubst{};
    }
  });
}

// Default to generic level (see ctype.ml)
TypeExpr* apply(env::t env, Slice<TypeExpr*> params, TypeExpr* body, Slice<TypeExpr*> args,
                bool use_current_level) {
  simple_abbrevs()->contents = mnil();
  long level = use_current_level ? current_level : generic_level;
  try {
    return subst(env, level, PrivateFlag::Public, make<MemoRef>(mnil()), nullptr, params, args, body);
  } catch (const CannotSubst&) {
    throw CannotApply{};
  }
}

}  // namespace cppcaml::typing::ctype
