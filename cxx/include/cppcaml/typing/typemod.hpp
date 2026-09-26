// Port of typing/typemod.ml (TYPECHECKER.md, stage 5): typing of the module
// language (module expressions, module types, signatures, structures,
// compilation units, packages).  Shapes are computed where they create
// idents or uids (their counters reach the .cmi) but are not returned for
// the cmt.  Errors are `typemod::Error` (the OCaml Error.In_context);
// reporting comes with Printtyp.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/shape.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typedtree.hpp"

namespace cppcaml::typing::typemod {

namespace tt = typedtree;
namespace pt = parsetree;
using SigComponentKind = shape::SigComponentKind;

struct HidingError {  // Illegal_shadowing {..} | Appears_in_signature {..}
  enum class Kind { Illegal_shadowing, Appears_in_signature };
  Kind kind;
  Ident::t shadowed_item_id = nullptr;  // Illegal_shadowing
  SigComponentKind shadowed_item_kind{};
  Location shadowed_item_loc;
  Ident::t shadower_id = nullptr;
  Ident::t opened_item_id = nullptr;    // Appears_in_signature
  SigComponentKind opened_item_kind{};
  Ident::t user_id = nullptr;
  SigComponentKind user_kind{};
  Location user_loc;
};

struct Error : std::runtime_error {
  enum class Kind {
    Cannot_apply, Not_included, Cannot_eliminate_dependency, Signature_expected, Structure_expected,
    With_no_component, With_mismatch, With_makes_applicative_functor_ill_typed, With_changes_module_alias,
    With_creates_invalid_aliases, With_cannot_remove_constrained_type, With_package_manifest, Repeated_name,
    Non_generalizable, Non_generalizable_module, Implementation_is_required, Interface_not_compiled,
    Not_allowed_in_functor_body, Not_a_packed_module, Incomplete_packed_module, Scoping_pack,
    Recursive_module_require_explicit_type, Apply_generative, Cannot_scrape_alias, Cannot_scrape_package_type,
    Badly_formed_signature, Cannot_hide_id, Invalid_type_subst_rhs, Non_packable_local_modtype_subst,
    With_cannot_remove_packed_modtype, Cannot_alias, Val_in_structure
  };
  Location loc;
  env::t env;
  Kind kind;
  // payloads (which ones are set depends on the kind)
  const ModuleType* mty = nullptr;
  std::optional<includemod::Explanation> explanation;
  Longident::t lid = nullptr;
  Path::t path = nullptr;
  Path::t path2 = nullptr;
  Ident::t id = nullptr;
  TypeExpr* ty = nullptr;
  SigComponentKind component{};
  std::string name;
  std::vector<TypeExpr*> vars;           // Non_generalizable(_module)
  const ValueDescription* item = nullptr;  // Non_generalizable_module
  std::optional<typedecl::Error> typedecl_error;  // Badly_formed_signature
  std::optional<HidingError> hiding;
  Error(const Location& l, env::t e, Kind k) : std::runtime_error("Typemod.Error"), loc(l), env(e), kind(k) {}
};
// Error_forward of Location.error: an uninterpreted extension node
struct ErrorForward : std::runtime_error {
  const pt::Extension* ext;
  explicit ErrorForward(const pt::Extension* e) : std::runtime_error("Typemod.Error_forward"), ext(e) {}
};

// module Signature_names (abstract): the names already defined by a
// structure / signature, and the items to hide
struct SignatureNames;
Signature simplify(env::t env, SignatureNames* names, Signature sg);

struct TypeStructureResult {
  const tt::Structure* str;
  Signature sg;
  SignatureNames* names;
  env::t env;
  shape::t shape = nullptr;
};
const tt::ModuleExpr* type_module(env::t env, const pt::ModuleExpr* smod);
TypeStructureResult type_structure(env::t env, pt::Structure sstr);
TypeStructureResult type_toplevel_phrase(env::t env, pt::Structure sstr);
const tt::Signature* transl_signature(env::t env, pt::Signature ssg);
void check_nongen_signature(env::t env, Signature sg);
std::pair<Path::t, env::t> type_open_(bool* used_slot, bool toplevel, OverrideFlag ovf, env::t env,
                                      const Location& loc, const pt::LidLoc& lid);
const ModuleType* modtype_of_package(env::t env, const Location& loc, const Package* pack);

// Compilation units (driver side: Unit_info)
struct UnitInfo {
  std::string source_file;   // the .ml
  std::string modname;       // the unit name
  std::string prefix;        // output prefix (no extension)
  bool has_mli = false;      // an .mli was compiled (its .cmi is in [prefix].cmi)
  std::string cmi_file;      // the interface to check against, when has_mli
};
tt::Implementation type_implementation(const UnitInfo& target, env::t initial_env, pt::Structure ast);
const tt::Signature* type_interface(const UnitInfo& target, env::t env, pt::Signature ast);
env::t initial_env(const Location& loc, const std::optional<std::string>& initially_opened_module,
                   const std::vector<std::string>& open_implicit_modules);

// sets the Typecore / Typetexp / Typeclass / Ctype / Env / Mtype forward
// references to this module's functions
void install_forward_refs();

}  // namespace cppcaml::typing::typemod
