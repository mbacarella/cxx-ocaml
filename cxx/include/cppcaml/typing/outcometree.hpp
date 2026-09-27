// Port of typing/outcometree.mli (TYPECHECKER.md stage 9): the trees the
// printers (Oprint) lay out -- what Out_type builds from types and
// signatures.  Nodes are zone-allocated; lists are vectors.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cppcaml/typing/support.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::outcometree {

// out_name: printed names may be rewritten on the fly (a mutable record)
struct OutName {
  std::string printed_name;
};

struct OutIdent {
  enum class K : std::uint8_t { Oide_apply, Oide_dot, Oide_ident } k;
  const OutIdent* a = nullptr;  // Oide_apply: functor; Oide_dot: the prefix
  const OutIdent* b = nullptr;  // Oide_apply: argument
  std::string s;                // Oide_dot: the component
  OutName* name = nullptr;      // Oide_ident
};
const OutIdent* oide_ident(OutName* n);
const OutIdent* oide_dot(const OutIdent* p, std::string s);
const OutIdent* oide_apply(const OutIdent* f, const OutIdent* x);
OutName* out_name_create(std::string s);  // Out_name.create

struct OutAttribute {
  std::string oattr_name;
};

// Asttypes.variance / injectivity
enum class Variance : std::uint8_t { Covariant, Contravariant, NoVariance, Bivariant };
enum class Injectivity : std::uint8_t { Injective, NoInjectivity };

struct OutTypeParam {
  bool ot_non_gen;
  std::string ot_name;
  std::pair<Variance, Injectivity> ot_variance;
};

struct OutType;
struct OutLabel;
struct OutConstructor;
struct OutPackage;

// Asttypes.arg_label
struct ArgLabel {
  enum class K : std::uint8_t { Nolabel, Labelled, Optional } k = K::Nolabel;
  std::string s;
};

enum class OutRowK : std::uint8_t { Orow_closed, Orow_open_anonymous, Orow_open };
struct OutRow {
  OutRowK k = OutRowK::Orow_closed;
  const OutType* ty = nullptr;  // Orow_open
  bool operator==(const OutRow& o) const { return k == o.k && ty == o.ty; }
};

struct OutLabel {
  std::string olab_name;
  bool olab_mutable;  // Mutable | Immutable
  bool olab_atomic;   // Atomic | Nonatomic
  const OutType* olab_type;
};

struct OutConstructor {
  std::string ocstr_name;
  std::vector<const OutType*> ocstr_args;
  const OutType* ocstr_return_type = nullptr;  // option
};

struct OutPackage {
  const OutIdent* opack_path;
  std::vector<std::pair<std::string, const OutType*>> opack_constraints;
};

struct OutVariant {  // Ovar_fields of (string * bool * out_type list) list | Ovar_typ of out_type
  bool is_typ = false;
  struct Field {
    std::string label;
    bool amp;
    std::vector<const OutType*> tys;
  };
  std::vector<Field> fields;
  const OutType* typ = nullptr;
};

struct OutType {
  enum class K : std::uint8_t {
    Otyp_abstract, Otyp_open, Otyp_alias, Otyp_arrow, Otyp_class, Otyp_constr, Otyp_manifest, Otyp_object,
    Otyp_record, Otyp_stuff, Otyp_sum, Otyp_tuple, Otyp_var, Otyp_variant, Otyp_poly, Otyp_module,
    Otyp_attribute, Otyp_external, Otyp_functor
  } k;
  // Otyp_alias {non_gen; aliased; alias}; Otyp_var (non_gen, name)
  bool non_gen = false;
  const OutType* t1 = nullptr;  // alias: aliased; arrow: arg; manifest: first; poly: body; attribute: type;
                                // functor: result
  const OutType* t2 = nullptr;  // arrow: result; manifest: second
  std::string s;                // alias: the alias; stuff; var: the name; external: the name
  ArgLabel label;               // arrow / functor
  const OutIdent* id = nullptr;       // class / constr / functor: the parameter name
  std::vector<const OutType*> args;   // class / constr
  std::vector<std::pair<std::string, const OutType*>> fields;  // object
  OutRow row;                                                  // object
  std::vector<OutLabel> labels;                                // record
  std::vector<OutConstructor> constrs;                         // sum
  std::vector<std::pair<std::optional<std::string>, const OutType*>> tuple;  // tuple
  OutVariant variant;                                          // variant
  bool closed = false;                                         // variant
  std::optional<std::vector<std::string>> tags;                // variant
  std::vector<std::string> vars;                               // poly
  const OutPackage* pack = nullptr;                            // module / functor
  OutAttribute attr;                                           // attribute
};

OutType* otyp(OutType::K k);
bool equal(const OutType* a, const OutType* b);  // structural (=)

// ---- class types ----
struct OutClassSigItem;
struct OutClassType {
  enum class K : std::uint8_t { Octy_constr, Octy_arrow, Octy_signature } k;
  const OutIdent* id = nullptr;             // constr
  std::vector<const OutType*> tys;          // constr
  ArgLabel label;                           // arrow
  const OutType* ty = nullptr;              // arrow: the argument; signature: the self type (option)
  const OutClassType* cty = nullptr;        // arrow
  std::vector<const OutClassSigItem*> csil;  // signature
};
struct OutClassSigItem {
  enum class K : std::uint8_t { Ocsg_constraint, Ocsg_method, Ocsg_value } k;
  const OutType* t1 = nullptr;  // constraint (t1, t2); method / value: the type
  const OutType* t2 = nullptr;
  std::string name;
  bool b1 = false;  // method: private; value: mutable
  bool b2 = false;  // virtual
};

// ---- module types, signatures ----
enum class OutRecStatus : std::uint8_t { Orec_not, Orec_first, Orec_next };
enum class OutExtStatus : std::uint8_t { Oext_first, Oext_next, Oext_exception };

struct OutSigItem;
struct OutModuleType;
// (string option * out_module_type) option: None is the unit parameter "()"
struct OutFunctorParam {
  bool some = false;
  std::optional<std::string> name;
  const OutModuleType* mty = nullptr;
};
struct OutModuleType {
  enum class K : std::uint8_t { Omty_abstract, Omty_functor, Omty_ident, Omty_signature, Omty_alias } k;
  // Omty_functor of (string option * out_module_type) option * out_module_type
  OutFunctorParam param;
  const OutModuleType* res = nullptr;
  const OutIdent* id = nullptr;           // ident / alias
  std::vector<const OutSigItem*> sg;      // signature
};

enum class TypeImmediacyO : std::uint8_t { Unknown, Always, Always_on_64bits };

struct OutTypeDecl {
  std::string otype_name;
  std::vector<OutTypeParam> otype_params;
  const OutType* otype_type;
  bool otype_private;  // Private | Public
  TypeImmediacyO otype_immediate;
  bool otype_unboxed;
  std::vector<std::pair<const OutType*, const OutType*>> otype_constraints;
};

struct OutExtensionConstructor {
  std::string oext_name;
  std::string oext_type_name;
  std::vector<OutTypeParam> oext_type_params;
  std::vector<const OutType*> oext_args;
  const OutType* oext_ret_type = nullptr;
  bool oext_private;
};

struct OutTypeExtension {
  std::string otyext_name;
  std::vector<OutTypeParam> otyext_params;
  std::vector<OutConstructor> otyext_constructors;
  bool otyext_private;
};

struct OutValDecl {
  std::string oval_name;
  const OutType* oval_type;
  std::vector<std::string> oval_prims;
  std::vector<OutAttribute> oval_attributes;
};

struct OutSigItem {
  enum class K : std::uint8_t {
    Osig_class, Osig_class_type, Osig_typext, Osig_modtype, Osig_module, Osig_type, Osig_value, Osig_ellipsis
  } k;
  // class / class type: (virtual, name, params, class type, rec)
  bool virt = false;
  std::string name;  // class / class type / modtype / module
  std::vector<OutTypeParam> params;
  const OutClassType* clt = nullptr;
  OutRecStatus rs = OutRecStatus::Orec_not;
  const OutExtensionConstructor* ext = nullptr;  // typext
  OutExtStatus es = OutExtStatus::Oext_first;
  const OutModuleType* mty = nullptr;  // modtype / module
  const OutTypeDecl* td = nullptr;     // type
  const OutValDecl* vd = nullptr;      // value
};

}  // namespace cppcaml::typing::outcometree
