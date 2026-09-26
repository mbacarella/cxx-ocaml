// Port of typing/subst.ml (TYPECHECKER.md): substitutions on types,
// declarations and (lazily) signatures.
#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/lazy_backtrack.hpp"

namespace cppcaml::typing::subst {

struct TypeReplacement {  // Path of Path.t | Type_function of {params; body}
  bool is_path;
  Path::t path = nullptr;
  Slice<TypeExpr*> params;
  TypeExpr* body = nullptr;
};

struct S {
  PathMap<const TypeReplacement*> types;
  PathMap<Path::t> modules;
  PathMap<const ModuleType*> modtypes;
  bool for_saving = false;
  const Location* loc = nullptr;  // None = nullptr
};
using t = const S*;  // `safe subst` and `unsafe subst` share the representation

struct ModuleTypePathSubstitutedAway : std::runtime_error {
  Path::t path;
  const ModuleType* mty;
  ModuleTypePathSubstitutedAway(Path::t p, const ModuleType* m)
      : std::runtime_error("Subst.Module_type_path_substituted_away"), path(p), mty(m) {}
};

t identity();
t add_type(Ident::t id, Path::t p, t s);
t add_module(Ident::t id, Path::t p, t s);
t add_modtype(Ident::t id, Path::t p, t s);
t add_modtype_path(Path::t p, Path::t p2, t s);
t for_saving(t s);
t change_locs(t s, const Location& loc);
void reset_for_saving();

Path::t module_path(t s, Path::t p);
Path::t modtype_path(t s, Path::t p);
Path::t type_path(t s, Path::t p);

TypeExpr* type_expr(t s, TypeExpr* ty);
const TypeDeclaration* type_declaration(t s, const TypeDeclaration* d);
const ClassDeclaration* class_declaration(t s, const ClassDeclaration* d);
const ClassTypeDeclaration* cltype_declaration(t s, const ClassTypeDeclaration* d);
const ClassType* class_type(t s, const ClassType* c);
const ValueDescription* value_description(t s, const ValueDescription* d);
const ExtensionConstructor* extension_constructor(t s, const ExtensionConstructor* e);

struct Scoping {
  enum class Kind : std::uint8_t { Keep, Make_local, Rescope };
  Kind kind = Kind::Keep;
  int scope = 0;  // Rescope
  static Scoping keep() { return {}; }
  static Scoping make_local() { return {Kind::Make_local}; }
  static Scoping rescope(int n) { return {Kind::Rescope, n}; }
};

Signature signature(Scoping sc, t s, Signature sg);
const SignatureItem* signature_item(Scoping sc, t s, const SignatureItem* it);
const ModtypeDeclaration* modtype_declaration(Scoping sc, t s, const ModtypeDeclaration* d);
const ModuleDeclaration* module_declaration(Scoping sc, t s, const ModuleDeclaration* d);
const ModuleType* modtype(Scoping sc, t s, const ModuleType* m);
// apply (compose s1 s2) x = apply s2 (apply s1 x)
t compose(t s1, t s2);

// ---- Lazy ------------------------------------------------------------------
namespace lazy {

struct Modtype;
struct SignatureItem;
struct SigPrime {  // S_eager of Types.signature | S_lazy of signature_item list
  bool eager = true;
  Signature eager_sg;
  Slice<const SignatureItem*> lazy_sg;
};
struct SigThunk {  // (scoping * t * signature')
  Scoping scoping;
  subst::t s;
  SigPrime sg;
};
using Signature = LazyBacktrack<SigThunk, SigPrime>*;

struct FunctorParameter {
  bool is_unit = true;
  Ident::t id = nullptr;  // nullptr = None
  const Modtype* mty = nullptr;
};

struct Modtype {
  enum class Kind : std::uint8_t { MtyL_ident, MtyL_signature, MtyL_functor, MtyL_alias };
  Kind kind;
  Path::t path = nullptr;
  lazy::Signature sign = nullptr;
  FunctorParameter param;
  const Modtype* res = nullptr;
};

struct ModuleDecl {
  const Modtype* mdl_type;
  Attributes mdl_attributes;
  Location mdl_loc;
  Uid mdl_uid;
};

struct ModtypeDecl {
  const Modtype* mtdl_type;  // nullptr = None
  Attributes mtdl_attributes;
  Location mtdl_loc;
  Uid mtdl_uid;
};

struct SignatureItem {
  using K = typing::SignatureItem::Kind;
  K kind;
  Ident::t id;
  Visibility vis = Visibility::Exported;
  RecStatus rec = RecStatus::Trec_not;
  const ValueDescription* value = nullptr;
  const TypeDeclaration* type = nullptr;
  const ExtensionConstructor* ext = nullptr;
  ExtStatus ext_status = ExtStatus::Text_first;
  ModulePresence presence = ModulePresence::Mp_present;
  const ModuleDecl* md = nullptr;
  const ModtypeDecl* mtd = nullptr;
  const ClassDeclaration* cls = nullptr;
  const ClassTypeDeclaration* clty = nullptr;
};

const ModuleDecl* of_module_decl(const ModuleDeclaration* md);
const Modtype* of_modtype(const ModuleType* m);
const ModtypeDecl* of_modtype_decl(const ModtypeDeclaration* d);
lazy::Signature of_signature(typing::Signature sg);
lazy::Signature of_signature_items(Slice<const SignatureItem*> sg);
const SignatureItem* of_signature_item(const typing::SignatureItem* it);

const ModuleDecl* module_decl(Scoping sc, subst::t s, const ModuleDecl* md);
const Modtype* modtype(Scoping sc, subst::t s, const Modtype* m);
const ModtypeDecl* modtype_decl(Scoping sc, subst::t s, const ModtypeDecl* d);
lazy::Signature signature(Scoping sc, subst::t s, lazy::Signature sg);
const SignatureItem* signature_item(Scoping sc, subst::t s, const SignatureItem* it);

const ModuleDeclaration* force_module_decl(const ModuleDecl* md);
const ModuleType* force_modtype(const Modtype* m);
const ModtypeDeclaration* force_modtype_decl(const ModtypeDecl* d);
typing::Signature force_signature(lazy::Signature sg);
Slice<const SignatureItem*> force_signature_once(lazy::Signature sg);
const typing::SignatureItem* force_signature_item(const SignatureItem* it);

}  // namespace lazy

// ---- Unsafe ---------------------------------------------------------------
namespace unsafe {
t add_modtype_path(Path::t p, const ModuleType* mty, t s);
t add_modtype(Ident::t id, const ModuleType* mty, t s);
t add_type_path(Path::t id, Path::t p, t s);
t add_type_function(Path::t id, Slice<TypeExpr*> params, TypeExpr* body, t s);
t add_module_path(Path::t id, Path::t p, t s);
// The `wrap`ped entry points: Error (Fcm_type_substituted_away (p, mty)) is
// reported as a ModuleTypePathSubstitutedAway exception to the caller.
}  // namespace unsafe

}  // namespace cppcaml::typing::subst
