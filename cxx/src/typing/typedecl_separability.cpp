// Port of typing/typedecl_separability.ml: checking and inference of the
// separability signature of type declarations (see the long comments of
// the OCaml file for the theory: modes Ind < Sep < Deepsep, coinductive
// hypotheses safe / unsafe / poison on cyclic types).
#include "cppcaml/typing/typedecl_separability.hpp"

#include <map>
#include <set>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "typedecl_properties.hpp"

namespace cppcaml::typing::typedecl_separability {

using namespace types;
using Mode = Separability;

namespace {

int rank(Mode m) { return m == Mode::Ind ? 0 : m == Mode::Sep ? 1 : 2; }
Mode max_mode(Mode a, Mode b) { return rank(a) >= rank(b) ? a : b; }
// the mode on [_] imposed by the context composition [e(e'(_))]
Mode compose(Mode m1, Mode m2) {
  switch (m1) {
    case Mode::Deepsep: return Mode::Deepsep;
    case Mode::Sep: return m2;
    case Mode::Ind: return Mode::Ind;
  }
  return m1;
}

struct TypeVar {
  OptStr text;  // the user name of the type variable, None for '_'
  long id;      // the identifier of the type node of the variable
};
// TVarMap: keyed by the id (Map.add keeps the latest key)
struct Context {
  std::map<long, std::pair<TypeVar, Mode>> m;
};
Context operator+(const Context& a, const Context& b) {  // (++)
  Context r = a;
  for (auto& [id, e] : b.m) {
    auto it = r.m.find(id);
    if (it == r.m.end()) r.m.emplace(id, e);
    else it->second.second = max_mode(it->second.second, e.second);
  }
  return r;
}

// Summarize the right-hand-side of a type declaration
struct TypeStructure {
  enum class Kind { Synonym, Abstract, Open, Algebraic, Unboxed };
  Kind kind;
  TypeExpr* ty = nullptr;             // Synonym / Unboxed argument_type
  std::vector<TypeExpr*> instances;   // Unboxed result_type_parameter_instances
};
TypeStructure structure(const TypeDeclaration* def) {
  const TypeKind* k = def->type_kind;
  switch (k->kind) {
    case TypeKind::Kind::Type_open: return {TypeStructure::Kind::Open};
    case TypeKind::Kind::Type_abstract:
      // (the Typing_recovery.erroneous_type_check case is for merlin)
      if (!def->type_manifest) return {TypeStructure::Kind::Abstract};
      return {TypeStructure::Kind::Synonym, def->type_manifest};
    case TypeKind::Kind::Type_external: return {TypeStructure::Kind::Abstract};
    default: break;
  }
  TypeExpr* ty = nullptr;
  if (k->kind == TypeKind::Kind::Type_record && k->labels.size() == 1 &&
      k->record_repr.kind == RecordRepresentation::Kind::Record_unboxed) {
    ty = k->labels[0]->ld_type;
  } else if (k->kind == TypeKind::Kind::Type_variant && k->constructors.size() == 1 &&
             k->variant_repr == VariantRepresentation::Variant_unboxed) {
    const ConstructorArguments& a = k->constructors[0]->cd_args;
    if (a.kind == ConstructorArguments::Kind::Cstr_tuple && a.tuple.size() == 1) ty = a.tuple[0];
    else if (a.kind == ConstructorArguments::Kind::Cstr_record && a.record.size() == 1) ty = a.record[0]->ld_type;
  }
  if (!ty) return {TypeStructure::Kind::Algebraic};
  std::vector<TypeExpr*> params(def->type_params.begin(), def->type_params.end());
  if (k->kind == TypeKind::Kind::Type_variant && k->constructors[0]->cd_res) {
    auto* tc = as<Tconstr>(get_desc(k->constructors[0]->cd_res));
    if (!tc) throw std::logic_error("Typedecl_separability.structure");
    params.assign(tc->args.begin(), tc->args.end());
  }
  return {TypeStructure::Kind::Unboxed, ty, params};
}

std::vector<TypeVar> free_variables(TypeExpr* ty) {
  std::vector<TypeVar> out;
  for (TypeExpr* t : ctype::free_variables(ty)) {
    auto* v = as<Tvar>(get_desc(t));
    if (!v) throw std::logic_error("Typedecl_separability.free_variables");
    out.push_back({v->name, get_id(t)});
  }
  return out;
}

// Coinductive hypotheses: TypeMap of ModeSet, keyed by the type's id
using ModeMap = std::map<long, std::set<int>>;  // ranks
struct Hyps {
  ModeMap safe, unsafe, poison;
};
ModeMap merge(const ModeMap& a, const ModeMap& b) {
  ModeMap r = a;
  for (auto& [k, s] : b) r[k].insert(s.begin(), s.end());
  return r;
}
Hyps guard(const Hyps& h) { return {merge(h.safe, h.unsafe), {}, h.poison}; }
Hyps poison(const Hyps& h) { return {h.safe, {}, merge(h.poison, h.unsafe)}; }
Hyps add(TypeExpr* ty, Mode m, const Hyps& h) {
  ModeMap mm{{get_id(ty), {rank(m)}}};
  return {h.safe, merge(mm, h.unsafe), h.poison};
}
bool safe(TypeExpr* ty, Mode m, const Hyps& h) {
  auto it = h.safe.find(get_id(ty));
  if (it == h.safe.end() || it->second.empty()) return false;
  return *it->second.rbegin() >= rank(m);
}
bool unsafe(TypeExpr* ty, Mode m, const Hyps& h) {
  for (const ModeMap* s : {&h.unsafe, &h.poison}) {
    auto it = s->find(get_id(ty));
    if (it != s->end() && it->second.count(rank(m))) return true;
  }
  return false;
}

// any mode checking [ty : m] is satisfied in the "worse case" context that
// maps all free variables of [ty] to Deepsep
Context worst_case(TypeExpr* ty) {
  Context c;
  for (auto& v : free_variables(ty)) c.m[v.id] = {v, Mode::Deepsep};
  return c;
}

void immediate_subtypes_object_row(std::vector<TypeExpr*>& acc, TypeExpr* ty) {
  for (;;) {
    const TypeDesc* d = get_desc(ty);
    if (d->kind == DescKind::Tnil) return;
    if (auto* f = as<Tfield>(d)) {
      acc.insert(acc.begin(), f->ty);
      ty = f->rest;
      continue;
    }
    acc.insert(acc.begin(), ty);
    return;
  }
}
void immediate_subtypes_variant_row(std::vector<TypeExpr*>& acc, const RowDesc* desc) {
  for (auto& e : row_fields(desc)) {
    RowFieldView f = row_field_repr(e.field);
    if (f.kind == RowFieldView::Kind::Rpresent && f.present) acc.insert(acc.begin(), f.present);
    else if (f.kind == RowFieldView::Kind::Reither)
      for (TypeExpr* t : f.arg_types) acc.insert(acc.begin(), t);  // List.rev_append
  }
  TypeExpr* row = row_more(desc);
  if (auto* v = as<Tvariant>(get_desc(row))) immediate_subtypes_variant_row(acc, v->row);
  else acc.insert(acc.begin(), row);
}

}  // namespace

// all the immediate sub-type-expressions of [ty] (never on a Tfunctor)
std::vector<TypeExpr*> immediate_subtypes(TypeExpr* ty) {
  const TypeDesc* d = get_desc(ty);
  std::vector<TypeExpr*> acc;
  switch (d->kind) {
    case DescKind::Tarrow: return {as<Tarrow>(d)->t1, as<Tarrow>(d)->t2};
    case DescKind::Ttuple:
      for (auto& x : as<Ttuple>(d)->elems) acc.push_back(x.ty);
      return acc;
    case DescKind::Tpackage:
      for (auto& c : as<Tpackage>(d)->pack->pack_constraints) acc.push_back(c.ty);
      return acc;
    case DescKind::Tobject: {
      auto* o = as<Tobject>(d);
      if (o->name->contents) acc.assign(o->name->contents->args.begin(), o->name->contents->args.end());
      immediate_subtypes_object_row(acc, o->fields);
      return acc;
    }
    case DescKind::Tvariant: immediate_subtypes_variant_row(acc, as<Tvariant>(d)->row); return acc;
    case DescKind::Tnil:
    case DescKind::Tfield: immediate_subtypes_object_row(acc, ty); return acc;
    case DescKind::Tvar:
    case DescKind::Tunivar: return {};
    case DescKind::Tpoly: return {as<Tpoly>(d)->body};
    case DescKind::Tconstr: {
      auto a = as<Tconstr>(d)->args;
      return std::vector<TypeExpr*>(a.begin(), a.end());
    }
    case DescKind::Tfunctor: throw std::logic_error("[Typedecl_separability.immediate_subtypes] invalid argument");
    default: throw std::logic_error("immediate_subtypes");
  }
}

namespace {

// the most permissive context [gamma] such that [ty] is separable at mode
// [m] in [gamma]
Context check_type(env::t env, TypeExpr* ty, Mode m) {
  std::function<Context(env::t, const Hyps&, TypeExpr*, Mode)> rec = [&](env::t env, const Hyps& hyps0,
                                                                          TypeExpr* ty, Mode m) -> Context {
    if (safe(ty, m, hyps0)) return {};
    if (unsafe(ty, m, hyps0)) return worst_case(ty);
    Hyps hyps = add(ty, m, hyps0);
    const TypeDesc* d = get_desc(ty);
    if (d->kind == DescKind::Tlink || d->kind == DescKind::Texpand || d->kind == DescKind::Tsubst)
      throw std::logic_error("check_type");
    if (m == Mode::Ind) return {};
    switch (d->kind) {
      case DescKind::Tvar: {
        Context c;
        c.m[get_id(ty)] = {TypeVar{as<Tvar>(d)->name, get_id(ty)}, m};
        return c;
      }
      case DescKind::Tarrow:
      case DescKind::Ttuple:
      case DescKind::Tvariant:
      case DescKind::Tobject:
      case DescKind::Tnil:
      case DescKind::Tfield:
      case DescKind::Tpackage: {
        if (m == Mode::Sep) return {};
        Context c;
        for (TypeExpr* t : immediate_subtypes(ty)) c = c + rec(env, guard(hyps), t, Mode::Deepsep);
        return c;
      }
      case DescKind::Tfunctor: {
        if (m == Mode::Sep) return {};
        auto* f = as<Tfunctor>(d);
        const ModuleType* mty = ctype::modtype_of_package(env, location::none(), f->pack);
        env::t env2 = env::add_module(Ident::of_unscoped(f->id), ModulePresence::Mp_present, mty, env);
        Context c;
        for (auto& pc : f->pack->pack_constraints) c = c + rec(env, guard(hyps), pc.ty, Mode::Deepsep);
        return c + rec(env2, guard(hyps), f->body, Mode::Deepsep);
      }
      // Polymorphic type: the new variable is ignored (see the OCaml file)
      case DescKind::Tpoly: return rec(env, hyps, as<Tpoly>(d)->body, m);
      case DescKind::Tunivar: return {};
      case DescKind::Tconstr: {
        auto* tc = as<Tconstr>(d);
        Slice<Separability> msig = env::find_type(tc->path, env)->type_separability;
        if (tc->args.size() != msig.size()) throw std::invalid_argument("List.combine");
        Context c;
        for (std::size_t k = 0; k < tc->args.size(); ++k) {
          Mode m_param = msig[k];
          Hyps h = m_param == Mode::Ind ? guard(hyps) : m_param == Mode::Sep ? hyps : poison(hyps);
          c = c + rec(env, h, tc->args[k], compose(m, m_param));
        }
        return c;
      }
      default: throw std::logic_error("check_type");
    }
  };
  return rec(env, Hyps{}, ty, m);
}

std::vector<Mode> best_msig(const TypeDeclaration* decl) { return std::vector<Mode>(decl->type_params.size(), Mode::Ind); }
std::vector<Mode> worst_msig(const TypeDeclaration* decl) {
  return std::vector<Mode>(decl->type_params.size(), Mode::Deepsep);
}
// the mode signature of an abstract/external type: assume the worst,
// unless the type is immediate
std::vector<Mode> msig_of_external_type(const TypeDeclaration* decl) {
  if (decl->type_immediate == TypeImmediacy::Unknown) return worst_msig(decl);
  return best_msig(decl);
}

// the separability signature of a single-constructor type whose definition
// is valid in the mode context [context]
std::vector<Mode> msig_of_context(const Location& decl_loc, const std::vector<TypeExpr*>& parameters,
                                  Context context) {
  auto get = [&](long id) {
    auto it = context.m.find(id);
    return it == context.m.end() ? Mode::Ind : it->second.second;
  };
  auto set_ind = [&](const TypeVar& v) { context.m[v.id] = {v, Mode::Ind}; };
  // processed from left to right (List.fold_left): see the OCaml comment on
  // non-principality
  std::vector<Mode> mode_signature;
  for (TypeExpr* param_instance : parameters) {
    if (auto* v = as<Tvar>(get_desc(param_instance))) {
      TypeVar var{v->name, get_id(param_instance)};
      mode_signature.push_back(get(var.id));
      set_ind(var);
    } else {
      std::vector<TypeVar> instance_exis = free_variables(param_instance);
      bool all_ind = true;
      for (auto& e : instance_exis) all_ind = all_ind && get(e.id) == Mode::Ind;
      if (all_ind) {
        mode_signature.push_back(Mode::Ind);
      } else {
        mode_signature.push_back(Mode::Deepsep);
        for (auto& e : instance_exis) set_ind(e);
      }
    }
  }
  // all variables remaining in the context are purely existential and
  // should not require a stronger mode than Ind
  for (auto& [id, e] : context.m)
    if (rank(e.second) > rank(Mode::Ind)) throw Error(decl_loc, e.first.text);
  return mode_signature;
}

std::vector<Mode> check_def(env::t env, const TypeDeclaration* def) {
  TypeStructure s = structure(def);
  switch (s.kind) {
    case TypeStructure::Kind::Abstract: return msig_of_external_type(def);
    case TypeStructure::Kind::Synonym:
      return msig_of_context(def->type_loc, std::vector<TypeExpr*>(def->type_params.begin(), def->type_params.end()),
                             check_type(env, s.ty, Mode::Sep));
    case TypeStructure::Kind::Open:
    case TypeStructure::Kind::Algebraic: return best_msig(def);
    case TypeStructure::Kind::Unboxed: return msig_of_context(def->type_loc, s.instances, check_type(env, s.ty, Mode::Sep));
  }
  return best_msig(def);
}

}  // namespace

// (Config.flat_float_array is true)
std::vector<Separability> compute_decl(env::t env, const TypeDeclaration* decl) { return check_def(env, decl); }

std::vector<std::pair<Ident::t, const TypeDeclaration*>> update_decls(
    env::t env, const std::vector<std::pair<Ident::t, const TypeDeclaration*>>& decls) {
  using namespace typedecl_properties;
  using Prop = std::vector<Separability>;
  Property<Prop, Unit> property;
  property.eq = [](const Prop& a, const Prop& b) { return a == b; };
  // the update function is monotonous: ~new_prop is always more informative
  property.merge = [](const Prop&, const Prop& new_prop) { return new_prop; };
  property.default_ = [](Decl d) { return best_msig(d); };
  property.compute = [](env::t e, Decl d, const Unit&) { return compute_decl(e, d); };
  property.update_decl = [](Decl decl, const Prop& sep) {
    TypeDeclaration* d = make<TypeDeclaration>(*decl);
    d->type_separability = slice(sep);
    return static_cast<Decl>(d);
  };
  property.check = [](env::t, Ident::t, Decl, const Unit&) {};  // FIXME run final check? (typedecl_separability.ml)
  return compute_property_noreq(property, env, decls);
}

}  // namespace cppcaml::typing::typedecl_separability
