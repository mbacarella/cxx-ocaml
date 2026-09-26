// Port of typing/predef.ml.  See predef.hpp.
#include "cppcaml/typing/predef.hpp"

namespace cppcaml::typing::predef {

using namespace types;
using namespace btype;
using TC = TypeConstr;

const std::vector<TypeConstr>& all_type_constrs() {
  static const std::vector<TypeConstr> v = {
      TC::Int, TC::Char, TC::String, TC::Bytes, TC::Float, TC::Bool, TC::Unit, TC::Exn,
      TC::Eff, TC::Continuation, TC::Array, TC::List, TC::Option, TC::Nativeint, TC::Int32,
      TC::Int64, TC::Lazy_t, TC::Extension_constructor, TC::Floatarray, TC::Iarray,
      TC::Atomic_loc, TC::Todo_info};
  return v;
}

bool is_abstract_type_constr(TypeConstr c) {
  switch (c) {
    case TC::Bool: case TC::Unit: case TC::Exn: case TC::Eff: case TC::List: case TC::Option:
      return false;
    default:
      return true;
  }
}

static std::vector<std::pair<std::string_view, Ident::t>> g_builtin_idents;  // reversed

static Ident::t ident_create(std::string_view s) {
  Ident::t id = Ident::create_predef(s);
  g_builtin_idents.emplace_back(id->name_, id);
  return id;
}

// predef.ml's toplevel: the `let .. and ..` groups run left to right.
const Idents& idents() {
  static const Idents ids = [] {
    ZoneScope perm(permanent_zone());
    Idents i;
    i.int_ = ident_create("int");
    i.char_ = ident_create("char");
    i.bytes = ident_create("bytes");
    i.float_ = ident_create("float");
    i.bool_ = ident_create("bool");
    i.unit = ident_create("unit");
    i.exn = ident_create("exn");
    i.eff = ident_create("eff");
    i.continuation = ident_create("continuation");
    i.array = ident_create("array");
    i.list = ident_create("list");
    i.option = ident_create("option");
    i.nativeint = ident_create("nativeint");
    i.int32 = ident_create("int32");
    i.int64 = ident_create("int64");
    i.lazy_t = ident_create("lazy_t");
    i.string = ident_create("string");
    i.extension_constructor = ident_create("extension_constructor");
    i.floatarray = ident_create("floatarray");
    i.iarray = ident_create("iarray");
    i.atomic_loc = ident_create("atomic_loc");
    i.todo_info = ident_create("todo_info");
    i.match_failure = ident_create("Match_failure");
    i.out_of_memory = ident_create("Out_of_memory");
    i.invalid_argument = ident_create("Invalid_argument");
    i.failure = ident_create("Failure");
    i.not_found = ident_create("Not_found");
    i.sys_error = ident_create("Sys_error");
    i.end_of_file = ident_create("End_of_file");
    i.division_by_zero = ident_create("Division_by_zero");
    i.stack_overflow = ident_create("Stack_overflow");
    i.sys_blocked_io = ident_create("Sys_blocked_io");
    i.assert_failure = ident_create("Assert_failure");
    i.undefined_recursive_module = ident_create("Undefined_recursive_module");
    i.continuation_already_taken = ident_create("Continuation_already_taken");
    i.todo = ident_create("Todo");
    i.false_ = ident_create("false");
    i.true_ = ident_create("true");
    i.void_ = ident_create("()");
    i.nil = ident_create("[]");
    i.cons = ident_create("::");
    i.none = ident_create("None");
    i.some = ident_create("Some");
    return i;
  }();
  return ids;
}

Ident::t ident_of_type_constr(TypeConstr c) {
  const Idents& i = idents();
  switch (c) {
    case TC::Int: return i.int_;
    case TC::Char: return i.char_;
    case TC::String: return i.string;
    case TC::Bytes: return i.bytes;
    case TC::Float: return i.float_;
    case TC::Bool: return i.bool_;
    case TC::Unit: return i.unit;
    case TC::Exn: return i.exn;
    case TC::Eff: return i.eff;
    case TC::Continuation: return i.continuation;
    case TC::Array: return i.array;
    case TC::List: return i.list;
    case TC::Option: return i.option;
    case TC::Nativeint: return i.nativeint;
    case TC::Int32: return i.int32;
    case TC::Int64: return i.int64;
    case TC::Lazy_t: return i.lazy_t;
    case TC::Extension_constructor: return i.extension_constructor;
    case TC::Floatarray: return i.floatarray;
    case TC::Iarray: return i.iarray;
    case TC::Atomic_loc: return i.atomic_loc;
    case TC::Todo_info: return i.todo_info;
  }
  return nullptr;
}

std::string_view name_of_type_constr(TypeConstr c) {
  return ident::name(ident_of_type_constr(c));
}

const Paths& paths() {
  static const Paths ps = [] {
    ZoneScope perm(permanent_zone());
    const Idents& i = idents();
    Paths p;
    p.int_ = Path::pident(i.int_);
    p.char_ = Path::pident(i.char_);
    p.bytes = Path::pident(i.bytes);
    p.float_ = Path::pident(i.float_);
    p.bool_ = Path::pident(i.bool_);
    p.unit = Path::pident(i.unit);
    p.exn = Path::pident(i.exn);
    p.eff = Path::pident(i.eff);
    p.continuation = Path::pident(i.continuation);
    p.array = Path::pident(i.array);
    p.list = Path::pident(i.list);
    p.option = Path::pident(i.option);
    p.nativeint = Path::pident(i.nativeint);
    p.int32 = Path::pident(i.int32);
    p.int64 = Path::pident(i.int64);
    p.lazy_t = Path::pident(i.lazy_t);
    p.string = Path::pident(i.string);
    p.extension_constructor = Path::pident(i.extension_constructor);
    p.floatarray = Path::pident(i.floatarray);
    p.iarray = Path::pident(i.iarray);
    p.atomic_loc = Path::pident(i.atomic_loc);
    p.todo_info = Path::pident(i.todo_info);
    p.match_failure = Path::pident(i.match_failure);
    p.assert_failure = Path::pident(i.assert_failure);
    p.undefined_recursive_module = Path::pident(i.undefined_recursive_module);
    p.todo = Path::pident(i.todo);
    return p;
  }();
  return ps;
}

Path::t path_of_type_constr(TypeConstr c) { return Path::pident(ident_of_type_constr(c)); }

std::optional<TypeConstr> find_type_constr(Path::t p) {
  for (TypeConstr c : all_type_constrs())
    if (path::compare(path_of_type_constr(c), p) == 0) return c;
  return std::nullopt;
}

static TypeExpr* tconstr_(Path::t p, std::vector<TypeExpr*> args) {
  return newgenty(tconstr(p, slice(args), make<MemoRef>(mnil())));
}

// The nullary ones are shared values in predef.ml (`let type_int = ...`),
// created together in the `let .. and ..` group's order.
struct Nullary {
  TypeExpr *int_, *char_, *bytes, *float_, *bool_, *unit, *exn, *nativeint, *int32, *int64,
      *string, *extension_constructor, *floatarray, *todo_info;
};
static const Nullary& nullary() {
  static const Nullary n = [] {
    ZoneScope perm(permanent_zone());
    const Paths& p = paths();
    Nullary x;
    x.int_ = tconstr_(p.int_, {});
    x.char_ = tconstr_(p.char_, {});
    x.bytes = tconstr_(p.bytes, {});
    x.float_ = tconstr_(p.float_, {});
    x.bool_ = tconstr_(p.bool_, {});
    x.unit = tconstr_(p.unit, {});
    x.exn = tconstr_(p.exn, {});
    x.nativeint = tconstr_(p.nativeint, {});
    x.int32 = tconstr_(p.int32, {});
    x.int64 = tconstr_(p.int64, {});
    x.string = tconstr_(p.string, {});
    x.extension_constructor = tconstr_(p.extension_constructor, {});
    x.floatarray = tconstr_(p.floatarray, {});
    x.todo_info = tconstr_(p.todo_info, {});
    return x;
  }();
  return n;
}

TypeExpr* type_int() { return nullary().int_; }
TypeExpr* type_char() { return nullary().char_; }
TypeExpr* type_bytes() { return nullary().bytes; }
TypeExpr* type_float() { return nullary().float_; }
TypeExpr* type_bool() { return nullary().bool_; }
TypeExpr* type_unit() { return nullary().unit; }
TypeExpr* type_exn() { return nullary().exn; }
TypeExpr* type_nativeint() { return nullary().nativeint; }
TypeExpr* type_int32() { return nullary().int32; }
TypeExpr* type_int64() { return nullary().int64; }
TypeExpr* type_string() { return nullary().string; }
TypeExpr* type_extension_constructor() { return nullary().extension_constructor; }
TypeExpr* type_floatarray() { return nullary().floatarray; }
TypeExpr* type_todo_info() { return nullary().todo_info; }
TypeExpr* type_eff(TypeExpr* t) { return tconstr_(paths().eff, {t}); }
TypeExpr* type_continuation(TypeExpr* t1, TypeExpr* t2) {
  return tconstr_(paths().continuation, {t1, t2});
}
TypeExpr* type_array(TypeExpr* t) { return tconstr_(paths().array, {t}); }
TypeExpr* type_list(TypeExpr* t) { return tconstr_(paths().list, {t}); }
TypeExpr* type_option(TypeExpr* t) { return tconstr_(paths().option, {t}); }
TypeExpr* type_lazy_t(TypeExpr* t) { return tconstr_(paths().lazy_t, {t}); }
TypeExpr* type_iarray(TypeExpr* t) { return tconstr_(paths().iarray, {t}); }
TypeExpr* type_atomic_loc(TypeExpr* t) { return tconstr_(paths().atomic_loc, {t}); }

const std::vector<Ident::t>& all_predef_exns() {
  static const std::vector<Ident::t> v = [] {
    const Idents& i = idents();
    return std::vector<Ident::t>{
        i.match_failure, i.out_of_memory, i.invalid_argument, i.failure, i.not_found,
        i.sys_error, i.end_of_file, i.division_by_zero, i.stack_overflow, i.sys_blocked_io,
        i.assert_failure, i.undefined_recursive_module, i.continuation_already_taken, i.todo};
  }();
  return v;
}

static const ConstructorDeclaration* cstr(Ident::t id, std::vector<TypeExpr*> args) {
  ConstructorArguments a;
  a.tuple = slice(args);
  return make<ConstructorDeclaration>(id, a, nullptr, location::none(), Attributes{},
                                      uid::of_predef_id(ident::name(id)));
}

static const TypeKind* variant(std::vector<const ConstructorDeclaration*> cs) {
  auto* k = make<TypeKind>();
  k->kind = TypeKind::Kind::Type_variant;
  k->constructors = slice(cs);
  k->variant_repr = VariantRepresentation::Variant_regular;
  return k;
}

const TypeDeclaration* decl_of_type_constr(TypeConstr c) {
  std::string_view name = name_of_type_constr(c);
  Uid type_uid = uid::of_predef_id(ident::name(ident_of_type_constr(c)));
  auto external = [&] {
    auto* k = make<TypeKind>();
    k->kind = TypeKind::Kind::Type_external;
    k->external = zborrow(name);
    return static_cast<const TypeKind*>(k);
  };
  auto decl0 = [&](TypeImmediacy immediate, const TypeKind* kind) {
    return make<TypeDeclaration>(Slice<TypeExpr*>{}, 0L, kind, PrivateFlag::Public, nullptr,
                                 Slice<variance::t>{}, Slice<Separability>{}, false, lowest_level,
                                 location::none(), Attributes{}, immediate, false, type_uid);
  };
  auto decl1 = [&](variance::t var, const std::function<const TypeKind*(TypeExpr*)>& kind) {
    TypeExpr* param = newgenvar();
    auto* d = decl0(TypeImmediacy::Unknown, kind(param));
    d->type_params = slice({param});
    d->type_arity = 1;
    d->type_variance = slice({var});
    d->type_separability = slice({Separability::Ind});
    return d;
  };
  auto ext1 = [&](TypeExpr*) { return external(); };
  switch (c) {
    case TC::Int:
    case TC::Char:
      return decl0(TypeImmediacy::Always, external());
    case TC::String: case TC::Bytes: case TC::Float: case TC::Floatarray: case TC::Nativeint:
    case TC::Int32: case TC::Int64: case TC::Extension_constructor: case TC::Todo_info:
      return decl0(TypeImmediacy::Unknown, external());
    case TC::Bool: {
      const Idents& i = idents();
      return decl0(TypeImmediacy::Always, variant({cstr(i.false_, {}), cstr(i.true_, {})}));
    }
    case TC::Unit:
      return decl0(TypeImmediacy::Always, variant({cstr(idents().void_, {})}));
    case TC::Exn: {
      auto* k = make<TypeKind>();
      k->kind = TypeKind::Kind::Type_open;
      return decl0(TypeImmediacy::Unknown, k);
    }
    case TC::Eff:
      return decl1(variance::full(), [](TypeExpr*) {
        auto* k = make<TypeKind>();
        k->kind = TypeKind::Kind::Type_open;
        return static_cast<const TypeKind*>(k);
      });
    case TC::Continuation: {
      // `let param1, param2 = newgenvar (), newgenvar ()`: a tuple, built
      // right to left.
      TypeExpr* param2 = newgenvar();
      TypeExpr* param1 = newgenvar();
      auto* d = decl0(TypeImmediacy::Unknown, external());
      d->type_params = slice({param1, param2});
      d->type_arity = 2;
      d->type_variance = slice({variance::contravariant(), variance::covariant()});
      d->type_separability = slice({Separability::Ind, Separability::Ind});
      return d;
    }
    case TC::Array:
    case TC::Atomic_loc:
      return decl1(variance::full(), ext1);
    case TC::Iarray:
      return decl1(variance::covariant(), ext1);
    case TC::List:
      return decl1(variance::covariant(), [](TypeExpr* tvar) {
        const Idents& i = idents();
        // `variant [cstr nil []; cstr cons [tvar; type_list tvar]]`: the list
        // is built right to left
        auto* cons = cstr(i.cons, {tvar, type_list(tvar)});
        auto* nil = cstr(i.nil, {});
        return variant({nil, cons});
      });
    case TC::Option:
      return decl1(variance::covariant(), [](TypeExpr* tvar) {
        const Idents& i = idents();
        auto* some = cstr(i.some, {tvar});
        auto* none = cstr(i.none, {});
        return variant({none, some});
      });
    case TC::Lazy_t:
      return decl1(variance::covariant(), ext1);
  }
  return nullptr;
}

const ExtensionConstructor* predef_extension(Ident::t id, Slice<TypeExpr*> args) {
  ConstructorArguments a;
  a.tuple = args;
  // [Ast_helper.Attr.mk (mknoloc "ocaml.warn_on_literal_pattern") (PStr [])]
  auto* pstr_nil = make<OValue>(OValue::Kind::Block, 0L, std::string_view{}, 0.0, 0u);
  pstr_nil->fields = slice({static_cast<const OValue*>(make<OValue>(OValue::Kind::Int, 0L))});
  auto* attr = make<Attribute>(zborrow("ocaml.warn_on_literal_pattern"), location::none(),
                               pstr_nil, location::none());
  return make<ExtensionConstructor>(paths().exn, Slice<TypeExpr*>{}, a, nullptr,
                                    PrivateFlag::Public, location::none(),
                                    slice({static_cast<const Attribute*>(attr)}),
                                    uid::of_predef_id(ident::name(id)));
}

std::vector<std::pair<Ident::t, std::function<Slice<TypeExpr*>()>>> initial_extensions() {
  const Idents& i = idents();
  auto triple = [] {
    std::vector<LabeledTy> l = {{OptStr::none(), type_string()},
                                {OptStr::none(), type_int()},
                                {OptStr::none(), type_int()}};
    return slice({newgenty(ttuple(slice(l)))});
  };
  auto none = [] { return Slice<TypeExpr*>{}; };
  auto str = [] { return slice({type_string()}); };
  std::vector<std::pair<Ident::t, std::function<Slice<TypeExpr*>()>>> v;
  v.emplace_back(i.assert_failure, triple);
  v.emplace_back(i.division_by_zero, none);
  v.emplace_back(i.end_of_file, none);
  v.emplace_back(i.failure, str);
  v.emplace_back(i.invalid_argument, str);
  v.emplace_back(i.match_failure, triple);
  v.emplace_back(i.not_found, none);
  v.emplace_back(i.out_of_memory, none);
  v.emplace_back(i.stack_overflow, none);
  v.emplace_back(i.sys_blocked_io, none);
  v.emplace_back(i.sys_error, str);
  v.emplace_back(i.todo, [] { return slice({tconstr_(paths().todo_info, {})}); });
  v.emplace_back(i.undefined_recursive_module, triple);
  v.emplace_back(i.continuation_already_taken, none);
  return v;
}

void init() {
  idents();
  paths();
  nullary();
  all_predef_exns();
}

std::vector<std::pair<std::string_view, Ident::t>> builtin_values() {
  std::vector<std::pair<std::string_view, Ident::t>> v;
  for (Ident::t id : all_predef_exns()) v.emplace_back(ident::name(id), id);
  return v;
}

std::vector<std::pair<std::string_view, Ident::t>> builtin_idents() {
  idents();
  return g_builtin_idents;  // List.rev of the consed list = creation order
}

}  // namespace cppcaml::typing::predef
