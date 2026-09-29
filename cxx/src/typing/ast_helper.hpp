// The parts of parsing/ast_helper.ml the typer uses to build syntax
// (Exp.constraint_, Pat.constraint_, Typ.poly, Mty.with_, ...).  The default
// location is `!default_loc` = Location.none, as the typer never sets it.
#pragma once

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::ast_helper {

namespace pt = parsetree;

inline const pt::Expression* exp_mk(const pt::ExpressionDesc* d, const Location& loc,
                                    pt::Attributes attrs = {}) {
  return make<pt::Expression>(d, loc, pt::LocationStack{}, attrs);
}
inline const pt::Pattern* pat_mk(const pt::PatternDesc* d, const Location& loc,
                                 pt::Attributes attrs = {}) {
  return make<pt::Pattern>(d, loc, pt::LocationStack{}, attrs);
}
inline const pt::CoreType* typ_mk(const pt::CoreTypeDesc* d, const Location& loc,
                                  pt::Attributes attrs = {}) {
  return make<pt::CoreType>(d, loc, pt::LocationStack{}, attrs);
}

inline const pt::Expression* exp_constraint(const Location& loc, const pt::Expression* e,
                                            const pt::CoreType* t) {
  return exp_mk(make<pt::Pexp_constraint>(pt::Pexp_constraint{{pt::ExpressionDesc::Kind::Pexp_constraint}, e, t}), loc);
}
inline const pt::Expression* exp_coerce(const Location& loc, const pt::Expression* e,
                                        const pt::CoreType* from, const pt::CoreType* to) {
  return exp_mk(make<pt::Pexp_coerce>(pt::Pexp_coerce{{pt::ExpressionDesc::Kind::Pexp_coerce}, e, from, to}), loc);
}
inline const pt::Expression* exp_newtype(const Location& loc, const pt::StrLoc& name,
                                         const pt::Expression* e) {
  return exp_mk(make<pt::Pexp_newtype>(pt::Pexp_newtype{{pt::ExpressionDesc::Kind::Pexp_newtype}, name, e}), loc);
}
inline const pt::Expression* exp_ident(const Location& loc, const pt::LidLoc& lid) {
  return exp_mk(make<pt::Pexp_ident>(pt::Pexp_ident{{pt::ExpressionDesc::Kind::Pexp_ident}, lid}), loc);
}
inline const pt::Pattern* pat_constraint(const Location& loc, const pt::Pattern* p,
                                         const pt::CoreType* t) {
  return pat_mk(make<pt::Ppat_constraint>(pt::Ppat_constraint{{pt::PatternDesc::Kind::Ppat_constraint}, p, t}), loc);
}
inline const pt::Pattern* pat_any(const Location& loc) {
  return pat_mk(make<pt::Ppat_any>(pt::PatternDesc::Kind::Ppat_any), loc);
}
inline const pt::CoreType* typ_poly(const Location& loc, Slice<pt::StrLoc> vars, const pt::CoreType* t) {
  return typ_mk(make<pt::Ptyp_poly>(pt::Ptyp_poly{{pt::CoreTypeDesc::Kind::Ptyp_poly}, vars, t}), loc);
}
inline const pt::CoreType* typ_package(const Location& loc, const pt::PackageType* p) {
  return typ_mk(make<pt::Ptyp_package>(pt::Ptyp_package{{pt::CoreTypeDesc::Kind::Ptyp_package}, p}), loc);
}

// Typ.varify_constructors (raises the Variable_in_scope syntax error, see
// typecore.hpp)
const pt::CoreType* varify_constructors(Slice<pt::StrLoc> var_names, const pt::CoreType* t);

}  // namespace cppcaml::typing::ast_helper
