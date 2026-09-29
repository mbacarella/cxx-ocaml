// Port of typing/mtype.ml (cxx/PORTING.md): operations on module types
// (scraping, strengthening, nondep, enrich, alias removal, ...).
#pragma once

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/env.hpp"

namespace cppcaml::typing::mtype {

namespace lz = subst::lazy;

const lz::Modtype* scrape_lazy(env::t env, const lz::Modtype* mty);
const ModuleType* scrape(env::t env, const ModuleType* mty);
const ModuleType* freshen(int scope, const ModuleType* mty);
const lz::Modtype* strengthen_lazy(bool aliasable, env::t env, const lz::Modtype* mty, Path::t p);
const lz::ModuleDecl* strengthen_lazy_decl(bool aliasable, env::t env, const lz::ModuleDecl* md,
                                           Path::t p);
const ModuleType* strengthen(bool aliasable, env::t env, const ModuleType* mty, Path::t p);
const ModuleDeclaration* strengthen_decl(bool aliasable, env::t env, const ModuleDeclaration* md,
                                         Path::t p);
const ModuleType* scrape_for_functor_arg(env::t env, const ModuleType* mty);
const ModuleType* scrape_for_type_of(bool remove_aliases, env::t env, const ModuleType* mty);
// raise ctype::NondepCannotErase
const ModuleType* nondep_supertype(env::t env, const std::vector<Ident::t>& ids, const ModuleType* mty);
const SignatureItem* nondep_sig_item(env::t env, const std::vector<Ident::t>& ids, const SignatureItem* item);
bool no_code_needed(env::t env, const ModuleType* mty);
bool no_code_needed_sig(env::t env, Signature sg);
const ModuleType* enrich_modtype(env::t env, Path::t p, const ModuleType* mty);
const TypeDeclaration* enrich_typedecl(env::t env, Path::t p, Ident::t id, const TypeDeclaration* decl);
std::vector<Path::t> type_paths(env::t env, Path::t p, const ModuleType* mty);
bool contains_type(env::t env, const ModuleType* mty);
void lower_nongen(long nglev, const ModuleType* mty);


}  // namespace cppcaml::typing::mtype

namespace cppcaml::typing {
// Installs every forward reference between the typing modules
// (Env.strengthen := Mtype.strengthen_lazy, ...).  Idempotent.
void install_forward_refs();
}  // namespace cppcaml::typing
