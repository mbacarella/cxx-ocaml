// Port of typing/btype.ml (TYPECHECKER.md): basic operations on core types.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cppcaml/typing/types.hpp"

namespace cppcaml::typing::btype {

// ---- sets, maps and hashtables of types ------------------------------------
// TransientTypeOps.compare = t1.id - t2.id; the wrappers apply repr first.
struct ById {
  bool operator()(const TypeExpr* a, const TypeExpr* b) const { return a->id < b->id; }
};
using TransientTypeSet = std::set<TypeExpr*, ById>;
// TypeSet: add/mem/singleton take repr.
struct TypeSet {
  TransientTypeSet s;
  void add(TypeExpr* t) { s.insert(types::repr(t)); }
  bool mem(TypeExpr* t) const { return s.count(types::repr(t)) != 0; }
  std::vector<TypeExpr*> elements() const { return {s.begin(), s.end()}; }
  bool is_empty() const { return s.empty(); }
  bool subset(const TypeSet& o) const {
    for (TypeExpr* t : s)
      if (!o.s.count(t)) return false;
    return true;
  }
  TypeSet inter(const TypeSet& o) const {
    TypeSet r;
    for (TypeExpr* t : s)
      if (o.s.count(t)) r.s.insert(t);
    return r;
  }
};
template <class V>
using TransientTypeMap = std::map<TypeExpr*, V, ById>;
// TypeHash: keys are repr'd transient exprs (physical identity).
template <class V>
struct TypeHash {
  std::unordered_map<TypeExpr*, V> h;
  bool mem(TypeExpr* t) const { return h.count(types::repr(t)) != 0; }
  void add(TypeExpr* t, V v) { h.emplace(types::repr(t), std::move(v)); }
  void replace(TypeExpr* t, V v) { h[types::repr(t)] = std::move(v); }
  void remove(TypeExpr* t) { h.erase(types::repr(t)); }
  V* find_opt(TypeExpr* t) {
    auto it = h.find(types::repr(t));
    return it == h.end() ? nullptr : &it->second;
  }
};

// TypePairs: a set of repr'd pairs remembering insertion order.
class TypePairs {
 public:
  void clear() { set_.clear(); elems_.clear(); }
  void add(TypeExpr* a, TypeExpr* b);
  bool mem(TypeExpr* a, TypeExpr* b) const;
  // iterate in insertion order
  void iter(const std::function<void(TypeExpr*, TypeExpr*)>& f) const;

 private:
  struct H {
    std::size_t operator()(const std::pair<TypeExpr*, TypeExpr*>& p) const {
      return static_cast<std::size_t>(p.first->id + 93 * p.second->id);
    }
  };
  std::unordered_set<std::pair<TypeExpr*, TypeExpr*>, H> set_;
  std::vector<std::pair<TypeExpr*, TypeExpr*>> elems_;
};

// ---- levels -----------------------------------------------------------------
inline constexpr long generic_level = ident::highest_scope;
inline constexpr long lowest_level = ident::lowest_scope;

// ---- leveled type pool --------------------------------------------------
// with_new_pool(level, f): run f with a fresh pool on top, return the nodes
// added to it (most recent first, as the OCaml list).
std::vector<TypeExpr*> with_new_pool(long level, const std::function<void()>& f);
void add_to_pool(long level, TypeExpr* ty);

// ---- type creators ------------------------------------------------------
TypeExpr* newty3(long level, long scope, const TypeDesc* desc);
TypeExpr* newty2(long level, const TypeDesc* desc);
TypeExpr* newgenty(const TypeDesc* desc);
TypeExpr* newgenmono(TypeExpr* ty);  // Tpoly (ty, [])
TypeExpr* newgenvar(OptStr name = OptStr::none());
TypeExpr* newgenstub(long scope);

// ---- checks -----------------------------------------------------------------
bool is_Tvar(TypeExpr* ty);
bool is_Tunivar(TypeExpr* ty);
bool is_Tconstr(TypeExpr* ty);
bool is_Tpoly(TypeExpr* ty);
bool is_poly_Tpoly(TypeExpr* ty);
bool type_kind_is_abstract(const TypeDeclaration* decl);
TypeOrigin type_origin(const TypeDeclaration* decl);
inline constexpr const char* dummy_method = "*dummy method*";
const TypeDesc* get_constr_desc(TypeExpr* ty);

// ---- poly types -------------------------------------------------------------
bool tpoly_is_mono(TypeExpr* ty);
std::pair<TypeExpr*, Slice<TypeExpr*>> tpoly_get_poly(TypeExpr* ty);
TypeExpr* tpoly_get_mono(TypeExpr* ty);
TypeExpr* tpoly_get_mono_opt(TypeExpr* ty);  // nullptr = None

// ---- rows ---------------------------------------------------------------------
const FixedExplanation* merge_fixed_explanation(const FixedExplanation* f1,
                                                const FixedExplanation* f2);
const FixedExplanation* fixed_explanation(const RowDesc* row);
bool is_fixed(const RowDesc* row);
bool has_fixed_explanation(const RowDesc* row);
bool static_row(const RowDesc* row);
long hash_variant(std::string_view s);
TypeExpr* proxy(TypeExpr* ty);
TypeExpr* row_of_type(TypeExpr* t);
bool has_constr_row(TypeExpr* t);
bool is_row_name(std::string_view s);
bool is_constr_row(bool allow_ident, TypeExpr* t);
void set_static_row_name(const TypeDeclaration* decl, Path::t path);

// ---- traversal --------------------------------------------------------------
// fold_row / fold_type_desc / fold_type_expr: every OCaml fold here is a
// sequence of calls in a fixed order, so the iter_ forms (same order) serve
// with the accumulator captured by the callback.
void iter_row(const std::function<void(TypeExpr*)>& f, const RowDesc* row);
void iter_type_expr(const std::function<void(TypeExpr*)>& f, TypeExpr* ty);
void iter_type_desc(const std::function<void(TypeExpr*)>& f, const TypeDesc* d);
void iter_abbrev_memo(const std::function<void(TypeExpr*)>& f, const AbbrevMemo* m);
void iter_type_expr_cstr_args(const std::function<void(TypeExpr*)>& f,
                              const ConstructorArguments& a);
ConstructorArguments map_type_expr_cstr_args(const std::function<TypeExpr*(TypeExpr*)>& f,
                                             const ConstructorArguments& a);
void iter_type_expr_kind(const std::function<void(TypeExpr*)>& f, const TypeKind* k);
const Package* map_pack(const std::function<Path::t(Path::t)>& map_path,
                        const std::function<TypeExpr*(TypeExpr*)>& map_type,
                        const Package* p);

// ---- marking --------------------------------------------------------------
void mark_type(types::TypeMark& mark, TypeExpr* ty);
void mark_type_params(types::TypeMark& mark, TypeExpr* ty);

// ---- (object-oriented) iterator -------------------------------------------
// `'a type_iterators`: a record of open-recursive functions.  it_type_expr
// and it_do_type_expr take the node (the `(type_expr -> unit)` instance; the
// `without_type_expr` instance ignores it).
struct TypeIterators {
  std::function<void(TypeIterators&, Signature)> it_signature;
  std::function<void(TypeIterators&, const SignatureItem*)> it_signature_item;
  std::function<void(TypeIterators&, const ValueDescription*)> it_value_description;
  std::function<void(TypeIterators&, const TypeDeclaration*)> it_type_declaration;
  std::function<void(TypeIterators&, const ExtensionConstructor*)> it_extension_constructor;
  std::function<void(TypeIterators&, const ModuleDeclaration*)> it_module_declaration;
  std::function<void(TypeIterators&, const ModtypeDeclaration*)> it_modtype_declaration;
  std::function<void(TypeIterators&, const ClassDeclaration*)> it_class_declaration;
  std::function<void(TypeIterators&, const ClassTypeDeclaration*)> it_class_type_declaration;
  std::function<void(TypeIterators&, const FunctorParameter&)> it_functor_param;
  std::function<void(TypeIterators&, const ModuleType*)> it_module_type;
  std::function<void(TypeIterators&, const ClassType*)> it_class_type;
  std::function<void(TypeIterators&, const TypeKind*)> it_type_kind;
  std::function<void(TypeIterators&, TypeExpr*)> it_do_type_expr;
  std::function<void(TypeIterators&, TypeExpr*)> it_type_expr;
  std::function<void(Path::t)> it_path;
};
TypeIterators type_iterators_without_type_expr();
TypeIterators type_iterators(types::TypeMark& mark);

// ---- copying --------------------------------------------------------------
const RowDesc* copy_row(const std::function<TypeExpr*(TypeExpr*)>& f, bool fixed,
                        const RowDesc* row, bool keep, TypeExpr* more);
Commutable* copy_commu(Commutable* c);
const TypeDesc* copy_type_desc(const std::function<TypeExpr*(TypeExpr*)>& f,
                               const TypeDesc* d, bool keep_names = false);

// For_copy (copy scopes)
struct CopyScope {
  std::vector<std::pair<TypeExpr*, const TypeDesc*>> saved_desc;
};
void redirect_desc(CopyScope& scope, TypeExpr* ty, const TypeDesc* desc);
void with_copy_scope(const std::function<void(CopyScope&)>& f);

// ---- memorization of abbreviation expansion --------------------------------
TypeExpr* find_expans(PrivateFlag priv, Path::t p1, const AbbrevMemo* m);  // nullptr = None
void cleanup_abbrev_memo();
void memorize_abbrev(MemoRef* mem, PrivateFlag privacy, Path::t path,
                     TypeExpr* abbreviation, TypeExpr* expansion);
void forget_abbrev_memo(MemoRef* mem, Path::t path);

// re-exported backtracking (backtrack cleans the abbreviation memo)
types::Snapshot snapshot();
void backtrack(types::Snapshot s);

// ---- labels -------------------------------------------------------------------
bool is_optional(const ArgLabel& l);
std::string_view label_name(const ArgLabel& l);
std::string prefixed_label_name(const ArgLabel& l);
// extract_label l ls: (label, value, had-predecessors, remaining) option
template <class T>
std::optional<std::tuple<ArgLabel, T, bool, std::vector<std::pair<ArgLabel, T>>>>
extract_label(std::string_view l, const std::vector<std::pair<ArgLabel, T>>& ls) {
  for (std::size_t k = 0; k < ls.size(); ++k)
    if (label_name(ls[k].first) == l) {
      // List.rev_append hd ls: the elements before k, reversed back into order
      std::vector<std::pair<ArgLabel, T>> rest;
      for (std::size_t j = 0; j < ls.size(); ++j)
        if (j != k) rest.push_back(ls[j]);
      return std::make_tuple(ls[k].first, ls[k].second, k > 0, std::move(rest));
    }
  return std::nullopt;
}

// ---- class types ------------------------------------------------------------
ClassSignature* signature_of_class_type(const ClassType* cty);
const ClassType* class_body(const ClassType* cty);
const ClassType* scrape_class_type(const ClassType* cty);
long class_type_arity(const ClassType* cty);
const ClassType* abbreviate_class_type(Path::t path, Slice<TypeExpr*> params,
                                       const ClassType* cty);
TypeExpr* self_type(const ClassType* cty);
TypeExpr* self_type_row(const ClassType* cty);
std::vector<std::string_view> methods(const ClassSignature* sign);
std::vector<std::string_view> virtual_methods(const ClassSignature* sign);
std::set<std::string_view> concrete_methods(const ClassSignature* sign);
std::vector<std::string_view> public_methods(const ClassSignature* sign);
std::vector<std::string_view> instance_vars(const ClassSignature* sign);
std::vector<std::string_view> virtual_instance_vars(const ClassSignature* sign);
std::set<std::string_view> concrete_instance_vars(const ClassSignature* sign);
TypeExpr* method_type(std::string_view label, const ClassSignature* sign);
TypeExpr* instance_variable_type(std::string_view label, const ClassSignature* sign);

// ---- deep occurrences and folded description -----------------------------
bool deep_occur(TypeExpr* t0, TypeExpr* ty);
bool deep_occur_list(TypeExpr* t0, const std::vector<TypeExpr*>& tyl);
const TypeDesc* get_folded_desc(bool keep_Tvar, TypeExpr* ty);

}  // namespace cppcaml::typing::btype
