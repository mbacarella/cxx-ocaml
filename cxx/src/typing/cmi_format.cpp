// Port of file_formats/cmi_format.ml (read side).  See cmi_format.hpp.
//
// The decoder follows each OCaml type's marshaled layout: constant
// constructors are immediates numbered in declaration order, non-constant
// ones are blocks tagged in declaration order, records and inline records are
// blocks of their fields in order, `option` is 0 | Some = block tag 0.
#include "cppcaml/typing/cmi_format.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <tuple>
#include <iterator>
#include <unordered_map>

#include <cstdio>

#include "cppcaml/blake2.hpp"
#include "cppcaml/marshal.hpp"
#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/instruct.hpp"

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

class Reader {
 public:
  explicit Reader(const m::Arena& a) : a_(a) {}

  // ---- primitive access ----
  const m::Value& v(std::size_t id) const { return a_[id]; }
  bool is_int(std::size_t id) const { return v(id).kind == m::Value::Kind::Int; }
  long ival(std::size_t id) const {
    if (!is_int(id)) throw Corrupt{};
    return static_cast<long>(v(id).i);
  }
  unsigned tag(std::size_t id) const {
    if (v(id).kind != m::Value::Kind::Block) throw Corrupt{};
    return v(id).tag;
  }
  std::size_t f(std::size_t id, std::size_t k) const {
    const m::Value& x = v(id);
    if (x.kind != m::Value::Kind::Block || k >= x.fields.size()) throw Corrupt{};
    return x.fields[k];
  }
  // one copy per marshaled string: input_value's sharing (a string the
  // .cmi shares stays one string, which output_cmi shares again)
  std::string_view str(std::size_t id) {
    if (v(id).kind != m::Value::Kind::String) throw Corrupt{};
    if (auto it = str_.find(id); it != str_.end()) return it->second;
    std::string_view s = zstr(v(id).str());
    str_[id] = s;
    return s;
  }
  bool boolean(std::size_t id) const { return ival(id) != 0; }

  // one Slice per marshaled list (input_value's sharing of a whole list;
  // a list sharing only its tail with another is not recorded)
  template <class T, class F>
  Slice<T> list(std::size_t id, F&& elt) {
    if (is_int(id)) return {};
    if (auto it = list_memo_.find(id); it != list_memo_.end())
      return Slice<T>{static_cast<const T*>(it->second.first), it->second.second};
    std::size_t start = id;
    std::vector<T> out;
    while (!is_int(id)) {
      out.push_back(elt(f(id, 0)));
      id = f(id, 1);
    }
    Slice<T> r = slice(out);
    list_memo_[start] = {static_cast<const void*>(r.p), r.n};
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
    auto [it, fresh] = optstr_obj_.try_emplace(id, nullptr);
    if (fresh) it->second = zone().alloc(1, 1);
    return OptStr{true, str(f(id, 0)), it->second};
  }

  // ---- Ident / Path ----
  ident::Unscoped* unscoped(std::size_t id) {
    if (auto it = us_.find(id); it != us_.end()) return it->second;
    auto* u = make<ident::Unscoped>(ident::Unscoped::State::Udesc);
    us_[id] = u;
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
    if (auto it = ident_.find(id); it != ident_.end()) return it->second;
    Ident::t r = ident_raw(id);
    ident_[id] = r;
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
    if (auto it = path_.find(id); it != path_.end()) return it->second;
    Path::t r = path_raw(id);
    path_[id] = r;
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
    if (auto it = pos_.find(id); it != pos_.end()) return *it->second;
    auto* p = make<Position>(Position{str(f(id, 0)), ival(f(id, 1)), ival(f(id, 2)), ival(f(id, 3))});
    p->obj = p;
    pos_[id] = p;
    return *p;
  }
  Location loc(std::size_t id) {
    if (auto it = loc_.find(id); it != loc_.end()) return *it->second;
    Position a = position(f(id, 0));
    Position e = position(f(id, 1));
    auto* l = make<Location>(Location{a, e, boolean(f(id, 2))});
    l->obj = l;
    loc_[id] = l;
    return *l;
  }
  Uid uid(std::size_t id) {
    Uid u;
    if (is_int(id)) {  // Internal
      u.kind = Uid::Kind::Internal;
      return u;
    }
    // one identity per marshaled record (input_value's sharing)
    auto [ot, fresh] = uid_obj_.try_emplace(id, nullptr);
    if (fresh) ot->second = zone().alloc(1, 1);
    u.obj = ot->second;
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
    return ArgLabel{tag(id) == 0 ? ArgLabel::Kind::Labelled : ArgLabel::Kind::Optional,
                    str(f(id, 0))};
  }

  const OValue* ovalue(std::size_t id) {
    const m::Value& x = v(id);
    switch (x.kind) {
      case m::Value::Kind::Int: return make<OValue>(OValue::Kind::Int, (long)x.i);
      case m::Value::Kind::String:
        return make<OValue>(OValue::Kind::String, 0L, str(id));  // input_value's sharing
      case m::Value::Kind::Double:
        return make<OValue>(OValue::Kind::Double, 0L, std::string_view{}, x.d());
      case m::Value::Kind::Block: {
        if (auto it = ov_.find(id); it != ov_.end()) return it->second;
        auto* o = make<OValue>(OValue::Kind::Block, 0L, std::string_view{}, 0.0, x.tag);
        ov_[id] = o;
        std::vector<const OValue*> fs;
        for (std::size_t k = 0; k < x.fields.size(); ++k) fs.push_back(ovalue(x.fields[k]));
        o->fields = slice(fs);
        if (x.tag == 0 && fs.size() == 4 && fs[0]->kind == OValue::Kind::String && fs[1]->kind == OValue::Kind::Int &&
            fs[2]->kind == OValue::Kind::Int && fs[3]->kind == OValue::Kind::Int) {
          (void)position(id);
          o->pos = pos_.at(id);
        }
        return o;
      }
      default:
        throw Corrupt{};
    }
  }

  Attributes attributes(std::size_t id) {
    return list<const Attribute*>(id, [&](std::size_t a) -> const Attribute* {
      std::size_t nm = f(a, 0);  // string loc = {txt; loc}
      return make<Attribute>(str(f(nm, 0)), loc(f(nm, 1)), ovalue(f(a, 1)), loc(f(a, 2)));
    });
  }

  // ---- type expressions ----
  Commutable* commu(std::size_t id) {
    if (is_int(id)) return ival(id) == 0 ? types::cok() : types::cunknown();
    if (auto it = commu_.find(id); it != commu_.end()) return it->second;
    auto* c = make<Commutable>(Commutable::Kind::Cvar, nullptr);
    commu_[id] = c;
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
    if (auto it = fk_.find(id); it != fk_.end()) return it->second;
    auto* k = make<FieldKind>(FieldKind::Kind::FKvar, nullptr);
    fk_[id] = k;
    k->field_kind = field_kind(f(id, 0));
    return k;
  }

  const PathArgs* path_args(std::size_t id) {
    return make<PathArgs>(path(f(id, 0)), list<TypeExpr*>(f(id, 1), [&](std::size_t t) {
                            return ty(t);
                          }));
  }

  NameRef* name_ref(std::size_t id) {
    if (auto it = nm_.find(id); it != nm_.end()) return it->second;
    auto* r = make<NameRef>(nullptr);
    nm_[id] = r;
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
    if (auto it = memo_.find(id); it != memo_.end()) return it->second;
    auto* r = make<MemoRef>(types::mnil());
    memo_[id] = r;
    r->contents = memo(f(id, 0));
    return r;
  }

  RowFieldCell* row_cell(std::size_t id) {
    if (auto it = cell_.find(id); it != cell_.end()) return it->second;
    auto* c = make<RowFieldCell>(types::rfnone());
    cell_[id] = c;
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
      return RowFieldEntry{str(f(e, 0)), row_field(f(e, 1))};
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
    if (auto it = desc_.find(id); it != desc_.end()) return it->second;
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
      case 6: d = types::tvariant(row(f(id, 0))); break;
      case 7: d = types::tunivar(opt_str(f(id, 0))); break;
      case 8: {
        TypeExpr* a = ty(f(id, 0));
        d = types::tpoly(a, tys(f(id, 1)));
        break;
      }
      case 9: d = types::tpackage(package(f(id, 0))); break;
      case 10: {
        ArgLabel l = arg_label(f(id, 0));
        ident::Unscoped* u = unscoped(f(id, 1));
        const Package* p = package(f(id, 2));
        d = types::tfunctor(l, u, p, ty(f(id, 3)));
        break;
      }
      case 11: {
        TypeExpr* a = ty(f(id, 0));
        Path::t p = path(f(id, 1));
        d = types::texpand(a, p, tys(f(id, 2)));
        break;
      }
      case 12: d = types::tlink(ty(f(id, 0))); break;
      case 13: {
        TypeExpr* a = ty(f(id, 0));
        d = types::tsubst(a, opt_ptr(f(id, 1), [&](std::size_t t) { return ty(t); }));
        break;
      }
      default: throw Corrupt{};
    }
    desc_[id] = d;
    return d;
  }

  TypeExpr* ty(std::size_t id) {
    if (auto it = ty_.find(id); it != ty_.end()) return it->second;
    // transient_expr = { mutable desc; mutable level; mutable scope; id }
    auto* t = types::create_expr(nullptr, ival(f(id, 1)), ival(f(id, 2)), ival(f(id, 3)));
    ty_[id] = t;
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
        } else if (tag(o) == 0) {
          k->origin.kind = OK::Existential;
          k->origin.existential = str(f(o, 0));
          k->origin.obj = block_identity(o);
        } else {
          k->origin.kind = OK::Equation;
          k->origin.eq1 = ty(f(o, 0));
          k->origin.eq2 = ty(f(o, 1));
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
          auto [ot, fresh] = repr_obj_.try_emplace(r, nullptr);
          if (fresh) ot->second = zone().alloc(1, 1);
          k->record_repr.obj = ot->second;
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
    if (auto it = csig_.find(id); it != csig_.end()) return it->second;
    auto* c = make<ClassSignature>();
    csig_[id] = c;
    c->csig_self = ty(f(id, 0));
    c->csig_self_row = ty(f(id, 1));
    c->csig_dummy_method = field_kind(f(id, 2));
    c->csig_vars = StrMap<VarEntry>(strmap<VarEntry>(f(id, 3), [&](std::size_t x) {
      MutableFlag mu = mutable_flag(f(x, 0));
      VirtualFlag vi = virtual_flag(f(x, 1));
      return VarEntry{mu, vi, ty(f(x, 2))};
    }));
    c->csig_meths = StrMap<MethEntry>(strmap<MethEntry>(f(id, 4), [&](std::size_t x) {
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
      auto [ot, fresh] = repr_obj_.try_emplace(id, nullptr);
      if (fresh) ot->second = zone().alloc(1, 1);
      n.obj = ot->second;
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
        std::size_t p = f(id, 0);
        k.prim = make<PrimitiveDescription>(
            str(f(p, 0)), ival(f(p, 1)), boolean(f(p, 2)), str(f(p, 3)),
            list<NativeRepr>(f(p, 4), [&](std::size_t x) { return native_repr(x); }),
            native_repr(f(p, 5)));
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
            auto [ot, fresh] = some_obj_.try_emplace(f(p, 0), nullptr);
            if (fresh) ot->second = zone().alloc(1, 1);
            mt->param.some_obj = ot->second;
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

 private:
  const m::Arena& a_;
  std::unordered_map<std::size_t, TypeExpr*> ty_;
  std::unordered_map<std::size_t, const TypeDesc*> desc_;
  std::unordered_map<std::size_t, Commutable*> commu_;
  std::unordered_map<std::size_t, FieldKind*> fk_;
  std::unordered_map<std::size_t, NameRef*> nm_;
  std::unordered_map<std::size_t, MemoRef*> memo_;
  std::unordered_map<std::size_t, RowFieldCell*> cell_;
  std::unordered_map<std::size_t, ident::Unscoped*> us_;
  std::unordered_map<std::size_t, ClassSignature*> csig_;
  std::unordered_map<std::size_t, const OValue*> ov_;
  std::unordered_map<std::size_t, std::string_view> str_;
  std::unordered_map<std::size_t, Ident::t> ident_;
  std::unordered_map<std::size_t, Path::t> path_;
  std::unordered_map<std::size_t, const void*> uid_obj_;
  std::unordered_map<std::size_t, const void*> some_obj_;
  std::unordered_map<std::size_t, const void*> block_obj_;
  // one identity per marshaled block
  const void* block_identity(std::size_t id) {
    auto [it, fresh] = block_obj_.try_emplace(id, nullptr);
    if (fresh) it->second = zone().alloc(1, 1);
    return it->second;
  }
  std::unordered_map<std::size_t, const Position*> pos_;
  std::unordered_map<std::size_t, const Location*> loc_;
  std::unordered_map<std::size_t, const void*> repr_obj_;
  std::unordered_map<std::size_t, const void*> optstr_obj_;
  std::unordered_map<std::size_t, std::pair<const void*, std::size_t>> list_memo_;
};


// ---- write side: output_value of the Types graph ---------------------------
// The inverse of Reader: the same layouts, built as omarshal values.  Every
// node the port shares (type_exprs, their descs, the mutable cells, idents,
// paths, declarations) becomes ONE value, created before its fields so that
// cycles close, and omarshal emits a repeat as a back-reference: Marshal's
// sharing of the physical graph.
namespace o = cppcaml::omarshal;

class Writer {
 public:
  using V = o::ValPtr;
  V i(long n) { return o::vint(n); }
  V b(bool x) { return o::vint(x ? 1 : 0); }
  // A string is one value per zone copy: the port's string_views alias
  // exactly where ocamlc's strings are one object (a copied name, a path
  // component taken from an ident); a null view has no identity.
  V str(std::string_view sv) {
    if (!sv.data()) return o::vstr(std::string(sv));
    auto [it, fresh] = strs_.try_emplace(std::make_pair(sv.data(), sv.size()), nullptr);
    if (fresh) it->second = o::vstr(std::string(sv));
    return it->second;
  }
  V some(V x) { return o::vblock(0, {std::move(x)}); }
  // a value built by [make], one per identity token (nullptr: a fresh one)
  template <class F>
  V shared_by(const void* obj, F&& make) {
    if (!obj) return make();
    if (auto it = by_obj_.find(obj); it != by_obj_.end()) return it->second;
    V v = make();
    by_obj_[obj] = v;
    return v;
  }
  // `Some x`, one block per identity token (nullptr: a fresh block)
  template <class F>
  V some_shared(const void* obj, F&& x) {
    if (!obj) return some(x());
    if (auto it = somes_.find(obj); it != somes_.end()) return it->second;
    V v = some(x());
    somes_[obj] = v;
    return v;
  }
  V none() { return o::vint(0); }
  template <class L, class F>
  V list(const L& l, F&& elt) {
    std::vector<V> xs;
    for (auto& x : l) xs.push_back(elt(x));
    return o::vlist(xs);
  }
  // A list the port keeps as a Slice: copies of a declaration share the
  // Slice's storage exactly where ocamlc's copies share the list (Subst keeps
  // type_variance / type_separability ...), so one value per storage.
  template <class T, class F>
  V list(const Slice<T>& l, F&& elt) {
    if (l.empty() || getenv("CMIW_NOLISTSHARE")) {
      std::vector<V> xs;
      for (auto& x : l) xs.push_back(elt(x));
      return o::vlist(xs);
    }
    auto key = std::make_pair(static_cast<const void*>(l.p), l.n);
    if (auto it = lists_.find(key); it != lists_.end()) return it->second;
    std::vector<V> xs;
    for (auto& x : l) xs.push_back(elt(x));
    V v = o::vlist(xs);
    lists_[key] = v;
    return v;
  }
  V opt_str(const OptStr& x) {
    if (!x.some) return none();
    if (!x.obj) return some(str(x.v));
    if (auto it = memo_.find(x.obj); it != memo_.end()) return it->second;
    V v = some(str(x.v));
    memo_[x.obj] = v;
    return v;
  }
  // a block registered under [key] before its fields are filled
  template <class K>
  V shared(std::unordered_map<const void*, V>& memo, const K* key, int tag,
           const std::function<std::vector<V>()>& fields) {
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    V v = o::vblock(tag, {});
    memo[key] = v;
    v->fields = fields();
    return v;
  }

  // ---- Ident / Path ----
  V unscoped(const ident::Unscoped* u) {
    return shared(memo_, u, 0, [&]() -> std::vector<V> {  // { mutable state }
      if (u->state == ident::Unscoped::State::Udesc)
        return {o::vblock(0, {o::vblock(0, {str(u->name), i(u->stamp)})})};
      return {o::vblock(1, {unscoped(u->ulink)})};
    });
  }
  V ident(Ident::t id) {
    using K = Ident::Kind;
    int tag = static_cast<int>(id->kind);  // Local, Scoped, Global, Predef, Unscoped
    return shared(memo_, id, tag, [&]() -> std::vector<V> {
      switch (id->kind) {
        case K::Local: return {str(id->name_), i(id->stamp_)};
        case K::Scoped: return {str(id->name_), i(id->stamp_), i(id->scope_)};
        case K::Global: return {str(id->name_)};
        case K::Predef: return {str(id->name_), i(id->stamp_)};
        case K::Unscoped: return {unscoped(id->us)};
      }
      return {};
    });
  }
  V path(Path::t p) {
    using K = Path::Kind;
    return shared(memo_, p, static_cast<int>(p->kind), [&]() -> std::vector<V> {
      switch (p->kind) {
        case K::Pident: return {ident(p->id)};
        case K::Pdot: return {path(p->p1), str(p->s)};
        case K::Papply: return {path(p->p1), path(p->p2)};
        case K::Pextra_ty:
          return {path(p->p1), p->extra == Path::Extra::Pext_ty ? i(0) : o::vblock(0, {str(p->s)})};
      }
      return {};
    });
  }

  // ---- support ----
  // Sharing that ocamlc's values have physically and Marshal keeps.  The
  // lexer makes one position per token boundary, all holding the one
  // filename string of its lexbuf, and parsed locations flow into the
  // declarations by reference; a signature read from another .cmi carries
  // that file's own strings (input_value's sharing, the Reader's).  So a
  // filename is one value per string object (storage), equal positions of
  // one filename are one value, and equal locations one record (measured
  // against ocamlc's .cmi files: cmi_port_parity.sh, and -pack's, whose
  // members' signatures come from several .cmi: pack_parity.sh).
  V fname(std::string_view f) {
    if (f.data()) return str(f);
    auto [it, fresh] = fnames_.try_emplace(std::string(f), nullptr);
    if (fresh) it->second = str(f);
    return it->second;
  }
  V position(const Position& x) {
    if (same_record(x)) {  // a record read from a .cmi: its own block
      auto [it, fresh] = pos_objs_.try_emplace(x.obj, nullptr);
      if (fresh) it->second = o::vblock(0, {str(x.pos_fname), i(x.pos_lnum), i(x.pos_bol), i(x.pos_cnum)});
      return it->second;
    }
    V f = fname(x.pos_fname);
    auto [it, fresh] = poss_.try_emplace(std::make_tuple(f.get(), x.pos_lnum, x.pos_bol, x.pos_cnum), nullptr);
    if (fresh) it->second = o::vblock(0, {f, i(x.pos_lnum), i(x.pos_bol), i(x.pos_cnum)});
    return it->second;
  }
  V loc(const Location& l) {
    if (same_record(l)) {
      auto [it, fresh] = loc_objs_.try_emplace(l.obj, nullptr);
      if (fresh) {
        V a = position(l.loc_start);
        V e = position(l.loc_end);
        it->second = o::vblock(0, {a, e, b(l.loc_ghost)});
        // a copy of it the port rebuilt from its positions is this record too
        // (not for a parser record distinct from its equal-valued twin)
        if (!l.distinct) locs_.try_emplace(std::make_tuple(a.get(), e.get(), l.loc_ghost), it->second);
      }
      return it->second;
    }
    V a = position(l.loc_start);
    V e = position(l.loc_end);
    auto [it, fresh] = locs_.try_emplace(std::make_tuple(a.get(), e.get(), l.loc_ghost), nullptr);
    if (fresh) it->second = o::vblock(0, {a, e, b(l.loc_ghost)});
    return it->second;
  }
  // The current unit's name: Uid.mk takes it from Unit_info, the string the
  // cmi header's cmi_name also is, so the unit's uids and the header share
  // one string.  A loaded unit's uids carry that unit's own name string
  // (shared by identity, str()), which Env's hashconsed persistent idents
  // share too.
  void set_current_unit(std::string_view n, V value = nullptr) {
    current_unit_ = std::string(n);
    current_unit_name_ = value ? value : o::vstr(current_unit_);
  }
  // the same, the name being the string object [n] itself (one value with
  // every other use of that storage)
  void set_current_unit_shared(std::string_view n) {
    current_unit_ = std::string(n);
    current_unit_name_ = str(n);
  }
  V unit_name(std::string_view n) {
    if (current_unit_name_ && n == current_unit_) return current_unit_name_;
    return str(n);
  }
 public:
  template <class F>
  V uid_shared(const Uid& u, F&& make_value) {
    if (!u.obj) return make_value();
    if (auto it = memo_.find(u.obj); it != memo_.end()) return it->second;
    V v = make_value();
    memo_[u.obj] = v;
    return v;
  }
  V uid(const Uid& u) {
    switch (u.kind) {
      case Uid::Kind::Internal: return i(0);
      case Uid::Kind::Compilation_unit:
        return uid_shared(u, [&] { return o::vblock(0, {unit_name(u.comp_unit)}); });
      // a uid is passed by reference: one value per record (Uid.obj)
      case Uid::Kind::Item:
        return uid_shared(u, [&] {
          return o::vblock(1, {unit_name(u.comp_unit), i(u.id), i(u.from == Uid::From::Intf ? 0 : 1)});
        });
      case Uid::Kind::Local_opaque_item:
        return uid_shared(u, [&] { return o::vblock(2, {unit_name(u.comp_unit), i(u.id)}); });
      case Uid::Kind::Predef: return uid_shared(u, [&] { return o::vblock(3, {str(u.comp_unit)}); });
    }
    return i(0);
  }
  // An arg_label is copied by reference (Subst keeps Tarrow's label): one
  // value per label whose name is one string.
  V arg_label(const ArgLabel& l) {
    if (l.kind == ArgLabel::Kind::Nolabel) return i(0);
    int tag = l.kind == ArgLabel::Kind::Labelled ? 0 : 1;
    if (l.name.empty()) return o::vblock(tag, {str(l.name)});
    auto [it, fresh] = labels_.try_emplace(std::make_tuple(tag, l.name.data(), l.name.size()), nullptr);
    if (fresh) it->second = o::vblock(tag, {str(l.name)});
    return it->second;
  }
  V ovalue(const OValue* x) {
    switch (x->kind) {
      case OValue::Kind::Int: return i(x->i);
      case OValue::Kind::String: return str(x->s);
      case OValue::Kind::Double: return o::vdbl(x->d);
      case OValue::Kind::Block:
        // A Parsetree value's only {string; int; int; int} record is a
        // Lexing.position: the lexer's positions, shared with the file's
        // others (position()).
        if (x->tag == 0 && x->fields.size() == 4 &&
            x->fields[0]->kind == OValue::Kind::String && x->fields[1]->kind == OValue::Kind::Int &&
            x->fields[2]->kind == OValue::Kind::Int && x->fields[3]->kind == OValue::Kind::Int)
          return x->pos ? position(*x->pos)
                        : position(Position{x->fields[0]->s, x->fields[1]->i, x->fields[2]->i, x->fields[3]->i});
        return shared(memo_, x, static_cast<int>(x->tag), [&]() -> std::vector<V> {
          std::vector<V> fs;
          for (const OValue* f : x->fields) fs.push_back(ovalue(f));
          return fs;
        });
    }
    return i(0);
  }
  V attributes(const Attributes& as) {
    return list(as, [&](const Attribute* a) {
      return shared(memo_, a, 0, [&]() -> std::vector<V> {
        V name;
        if (a->name_obj) {
          V l = loc(a->attr_name_loc);
          auto [it, fresh] = attr_names_.try_emplace(std::make_pair(a->name_obj, l.get()), nullptr);
          if (fresh) it->second = o::vblock(0, {str(a->attr_name), l});
          name = it->second;
        } else {
          name = o::vblock(0, {str(a->attr_name), loc(a->attr_name_loc)});
        }
        V payload = ovalue(a->attr_payload);
        // a doc attribute (Docstrings): its attr_loc is its payload item's
        // loc record, PStr [{pstr_desc; pstr_loc}]
        if (a->name_obj) {
          const OValue* p = a->attr_payload;
          if (p && p->kind == OValue::Kind::Block && p->tag == 0 && p->fields.size() == 1) {
            const OValue* cons = p->fields[0];
            if (cons->kind == OValue::Kind::Block && cons->fields.size() == 2) {
              const OValue* item = cons->fields[0];
              if (item->kind == OValue::Kind::Block && item->fields.size() == 2)
                return {name, payload, ovalue(item->fields[1])};
            }
          }
        }
        return {name, payload, loc(a->attr_loc)};
      });
    });
  }

  // ---- type expressions ----
  V commu(const Commutable* c) {
    switch (c->kind) {
      case Commutable::Kind::Cok: return i(0);
      case Commutable::Kind::Cunknown: return i(1);
      case Commutable::Kind::Cvar: return shared(memo_, c, 0, [&]() -> std::vector<V> { return {commu(c->commu)}; });
    }
    return i(0);
  }
  V field_kind(const FieldKind* k) {
    switch (k->kind) {
      case FieldKind::Kind::FKprivate: return i(0);
      case FieldKind::Kind::FKpublic: return i(1);
      case FieldKind::Kind::FKabsent: return i(2);
      case FieldKind::Kind::FKvar:
        return shared(memo_, k, 0, [&]() -> std::vector<V> { return {field_kind(k->field_kind)}; });
    }
    return i(0);
  }
  V path_args(const PathArgs* pa) {
    return shared(memo_, pa, 0, [&]() -> std::vector<V> { return {path(pa->path), path_args_list(pa)}; });
  }
  // `x :: tail` with the tail list shared (PathArgs::tail), else the list
  V path_args_list(const PathArgs* pa) {
    if (pa->tail.empty()) return tys(pa->args);
    auto key = std::make_pair(static_cast<const void*>(pa->args.p), pa->args.n);
    if (auto it = lists_.find(key); it != lists_.end()) return it->second;
    V hd = ty(pa->args[0]);
    V v = o::vblock(0, {hd, tys(pa->tail)});
    lists_[key] = v;
    return v;
  }
  V name_ref(const NameRef* r) {
    return shared(memo_, r, 0, [&]() -> std::vector<V> {
      return {r->contents ? some(path_args(r->contents)) : none()};
    });
  }
  V memo(const AbbrevMemo* m) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return i(0);
      case AbbrevMemo::Kind::Mcons:
        return shared(memo_, m, 0, [&]() -> std::vector<V> {
          return {i(m->privacy == PrivateFlag::Private ? 0 : 1), path(m->path), ty(m->abbreviation),
                  ty(m->expansion), memo(m->rem)};
        });
      case AbbrevMemo::Kind::Mlink:
        return shared(memo_, m, 1, [&]() -> std::vector<V> { return {memo_ref(m->link)}; });
    }
    return i(0);
  }
  V memo_ref(const MemoRef* r) {
    return shared(memo_, r, 0, [&]() -> std::vector<V> { return {memo(r->contents)}; });
  }
  V row_cell(const RowFieldCell* c) {
    return shared(memo_, c, 0, [&]() -> std::vector<V> { return {row_field(c->contents)}; });
  }
  V row_field(const RowField* f) {
    switch (f->kind) {
      case RowField::Kind::RFabsent: return i(0);
      case RowField::Kind::RFnone: return i(1);
      case RowField::Kind::RFpresent:
        return shared(memo_, f, 0, [&]() -> std::vector<V> { return {f->present ? some(ty(f->present)) : none()}; });
      case RowField::Kind::RFeither:
        return shared(memo_, f, 1, [&]() -> std::vector<V> {
          return {b(f->no_arg), tys(f->arg_type), b(f->matched), row_cell(f->ext)};
        });
    }
    return i(0);
  }
  V fixed(const FixedExplanation* x) {
    using FK = FixedExplanation::Kind;
    switch (x->kind) {
      case FK::Fixed_private: return i(0);
      case FK::Rigid: return i(1);
      case FK::Univar: return o::vblock(0, {ty(x->univar)});
      case FK::Reified: return o::vblock(1, {path(x->reified)});
    }
    return i(0);
  }
  V row(const RowDesc* r) {
    return shared(memo_, r, 0, [&]() -> std::vector<V> {
      return {list(r->row_fields, [&](const RowFieldEntry& e) { return o::vblock(0, {str(e.label), row_field(e.field)}); }),
              ty(r->row_more), b(r->row_closed),
              // the options are passed on with the row's other fields
              // (create_row ~fixed:(row_fixed row) ~name:(row_name row)):
              // one `Some` per explanation / name record
              r->row_fixed ? some_shared(r->row_fixed, [&] { return fixed(r->row_fixed); }) : none(),
              r->row_name ? some_shared(r->row_name, [&] { return path_args(r->row_name); }) : none()};
    });
  }
  V package(const Package* p) {
    return shared(memo_, p, 0, [&]() -> std::vector<V> {
      return {path(p->pack_path), list(p->pack_constraints, [&](const PackConstraint& c) {
                return o::vblock(0, {list(c.path, [&](std::string_view s) { return str(s); }), ty(c.ty)});
              })};
    });
  }
  V tys(const Slice<TypeExpr*>& l) {
    return list(l, [&](TypeExpr* t) { return ty(t); });
  }
  V desc(const TypeDesc* d) {
    if (d->kind == DescKind::Tnil) return i(0);
    // non-constant constructors in declaration order, Tnil (constant) skipped
    int tag = static_cast<int>(d->kind);
    if (d->kind > DescKind::Tnil) --tag;
    return shared(memo_, d, tag, [&]() -> std::vector<V> {
      switch (d->kind) {
        case DescKind::Tvar: return {opt_str(as<Tvar>(d)->name)};
        case DescKind::Tarrow: {
          auto* a = as<Tarrow>(d);
          return {arg_label(a->label), ty(a->t1), ty(a->t2), commu(a->commu)};
        }
        case DescKind::Ttuple:
          return {list(as<Ttuple>(d)->elems, [&](const LabeledTy& e) { return o::vblock(0, {opt_str(e.label), ty(e.ty)}); })};
        case DescKind::Tconstr: {
          auto* c = as<Tconstr>(d);
          return {path(c->path), tys(c->args), memo_ref(c->memo)};
        }
        case DescKind::Tobject: {
          auto* ob = as<Tobject>(d);
          return {ty(ob->fields), name_ref(ob->name)};
        }
        case DescKind::Tfield: {
          auto* f = as<Tfield>(d);
          return {str(f->label), field_kind(f->kind_), ty(f->ty), ty(f->rest)};
        }
        case DescKind::Tvariant: return {row(as<Tvariant>(d)->row)};
        case DescKind::Tunivar: return {opt_str(as<Tunivar>(d)->name)};
        case DescKind::Tpoly: {
          auto* p = as<Tpoly>(d);
          return {ty(p->body), tys(p->vars)};
        }
        case DescKind::Tpackage: return {package(as<Tpackage>(d)->pack)};
        case DescKind::Tfunctor: {
          auto* f = as<Tfunctor>(d);
          return {arg_label(f->label), unscoped(f->id), package(f->pack), ty(f->body)};
        }
        case DescKind::Texpand: {
          auto* e = as<Texpand>(d);
          return {ty(e->ty), path(e->path), tys(e->args)};
        }
        case DescKind::Tlink: return {ty(as<Tlink>(d)->ty)};
        case DescKind::Tsubst: {
          auto* s = as<Tsubst>(d);
          return {ty(s->ty), s->row ? some(ty(s->row)) : none()};
        }
        case DescKind::Tnil: break;
      }
      return {};
    });
  }
  V ty(const TypeExpr* t) {
    // transient_expr = { mutable desc; mutable level; mutable scope; id }
    return shared(memo_, t, 0, [&]() -> std::vector<V> { return {desc(t->desc), i(t->level), i(t->scope), i(t->id)}; });
  }

  // ---- declarations ----
  V label_decl(const LabelDeclaration* l) {
    return shared(memo_, l, 0, [&]() -> std::vector<V> {
      return {ident(l->ld_id), i(l->ld_mutable == MutableFlag::Immutable ? 0 : 1),
              i(l->ld_atomic == AtomicFlag::Nonatomic ? 0 : 1), ty(l->ld_type), loc(l->ld_loc),
              attributes(l->ld_attributes), uid(l->ld_uid)};
    });
  }
  V cstr_args(const ConstructorArguments& a) {
    if (a.kind == ConstructorArguments::Kind::Cstr_tuple) return o::vblock(0, {tys(a.tuple)});
    return o::vblock(1, {list(a.record, [&](const LabelDeclaration* l) { return label_decl(l); })});
  }
  V cstr_decl(const ConstructorDeclaration* c) {
    return shared(memo_, c, 0, [&]() -> std::vector<V> {
      return {ident(c->cd_id), cstr_args(c->cd_args), c->cd_res ? some(ty(c->cd_res)) : none(), loc(c->cd_loc),
              attributes(c->cd_attributes), uid(c->cd_uid)};
    });
  }
  V private_flag(PrivateFlag p) { return i(p == PrivateFlag::Private ? 0 : 1); }
  // a type_kind is shared by reference (Types.kind_abstract is one value)
  V type_kind(const TypeKind* k) {
    if (auto it = memo_.find(k); it != memo_.end()) return it->second;
    V v = type_kind_(k);
    memo_[k] = v;
    return v;
  }
  V type_kind_(const TypeKind* k) {
    using KK = TypeKind::Kind;
    switch (k->kind) {
      case KK::Type_open: return i(0);
      case KK::Type_abstract: {
        using OK = TypeOrigin::Kind;
        V orig;
        switch (k->origin.kind) {
          case OK::Definition: orig = i(0); break;
          case OK::Rec_check_regularity: orig = i(1); break;
          case OK::Approx_recmod: orig = i(2); break;
          case OK::Existential:
            orig = shared_by(k->origin.obj, [&] { return o::vblock(0, {str(k->origin.existential)}); });
            break;
          case OK::Equation:
            orig = shared_by(k->origin.obj, [&] { return o::vblock(1, {ty(k->origin.eq1), ty(k->origin.eq2)}); });
            break;
        }
        return o::vblock(0, {orig});
      }
      case KK::Type_record: {
        using RK = RecordRepresentation::Kind;
        const RecordRepresentation& r = k->record_repr;
        auto block = [&]() -> V {
          switch (r.kind) {
            case RK::Record_unboxed: return o::vblock(0, {b(r.unboxed_inlined)});
            case RK::Record_inlined: return o::vblock(1, {i(r.inlined_tag)});
            case RK::Record_extension: return o::vblock(2, {path(r.extension)});
            default: return i(0);
          }
        };
        V repr;
        switch (r.kind) {
          case RK::Record_regular: repr = i(0); break;
          case RK::Record_float: repr = i(1); break;
          default:
            if (!r.obj) {
              repr = block();
            } else if (auto it = memo_.find(r.obj); it != memo_.end()) {
              repr = it->second;
            } else {
              repr = block();
              memo_[r.obj] = repr;
            }
        }
        return o::vblock(1, {list(k->labels, [&](const LabelDeclaration* l) { return label_decl(l); }), repr});
      }
      case KK::Type_variant:
        return o::vblock(2, {list(k->constructors, [&](const ConstructorDeclaration* c) { return cstr_decl(c); }),
                             i(k->variant_repr == VariantRepresentation::Variant_regular ? 0 : 1)});
      case KK::Type_external: return o::vblock(3, {str(k->external)});
    }
    return i(0);
  }
  V type_decl(const TypeDeclaration* d) {
    return shared(memo_, d, 0, [&]() -> std::vector<V> {
      return {tys(d->type_params), i(d->type_arity), type_kind(d->type_kind), private_flag(d->type_private),
              d->type_manifest ? some(ty(d->type_manifest)) : none(),
              list(d->type_variance, [&](variance::t v) { return i(v); }),
              list(d->type_separability, [&](Separability x) { return i(static_cast<long>(x)); }),
              b(d->type_is_newtype), i(d->type_expansion_scope), loc(d->type_loc), attributes(d->type_attributes),
              i(static_cast<long>(d->type_immediate)), b(d->type_unboxed_default), uid(d->type_uid)};
    });
  }
  V ext_constr(const ExtensionConstructor* e) {
    return shared(memo_, e, 0, [&]() -> std::vector<V> {
      return {path(e->ext_type_path), tys(e->ext_type_params), cstr_args(e->ext_args),
              e->ext_ret_type ? some(ty(e->ext_ret_type)) : none(), private_flag(e->ext_private), loc(e->ext_loc),
              attributes(e->ext_attributes), uid(e->ext_uid)};
    });
  }
  template <class Val, class F>
  V strmap(const StrMapNode<Val>* n, F&& data) {
    if (!n) return i(0);
    V l = strmap<Val>(n->l, data);
    V k = str(n->v);
    V d = data(n->d);
    V r = strmap<Val>(n->r, data);
    return o::vblock(0, {l, k, d, r, i(n->h)});
  }
  V virtual_flag(VirtualFlag v) { return i(v == VirtualFlag::Virtual ? 0 : 1); }
  V mutable_flag(MutableFlag m) { return i(m == MutableFlag::Immutable ? 0 : 1); }
  V class_sig(const ClassSignature* c) {
    return shared(memo_, c, 0, [&]() -> std::vector<V> {
      return {ty(c->csig_self), ty(c->csig_self_row), field_kind(c->csig_dummy_method),
              strmap<VarEntry>(c->csig_vars.root(), [&](const VarEntry& e) {
                return o::vblock(0, {mutable_flag(e.mut), virtual_flag(e.virt), ty(e.ty)});
              }),
              strmap<MethEntry>(c->csig_meths.root(), [&](const MethEntry& e) {
                // Mprivate k is made once per method and copied by
                // reference (Subst keeps a meths entry's privacy): one value
                // per field kind
                V p = i(0);
                if (e.priv.is_private) {
                  if (auto it = mprivate_.find(e.priv.kind); it != mprivate_.end()) {
                    p = it->second;
                  } else {
                    p = o::vblock(0, {field_kind(e.priv.kind)});
                    mprivate_[e.priv.kind] = p;
                  }
                }
                return o::vblock(0, {p, virtual_flag(e.virt), ty(e.ty)});
              })};
    });
  }
  V class_type(const ClassType* c) {
    using CK = ClassType::Kind;
    return shared(memo_, c, static_cast<int>(c->kind), [&]() -> std::vector<V> {
      switch (c->kind) {
        case CK::Cty_constr: return {path(c->path), tys(c->args), class_type(c->cty)};
        case CK::Cty_signature: return {class_sig(c->sign)};
        case CK::Cty_arrow: return {arg_label(c->label), ty(c->arg), class_type(c->cty)};
      }
      return {};
    });
  }
  V variances(const Slice<variance::t>& l) {
    return list(l, [&](variance::t v) { return i(v); });
  }
  V native_repr(const NativeRepr& n) {
    switch (n.kind) {
      case NativeRepr::Kind::Same_as_ocaml_repr: return i(0);
      case NativeRepr::Kind::Unboxed_float: return i(1);
      case NativeRepr::Kind::Untagged_immediate: return i(2);
      case NativeRepr::Kind::Unboxed_integer: {
        if (!n.obj) return o::vblock(0, {i(static_cast<long>(n.bi))});
        if (auto it = memo_.find(n.obj); it != memo_.end()) return it->second;
        V v = o::vblock(0, {i(static_cast<long>(n.bi))});
        memo_[n.obj] = v;
        return v;
      }
    }
    return i(0);
  }
  V value_kind(const ValueKind& k) {
    switch (k.kind) {
      case ValueKind::Kind::Val_reg: return i(0);
      case ValueKind::Kind::Val_prim: {
        // one `Val_prim prim` block per declaration: copies of the value
        // description (Subst, the signature's) keep its val_kind
        const PrimitiveDescription* p = k.prim;
        if (auto it = val_prims_.find(p); it != val_prims_.end()) return it->second;
        V v = o::vblock(0, {o::vblock(0, {str(p->prim_name), i(p->prim_arity), b(p->prim_alloc),
                                          str(p->prim_native_name),
                                          list(p->prim_native_repr_args, [&](const NativeRepr& n) { return native_repr(n); }),
                                          native_repr(p->prim_native_repr_res)})});
        val_prims_[p] = v;
        return v;
      }
      case ValueKind::Kind::Val_ivar: return o::vblock(1, {mutable_flag(k.ivar_mut), str(k.ivar_name)});
      // (never in a signature: a debug event's Env summary)
      case ValueKind::Kind::Val_self: {
        // the self_meths value: typeclass builds one per class
        // (self_var_kind) for every Val_self of it -- one per k.meths
        V meths;
        if (auto it = self_meths_.find(k.meths); it != self_meths_.end()) {
          meths = it->second;
        } else if (!k.self_virtual) {
          meths = o::vblock(0, {ident_map(*k.meths)});  // Self_concrete
          self_meths_[k.meths] = meths;
        } else {                                        // Self_virtual of a ref
          V r = shared(memo_, k.meths, 0, [&]() -> std::vector<V> { return {ident_map(*k.meths)}; });
          meths = o::vblock(1, {r});
          self_meths_[k.meths] = meths;
        }
        return o::vblock(2, {class_sig(k.sign), meths, ident_map(k.vars), str(k.cl_num)});
      }
      case ValueKind::Kind::Val_anc:
        return o::vblock(3, {class_sig(k.sign), ident_map(*k.meths), str(k.cl_num)});
    }
    return i(0);
  }
  // an `Ident.t Meths.t` / `Vars.t`: one value per map node
  V ident_map(const StrMap<Ident::t>& m) { return strmap_shared<Ident::t>(m.root(), [&](Ident::t id) { return ident(id); }); }
  template <class Val, class F>
  V strmap_shared(const StrMapNode<Val>* n, F&& data) {
    if (!n) return i(0);
    return shared(memo_, n, 0, [&]() -> std::vector<V> {
      V l = strmap_shared<Val>(n->l, data);
      V k = str(n->v);
      V d = data(n->d);
      V r = strmap_shared<Val>(n->r, data);
      return {l, k, d, r, i(n->h)};
    });
  }
  V module_type(const ModuleType* mt) {
    using MK = ModuleType::Kind;
    return shared(memo_, mt, static_cast<int>(mt->kind), [&]() -> std::vector<V> {
      switch (mt->kind) {
        case MK::Mty_ident: return {path(mt->path)};
        case MK::Mty_signature: return {signature(mt->sign)};
        case MK::Mty_functor: {
          V param = mt->param.is_unit
                        ? i(0)
                        : o::vblock(0, {mt->param.id ? some_shared(mt->param.some_obj, [&] { return ident(mt->param.id); })
                                                     : none(),
                                        module_type(mt->param.mty)});
          return {param, module_type(mt->res)};
        }
        case MK::Mty_alias: return {path(mt->path)};
      }
      return {};
    });
  }
  // the declarations, one value per record
  V value_desc(const ValueDescription* vd) {
    return shared(memo_, vd, 0, [&]() -> std::vector<V> {
      return {ty(vd->val_type), value_kind(vd->val_kind), loc(vd->val_loc), attributes(vd->val_attributes),
              uid(vd->val_uid)};
    });
  }
  V module_decl(const ModuleDeclaration* md) {
    return shared(memo_, md, 0, [&]() -> std::vector<V> {
      return {module_type(md->md_type), attributes(md->md_attributes), loc(md->md_loc), uid(md->md_uid)};
    });
  }
  V modtype_decl(const ModtypeDeclaration* mtd) {
    return shared(memo_, mtd, 0, [&]() -> std::vector<V> {
      return {mtd->mtd_type ? some(module_type(mtd->mtd_type)) : none(), attributes(mtd->mtd_attributes),
              loc(mtd->mtd_loc), uid(mtd->mtd_uid)};
    });
  }
  V class_decl(const ClassDeclaration* cd) {
    return shared(memo_, cd, 0, [&]() -> std::vector<V> {
      return {tys(cd->cty_params), class_type(cd->cty_type), path(cd->cty_path),
              cd->cty_new ? some(ty(cd->cty_new)) : none(), variances(cd->cty_variance), loc(cd->cty_loc),
              attributes(cd->cty_attributes), uid(cd->cty_uid)};
    });
  }
  V cltype_decl(const ClassTypeDeclaration* cd) {
    return shared(memo_, cd, 0, [&]() -> std::vector<V> {
      return {tys(cd->clty_params), class_type(cd->clty_type), path(cd->clty_path), type_decl(cd->clty_hash_type),
              variances(cd->clty_variance), loc(cd->clty_loc), attributes(cd->clty_attributes), uid(cd->clty_uid)};
    });
  }
  V sig_item(const SignatureItem* it) {
    using SK = SignatureItem::Kind;
    V vis = i(it->vis == Visibility::Exported ? 0 : 1);
    V rec = i(static_cast<long>(it->rec));
    switch (it->kind) {
      case SK::Sig_value: return o::vblock(0, {ident(it->id), value_desc(it->value), vis});
      case SK::Sig_type: return o::vblock(1, {ident(it->id), type_decl(it->type), rec, vis});
      case SK::Sig_typext:
        return o::vblock(2, {ident(it->id), ext_constr(it->ext), i(static_cast<long>(it->ext_status)), vis});
      case SK::Sig_module:
        return o::vblock(3, {ident(it->id), i(it->presence == ModulePresence::Mp_present ? 0 : 1), module_decl(it->md),
                             rec, vis});
      case SK::Sig_modtype: return o::vblock(4, {ident(it->id), modtype_decl(it->mtd), vis});
      case SK::Sig_class: return o::vblock(5, {ident(it->id), class_decl(it->cls), rec, vis});
      case SK::Sig_class_type: return o::vblock(6, {ident(it->id), cltype_decl(it->clty), rec, vis});
    }
    return i(0);
  }
  V signature(const Signature& sg) {
    return list(sg, [&](const SignatureItem* it) { return sig_item(it); });
  }

 private:
  std::unordered_map<const void*, V> memo_;
  std::map<std::string, V> fnames_;
  std::string current_unit_;
  V current_unit_name_;
  std::map<std::pair<const char*, std::size_t>, V> strs_;
  std::map<std::pair<const void*, std::size_t>, V> lists_;
  std::unordered_map<const void*, V> mprivate_;
  std::map<std::tuple<int, const char*, std::size_t>, V> labels_;
  std::map<std::tuple<const void*, long, long, long>, V> poss_;
  std::map<std::tuple<const void*, const void*, bool>, V> locs_;
  std::unordered_map<const void*, V> self_meths_;  // Val_self's self_meths blocks
  std::unordered_map<const void*, V> pos_objs_, loc_objs_;
  std::unordered_map<const void*, V> somes_;
  std::unordered_map<const void*, V> val_prims_;
  std::unordered_map<const void*, V> by_obj_;
  std::map<std::pair<const void*, const void*>, V> attr_names_;
};

// ---- the debugging events of a .cmo (Emitcode.to_file with -g) ------------
// Instruct.debug_event records, whose typing values (Env summaries, the
// declarations and types they hold) go through the Writer, and whose
// compilation environments are Ident.tbl trees: one value per node / record.
class EventWriter {
 public:
  using V = o::ValPtr;
  explicit EventWriter(Writer& w) : w_(w) {}

  V summary(const env::Summary* s) {
    using K = env::Summary::Kind;
    if (s->kind == K::Env_empty) return w_.i(0);
    int tag = static_cast<int>(s->kind) - 1;  // Env_empty is the constant constructor
    return w_.shared(memo_, s, tag, [&]() -> std::vector<V> {
      V next = summary(s->next);
      switch (s->kind) {
        case K::Env_empty: break;
        case K::Env_value: return {next, w_.ident(s->id), w_.value_desc(s->value)};
        case K::Env_type: return {next, w_.ident(s->id), w_.type_decl(s->type)};
        case K::Env_extension: return {next, w_.ident(s->id), w_.ext_constr(s->ext)};
        case K::Env_module:
          return {next, w_.ident(s->id), w_.i(s->presence == ModulePresence::Mp_present ? 0 : 1),
                  w_.module_decl(s->md)};
        case K::Env_modtype: return {next, w_.ident(s->id), w_.modtype_decl(s->mtd)};
        case K::Env_class: return {next, w_.ident(s->id), w_.class_decl(s->cls)};
        case K::Env_cltype: return {next, w_.ident(s->id), w_.cltype_decl(s->clty)};
        case K::Env_open: return {next, w_.path(s->path)};
        case K::Env_not_aliasable: return {next, w_.ident(s->id)};
        case K::Env_constraints:
          return {next, path_map(s->constraints.root(), [&](const TypeDeclaration* d) { return w_.type_decl(d); })};
        case K::Env_copy_types: return {next};
        case K::Env_persistent: return {next, w_.ident(s->id)};
        case K::Env_value_unbound: {
          using RK = env::ValueUnboundReason::Kind;
          V reason;
          switch (s->value_reason.kind) {
            case RK::Val_unbound_instance_variable: reason = w_.i(0); break;
            case RK::Val_unbound_self: reason = w_.i(1); break;
            case RK::Val_unbound_ancestor: reason = w_.i(2); break;
            case RK::Val_unbound_ghost_recursive: reason = o::vblock(0, {w_.loc(s->value_reason.ghost_loc)}); break;
          }
          return {next, w_.str(s->name), reason};
        }
        case K::Env_module_unbound:
          return {next, w_.str(s->name),
                  o::vblock(0, {w_.opt_str(s->module_reason.container), w_.str(s->module_reason.unbound)})};
      }
      return {};
    });
  }
  // a Path.Map: one value per node
  template <class Val, class F>
  V path_map(const PMapNode<Path::t, Val>* n, F&& data) {
    if (!n) return w_.i(0);
    return w_.shared(memo_, n, 0, [&]() -> std::vector<V> {
      V l = path_map<Val>(n->l, data);
      V k = w_.path(n->v);
      V d = data(n->d);
      V r = path_map<Val>(n->r, data);
      return {l, k, d, r, w_.i(n->h)};
    });
  }
  // an 'a Ident.tbl: one value per node and per data record
  template <class A, class F>
  V tbl(const typename ident::Tbl<A>::Node* n, F&& data) {
    if (!n) return w_.i(0);
    return w_.shared(memo_, n, 0, [&]() -> std::vector<V> {
      V l = tbl<A>(n->l, data);
      V d = tbl_data<A>(n->d, data);
      V r = tbl<A>(n->r, data);
      return {l, d, r, w_.i(n->h)};
    });
  }
  template <class A, class F>
  V tbl_data(const ident::TblData<A>* d, F&& data) {
    return w_.shared(memo_, d, 0, [&]() -> std::vector<V> {
      return {w_.ident(d->ident), w_.str(d->name), w_.i(d->stamp), data(d->data),
              d->previous ? w_.some(tbl_data<A>(d->previous, data)) : w_.none()};
    });
  }
  V closure_env(const instruct::ClosureEnv& c) {
    if (!c.in_closure) return w_.i(0);  // Not_in_closure
    auto make = [&]() {
      return o::vblock(0, {tbl<instruct::ClosureEntry>(c.entries.root(), [&](const instruct::ClosureEntry& e) {
                             return o::vblock(e.k == instruct::ClosureEntry::K::Free_variable ? 0 : 1, {w_.i(e.pos)});
                           }),
                           w_.i(c.env_pos)});
    };
    if (c.obj) {
      if (auto it = memo_.find(c.obj); it != memo_.end()) return it->second;
      return memo_[c.obj] = make();
    }
    auto key = std::make_pair(static_cast<const void*>(c.entries.root()), c.env_pos);
    if (auto it = closures_.find(key); it != closures_.end()) return it->second;
    return closures_[key] = make();
  }
  V compenv(const instruct::CompilationEnv& e) {
    auto make = [&]() {
      return o::vblock(0, {tbl<long>(e.ce_stack.root(), [&](long pos) { return w_.i(pos); }), closure_env(e.ce_closure)});
    };
    if (e.obj) {
      if (auto it = memo_.find(e.obj); it != memo_.end()) return it->second;
      return memo_[e.obj] = make();
    }
    auto key = std::make_tuple(static_cast<const void*>(e.ce_stack.root()), e.ce_closure.in_closure,
                               e.ce_closure.obj ? e.ce_closure.obj : static_cast<const void*>(e.ce_closure.entries.root()),
                               e.ce_closure.env_pos);
    if (auto it = envs_.find(key); it != envs_.end()) return it->second;
    return envs_[key] = make();
  }
  V event(const instruct::DebugEvent* ev) {
    using EK = instruct::DebugEventKindK;
    using IK = instruct::DebugEventInfoK;
    using RK = instruct::DebugEventReprK;
    return w_.shared(memo_, ev, 0, [&]() -> std::vector<V> {
      V kind;
      switch (ev->ev_kind.k) {
        case EK::Event_before: kind = w_.i(0); break;
        case EK::Event_after: kind = o::vblock(0, {w_.ty(ev->ev_kind.after_type)}); break;
        case EK::Event_pseudo: kind = w_.i(1); break;
      }
      V info;
      switch (ev->ev_info.k) {
        case IK::Event_function: info = w_.i(0); break;
        case IK::Event_return: info = o::vblock(0, {w_.i(ev->ev_info.return_arity)}); break;
        case IK::Event_other: info = w_.i(1); break;
      }
      V repr = w_.i(0);  // Event_none
      if (ev->ev_repr.k != RK::Event_none) {
        const lambda::IntRef* r = ev->ev_repr.ref;
        V ref = w_.shared(memo_, r, 0, [&]() -> std::vector<V> { return {w_.i(r->contents)}; });
        repr = o::vblock(ev->ev_repr.k == RK::Event_parent ? 0 : 1, {ref});
      }
      // ev_typsubst: Bytegen's is Subst.identity, one static record
      if (!identity_)
        identity_ = o::vblock(0, {w_.i(0), w_.i(0), w_.i(0), w_.b(false), w_.none()});
      return {w_.i(ev->ev_pos), w_.unit_name(ev->ev_module), w_.loc(ev->ev_loc), kind, w_.str(ev->ev_defname),
              info, summary(ev->ev_typenv), identity_, compenv(ev->ev_compenv), w_.i(ev->ev_stacksize), repr};
    });
  }

 private:
  Writer& w_;
  std::unordered_map<const void*, V> memo_;
  std::map<std::pair<const void*, long>, V> closures_;
  std::map<std::tuple<const void*, bool, const void*, long>, V> envs_;
  V identity_;
};

}  // namespace

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
  return o::marshal(o::vlist(evs));
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

std::string output_cmi(const std::string& filename, const CmiInfos& cmi) {
  // (the provided signature must have been substituted for saving)
  Writer w;
  w.set_current_unit(cmi.cmi_name);
  o::ValPtr name = w.unit_name(cmi.cmi_name);
  o::ValPtr header = o::vblock(0, {name, w.signature(cmi.cmi_sign)});
  std::vector<std::uint8_t> hbytes = o::marshal(header);
  std::string prefix(cmi_magic_number);
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
  // Misc.output_to_file_via_temporary
  std::string tmp = filename + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open " + tmp);
    out.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    out.write(reinterpret_cast<const char*>(cbytes.data()), static_cast<std::streamsize>(cbytes.size()));
    out.write(reinterpret_cast<const char*>(fbytes.data()), static_cast<std::streamsize>(fbytes.size()));
    if (!out) throw std::runtime_error("Cannot write " + tmp);
  }
  if (std::rename(tmp.c_str(), filename.c_str()) != 0) {
    std::remove(tmp.c_str());
    throw std::runtime_error("Cannot rename " + tmp + " to " + filename);
  }
  return crc;
}

CmiInfos read_cmi(const std::string& filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot open " + filename);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  const std::string magic = cmi_magic_number;
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
  try {
    m::Arena arena;
    std::size_t off = magic.size();
    std::size_t header = m::read_value(bytes.data(), bytes.size(), off, arena);
    std::size_t crcs = m::read_value(bytes.data(), bytes.size(), off, arena);
    std::size_t flags = m::read_value(bytes.data(), bytes.size(), off, arena);
    arena.finalize();
    Reader r(arena);
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

}  // namespace cppcaml::typing::cmi_format
