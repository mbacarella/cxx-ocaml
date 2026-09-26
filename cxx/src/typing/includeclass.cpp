// Port of typing/includeclass.ml: inclusion checks for the class language.
// (Alerts are not ported; reporting comes with Printtyp.)
#include "cppcaml/typing/includeclass.hpp"

namespace cppcaml::typing::includeclass {

std::vector<ctype::ClassMatchFailure> class_types(env::t env, const ClassType* cty1, const ClassType* cty2) {
  return ctype::match_class_types(env, cty1, cty2);
}

std::vector<ctype::ClassMatchFailure> class_type_declarations(const Location&, env::t env,
                                                              const ClassTypeDeclaration* cty1,
                                                              const ClassTypeDeclaration* cty2) {
  return ctype::match_class_declarations(env, cty1->clty_params, cty1->clty_type, cty2->clty_params,
                                         cty2->clty_type);
}

std::vector<ctype::ClassMatchFailure> class_declarations(env::t env, const ClassDeclaration* cty1,
                                                         const ClassDeclaration* cty2) {
  if (!cty1->cty_new && cty2->cty_new) {
    ctype::ClassMatchFailure f{};
    f.kind = ctype::ClassMatchFailure::Kind::CM_Virtual_class;
    return {f};
  }
  return ctype::match_class_declarations(env, cty1->cty_params, cty1->cty_type, cty2->cty_params, cty2->cty_type);
}

}  // namespace cppcaml::typing::includeclass
