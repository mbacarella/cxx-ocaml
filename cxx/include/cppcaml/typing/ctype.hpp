// Port of typing/ctype.ml (TYPECHECKER.md): operations on core types --
// levels and generalization, instantiation, expansion, unification,
// moregen / equality, subtyping, class signatures, nondep.
//
// The implementation is split by area (ctype.cpp, ctype_expand.cpp,
// ctype_unify.cpp, ...), following ctype.ml's order.
#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/errortrace.hpp"

namespace cppcaml::typing::ctype {

namespace et = errortrace;

// ---- exceptions ---------------------------------------------------------------------
// Trace exceptions (internal) and error exceptions (exported), as ctype.ml.
struct UnifyTrace { et::TypeTrace trace; };
struct EqualityTrace { et::TypeTrace trace; };
struct MoregenTrace { et::TypeTrace trace; };

struct Unify : std::runtime_error {
  et::UnificationError err;
  explicit Unify(et::UnificationError e) : std::runtime_error("Ctype.Unify"), err(std::move(e)) {}
};
struct Equality : std::runtime_error {
  et::EqualityError err;
  explicit Equality(et::EqualityError e)
      : std::runtime_error("Ctype.Equality"), err(std::move(e)) {}
};
struct Moregen : std::runtime_error {
  et::MoregenError err;
  explicit Moregen(et::MoregenError e) : std::runtime_error("Ctype.Moregen"), err(std::move(e)) {}
};
struct Subtype : std::runtime_error {
  et::subtype::Error err;
  explicit Subtype(et::subtype::Error e) : std::runtime_error("Ctype.Subtype"), err(std::move(e)) {}
};
struct Escape : std::runtime_error {
  et::Escape<TypeExpr*> esc;
  explicit Escape(et::Escape<TypeExpr*> e) : std::runtime_error("Ctype.Escape"), esc(e) {}
};
struct Tags : std::runtime_error {
  std::string_view l1, l2;
  Tags(std::string_view a, std::string_view b) : std::runtime_error("Ctype.Tags"), l1(a), l2(b) {}
};
struct CannotExpand {};
struct CannotApply {};
struct CannotSubst {};
struct CannotUnifyUniversalVariables {
  et::Order order;
  et::Diff<TypeExpr*> diff;
};
struct OutOfScopeUniversalVariable {};
struct MatchesFailure {
  env::t env;
  et::UnificationError err;
};
struct Incompatible {};

// type _ trace_exn = Unify | Moregen | Equality
enum class TraceExn { Unify, Moregen, Equality };
[[noreturn]] void raise_trace_for(TraceExn tr_exn, et::TypeTrace tr);
[[noreturn]] void raise_unexplained_for(TraceExn tr_exn);
[[noreturn]] void raise_for(TraceExn tr_exn, et::Elt<TypeExpr*> e);

// ---- GADT instance tracing -----------------------------------------------------------
extern bool trace_gadt_instances;
bool check_trace_gadt_instances(env::t env, bool force = false);
void reset_trace_gadt_instances(bool b);
template <class F>
auto wrap_trace_gadt_instances(env::t env, F&& f, bool force = false) -> decltype(f()) {
  bool b = check_trace_gadt_instances(env, force);
  struct R {
    bool b;
    ~R() { reset_trace_gadt_instances(b); }
  } r{b};
  return f();
}

// ---- levels ---------------------------------------------------------------------
extern long current_level, nongen_level, global_level;
long get_current_level();
void init_def(long level);
void begin_def();
void begin_class_def();
void raise_nongen_level();
void end_def();
long create_scope();
void reset();
void reset_global_level();
long increase_global_level();
void restore_global_level(long gl);

// with_local_level_gen ~begin_def ~structure ?before_generalize f
void with_local_level_gen_(bool class_def, bool structure, const std::function<void()>& f,
                           const std::function<void()>& before_generalize = {});

template <class F>
auto with_local_level_generalize(F&& f) -> decltype(f()) {
  std::optional<decltype(f())> r;
  with_local_level_gen_(false, false, [&] { r.emplace(f()); });
  return std::move(*r);
}
template <class F, class G>
auto with_local_level_generalize(F&& f, G&& before_generalize) -> decltype(f()) {
  std::optional<decltype(f())> r;
  with_local_level_gen_(false, false, [&] { r.emplace(f()); }, [&] { before_generalize(*r); });
  return std::move(*r);
}
template <class F>
auto with_local_level_generalize_structure(F&& f) -> decltype(f()) {
  std::optional<decltype(f())> r;
  with_local_level_gen_(false, true, [&] { r.emplace(f()); });
  return std::move(*r);
}
template <class F>
auto with_local_level_generalize_if(bool cond, F&& f) -> decltype(f()) {
  if (cond) return with_local_level_generalize(f);
  return f();
}
template <class F, class G>
auto with_local_level_generalize_if(bool cond, F&& f, G&& before_generalize) -> decltype(f()) {
  if (cond) return with_local_level_generalize(f, before_generalize);
  return f();
}
template <class F>
auto with_local_level_generalize_structure_if(bool cond, F&& f) -> decltype(f()) {
  if (cond) return with_local_level_generalize_structure(f);
  return f();
}
template <class F>
auto with_local_level_generalize_structure_if_principal(F&& f) -> decltype(f()) {
  return with_local_level_generalize_structure_if(clflags::principal, f);
}
template <class F>
auto with_local_level_generalize_for_class(F&& f) -> decltype(f()) {
  std::optional<decltype(f())> r;
  with_local_level_gen_(true, false, [&] { r.emplace(f()); });
  return std::move(*r);
}

// wrap_end_def: run f, end_def even on exceptions
template <class F>
auto wrap_end_def(F&& f) -> decltype(f()) {
  struct E {
    ~E() { end_def(); }
  } e;
  return f();
}

template <class F>
auto with_local_level(F&& f) -> decltype(f()) {
  begin_def();
  return wrap_end_def(f);
}
template <class F, class P>
auto with_local_level(F&& f, P&& post) -> decltype(f()) {
  begin_def();
  auto result = wrap_end_def(f);
  post(result);
  return result;
}
template <class F, class P>
auto with_local_level_if(bool cond, F&& f, P&& post) -> decltype(f()) {
  if (cond) return with_local_level(f, post);
  return f();
}
template <class F, class P>
auto with_local_level_if_principal(F&& f, P&& post) -> decltype(f()) {
  return with_local_level_if(clflags::principal, f, post);
}
// with_local_level_iter f ~post: f returns (result, l); post is iterated on l
template <class F, class P>
auto with_local_level_iter(F&& f, P&& post) -> decltype(f().first) {
  begin_def();
  auto r = wrap_end_def(f);
  for (auto& x : r.second) post(x);
  return std::move(r.first);
}
template <class F, class P>
auto with_local_level_iter_if(bool cond, F&& f, P&& post) -> decltype(f().first) {
  if (cond) return with_local_level_iter(f, post);
  return f().first;
}
template <class F, class P>
auto with_local_level_iter_if_principal(F&& f, P&& post) -> decltype(f().first) {
  return with_local_level_iter_if(clflags::principal, f, post);
}
void save_levels();
template <class F>
auto with_level(long level, F&& f) -> decltype(f()) {
  save_levels();
  init_def(level);
  return wrap_end_def(f);
}
template <class F>
auto with_level_if(bool cond, long level, F&& f) -> decltype(f()) {
  if (cond) return with_level(level, f);
  return f();
}
template <class F>
auto with_local_level_for_class(F&& f) -> decltype(f()) {
  begin_class_def();
  return wrap_end_def(f);
}
template <class F>
auto with_raised_nongen_level(F&& f) -> decltype(f()) {
  raise_nongen_level();
  return wrap_end_def(f);
}

// ---- type creators ---------------------------------------------------------------
TypeExpr* newty(const TypeDesc* desc);
TypeExpr* new_scoped_ty(long scope, const TypeDesc* desc);
TypeExpr* newvar(OptStr name = OptStr::none());
TypeExpr* newvar2(long level, OptStr name = OptStr::none());
TypeExpr* new_global_var(OptStr name = OptStr::none());
TypeExpr* newstub(long scope);
TypeExpr* newobj(TypeExpr* fields);
TypeExpr* newconstr(Path::t path, Slice<TypeExpr*> tyl);
TypeExpr* newmono(TypeExpr* ty);
TypeExpr* newmono_package(const Package* pty, std::optional<long> level = std::nullopt);
TypeExpr* none();

// ---- Pattern_env ----------------------------------------------------------------
struct PatternEnv {
  struct EnvOp {  // Enter_type of Ident.t * type_declaration | Local_constraint of Path.t * ..
    bool is_enter_type;
    Ident::t id = nullptr;
    Path::t source = nullptr;
    const TypeDeclaration* decl = nullptr;
  };
  struct State {
    env::t env;
    std::vector<EnvOp> op_list;
  };
  env::t env;
  std::vector<EnvOp> op_list;  // most recent first (the OCaml list)
  long equations_scope;
  bool in_counterexample;

  static PatternEnv* make_(env::t env, long equations_scope, bool in_counterexample);
  PatternEnv* copy(std::optional<long> equations_scope = std::nullopt) const;
  Ident::t enter_type(long scope, std::string_view lbl, const TypeDeclaration* decl);
  void add_local_constraint(Path::t source, const TypeDeclaration* dest);
  void set_env(env::t e) { env = e; }
  State save() const { return {env, op_list}; }
  void reset(const State& s) {
    env = s.env;
    op_list = s.op_list;
  }
  // with_mty penv id_pairs id mty f
  void with_mty(Slice<std::pair<ident::Unscoped*, ident::Unscoped*>> id_pairs,
                ident::Unscoped* id, const ModuleType* mty, const std::function<void()>& f);

 private:
  void do_op(const EnvOp& op);
};

// ---- unification environment ----------------------------------------------------------
struct Uenv {  // Expression {env; in_subst} | Pattern {penv; equated_types; ...}
  bool is_pattern;
  env::t expr_env = nullptr;
  bool in_subst = false;
  PatternEnv* penv = nullptr;
  btype::TypePairs* equated_types = nullptr;
  bool assume_injective = false;
  btype::TypePairs* unify_eq_set = nullptr;

  static Uenv expression(env::t env, bool in_subst = false) {
    Uenv u{false};
    u.expr_env = env;
    u.in_subst = in_subst;
    return u;
  }
};
env::t get_env(const Uenv& u);
bool in_pattern_mode(const Uenv& u);

// ---- checks for type definitions ----------------------------------------------------
bool in_current_module(Path::t p);
bool in_pervasives(Path::t p);
bool is_datatype(const TypeDeclaration* decl);

// ---- object types ---------------------------------------------------------------------
struct FieldEntry {
  std::string_view name;
  FieldKind* kind;
  TypeExpr* ty;
};
TypeExpr* object_fields(TypeExpr* ty);
std::pair<std::vector<FieldEntry>, TypeExpr*> flatten_fields(TypeExpr* ty);
TypeExpr* build_fields(long level, const std::vector<FieldEntry>& fields, TypeExpr* rest);
struct FieldPair {
  std::string_view name;
  FieldKind* k1;
  TypeExpr* t1;
  FieldKind* k2;
  TypeExpr* t2;
};
struct AssociatedFields {
  std::vector<FieldPair> pairs;
  std::vector<FieldEntry> miss1;
  std::vector<FieldEntry> miss2;
};
AssociatedFields associate_fields(const std::vector<FieldEntry>& f1,
                                  const std::vector<FieldEntry>& f2);
TypeExpr* object_row(TypeExpr* ty);
bool opened_object(TypeExpr* ty);
bool concrete_object(TypeExpr* ty);
TypeExpr* fields_row_variable(TypeExpr* ty);
void set_object_name(Path::t p, Slice<TypeExpr*> params, TypeExpr* ty);
void remove_object_name(TypeExpr* ty);

// ---- row types ------------------------------------------------------------------------
std::vector<RowFieldEntry> sort_row_fields(std::vector<RowFieldEntry> l);
struct MergedRowFields {
  std::vector<RowFieldEntry> r1, r2;
  struct Pair {
    std::string_view label;
    const RowField* f1;
    const RowField* f2;
  };
  std::vector<Pair> pairs;
};
MergedRowFields merge_row_fields(const std::vector<RowFieldEntry>& fi1,
                                 const std::vector<RowFieldEntry>& fi2);
std::vector<RowFieldEntry> filter_row_fields(bool erase, const std::vector<RowFieldEntry>& fi);

// ---- genericity -------------------------------------------------------------------------
enum class VariableKind { Row_variable, Type_variable };
struct NonClosed {
  TypeExpr* ty;
  VariableKind kind;
};
std::vector<TypeExpr*> free_variables(TypeExpr* ty, env::t env = nullptr);
std::vector<TypeExpr*> free_variables_list(const std::vector<TypeExpr*>& tyl, env::t env = nullptr);
bool contains_nongen_variables(TypeExpr* ty, env::t env = nullptr);
bool closed_type_expr(TypeExpr* ty, env::t env = nullptr);
TypeExpr* closed_type_decl(const TypeDeclaration* decl);  // nullptr = None
struct ClosedClassFailure {
  TypeExpr* free_variable;
  VariableKind kind;
  std::string_view meth;
  TypeExpr* meth_ty;
};
std::optional<ClosedClassFailure> closed_class(Slice<TypeExpr*> params, const ClassSignature* sign);

TypeExpr* duplicate_type(TypeExpr* ty);
const ClassType* duplicate_class_type(const ClassType* cty);

// ---- levels of types ------------------------------------------------------------------
extern std::function<TypeExpr*(env::t, TypeExpr*)> forward_try_expand_safe;
// Stub set by Typemod
extern std::function<const ModuleType*(env::t, const Location&, const Package*)> modtype_of_package_ref;
const ModuleType* modtype_of_package(env::t env, const Location& loc, const Package* pack);
void set_modtype_of_package(std::function<const ModuleType*(env::t, const Location&, const Package*)> f);

void check_scope_escape(env::t env, long level, TypeExpr* ty);
void update_scope(long scope, TypeExpr* ty);
void update_scope_for(TraceExn tr_exn, long scope, TypeExpr* ty);
bool check_level_type(long level, TypeExpr* ty);
void update_level(env::t env, long level, TypeExpr* ty);
void update_level_for(TraceExn tr_exn, env::t env, long level, TypeExpr* ty);
void lower_variables_only(env::t env, long level, TypeExpr* ty);
void lower_contravariant(env::t env, TypeExpr* ty);
void generalize_class_type(const std::function<void(TypeExpr*)>& gen, const ClassType* cty);
void limited_generalize(TypeExpr* ty0, TypeExpr* inside);
void limited_generalize_class_type(TypeExpr* rv, const ClassType* inside);
std::function<btype::TypeSet(TypeExpr*)> compute_univars(TypeExpr* ty);
btype::TypeSet type_subexpressions_with_free_occurrences(const std::vector<Ident::t>& ids,
                                                         TypeExpr* ty);
bool fully_generic(TypeExpr* ty);

// ---- instantiation ---------------------------------------------------------------
struct Partial {  // (free_univars, keep)
  std::function<btype::TypeSet(TypeExpr*)> free_univars;
  bool keep;
};
struct UnscopedMapping {
  std::vector<std::pair<Ident::t, Path::t>> map;
  std::function<bool(TypeExpr*)> closed;
};
UnscopedMapping empty_unscoped_mapping();
UnscopedMapping compute_new_closed(ident::Unscoped* us, ident::Unscoped* us2,
                                   const std::vector<std::pair<Ident::t, Path::t>>& id_map,
                                   TypeExpr* ty);
TypeExpr* copy(btype::CopyScope& copy_scope, TypeExpr* ty, const Partial* partial = nullptr,
               bool keep_names = false, std::optional<long> scope = std::nullopt,
               const UnscopedMapping* unscoped = nullptr);
extern MemoRef* abbreviations;
MemoRef* proper_abbrevs(Slice<TypeExpr*> tl, MemoRef* abbrev);
MemoRef* simple_abbrevs();

// instance ?partial sch (partial: nullopt = None, else Some keep)
TypeExpr* instance(TypeExpr* sch, std::optional<bool> partial = std::nullopt);
TypeExpr* generic_instance(TypeExpr* sch);
std::vector<TypeExpr*> instance_list(const std::vector<TypeExpr*>& schl);
TypeExpr* subst_unscoped(ident::Unscoped* us1, ident::Unscoped* us2, TypeExpr* ty);
std::string get_new_abstract_name(env::t env, std::string_view s);
const TypeDeclaration* new_local_type(TypeOrigin origin, const Location& loc = location::none(),
                                      TypeExpr* manifest = nullptr, long scope = 0);

struct ExistentialTreatment {  // Keep_existentials_flexible | Make_existentials_abstract of penv
  PatternEnv* make_abstract = nullptr;
};
struct InstancedConstructor {
  std::vector<TypeExpr*> args;
  TypeExpr* res;
  std::vector<TypeExpr*> existentials;
};
InstancedConstructor instance_constructor(ExistentialTreatment et, const ConstructorDescription* cstr);
std::pair<std::vector<TypeExpr*>, TypeExpr*> instance_parameterized_type(
    Slice<TypeExpr*> sch_args, TypeExpr* sch, bool keep_names = false,
    std::optional<long> scope = std::nullopt);
const TypeDeclaration* instance_declaration(const TypeDeclaration* decl);
const TypeDeclaration* generic_instance_declaration(const TypeDeclaration* decl);
std::pair<std::vector<TypeExpr*>, const ClassType*> instance_class(Slice<TypeExpr*> params,
                                                                   const ClassType* cty);
std::pair<std::vector<TypeExpr*>, TypeExpr*> instance_poly_fixed(Slice<TypeExpr*> univars,
                                                                 TypeExpr* sch,
                                                                 bool keep_names = false);
TypeExpr* instance_poly(Slice<TypeExpr*> univars, TypeExpr* sch, bool keep_names = false);
TypeExpr* maybe_instance_poly(TypeExpr* ty);
TypeExpr* instance_funct_opt(Ident::t id_in, Path::t p_out, bool fixed, TypeExpr* sch);
TypeExpr* instance_funct(Ident::t id_in, Path::t p_out, bool fixed, TypeExpr* sch);
std::pair<env::t, TypeExpr*> open_tfunctor(env::t env, const Location& loc, ident::Unscoped* us,
                                           const Package* pack, TypeExpr* ty);
struct InstancedLabel {
  std::vector<TypeExpr*> vars;
  TypeExpr* arg;
  TypeExpr* res;
};
InstancedLabel instance_label(bool fixed, const LabelDescription* lbl);

// NB: raises Unify (the error), set by the unification section
extern std::function<void(const Uenv&, TypeExpr*, TypeExpr*)> unify_var_ref;
TypeExpr* subst(env::t env, long level, PrivateFlag priv, MemoRef* abbrev, TypeExpr* oty,
                Slice<TypeExpr*> params, Slice<TypeExpr*> args, TypeExpr* body,
                std::optional<long> scope = std::nullopt);
TypeExpr* apply(env::t env, Slice<TypeExpr*> params, TypeExpr* body, Slice<TypeExpr*> args,
                bool use_current_level = false);

// ---- abbreviation expansion ---------------------------------------------------------
using FindTypeExpansion = std::function<env::TypeExpansion(Path::t, env::t)>;
TypeExpr* expand_abbrev_gen(bool link, PrivateFlag kind, const FindTypeExpansion& fte, env::t env,
                            TypeExpr* ty);
TypeExpr* expand_abbrev(bool link, env::t env, TypeExpr* ty);
TypeExpr* expand_head_once(env::t env, TypeExpr* ty);
bool safe_abbrev(env::t env, TypeExpr* ty);
TypeExpr* try_expand_once(bool link, env::t env, TypeExpr* ty);
TypeExpr* try_expand_safe(env::t env, TypeExpr* ty);          // ~link:true
TypeExpr* try_expand_safe_no_link(env::t env, TypeExpr* ty);  // ~link:false
TypeExpr* try_expand_head(const std::function<TypeExpr*(env::t, TypeExpr*)>& try_once,
                          env::t env, TypeExpr* ty);
TypeExpr* expand_head_unif(env::t env, TypeExpr* ty);
TypeExpr* expand_head(env::t env, TypeExpr* ty);
TypeExpr* expand_head_nolink(env::t env, TypeExpr* ty);

struct TypedeclExtraction {  // Typedecl of p * p' * decl | Has_no_typedecl | May_have_typedecl
  enum class Kind { Typedecl, Has_no_typedecl, May_have_typedecl };
  Kind kind;
  Path::t p = nullptr;
  Path::t p2 = nullptr;
  const TypeDeclaration* decl = nullptr;
};
TypedeclExtraction extract_concrete_typedecl(env::t env, TypeExpr* ty);

TypeExpr* expand_abbrev_opt(env::t env, TypeExpr* ty);
bool safe_abbrev_opt(env::t env, TypeExpr* ty);
TypeExpr* try_expand_once_opt(env::t env, TypeExpr* ty);
TypeExpr* try_expand_once_gen_nolink(const FindTypeExpansion& fte, env::t env, TypeExpr* ty);
TypeExpr* try_expand_safe_opt(env::t env, TypeExpr* ty);
TypeExpr* expand_head_opt(env::t env, TypeExpr* ty);
TypeExpr* full_expand(bool may_forget_scope, env::t env, TypeExpr* ty);
bool generic_abbrev(env::t env, Path::t path);
bool generic_private_abbrev(env::t env, Path::t path);
const Package* extract_package_modulo_subtype(env::t env, TypeExpr* ty);  // raises env::NotFound
bool is_contractive(env::t env, Path::t p);

// ---- occur check -------------------------------------------------------------------
struct Occur {};
extern bool type_changed;
bool allow_recursive_equations(const Uenv& uenv);
void occur(const Uenv& uenv, TypeExpr* ty0, TypeExpr* ty);
void occur_for(TraceExn tr_exn, const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
bool occur_in(env::t env, TypeExpr* ty0, TypeExpr* t);
bool local_non_recursive_abbrev(const Uenv& uenv, Path::t p, TypeExpr* ty);

// ---- polymorphic unification ------------------------------------------------------
struct UnivarCell {
  TypeExpr* univ;
  TyOptRef* ref;
};
struct UnivarPair {
  std::vector<UnivarCell> cl1, cl2;
};
extern std::vector<UnivarPair> univar_pairs;  // head first
template <class F>
auto with_univar_pairs(std::vector<UnivarPair> pairs, F&& f) -> decltype(f()) {
  std::vector<UnivarPair> old = std::move(univar_pairs);
  univar_pairs = std::move(pairs);
  struct R {
    std::vector<UnivarPair>& up;
    std::vector<UnivarPair> old;
    ~R() { up = std::move(old); }
  } r{univar_pairs, std::move(old)};
  return f();
}
void unify_univar(TypeExpr* t1, TypeExpr* t2, const std::vector<UnivarPair>& pairs);
void occur_univar_or_unscoped(env::t env, TypeExpr* ty, bool inj_only = false);
bool has_free_univars(env::t env, TypeExpr* ty);
bool has_injective_univars(env::t env, TypeExpr* ty);
void enter_poly(env::t env, TypeExpr* t1, Slice<TypeExpr*> tl1, TypeExpr* t2,
                Slice<TypeExpr*> tl2, const std::function<void(TypeExpr*, TypeExpr*)>& f);
void enter_poly_for(TraceExn tr_exn, env::t env, TypeExpr* t1, Slice<TypeExpr*> tl1,
                    TypeExpr* t2, Slice<TypeExpr*> tl2,
                    const std::function<void(TypeExpr*, TypeExpr*)>& f);
void identifier_escape(env::t env, const std::vector<ident::Unscoped*>& idl, TypeExpr* ty);
void identifier_escape_for(TraceExn tr_exn, env::t env, const std::vector<ident::Unscoped*>& idl,
                           TypeExpr* t);
using IdPairs = Slice<std::pair<ident::Unscoped*, ident::Unscoped*>>;
void enter_functor_with_mtys_for(TraceExn tr_exn, env::t env, ident::Unscoped* id1,
                                 const ModuleType* mty1, TypeExpr* t1, ident::Unscoped* id2,
                                 const ModuleType* mty2, TypeExpr* t2,
                                 const std::function<void(env::t)>& f);
std::pair<TypeExpr*, std::vector<TypeExpr*>> polyfy(env::t env, TypeExpr* ty,
                                                    const std::vector<TypeExpr*>& vars);
TypeExpr* reify_univars(env::t env, TypeExpr* ty);

// ---- unification ---------------------------------------------------------------------
et::ExpandedType expand_type(env::t env, TypeExpr* ty);
et::ErrorTrace expand_trace(env::t env, const et::TypeTrace& trace);
et::Elt<et::ExpandedType> expanded_diff(env::t env, TypeExpr* got, TypeExpr* expected);
et::Elt<et::ExpandedType> unexpanded_diff(TypeExpr* got, TypeExpr* expected);
bool compatible_labels(bool in_pattern_mode, const ArgLabel& l1, const ArgLabel& l2);
void mcomp(env::t env, TypeExpr* t1, TypeExpr* t2);  // raises Incompatible
bool eq_package_path(env::t env, Path::t p1, Path::t p2);
struct NondepCannotErase {
  Ident::t id;
};
extern std::function<TypeExpr*(env::t, const std::vector<Ident::t>&, TypeExpr*)> nondep_type_ref;
// package_subtype: returns an error or success (Includemod sets it)
struct PackageSubtypeResult {
  bool ok;
  et::FirstClassModule err;
};
extern std::function<PackageSubtypeResult(env::t, const Package*, const Package*)> package_subtype;
extern bool rigid_variants;

void unify(env::t env, TypeExpr* t1, TypeExpr* t2);
void unify_uenv(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);  // raises Unify
void unify_pairs(env::t env, TypeExpr* t1, TypeExpr* t2, std::vector<UnivarPair> pairs);
btype::TypePairs* unify_gadt(PatternEnv* penv, TypeExpr* pat, TypeExpr* expected);
void unify_var(env::t env, TypeExpr* t1, TypeExpr* t2);
void unify_var_uenv(const Uenv& uenv, TypeExpr* t1, TypeExpr* t2);
void enforce_current_level(env::t env, TypeExpr* ty);
TypeExpr* expand_head_trace(env::t env, TypeExpr* t);

// ---- special cases of unification ------------------------------------------------------
struct FilteredArrow {
  TypeExpr* ty_param;
  TypeExpr* ty_ret;
};
struct FilterArrowFailure {  // Unification_error | Label_mismatch | Not_a_function
  enum class Kind { Unification_error, Label_mismatch, Not_a_function };
  Kind kind;
  et::UnificationError err;
  ArgLabel got, expected;
  TypeExpr* expected_type = nullptr;
};
template <class T>
struct FResult {  // (T, filter_arrow_failure) result
  bool ok;
  T value{};
  FilterArrowFailure error{};
};
struct Tfunctor_ {  // Types.tfunctor
  ident::Unscoped* id_us;
  const Package* pack;
  TypeExpr* ty;
};
void instance_funct_nondep_inplace(env::t env, const Tfunctor_& tfun, const ModuleType* mty);
TypeExpr* instance_funct_nondep(env::t env, const ArgLabel& l, const Tfunctor_& tfun,
                                const ModuleType* mty);
FResult<FilteredArrow> filter_arrow(env::t env, bool in_apply, TypeExpr* t, const ArgLabel& l,
                                    bool param_hole);
struct FunctorView {
  ident::Unscoped* id;
  const Package* pack;
  TypeExpr* ty;
};
FResult<std::optional<FunctorView>> filter_functor(env::t env, TypeExpr* t, const ArgLabel& l);
FResult<std::pair<env::t, TypeExpr*>> filter_arity(env::t env, TypeExpr* t, const ArgLabel& l);
bool is_really_poly(env::t env, TypeExpr* ty);

struct FilterMethodFailed : std::runtime_error {
  enum class Kind { Unification_error, Not_a_method, Not_an_object };
  Kind kind;
  et::UnificationError err;
  TypeExpr* ty = nullptr;
  explicit FilterMethodFailed(Kind k) : std::runtime_error("Ctype.Filter_method_failed"), kind(k) {}
};
TypeExpr* filter_method(env::t env, std::string_view name, TypeExpr* ty);
struct FilterMethodRowFailed {};

// ---- class signatures ----------------------------------------------------------------
ClassSignature* new_class_signature();
void add_dummy_method(env::t env, long scope, ClassSignature* sign);
void remove_dummy_method(ClassSignature* sign);
struct AddMethodFailed : std::runtime_error {
  bool unexpected_method;  // else Type_mismatch
  et::UnificationError err;
  explicit AddMethodFailed(bool u) : std::runtime_error("Ctype.Add_method_failed"), unexpected_method(u) {}
};
void add_method(env::t env, std::string_view label, PrivateFlag priv, VirtualFlag virt,
                TypeExpr* ty, ClassSignature* sign);
struct AddInstanceVariableFailed : std::runtime_error {
  bool mutability_mismatch;  // else Type_mismatch
  MutableFlag mut = MutableFlag::Immutable;
  et::UnificationError err;
  explicit AddInstanceVariableFailed(bool m)
      : std::runtime_error("Ctype.Add_instance_variable_failed"), mutability_mismatch(m) {}
};
void add_instance_variable(bool strict, env::t env, std::string_view label, MutableFlag mut,
                           VirtualFlag virt, TypeExpr* ty, ClassSignature* sign);
struct InheritClassSignatureFailed : std::runtime_error {
  enum class Kind { Self_type_mismatch, Method, Instance_variable };
  Kind kind;
  et::UnificationError err;         // Self_type_mismatch
  std::string_view label;           // Method / Instance_variable
  std::optional<AddMethodFailed> method;
  std::optional<AddInstanceVariableFailed> ivar;
  explicit InheritClassSignatureFailed(Kind k)
      : std::runtime_error("Ctype.Inherit_class_signature_failed"), kind(k) {}
};
void inherit_class_signature(bool strict, env::t env, ClassSignature* sign1,
                             const ClassSignature* sign2);
std::vector<std::string_view> update_implicitly_public_methods(ClassSignature* sign);
std::vector<std::string_view> update_implicitly_declared_methods(env::t env, ClassSignature* sign);
void hide_private_methods(const ClassSignature* sign);
void reveal_private_methods(env::t env, ClassSignature* sign);
bool close_class_signature(env::t env, ClassSignature* sign);
TypeExpr* copy_spine(TypeExpr* ty);
void generalize_class_signature_spine(ClassSignature* sign);

// ---- matching between type schemes -----------------------------------------------------
void moregen(btype::TypePairs& type_pairs, env::t env, TypeExpr* patt, TypeExpr* subj);
void moregeneral(env::t env, TypeExpr* pat_sch, TypeExpr* subj_sch);  // raises Moregen
bool is_moregeneral(env::t env, TypeExpr* pat_sch, TypeExpr* subj_sch);
std::vector<TypeExpr*> rigidify(TypeExpr* ty);
bool all_distinct_vars(env::t env, const std::vector<TypeExpr*>& vars);
void matches(bool expand_error_trace, env::t env, TypeExpr* ty, TypeExpr* ty2);  // MatchesFailure
bool does_match(env::t env, TypeExpr* ty, TypeExpr* ty2);

// ---- equivalence between parameterized types -------------------------------------------
TypeExpr* expand_head_rigid(env::t env, TypeExpr* ty);
void eqtype(bool rename, btype::TypePairs& type_pairs,
            std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst, env::t env, TypeExpr* t1,
            TypeExpr* t2);
void equal(env::t env, bool rename, Slice<TypeExpr*> tyl1, Slice<TypeExpr*> tyl2);  // Equality
bool is_equal(env::t env, bool rename, Slice<TypeExpr*> tyl1, Slice<TypeExpr*> tyl2);
void equal_private(env::t env, Slice<TypeExpr*> params1, TypeExpr* ty1, Slice<TypeExpr*> params2,
                   TypeExpr* ty2);

// ---- class type matching --------------------------------------------------------------
struct ClassMatchFailure {  // class_match_failure
  enum class Kind {
    CM_Virtual_class, CM_Parameter_arity_mismatch, CM_Type_parameter_mismatch,
    CM_Class_type_mismatch, CM_Parameter_mismatch, CM_Val_type_mismatch, CM_Meth_type_mismatch,
    CM_Non_mutable_value, CM_Non_concrete_value, CM_Missing_value, CM_Missing_method,
    CM_Hide_public, CM_Hide_virtual, CM_Public_method, CM_Private_method, CM_Virtual_method
  };
  Kind kind;
  std::string_view label;                  // the string argument (Hide_virtual: the second)
  std::string_view kind_name;              // CM_Hide_virtual's first string
  long index = 0, index2 = 0;              // arity / parameter positions
  env::t env = nullptr;
  et::EqualityError equality;              // CM_Type_parameter_mismatch
  et::MoregenError moregen;                // CM_Parameter_mismatch
  et::ComparisonError comparison{};        // CM_Val/Meth_type_mismatch
  const ClassType* cty1 = nullptr;         // CM_Class_type_mismatch
  const ClassType* cty2 = nullptr;
};
struct ClassMatchFailures {  // exception Failure of class_match_failure list
  std::vector<ClassMatchFailure> errors;
};
std::vector<ClassMatchFailure> match_class_types(env::t env, const ClassType* pat_sch,
                                                 const ClassType* subj_sch, bool trace = true);
std::vector<ClassMatchFailure> match_class_declarations(env::t env, Slice<TypeExpr*> patt_params,
                                                        const ClassType* patt_type,
                                                        Slice<TypeExpr*> subj_params,
                                                        const ClassType* subj_type);

// ---- subtyping ---------------------------------------------------------------------------
std::pair<const TypeDeclaration*, TypeExpr*> find_cltype_for_path(env::t env, Path::t p);
std::pair<TypeExpr*, bool> enlarge_type(env::t env, TypeExpr* ty);
// raises Subtype; the result enforces the accumulated constraints (raises Subtype)
std::function<void()> subtype(env::t env, TypeExpr* ty1, TypeExpr* ty2);

// ---- miscellaneous -------------------------------------------------------------------------
TypeExpr* unalias(TypeExpr* ty);
std::optional<btype::TypeSet> nongen_vars_in_schema(env::t env, TypeExpr* ty);
btype::TypeSet nongen_class_declaration(const ClassDeclaration* cty);
std::optional<btype::TypeSet> nongen_vars_in_class_declaration(const ClassDeclaration* cty);
void normalize_type(TypeExpr* ty);
struct ArrowArg {  // Arg_value of type_expr | Arg_module of Ident.Unscoped.t * package
  bool is_module;
  TypeExpr* value;
  ident::Unscoped* id;
  const Package* pack;
};
struct ArrowSpine {
  std::vector<std::pair<ArgLabel, ArrowArg>> args;
  bool ret_cycle = false;  // Ret_cycle | Ret_type ret
  TypeExpr* ret = nullptr;
};
ArrowSpine arrow_spine(env::t env, TypeExpr* ty);
std::pair<std::vector<ArgLabel>, bool> arrow_labels(env::t env, TypeExpr* ty);  // is_ret_tvar

// ---- remove dependencies -------------------------------------------------------------------
TypeExpr* nondep_type(env::t env, const std::vector<Ident::t>& ids, TypeExpr* ty);
const TypeDeclaration* nondep_type_decl(env::t env, const std::vector<Ident::t>& mid,
                                        bool is_covariant, const TypeDeclaration* decl);
const ExtensionConstructor* nondep_extension_constructor(env::t env,
                                                         const std::vector<Ident::t>& ids,
                                                         const ExtensionConstructor* ext);
const ClassDeclaration* nondep_class_declaration(env::t env, const std::vector<Ident::t>& ids,
                                                 const ClassDeclaration* decl);
const ClassTypeDeclaration* nondep_cltype_declaration(env::t env, const std::vector<Ident::t>& ids,
                                                      const ClassTypeDeclaration* decl);
void collapse_conj_params(env::t env, Slice<TypeExpr*> params);
bool same_constr(env::t env, TypeExpr* t1, TypeExpr* t2);
TypeImmediacy immediacy(env::t env, TypeExpr* typ);

}  // namespace cppcaml::typing::ctype
