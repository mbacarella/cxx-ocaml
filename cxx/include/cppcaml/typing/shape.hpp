// Port of typing/shape.ml (TYPECHECKER.md, stage 5): module shapes.  Shapes
// only feed the cmt file, but Typemod / Includemod build them as OCaml does,
// because the functions that create identifiers (fresh_var,
// leaf_for_unpack) advance the ident stamp counter, which reaches the cmi.
// Shape.Uid is typing::Uid (support.hpp); Uid.Deps (cmt-only) is not ported.
#pragma once

#include <functional>
#include <utility>

#include "cppcaml/typing/path.hpp"
#include "cppcaml/typing/support.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::shape {

enum class SigComponentKind : std::uint8_t {
  Value, Type, Constructor, Label, Module, Module_type, Extension_constructor, Class, Class_type
};
std::string_view to_string(SigComponentKind k);
bool can_appear_in_types(SigComponentKind k);

struct Item {  // string * Sig_component_kind.t
  std::string_view name;
  SigComponentKind kind;
  // the tuple's identity (one per Item.make / Item.value ... call; Map.map
  // and the shape reducer keep the key objects): the .cmt shares them
  const void* obj = nullptr;
};
struct ItemCmp {  // Stdlib.compare on the pair
  int operator()(const Item& a, const Item& b) const;
};
namespace item {
Item make(std::string_view str, SigComponentKind ns);
Item value(Ident::t id);
Item type_(Ident::t id);
Item constr(Ident::t id);
Item label(Ident::t id);
Item module_(Ident::t id);
Item module_type(Ident::t id);
Item extension_constructor(Ident::t id);
Item class_(Ident::t id);
Item class_type(Ident::t id);
}  // namespace item

struct Shape;
using t = const Shape*;
using ItemMap = PMap<Item, t, ItemCmp>;

struct Shape {
  enum class Kind : std::uint8_t { Var, Abs, App, Struct, Pack, Alias, Leaf, Proj, Comp_unit, Error };
  bool has_uid = false;  // uid : Uid.t option
  Uid uid{};
  // the `Some uid` block's identity: one per constructor call that builds
  // it, kept by record copies ({t with ...}) and the shape reducer
  const void* uid_obj = nullptr;
  Kind kind;
  Ident::t var = nullptr;   // Var / Abs / Pack
  t t1 = nullptr;           // Abs body / App fn / Alias / Proj
  t t2 = nullptr;           // App arg
  ItemMap map;              // Struct
  Item item{};              // Proj
  std::string_view str;     // Comp_unit / Error
  bool approximated = false;
};

t strip_head_aliases(t s);
std::pair<Ident::t, t> fresh_var(const Uid& uid, std::string_view name = "shape-var");
Ident::t for_unnamed_functor_param();
t var(const Uid& uid, Ident::t id);
t abs(const Uid* uid, Ident::t var, t body);
t str(const Uid* uid, ItemMap map);
// `str ?uid:src.uid map`: the uid option is src's own block
t str_uid_of(t src, ItemMap map);
t alias(const Uid* uid, t s);
t leaf(const Uid& uid);
t approx(t s);
t proj(const Uid* uid, t s, const Item& item);
t app(const Uid* uid, t f, t arg);
// Some (x, t) | None (nullopt)
std::optional<std::pair<Ident::t, t>> decompose_abs(t s);
t dummy_mod();
t of_path(const std::function<t(SigComponentKind, Ident::t)>& find_shape, SigComponentKind ns, Path::t path);
t for_persistent_unit(std::string_view s);
t leaf_for_unpack();
t set_uid_if_none(t s, const Uid& uid);

// Shape.Map
namespace map {
ItemMap empty();
ItemMap add(ItemMap m, const Item& item, t s);
ItemMap add_value(ItemMap m, Ident::t id, const Uid& uid);
ItemMap add_value_proj(ItemMap m, Ident::t id, t s);
ItemMap add_type(ItemMap m, Ident::t id, t s);
ItemMap add_type_proj(ItemMap m, Ident::t id, t s);
ItemMap add_constr(ItemMap m, Ident::t id, t s);
ItemMap add_constr_proj(ItemMap m, Ident::t id, t s);
ItemMap add_label(ItemMap m, Ident::t id, const Uid& uid);
ItemMap add_label_proj(ItemMap m, Ident::t id, t s);
ItemMap add_module(ItemMap m, Ident::t id, t s);
ItemMap add_module_proj(ItemMap m, Ident::t id, t s);
ItemMap add_module_type(ItemMap m, Ident::t id, const Uid& uid);
ItemMap add_module_type_proj(ItemMap m, Ident::t id, t s);
ItemMap add_extcons(ItemMap m, Ident::t id, t s);
ItemMap add_extcons_proj(ItemMap m, Ident::t id, t s);
ItemMap add_class(ItemMap m, Ident::t id, const Uid& uid);
ItemMap add_class_proj(ItemMap m, Ident::t id, t s);
ItemMap add_class_type(ItemMap m, Ident::t id, const Uid& uid);
ItemMap add_class_type_proj(ItemMap m, Ident::t id, t s);
}  // namespace map

}  // namespace cppcaml::typing::shape
