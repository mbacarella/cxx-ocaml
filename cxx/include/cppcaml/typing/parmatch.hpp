// Port of typing/parmatch.ml (TYPECHECKER.md, stage 4c): exhaustiveness and
// usefulness of pattern matching.  Typecore calls check_partial /
// check_unused (whose refutation checks reject programs), pressure_variants
// (which closes polymorphic-variant rows) and pats_of_type.
#pragma once

#include <functional>
#include <vector>

#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::parmatch {

namespace tt = typedtree;

template <class P>
struct ParmatchCase {  // 'pattern parmatch_case
  P pattern;
  bool has_guard;
  bool needs_refute;
};
using TypedCase = ParmatchCase<const tt::Pattern*>;
using UntypedCase = ParmatchCase<const parsetree::Pattern*>;
TypedCase typed_case(const tt::Case* c);
UntypedCase untyped_case(const parsetree::Case* c);

inline constexpr std::string_view some_private_tag = "<some private tag>";

// exception Empty (Empty pattern)
struct Empty {};

// [const_compare c1 c2] compares the actual values represented by [c1] and
// [c2] (MPR#5758)
int const_compare(const tt::Constant& c1, const tt::Constant& c2);
// le_pat p q: forall V, V matches q implies V matches p
bool le_pat(const tt::Pattern* p, const tt::Pattern* q);
bool le_pats(const std::vector<const tt::Pattern*>& ps, const std::vector<const tt::Pattern*>& qs);
// module Compat (Constr) / SyntacticCompat
bool compat_with(const std::function<bool(const ConstructorDescription*, const ConstructorDescription*)>& equal,
                 const tt::Pattern* p, const tt::Pattern* q);
bool compat(const tt::Pattern* p, const tt::Pattern* q);
bool compats(const std::vector<const tt::Pattern*>& ps, const std::vector<const tt::Pattern*>& qs);
// lub p q: a pattern matching the values matched by p and q (raises Empty)
const tt::Pattern* lub(const tt::Pattern* p, const tt::Pattern* q);
std::vector<const tt::Pattern*> lubs(const std::vector<const tt::Pattern*>& ps,
                                     const std::vector<const tt::Pattern*>& qs);
std::vector<const tt::Pattern*> set_args(const tt::Pattern* q, const std::vector<const tt::Pattern*>& r);
const tt::Pattern* pat_of_constr(const tt::Pattern* ex_pat, const ConstructorDescription* cstr);
// complete_constrs {pat_env; pat_desc = cstr} used_constrs
std::vector<const ConstructorDescription*> complete_constrs(
    env::t pat_env, const ConstructorDescription* cstr,
    const std::vector<const ConstructorDescription*>& used_constrs);
std::vector<const ConstructorDescription*> get_variant_constructors(env::t env, TypeExpr* ty);

std::vector<const tt::Pattern*> pats_of_type(env::t env, TypeExpr* ty);
void pressure_variants(env::t env, const std::vector<const tt::Pattern*>& pats);
void pressure_variants_in_computation_pattern(env::t env,
                                              const std::vector<const tt::Pattern*>& pats);
tt::Partial check_partial(const std::function<const tt::Pattern*(const tt::Pattern*)>& pred,
                          const Location& loc, const std::vector<TypedCase>& cases);
void check_unused(const std::function<const tt::Pattern*(bool refute, const tt::Pattern*)>& pred,
                  const std::vector<TypedCase>& cases);
bool irrefutable(const tt::Pattern* p);
bool inactive(tt::Partial partial, const tt::Pattern* p);
void check_ambiguous_bindings(const std::vector<const tt::Case*>& cases);

// creates parmatch.ml's module-initialization values (extra_pat)
void module_init();

}  // namespace cppcaml::typing::parmatch
