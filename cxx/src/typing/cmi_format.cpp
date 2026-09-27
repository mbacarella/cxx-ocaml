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

std::pair<std::string, std::string> output_cmi_bytes(const CmiInfos& cmi) {
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
