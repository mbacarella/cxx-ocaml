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

// Non-owning handle to a node of the decoded cmi graph (Path / TypeExpr /
// ModuleType / Signature).  Every node lives in a process-lifetime bump arena
// (see cmi.cpp) that is never freed mid-compile: a decoded .cmi stays in the
// load cache for the whole process, so ownership is meaningless and no
// consumer mutates a decoded graph.  Dropping shared_ptr removes the atomic
// refcount writes on every handle copy -- a prerequisite for mapping a
// pre-decoded graph read-only (refcounts would write into the mapped pages).
// The interface mirrors the shared_ptr subset the codebase actually uses
// (operator->/*/bool/==/</get/reset, null default-construction) so use sites
// compile unchanged; only the factory sites changed (*_alloc below).
template <class T>
struct GraphPtr {
  T* p_ = nullptr;
  constexpr GraphPtr() noexcept = default;
  constexpr GraphPtr(std::nullptr_t) noexcept {}
  explicit constexpr GraphPtr(T* p) noexcept : p_(p) {}
  T* operator->() const noexcept { return p_; }
  T& operator*() const noexcept { return *p_; }
  T* get() const noexcept { return p_; }
  explicit constexpr operator bool() const noexcept { return p_ != nullptr; }
  void reset() noexcept { p_ = nullptr; }
  friend constexpr bool operator==(GraphPtr a, GraphPtr b) noexcept { return a.p_ == b.p_; }
  friend constexpr bool operator!=(GraphPtr a, GraphPtr b) noexcept { return a.p_ != b.p_; }
  friend constexpr bool operator<(GraphPtr a, GraphPtr b) noexcept { return a.p_ < b.p_; }
};

// Ident.t (typing/ident.ml): name is always field 0 in every variant.
struct Ident {
  enum Kind { Local = 0, Scoped = 1, Global = 2, Predef = 3, Unscoped = 4 };
  Kind kind = Local;
  std::string name;
  long long stamp = 0;
};

struct Path;
using PathPtr = GraphPtr<Path>;

// Path.t (typing/path.ml).
struct Path {
  enum Kind { Pident = 0, Pdot = 1, Papply = 2, Pextra_ty = 3 };
  Kind kind = Pident;
  Ident id;          // Pident
  PathPtr a, b;      // Pdot: a.s ; Papply: a(b) ; Pextra_ty: a
  std::string s;     // Pdot field
};

struct TypeExpr;
using TypePtr = GraphPtr<TypeExpr>;

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

  // Tvariant: the directly-named polymorphic-variant tags (row_fields labels),
  // so a `#poly` type pattern over an imported abbreviation resolves its tag set.
  std::vector<std::string> pv_tags;
  // Tvariant full row shape (parallel to pv_tags; pv_args empty on a row that
  // didn't decode cleanly -- consumers must handle the tags-only form):
  std::vector<TypePtr> pv_args;        // tag argument, null = constant tag
  std::vector<char> pv_present;        // RFpresent (vs Reither) per tag
  bool row_closed = false;             // row_desc.row_closed
  bool row_more_nil = false;           // row_more is Tnil (an exact row)
};

// A source Location.t decoded from a cmi (val_loc / type_loc / cd_loc / …), kept
// so an `include`d / functor-applied member re-emits the DEPENDENCY's original
// location (`Set.empty` keeps `set.mli:77:4`).  pos_fname is the same for start
// and end in every real location, so one `fname` suffices.  ghost => the decl
// had Location.none (nothing to preserve).
struct RLoc {
  std::string fname;
  int l_s = 0, b_s = 0, c_s = -1, l_e = 0, b_e = 0, c_e = -1;
  bool ghost = true;
};

struct SigValue {
  std::string name;
  TypePtr type;
  RLoc loc;
  // For a Val_prim value (`external x = "%op"` / C primitive), its prim_name
  // (e.g. "%compare", "caml_format_int"); empty for an ordinary Val_reg value.
  std::string prim;
  int prim_arity = 0;  // Primitive.description prim_arity (number of arguments)
  // Rest of the Primitive.description, in the WRITER's encoding (cmiw::SigItem
  // prim_* fields) so a cmi splice round-trips `external f = "a" "b"
  // [@@unboxed] [@@noalloc]` faithfully: second (native) name, prim_alloc
  // (false = [@@noalloc]), per-arg native_repr codes and the result's.
  std::string prim_native;
  bool prim_alloc = true;
  std::vector<int> prim_reprs;
  int prim_repr_res = 0;
};

// label_declaration / constructor_declaration (typing/types.mli).
struct LabelDecl {
  std::string name;
  bool mutable_ = false;
  TypePtr type;
  RLoc loc;
};

struct ConstructorDecl {
  std::string name;
  std::vector<TypePtr> args;       // Cstr_tuple
  std::vector<LabelDecl> inline_record;  // Cstr_record (inline record args)
  bool is_inline_record = false;
  TypePtr res;                     // cd_res (GADT return type), may be null
  RLoc loc;
};

// type_declaration (the subset Env/typing needs first).
struct TypeDecl {
  std::string name;
  long long stamp = 0;  // the decl's own Ident stamp (Sig_type's ident)
  std::vector<TypePtr> params;
  int arity = 0;
  enum Kind { Abstract, Record, Variant, Open, External } kind = Abstract;
  std::vector<LabelDecl> labels;        // Record
  std::vector<ConstructorDecl> ctors;   // Variant
  std::string external_name;            // External
  // `[@@unboxed]`: a single single-field ctor (Variant_unboxed) or single-field
  // record (Record_unboxed) whose value IS its argument -- no box, no field read.
  bool unboxed = false;
  bool priv = false;                    // type_private = Private
  TypePtr manifest;                     // type_manifest option (abbreviation)
  // type_variance, one RAW Variance.t int per parameter (a bitfield Printtyp
  // renders as `+`/`-`/`!`); empty when the decl predates the decode.
  std::vector<long long> variances;
  RLoc loc;                             // type_loc
};

struct Signature;
struct ModuleType;
using ModuleTypePtr = GraphPtr<ModuleType>;
using SignaturePtr = GraphPtr<Signature>;

// Allocate a graph node from the process-lifetime cmi arena (cmi.cpp).  These
// are the only factories; the graph is built exclusively by the cmi Decoder.
TypePtr type_alloc();
PathPtr path_alloc();
ModuleTypePtr modtype_alloc();
SignaturePtr sig_alloc(Signature&& s);

struct ModuleDecl {
  std::string name;
  ModuleTypePtr type;
  RLoc loc;  // md_loc
};

struct ModtypeDecl {
  std::string name;
  ModuleTypePtr type;  // null => abstract module type
  RLoc loc;  // mtd_loc
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
  // The source-interleaved item order the grouped vectors above lose: one
  // entry per signature item in declaration order, carrying its kind, its
  // index into the per-kind vector, and whether it takes a runtime field
  // (decided at decode time, where val_kind/Mp_absent are visible).  This is
  // what lets a namespaced signature be rebuilt without guessing which
  // namespace a bare `fields` name belongs to (see field_ns in cmi.cpp).
  struct OrderEnt {
    enum Kind : unsigned char { Value, Type, Typext, Module, Modtype } kind;
    int idx;       // index into the corresponding vector
    bool runtime;  // pushed onto `fields`
  };
  std::vector<OrderEnt> order;
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
  static const CmiFile& load(const std::string& path);
  // A lean decode that fills ONLY the top-level type declarations (sig().types),
  // skipping module values and nested-module signatures -- the dominant decode
  // cost.  Used by the labelset index, which walks every in-scope cmi but reads
  // only their records/variants.  Kept in a SEPARATE cache from load() so a
  // partial signature never satisfies a caller that needs the full one.
  static const CmiFile& load_types_only(const std::string& path);
  // Paths of every .cmi that a full load() has decoded so far this compile --
  // i.e. the modules actually referenced (their signatures were needed to
  // type-check).  A module not in this set is not in scope for disambiguation.
  static std::vector<std::string> loaded_paths();

  const std::string& module_name() const { return module_name_; }
  const Signature& sig() const { return sig_; }
  const std::vector<SigValue>& values() const { return sig_.values; }
  const std::vector<TypeDecl>& types() const { return sig_.types; }
  const std::vector<ModuleDecl>& modules() const { return sig_.modules; }
  const SigValue* find_value(const std::string& name) const;
  const TypeDecl* find_type(const std::string& name) const;
  const ModuleDecl* find_module(const std::string& name) const;
  // The module names in this cmi's crc table ("Interfaces imported") -- every
  // interface the unit was type-checked against, i.e. its transitive type
  // dependencies.  Used to scope the labelset index to reachable modules.
  const std::vector<std::string>& imports() const { return imports_; }

private:
  // Decode a parsed cmi (the header value at arena id `header`) with every
  // allocation reachable from the returned CmiFile contained in one private
  // memory region, sealed read-only afterwards -- the in-memory form of the
  // mmap cmi cache (see cmi.cpp).  Returns null when region decode is
  // unavailable or fails; the caller then decodes normally.  Only built when
  // the mimalloc arena API is present (CPPCAML_HAVE_MIMALLOC).
  static const CmiFile* decode_in_region(const std::string& path,
                                         const marshal::Arena& arena,
                                         std::size_t header,
                                         const std::vector<std::string>& imports);
  // Map a previously dumped region blob for `path` back at its recorded fixed
  // address (read-only, zero decode, zero fixup).  Returns null when there is
  // no valid blob (absent, stale, wrong compiler build, address taken).
  static const CmiFile* load_from_blob(const std::string& path);

  std::string module_name_;
  Signature sig_;
  std::vector<std::string> imports_;
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

// Shape.Uid.t (typing/shape.ml).  Every declaration in a cmi carries one: a
// genuinely-new decl in the current unit gets `Item{comp_unit; id; from}` with
// `id` a per-unit counter (0,1,2,.. in typing-traversal order); a decl pulled
// in via include/functor/alias keeps its ORIGINAL uid (its own comp_unit+id),
// read from the source cmi.  `Internal` (the default) marshals as the immediate
// 0 -- what the writer emitted before uids were modelled.
struct Uid {
  enum K { Internal, Item, Predef, CompUnit } k = Internal;
  std::string unit;   // Item: comp_unit;  Predef/CompUnit: the name
  int id = 0;         // Item
  bool intf = true;   // Item: `from` (Intf when compiling a .mli, else Impl)
};

// A declaration's source Location.t, mirroring ast::Location.  file_id resolves
// to a filename at emit time via write_cmi's filename table (0 = source path,
// >0 = a `# N "file"` directive).  Default = ghost -> Location.none (what the
// writer emitted before locations were modelled).
// file_id resolves to a filename via write_cmi's table; but a location READ
// from a dependency's cmi (an `include`d / functor-applied member) carries a
// FOREIGN pos_fname (e.g. "set.mli") that isn't in this unit's table -- store
// it verbatim in `fname` and emit_pos prefers it over the file_id lookup.
struct WPos { int lnum = 0, bol = 0, cnum = -1, file_id = 0; std::string fname; };
struct Loc { WPos start, end; bool ghost = true; };

struct Ty;
using TyPtr = std::shared_ptr<Ty>;
struct Ty {
  enum K { Constr, Arrow, Tuple, Var, Variant, Object, Package, Poly } k = Constr;
  std::string name;            // Constr: type-ctor name ("int","list","M.t",...);
                               // Package: the modtype path ("S", "Set.OrderedType")
  std::vector<TyPtr> args;     // Constr: type args; Arrow: {dom, cod}; Tuple:
                               // elems; Object: method types (parallel to
                               // pv_tags); Variant: per-tag arg types (parallel
                               // to pv_tags, null = constant tag)
  int var = 0;                 // Var: identity within one signature item
  std::string var_name;        // Var: source name (Tvar Some) -- a type decl's
                               // params keep their written names ('outputValue)
  int label_kind = 0;          // Arrow: 0 Nolabel, 1 Labelled, 2 Optional
  std::string label;           // Arrow: label name (Labelled/Optional)
  std::vector<std::string> pv_tags;  // Variant: polymorphic-variant tag names;
                                     // Object: method names
  int row_kind = 2;            // Variant: 0 open `[>`, 1 upper `[<`, 2 exact `[ ]`
  std::vector<std::string> pv_present;  // Variant: `[< L > `P ]` present tags
  std::vector<char> pv_conj;   // Variant: per-tag CONJUNCTIVE-constant flag
                               // (`` `A of & t ``: Reither no_arg=true WITH an
                               // arg list); empty = none
  bool univar = false;         // Var: a universally-quantified var (Tunivar) --
                               // a poly field's `'a.` binder
  std::vector<int> poly_ids;   // Poly: the quantified vars' ids (args[0] = body)
  std::string binder;          // Package: the dependent binder (`(module M : T)`
                               // as a parameter) -- when the codomain cites
                               // `M.t`, the writer emits Tfunctor, not Tarrow
  int engine_stamp = 0;        // Constr: the engine decl's identity stamp
                               // (globally unique across checkers; 0 = none).
                               // Lets the writer cite the RIGHT `t` when a
                               // module shadows an outer decl of the same name
                               // (ocamlc prints the outer one `t/2`).
  std::string row_name;        // Variant: a named row bound (`[< int u]`) -- the
                               // abbreviation's path, emitted as row_desc.row_name
  std::vector<TyPtr> row_name_args;  // = Some(path, args); Printtyp prints
                                     // `[< int u > `A ]` instead of the raw tags
};
TyPtr ty_variant(std::vector<std::string> tags);      // exact all-constant row
TyPtr ty_variant_row(std::vector<std::string> tags, std::vector<TyPtr> args,
                     int row_kind, std::vector<std::string> present);
TyPtr ty_object(std::vector<std::string> names, std::vector<TyPtr> tys);  // closed `< m : t >`
// First-class module `(module S)` / `(module S with type t = u ...)`:
// constraint names in pv_tags (dotted), constraint types in args.
TyPtr ty_package(std::string mty, std::vector<std::string> cnames, std::vector<TyPtr> ctys);
// Explicit polymorphism `'a. t` (a record field / method type): body + the
// quantified vars' ids (the body's matching Var nodes carry univar=true).
TyPtr ty_poly(TyPtr body, std::vector<int> poly_ids);
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
struct Label { std::string name; bool mut = false; bool atomic = false; TyPtr ty; Uid uid; Loc loc; };  // record field
// One member of a class body: a val (mut/virt) or a method (priv/virt).
struct ClassField {
  std::string name; TyPtr ty;
  bool is_method = false, mut = false, virt = false, priv = false;
  // A method whose inferred type IS the object's own self type (`method m =
  // {< >}`): the writer emits the shared csig_self node so Printtyp aliases the
  // self-row proxy and prints `object ('a) .. method m : 'a end`.
  bool self_ref = false;
};
struct Ctor {
  std::string name;
  std::vector<TyPtr> args;           // Cstr_tuple args
  std::vector<Label> inline_record;  // Cstr_record (inline-record ctor); when
                                     // non-empty, takes precedence over args
  TyPtr res;                         // cd_res: GADT return (`Any : 'a -> any`);
                                     // null = ordinary constructor
  Uid uid;
  Loc loc;
};
struct SigItem;
struct SigItem {
  enum K { Value, Type, Module, Modtype, Exception, Class } k = Value;
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
  // Val_prim even when BOTH prim names are empty: `external p : t = ""` is
  // still an external (Printtyp prints it back; it takes no runtime field).
  bool prim_external = false;
  // Val_prim: prim_alloc (false = [@@noalloc]) and the native_repr of each
  // argument / the result.  Codes: 0 Same_as_ocaml_repr, 1 Unboxed_float,
  // 2 Untagged_immediate, 3/4/5 Unboxed_integer int32/int64/nativeint.
  // prim_reprs shorter than the arity is padded with Same_as.
  bool prim_alloc = true;
  std::vector<int> prim_reprs;
  int prim_repr_res = 0;
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
  // A GENERATIVE functor `module F () -> ..`: the parameter is Unit, not Named.
  // Printtyp renders it `()`; a Named-with-empty-sig came out `( : sig end)`.
  bool functor_unit = false;
  std::string functor_param;  // the parameter's name (e.g. "Ord")
  std::vector<SigItem> param_sig;  // the parameter's signature items (OrderedType)
  // Exception: a `type t += ..` extension constructor rather than a plain
  // exception.  ext_path is the extended type's source path ("Effect.t";
  // empty = the predefined exn), ext_params its declared params' source names
  // ("_" for `type _ t +=` -- ocamlc stores Tvar(Some "_"), printed back
  // verbatim), ext_ret the GADT return type (`E : unit Effect.t`; null =
  // none), text_kind the Sig_typext ext_status (0 Text_first / 1 Text_next /
  // 2 Text_exception).
  std::string ext_path;
  std::vector<std::string> ext_params;
  TyPtr ext_ret;
  int text_kind = 2;
  bool type_open = false;     // Type: `type t = ..` (Type_open kind)
  bool type_private = false;  // Type: `type t = private ..`
  // Type: the checker's decl-identity stamp (matches Ty::engine_stamp on
  // Constr nodes citing this decl; 0 = none).  The writer maps it to the
  // emitted Local ident so a shadowed outer decl is cited correctly (`t/2`).
  int engine_stamp = 0;
  // Class: the bridged SELF object node.  Class fields citing it (a method
  // `unit -> 'self`) share this exact Ty node (one BridgeCtx spans the whole
  // class), and the writer maps it to the emitted csig_self, so Printtyp
  // prints `object ('a) .. method m : unit -> 'a end`.
  TyPtr class_self;
  // Class: an ALIAS of another class (`class c = with_param args`): the
  // target path as written.  The writer wraps the signature in
  // Cty_constr(target, [], inner) -- Printtyp prints `class c : with_param`
  // -- and cty_new becomes Tconstr(target's ghost type).
  std::string class_constr_ref;
  // Type: Type_immediacy (0 Unknown / 1 Always / 2 Always_on_64bits) from a
  // `[@@immediate]` / `[@@immediate64]` attribute (Printtyp renders it back).
  int type_immediate = 0;
  // Type: `[@@unboxed]` -- emitted as the Variant_unboxed / Record_unboxed
  // REPRESENTATION (Printtyp derives the printed attr from the representation,
  // not an attribute node).
  bool type_unboxed = false;
  // Type: an EMPTY variant (`type empty = |`) -- Type_variant([]), not abstract.
  bool type_empty_variant = false;
  // Type: params+manifest came from a `with type` constraint whose RHS was
  // written N scope levels OUTSIDE this item's own signature level.  The
  // writer resolves the manifest's bare type names skipping the innermost N
  // scopes, so `Map.S with type key = t` cites the ENCLOSING t, not Map.S's
  // own abstract `t` (self-capture).
  int with_scope_skip = 0;
  // Type: raw Variance.t per parameter (a functor result spliced from a read
  // .cmi keeps `type +!'a t`).  Empty = Variance.unknown (7) for every param.
  std::vector<long long> type_variances;
  // Module: rec_status (0 Trec_not / 1 Trec_first / 2 Trec_next) -- a
  // `module rec A .. and B ..` group prints as such only when marked.
  int rec_status = 0;
  // Functor: curried parameters AFTER the first (`(X : S) (Y : T) -> ..`), in
  // source order.  Parallel arrays (a nested param struct can't hold a vector
  // of the still-incomplete SigItem by name).
  std::vector<std::string> more_param_names;
  std::vector<std::vector<SigItem>> more_param_sigs;
  std::vector<char> more_param_units;
  // Functor: a parameter whose source modtype is a NAMED reference
  // (`(K : Key)`) emits Mty_ident(Key), not the inlined signature -- ocamlc
  // stores (and prints) the name.  Empty = inline param_sig.
  std::string functor_param_ref;
  // Functor: a FIRST parameter that is ITSELF a functor (higher-order,
  // `(MakeDiet : (X : ORD) -> SET with ..)`): 0 or 1 element, a Module
  // SigItem with is_functor describing the parameter's module type; the
  // emitter reuses functor-module emission and takes its md_type.  When
  // non-empty it overrides param_sig/functor_param_ref.
  std::vector<SigItem> param_functor;
  std::vector<std::string> more_param_refs;
  // Module: `module MD5 : S` -- the decl's modtype is the NAMED reference S
  // (Mty_ident), not S's expansion.  Empty = Mty_signature(sub).
  // Modtype: an ALIAS body `module type S2 = S1` / `= M.T` -- mtd_type =
  // Some(Mty_ident); `sub` stays the resolved fallback layout.
  std::string modtype_ref;
  // Functor: a NAMED result modtype (`module F () : Ret`) emits
  // Mty_ident(Ret) as the body; `sub` stays the resolved fallback layout.
  std::string functor_result_ref;
  // Class: `class name : dom1 -> .. -> object <fields> end`.  Emitted as
  // Sig_class followed by its two GHOST companions (Sig_class_type + Sig_type
  // of the same name -- the reader's Signature_group asserts they follow, the
  // printer never shows them).  A Class item takes THREE idents/stamps.
  std::vector<ClassField> class_fields;   // vals + methods, source order
  std::vector<TyPtr> class_params;        // `['a, _] c` type params (cty_params)
  std::vector<TyPtr> class_arrow_doms;    // constructor params, outermost first
  std::vector<int> class_arrow_lks;       // 0 Nolabel / 1 Labelled / 2 Optional
  std::vector<std::string> class_arrow_lbls;
  bool class_virtual = false;             // `class virtual c` -> cty_new = None
  int class_self_param = -1;              // `object (self : 'a)`: the cty_params
                                          // index whose var IS csig_self, so
                                          // Printtyp shows `object ('a) constraint`
  // `class type ct = object .. end`: emit only Sig_class_type + its ghost
  // Sig_type (TWO stamps); arrows/cty_new don't apply.
  bool class_is_type = false;
  // Modtype: an ABSTRACT declaration `module type S` -- mtd_type = None
  // (Printtyp prints it back as bare `module type S`).
  bool modtype_abstract = false;
  // This item's Shape.Uid, assigned by write_cmi's assign_uids pass (kept last
  // so the positional aggregate initialisers above stay valid).
  Uid uid;
  // This declaration's source location (val_loc/type_loc/md_loc/...); ghost by
  // default so unset items emit Location.none as before.
  Loc loc;
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
// An ABSTRACT `module type S` (no body): mtd_type = None.
inline SigItem sig_modtype_abstract(std::string n) {
  SigItem s; s.k = SigItem::Modtype; s.name = std::move(n); s.modtype_abstract = true;
  return s;
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
  s.prim = std::move(prim); s.prim_native = std::move(native);
  s.prim_external = true; return s;
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

// The shadowing NAMESPACE+name of a SigItem, or "" if it doesn't shadow here
// (type / modtype).  Values, modules and exception ctors are SEPARATE
// namespaces.  An `external` is in the VALUE namespace like ocamlc (a
// duplicated `external (@@)` keeps one entry; a val/external pair keeps the
// later) -- it still takes no runtime field, and lambda's modsig push()
// erases same-namespace duplicates identically, so field layouts agree.
inline std::string field_key(const SigItem& s) {
  if (s.k == SigItem::Value) return "v:" + s.name;
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
                      const std::vector<Import>& imports = {}, bool intf = true,
                      const std::vector<std::string>& src_files = {});
// Convenience: a values-only signature.
std::string write_cmi(const std::string& path, const std::string& modname,
                      const std::vector<std::pair<std::string, TyPtr>>& values,
                      const std::vector<Import>& imports = {});

// `ocamlc -pack`: write the packed module's .cmi.  Each member's own .cmi
// signature is reused verbatim as a `module <Member> : sig ... end` entry, so
// the pack's interface is exactly the members wrapped one level deeper.
// `member_cmis` are the members' .cmi paths, in pack order.  Returns the
// self-CRC.
std::string write_packed_cmi(const std::string& path, const std::string& pack_name,
                             const std::vector<std::string>& member_cmis);

// Read a .cmi's whole (interface name, crc) list -- the module itself plus every
// interface it imports (crc empty for a `None`/alias entry).  Drives a .cmo's
// cu_imports for the linker's interface-consistency check.
std::vector<std::pair<std::string, std::string>> read_cmi_crcs(const std::string& path);

// Interface CRC of a compilation-unit global (locate its .cmi, read its self-CRC).
// Empty if not found.  Used to record code-referenced units in cu_imports.
std::string module_cmi_crc(const std::string& mod);

}  // namespace cmiw

}  // namespace cppcaml::cmi
