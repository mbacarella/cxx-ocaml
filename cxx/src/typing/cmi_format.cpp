// Port of file_formats/cmi_format.ml (read side).  See cmi_format.hpp.
//
// The decoder follows each OCaml type's marshaled layout: constant
// constructors are immediates numbered in declaration order, non-constant
// ones are blocks tagged in declaration order, records and inline records are
// blocks of their fields in order, `option` is 0 | Some = block tag 0.
#include "cppcaml/typing/cmi_format.hpp"

#include <optional>
#include <cerrno>
#include <cstring>

#include "cppcaml/typing/arg.hpp"
#include "cppcaml/typing/config.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <tuple>
#include <iterator>
#include <type_traits>
#include <unordered_map>

#include <cstdio>
#include <cstring>

#include "cppcaml/blake2.hpp"
#include "cppcaml/marshal.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/cmi_image.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/instruct.hpp"

#include "cmi_marshal.hpp"
#include "cmi_writer.hpp"

namespace cppcaml::typing::cmi_format {

namespace m = cppcaml::marshal;

Error::Error(Kind k, std::string f, std::string on)
    : std::runtime_error([&] {
        switch (k) {
          case Kind::Not_an_interface: return f + " is not a compiled interface";
          case Kind::Wrong_version_interface:
            return f + " is not a compiled interface for this version of OCaml.\n"
                       "It seems to be for " + on + " version of OCaml.";
          default: return "Corrupted compiled interface " + f;
        }
      }()),
      kind(k), filename(std::move(f)), older_newer(std::move(on)) {}

namespace {

struct Corrupt {};

// The Reader's memos -- one decoded object per marshaled block and kind of
// object -- in one slot per node of the graph: a node is decoded as one kind
// of object, save rare cases (a payload's position record, also an OValue),
// which go to an overflow map.  Values: at most 16 bytes, trivially copyable.
class SlotTable {
 public:
  struct Slot {
    alignas(8) unsigned char val[16];
    std::uint8_t kind = 0;  // 0: empty
  };
  explicit SlotTable(std::size_t n) : slots(n) {}
  std::vector<Slot> slots;
};
template <class V, std::uint8_t K>
class FlatMemo {
  static_assert(sizeof(V) <= 16 && std::is_trivially_copyable_v<V>);

 public:
  explicit FlatMemo(SlotTable& t) : t_(t) {}
  bool get(std::size_t id, V& out) const {
    if (!cmi_marshal::is_imm(id)) {
      const SlotTable::Slot& sl = t_.slots[cmi_marshal::node_index(id)];
      if (sl.kind == K) {
        std::memcpy(&out, sl.val, sizeof(V));
        return true;
      }
      if (sl.kind == 0) return false;
    }
    auto it = over_.find(id);
    if (it == over_.end()) return false;
    out = it->second;
    return true;
  }
  void put(std::size_t id, const V& v) {
    if (!cmi_marshal::is_imm(id)) {
      SlotTable::Slot& sl = t_.slots[cmi_marshal::node_index(id)];
      if (sl.kind == 0 || sl.kind == K) {
        std::memcpy(sl.val, &v, sizeof(V));
        sl.kind = K;
        return;
      }
    }
    over_[id] = v;
  }
  template <class F>
  V get_or(std::size_t id, F&& make_value) {
    V v;
    if (get(id, v)) return v;
    v = make_value();
    put(id, v);
    return v;
  }
  V must(std::size_t id) const {
    V v;
    if (!get(id, v)) throw std::out_of_range("cmi_format: memo");
    return v;
  }

 private:
  SlotTable& t_;
  std::unordered_map<std::size_t, V> over_;
};
struct ListMemo {
  const void* p;
  std::size_t n;
};
// one identity token per marshaled block
inline const void* new_identity() { return zone().alloc(1, 1); }

class Reader {
 public:
  explicit Reader(const cmi_marshal::Graph& g) : g_(g), slots_(g.nodes.size()) {}

  // ---- primitive access ----
  bool is_int(std::size_t id) const {
    return cmi_marshal::is_imm(id) || g_.node(id).kind == cmi_marshal::Kind::Int;
  }
  long ival(std::size_t id) const {
    if (cmi_marshal::is_imm(id)) return cmi_marshal::imm(id);
    const cmi_marshal::Node& x = g_.node(id);
    if (x.kind != cmi_marshal::Kind::Int) throw Corrupt{};
    return static_cast<long>(static_cast<std::int64_t>(x.v));
  }
  unsigned tag(std::size_t id) const {
    if (cmi_marshal::is_imm(id)) throw Corrupt{};
    const cmi_marshal::Node& x = g_.node(id);
    if (x.kind != cmi_marshal::Kind::Block) throw Corrupt{};
    return x.tag;
  }
  std::size_t f(std::size_t id, std::size_t k) const {
    if (cmi_marshal::is_imm(id)) throw Corrupt{};
    const cmi_marshal::Node& x = g_.node(id);
    if (x.kind != cmi_marshal::Kind::Block || k >= x.n) throw Corrupt{};
    return g_.field(x.v + k);
  }
  // one copy per marshaled string: input_value's sharing (a string the
  // .cmi shares stays one string, which output_cmi shares again)
  std::string_view str(std::size_t id) {
    if (cmi_marshal::is_imm(id) || g_.node(id).kind != cmi_marshal::Kind::String) throw Corrupt{};
    if (std::string_view s; str_.get(id, s)) return s;
    std::optional<ZoneScope> in_keep;
    if (keep_) in_keep.emplace(*keep_);
    std::string_view s = zstr(g_.string(g_.node(id)));
    str_.put(id, s);
    return s;
  }
  bool boolean(std::size_t id) const { return ival(id) != 0; }

  // one Slice per marshaled list (input_value's sharing of a whole list;
  // a list sharing only its tail with another is not recorded)
  template <class T, class F>
  Slice<T> list(std::size_t id, F&& elt) {
    if (is_int(id)) return {};
    if (ListMemo lm; list_memo_.get(id, lm)) return Slice<T>{static_cast<const T*>(lm.p), lm.n};
    std::size_t start = id;
    std::vector<T> out;
    while (!is_int(id)) {
      out.push_back(elt(f(id, 0)));
      id = f(id, 1);
    }
    Slice<T> r = slice(out);
    list_memo_.put(start, ListMemo{static_cast<const void*>(r.p), r.n});
    return r;
  }
  template <class T, class F>
  std::vector<T> list_vec(std::size_t id, F&& elt) {
    std::vector<T> out;
    while (!is_int(id)) {
      out.push_back(elt(f(id, 0)));
      id = f(id, 1);
    }
    return out;
  }
  // `option` of something decoded as a pointer: None = nullptr.
  template <class F>
  auto opt_ptr(std::size_t id, F&& elt) -> decltype(elt(id)) {
    if (is_int(id)) return nullptr;
    return elt(f(id, 0));
  }
  OptStr opt_str(std::size_t id) {
    if (is_int(id)) return OptStr::none();
    const void* obj = optstr_obj_.get_or(id, new_identity);
    return OptStr{true, str(f(id, 0)), obj};
  }

  // ---- Ident / Path ----
  ident::Unscoped* unscoped(std::size_t id) {
    if (ident::Unscoped* mv; us_.get(id, mv)) return mv;
    auto* u = make<ident::Unscoped>(ident::Unscoped::State::Udesc);
    us_.put(id, u);
    std::size_t st = f(id, 0);  // { mutable state }
    if (tag(st) == 0) {         // Udesc of desc
      std::size_t d = f(st, 0);
      u->name = str(f(d, 0));
      u->stamp = static_cast<int>(ival(f(d, 1)));
    } else {                    // Ulink of t
      u->state = ident::Unscoped::State::Ulink;
      u->ulink = unscoped(f(st, 0));
    }
    return u;
  }

  // idents and paths are shared as input_value shares them: one object per
  // marshaled block (a cmi's Predef idents, its repeated paths)
  Ident::t ident(std::size_t id) {
    if (Ident::t mv; ident_.get(id, mv)) return mv;
    Ident::t r = ident_raw(id);
    ident_.put(id, r);
    return r;
  }
  Ident::t ident_raw(std::size_t id) {
    using K = Ident::Kind;
    switch (tag(id)) {
      case 0: return Ident::make_raw(K::Local, str(f(id, 0)), (int)ival(f(id, 1)), 0, nullptr);
      case 1:
        return Ident::make_raw(K::Scoped, str(f(id, 0)), (int)ival(f(id, 1)),
                               (int)ival(f(id, 2)), nullptr);
      case 2: return Ident::make_raw(K::Global, str(f(id, 0)), 0, 0, nullptr);
      case 3: return Ident::make_raw(K::Predef, str(f(id, 0)), (int)ival(f(id, 1)), 0, nullptr);
      case 4: return Ident::make_raw(K::Unscoped, {}, 0, 0, unscoped(f(id, 0)));
    }
    throw Corrupt{};
  }

  Path::t path(std::size_t id) {
    if (Path::t mv; path_.get(id, mv)) return mv;
    Path::t r = path_raw(id);
    path_.put(id, r);
    return r;
  }
  Path::t path_raw(std::size_t id) {
    switch (tag(id)) {
      case 0: return Path::pident(ident(f(id, 0)));
      case 1: return Path::pdot(path(f(id, 0)), str(f(id, 1)));
      case 2: return Path::papply(path(f(id, 0)), path(f(id, 1)));
      case 3: {
        std::size_t e = f(id, 1);
        if (is_int(e)) return Path::pextra_ty(path(f(id, 0)), Path::Extra::Pext_ty);
        return Path::pextra_ty(path(f(id, 0)), Path::Extra::Pcstr_ty, str(f(e, 0)));
      }
    }
    throw Corrupt{};
  }

  // ---- support ----
  // one record per marshaled block, with its identity (support.hpp)
  Position position(std::size_t id) {
    if (const Position* mv; pos_.get(id, mv)) return *mv;
    std::string_view fname = str(f(id, 0));
    std::optional<ZoneScope> in_keep;
    if (keep_) in_keep.emplace(*keep_);
    auto* p = make<Position>(mkpos(fname, ival(f(id, 1)), ival(f(id, 2)), ival(f(id, 3))));
    p->obj = p;
    pos_.put(id, p);
    return *p;
  }
  Location loc(std::size_t id) {
    if (const Location* mv; loc_.get(id, mv)) return *mv;
    Position a = position(f(id, 0));
    Position e = position(f(id, 1));
    auto* l = make<Location>(Location{a, e, boolean(f(id, 2))});
    l->obj = loc_record(*l);
    loc_.put(id, l);
    return *l;
  }
  Uid uid(std::size_t id) {
    Uid u;
    if (is_int(id)) {  // Internal
      u.kind = Uid::Kind::Internal;
      return u;
    }
    // one identity per marshaled record (input_value's sharing)
    u.obj = uid_obj_.get_or(id, new_identity);
    switch (tag(id)) {
      case 0: u.kind = Uid::Kind::Compilation_unit; u.comp_unit = str(f(id, 0)); break;
      case 1:
        u.kind = Uid::Kind::Item;
        u.comp_unit = str(f(id, 0));
        u.id = ival(f(id, 1));
        u.from = ival(f(id, 2)) == 0 ? Uid::From::Intf : Uid::From::Impl;
        break;
      case 2:
        u.kind = Uid::Kind::Local_opaque_item;
        u.comp_unit = str(f(id, 0));
        u.id = ival(f(id, 1));
        break;
      case 3: u.kind = Uid::Kind::Predef; u.comp_unit = str(f(id, 0)); break;
      default: throw Corrupt{};
    }
    return u;
  }
  ArgLabel arg_label(std::size_t id) {
    if (is_int(id)) return ArgLabel::nolabel();
    // input_value's blocks: one label object per marshaled block
    auto [it, fresh] = label_objs_.try_emplace(id, nullptr);
    if (fresh) it->second = ArgLabel::fresh_obj();
    return ArgLabel{tag(id) == 0 ? ArgLabel::Kind::Labelled : ArgLabel::Kind::Optional,
                    str(f(id, 0)), it->second};
  }
  std::unordered_map<std::size_t, const void*> label_objs_;

  const OValue* ovalue(std::size_t id) {
    if (cmi_marshal::is_imm(id)) return make<OValue>(OValue::Kind::Int, cmi_marshal::imm(id));
    const cmi_marshal::Node& x = g_.node(id);
    switch (x.kind) {
      case cmi_marshal::Kind::Int:
        return make<OValue>(OValue::Kind::Int, static_cast<long>(static_cast<std::int64_t>(x.v)));
      case cmi_marshal::Kind::String:
        return make<OValue>(OValue::Kind::String, 0L, str(id));  // input_value's sharing
      case cmi_marshal::Kind::Double:
        return make<OValue>(OValue::Kind::Double, 0L, std::string_view{}, g_.dbl(x));
      case cmi_marshal::Kind::Block: {
        if (OValue* o; ov_.get(id, o)) return o;
        auto* o = make<OValue>(OValue::Kind::Block, 0L, std::string_view{}, 0.0, x.tag);
        ov_.put(id, o);
        std::vector<const OValue*> fs;
        for (std::size_t k = 0; k < x.n; ++k) fs.push_back(ovalue(g_.field(x.v + k)));
        o->fields = slice(fs);
        if (x.tag == 0 && fs.size() == 4 && fs[0]->kind == OValue::Kind::String && fs[1]->kind == OValue::Kind::Int &&
            fs[2]->kind == OValue::Kind::Int && fs[3]->kind == OValue::Kind::Int) {
          (void)position(id);
          o->pos = pos_.must(id);
        }
        return o;
      }
      default:
        throw Corrupt{};
    }
  }

  Attributes attributes(std::size_t id) {
    // one attribute per marshaled block, whatever list it is reached from
    // (Subst's List.filter builds new lists of the same attributes)
    return list<const Attribute*>(id, [&](std::size_t a) -> const Attribute* {
      return attr_.get_or(a, [&] {
        std::size_t nm = f(a, 0);  // string loc = {txt; loc}
        return static_cast<const Attribute*>(
            make<Attribute>(str(f(nm, 0)), loc(f(nm, 1)), ovalue(f(a, 1)), loc(f(a, 2))));
      });
    });
  }

  // ---- type expressions ----
  Commutable* commu(std::size_t id) {
    if (is_int(id)) return ival(id) == 0 ? types::cok() : types::cunknown();
    if (Commutable* mv; commu_.get(id, mv)) return mv;
    auto* c = make<Commutable>(Commutable::Kind::Cvar, nullptr);
    commu_.put(id, c);
    c->commu = commu(f(id, 0));
    return c;
  }

  FieldKind* field_kind(std::size_t id) {
    if (is_int(id)) {
      switch (ival(id)) {
        case 0: return types::fkprivate();
        case 1: return types::fkpublic();
        default: return types::fkabsent();
      }
    }
    if (FieldKind* mv; fk_.get(id, mv)) return mv;
    auto* k = make<FieldKind>(FieldKind::Kind::FKvar, nullptr);
    fk_.put(id, k);
    k->field_kind = field_kind(f(id, 0));
    return k;
  }

  const PathArgs* path_args(std::size_t id) {
    return make<PathArgs>(path(f(id, 0)), list<TypeExpr*>(f(id, 1), [&](std::size_t t) {
                            return ty(t);
                          }));
  }

  NameRef* name_ref(std::size_t id) {
    if (NameRef* mv; nm_.get(id, mv)) return mv;
    auto* r = make<NameRef>(nullptr);
    nm_.put(id, r);
    r->contents = opt_ptr(f(id, 0), [&](std::size_t x) { return path_args(x); });
    return r;
  }

  const AbbrevMemo* memo(std::size_t id) {
    if (is_int(id)) return types::mnil();
    if (tag(id) == 0)
      return make<AbbrevMemo>(AbbrevMemo::Kind::Mcons,
                              ival(f(id, 0)) == 0 ? PrivateFlag::Private : PrivateFlag::Public,
                              path(f(id, 1)), ty(f(id, 2)), ty(f(id, 3)), memo(f(id, 4)));
    return make<AbbrevMemo>(AbbrevMemo::Kind::Mlink, PrivateFlag::Public, nullptr, nullptr,
                            nullptr, nullptr, memo_ref(f(id, 0)));
  }

  MemoRef* memo_ref(std::size_t id) {
    if (MemoRef* mv; memo_.get(id, mv)) return mv;
    auto* r = make<MemoRef>(types::mnil());
    memo_.put(id, r);
    r->contents = memo(f(id, 0));
    return r;
  }

  RowFieldCell* row_cell(std::size_t id) {
    if (RowFieldCell* mv; cell_.get(id, mv)) return mv;
    auto* c = make<RowFieldCell>(types::rfnone());
    cell_.put(id, c);
    c->contents = row_field(f(id, 0));
    return c;
  }

  const RowField* row_field(std::size_t id) {
    if (is_int(id)) return ival(id) == 0 ? types::rfabsent() : types::rfnone();
    if (tag(id) == 0)
      return make<RowField>(RowField::Kind::RFpresent,
                            opt_ptr(f(id, 0), [&](std::size_t t) { return ty(t); }));
    // RFeither {no_arg; arg_type; matched; ext}
    bool no_arg = boolean(f(id, 0));
    auto args = list<TypeExpr*>(f(id, 1), [&](std::size_t t) { return ty(t); });
    bool matched = boolean(f(id, 2));
    return make<RowField>(RowField::Kind::RFeither, nullptr, no_arg, args, matched,
                          row_cell(f(id, 3)));
  }

  const FixedExplanation* fixed(std::size_t id) {
    using FK = FixedExplanation::Kind;
    if (is_int(id)) return make<FixedExplanation>(ival(id) == 0 ? FK::Fixed_private : FK::Rigid);
    if (tag(id) == 0) return make<FixedExplanation>(FK::Univar, ty(f(id, 0)));
    return make<FixedExplanation>(FK::Reified, nullptr, path(f(id, 0)));
  }

  const RowDesc* row(std::size_t id) {
    auto fields = list<RowFieldEntry>(f(id, 0), [&](std::size_t e) {
      // one identity per marshaled tuple (input_value's sharing)
      auto [it, fresh] = entry_objs_.try_emplace(e, nullptr);
      if (fresh) it->second = fresh_identity();
      return RowFieldEntry{str(f(e, 0)), row_field(f(e, 1)), it->second};
    });
    TypeExpr* more = ty(f(id, 1));
    bool closed = boolean(f(id, 2));
    auto* fx = opt_ptr(f(id, 3), [&](std::size_t x) { return fixed(x); });
    auto* nm = opt_ptr(f(id, 4), [&](std::size_t x) { return path_args(x); });
    return make<RowDesc>(fields, more, closed, fx, nm);
  }

  const Package* package(std::size_t id) {
    Path::t p = path(f(id, 0));
    auto cs = list<PackConstraint>(f(id, 1), [&](std::size_t c) {
      return PackConstraint{list<std::string_view>(f(c, 0), [&](std::size_t s) { return str(s); }),
                            ty(f(c, 1))};
    });
    return make<Package>(p, cs);
  }

  Slice<TypeExpr*> tys(std::size_t id) {
    return list<TypeExpr*>(id, [&](std::size_t t) { return ty(t); });
  }

  const TypeDesc* desc(std::size_t id) {
    if (is_int(id)) {
      if (ival(id) == 0) return types::tnil();
      throw Corrupt{};
    }
    if (const TypeDesc* mv; desc_.get(id, mv)) return mv;
    const TypeDesc* d = nullptr;
    switch (tag(id)) {
      case 0: d = types::tvar(opt_str(f(id, 0))); break;
      case 1:
        d = types::tarrow(arg_label(f(id, 0)), ty(f(id, 1)), ty(f(id, 2)), commu(f(id, 3)));
        break;
      case 2:
        d = types::ttuple(list<LabeledTy>(f(id, 0), [&](std::size_t e) {
          return LabeledTy{opt_str(f(e, 0)), ty(f(e, 1))};
        }));
        break;
      case 3: {
        Path::t p = path(f(id, 0));
        auto args = tys(f(id, 1));
        d = types::tconstr(p, args, memo_ref(f(id, 2)));
        break;
      }
      case 4: {
        TypeExpr* fl = ty(f(id, 0));
        d = types::tobject(fl, name_ref(f(id, 1)));
        break;
      }
      case 5: {
        std::string_view l = str(f(id, 0));
        FieldKind* k = field_kind(f(id, 1));
        TypeExpr* a = ty(f(id, 2));
        d = types::tfield(l, k, a, ty(f(id, 3)));
        break;
      }
      case 6: d = types::tlink(ty(f(id, 0))); break;
      case 7: {
        TypeExpr* a = ty(f(id, 0));
        d = types::tsubst(a, opt_ptr(f(id, 1), [&](std::size_t t) { return ty(t); }));
        break;
      }
      case 8: d = types::tvariant(row(f(id, 0))); break;
      case 9: d = types::tunivar(opt_str(f(id, 0))); break;
      case 10: {
        TypeExpr* a = ty(f(id, 0));
        d = types::tpoly(a, tys(f(id, 1)));
        break;
      }
      case 11: d = types::tpackage(package(f(id, 0))); break;
      case 12: {
        ArgLabel l = arg_label(f(id, 0));
        ident::Unscoped* u = unscoped(f(id, 1));
        const Package* p = package(f(id, 2));
        d = types::tfunctor(l, u, p, ty(f(id, 3)));
        break;
      }
      default: throw Corrupt{};
    }
    desc_.put(id, d);
    return d;
  }

  TypeExpr* ty(std::size_t id) {
    if (TypeExpr* mv; ty_.get(id, mv)) return mv;
    // transient_expr = { mutable desc; mutable level; mutable scope; id }
    auto* t = types::create_expr(nullptr, ival(f(id, 1)), ival(f(id, 2)), ival(f(id, 3)));
    ty_.put(id, t);
    t->desc = desc(f(id, 0));
    return t;
  }

  // ---- declarations ----
  static variance::t variance_of(long x) { return x; }

  const LabelDeclaration* label_decl(std::size_t id) {
    Ident::t i = ident(f(id, 0));
    auto mut = ival(f(id, 1)) == 0 ? MutableFlag::Immutable : MutableFlag::Mutable;
    auto at = ival(f(id, 2)) == 0 ? AtomicFlag::Nonatomic : AtomicFlag::Atomic;
    TypeExpr* t = ty(f(id, 3));
    Location l = loc(f(id, 4));
    Attributes as = attributes(f(id, 5));
    return make<LabelDeclaration>(i, mut, at, t, l, as, uid(f(id, 6)));
  }

  ConstructorArguments cstr_args(std::size_t id) {
    ConstructorArguments a;
    if (tag(id) == 0) {
      a.kind = ConstructorArguments::Kind::Cstr_tuple;
      a.tuple = tys(f(id, 0));
    } else {
      a.kind = ConstructorArguments::Kind::Cstr_record;
      a.record = list<const LabelDeclaration*>(f(id, 0), [&](std::size_t x) {
        return label_decl(x);
      });
    }
    return a;
  }

  const ConstructorDeclaration* cstr_decl(std::size_t id) {
    Ident::t i = ident(f(id, 0));
    ConstructorArguments args = cstr_args(f(id, 1));
    TypeExpr* res = opt_ptr(f(id, 2), [&](std::size_t t) { return ty(t); });
    Location l = loc(f(id, 3));
    Attributes as = attributes(f(id, 4));
    return make<ConstructorDeclaration>(i, args, res, l, as, uid(f(id, 5)));
  }

  PrivateFlag private_flag(std::size_t id) {
    return ival(id) == 0 ? PrivateFlag::Private : PrivateFlag::Public;
  }

  const TypeKind* type_kind(std::size_t id) {
    auto* k = make<TypeKind>();
    using KK = TypeKind::Kind;
    if (is_int(id)) {
      k->kind = KK::Type_open;
      return k;
    }
    switch (tag(id)) {
      case 0: {
        k->kind = KK::Type_abstract;
        std::size_t o = f(id, 0);
        using OK = TypeOrigin::Kind;
        if (is_int(o)) {
          switch (ival(o)) {
            case 0: k->origin.kind = OK::Definition; break;
            case 1: k->origin.kind = OK::Rec_check_regularity; break;
            default: k->origin.kind = OK::Approx_recmod; break;
          }
        } else {
          k->origin.kind = OK::Existential;
          k->origin.existential = str(f(o, 0));
          k->origin.obj = block_identity(o);
        }
        break;
      }
      case 1: {
        k->kind = KK::Type_record;
        k->labels = list<const LabelDeclaration*>(f(id, 0), [&](std::size_t x) {
          return label_decl(x);
        });
        std::size_t r = f(id, 1);
        using RK = RecordRepresentation::Kind;
        if (is_int(r)) {
          k->record_repr.kind = ival(r) == 0 ? RK::Record_regular : RK::Record_float;
        } else {
          k->record_repr.obj = repr_obj_.get_or(r, new_identity);
          switch (tag(r)) {
            case 0: k->record_repr.kind = RK::Record_unboxed;
                    k->record_repr.unboxed_inlined = boolean(f(r, 0)); break;
            case 1: k->record_repr.kind = RK::Record_inlined;
                    k->record_repr.inlined_tag = ival(f(r, 0)); break;
            default: k->record_repr.kind = RK::Record_extension;
                     k->record_repr.extension = path(f(r, 0)); break;
          }
        }
        break;
      }
      case 2:
        k->kind = KK::Type_variant;
        k->constructors = list<const ConstructorDeclaration*>(f(id, 0), [&](std::size_t x) {
          return cstr_decl(x);
        });
        k->variant_repr = ival(f(id, 1)) == 0 ? VariantRepresentation::Variant_regular
                                              : VariantRepresentation::Variant_unboxed;
        break;
      case 3:
        k->kind = KK::Type_external;
        k->external = str(f(id, 0));
        break;
      default:
        throw Corrupt{};
    }
    return k;
  }

  const TypeDeclaration* type_decl(std::size_t id) {
    auto params = tys(f(id, 0));
    long arity = ival(f(id, 1));
    const TypeKind* kind = type_kind(f(id, 2));
    PrivateFlag priv = private_flag(f(id, 3));
    TypeExpr* manifest = opt_ptr(f(id, 4), [&](std::size_t t) { return ty(t); });
    auto var = list<variance::t>(f(id, 5), [&](std::size_t x) { return variance_of(ival(x)); });
    auto sep = list<Separability>(f(id, 6), [&](std::size_t x) {
      return static_cast<Separability>(ival(x));
    });
    bool newtype = boolean(f(id, 7));
    long exp_scope = ival(f(id, 8));
    Location l = loc(f(id, 9));
    Attributes as = attributes(f(id, 10));
    auto imm = static_cast<TypeImmediacy>(ival(f(id, 11)));
    bool ubd = boolean(f(id, 12));
    return make<TypeDeclaration>(params, arity, kind, priv, manifest, var, sep, newtype,
                                 exp_scope, l, as, imm, ubd, uid(f(id, 13)));
  }

  const ExtensionConstructor* ext_constr(std::size_t id) {
    Path::t p = path(f(id, 0));
    auto params = tys(f(id, 1));
    ConstructorArguments args = cstr_args(f(id, 2));
    TypeExpr* ret = opt_ptr(f(id, 3), [&](std::size_t t) { return ty(t); });
    PrivateFlag priv = private_flag(f(id, 4));
    Location l = loc(f(id, 5));
    Attributes as = attributes(f(id, 6));
    return make<ExtensionConstructor>(p, params, args, ret, priv, l, as, uid(f(id, 7)));
  }

  // String.Map: Empty = 0 | Node {l; v; d; r; h}
  template <class V, class F>
  const StrMapNode<V>* strmap(std::size_t id, F&& data) {
    if (is_int(id)) return nullptr;
    auto* l = strmap<V>(f(id, 0), data);
    std::string_view k = str(f(id, 1));
    V d = data(f(id, 2));
    auto* r = strmap<V>(f(id, 3), data);
    return StrMap<V>::node(l, k, d, r, (int)ival(f(id, 4)));
  }

  VirtualFlag virtual_flag(std::size_t id) {
    return ival(id) == 0 ? VirtualFlag::Virtual : VirtualFlag::Concrete;
  }
  MutableFlag mutable_flag(std::size_t id) {
    return ival(id) == 0 ? MutableFlag::Immutable : MutableFlag::Mutable;
  }

  ClassSignature* class_sig(std::size_t id) {
    if (ClassSignature* mv; csig_.get(id, mv)) return mv;
    auto* c = make<ClassSignature>();
    csig_.put(id, c);
    c->csig_self = ty(f(id, 0));
    c->csig_self_row = ty(f(id, 1));
    c->csig_vars = StrMap<VarEntry>(strmap<VarEntry>(f(id, 2), [&](std::size_t x) {
      MutableFlag mu = mutable_flag(f(x, 0));
      VirtualFlag vi = virtual_flag(f(x, 1));
      return VarEntry{mu, vi, ty(f(x, 2))};
    }));
    c->csig_meths = StrMap<MethEntry>(strmap<MethEntry>(f(id, 3), [&](std::size_t x) {
      MethodPrivacy p;
      std::size_t pv = f(x, 0);
      if (!is_int(pv)) {
        p.is_private = true;
        p.kind = field_kind(f(pv, 0));
      }
      VirtualFlag vi = virtual_flag(f(x, 1));
      return MethEntry{p, vi, ty(f(x, 2))};
    }));
    return c;
  }

  const ClassType* class_type(std::size_t id) {
    using CK = ClassType::Kind;
    auto* c = make<ClassType>(CK::Cty_constr);
    switch (tag(id)) {
      case 0:
        c->path = path(f(id, 0));
        c->args = tys(f(id, 1));
        c->cty = class_type(f(id, 2));
        break;
      case 1:
        c->kind = CK::Cty_signature;
        c->sign = class_sig(f(id, 0));
        break;
      case 2:
        c->kind = CK::Cty_arrow;
        c->label = arg_label(f(id, 0));
        c->arg = ty(f(id, 1));
        c->cty = class_type(f(id, 2));
        break;
      default:
        throw Corrupt{};
    }
    return c;
  }

  Slice<variance::t> variances(std::size_t id) {
    return list<variance::t>(id, [&](std::size_t x) { return variance_of(ival(x)); });
  }

  NativeRepr native_repr(std::size_t id) {
    NativeRepr n;
    if (is_int(id)) {
      switch (ival(id)) {
        case 0: n.kind = NativeRepr::Kind::Same_as_ocaml_repr; break;
        case 1: n.kind = NativeRepr::Kind::Unboxed_float; break;
        default: n.kind = NativeRepr::Kind::Untagged_immediate; break;
      }
    } else {
      n.kind = NativeRepr::Kind::Unboxed_integer;
      n.obj = repr_obj_.get_or(id, new_identity);
      n.bi = static_cast<BoxedInteger>(ival(f(id, 0)));
    }
    return n;
  }

  ValueKind value_kind(std::size_t id) {
    ValueKind k;
    if (is_int(id)) return k;  // Val_reg
    switch (tag(id)) {
      case 0: {
        k.kind = ValueKind::Kind::Val_prim;
        // one description per marshaled block: input_value keeps a
        // description reached twice (a signature's value copied by Subst
        // keeps its val_kind) one object, which the writers share
        std::size_t p = f(id, 0);
        k.prim = prim_.get_or(p, [&] {
          return static_cast<const PrimitiveDescription*>(make<PrimitiveDescription>(
              str(f(p, 0)), ival(f(p, 1)), boolean(f(p, 2)), str(f(p, 3)),
              list<NativeRepr>(f(p, 4), [&](std::size_t x) { return native_repr(x); }),
              native_repr(f(p, 5))));
        });
        break;
      }
      case 1:
        k.kind = ValueKind::Kind::Val_ivar;
        k.ivar_mut = mutable_flag(f(id, 0));
        k.ivar_name = str(f(id, 1));
        break;
      case 2: k.kind = ValueKind::Kind::Val_self; break;
      default: k.kind = ValueKind::Kind::Val_anc; break;
    }
    return k;
  }

  const ModuleType* module_type(std::size_t id) {
    using MK = ModuleType::Kind;
    auto* mt = make<ModuleType>(MK::Mty_ident);
    switch (tag(id)) {
      case 0: mt->path = path(f(id, 0)); break;
      case 1: mt->kind = MK::Mty_signature; mt->sign = signature(f(id, 0)); break;
      case 2: {
        mt->kind = MK::Mty_functor;
        std::size_t p = f(id, 0);
        if (!is_int(p)) {
          mt->param.is_unit = false;
          mt->param.id = opt_ptr(f(p, 0), [&](std::size_t x) { return ident(x); });
          if (!is_int(f(p, 0))) {  // one identity per marshaled `Some` block
            mt->param.some_obj = some_obj_.get_or(f(p, 0), new_identity);
          }
          mt->param.mty = module_type(f(p, 1));
        }
        mt->res = module_type(f(id, 1));
        break;
      }
      case 3: mt->kind = MK::Mty_alias; mt->path = path(f(id, 0)); break;
      default: throw Corrupt{};
    }
    return mt;
  }

  const SignatureItem* sig_item(std::size_t id) {
    using SK = SignatureItem::Kind;
    auto* it = make<SignatureItem>(SK::Sig_value, nullptr);
    it->id = ident(f(id, 0));
    auto vis = [&](std::size_t x) {
      return ival(x) == 0 ? Visibility::Exported : Visibility::Hidden;
    };
    auto rec = [&](std::size_t x) { return static_cast<RecStatus>(ival(x)); };
    switch (tag(id)) {
      case 0: {
        std::size_t vd = f(id, 1);
        TypeExpr* t = ty(f(vd, 0));
        ValueKind k = value_kind(f(vd, 1));
        Location l = loc(f(vd, 2));
        Attributes as = attributes(f(vd, 3));
        it->value = make<ValueDescription>(t, k, l, as, uid(f(vd, 4)));
        it->vis = vis(f(id, 2));
        break;
      }
      case 1:
        it->kind = SK::Sig_type;
        it->type = type_decl(f(id, 1));
        it->rec = rec(f(id, 2));
        it->vis = vis(f(id, 3));
        break;
      case 2:
        it->kind = SK::Sig_typext;
        it->ext = ext_constr(f(id, 1));
        it->ext_status = static_cast<ExtStatus>(ival(f(id, 2)));
        it->vis = vis(f(id, 3));
        break;
      case 3: {
        it->kind = SK::Sig_module;
        it->presence = ival(f(id, 1)) == 0 ? ModulePresence::Mp_present
                                           : ModulePresence::Mp_absent;
        std::size_t md = f(id, 2);
        const ModuleType* mt = module_type(f(md, 0));
        Attributes as = attributes(f(md, 1));
        Location l = loc(f(md, 2));
        it->md = make<ModuleDeclaration>(mt, as, l, uid(f(md, 3)));
        it->rec = rec(f(id, 3));
        it->vis = vis(f(id, 4));
        break;
      }
      case 4: {
        it->kind = SK::Sig_modtype;
        std::size_t mtd = f(id, 1);
        const ModuleType* mt = opt_ptr(f(mtd, 0), [&](std::size_t x) { return module_type(x); });
        Attributes as = attributes(f(mtd, 1));
        Location l = loc(f(mtd, 2));
        it->mtd = make<ModtypeDeclaration>(mt, as, l, uid(f(mtd, 3)));
        it->vis = vis(f(id, 2));
        break;
      }
      case 5: {
        it->kind = SK::Sig_class;
        std::size_t cd = f(id, 1);
        auto params = tys(f(cd, 0));
        const ClassType* ct = class_type(f(cd, 1));
        Path::t p = path(f(cd, 2));
        TypeExpr* nw = opt_ptr(f(cd, 3), [&](std::size_t t) { return ty(t); });
        auto var = variances(f(cd, 4));
        Location l = loc(f(cd, 5));
        Attributes as = attributes(f(cd, 6));
        it->cls = make<ClassDeclaration>(params, ct, p, nw, var, l, as, uid(f(cd, 7)));
        it->rec = rec(f(id, 2));
        it->vis = vis(f(id, 3));
        break;
      }
      case 6: {
        it->kind = SK::Sig_class_type;
        std::size_t cd = f(id, 1);
        auto params = tys(f(cd, 0));
        const ClassType* ct = class_type(f(cd, 1));
        Path::t p = path(f(cd, 2));
        const TypeDeclaration* hash = type_decl(f(cd, 3));
        auto var = variances(f(cd, 4));
        Location l = loc(f(cd, 5));
        Attributes as = attributes(f(cd, 6));
        it->clty = make<ClassTypeDeclaration>(params, ct, p, hash, var, l, as, uid(f(cd, 7)));
        it->rec = rec(f(id, 2));
        it->vis = vis(f(id, 3));
        break;
      }
      default:
        throw Corrupt{};
    }
    return it;
  }

  Signature signature(std::size_t id) {
    return list<const SignatureItem*>(id, [&](std::size_t x) { return sig_item(x); });
  }

 protected:
  const cmi_marshal::Graph& graph() const { return g_; }

 private:
  const cmi_marshal::Graph& g_;
 public:
  Zone* keep_ = nullptr;  // input_ovalue's keep zone (strings, positions)
 private:
  SlotTable slots_;
  FlatMemo<TypeExpr*, 1> ty_{slots_};
  FlatMemo<const TypeDesc*, 2> desc_{slots_};
  FlatMemo<Commutable*, 3> commu_{slots_};
  FlatMemo<FieldKind*, 4> fk_{slots_};
  FlatMemo<NameRef*, 5> nm_{slots_};
  FlatMemo<MemoRef*, 6> memo_{slots_};
  FlatMemo<RowFieldCell*, 7> cell_{slots_};
  FlatMemo<ident::Unscoped*, 8> us_{slots_};
  FlatMemo<ClassSignature*, 9> csig_{slots_};
  std::unordered_map<std::size_t, const void*> entry_objs_;  // row fields' tuples
  FlatMemo<OValue*, 10> ov_{slots_};
  FlatMemo<std::string_view, 11> str_{slots_};
  FlatMemo<Ident::t, 12> ident_{slots_};
  FlatMemo<Path::t, 13> path_{slots_};
  FlatMemo<const void*, 14> uid_obj_{slots_};
  FlatMemo<const void*, 15> some_obj_{slots_};
  FlatMemo<const void*, 16> block_obj_{slots_};
  // one identity per marshaled block
  const void* block_identity(std::size_t id) { return block_obj_.get_or(id, new_identity); }
  FlatMemo<const Position*, 17> pos_{slots_};
  FlatMemo<const Location*, 18> loc_{slots_};
  FlatMemo<const void*, 19> repr_obj_{slots_};
  FlatMemo<const void*, 20> optstr_obj_{slots_};
  FlatMemo<ListMemo, 21> list_memo_{slots_};
  FlatMemo<const PrimitiveDescription*, 22> prim_{slots_};
  FlatMemo<const Attribute*, 23> attr_{slots_};
};


}  // namespace

using writer::EventWriter;
using writer::Writer;
namespace o = cppcaml::omarshal;

std::vector<o::ValPtr> debug_event_values(const std::vector<const instruct::DebugEvent*>& events,
                                          o::ValPtr unit_name) {
  Writer w;
  if (!events.empty()) w.set_current_unit(events.front()->ev_module, unit_name);
  EventWriter ew(w);
  std::vector<o::ValPtr> evs;
  for (const instruct::DebugEvent* ev : events) evs.push_back(ew.event(ev));
  return evs;
}

std::vector<std::uint8_t> marshal_debug_events(const std::vector<const instruct::DebugEvent*>& events) {
  Writer w;
  // the uids' and ev_module's unit name: the one Unit_info string, which
  // the unit's module ident (a top-level scope's name) shares too
  if (!events.empty()) w.set_current_unit_shared(events.front()->ev_module);
  EventWriter ew(w);
  std::vector<o::ValPtr> evs;
  for (const instruct::DebugEvent* ev : events) evs.push_back(ew.event(ev));
  return o::marshal(o::vlist(evs), config::compression_supported);  // Compression.output_value
}

const OValue* input_ovalue(const std::uint8_t* data, std::size_t len, std::size_t& off, Zone* keep) {
  cmi_marshal::Graph graph;
  std::size_t root = cmi_marshal::read_value(data, len, off, graph);
  Reader r(graph);
  r.keep_ = keep;
  try {
    return r.ovalue(root);
  } catch (const Corrupt&) {
    throw m::Error("input_value: ill-formed value");
  }
}

std::vector<std::uint8_t> output_ovalue(const OValue* v) {
  Writer w;
  return o::marshal(w.ovalue(v));
}

std::size_t marshaled_size(const ModuleType* a, const ModuleType* b) {
  Writer w;
  w.set_current_unit(env::get_current_unit_name());
  // (got, expected): a tuple, fields evaluated right to left
  o::ValPtr vb = w.module_type(b);
  o::ValPtr va = w.module_type(a);
  return o::marshal(o::vblock(0, {va, vb})).size();
}
std::size_t marshaled_size(const ModtypeDeclaration* a, const ModtypeDeclaration* b) {
  Writer w;
  w.set_current_unit(env::get_current_unit_name());
  o::ValPtr vb = w.modtype_decl(b);
  o::ValPtr va = w.modtype_decl(a);
  return o::marshal(o::vblock(0, {va, vb})).size();
}

std::pair<std::string, std::string> output_cmi_bytes(const CmiInfos& cmi) {
  // (the provided signature must have been substituted for saving)
  Writer w;
  w.set_current_unit(cmi.cmi_name);
  o::ValPtr name = w.unit_name(cmi.cmi_name);
  o::ValPtr header = o::vblock(0, {name, w.signature(cmi.cmi_sign)});
  std::vector<std::uint8_t> hbytes = o::marshal(header, config::compression_supported);  // Compression.output_value
  std::string prefix(config::cmi_magic_number);
  prefix.append(reinterpret_cast<const char*>(hbytes.data()), hbytes.size());
  // Digest.BLAKE128.file filename, after the flush: the magic and the header
  std::string crc = blake2::blake128(reinterpret_cast<const unsigned char*>(prefix.data()), prefix.size());
  std::vector<o::ValPtr> crcs{o::vblock(0, {w.str(cmi.cmi_name), w.some(w.str(crc))})};
  for (auto& [name, c] : cmi.cmi_crcs)
    crcs.push_back(o::vblock(0, {w.str(name), c ? w.some(w.str(*c)) : w.none()}));
  std::vector<std::uint8_t> cbytes = o::marshal(o::vlist(crcs));
  std::vector<o::ValPtr> flags;
  for (const PersFlag& f : cmi.cmi_flags) {
    switch (f.kind) {
      case PersFlag::Kind::Rectypes: flags.push_back(w.i(0)); break;
      case PersFlag::Kind::Opaque: flags.push_back(w.i(1)); break;
      case PersFlag::Kind::Alerts:
        flags.push_back(o::vblock(0, {w.strmap<std::string_view>(f.alerts.root(), [&](std::string_view s) {
          return w.str(s);
        })}));
        break;
    }
  }
  std::vector<std::uint8_t> fbytes = o::marshal(o::vlist(flags));
  std::string out = prefix;
  out.append(reinterpret_cast<const char*>(cbytes.data()), cbytes.size());
  out.append(reinterpret_cast<const char*>(fbytes.data()), fbytes.size());
  return {out, crc};
}

std::string output_cmi(const std::string& filename, const CmiInfos& cmi) {
  auto [bytes, crc] = output_cmi_bytes(cmi);
  // Misc.output_to_file_via_temporary
  std::string tmp = filename + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open " + tmp);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("Cannot write " + tmp);
  }
  if (std::rename(tmp.c_str(), filename.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error("Cannot rename " + tmp + " to " + filename);
  }
  return crc;
}

namespace {
// read_cmi's decoding of the file's bytes (after its magic)
CmiInfos decode_cmi(const std::vector<std::uint8_t>& bytes, std::size_t magic_size, const std::string& filename) {
  try {
    cmi_marshal::Graph graph;
    std::size_t off = magic_size;
    std::size_t header = cmi_marshal::read_value(bytes.data(), bytes.size(), off, graph);
    std::size_t crcs = cmi_marshal::read_value(bytes.data(), bytes.size(), off, graph);
    std::size_t flags = cmi_marshal::read_value(bytes.data(), bytes.size(), off, graph);
    Reader r(graph);
    CmiInfos ci;
    ci.cmi_name = r.str(r.f(header, 0));
    ci.cmi_sign = r.signature(r.f(header, 1));
    ci.cmi_crcs = r.list_vec<std::pair<std::string, std::optional<std::string>>>(
        crcs, [&](std::size_t e) {
          std::pair<std::string, std::optional<std::string>> p;
          p.first = std::string(r.str(r.f(e, 0)));
          std::size_t d = r.f(e, 1);
          if (!r.is_int(d)) p.second = std::string(r.str(r.f(d, 0)));
          return p;
        });
    ci.cmi_flags = r.list_vec<PersFlag>(flags, [&](std::size_t x) {
      PersFlag pf{PersFlag::Kind::Rectypes};
      if (r.is_int(x)) {
        pf.kind = r.ival(x) == 0 ? PersFlag::Kind::Rectypes : PersFlag::Kind::Opaque;
      } else {
        pf.kind = PersFlag::Kind::Alerts;
        pf.alerts = StrMap<std::string_view>(r.strmap<std::string_view>(
            r.f(x, 0), [&](std::size_t s) { return r.str(s); }));
      }
      return pf;
    });
    return ci;
  } catch (const Corrupt&) {
    throw Error(Error::Kind::Corrupted_interface, filename);
  } catch (const m::Error&) {
    throw Error(Error::Kind::Corrupted_interface, filename);
  }
}
}  // namespace

CmiInfos read_cmi(const std::string& filename) {
  // a decoded image of this very file, mapped (cmi_image.hpp)
  if (std::optional<CmiInfos> ci = cmi_image::load(filename)) return std::move(*ci);
  // the whole file in one read (a .cmi is read on every unit's startup)
  std::vector<std::uint8_t> bytes;
  {
    std::FILE* f = std::fopen(filename.c_str(), "rb");
    // open_in_bin's Sys_error
    if (!f) throw arg::SysError(filename + ": " + std::strerror(errno));
    struct Close {
      std::FILE* f;
      ~Close() { std::fclose(f); }
    } close{f};
    if (std::fseek(f, 0, SEEK_END) == 0) {
      long n = std::ftell(f);
      if (n > 0) bytes.resize(static_cast<std::size_t>(n));
      std::rewind(f);
    }
    std::size_t got = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), f);
    bytes.resize(got);
    // a file that grew (or could not be sized): read the rest
    std::uint8_t chunk[65536];
    for (std::size_t k; (k = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + k);
  }
  const std::string magic = config::cmi_magic_number;
  if (bytes.size() < magic.size())
    throw Error(Error::Kind::Corrupted_interface, filename);
  std::string buffer(bytes.begin(), bytes.begin() + magic.size());
  if (buffer != magic) {
    std::size_t pre_len = magic.size() - 3;
    if (buffer.compare(0, pre_len, magic, 0, pre_len) == 0)
      throw Error(Error::Kind::Wrong_version_interface, filename,
                  buffer < magic ? "an older" : "a newer");
    throw Error(Error::Kind::Not_an_interface, filename);
  }
  // decoded into an image recorded for the next compilations, when possible
  if (std::optional<CmiInfos> ci = cmi_image::record(filename, bytes.size(), [&] {
        return decode_cmi(bytes, magic.size(), filename);
      }))
    return std::move(*ci);
  return decode_cmi(bytes, magic.size(), filename);
}

}  // namespace cppcaml::typing::cmi_format

// ---- .cmx: Compilenv.read_unit_info ------------------------------------------------------
// The unit_infos, decoded by the .cmi Reader extended to Clambda's types
// (one object per marshaled block where OCaml's identity matters: the
// function descriptions -- mutable -- and the approximations, which the
// current unit's approximation may re-export).
#include "cppcaml/typing/cmx_format.hpp"

namespace cppcaml::typing::cmi_format {
namespace {

using namespace clambda;

// Clambda_primitives.primitive's constructors with arguments (numbered
// apart from the constant ones, each in declaration order)
bool prim_has_args(Primitive::K k) {
  using K = Primitive::K;
  switch (k) {
    case K::Pread_symbol: case K::Pmakeblock: case K::Pmakelazyblock: case K::Pfield: case K::Psetfield:
    case K::Psetfield_computed: case K::Pfloatfield: case K::Psetfloatfield: case K::Pduprecord:
    case K::Pccall: case K::Praise: case K::Pdivint: case K::Pmodint: case K::Pintcomp:
    case K::Pcompare_bints: case K::Poffsetint: case K::Poffsetref: case K::Pfloatcomp: case K::Pmakearray:
    case K::Pduparray: case K::Parraylength: case K::Parrayrefu: case K::Parraysetu: case K::Parrayrefs:
    case K::Parraysets: case K::Pbintofint: case K::Pintofbint: case K::Pcvtbint: case K::Pnegbint:
    case K::Paddbint: case K::Psubbint: case K::Pmulbint: case K::Pdivbint: case K::Pmodbint:
    case K::Pandbint: case K::Porbint: case K::Pxorbint: case K::Plslbint: case K::Plsrbint:
    case K::Pasrbint: case K::Pbintcomp: case K::Pbigarrayref: case K::Pbigarrayset: case K::Pbigarraydim:
    case K::Pstring_load: case K::Pbytes_load: case K::Pbytes_set: case K::Pbigstring_load:
    case K::Pbigstring_set: case K::Pbbswap:
      return true;
    default: return false;
  }
}

// the constructor of an immediate / a block tag
Primitive::K prim_kind(bool block, long n) {
  long k = 0;
  for (int i = 0; i <= static_cast<int>(Primitive::K::Ppoll); ++i) {
    auto kind = static_cast<Primitive::K>(i);
    if (prim_has_args(kind) == block) {
      if (k == n) return kind;
      ++k;
    }
  }
  throw Corrupt{};
}

class CmxReader : public Reader {
 public:
  using Reader::Reader;

  lambda::ValueKind vk(std::size_t id) {
    lambda::ValueKind k;
    if (is_int(id)) {
      switch (ival(id)) {
        case 0: k.kind = lambda::ValueKind::Kind::Pgenval; break;
        case 1: k.kind = lambda::ValueKind::Kind::Pfloatval; break;
        default: k.kind = lambda::ValueKind::Kind::Pintval; break;
      }
    } else {
      k.kind = lambda::ValueKind::Kind::Pboxedintval;
      k.bi = static_cast<BoxedInteger>(ival(f(id, 0)));
      // input_value makes one block per marshaled block, which output_value
      // shares again
      static std::uint32_t next_origin = 0;
      auto [it, fresh] = boxedint_origins_.try_emplace(id, 0);
      if (fresh) it->second = ++next_origin;
      k.origin = it->second;
    }
    return k;
  }
  std::unordered_map<std::size_t, std::uint32_t> boxedint_origins_;

  debuginfo::scopes scopes(std::size_t id) {
    if (is_int(id)) return nullptr;  // Empty
    if (auto it = scopes_.find(id); it != scopes_.end()) return it->second;
    auto* s = make<debuginfo::Scopes>(debuginfo::Scopes{static_cast<debuginfo::Scopes::Item>(ival(f(id, 0))),
                                                         str(f(id, 1)), str(f(id, 2))});
    scopes_[id] = s;
    return s;
  }
  debuginfo::t dbg(std::size_t id) {
    debuginfo::t r = list<debuginfo::Item>(id, [&](std::size_t x) {
      return debuginfo::Item{str(f(x, 0)), ival(f(x, 1)), ival(f(x, 2)), ival(f(x, 3)),
                             ival(f(x, 4)), ival(f(x, 5)), ival(f(x, 6)), scopes(f(x, 7))};
    });
    // the cells' and items' identities (lists here share tails and items)
    if (!r.empty() && !debuginfo::has_shape(r)) {
      std::vector<std::uint64_t> cells, items;
      for (std::size_t c = id; !is_int(c); c = f(c, 1)) {
        cells.push_back(key_of(c));
        items.push_back(key_of(f(c, 0)));
      }
      debuginfo::set_shape(r, std::move(cells), std::move(items));
    }
    return r;
  }
  std::uint64_t key_of(std::size_t node) {
    auto [it, fresh] = keys_.try_emplace(node, 0);
    if (fresh) it->second = debuginfo::fresh_key();
    return it->second;
  }
  std::unordered_map<std::size_t, std::uint64_t> keys_;

  VarWithProvenance vp(std::size_t id) {
    if (tag(id) == 0) return {ident(f(id, 0)), nullptr};
    std::size_t pr = f(id, 1);
    auto* p = make<Provenance>(Provenance{path(f(pr, 0)), dbg(f(pr, 1)), ident(f(pr, 2))});
    return {ident(f(id, 0)), p};
  }
  UParam uparam(std::size_t id) { return {vp(f(id, 0)), vk(f(id, 1))}; }

  UConstant uconstant(std::size_t id) {
    if (auto it = uc_.find(id); it != uc_.end()) return it->second;
    UConstant c;
    if (tag(id) == 1) c = uconst_int(ival(f(id, 0)));
    else {
      std::size_t o = f(id, 1);
      c = uconst_ref(str(f(id, 0)), is_int(o) ? nullptr : structured(f(o, 0)));
    }
    uc_[id] = c;
    return c;
  }
  std::unordered_map<std::size_t, UConstant> uc_;
  const UStructuredConstant* structured(std::size_t id) {
    if (auto it = sc_.find(id); it != sc_.end()) return it->second;
    auto* c = make<UStructuredConstant>();
    sc_[id] = c;
    using K = UStructuredConstant::Kind;
    c->kind = static_cast<K>(tag(id));
    switch (c->kind) {
      case K::Uconst_float: c->f = dbl(f(id, 0)); break;
      case K::Uconst_int32:
      case K::Uconst_int64:
      case K::Uconst_nativeint: c->i = ival(f(id, 0)); break;
      case K::Uconst_block:
        c->tag = ival(f(id, 0));
        c->fields = list<UConstant>(f(id, 1), [&](std::size_t x) { return uconstant(x); });
        break;
      case K::Uconst_float_array:
        c->floats = list<double>(f(id, 0), [&](std::size_t x) { return dbl(x); });
        break;
      case K::Uconst_string: c->s = str(f(id, 0)); break;
      case K::Uconst_closure:
        c->funs = list<const UFunction*>(f(id, 0), [&](std::size_t x) { return ufunction(x); });
        c->s = str(f(id, 1));
        c->fields = list<UConstant>(f(id, 2), [&](std::size_t x) { return uconstant(x); });
        break;
    }
    return c;
  }
  double dbl(std::size_t id) {
    if (cmi_marshal::is_imm(id) || graph().node(id).kind != cmi_marshal::Kind::Double) throw Corrupt{};
    double d;
    std::uint64_t bits = graph().node(id).v;
    std::memcpy(&d, &bits, sizeof d);
    return d;
  }

  const UFunction* ufunction(std::size_t id) {
    if (auto it = fun_.find(id); it != fun_.end()) return it->second;
    auto* u = make<UFunction>();
    fun_[id] = u;
    u->label = str(f(id, 0));
    u->arity = ival(f(id, 1));
    u->params = list<UParam>(f(id, 2), [&](std::size_t x) { return uparam(x); });
    u->return_ = vk(f(id, 3));
    u->body = ulam(f(id, 4));
    u->dbg = dbg(f(id, 5));
    std::size_t e = f(id, 6);
    u->env = is_int(e) ? nullptr : ident(f(e, 0));
    u->poll = static_cast<lambda::PollAttribute>(ival(f(id, 7)));
    return u;
  }

  RecordRepresentation record_rep(std::size_t r) {
    using RK = RecordRepresentation::Kind;
    RecordRepresentation rr;
    if (is_int(r)) {
      rr.kind = ival(r) == 0 ? RK::Record_regular : RK::Record_float;
      return rr;
    }
    switch (tag(r)) {
      case 0: rr.kind = RK::Record_unboxed; rr.unboxed_inlined = boolean(f(r, 0)); break;
      case 1: rr.kind = RK::Record_inlined; rr.inlined_tag = ival(f(r, 0)); break;
      default: rr.kind = RK::Record_extension; rr.extension = path(f(r, 0)); break;
    }
    return rr;
  }
  const PrimitiveDescription* prim_desc(std::size_t p) {
    if (auto it = pd_.find(p); it != pd_.end()) return it->second;
    auto* d = make<PrimitiveDescription>(str(f(p, 0)), ival(f(p, 1)), boolean(f(p, 2)), str(f(p, 3)),
                                         list<NativeRepr>(f(p, 4), [&](std::size_t x) { return native_repr(x); }),
                                         native_repr(f(p, 5)));
    pd_[p] = d;
    return d;
  }
  void sized(Primitive& p, std::size_t t) {  // (memory_access_size * is_safe)
    p.size = static_cast<MemoryAccessSize>(ival(f(t, 0)));
    p.safe = static_cast<lambda::IsSafe>(ival(f(t, 1)));
  }

  Primitive primitive(std::size_t id) {
    if (is_int(id)) return Primitive{prim_kind(false, ival(id))};
    // one identity per marshaled block
    if (auto it = pr_.find(id); it != pr_.end()) return it->second;
    Primitive p = primitive_(id);
    pr_[id] = p;
    return p;
  }
  std::unordered_map<std::size_t, Primitive> pr_;
  Primitive primitive_(std::size_t id) {
    using K = Primitive::K;
    Primitive p{prim_kind(true, tag(id))};
    auto I = [&](std::size_t k) { return ival(f(id, k)); };
    switch (p.kind) {
      case K::Pread_symbol: p.sym = str(f(id, 0)); break;
      case K::Pmakeblock: {
        p.n = I(0);
        p.mut = mutable_flag(f(id, 1));
        std::size_t sh = f(id, 2);
        if (!is_int(sh)) {
          p.shape.some = true;
          p.shape.kinds = list<lambda::ValueKind>(f(sh, 0), [&](std::size_t x) { return vk(x); });
        }
        break;
      }
      case K::Pmakelazyblock: p.lazy_tag = static_cast<lambda::LazyBlockTag>(I(0)); break;
      case K::Pfield:
        p.n = I(0);
        p.ptr = static_cast<lambda::ImmediateOrPointer>(I(1));
        p.mut = mutable_flag(f(id, 2));
        break;
      case K::Psetfield:
        p.n = I(0);
        p.ptr = static_cast<lambda::ImmediateOrPointer>(I(1));
        p.init = static_cast<lambda::InitializationOrAssignment>(I(2));
        break;
      case K::Psetfield_computed:
        p.ptr = static_cast<lambda::ImmediateOrPointer>(I(0));
        p.init = static_cast<lambda::InitializationOrAssignment>(I(1));
        break;
      case K::Pfloatfield: p.n = I(0); break;
      case K::Psetfloatfield: p.n = I(0); p.init = static_cast<lambda::InitializationOrAssignment>(I(1)); break;
      case K::Pduprecord: p.repr = record_rep(f(id, 0)); p.n = I(1); break;
      case K::Pccall: p.ccall = prim_desc(f(id, 0)); break;
      case K::Praise: p.raise = static_cast<lambda::RaiseKind>(I(0)); break;
      case K::Pdivint:
      case K::Pmodint: p.safe = static_cast<lambda::IsSafe>(I(0)); break;
      case K::Pintcomp: p.icmp = static_cast<lambda::IntegerComparison>(I(0)); break;
      case K::Pcompare_bints: p.bi = static_cast<BoxedInteger>(I(0)); break;
      case K::Poffsetint:
      case K::Poffsetref: p.n = I(0); break;
      case K::Pfloatcomp: p.fcmp = static_cast<lambda::FloatComparison>(I(0)); break;
      case K::Pmakearray:
      case K::Pduparray: p.array = static_cast<lambda::ArrayKind>(I(0)); p.mut = mutable_flag(f(id, 1)); break;
      case K::Parraylength: case K::Parrayrefu: case K::Parraysetu: case K::Parrayrefs: case K::Parraysets:
        p.array = static_cast<lambda::ArrayKind>(I(0));
        break;
      case K::Pbintofint: case K::Pintofbint: case K::Pnegbint: case K::Paddbint: case K::Psubbint:
      case K::Pmulbint: case K::Pandbint: case K::Porbint: case K::Pxorbint: case K::Plslbint: case K::Plsrbint:
      case K::Pasrbint: case K::Pbbswap:
        p.bi = static_cast<BoxedInteger>(I(0));
        break;
      case K::Pcvtbint: p.bi = static_cast<BoxedInteger>(I(0)); p.bi2 = static_cast<BoxedInteger>(I(1)); break;
      case K::Pdivbint:
      case K::Pmodbint:  // { size; is_safe }
        p.bi = static_cast<BoxedInteger>(I(0));
        p.safe = static_cast<lambda::IsSafe>(I(1));
        break;
      case K::Pbintcomp: p.bi = static_cast<BoxedInteger>(I(0)); p.icmp = static_cast<lambda::IntegerComparison>(I(1)); break;
      case K::Pbigarrayref:
      case K::Pbigarrayset:
        p.unsafe = boolean(f(id, 0));
        p.n = I(1);
        p.ba_kind = static_cast<lambda::BigarrayKind>(I(2));
        p.ba_layout = static_cast<lambda::BigarrayLayout>(I(3));
        break;
      case K::Pbigarraydim: p.n = I(0); break;
      case K::Pstring_load: case K::Pbytes_load: case K::Pbytes_set: case K::Pbigstring_load:
      case K::Pbigstring_set:
        sized(p, f(id, 0));
        break;
      default: throw Corrupt{};
    }
    return p;
  }

  template <class T, class F>
  Slice<T> array(std::size_t id, F&& elt) {  // an array: a block (Atom 0 when empty)
    if (is_int(id)) return {};
    std::size_t n = graph().node(id).n;
    std::vector<T> v;
    for (std::size_t k = 0; k < n; ++k) v.push_back(elt(f(id, k)));
    return slice(v);
  }
  std::unordered_map<std::size_t, Slice<long>> index_arrays_;

  ulambda ulam(std::size_t id) {
    if (is_int(id)) return uunreachable();  // the constant constructor
    if (auto it = ul_.find(id); it != ul_.end()) return it->second;
    ulambda r = ulam_raw(id);
    ul_[id] = r;
    return r;
  }
  Slice<ulambda> ulams(std::size_t id) { return list<ulambda>(id, [&](std::size_t x) { return ulam(x); }); }
  ulambda ulam_raw(std::size_t id) {
    switch (static_cast<UK>(tag(id))) {
      case UK::Uvar: return uvar(ident(f(id, 0)));
      case UK::Uconst: return uconst(uconstant(f(id, 0)));
      case UK::Udirect_apply: return udirect_apply(str(f(id, 0)), ulams(f(id, 1)), dbg(f(id, 2)));
      case UK::Ugeneric_apply: return ugeneric_apply(ulam(f(id, 0)), ulams(f(id, 1)), dbg(f(id, 2)));
      case UK::Uclosure:
        return uclosure(list<const UFunction*>(f(id, 0), [&](std::size_t x) { return ufunction(x); }),
                        ulams(f(id, 1)));
      case UK::Uoffset: return uoffset(ulam(f(id, 0)), ival(f(id, 1)));
      case UK::Ulet:
        return ulet(mutable_flag(f(id, 0)), vk(f(id, 1)), vp(f(id, 2)), ulam(f(id, 3)), ulam(f(id, 4)));
      case UK::Uphantom_let: {
        std::size_t o = f(id, 1);
        return uphantom_let(vp(f(id, 0)), is_int(o) ? nullptr : phantom(f(o, 0)), ulam(f(id, 2)));
      }
      case UK::Uprim: return uprim(primitive(f(id, 0)), ulams(f(id, 1)), dbg(f(id, 2)));
      case UK::Uswitch: {
        std::size_t sw = f(id, 1);
        USwitch u;
        // one index array per marshaled block (an inlined copy shares its
        // original's)
        auto index = [&](std::size_t a) {
          auto [it, fresh] = index_arrays_.try_emplace(a);
          if (fresh) it->second = array<long>(a, [&](std::size_t x) { return ival(x); });
          return it->second;
        };
        u.us_index_consts = index(f(sw, 0));
        u.us_actions_consts = array<ulambda>(f(sw, 1), [&](std::size_t x) { return ulam(x); });
        u.us_index_blocks = index(f(sw, 2));
        u.us_actions_blocks = array<ulambda>(f(sw, 3), [&](std::size_t x) { return ulam(x); });
        return uswitch(ulam(f(id, 0)), u, dbg(f(id, 2)));
      }
      case UK::Ustringswitch: {
        std::size_t d = f(id, 2);
        return ustringswitch(ulam(f(id, 0)), list<UStringCase>(f(id, 1), [&](std::size_t x) {
                               return UStringCase{str(f(x, 0)), ulam(f(x, 1))};
                             }),
                             is_int(d) ? nullptr : ulam(f(d, 0)));
      }
      case UK::Ustaticfail: return ustaticfail(ival(f(id, 0)), ulams(f(id, 1)));
      case UK::Ucatch:
        return ucatch(ival(f(id, 0)), list<UParam>(f(id, 1), [&](std::size_t x) { return uparam(x); }),
                      ulam(f(id, 2)), ulam(f(id, 3)));
      case UK::Utrywith: return utrywith(ulam(f(id, 0)), vp(f(id, 1)), ulam(f(id, 2)));
      case UK::Uifthenelse: return uifthenelse(ulam(f(id, 0)), ulam(f(id, 1)), ulam(f(id, 2)));
      case UK::Usequence: return usequence(ulam(f(id, 0)), ulam(f(id, 1)));
      case UK::Uwhile: return uwhile(ulam(f(id, 0)), ulam(f(id, 1)));
      case UK::Ufor:
        return ufor(vp(f(id, 0)), ulam(f(id, 1)), ulam(f(id, 2)),
                    static_cast<parsetree::DirectionFlag>(ival(f(id, 3))), ulam(f(id, 4)));
      case UK::Uassign: return uassign(ident(f(id, 0)), ulam(f(id, 1)));
      case UK::Usend:
        return usend(static_cast<lambda::MethKind>(ival(f(id, 0))), ulam(f(id, 1)), ulam(f(id, 2)), ulams(f(id, 3)),
                     dbg(f(id, 4)));
      default: throw Corrupt{};
    }
  }
  const UPhantomDefiningExpr* phantom(std::size_t id) {
    auto* e = make<UPhantomDefiningExpr>();
    e->kind = static_cast<UPhantomDefiningExpr::Kind>(tag(id));
    switch (e->kind) {
      case UPhantomDefiningExpr::Kind::Uphantom_const: e->c = uconstant(f(id, 0)); break;
      case UPhantomDefiningExpr::Kind::Uphantom_var: e->var = ident(f(id, 0)); break;
      case UPhantomDefiningExpr::Kind::Uphantom_offset_var:
      case UPhantomDefiningExpr::Kind::Uphantom_read_field: e->var = ident(f(id, 0)); e->n = ival(f(id, 1)); break;
      case UPhantomDefiningExpr::Kind::Uphantom_read_symbol_field: e->sym = str(f(id, 0)); e->n = ival(f(id, 1)); break;
      case UPhantomDefiningExpr::Kind::Uphantom_block:
        e->n = ival(f(id, 0));
        e->fields = list<Var>(f(id, 1), [&](std::size_t x) { return ident(x); });
        break;
    }
    return e;
  }

  FunctionDescription* fundesc(std::size_t id) {
    if (auto it = fd_.find(id); it != fd_.end()) return it->second;
    auto* d = make<FunctionDescription>();
    fd_[id] = d;
    d->fun_label = str(f(id, 0));
    d->fun_arity = ival(f(id, 1));
    d->fun_closed = boolean(f(id, 2));
    std::size_t in = f(id, 3);
    if (!is_int(in)) {
      std::size_t pair = f(in, 0);
      d->has_inline = true;
      d->inline_params = list<VarWithProvenance>(f(pair, 0), [&](std::size_t x) { return vp(x); });
      d->inline_body = ulam(f(pair, 1));
    }
    d->fun_float_const_prop = boolean(f(id, 4));
    d->fun_poll = static_cast<lambda::PollAttribute>(ival(f(id, 5)));
    return d;
  }

  const ValueApproximation* approx(std::size_t id) {
    using K = ValueApproximation::Kind;
    if (is_int(id)) return value_unknown();
    if (auto it = ap_.find(id); it != ap_.end()) return it->second;
    auto* a = make<ValueApproximation>();
    ap_[id] = a;
    switch (tag(id)) {
      case 0:
        a->kind = K::Value_closure;
        a->fundesc = fundesc(f(id, 0));
        a->res = approx(f(id, 1));
        break;
      case 1:
        a->kind = K::Value_tuple;
        a->tuple = array<const ValueApproximation*>(f(id, 0), [&](std::size_t x) { return approx(x); });
        break;
      case 2: a->kind = K::Value_const; a->c = uconstant(f(id, 0)); break;
      case 3: a->kind = K::Value_global_field; a->sym = str(f(id, 0)); a->field = ival(f(id, 1)); break;
      default: throw Corrupt{};
    }
    return a;
  }

  cmx_format::Crcs crcs(std::size_t id) {
    return list_vec<std::pair<std::string_view, std::optional<std::string>>>(id, [&](std::size_t e) {
      std::pair<std::string_view, std::optional<std::string>> p;
      p.first = str(f(e, 0));
      std::size_t d = f(e, 1);
      if (!is_int(d)) p.second = std::string(str(f(d, 0)));
      return p;
    });
  }
  std::vector<long> ints(std::size_t id) {
    return list_vec<long>(id, [&](std::size_t x) { return ival(x); });
  }

  cmx_format::UnitInfos* unit_infos(std::size_t id) {
    auto* ui = make<cmx_format::UnitInfos>();
    ui->ui_name = str(f(id, 0));
    ui->ui_symbol = str(f(id, 1));
    ui->ui_defines = list_vec<std::string_view>(f(id, 2), [&](std::size_t x) { return str(x); });
    ui->ui_imports_cmi = crcs(f(id, 3));
    ui->ui_imports_cmx = crcs(f(id, 4));
    ui->ui_curry_fun = ints(f(id, 5));
    ui->ui_apply_fun = ints(f(id, 6));
    ui->ui_send_fun = ints(f(id, 7));
    std::size_t ei = f(id, 8);
    if (tag(ei) != 0) throw Corrupt{};  // Flambda export info
    ui->ui_export_info = approx(f(ei, 0));
    ui->ui_force_link = boolean(f(id, 9));
    std::size_t fp = f(id, 10);
    if (!is_int(fp)) ui->ui_for_pack = str(f(fp, 0));
    ui->ui_need_stdlib = boolean(f(id, 11));
    return ui;
  }

 private:
  std::unordered_map<std::size_t, debuginfo::scopes> scopes_;
  std::unordered_map<std::size_t, const UStructuredConstant*> sc_;
  std::unordered_map<std::size_t, const UFunction*> fun_;
  std::unordered_map<std::size_t, ulambda> ul_;
  std::unordered_map<std::size_t, FunctionDescription*> fd_;
  std::unordered_map<std::size_t, const ValueApproximation*> ap_;
  std::unordered_map<std::size_t, const PrimitiveDescription*> pd_;
};

}  // namespace
}  // namespace cppcaml::typing::cmi_format

namespace cppcaml::typing::cmx_format {

namespace {
std::vector<std::uint8_t> read_bytes(const std::string& filename) {
  std::vector<std::uint8_t> bytes;
  std::FILE* fp = std::fopen(filename.c_str(), "rb");
  if (!fp) throw std::runtime_error(filename + ": No such file or directory");
  std::uint8_t chunk[65536];
  for (std::size_t k; (k = std::fread(chunk, 1, sizeof chunk, fp)) > 0;) bytes.insert(bytes.end(), chunk, chunk + k);
  std::fclose(fp);
  return bytes;
}
}  // namespace

LibraryInfos read_library_info(const std::string& filename) {
  std::vector<std::uint8_t> bytes = read_bytes(filename);
  const std::string magic = config::cmxa_magic_number;
  if (bytes.size() < magic.size() || std::string(bytes.begin(), bytes.begin() + magic.size()) != magic)
    throw Error(Error::Kind::Not_a_unit_info, filename);
  try {
    cmi_marshal::Graph graph;
    std::size_t off = magic.size();
    std::size_t root = cmi_marshal::read_value(bytes.data(), bytes.size(), off, graph);
    cmi_format::CmxReader r(graph);
    LibraryInfos l;
    l.lib_units = r.list_vec<std::pair<UnitInfos*, std::string>>(r.f(root, 0), [&](std::size_t e) {
      return std::pair<UnitInfos*, std::string>(r.unit_infos(r.f(e, 0)), std::string(r.str(r.f(e, 1))));
    });
    auto strs = [&](std::size_t id) {
      return r.list_vec<std::string>(id, [&](std::size_t x) { return std::string(r.str(x)); });
    };
    l.lib_ccobjs = strs(r.f(root, 1));
    l.lib_ccopts = strs(r.f(root, 2));
    return l;
  } catch (const cmi_format::Corrupt&) {
    throw Error(Error::Kind::Corrupted_unit_info, filename);
  } catch (const cppcaml::marshal::Error&) {
    throw Error(Error::Kind::Corrupted_unit_info, filename);
  }
}

std::pair<UnitInfos*, std::string> read_unit_info(const std::string& filename) {
  std::vector<std::uint8_t> bytes = read_bytes(filename);
  const std::string magic = config::cmx_magic_number;
  if (bytes.size() < magic.size()) throw Error(Error::Kind::Corrupted_unit_info, filename);
  if (std::string(bytes.begin(), bytes.begin() + magic.size()) != magic)
    throw Error(Error::Kind::Not_a_unit_info, filename);
  try {
    cmi_marshal::Graph graph;
    std::size_t off = magic.size();
    std::size_t root = cmi_marshal::read_value(bytes.data(), bytes.size(), off, graph);
    cmi_format::CmxReader r(graph);
    UnitInfos* ui = r.unit_infos(root);
    // Digest.BLAKE128.input ic: the 16 bytes that follow
    if (bytes.size() < off + 16) throw Error(Error::Kind::Corrupted_unit_info, filename);
    std::string crc(bytes.begin() + static_cast<long>(off), bytes.begin() + static_cast<long>(off) + 16);
    return {ui, crc};
  } catch (const cmi_format::Corrupt&) {
    throw Error(Error::Kind::Corrupted_unit_info, filename);
  } catch (const cppcaml::marshal::Error&) {
    throw Error(Error::Kind::Corrupted_unit_info, filename);
  }
}

}  // namespace cppcaml::typing::cmx_format


// ---- .cmx: Compilenv.write_unit_info -------------------------------------------------------
// The unit_infos marshaled with the sharing ocamlopt's values have: one
// value per object where the port keeps OCaml's identity (strings by
// storage, idents, paths, structured constants, ulambda nodes, functions,
// function descriptions, approximations, debuginfo lists by storage).
namespace cppcaml::typing::cmx_format {
namespace {

namespace o = cppcaml::omarshal;
using V = o::ValPtr;
using namespace clambda;

class CmxWriter {
 public:
  cmi_format::writer::Writer w;

  V vk(const lambda::ValueKind& k) {
    switch (k.kind) {
      case lambda::ValueKind::Kind::Pgenval: return w.i(0);
      case lambda::ValueKind::Kind::Pfloatval: return w.i(1);
      case lambda::ValueKind::Kind::Pintval: return w.i(2);
      case lambda::ValueKind::Kind::Pboxedintval: {
        // Typeopt's [Pboxedintval Pint32] (...) are literals: one static block
        // each in ocamlopt; one read from a .cmx is its own block
        V& v = boxedint_kinds_[{k.origin, static_cast<int>(k.bi)}];
        if (!v) v = o::vblock(0, {w.i(static_cast<long>(k.bi))});
        return v;
      }
    }
    return w.i(0);
  }
  std::map<std::pair<std::uint32_t, int>, V> boxedint_kinds_;
  std::unordered_map<const long*, V> index_arrays_;

  V scopes(debuginfo::scopes s) {
    if (!s) return w.i(0);
    return memo(s, [&] { return o::vblock(0, {w.i(static_cast<long>(s->item)), w.str(s->str), w.str(s->str_fun)}); });
  }
  // a debuginfo list with its cells' and items' identities (debuginfo::cell_key)
  V dbg(const debuginfo::t& d) {
    V tail = w.i(0);
    for (std::size_t k = d.size(); k-- > 0;) {
      std::uint64_t ck = debuginfo::cell_key(d, k);
      if (auto it = dbg_cells_.find(ck); it != dbg_cells_.end()) {
        tail = it->second;
        continue;
      }
      std::uint64_t ik = debuginfo::item_key(d, k);
      V item;
      if (auto it = dbg_items_.find(ik); it != dbg_items_.end()) item = it->second;
      else {
        const debuginfo::Item& x = d[k];
        item = o::vblock(0, {w.str(x.dinfo_file), w.i(x.dinfo_line), w.i(x.dinfo_char_start), w.i(x.dinfo_char_end),
                             w.i(x.dinfo_start_bol), w.i(x.dinfo_end_bol), w.i(x.dinfo_end_line), scopes(x.dinfo_scopes)});
        dbg_items_[ik] = item;
      }
      V cell = o::vblock(0, {item, tail});
      dbg_cells_[ck] = cell;
      tail = cell;
    }
    return tail;
  }
  std::unordered_map<std::uint64_t, V> dbg_cells_, dbg_items_;

  V vp(const VarWithProvenance& v) {
    if (!v.provenance) return o::vblock(0, {w.ident(v.var)});
    const Provenance* p = v.provenance;
    V pv = memo(p, [&] { return o::vblock(0, {w.path(p->module_path), dbg(p->location), w.ident(p->original_ident)}); });
    return o::vblock(1, {w.ident(v.var), pv});
  }
  V uparam(const UParam& p) { return o::vblock(0, {vp(p.var), vk(p.kind)}); }

  V uconstant(const UConstant& c) {
    auto make = [&] {
      if (c.kind == UConstant::Kind::Uconst_int) return o::vblock(1, {w.i(c.i)});
      return o::vblock(0, {w.str(c.sym), c.sc ? w.some(structured(c.sc)) : w.none()});
    };
    if (!c.id) return make();
    if (auto it = uconsts_.find(c.id); it != uconsts_.end()) return it->second;
    V v = make();
    uconsts_[c.id] = v;
    return v;
  }
  std::unordered_map<unsigned long, V> uconsts_;

  static V boxed(char k, std::int64_t n) {
    std::string raw;
    auto be32 = [&](std::uint32_t x) {
      for (int s = 3; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
    };
    auto be64 = [&](std::uint64_t x) {
      for (int s = 7; s >= 0; --s) raw.push_back(static_cast<char>((x >> (8 * s)) & 0xff));
    };
    raw.push_back(static_cast<char>(0x19));  // CODE_CUSTOM_FIXED
    if (k == 'i') {
      raw += "_i";
      raw.push_back('\0');
      be32(static_cast<std::uint32_t>(n));
      return o::vcustom2(raw, 4, 4);
    }
    if (k == 'j') {
      raw += "_j";
      raw.push_back('\0');
      be64(static_cast<std::uint64_t>(n));
      return o::vcustom2(raw, 8, 8);
    }
    raw += "_n";
    raw.push_back('\0');
    if (n >= INT32_MIN && n <= INT32_MAX) {
      raw.push_back(1);
      be32(static_cast<std::uint32_t>(n));
    } else {
      raw.push_back(2);
      be64(static_cast<std::uint64_t>(n));
    }
    return o::vcustom2(raw, 4, 8);
  }

  V structured(const UStructuredConstant* c) {
    using K = UStructuredConstant::Kind;
    return memo(c, [&]() -> V {
      int tag = static_cast<int>(c->kind);
      switch (c->kind) {
        case K::Uconst_float: return o::vblock(tag, {o::vdbl(c->f)});
        case K::Uconst_int32: return o::vblock(tag, {boxed('i', c->i)});
        case K::Uconst_int64: return o::vblock(tag, {boxed('j', c->i)});
        case K::Uconst_nativeint: return o::vblock(tag, {boxed('n', c->i)});
        case K::Uconst_block:
          return o::vblock(tag, {w.i(c->tag), w.list(c->fields, [&](const UConstant& u) { return uconstant(u); })});
        case K::Uconst_float_array:
          return o::vblock(tag, {w.list(c->floats, [&](double d) { return o::vdbl(d); })});
        case K::Uconst_string: return o::vblock(tag, {w.str(c->s)});
        case K::Uconst_closure:
          return o::vblock(tag, {w.list(c->funs, [&](const UFunction* f) { return ufunction(f); }), w.str(c->s),
                                 w.list(c->fields, [&](const UConstant& u) { return uconstant(u); })});
      }
      return w.i(0);
    });
  }

  V ufunction(const UFunction* f) {
    return memo(f, [&] {
      return o::vblock(0, {w.str(f->label), w.i(f->arity), w.list(f->params, [&](const UParam& p) { return uparam(p); }),
                           vk(f->return_), ulam(f->body), dbg(f->dbg), f->env ? w.some(w.ident(f->env)) : w.none(),
                           w.i(static_cast<long>(f->poll))});
    });
  }

  V record_rep(const RecordRepresentation& r) {
    using RK = RecordRepresentation::Kind;
    switch (r.kind) {
      case RK::Record_regular: return w.i(0);
      case RK::Record_float: return w.i(1);
      case RK::Record_unboxed: return o::vblock(0, {w.b(r.unboxed_inlined)});
      case RK::Record_inlined: return o::vblock(1, {w.i(r.inlined_tag)});
      default: return o::vblock(2, {w.path(r.extension)});
    }
  }

  V primitive(const Primitive& p) {
    if (auto it = prims_.find(p.id); it != prims_.end()) return it->second;
    V v = primitive_(p);
    prims_[p.id] = v;
    return v;
  }
  std::unordered_map<unsigned long, V> prims_;
  V primitive_(const Primitive& p) {
    using K = Primitive::K;
    // numbering: constant and non-constant constructors apart, in declaration order
    auto has_args = [](K k) {
      switch (k) {
        case K::Pread_symbol: case K::Pmakeblock: case K::Pmakelazyblock: case K::Pfield: case K::Psetfield:
        case K::Psetfield_computed: case K::Pfloatfield: case K::Psetfloatfield: case K::Pduprecord:
        case K::Pccall: case K::Praise: case K::Pdivint: case K::Pmodint: case K::Pintcomp:
        case K::Pcompare_bints: case K::Poffsetint: case K::Poffsetref: case K::Pfloatcomp: case K::Pmakearray:
        case K::Pduparray: case K::Parraylength: case K::Parrayrefu: case K::Parraysetu: case K::Parrayrefs:
        case K::Parraysets: case K::Pbintofint: case K::Pintofbint: case K::Pcvtbint: case K::Pnegbint:
        case K::Paddbint: case K::Psubbint: case K::Pmulbint: case K::Pdivbint: case K::Pmodbint:
        case K::Pandbint: case K::Porbint: case K::Pxorbint: case K::Plslbint: case K::Plsrbint:
        case K::Pasrbint: case K::Pbintcomp: case K::Pbigarrayref: case K::Pbigarrayset: case K::Pbigarraydim:
        case K::Pstring_load: case K::Pbytes_load: case K::Pbytes_set: case K::Pbigstring_load:
        case K::Pbigstring_set: case K::Pbbswap:
          return true;
        default: return false;
      }
    };
    bool block = has_args(p.kind);
    long n = 0;
    for (int k = 0; k < static_cast<int>(p.kind); ++k)
      if (has_args(static_cast<K>(k)) == block) ++n;
    if (!block) return w.i(n);
    int tag = static_cast<int>(n);
    auto I = [&](long x) { return w.i(x); };
    auto sized = [&]() { return o::vblock(0, {I(static_cast<long>(p.size)), I(static_cast<long>(p.safe))}); };
    switch (p.kind) {
      case K::Pread_symbol: return o::vblock(tag, {w.str(p.sym)});
      case K::Pmakeblock:
        return o::vblock(tag, {I(p.n), w.mutable_flag(p.mut),
                               p.shape.some ? w.some(w.list(p.shape.kinds, [&](const lambda::ValueKind& k) { return vk(k); }))
                                            : w.none()});
      case K::Pmakelazyblock: return o::vblock(tag, {I(static_cast<long>(p.lazy_tag))});
      case K::Pfield: return o::vblock(tag, {I(p.n), I(static_cast<long>(p.ptr)), w.mutable_flag(p.mut)});
      case K::Psetfield: return o::vblock(tag, {I(p.n), I(static_cast<long>(p.ptr)), I(static_cast<long>(p.init))});
      case K::Psetfield_computed: return o::vblock(tag, {I(static_cast<long>(p.ptr)), I(static_cast<long>(p.init))});
      case K::Pfloatfield: return o::vblock(tag, {I(p.n)});
      case K::Psetfloatfield: return o::vblock(tag, {I(p.n), I(static_cast<long>(p.init))});
      case K::Pduprecord: return o::vblock(tag, {record_rep(p.repr), I(p.n)});
      case K::Pccall: return o::vblock(tag, {w.prim_desc(p.ccall)});
      case K::Praise: return o::vblock(tag, {I(static_cast<long>(p.raise))});
      case K::Pdivint:
      case K::Pmodint: return o::vblock(tag, {I(static_cast<long>(p.safe))});
      case K::Pintcomp: return o::vblock(tag, {I(static_cast<long>(p.icmp))});
      case K::Pcompare_bints: return o::vblock(tag, {I(static_cast<long>(p.bi))});
      case K::Poffsetint:
      case K::Poffsetref: return o::vblock(tag, {I(p.n)});
      case K::Pfloatcomp: return o::vblock(tag, {I(static_cast<long>(p.fcmp))});
      case K::Pmakearray:
      case K::Pduparray: return o::vblock(tag, {I(static_cast<long>(p.array)), w.mutable_flag(p.mut)});
      case K::Pcvtbint: return o::vblock(tag, {I(static_cast<long>(p.bi)), I(static_cast<long>(p.bi2))});
      case K::Pdivbint:
      case K::Pmodbint: return o::vblock(tag, {I(static_cast<long>(p.bi)), I(static_cast<long>(p.safe))});
      case K::Pbintcomp: return o::vblock(tag, {I(static_cast<long>(p.bi)), I(static_cast<long>(p.icmp))});
      case K::Pbigarrayref:
      case K::Pbigarrayset:
        return o::vblock(tag, {w.b(p.unsafe), I(p.n), I(static_cast<long>(p.ba_kind)), I(static_cast<long>(p.ba_layout))});
      case K::Pbigarraydim: return o::vblock(tag, {I(p.n)});
      case K::Pstring_load: case K::Pbytes_load: case K::Pbytes_set: case K::Pbigstring_load:
      case K::Pbigstring_set:
        return o::vblock(tag, {sized()});
      case K::Parraylength: case K::Parrayrefu: case K::Parraysetu: case K::Parrayrefs: case K::Parraysets:
        return o::vblock(tag, {I(static_cast<long>(p.array))});
      default: return o::vblock(tag, {I(static_cast<long>(p.bi))});  // the boxed-integer operations
    }
  }

  V ulams(const Slice<ulambda>& l) { return w.list(l, [&](ulambda u) { return ulam(u); }); }
  // an array: one value per storage (a switch's index arrays, kept by
  // Closure's substitution)
  template <class T, class F>
  V array(const Slice<T>& a, F&& elt) {
    std::vector<V> xs;
    if (a.empty()) return o::vblock(0, xs);
    if (auto it = arrays_.find(a.p); it != arrays_.end()) return it->second;
    for (auto& x : a) xs.push_back(elt(x));
    V v = o::vblock(0, xs);
    arrays_[a.p] = v;
    return v;
  }
  std::unordered_map<const void*, V> arrays_;

  V ulam(ulambda u) {
    if (u->kind == UK::Uunreachable) return w.i(0);
    return memo(u, [&]() -> V {
      int tag = static_cast<int>(u->kind);
      switch (u->kind) {
        case UK::Uvar: return o::vblock(tag, {w.ident(static_cast<const Uvar*>(u)->id)});
        case UK::Uconst: return o::vblock(tag, {uconstant(static_cast<const Uconst*>(u)->c)});
        case UK::Udirect_apply: {
          auto* x = static_cast<const Udirect_apply*>(u);
          return o::vblock(tag, {w.str(x->f), ulams(x->args), dbg(x->dbg)});
        }
        case UK::Ugeneric_apply: {
          auto* x = static_cast<const Ugeneric_apply*>(u);
          return o::vblock(tag, {ulam(x->f), ulams(x->args), dbg(x->dbg)});
        }
        case UK::Uclosure: {
          auto* x = static_cast<const Uclosure*>(u);
          return o::vblock(tag, {w.list(x->funs, [&](const UFunction* f) { return ufunction(f); }), ulams(x->fv)});
        }
        case UK::Uoffset: {
          auto* x = static_cast<const Uoffset*>(u);
          return o::vblock(tag, {ulam(x->l), w.i(x->ofs)});
        }
        case UK::Ulet: {
          auto* x = static_cast<const Ulet*>(u);
          return o::vblock(tag, {w.mutable_flag(x->mut), vk(x->k), vp(x->id), ulam(x->arg), ulam(x->body)});
        }
        case UK::Uprim: {
          auto* x = static_cast<const Uprim*>(u);
          return o::vblock(tag, {primitive(x->p), ulams(x->args), dbg(x->dbg)});
        }
        case UK::Uswitch: {
          auto* x = static_cast<const Uswitch*>(u);
          // Closure.substitute keeps a switch's index arrays: one value each
          auto ints = [&](const Slice<long>& a) {
            if (a.empty()) return array(a, [&](long n) { return w.i(n); });
            V& v = index_arrays_[a.begin()];
            if (!v) v = array(a, [&](long n) { return w.i(n); });
            return v;
          };
          auto acts = [&](const Slice<ulambda>& a) { return array(a, [&](ulambda y) { return ulam(y); }); };
          V sw = o::vblock(0, {ints(x->sw.us_index_consts), acts(x->sw.us_actions_consts), ints(x->sw.us_index_blocks),
                               acts(x->sw.us_actions_blocks)});
          return o::vblock(tag, {ulam(x->arg), sw, dbg(x->dbg)});
        }
        case UK::Ustringswitch: {
          auto* x = static_cast<const Ustringswitch*>(u);
          return o::vblock(tag, {ulam(x->arg),
                                 w.list(x->cases, [&](const UStringCase& c) { return o::vblock(0, {w.str(c.s), ulam(c.action)}); }),
                                 x->def ? w.some(ulam(x->def)) : w.none()});
        }
        case UK::Ustaticfail: {
          auto* x = static_cast<const Ustaticfail*>(u);
          return o::vblock(tag, {w.i(x->i), ulams(x->args)});
        }
        case UK::Ucatch: {
          auto* x = static_cast<const Ucatch*>(u);
          return o::vblock(tag, {w.i(x->i), w.list(x->vars, [&](const UParam& p) { return uparam(p); }), ulam(x->body),
                                 ulam(x->handler)});
        }
        case UK::Utrywith: {
          auto* x = static_cast<const Utrywith*>(u);
          return o::vblock(tag, {ulam(x->body), vp(x->exn), ulam(x->handler)});
        }
        case UK::Uifthenelse: {
          auto* x = static_cast<const Uifthenelse*>(u);
          return o::vblock(tag, {ulam(x->cond), ulam(x->ifso), ulam(x->ifnot)});
        }
        case UK::Usequence: {
          auto* x = static_cast<const Usequence*>(u);
          return o::vblock(tag, {ulam(x->l1), ulam(x->l2)});
        }
        case UK::Uwhile: {
          auto* x = static_cast<const Uwhile*>(u);
          return o::vblock(tag, {ulam(x->cond), ulam(x->body)});
        }
        case UK::Ufor: {
          auto* x = static_cast<const Ufor*>(u);
          return o::vblock(tag, {vp(x->id), ulam(x->lo), ulam(x->hi), w.i(static_cast<long>(x->dir)), ulam(x->body)});
        }
        case UK::Uassign: {
          auto* x = static_cast<const Uassign*>(u);
          return o::vblock(tag, {w.ident(x->id), ulam(x->e)});
        }
        case UK::Usend: {
          auto* x = static_cast<const Usend*>(u);
          return o::vblock(tag, {w.i(static_cast<long>(x->k)), ulam(x->met), ulam(x->obj), ulams(x->args), dbg(x->dbg)});
        }
        default: throw std::runtime_error("Cmx writer: Uphantom_let");
      }
    });
  }

  V fundesc(const FunctionDescription* d) {
    return memo(d, [&] {
      V inl = d->has_inline
                  ? w.some(o::vblock(0, {w.list(d->inline_params, [&](const VarWithProvenance& v) { return vp(v); }),
                                         ulam(d->inline_body)}))
                  : w.none();
      return o::vblock(0, {w.str(d->fun_label), w.i(d->fun_arity), w.b(d->fun_closed), inl, w.b(d->fun_float_const_prop),
                           w.i(static_cast<long>(d->fun_poll))});
    });
  }

  V approx(const ValueApproximation* a) {
    using K = ValueApproximation::Kind;
    if (a->kind == K::Value_unknown) return w.i(0);
    return memo(a, [&]() -> V {
      switch (a->kind) {
        case K::Value_closure: return o::vblock(0, {fundesc(a->fundesc), approx(a->res)});
        case K::Value_tuple: return o::vblock(1, {array(a->tuple, [&](const ValueApproximation* x) { return approx(x); })});
        case K::Value_const: return o::vblock(2, {uconstant(a->c)});
        default: return o::vblock(3, {w.str(a->sym), w.i(a->field)});
      }
    });
  }

  V crcs(const Crcs& l) {
    return w.list(l, [&](const std::pair<std::string_view, std::optional<std::string>>& e) {
      return o::vblock(0, {w.str(e.first), e.second ? w.some(o::vstr(*e.second)) : w.none()});
    });
  }
  V ints(const std::vector<long>& l) {
    return w.list(l, [&](long n) { return w.i(n); });
  }

 private:
  std::unordered_map<const void*, V> memo_;
  template <class F>
  V memo(const void* key, F&& make) {
    if (auto it = memo_.find(key); it != memo_.end()) return it->second;
    V v = make();
    memo_[key] = v;
    return v;
  }
};

}  // namespace

namespace {
V unit_infos(CmxWriter& cw, const UnitInfos& ui, const V& export_info) {
  auto& w = cw.w;
  return o::vblock(0, {w.str(ui.ui_name), w.str(ui.ui_symbol),
                       w.list(ui.ui_defines, [&](std::string_view s) { return w.str(s); }), cw.crcs(ui.ui_imports_cmi),
                       cw.crcs(ui.ui_imports_cmx), cw.ints(ui.ui_curry_fun), cw.ints(ui.ui_apply_fun),
                       cw.ints(ui.ui_send_fun), export_info, w.b(ui.ui_force_link),
                       ui.ui_for_pack ? w.some(w.str(*ui.ui_for_pack)) : w.none(), w.b(ui.ui_need_stdlib)});
}
}  // namespace

std::string write_unit_info(const UnitInfos& ui) {
  CmxWriter cw;
  V info = unit_infos(cw, ui, o::vblock(0, {cw.approx(ui.ui_export_info)}));
  std::vector<std::uint8_t> bytes = o::marshal(info);
  std::string file = config::cmx_magic_number;
  file.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  // Digest.BLAKE128.file filename, after the flush
  file += blake2::blake128(reinterpret_cast<const unsigned char*>(file.data()), file.size());
  return file;
}

std::string write_library_info(const LibraryInfos& l) {
  CmxWriter cw;
  auto& w = cw.w;
  V default_export_info = o::vblock(0, {w.i(0)});  // Clambda Value_unknown
  std::vector<V> units;
  for (auto& [ui, crc] : l.lib_units)
    units.push_back(o::vblock(0, {unit_infos(cw, *ui, default_export_info), o::vstr(crc)}));
  auto strs = [&](const std::vector<std::string>& xs) {
    std::vector<V> v;
    for (auto& x : xs) v.push_back(o::vstr(x));
    return o::vlist(v);
  };
  V infos = o::vblock(0, {o::vlist(units), strs(l.lib_ccobjs), strs(l.lib_ccopts)});
  std::vector<std::uint8_t> bytes = o::marshal(infos);
  std::string file = config::cmxa_magic_number;
  file.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return file;
}

}  // namespace cppcaml::typing::cmx_format
