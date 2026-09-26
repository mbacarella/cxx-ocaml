// Definitions shared by the typedecl*.cpp parts of the typing/typedecl.ml
// port.
#pragma once

#include <optional>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/primitive.hpp"
#include "cppcaml/typing/subst.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typedecl_separability.hpp"
#include "cppcaml/typing/typedecl_unboxed.hpp"
#include "cppcaml/typing/typeopt.hpp"
#include "cppcaml/typing/typetexp.hpp"

namespace cppcaml::typing::typedecl {

using EK = Error::Kind;
// Config.max_tag
inline constexpr long max_tag = 245;

[[noreturn]] void raise_error(const Error& e);
std::optional<bool> get_unboxed_from_attributes(const pt::TypeDeclaration* sdecl);
env::t add_type_attrs(bool check, Ident::t id, const TypeDeclaration* decl, env::t env);
env::t enter_type(const std::optional<TypeOrigin>& abstract_abbrevs, RecFlag rec_flag, env::t env,
                  const pt::TypeDeclaration* sdecl, Ident::t id, Uid uid);
std::vector<Separability> default_separability(long arity);
void set_private_row(env::t env, const Location& loc, Path::t p, const TypeDeclaration* decl);
std::vector<tt::TypeParam> make_params(env::t env, Slice<pt::TypeParam> params);
std::pair<std::vector<const tt::TLabelDeclaration*>, std::vector<const LabelDeclaration*>> transl_labels(
    env::t env, const typetexp::ty_var_env::PolyUnivars* univars, bool closed,
    Slice<const pt::LabelDeclaration*> lbls);
struct MadeConstructor {
  tt::TConstructorArguments targs;
  const tt::CoreType* tret_type;  // option
  ConstructorArguments args;
  TypeExpr* ret_type;             // option
};
MadeConstructor make_constructor(env::t env, const Location& loc, Path::t type_path,
                                 const std::vector<TypeExpr*>& type_params, Slice<pt::StrLoc> svars,
                                 const pt::ConstructorArguments& sargs, const pt::CoreType* sret_type);
const TypeDeclaration* name_recursion(const pt::TypeDeclaration* sdecl, Ident::t id, const TypeDeclaration* decl);

}  // namespace cppcaml::typing::typedecl
