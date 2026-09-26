// Port of typing/mtype.ml (TYPECHECKER.md), so far the part Env needs:
// scraping and strengthening.  The rest (nondep_*, enrich, remove_aliases,
// ...) comes with Typemod.
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


}  // namespace cppcaml::typing::mtype

namespace cppcaml::typing {
// Installs every forward reference between the typing modules
// (Env.strengthen := Mtype.strengthen_lazy, ...).  Idempotent.
void install_forward_refs();
}  // namespace cppcaml::typing
