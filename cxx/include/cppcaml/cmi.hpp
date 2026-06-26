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

// A computed module coercion, Includemod-style: how to build a value of the
// TARGET signature from the SOURCE module's runtime block.  The back end REPLAYS
// this (target field i <- source field `fields[i].src_field`, recursively coercing
// a submodule through `sub`) instead of reconstructing field layouts heuristically
// -- so it can never project a wrong/absent slot.  `ok` is false (with `error`
// naming the offending member) when the source does not match the signature, i.e.
// an OCaml signature mismatch.
struct ModCoercion {
  struct Field {
    int src_field = -1;                // index in the SOURCE runtime block
    std::shared_ptr<ModCoercion> sub;  // null = identity; else coerce this submodule
  };
  std::vector<Field> fields;  // one per TARGET runtime field, in target order
  bool identity = false;      // source already matches target field-for-field
  bool ok = true;             // false => a required member was absent / ambiguous
  std::string error;          // the offending member path (when !ok)
};
// Match `src` against `tgt` by (namespace, name), recursively for submodules,
// yielding the position mapping.  Pure: depends only on the two signatures.
ModCoercion compute_coercion(const Signature& src, const Signature& tgt);

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

// --- .cmi WRITER (inverse of CmiFile::load) -------------------------------
// A minimal type descriptor for the values a module exports, enough to build a
// valid Types.signature the oracle reads back.  Grown construct by construct
// (predefined constructors first, then arrows / tuples / variables).
namespace cmiw {

struct Ty;
using TyPtr = std::shared_ptr<Ty>;
struct Ty {
  enum K { Constr, Arrow, Tuple, Var } k = Constr;
  std::string name;            // Constr: type-ctor name ("int","list","M.t",...)
  std::vector<TyPtr> args;     // Constr: type args; Arrow: {dom, cod}; Tuple: elems
  int var = 0;                 // Var: identity within one signature item
  int label_kind = 0;          // Arrow: 0 Nolabel, 1 Labelled, 2 Optional
  std::string label;           // Arrow: label name (Labelled/Optional)
};
TyPtr ty_predef(const std::string& name);             // nullary predef constr
TyPtr ty_constr(const std::string& name, std::vector<TyPtr> args);
TyPtr ty_arrow(const TyPtr& dom, const TyPtr& cod);
TyPtr ty_arrow_lbl(const TyPtr& dom, const TyPtr& cod, int label_kind,
                   const std::string& label);
TyPtr ty_tuple(std::vector<TyPtr> elems);
TyPtr ty_var(int id);

// An interface import (module name + its .cmi's BLAKE128 CRC).
struct Import { std::string name; std::string crc; };

// One signature item, in source order.  A Type item emits Sig_type (it takes no
// runtime field, so it doesn't shift the value field layout the .cmo expects);
// a Value item emits Sig_value.
struct Label { std::string name; bool mut = false; TyPtr ty; };  // record field
struct Ctor {
  std::string name;
  std::vector<TyPtr> args;           // Cstr_tuple args
  std::vector<Label> inline_record;  // Cstr_record (inline-record ctor); when
                                     // non-empty, takes precedence over args
};
struct SigItem;
struct SigItem {
  enum K { Value, Type, Module, Modtype, Exception } k = Value;
  std::string name;
  TyPtr ty;                   // Value: the value's type
  std::vector<TyPtr> params;  // Type: type parameters (Var descriptors)
  TyPtr manifest;             // Type: null = abstract; else `type name = manifest`
  std::vector<Ctor> ctors;    // Type: non-empty => Type_variant
  std::vector<Label> labels;  // Type: non-empty => Type_record
  // Value: a non-empty prim makes it Val_prim (an `external`) -- inlined as the
  // primitive by consumers and taking NO module field (so it doesn't shift the
  // value field layout).
  std::string prim, prim_native;
  std::vector<SigItem> sub;   // Module: the submodule's signature items; for a
                              // functor (functor_param set) these are the RESULT
                              // signature items (Map.Make's S).
  // Module: if non-empty, this is a module ALIAS `module name = <alias>` (the
  // alias is a compilation-unit global like "Stdlib__List"); emitted as
  // Mty_alias instead of Mty_signature.  `sub` is then ignored.
  std::string alias;
  // Module: if set, this is a FUNCTOR `module name (P : _) : <sub>`; emitted as
  // Mty_functor(Named(P, <opaque>), Mty_signature(sub)).  The param's own
  // signature is left opaque (consumers only need the result layout).
  bool is_functor = false;
  std::string functor_param;  // the parameter's name (e.g. "Ord")
  std::vector<SigItem> param_sig;  // the parameter's signature items (OrderedType)
};
inline SigItem sig_module_functor(std::string n, std::string param,
                                  std::vector<SigItem> param_sig,
                                  std::vector<SigItem> result) {
  SigItem s; s.k = SigItem::Module; s.name = std::move(n);
  s.is_functor = true; s.functor_param = std::move(param);
  s.param_sig = std::move(param_sig); s.sub = std::move(result);
  return s;
}
inline SigItem sig_module(std::string n, std::vector<SigItem> items) {
  SigItem s; s.k = SigItem::Module; s.name = std::move(n); s.sub = std::move(items); return s;
}
// A `module type S = sig .. end` declaration (takes NO runtime field).  Lets a
// functor parameter `(H : Hashtbl.HashedType)` resolve H.equal/H.hash to fields.
inline SigItem sig_modtype(std::string n, std::vector<SigItem> items) {
  SigItem s; s.k = SigItem::Modtype; s.name = std::move(n); s.sub = std::move(items); return s;
}
// An `exception E [of t1 * ..]` declaration.  Emitted as Sig_typext over the
// predefined `exn` type; TAKES A RUNTIME FIELD, so it must appear in the .cmi to
// keep the surrounding value field layout aligned with the .cmo (Parsing's
// Parse_error/YYexit shifted peek_val by 2 -> mis-driven parse engine).  `args`
// holds the constructor argument types (empty for a nullary exception).
inline SigItem sig_exception(std::string n, std::vector<TyPtr> args) {
  SigItem s; s.k = SigItem::Exception; s.name = std::move(n);
  Ctor c; c.name = s.name; c.args = std::move(args); s.ctors.push_back(std::move(c));
  return s;
}
// `exception E of { l1 : t1; .. }`: an inline-record payload.  Emitted as a
// Cstr_record extension_constructor so a consumer matching `M.E {l = ..}` can
// resolve the labels via the typext's inline_record (without it, ext_match bails
// on the inline-record arm and the WHOLE match collapses to its first arm).
inline SigItem sig_exception_record(std::string n, std::vector<Label> labels) {
  SigItem s; s.k = SigItem::Exception; s.name = std::move(n);
  Ctor c; c.name = s.name; c.inline_record = std::move(labels); s.ctors.push_back(std::move(c));
  return s;
}
inline SigItem sig_module_alias(std::string n, std::string target) {
  SigItem s; s.k = SigItem::Module; s.name = std::move(n); s.alias = std::move(target); return s;
}
inline SigItem sig_value(std::string n, TyPtr t) { return {SigItem::Value, std::move(n), std::move(t), {}, nullptr, {}, {}, "", ""}; }
inline SigItem sig_external(std::string n, TyPtr t, std::string prim, std::string native) {
  SigItem s; s.k = SigItem::Value; s.name = std::move(n); s.ty = std::move(t);
  s.prim = std::move(prim); s.prim_native = std::move(native); return s;
}
inline SigItem sig_type(std::string n, std::vector<TyPtr> ps, TyPtr man) {
  return {SigItem::Type, std::move(n), nullptr, std::move(ps), std::move(man), {}};
}
inline SigItem sig_variant(std::string n, std::vector<TyPtr> ps, std::vector<Ctor> cs) {
  return {SigItem::Type, std::move(n), nullptr, std::move(ps), nullptr, std::move(cs), {}};
}
inline SigItem sig_record(std::string n, std::vector<TyPtr> ps, std::vector<Label> ls) {
  return {SigItem::Type, std::move(n), nullptr, std::move(ps), nullptr, {}, std::move(ls)};
}

// The runtime-field NAMESPACE+name of a field-taking SigItem, or "" if it takes no
// field (type / modtype / `external` value).  Values, modules and exception ctors
// are SEPARATE namespaces: a value `x` and a module `x` each take a field.
inline std::string field_key(const SigItem& s) {
  if (s.k == SigItem::Value && s.prim.empty()) return "v:" + s.name;
  if (s.k == SigItem::Module) return "m:" + s.name;
  if (s.k == SigItem::Exception) return "e:" + s.name;
  return "";
}
// Canonical OCaml shadowing dedup: when a field-taking member is declared more than
// once in the same namespace (an `include` then a later decl, or two includes both
// carrying it), keep ONLY the LAST occurrence, at its position.  Used by the .cmi
// writer; the AST-side field layouts in lambda.cpp dedup by the same rule, so every
// computation of a module's field order agrees.
inline std::vector<SigItem> dedup_shadowed_fields(std::vector<SigItem> in) {
  std::unordered_map<std::string, int> last;
  for (int i = 0; i < (int)in.size(); ++i)
    if (std::string k = field_key(in[i]); !k.empty()) last[k] = i;
  bool dup = false;
  for (int i = 0; i < (int)in.size(); ++i)
    if (std::string k = field_key(in[i]); !k.empty() && last[k] != i) { dup = true; break; }
  if (!dup) return in;
  std::vector<SigItem> out; out.reserve(in.size());
  for (int i = 0; i < (int)in.size(); ++i) {
    if (std::string k = field_key(in[i]); !k.empty() && last[k] != i) continue;  // shadowed
    out.push_back(std::move(in[i]));
  }
  return out;
}

// Configure how the writer resolves a referenced module name (`Buffer` in a
// `Buffer.t` type) to its compilation-unit global (`Stdlib__Buffer`) and to its
// .cmi file (for the import CRC).  Mirrors lambda's resolve_cmi/global_of.
void set_module_dirs(const std::string& stdlib_dir,
                     const std::vector<std::string>& dirs);

// Write magic + marshal(name,sign) + BLAKE128 self-CRC + marshal(crcs) +
// marshal(flags) to `path`.  `imports` are the non-self interfaces (self is
// prepended automatically with the computed CRC).  Returns the self-CRC.
std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<SigItem>& items,
                      const std::vector<Import>& imports = {});
// Convenience: a values-only signature.
std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<std::pair<std::string, TyPtr>>& values,
                      const std::vector<Import>& imports = {});

}  // namespace cmiw

}  // namespace cppcaml::cmi
