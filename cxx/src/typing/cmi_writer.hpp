// The write side of cmi_format.cpp (internal): output_value of the Types
// graph (Writer) and of the -g debug events' typing values (EventWriter),
// shared with the .cmt writer (cmt_format.cpp).  Not a public header.
#pragma once

#include <functional>
#include <map>
#include <tuple>
#include <unordered_map>

#include "cppcaml/flat_map.hpp"

#include "cppcaml/omarshal.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/instruct.hpp"
#include "cppcaml/typing/zone.hpp"

namespace cppcaml::typing::cmi_format::writer {

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
  // a declaration's `Some ty` block: one per token (types.hpp, SomeToken)
  V some_tok(const SomeToken& t, V x) {
    auto [it, fresh] = some_toks_.try_emplace(t.get(), nullptr);
    if (fresh) it->second = some(std::move(x));
    return it->second;
  }
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
    if (l.empty()) {
      std::vector<V> xs;
      for (auto& x : l) xs.push_back(elt(x));
      return o::vlist(xs);
    }
    auto key = std::make_pair(static_cast<const void*>(l.p), l.n);
    if (auto it = lists_.find(key); it != lists_.end()) return it->second;
    // the cells registered before the elements are written: an element may
    // reach its own list again (a Texpand's args through the expansion)
    Slice<T> tail = slice_tail(l);  // the cells past the prefix: the tail's list
    std::size_t n = l.size() - tail.size();
    std::vector<V> cells;
    for (std::size_t k = 0; k < n; ++k) cells.push_back(o::vblock(0, {}));
    lists_[key] = cells[0];
    for (std::size_t k = 0; k < n; ++k) {
      V h = elt(l[k]);
      cells[k]->fields = {h, k + 1 < n ? cells[k + 1] : tail.empty() ? i(0) : V{}};
    }
    if (!tail.empty()) cells[n - 1]->fields[1] = list(tail, elt);
    return cells[0];
  }
  V opt_str(const OptStr& x) {
    if (!x.some) return none();
    if (!x.obj) return some(str(x.v));
    if (auto it = memo_.find(x.obj); it != memo_.end()) return it->second;
    V v = some(str(x.v));
    memo_[x.obj] = v;
    return v;
  }
  // a block's fields as [shared] builds them (off the heap for a few)
  using Fields = SmallVec<V, 8>;
  // a block registered under [key] before its fields are filled
  template <class K, class F>
  V shared(FlatMap<const void*, V>& memo, const K* key, int tag, F&& fields) {
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    V v = o::vblock(tag, {});
    memo[key] = v;
    Fields fs = fields();
    v->fields.assign(fs.data(), fs.size());
    return v;
  }

  // ---- Ident / Path ----
  V unscoped(const ident::Unscoped* u) {
    return shared(memo_, u, 0, [&]() -> Fields {  // { mutable state }
      if (u->state == ident::Unscoped::State::Udesc)
        return {o::vblock(0, {o::vblock(0, {str(u->name), i(u->stamp)})})};
      return {o::vblock(1, {unscoped(u->ulink)})};
    });
  }
  V ident(Ident::t id) {
    using K = Ident::Kind;
    int tag = static_cast<int>(id->kind);  // Local, Scoped, Global, Predef, Unscoped
    return shared(memo_, id, tag, [&]() -> Fields {
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
    return shared(memo_, p, static_cast<int>(p->kind), [&]() -> Fields {
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
    if (current_unit_by_identity_) {
      if (n.data() == current_unit_view_.data() && n.size() == current_unit_view_.size()) return current_unit_name_;
      return str(n);
    }
    if (current_unit_name_ && n == current_unit_) return current_unit_name_;
    return str(n);
  }
  // the .cmt's: the unit's name is the one string [n] (uid::unit_name_
  // string); a uid read from a .cmi carries that file's own string
  void set_current_unit_identity(std::string_view n) {
    set_current_unit_shared(n);
    current_unit_by_identity_ = true;
    current_unit_view_ = n;
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
    if (l.obj) {  // a .cmi's label: that block's one value
      auto [it, fresh] = label_objs_.try_emplace(l.obj, nullptr);
      if (fresh) it->second = o::vblock(tag, {str(l.name)});
      return it->second;
    }
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
                        : position(mkpos(x->fields[0]->s, x->fields[1]->i, x->fields[2]->i, x->fields[3]->i));
        // a parsetree location record: that record's one value (loc())
        if (x->loc_rec) {
          return loc(*x->loc_rec);
        }
        if (x->loc_val) return loc(*x->loc_val);
        return shared(memo_, x, static_cast<int>(x->tag), [&]() -> Fields {
          std::vector<V> fs;
          for (const OValue* f : x->fields) fs.push_back(ovalue(f));
          return fs;
        });
    }
    return i(0);
  }
  V attributes(const Attributes& as) {
    return list(as, [&](const Attribute* a) {
      return shared(memo_, a, 0, [&]() -> Fields {
        V name;
        if (a->name_obj) {
          V l = loc(a->attr_name_loc);
          auto [it, fresh] = attr_names_.try_emplace(std::pair<const void*, const void*>(a->name_obj, l.get()), nullptr);
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
      case Commutable::Kind::Cvar: return shared(memo_, c, 0, [&]() -> Fields { return {commu(c->commu)}; });
    }
    return i(0);
  }
  V field_kind(const FieldKind* k) {
    switch (k->kind) {
      case FieldKind::Kind::FKprivate: return i(0);
      case FieldKind::Kind::FKpublic: return i(1);
      case FieldKind::Kind::FKabsent: return i(2);
      case FieldKind::Kind::FKvar:
        return shared(memo_, k, 0, [&]() -> Fields { return {field_kind(k->field_kind)}; });
    }
    return i(0);
  }
  V path_args(const PathArgs* pa) {
    return shared(memo_, pa, 0, [&]() -> Fields { return {path(pa->path), path_args_list(pa)}; });
  }
  // `x :: tail` with the tail list shared (PathArgs::tail), else the list
  V path_args_list(const PathArgs* pa) {
    if (pa->tail.empty()) return tys(pa->args);
    auto key = std::make_pair(static_cast<const void*>(pa->args.p), pa->args.n);
    if (auto it = lists_.find(key); it != lists_.end()) return it->second;
    V v = o::vblock(0, {});
    lists_[key] = v;
    V hd = ty(pa->args[0]);
    v->fields = {hd, tys(pa->tail)};
    return v;
  }
  V name_ref(const NameRef* r) {
    return shared(memo_, r, 0, [&]() -> Fields {
      return {r->contents ? some(path_args(r->contents)) : none()};
    });
  }
  V memo(const AbbrevMemo* m) {
    switch (m->kind) {
      case AbbrevMemo::Kind::Mnil: return i(0);
      case AbbrevMemo::Kind::Mcons:
        return shared(memo_, m, 0, [&]() -> Fields {
          return {i(m->privacy == PrivateFlag::Private ? 0 : 1), path(m->path), ty(m->abbreviation),
                  ty(m->expansion), memo(m->rem)};
        });
      case AbbrevMemo::Kind::Mlink:
        return shared(memo_, m, 1, [&]() -> Fields { return {memo_ref(m->link)}; });
    }
    return i(0);
  }
  V memo_ref(const MemoRef* r) {
    return shared(memo_, r, 0, [&]() -> Fields { return {memo(r->contents)}; });
  }
  V row_cell(const RowFieldCell* c) {
    return shared(memo_, c, 0, [&]() -> Fields { return {row_field(c->contents)}; });
  }
  V row_field(const RowField* f) {
    switch (f->kind) {
      case RowField::Kind::RFabsent: return i(0);
      case RowField::Kind::RFnone: return i(1);
      case RowField::Kind::RFpresent:
        return shared(memo_, f, 0, [&]() -> Fields { return {f->present ? some(ty(f->present)) : none()}; });
      case RowField::Kind::RFeither:
        return shared(memo_, f, 1, [&]() -> Fields {
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
    return shared(memo_, r, 0, [&]() -> Fields {
      return {list(r->row_fields,
                   [&](const RowFieldEntry& e) {
                     return shared(memo_, e.obj, 0,
                                   [&]() -> Fields { return {str(e.label), row_field(e.field)}; });
                   }),
              ty(r->row_more), b(r->row_closed),
              // the options are passed on with the row's other fields
              // (create_row ~fixed:(row_fixed row) ~name:(row_name row)):
              // one `Some` per explanation / name record
              r->row_fixed ? some_shared(r->row_fixed, [&] { return fixed(r->row_fixed); }) : none(),
              r->row_name ? some_shared(r->row_name, [&] { return path_args(r->row_name); }) : none()};
    });
  }
  V package(const Package* p) {
    return shared(memo_, p, 0, [&]() -> Fields {
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
    return shared(memo_, d, tag, [&]() -> Fields {
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
          return {ty(e->ty), shared(memo_, e->abbrev, 0, [&]() -> Fields {
                    return {path(e->abbrev->path), tys(e->abbrev->args), i(e->abbrev->level)};
                  })};
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
    return shared(memo_, t, 0, [&]() -> Fields { return {desc(t->desc), i(t->level), i(t->scope), i(t->id)}; });
  }

  // ---- declarations ----
  V label_decl(const LabelDeclaration* l) {
    return shared(memo_, l, 0, [&]() -> Fields {
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
    return shared(memo_, c, 0, [&]() -> Fields {
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
        V repr = record_repr(k->record_repr);
        return o::vblock(1, {list(k->labels, [&](const LabelDeclaration* l) { return label_decl(l); }), repr});
      }
      case KK::Type_variant:
        return o::vblock(2, {list(k->constructors, [&](const ConstructorDeclaration* c) { return cstr_decl(c); }),
                             i(k->variant_repr == VariantRepresentation::Variant_regular ? 0 : 1)});
      case KK::Type_external: return o::vblock(3, {str(k->external)});
    }
    return i(0);
  }
  // a record_representation: the constant ones immediate, a block one per
  // identity (RecordRepresentation::obj)
  V record_repr(const RecordRepresentation& r) {
    using RK = RecordRepresentation::Kind;
    auto block = [&]() -> V {
      switch (r.kind) {
        case RK::Record_unboxed: return o::vblock(0, {b(r.unboxed_inlined)});
        case RK::Record_inlined: return o::vblock(1, {i(r.inlined_tag)});
        case RK::Record_extension: return o::vblock(2, {path(r.extension)});
        default: return i(0);
      }
    };
    switch (r.kind) {
      case RK::Record_regular: return i(0);
      case RK::Record_float: return i(1);
      default:
        if (!r.obj) return block();
        if (auto it = memo_.find(r.obj); it != memo_.end()) return it->second;
        V repr = block();
        memo_[r.obj] = repr;
        return repr;
    }
  }
  V type_decl(const TypeDeclaration* d) {
    return shared(memo_, d, 0, [&]() -> Fields {
      return {tys(d->type_params), i(d->type_arity), type_kind(d->type_kind), private_flag(d->type_private),
              d->type_manifest ? some_tok(d->manifest_obj, ty(d->type_manifest)) : none(),
              list(d->type_variance, [&](variance::t v) { return i(v); }),
              list(d->type_separability, [&](Separability x) { return i(static_cast<long>(x)); }),
              b(d->type_is_newtype), i(d->type_expansion_scope), loc(d->type_loc), attributes(d->type_attributes),
              i(static_cast<long>(d->type_immediate)), b(d->type_unboxed_default), uid(d->type_uid)};
    });
  }
  V ext_constr(const ExtensionConstructor* e) {
    return shared(memo_, e, 0, [&]() -> Fields {
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
    return shared(memo_, c, 0, [&]() -> Fields {
      return {ty(c->csig_self), ty(c->csig_self_row), field_kind(c->csig_dummy_method),
              strmap<VarEntry>(c->csig_vars.root(), [&](const VarEntry& e) {
                return o::vblock(0, {mutable_flag(e.mut), virtual_flag(e.virt), ty(e.ty)});
              }),
              strmap<MethEntry>(c->csig_meths.root(), [&](const MethEntry& e) {
                // Mprivate k is made by add_method / reveal_private_methods
                // and copied by reference (Subst keeps a meths entry's
                // privacy): one value per identity
                V p = i(0);
                if (e.priv.is_private) {
                  const void* key = e.priv.obj ? e.priv.obj : e.priv.kind;
                  if (auto it = mprivate_.find(key); it != mprivate_.end()) {
                    p = it->second;
                  } else {
                    p = o::vblock(0, {field_kind(e.priv.kind)});
                    mprivate_[key] = p;
                  }
                }
                return o::vblock(0, {p, virtual_flag(e.virt), ty(e.ty)});
              })};
    });
  }
  V class_type(const ClassType* c) {
    using CK = ClassType::Kind;
    return shared(memo_, c, static_cast<int>(c->kind), [&]() -> Fields {
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
  // a Primitive.description record: one per description
  V prim_desc(const PrimitiveDescription* p) {
    if (auto it = prim_descs_.find(p); it != prim_descs_.end()) return it->second;
    V v = o::vblock(0, {str(p->prim_name), i(p->prim_arity), b(p->prim_alloc), str(p->prim_native_name),
                        list(p->prim_native_repr_args, [&](const NativeRepr& n) { return native_repr(n); }),
                        native_repr(p->prim_native_repr_res)});
    prim_descs_[p] = v;
    return v;
  }
  V value_kind(const ValueKind& k) {
    switch (k.kind) {
      case ValueKind::Kind::Val_reg: return i(0);
      case ValueKind::Kind::Val_prim: {
        // one `Val_prim prim` block per declaration: copies of the value
        // description (Subst, the signature's) keep its val_kind
        const PrimitiveDescription* p = k.prim;
        if (auto it = val_prims_.find(p); it != val_prims_.end()) return it->second;
        V v = o::vblock(0, {prim_desc(p)});
        val_prims_[p] = v;
        return v;
      }
      case ValueKind::Kind::Val_ivar:
        return shared_by(k.obj, [&] { return o::vblock(1, {mutable_flag(k.ivar_mut), str(k.ivar_name)}); });
      // (never in a signature: a debug event's Env summary)
      case ValueKind::Kind::Val_self: return shared_by(k.obj, [&] {
        // the self_meths value: typeclass builds one per class
        // (self_var_kind) for every Val_self of it -- one per k.meths
        V meths;
        if (auto it = self_meths_.find(k.meths); it != self_meths_.end()) {
          meths = it->second;
        } else if (!k.self_virtual) {
          meths = o::vblock(0, {ident_map(*k.meths)});  // Self_concrete
          self_meths_[k.meths] = meths;
        } else {                                        // Self_virtual of a ref
          V r = shared(memo_, k.meths, 0, [&]() -> Fields { return {ident_map(*k.meths)}; });
          meths = o::vblock(1, {r});
          self_meths_[k.meths] = meths;
        }
        return o::vblock(2, {class_sig(k.sign), meths, ident_map(k.vars), str(k.cl_num)});
      });
      case ValueKind::Kind::Val_anc: return shared_by(k.obj, [&] {
        return o::vblock(3, {class_sig(k.sign), ident_map(*k.meths), str(k.cl_num)});
      });
    }
    return i(0);
  }
  // an `Ident.t Meths.t` / `Vars.t`: one value per map node
  V ident_map(const StrMap<Ident::t>& m) { return strmap_shared<Ident::t>(m.root(), [&](Ident::t id) { return ident(id); }); }
  template <class Val, class F>
  V strmap_shared(const StrMapNode<Val>* n, F&& data) {
    if (!n) return i(0);
    return shared(memo_, n, 0, [&]() -> Fields {
      V l = strmap_shared<Val>(n->l, data);
      V k = str(n->v);
      V d = data(n->d);
      V r = strmap_shared<Val>(n->r, data);
      return {l, k, d, r, i(n->h)};
    });
  }
  V module_type(const ModuleType* mt) {
    using MK = ModuleType::Kind;
    return shared(memo_, mt, static_cast<int>(mt->kind), [&]() -> Fields {
      switch (mt->kind) {
        case MK::Mty_ident: return {path(mt->path)};
        case MK::Mty_signature: return {signature(mt->sign)};
        case MK::Mty_functor: {
          V param = mt->param.is_unit
                        ? i(0)
                        : shared_by(mt->param.named_obj, [&] {
                            return o::vblock(0, {mt->param.id ? some_shared(mt->param.some_obj,
                                                                            [&] { return ident(mt->param.id); })
                                                              : none(),
                                                 module_type(mt->param.mty)});
                          });
          return {param, module_type(mt->res)};
        }
        case MK::Mty_alias: return {path(mt->path)};
      }
      return {};
    });
  }
  // the declarations, one value per record
  V value_desc(const ValueDescription* vd) {
    return shared(memo_, vd, 0, [&]() -> Fields {
      return {ty(vd->val_type), value_kind(vd->val_kind), loc(vd->val_loc), attributes(vd->val_attributes),
              uid(vd->val_uid)};
    });
  }
  V module_decl(const ModuleDeclaration* md) {
    return shared(memo_, md, 0, [&]() -> Fields {
      return {module_type(md->md_type), attributes(md->md_attributes), loc(md->md_loc), uid(md->md_uid)};
    });
  }
  V modtype_decl(const ModtypeDeclaration* mtd) {
    return shared(memo_, mtd, 0, [&]() -> Fields {
      return {mtd->mtd_type ? some(module_type(mtd->mtd_type)) : none(), attributes(mtd->mtd_attributes),
              loc(mtd->mtd_loc), uid(mtd->mtd_uid)};
    });
  }
  V class_decl(const ClassDeclaration* cd) {
    return shared(memo_, cd, 0, [&]() -> Fields {
      return {tys(cd->cty_params), class_type(cd->cty_type), path(cd->cty_path),
              cd->cty_new ? some_tok(cd->new_obj, ty(cd->cty_new)) : none(), variances(cd->cty_variance), loc(cd->cty_loc),
              attributes(cd->cty_attributes), uid(cd->cty_uid)};
    });
  }
  V cltype_decl(const ClassTypeDeclaration* cd) {
    return shared(memo_, cd, 0, [&]() -> Fields {
      return {tys(cd->clty_params), class_type(cd->clty_type), path(cd->clty_path), type_decl(cd->clty_hash_type),
              variances(cd->clty_variance), loc(cd->clty_loc), attributes(cd->clty_attributes), uid(cd->clty_uid)};
    });
  }
  // one value per signature item: an item is one object in every list
  // holding it (a structure's str_type and an include's incl_type ...)
  V sig_item(const SignatureItem* it) {
    if (auto m = sig_items_.find(it); m != sig_items_.end()) return m->second;
    V v = sig_item_(it);
    sig_items_[it] = v;
    return v;
  }
  V sig_item_(const SignatureItem* it) {
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
  FlatMap<const void*, V> memo_;
  FlatMap<std::string, V> fnames_;
  std::string current_unit_;
  V current_unit_name_;
  bool current_unit_by_identity_ = false;
  std::string_view current_unit_view_;
  FlatMap<std::pair<const char*, std::size_t>, V> strs_;
  FlatMap<std::pair<const void*, std::size_t>, V> lists_;
  FlatMap<const void*, V> mprivate_;
  FlatMap<std::tuple<int, const char*, std::size_t>, V> labels_;
  FlatMap<const void*, V> label_objs_;
  FlatMap<std::tuple<const void*, long, long, long>, V> poss_;
  FlatMap<std::tuple<const void*, const void*, bool>, V> locs_;
  FlatMap<const void*, V> self_meths_;  // Val_self's self_meths blocks
  FlatMap<const void*, V> pos_objs_, loc_objs_;
  FlatMap<const void*, V> somes_;
  FlatMap<std::uint64_t, V> some_toks_;
  FlatMap<const void*, V> val_prims_;
  FlatMap<const void*, V> prim_descs_;
  FlatMap<const void*, V> sig_items_;
  FlatMap<const void*, V> by_obj_;
  FlatMap<std::pair<const void*, const void*>, V> attr_names_;
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
    return w_.shared(memo_, s, tag, [&]() -> Writer::Fields {
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
    return w_.shared(memo_, n, 0, [&]() -> Writer::Fields {
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
    return w_.shared(memo_, n, 0, [&]() -> Writer::Fields {
      V l = tbl<A>(n->l, data);
      V d = tbl_data<A>(n->d, data);
      V r = tbl<A>(n->r, data);
      return {l, d, r, w_.i(n->h)};
    });
  }
  template <class A, class F>
  V tbl_data(const ident::TblData<A>* d, F&& data) {
    return w_.shared(memo_, d, 0, [&]() -> Writer::Fields {
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
    return w_.shared(memo_, ev, 0, [&]() -> Writer::Fields {
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
        V ref = w_.shared(memo_, r, 0, [&]() -> Writer::Fields { return {w_.i(r->contents)}; });
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
  FlatMap<const void*, V> memo_;
  FlatMap<std::pair<const void*, long>, V> closures_;
  FlatMap<std::tuple<const void*, bool, const void*, long>, V> envs_;
  V identity_;
};

}  // namespace cppcaml::typing::cmi_format::writer
