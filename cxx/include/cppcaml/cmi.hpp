// Interprets a decoded Marshal arena as (a subset of) typing/types.mli:
// signatures, value descriptions, and the type_expr graph.  This is the data
// the type-checker's Env is built from when a .cmi is loaded.
//
// Decoding is lazy and memoized by arena id: a shared/cyclic type_expr graph
// (recursive types, unification links) maps to a shared/cyclic C++ graph
// without infinite recursion, and a caller that only needs one value's type
// only pays to decode that subgraph.
#pragma once

#include "cppcaml/marshal.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cppcaml::cmi {

// Ident.t (typing/ident.ml): name is always field 0 in every variant.
struct Ident {
  enum Kind { Local = 0, Scoped = 1, Global = 2, Predef = 3, Unscoped = 4 };
  Kind kind = Local;
  std::string name;
  long long stamp = 0;
};

struct Path;
using PathPtr = std::shared_ptr<Path>;

// Path.t (typing/path.ml).
struct Path {
  enum Kind { Pident = 0, Pdot = 1, Papply = 2, Pextra_ty = 3 };
  Kind kind = Pident;
  Ident id;          // Pident
  PathPtr a, b;      // Pdot: a.s ; Papply: a(b) ; Pextra_ty: a
  std::string s;     // Pdot field
};

struct TypeExpr;
using TypePtr = std::shared_ptr<TypeExpr>;

// type_desc (typing/types.mli), this tree's constructor order.  Only the cases
// reachable from the values we currently decode are fully populated; the rest
// record their kind so printing degrades gracefully rather than crashing.
struct TypeExpr {
  enum Kind {
    Tvar = 100, Tarrow, Ttuple, Tconstr, Tobject, Tfield, Tnil, Tvariant,
    Tunivar, Tpoly, Tpackage, Tfunctor, Texpand, Tlink, Tsubst, Other
  };
  Kind kind = Other;

  std::optional<std::string> name;  // Tvar / Tunivar

  // Tarrow: arg_label (0 Nolabel / 1 Labelled / 2 Optional) + label string.
  int label_kind = 0;
  std::string label;
  TypePtr dom, cod;

  std::vector<std::pair<std::optional<std::string>, TypePtr>> elems;  // Ttuple

  PathPtr path;                 // Tconstr / Texpand
  std::vector<TypePtr> args;    // Tconstr / Texpand

  TypePtr link;                 // Tlink / Tsubst indirection
};

struct SigValue {
  std::string name;
  TypePtr type;
  // For a Val_prim value (`external x = "%op"` / C primitive), its prim_name
  // (e.g. "%compare", "caml_format_int"); empty for an ordinary Val_reg value.
  std::string prim;
  int prim_arity = 0;  // Primitive.description prim_arity (number of arguments)
};

// label_declaration / constructor_declaration (typing/types.mli).
struct LabelDecl {
  std::string name;
  bool mutable_ = false;
  TypePtr type;
};

struct ConstructorDecl {
  std::string name;
  std::vector<TypePtr> args;       // Cstr_tuple
  std::vector<LabelDecl> inline_record;  // Cstr_record (inline record args)
  bool is_inline_record = false;
  TypePtr res;                     // cd_res (GADT return type), may be null
};

// type_declaration (the subset Env/typing needs first).
struct TypeDecl {
  std::string name;
  std::vector<TypePtr> params;
  int arity = 0;
  enum Kind { Abstract, Record, Variant, Open, External } kind = Abstract;
  std::vector<LabelDecl> labels;        // Record
  std::vector<ConstructorDecl> ctors;   // Variant
  std::string external_name;            // External
  TypePtr manifest;                     // type_manifest option (abbreviation)
};

struct Signature;
struct ModuleType;
using ModuleTypePtr = std::shared_ptr<ModuleType>;
using SignaturePtr = std::shared_ptr<Signature>;

struct ModuleDecl {
  std::string name;
  ModuleTypePtr type;
};

struct ModtypeDecl {
  std::string name;
  ModuleTypePtr type;  // null => abstract module type
};

// extension_constructor (the typext payload), simplified.
struct ExtConstructor {
  std::string name;
  PathPtr type_path;
  std::vector<TypePtr> args;            // Cstr_tuple
  std::vector<LabelDecl> inline_record; // Cstr_record
  bool is_inline_record = false;
  TypePtr res;                          // ext_ret_type
};

// module_type (typing/types.mli).
struct ModuleType {
  enum Kind { Ident, Sig, Functor, Alias } kind = Ident;
  PathPtr path;                              // Ident / Alias
  SignaturePtr sig;                          // Sig
  bool functor_unit = false;                 // Functor with Unit parameter
  std::optional<std::string> functor_param;  // Functor Named ident (if any)
  ModuleTypePtr functor_param_type;          // Functor parameter signature
  ModuleTypePtr functor_body;                // Functor result
};

// A decoded signature: items grouped by kind (declaration order preserved
// within each group), recursively nesting through module declarations.
struct Signature {
  std::vector<SigValue> values;
  std::vector<TypeDecl> types;
  std::vector<ModuleDecl> modules;
  std::vector<ModtypeDecl> modtypes;
  std::vector<ExtConstructor> typexts;
  // Names of the module's runtime block fields, in order (the Lambda back end
  // needs these to compile a qualified value to `field N (global M!)`): a
  // regular value (Val_reg, not an inlined %/C primitive), an exception, or a
  // submodule takes a field; types/modtypes do not.
  std::vector<std::string> fields;
};

// A loaded .cmi: owns the decoded signature and exposes its bindings.
// Type graphs are decoded on demand.
class CmiFile {
public:
  static CmiFile load(const std::string& path);

  const std::string& module_name() const { return module_name_; }
  const Signature& sig() const { return sig_; }
  const std::vector<SigValue>& values() const { return sig_.values; }
  const std::vector<TypeDecl>& types() const { return sig_.types; }
  const std::vector<ModuleDecl>& modules() const { return sig_.modules; }
  const SigValue* find_value(const std::string& name) const;
  const TypeDecl* find_type(const std::string& name) const;
  const ModuleDecl* find_module(const std::string& name) const;

private:
  std::string module_name_;
  Signature sig_;
};

// Render a type_expr / type declaration / module type in OCaml-ish syntax.
std::string print_type(const TypePtr& t);
std::string print_type_decl(const TypeDecl& d);
std::string print_module_type(const ModuleType& mt);

}  // namespace cppcaml::cmi
