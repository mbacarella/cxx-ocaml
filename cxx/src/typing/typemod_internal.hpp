// Definitions shared by the typemod*.cpp parts of the typing/typemod.ml
// port.
#pragma once

#include <map>
#include <optional>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/mtype.hpp"
#include "cppcaml/typing/signature_group.hpp"
#include "cppcaml/typing/subst.hpp"
#include "cppcaml/typing/typeclass.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typemod.hpp"
#include "cppcaml/typing/typetexp.hpp"

namespace cppcaml::typing::typemod {

using EK = Error::Kind;
[[noreturn]] void raise_error(const Error& e);
inline Error err(const Location& loc, env::t env, EK k) { return Error(loc, env, k); }

Path::t path_concat(Ident::t head, Path::t p);
Signature extract_sig(env::t env, const Location& loc, const ModuleType* mty);
Signature extract_sig_open(env::t env, const Location& loc, const ModuleType* mty);
std::pair<const tt::OpenDescription*, env::t> type_open_descr(bool* used_slot, bool toplevel, env::t env,
                                                              const pt::OpenDescription* sod);
extern std::function<std::pair<const tt::ModuleExpr*, const ModuleType*>(env::t, const pt::ModuleExpr*)>
    type_module_type_of_fwd;
void check_recmod_typedecls(env::t env, const std::vector<std::pair<Ident::t, const ModuleDeclaration*>>& decls);
void check_well_formed_module(env::t env, const Location& loc, const std::string& context, const ModuleType* mty);
std::optional<pt::LidLoc> type_decl_is_alias(const pt::TypeDeclaration* sdecl);

// types.ml's classify_signature_item: (kind, id, loc)
struct Classified {
  SigComponentKind kind;
  Ident::t id;
  Location loc;
};
Classified classify_signature_item(const SignatureItem* it);

// SignatureItem builders
const SignatureItem* sig_value(Ident::t id, const ValueDescription* v, Visibility vis);
const SignatureItem* sig_type(Ident::t id, const TypeDeclaration* d, RecStatus rs, Visibility vis);
const SignatureItem* sig_typext(Ident::t id, const ExtensionConstructor* e, ExtStatus es, Visibility vis);
const SignatureItem* sig_module(Ident::t id, ModulePresence pres, const ModuleDeclaration* md, RecStatus rs,
                                Visibility vis);
const SignatureItem* sig_modtype(Ident::t id, const ModtypeDeclaration* mtd, Visibility vis);
const SignatureItem* sig_class(Ident::t id, const ClassDeclaration* c, RecStatus rs, Visibility vis);
const SignatureItem* sig_class_type(Ident::t id, const ClassTypeDeclaration* c, RecStatus rs, Visibility vis);
const ModuleType* mty_signature(Signature sg);
const ModuleType* mty_ident(Path::t p);
const ModuleType* mty_alias(Path::t p);
const ModuleType* mty_functor(const FunctorParameter& param, const ModuleType* res);

// module Merge
namespace merge {
struct TypeResult {
  const tt::TTypeDeclaration* tdecl;
  Path::t path;
  pt::LidLoc lid;
  Signature sg;
};
TypeResult merge_type(bool destructive, env::t env, const Location& loc, Signature sg, const pt::LidLoc& lid,
                      const pt::TypeDeclaration* sdecl);
Signature merge_type_approx(bool destructive, env::t env, const Location& loc, Signature sg, const pt::LidLoc& lid);
struct Result {
  Path::t path;
  pt::LidLoc lid;
  Signature sg;
};
Result merge_module(bool approx, bool destructive, env::t env, const Location& loc, Signature sg,
                    const pt::LidLoc& lid, const ModuleDeclaration* md2, Path::t path, bool remove_aliases);
Result merge_modtype(bool approx, bool destructive, env::t env, const Location& loc, Signature sg,
                     const pt::LidLoc& lid, const ModuleType* mty);
bool is_destructive(const pt::WithConstraint* c);
const ModuleType* check_package_with_type_constraints(const Location& loc, env::t env, const ModuleType* mty,
                                                      bool compute,
                                                      Slice<std::pair<pt::LidLoc, const tt::CoreType*>> constraints);
}  // namespace merge

// map_rec / map_rec_type / map_rec_type_with_row_types / map_ext: the
// items of a (recursive) group, with their recursion status, before [rem]
template <class T, class F>
std::vector<const SignatureItem*> map_rec(F fn, const std::vector<T>& decls, std::vector<const SignatureItem*> rem) {
  std::vector<const SignatureItem*> out;
  for (std::size_t k = 0; k < decls.size(); ++k) out.push_back(fn(k == 0 ? RecStatus::Trec_first : RecStatus::Trec_next, decls[k]));
  out.insert(out.end(), rem.begin(), rem.end());
  return out;
}

const ModuleType* approx_modtype(env::t env, const pt::ModuleType* smty);
bool params_are_constrained(Slice<TypeExpr*> l);
std::vector<const SignatureItem*> map_rec_type_with_row_types(RecFlag rec_flag,
                                                              const std::vector<const tt::TTypeDeclaration*>& decls);

// module Signature_names
struct Shadowable {
  Ident::t self;
  std::vector<Ident::t> group;  // the element itself and all elements removed at the same time
  Location loc;
};
struct NameInfo {  // `Exported | `From_open | `Shadowable s | `Substituted_away subst
  enum class Kind { Exported, From_open, Shadowable, Substituted_away };
  Kind kind;
  Shadowable shadowable{};
  subst::t subst = nullptr;
};
void check_value(SignatureNames* t, const Location& loc, Ident::t id, const std::optional<NameInfo>& info = std::nullopt);
void check_type(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info = {NameInfo::Kind::Exported});
void check_typext(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info = {NameInfo::Kind::Exported});
void check_module(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info = {NameInfo::Kind::Exported});
void check_modtype(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info = {NameInfo::Kind::Exported});
void check_class(SignatureNames* t, const Location& loc, Ident::t id, const NameInfo& info = {NameInfo::Kind::Exported});
void check_class_type(SignatureNames* t, const Location& loc, Ident::t id,
                      const NameInfo& info = {NameInfo::Kind::Exported});
void check_sig_item(SignatureNames* t, const Location& loc, const signature_group::RecGroup& item,
                    const std::optional<NameInfo>& info = std::nullopt);
SignatureNames* create_signature_names();

// signatures (typemod_sig.cpp)
const tt::ModuleType* transl_modtype(env::t env, const pt::ModuleType* smty);
const tt::Signature* transl_signature_(env::t env, pt::Signature sg);
struct RecmoduleModtype {
  Ident::t id;
  pt::StrLoc name;
  const tt::ModuleType* mty;
  Uid uid;
  Attributes attrs;
  Location loc;
};
struct TranslRecmodule {
  std::vector<const tt::TModuleDeclaration*> decls;
  env::t env;
};
TranslRecmodule transl_recmodule_modtypes(env::t env, Slice<const pt::ModuleDeclaration*> sdecls);
const ModuleType* transl_modtype_longident_(const Location& loc, env::t env, Longident::t lid);
struct TranslModtypeDecl {
  const tt::TModuleTypeDeclaration* mtd;
  const ModtypeDeclaration* decl;
  env::t env;
};
TranslModtypeDecl transl_modtype_decl(env::t env, const pt::ModuleTypeDeclaration* pmtd);

// structures (typemod_str.cpp)
void check_nongen_signature_(env::t env, Signature sg);
void check_nongen_modtype(env::t env, const Location& loc, const ModuleType* mty);
env::t enrich_type_decls(Path::t anchor, const std::vector<const tt::TTypeDeclaration*>& decls, env::t oldenv,
                         env::t newenv);
const ModuleType* enrich_module_type(Path::t anchor, const OptStr& name, const ModuleType* mty, env::t env);
const ModuleType* modtype_of_package_(env::t env, const Location& loc, const Package* pack);
const tt::ModuleExpr* type_module_(bool alias, bool strengthen, bool funct_body, Path::t anchor, env::t env,
                                   const pt::ModuleExpr* smod);
TypeStructureResult type_structure_(bool toplevel, bool funct_body, Path::t anchor, env::t env, pt::Structure sstr);
struct TypeOpenDeclResult {
  const tt::OpenDeclaration* od;
  Signature sg;
  env::t env;
};
TypeOpenDeclResult type_open_decl_(bool* used_slot, bool toplevel, bool funct_body, SignatureNames* names,
                                   env::t env, const pt::OpenDeclaration* sod);

}  // namespace cppcaml::typing::typemod
