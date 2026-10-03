// Port of typing/ctype.ml, part 1: errors, levels, Pattern_env, the
// unification environment, object / row helpers, genericity checks, level
// updates and generalization (ctype.ml up to "Instantiation").
#include "cppcaml/typing/ctype.hpp"

#include <algorithm>
#include <unordered_map>

#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/subst.hpp"

namespace cppcaml::typing::ctype {

using namespace types;
using namespace btype;

// ---- errors ------------------------------------------------------------------------
void raise_trace_for(TraceExn tr_exn, et::TypeTrace tr) {
  switch (tr_exn) {
    case TraceExn::Unify: throw UnifyTrace{std::move(tr)};
    case TraceExn::Equality: throw EqualityTrace{std::move(tr)};
    case TraceExn::Moregen: throw MoregenTrace{std::move(tr)};
  }
  throw UnifyTrace{};
}
void raise_unexplained_for(TraceExn tr_exn) { raise_trace_for(tr_exn, {}); }
void raise_for(TraceExn tr_exn, et::Elt<TypeExpr*> e) { raise_trace_for(tr_exn, {e}); }

static et::Escape<TypeExpr*> escape(typename et::Escape<TypeExpr*>::Kind k) {
  et::Escape<TypeExpr*> e;
  e.kind = k;
  return e;
}
[[noreturn]] static void raise_scope_escape_exn(TypeExpr* ty) {
  auto e = escape(et::Escape<TypeExpr*>::Kind::Equation);
  e.equation = ty;
  throw Escape(e);
}
[[noreturn]] static void raise_escape_constructor(Path::t p) {
  auto e = escape(et::Escape<TypeExpr*>::Kind::Constructor);
  e.path = p;
  throw Escape(e);
}
[[noreturn]] static void raise_escape_module_type(Path::t p) {
  auto e = escape(et::Escape<TypeExpr*>::Kind::Module_type);
  e.path = p;
  throw Escape(e);
}
[[noreturn]] static void raise_escape_self() {
  throw Escape(escape(et::Escape<TypeExpr*>::Kind::Self));
}
static et::Elt<TypeExpr*> escape_elt(const et::Escape<TypeExpr*>& e) {
  auto x = et::Elt<TypeExpr*>::mk(et::Elt<TypeExpr*>::Kind::Escape);
  x.escape = e;
  return x;
}

// ---- control tracing of GADT instances ------------------------------------------------
bool trace_gadt_instances = false;
bool check_trace_gadt_instances(env::t env, bool force) {
  if (trace_gadt_instances || !(force || env::has_local_constraints(env))) return false;
  trace_gadt_instances = true;
  cleanup_abbrev_memo();
  return true;
}
void reset_trace_gadt_instances(bool b) {
  if (b) trace_gadt_instances = false;
}

// ---- abbreviations without parameters (shall reset after generalizing) ------------
MemoRef* simple_abbrevs() {
  static MemoRef* r = [] {
    ZoneScope perm(permanent_zone());
    return make<MemoRef>(mnil());
  }();
  return r;
}
MemoRef* proper_abbrevs(Slice<TypeExpr*> tl, MemoRef* abbrev) {
  if (!tl.empty() || trace_gadt_instances || clflags::principal) return abbrev;
  return simple_abbrevs();
}

// ---- type level management ----------------------------------------------------------
long current_level = 0;
long nongen_level = 0;
long global_level = 0;
static std::vector<std::pair<long, long>> g_saved_levels;  // most recent last

long get_current_level() { return current_level; }
void init_def(long level) {
  current_level = level;
  nongen_level = level;
}
void save_levels() { g_saved_levels.emplace_back(current_level, nongen_level); }
void begin_def() {
  save_levels();
  ++current_level;
  nongen_level = current_level;
}
void begin_class_def() {
  save_levels();
  ++current_level;
}
void raise_nongen_level() {
  save_levels();
  nongen_level = current_level;
}
void end_def() {
  if (g_saved_levels.empty()) throw std::logic_error("Ctype.end_def");
  auto [cl, nl] = g_saved_levels.back();
  g_saved_levels.pop_back();
  current_level = cl;
  nongen_level = nl;
}
long create_scope() {
  long level = current_level + 1;
  init_def(level);
  return level;
}

// with_local_level_gen handles both the scoping structure of levels and
// automatic generalization through pools (cf. btype.ml)
void with_local_level_gen_(bool class_def, bool structure, const std::function<void()>& f,
                           const std::function<void()>& before_generalize) {
  if (class_def) begin_class_def();
  else begin_def();
  long level = current_level;
  std::vector<TypeExpr*> pool = with_new_pool(current_level, [&] {
    // let result = wrap_end_def f in Option.iter (fun g -> g result) before_generalize
    struct E {
      bool done = false;
      ~E() {
        if (!done) end_def();
      }
    } e;
    f();
    e.done = true;
    end_def();
    if (before_generalize) before_generalize();
  });
  simple_abbrevs()->contents = mnil();
  for (TypeExpr* ty : pool) {
    // Already generic nodes are not tracked
    if (ty->level == generic_level) continue;
    const TypeDesc* d = ty->desc;
    if (d->kind == DescKind::Tvar && structure) {
      if (ty->level >= level) transient_expr::set_level(ty, current_level);
      add_to_pool(ty->level, ty);
    } else if (d->kind == DescKind::Tlink) {
      // If a node is no longer used as representative, no need to track it
    } else if (ty->level < level) {
      add_to_pool(ty->level, ty);
    } else {
      transient_expr::set_level(ty, generic_level);
      if (structure)
        if (auto* c = as<Tconstr>(ty->desc)) c->memo->contents = mnil();
    }
  }
}

void reset_global_level() { global_level = current_level; }
long increase_global_level() {
  long gl = global_level;
  global_level = current_level;
  return gl;
}
void restore_global_level(long gl) { global_level = gl; }

void reset() {
  current_level = 0;
  nongen_level = 0;
  global_level = 0;
  g_saved_levels.clear();
}

// ---- type creators ---------------------------------------------------------------
TypeExpr* newty(const TypeDesc* desc) { return newty2(current_level, desc); }
TypeExpr* new_scoped_ty(long scope, const TypeDesc* desc) {
  return newty3(current_level, scope, desc);
}
TypeExpr* newvar(OptStr name) { return newty2(current_level, tvar(name)); }
TypeExpr* newvar2(long level, OptStr name) { return newty2(level, tvar(name)); }
TypeExpr* new_global_var(OptStr name) { return newty2(global_level, tvar(name)); }
TypeExpr* newstub(long scope) { return newty3(current_level, scope, TVAR_NONE_LIT()); }
TypeExpr* newobj(TypeExpr* fields) { return newty(tobject(fields, make<NameRef>(nullptr))); }
TypeExpr* newconstr(Path::t path, Slice<TypeExpr*> tyl) {
  return newty(tconstr(path, tyl, make<MemoRef>(mnil())));
}
TypeExpr* newmono(TypeExpr* ty) { return newty(tpoly(ty, {})); }
TypeExpr* newmono_package(const Package* pty, std::optional<long> level) {
  long lv = level ? *level : current_level;
  // newty2 ~level (Tpoly (newty2 ~level (Tpackage pty), []))
  TypeExpr* inner = newty2(lv, tpackage(pty));
  return newty2(lv, tpoly(inner, {}));
}
// `let none = newty (Ttuple [])`: a module-initialization value (clearly
// ill-formed type), created once
TypeExpr* none() {
  static TypeExpr* n = [] {
    ZoneScope perm(permanent_zone());
    return newty(ttuple({}));
  }();
  return n;
}

// ---- Pattern_env ------------------------------------------------------------------
PatternEnv* PatternEnv::make_(env::t env, long equations_scope, bool in_counterexample) {
  auto* p = new PatternEnv{env, {}, equations_scope, in_counterexample};
  static std::vector<std::unique_ptr<PatternEnv>> keep;  // mutable records live on
  keep.emplace_back(p);
  return p;
}
PatternEnv* PatternEnv::copy(std::optional<long> es) const {
  PatternEnv* p = make_(env, es ? *es : equations_scope, in_counterexample);
  p->op_list = op_list;
  return p;
}
Ident::t PatternEnv::enter_type(long scope, std::string_view lbl, const TypeDeclaration* decl) {
  auto [id, new_env] = env::enter_type(static_cast<int>(scope), lbl, decl, env);
  env = new_env;
  op_list.insert(op_list.begin(), EnvOp{true, id, nullptr, decl});
  return id;
}
void PatternEnv::add_local_constraint(Path::t source, const TypeDeclaration* dest) {
  env = env::add_local_constraint(source, dest, env);
  op_list.insert(op_list.begin(), EnvOp{false, nullptr, source, dest});
}
void PatternEnv::do_op(const EnvOp& op) {
  if (op.is_enter_type) {
    env = env::reenter_type(op.id, op.decl, env);
    op_list.insert(op_list.begin(), op);
  } else {
    add_local_constraint(op.source, op.decl);
  }
}
void PatternEnv::with_mty(Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> id_pairs,
                          ident::Unscoped* id, const ModuleType* mty,
                          const std::function<void()>& f) {
  std::vector<EnvOp> old_ope_list = op_list;
  op_list.clear();
  env::t last_env = env;
  struct Clean {
    PatternEnv& p;
    env::t last_env;
    std::vector<EnvOp> old;
    ~Clean() {
      p.env = last_env;
      std::vector<EnvOp> ops = p.op_list;
      p.op_list = old;
      // List.fold_right (fun op () -> do_op penv op) ops (): last first
      for (auto it = ops.rbegin(); it != ops.rend(); ++it) p.do_op(*it);
    }
  } clean{*this, last_env, old_ope_list};
  env::t e = env::add_module(Ident::of_unscoped(id), ModulePresence::Mp_present, mty, last_env);
  e = env::with_pairs(id_pairs, e);
  env = e;
  f();
}

// [quick_eq_type_path] is used in fast-paths that check if two type
// paths are "clearly the same", it can under-approximate path
// equivalence to gain speed.  If [normalize] is [true], we also check
// quick-equivalence modulo normalization.
bool quick_eq_type_path(bool normalize, env::t env, Path::t p1, Path::t p2) {
  if (normalize) return env::type_path_equiv_modulo(env, p1, p2);
  return env::path_equiv(env, p1, p2);
}

// Check that [p1] and [p2] are equivalent, assuming that [p1] and [p2]
// have been normalized.
bool eq_expanded_type_path(env::t env, Path::t p1, Path::t p2) { return env::path_equiv(env, p1, p2); }

bool eq_package_path(env::t env, Path::t p1, Path::t p2) {
  return env::path_equiv(env, p1, p2) || env::modtype_path_equiv_modulo(env, p1, p2);
}

// ---- unification mode --------------------------------------------------------------
env::t get_env(const Uenv& u) { return u.is_pattern ? u.penv->env : u.expr_env; }
bool in_pattern_mode(const Uenv& u) { return u.is_pattern; }

// ---- checks for type definitions ----------------------------------------------------
bool is_datatype(const TypeDeclaration* decl) {
  return decl->type_kind->kind != TypeKind::Kind::Type_abstract;
}

// ---- object field manipulation -----------------------------------------------------
TypeExpr* object_fields(TypeExpr* ty) {
  auto* o = as<Tobject>(get_desc(ty));
  if (!o) throw std::logic_error("Ctype.object_fields");
  return o->fields;
}

std::pair<std::vector<FieldEntry>, TypeExpr*> flatten_fields(TypeExpr* ty) {
  std::vector<FieldEntry> l;
  for (;;) {
    auto* f = as<Tfield>(get_desc(ty));
    if (!f) break;
    l.insert(l.begin(), FieldEntry{f->label, f->kind_, f->ty});
    ty = f->rest;
  }
  // List.sort is a stable merge sort
  std::stable_sort(l.begin(), l.end(),
                   [](const FieldEntry& a, const FieldEntry& b) { return a.name < b.name; });
  return {l, ty};
}

TypeExpr* build_fields(long level, const std::vector<FieldEntry>& fields, TypeExpr* rest) {
  // List.fold_right: the last field is built first
  TypeExpr* ty2 = rest;
  for (auto it = fields.rbegin(); it != fields.rend(); ++it)
    ty2 = newty2(level, tfield(it->name, it->kind, it->ty, ty2));
  return ty2;
}

AssociatedFields associate_fields(const std::vector<FieldEntry>& fields1,
                                  const std::vector<FieldEntry>& fields2) {
  AssociatedFields r;
  std::size_t i = 0, j = 0;
  while (i < fields1.size() && j < fields2.size()) {
    const auto& a = fields1[i];
    const auto& b = fields2[j];
    if (a.name == b.name) {
      r.pairs.push_back({a.name, a.kind, a.ty, b.kind, b.ty});
      ++i;
      ++j;
    } else if (a.name < b.name) {
      r.miss1.push_back(a);
      ++i;
    } else {
      r.miss2.push_back(b);
      ++j;
    }
  }
  for (; i < fields1.size(); ++i) r.miss1.push_back(fields1[i]);
  for (; j < fields2.size(); ++j) r.miss2.push_back(fields2[j]);
  return r;
}

TypeExpr* object_row(TypeExpr* ty) {
  for (;;) {
    const TypeDesc* d = get_desc(ty);
    if (auto* o = as<Tobject>(d)) ty = o->fields;
    else if (auto* f = as<Tfield>(d)) ty = f->rest;
    else return ty;
  }
}

bool opened_object(TypeExpr* ty) {
  switch (get_desc(object_row(ty))->kind) {
    case DescKind::Tvar:
    case DescKind::Tunivar:
    case DescKind::Tconstr: return true;
    default: return false;
  }
}

bool concrete_object(TypeExpr* ty) {
  return get_desc(object_row(ty))->kind != DescKind::Tvar;
}

TypeExpr* fields_row_variable(TypeExpr* ty) {
  for (;;) {
    const TypeDesc* d = get_desc(ty);
    if (auto* f = as<Tfield>(d)) ty = f->rest;
    else if (d->kind == DescKind::Tvar) return ty;
    else throw std::logic_error("Ctype.fields_row_variable");
  }
}

void set_object_name(Path::t p, Slice<TypeExpr*> params, TypeExpr* ty) {
  const TypeDesc* d = get_desc(ty);
  if (auto* o = as<Tobject>(d)) {
    TypeExpr* rv = fields_row_variable(o->fields);
    std::vector<TypeExpr*> args{rv};
    args.insert(args.end(), params.begin(), params.end());
    set_name(o->name, make<PathArgs>(p, slice(args), params));  // Some (p, rv :: params)
  } else if (d->kind != DescKind::Tconstr) {
    throw std::logic_error("Ctype.set_object_name");
  }
}

void remove_object_name(TypeExpr* ty) {
  const TypeDesc* d = get_desc(ty);
  if (auto* o = as<Tobject>(d)) set_name(o->name, nullptr);
  else if (d->kind != DescKind::Tconstr) throw std::logic_error("Ctype.remove_object_name");
}

// ---- row types ---------------------------------------------------------------------------
std::vector<RowFieldEntry> sort_row_fields(std::vector<RowFieldEntry> l) {
  std::stable_sort(l.begin(), l.end(),
                   [](const RowFieldEntry& a, const RowFieldEntry& b) { return a.label < b.label; });
  return l;
}

static bool mem_assoc(std::string_view l, Slice<RowFieldEntry> fi) {
  return std::any_of(fi.begin(), fi.end(), [&](const RowFieldEntry& e) { return e.label == l; });
}

MergedRowFields merge_row_fields(Slice<RowFieldEntry> fi1, Slice<RowFieldEntry> fi2, Zone* in) {
  MergedRowFields r;
  if (fi1.empty() || fi2.empty() ||
      (fi1.size() == 1 && !mem_assoc(fi1[0].label, fi2)) ||
      (fi2.size() == 1 && !mem_assoc(fi2[0].label, fi1))) {
    r.r1 = fi1;
    r.r2 = fi2;
    return r;
  }
  // merge_rf [] [] [] (sort fi1) (sort fi2): pairs are consed (reversed)
  auto s1 = sort_row_fields(std::vector<RowFieldEntry>(fi1.begin(), fi1.end()));
  auto s2 = sort_row_fields(std::vector<RowFieldEntry>(fi2.begin(), fi2.end()));
  std::vector<RowFieldEntry> r1, r2;
  std::size_t i = 0, j = 0;
  while (i < s1.size() && j < s2.size()) {
    if (s1[i].label == s2[j].label) {
      r.pairs.insert(r.pairs.begin(), {s1[i].label, s1[i].field, s2[j].field});
      ++i;
      ++j;
    } else if (s1[i].label < s2[j].label) {
      r1.push_back(s1[i++]);
    } else {
      r2.push_back(s2[j++]);
    }
  }
  for (; i < s1.size(); ++i) r1.push_back(s1[i]);
  for (; j < s2.size(); ++j) r2.push_back(s2[j]);
  r.r1 = slice_in(in ? *in : zone(), r1);
  r.r2 = slice_in(in ? *in : zone(), r2);
  return r;
}

std::vector<RowFieldEntry> filter_row_fields(bool erase, Slice<RowFieldEntry> fi) {
  // recursion processes the tail first
  std::vector<RowFieldEntry> out;
  for (auto it = std::make_reverse_iterator(fi.end()); it != std::make_reverse_iterator(fi.begin()); ++it) {
    auto v = row_field_repr(it->field);
    if (v.kind == RowFieldView::Kind::Rabsent) continue;
    if (v.kind == RowFieldView::Kind::Reither && !v.matched && erase) {
      link_row_field_ext(it->field, rf_absent());
      continue;
    }
    out.insert(out.begin(), *it);
  }
  return out;
}

// ---- check genericity of type schemes -------------------------------------------------
// free_vars ~init ~add_one ?env mark ty: `add_one ty kind` is the callback
static void free_vars(const std::function<void(TypeExpr*, VariableKind)>& add_one, env::t env,
                      TypeMark& mark, TypeExpr* ty0) {
  std::function<void(VariableKind, TypeExpr*)> fv = [&](VariableKind kind, TypeExpr* ty) {
    if (!try_mark_node(mark, ty)) return;
    const TypeDesc* d = get_desc(ty);
    switch (d->kind) {
      case DescKind::Tvar:
        add_one(ty, kind);
        return;
      case DescKind::Tconstr:
        if (env) {
          auto* c = as<Tconstr>(d);
          try {
            env::TypeExpansion x = env::find_type_expansion(c->path, env);
            if (get_level(x.body) != generic_level) add_one(ty, kind);
          } catch (const env::NotFound&) {
          }
          for (TypeExpr* t : c->args) fv(VariableKind::Type_variable, t);
          return;
        }
        break;
      case DescKind::Tobject:
        // ignoring the second parameter of [Tobject] amounts to not counting
        // "virtual free variables".
        fv(VariableKind::Row_variable, as<Tobject>(d)->fields);
        return;
      case DescKind::Tfield: {
        auto* f = as<Tfield>(d);
        fv(VariableKind::Type_variable, f->ty);
        fv(VariableKind::Row_variable, f->rest);
        return;
      }
      case DescKind::Tvariant: {
        const RowDesc* row = as<Tvariant>(d)->row;
        iter_row([&](TypeExpr* t) { fv(VariableKind::Type_variable, t); }, row);
        if (!static_row(row)) fv(VariableKind::Row_variable, row_more(row));
        return;
      }
      default:
        break;
    }
    iter_type_expr([&](TypeExpr* t) { fv(kind, t); }, ty);
  };
  fv(VariableKind::Type_variable, ty0);
}

std::vector<TypeExpr*> free_variables(TypeExpr* ty, env::t env) {
  std::vector<TypeExpr*> acc;  // ty :: acc
  with_type_mark([&](TypeMark& mark) {
    free_vars([&](TypeExpr* t, VariableKind) { acc.insert(acc.begin(), t); }, env, mark, ty);
  });
  return acc;
}

std::vector<TypeExpr*> free_variables_list(const std::vector<TypeExpr*>& tyl, env::t env) {
  std::vector<TypeExpr*> acc;
  with_type_mark([&](TypeMark& mark) {
    for (TypeExpr* ty : tyl)
      free_vars([&](TypeExpr* t, VariableKind) { acc.insert(acc.begin(), t); }, env, mark, ty);
  });
  return acc;
}

bool contains_nongen_variables(TypeExpr* ty, env::t env) {
  bool acc = false;
  with_type_mark([&](TypeMark& mark) {
    free_vars([&](TypeExpr* t, VariableKind) { acc = acc || get_level(t) < generic_level; },
              env, mark, ty);
  });
  return acc;
}

static void closed_type(env::t env, TypeMark& mark, TypeExpr* ty) {
  free_vars([](TypeExpr* t, VariableKind k) { throw NonClosed{t, k}; }, env, mark, ty);
}

bool closed_type_expr(TypeExpr* ty, env::t env) {
  bool r = true;
  with_type_mark([&](TypeMark& mark) {
    try {
      closed_type(env, mark, ty);
    } catch (const NonClosed&) {
      r = false;
    }
  });
  return r;
}

TypeExpr* closed_type_decl(const TypeDeclaration* decl) {
  TypeExpr* r = nullptr;
  with_type_mark([&](TypeMark& mark) {
    try {
      for (TypeExpr* p : decl->type_params) mark_type(mark, p);
      const TypeKind* k = decl->type_kind;
      if (k->kind == TypeKind::Kind::Type_variant) {
        for (auto* cd : k->constructors) {
          if (cd->cd_res) continue;
          if (cd->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple)
            for (TypeExpr* t : cd->cd_args.tuple) closed_type(nullptr, mark, t);
          else
            for (auto* l : cd->cd_args.record) closed_type(nullptr, mark, l->ld_type);
        }
      } else if (k->kind == TypeKind::Kind::Type_record) {
        for (auto* l : k->labels) closed_type(nullptr, mark, l->ld_type);
      }
      if (decl->type_manifest) closed_type(nullptr, mark, decl->type_manifest);
    } catch (const NonClosed& e) {
      r = e.ty;
    }
  });
  return r;
}

std::optional<ClosedClassFailure> closed_class(Slice<TypeExpr*> params,
                                               const ClassSignature* sign) {
  std::optional<ClosedClassFailure> r;
  with_type_mark([&](TypeMark& mark) {
    for (TypeExpr* p : params) mark_type(mark, p);
    try_mark_node(mark, sign->csig_self_row);
    struct CCFailure {
      ClosedClassFailure f;
    };
    try {
      sign->csig_meths.iter([&](std::string_view lab, const MethEntry& e) {
        if (e.priv.is_private) return;
        try {
          closed_type(nullptr, mark, e.ty);
        } catch (const NonClosed& nc) {
          throw CCFailure{{nc.ty, nc.kind, lab, e.ty}};
        }
      });
    } catch (const CCFailure& f) {
      r = f.f;
    }
  });
  return r;
}

// ---- type duplication -------------------------------------------------------------------
TypeExpr* duplicate_type(TypeExpr* ty) { return subst::type_expr(subst::identity(), ty); }
const ClassType* duplicate_class_type(const ClassType* cty) {
  return subst::class_type(subst::identity(), cty);
}

// ---- type level manipulation ------------------------------------------------------------
std::function<TypeExpr*(env::t, TypeExpr*)> forward_try_expand_safe;

std::function<const ModuleType*(env::t, const Location&, const Package*)> modtype_of_package_ref;
void set_modtype_of_package(std::function<const ModuleType*(env::t, const Location&, const Package*)> f) {
  modtype_of_package_ref = std::move(f);
}
const ModuleType* modtype_of_package(env::t env, const Location& loc, const Package* pack) {
  return modtype_of_package_ref(env, loc, pack);
}

static Path::t normalize_or_raise_escape(env::t env, Path::t p) {
  Path::t p2 = env::try_normalize_modtype_path(env, p);
  if (!p2) raise_escape_module_type(p);
  return p2;
}

static void check_scope_escape_rec(TypeMark& mark, env::t env, long level, TypeExpr* ty) {
  long orig_level = get_level(ty);
  if (!try_mark_node(mark, ty)) return;
  if (level < get_scope(ty)) raise_scope_escape_exn(ty);
  const TypeDesc* d = get_desc(ty);
  if (auto* c = as<Tconstr>(d); c && level < path::scope(c->path)) {
    TypeExpr* ty2;
    try {
      ty2 = forward_try_expand_safe(env, ty);
    } catch (const CannotExpand&) {
      raise_escape_constructor(c->path);
    }
    check_scope_escape_rec(mark, env, level, ty2);
    return;
  }
  if (auto* pk = as<Tpackage>(d); pk && level < path::scope(pk->pack->pack_path)) {
    Path::t p2 = normalize_or_raise_escape(env, pk->pack->pack_path);
    check_scope_escape_rec(
        mark, env, level,
        newty2(orig_level, tpackage(make<Package>(p2, pk->pack->pack_constraints))));
    return;
  }
  if (auto* fu = as<Tfunctor>(d)) {
    if (level < path::scope(fu->pack->pack_path)) {
      Path::t p2 = normalize_or_raise_escape(env, fu->pack->pack_path);
      check_scope_escape_rec(
          mark, env, level,
          newty2(orig_level, tfunctor(fu->label, fu->id,
                                      make<Package>(p2, fu->pack->pack_constraints), fu->body)));
      return;
    }
    for (auto& c : fu->pack->pack_constraints) check_scope_escape_rec(mark, env, level, c.ty);
    const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
    env::t env2 = env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
    check_scope_escape_rec(mark, env2, level, fu->body);
    return;
  }
  iter_type_expr([&](TypeExpr* t) { check_scope_escape_rec(mark, env, level, t); }, ty);
}

void check_scope_escape(env::t env, long level, TypeExpr* ty) {
  with_type_mark([&](TypeMark& mark) {
    try {
      check_scope_escape_rec(mark, env, level, ty);
    } catch (Escape& e) {
      e.esc.context = ty;
      throw;
    }
  });
}

void update_scope(long scope, TypeExpr* ty) {
  if (get_scope(ty) < scope) {
    if (get_level(ty) < scope) raise_scope_escape_exn(ty);
    set_scope(ty, scope);
  }
}

void update_scope_for(TraceExn tr_exn, long scope, TypeExpr* ty) {
  try {
    update_scope(scope, ty);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

static bool needs_expand(env::t env, long level, Path::t path, Slice<TypeExpr*> args) {
  try {
    const TypeDeclaration* decl = env::find_type(path, env);
    // List.exists2 raises Invalid_argument on length mismatch
    if (decl->type_variance.size() != args.size())
      throw std::invalid_argument("List.exists2");
    for (std::size_t k = 0; k < args.size(); ++k)
      if (decl->type_variance[k] == variance::null && get_level(args[k]) > level) return true;
    return false;
  } catch (const env::NotFound&) {
    return false;
  }
}

// Note: the level of a type constructor must be greater than its binding
// time, so that a type constructor cannot escape the scope of its definition.
static bool check_level_type_rec(std::vector<TypeExpr*>& visited, long level, TypeExpr* ty) {
  if (get_level(ty) > level) return false;
  const Abbrev* a = get_abbrev(ty);
  if (!a) return true;
  if (a->level <= level) return true;
  if (path::scope(a->path) > level) return false;
  if (a->args.empty()) return true;
  if (std::find(visited.begin(), visited.end(), ty) != visited.end()) return true;
  visited.push_back(ty);
  bool ok = true;
  for (TypeExpr* t : a->args)
    if (!check_level_type_rec(visited, level, t)) {
      ok = false;
      break;
    }
  visited.pop_back();
  return ok;
}

bool check_level_type(long level, TypeExpr* ty) {
  std::vector<TypeExpr*> visited;
  return check_level_type_rec(visited, level, ty);
}

static void update_level_abbrev(env::t env, long level, bool expand, TypeExpr* ty);

static void update_level_rec(env::t env, long level, bool expand, TypeExpr* ty) {
  long ty_level = get_level(ty);
  if (ty_level <= level) {
    update_level_abbrev(env, level, expand, ty);
    return;
  }
  if (level < get_scope(ty)) raise_scope_escape_exn(ty);
  auto set_level_ = [&]() {
    set_level(ty, level);
    if (ty_level == generic_level) add_to_pool(level, repr(ty));
  };
  auto rec = [&](TypeExpr* t) { update_level_rec(env, level, expand, t); };
  const TypeDesc* d = get_desc(ty);
  // Remove out-of-scope Texpand
  if (auto* c = as<Tconstr>(d)) {
    if (level < path::scope(c->path)) {
      // Try first to replace an abbreviation by its expansion.
      TypeExpr* ty2;
      try {
        ty2 = forward_try_expand_safe(env, ty);
      } catch (const CannotExpand&) {
        raise_escape_constructor(c->path);
      }
      link_type(ty, ty2);
      update_level_rec(env, level, expand, ty);
      return;
    }
    if (!c->args.empty()) {
      bool ne = expand || needs_expand(env, level, c->path, c->args);
      // Do not lower the level of nodes that may be unrelated
      TypeExpr* ty2 = nullptr;
      if (ne) {
        try {
          ty2 = forward_try_expand_safe(env, ty);
        } catch (const CannotExpand&) {
        }
      }
      if (ty2) {
        link_type(ty, ty2);
        update_level_rec(env, level, expand, ty);
      } else {
        set_level_();
        iter_type_expr(rec, ty);
        update_level_abbrev(env, level, expand, ty);
      }
      return;
    }
  }
  if (auto* pk = as<Tpackage>(d); pk && level < path::scope(pk->pack->pack_path)) {
    Path::t pp = normalize_or_raise_escape(env, pk->pack->pack_path);
    set_type_desc(ty, tpackage(make<Package>(pp, pk->pack->pack_constraints)));
    update_level_rec(env, level, expand, ty);
    return;
  }
  if (auto* o = as<Tobject>(d); o && o->name->contents &&
                                level < path::scope(o->name->contents->path)) {
    set_name(o->name, nullptr);
    update_level_rec(env, level, expand, ty);
    return;
  }
  if (auto* v = as<Tvariant>(d)) {
    const PathArgs* nm = row_name(v->row);
    if (nm && level < path::scope(nm->path)) set_type_desc(ty, tvariant(set_row_name(v->row, nullptr)));
    set_level_();
    iter_type_expr(rec, ty);
    update_level_abbrev(env, level, expand, ty);
    return;
  }
  if (auto* fu = as<Tfunctor>(d)) {
    if (level < path::scope(fu->pack->pack_path)) {
      Path::t pp = normalize_or_raise_escape(env, fu->pack->pack_path);
      set_type_desc(ty, tfunctor(fu->label, fu->id, make<Package>(pp, fu->pack->pack_constraints),
                                 fu->body));
      update_level_rec(env, level, expand, ty);
      return;
    }
    for (auto& c : fu->pack->pack_constraints) rec(c.ty);
    const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
    env::t env2 = env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
    set_level_();
    update_level_rec(env2, level, expand, fu->body);
    return;
  }
  if (auto* f = as<Tfield>(d); f && f->label == dummy_method && level < get_scope(f->ty))
    raise_escape_self();
  set_level_();
  // XXX what about abbreviations in Tconstr ?
  iter_type_expr(rec, ty);
  update_level_abbrev(env, level, expand, ty);
}

static void update_level_abbrev(env::t env, long level, bool expand, TypeExpr* ty) {
  iter_abbrev(
      [&](Abbrev* abbr) {
        const Path::t& p = abbr->path;
        Slice<TypeExpr*> args = abbr->args;
        if (level >= abbr->level) return;
        if (level < path::scope(p)) {
          forget_abbrev(ty);
          return;
        }
        bool all = true;
        for (TypeExpr* a : args)
          if (!check_level_type(level, a)) {
            all = false;
            break;
          }
        if (all) {
          set_abbrev_level(abbr, level);
          return;
        }
        if (expand || needs_expand(env, level, p, args)) {
          forget_abbrev(ty);
          return;
        }
        set_abbrev_level(abbr, level);
        for (TypeExpr* a : args) update_level_rec(env, level, expand, a);
      },
      ty);
}

// First try without expanding, then expand everything, to avoid
// combinatorial blow-up
void update_level(env::t env, long level, TypeExpr* ty) {
  if (check_level_type(level, ty)) return;
  Snapshot snap = btype::snapshot();
  try {
    update_level_rec(env, level, false, ty);
  } catch (const Escape&) {
    btype::backtrack(snap);
    update_level_rec(env, level, true, ty);
  }
}

void update_level_for(TraceExn tr_exn, env::t env, long level, TypeExpr* ty) {
  try {
    update_level(env, level, ty);
  } catch (const Escape& e) {
    raise_for(tr_exn, escape_elt(e.esc));
  }
}

// Lower the level of a type to the current level
void enforce_current_level(env::t env, TypeExpr* ty) {
  try {
    update_level(env, current_level, ty);
  } catch (const Escape&) {
    misc::fatal_error("Ctype.enforce_current_level");
  }
}

// Lower level of type variables inside contravariant branches (see the long
// comment in ctype.ml).
static void lower_contravariant_rec(env::t env, long var_level,
                                    std::unordered_map<long, bool>& visited, bool contra,
                                    TypeExpr* ty) {
  bool must_visit = false;
  if (get_level(ty) > var_level) {
    auto it = visited.find(get_id(ty));
    must_visit = it == visited.end() ? true : (contra && !it->second);
  }
  if (!must_visit) return;
  auto visit = [&]() { visited[get_id(ty)] = contra; };  // Hashtbl.add shadows
  if (!get_abbrev(ty)) visit();
  auto lower_rec = [&](bool c, TypeExpr* t) { lower_contravariant_rec(env, var_level, visited, c, t); };
  const TypeDesc* d = get_constr_desc(ty);
  switch (d->kind) {
    case DescKind::Tvar:
      if (contra) set_level(ty, var_level);
      return;
    case DescKind::Tconstr: {
      auto* c = as<Tconstr>(d);
      if (c->args.empty()) return;
      std::vector<variance::t> var;
      bool maybe_expand;
      try {
        const TypeDeclaration* typ = env::find_type(c->path, env);
        var.assign(typ->type_variance.begin(), typ->type_variance.end());
        maybe_expand = type_kind_is_abstract(typ);
      } catch (const env::NotFound&) {
        // See testsuite/tests/typing-missing-cmi-2 for an example
        var.assign(c->args.size(), variance::unknown);
        maybe_expand = false;
      }
      if (std::all_of(var.begin(), var.end(), [](variance::t v) { return v == variance::null; }))
        return;
      auto not_expanded = [&]() {
        visit();
        if (var.size() != c->args.size()) throw std::invalid_argument("List.iter2");
        for (std::size_t k = 0; k < var.size(); ++k) {
          if (var[k] == variance::null) continue;
          if (variance::mem(variance::F::May_weak, var[k])) lower_rec(true, c->args[k]);
          else lower_rec(contra, c->args[k]);
        }
      };
      if (maybe_expand) {  // we expand cautiously to avoid missing cmis
        if (get_abbrev(ty)) {
          lower_rec(contra, ignore_abbrev(ty));
        } else {
          TypeExpr* ty2 = nullptr;
          try {
            ty2 = forward_try_expand_safe(env, ty);
          } catch (const CannotExpand&) {
          }
          if (ty2) {
            visit();
            lower_rec(contra, ty2);
          } else {
            not_expanded();
          }
        }
      } else {
        not_expanded();
      }
      return;
    }
    case DescKind::Tpackage:
      for (auto& c : as<Tpackage>(d)->pack->pack_constraints) lower_rec(true, c.ty);
      return;
    case DescKind::Tfunctor: {
      auto* fu = as<Tfunctor>(d);
      for (auto& c : fu->pack->pack_constraints) lower_rec(true, c.ty);
      const ModuleType* mty = modtype_of_package(env, location::none(), fu->pack);
      env::t env2 = env::add_module(Ident::of_unscoped(fu->id), ModulePresence::Mp_present, mty, env);
      lower_contravariant_rec(env2, var_level, visited, contra, fu->body);
      return;
    }
    case DescKind::Tarrow: {
      auto* a = as<Tarrow>(d);
      lower_rec(true, a->t1);
      lower_rec(contra, a->t2);
      return;
    }
    default:
      iter_type_expr([&](TypeExpr* t) { lower_rec(contra, t); }, ty);
  }
}

void lower_variables_only(env::t env, long level, TypeExpr* ty) {
  simple_abbrevs()->contents = mnil();
  std::unordered_map<long, bool> visited;
  lower_contravariant_rec(env, level, visited, true, ty);
}

void lower_contravariant(env::t env, TypeExpr* ty) {
  simple_abbrevs()->contents = mnil();
  std::unordered_map<long, bool> visited;
  lower_contravariant_rec(env, nongen_level, visited, false, ty);
}

void generalize_class_type(const std::function<void(TypeExpr*)>& gen, const ClassType* cty) {
  for (;;) {
    switch (cty->kind) {
      case ClassType::Kind::Cty_constr:
        for (TypeExpr* p : cty->args) gen(p);
        cty = cty->cty;
        continue;
      case ClassType::Kind::Cty_signature: {
        ClassSignature* csig = cty->sign;
        gen(csig->csig_self);
        gen(csig->csig_self_row);
        csig->csig_vars.iter([&](std::string_view, const VarEntry& e) { gen(e.ty); });
        csig->csig_meths.iter([&](std::string_view, const MethEntry& e) { gen(e.ty); });
        return;
      }
      case ClassType::Kind::Cty_arrow:
        gen(cty->arg);
        cty = cty->cty;
        continue;
    }
  }
}

// Only generalize the type ty0 in ty
void limited_generalize(TypeExpr* ty0, TypeExpr* ty) {
  TypeHash<std::vector<TypeExpr*>> graph;  // ty -> parents
  std::vector<TypeExpr*> roots;            // consed
  std::function<void(std::vector<TypeExpr*>, TypeExpr*)> inverse =
      [&](std::vector<TypeExpr*> pty, TypeExpr* t) {
        if (auto* parents = graph.find_opt(t)) {
          // parents := pty @ !parents
          parents->insert(parents->begin(), pty.begin(), pty.end());
          return;
        }
        long level = get_level(t);
        if (level > current_level) {
          graph.add(t, pty);
          // XXX: why generic_level needs to be a root
          if (level == generic_level || eq_type(t, ty0)) roots.insert(roots.begin(), t);
          iter_type_expr([&](TypeExpr* c) { inverse({t}, c); }, t);
        }
      };
  std::function<void(bool, TypeExpr*)> generalize_parents = [&](bool is_root, TypeExpr* t) {
    if (!(is_root || get_level(t) != generic_level)) return;
    set_level(t, generic_level);
    std::vector<TypeExpr*> parents = *graph.find_opt(t);
    for (TypeExpr* p : parents) generalize_parents(false, p);
    // Special case for rows: must generalize the row variable
    if (auto* v = as<Tvariant>(get_desc(t))) {
      TypeExpr* more = row_more(v->row);
      long lv = get_level(more);
      if ((graph.mem(more) || lv > current_level) && lv != generic_level)
        set_level(more, generic_level);
    }
  };
  inverse({}, ty);
  for (TypeExpr* r : roots) generalize_parents(true, r);
  for (auto& [t, _] : graph.h)
    if (get_level(t) != generic_level) set_level(t, current_level);
}

void limited_generalize_class_type(TypeExpr* rv, const ClassType* cty) {
  generalize_class_type([&](TypeExpr* inside) { limited_generalize(rv, inside); }, cty);
}

// Compute statically the free univars of all nodes in a type
struct InvTypeExpr {
  TypeExpr* inv_type;
  std::vector<InvTypeExpr*> inv_parents;
};

static void inv_type(TypeHash<InvTypeExpr*>& hash, std::vector<InvTypeExpr*> pty, TypeExpr* ty) {
  if (auto* inv = hash.find_opt(ty)) {
    (*inv)->inv_parents.insert((*inv)->inv_parents.begin(), pty.begin(), pty.end());
    return;
  }
  auto* inv = make<InvTypeExpr>(ty, pty);
  hash.add(ty, inv);
  iter_abbrev(
      [&](Abbrev* abbr) {
        for (TypeExpr* t : abbr->args) inv_type(hash, {inv}, t);
      },
      ty);
  iter_type_expr([&](TypeExpr* t) { inv_type(hash, {inv}, t); }, ty);
}

std::function<TypeSet(TypeExpr*)> compute_univars(TypeExpr* ty) {
  TypeHash<InvTypeExpr*> inverted;
  inv_type(inverted, {}, ty);
  auto node_univars = std::make_shared<TypeHash<TypeSet>>();
  std::function<void(TypeExpr*, InvTypeExpr*)> add_univar = [&](TypeExpr* univ, InvTypeExpr* inv) {
    if (auto* p = as<Tpoly>(get_desc(inv->inv_type))) {
      long uid = get_id(univ);
      for (TypeExpr* t : p->vars)
        if (get_id(t) == uid) return;
    }
    if (TypeSet* univs = node_univars->find_opt(inv->inv_type)) {
      if (!univs->mem(univ)) {
        univs->add(univ);
        for (InvTypeExpr* pa : inv->inv_parents) add_univar(univ, pa);
      }
    } else {
      TypeSet s;
      s.add(univ);
      node_univars->add(inv->inv_type, s);
      for (InvTypeExpr* pa : inv->inv_parents) add_univar(univ, pa);
    }
  };
  for (auto& [t, inv] : inverted.h)
    if (is_Tunivar(t)) add_univar(t, inv);
  return [node_univars](TypeExpr* t) {
    if (TypeSet* s = node_univars->find_opt(t)) return *s;
    return TypeSet{};
  };
}

TypeSet type_subexpressions_with_free_occurrences(const std::vector<Ident::t>& ids0,
                                                  TypeExpr* ty) {
  TypeHash<InvTypeExpr*> inverted;
  inv_type(inverted, {}, ty);
  TypeSet nodes;
  std::function<void(const std::vector<Ident::t>&, InvTypeExpr*)> add_all_parents =
      [&](const std::vector<Ident::t>& ids, InvTypeExpr* inv) {
        if (nodes.mem(inv->inv_type)) return;
        if (auto* fu = as<Tfunctor>(get_desc(inv->inv_type))) {
          Ident::t id2 = Ident::of_unscoped(fu->id);
          std::vector<Ident::t> ids2;
          for (Ident::t id : ids)
            if (!ident::same(id, id2)) ids2.push_back(id);
          if (!ids2.empty()) {
            nodes.add(inv->inv_type);
            for (InvTypeExpr* pa : inv->inv_parents) add_all_parents(ids2, pa);
          }
          return;
        }
        nodes.add(inv->inv_type);
        for (InvTypeExpr* pa : inv->inv_parents) add_all_parents(ids, pa);
      };
  for (auto& [t, inv] : inverted.h) {
    const TypeDesc* d = get_desc(t);
    Path::t p = nullptr;
    if (auto* c = as<Tconstr>(d)) p = c->path;
    else if (auto* o = as<Tobject>(d); o && o->name->contents) p = o->name->contents->path;
    else if (auto* fu = as<Tfunctor>(d)) p = fu->pack->pack_path;
    else if (auto* pk = as<Tpackage>(d)) p = pk->pack->pack_path;
    else if (auto* v = as<Tvariant>(d)) {
      if (const PathArgs* nm = row_name(v->row)) p = nm->path;
    }
    if (p && path::exists_free(ids0, p)) {
      add_all_parents(ids0, inv);
      continue;
    }
    iter_abbrev(
        [&](Abbrev* abbr) {
          if (path::exists_free(ids0, abbr->path)) add_all_parents(ids0, inv);
        },
        inv->inv_type);
  }
  return nodes;
}

bool fully_generic(TypeExpr* ty) {
  bool r = true;
  with_type_mark([&](TypeMark& mark) {
    struct ExitEx {};
    std::function<void(TypeExpr*)> aux = [&](TypeExpr* t) {
      if (!try_mark_node(mark, t)) return;
      if (get_level(t) == generic_level) iter_type_expr(aux, t);
      else throw ExitEx{};
    };
    try {
      aux(ty);
    } catch (const ExitEx&) {
      r = false;
    }
  });
  return r;
}

}  // namespace cppcaml::typing::ctype
