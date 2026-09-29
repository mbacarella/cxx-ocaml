// c++typing-dump: the C++ half of the oracles for the typing/ port
// (TYPECHECKER.md).  Decodes a .cmi with typing::cmi_format::read_cmi and
// prints the same structural dump as cxx/harness/typing_dump.ml, statement
// for statement, so first-visit numbering follows the same traversal order.
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

#include "cppcaml/typing/cmi_format.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/env.hpp"
#include "cppcaml/typing/parsetree.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/error_report.hpp"
#include "cppcaml/typing/typemod.hpp"
#include "cppcaml/typing/typeclass.hpp"
#include "cppcaml/typing/primitive.hpp"
#include "cppcaml/typing/typetexp.hpp"
#include "cppcaml/parser.hpp"
#include <sstream>
#include <fstream>
#include <functional>

using namespace cppcaml::typing;

namespace {

std::string b;
void s(std::string_view x) { b.append(x); }
void i(long n) { b += std::to_string(n); }

// Printf "%S": String.escaped between quotes.
void q(std::string_view x) {
  b += '"';
  for (unsigned char c : x) {
    switch (c) {
      case '"': b += "\\\""; break;
      case '\\': b += "\\\\"; break;
      case '\n': b += "\\n"; break;
      case '\t': b += "\\t"; break;
      case '\r': b += "\\r"; break;
      case '\b': b += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') {
          b += static_cast<char>(c);
        } else {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03u", static_cast<unsigned>(c));
          b += buf;
        }
    }
  }
  b += '"';
}

std::unordered_map<std::string, std::pair<std::unordered_map<const void*, long>, long>> tbls;
bool canonical = false;
std::unordered_map<long, long> ids_tbl, stamps_tbl;
long canon(std::unordered_map<long, long>& t, long n) {
  if (!canonical) return n;
  auto it = t.find(n);
  if (it != t.end()) return it->second;
  long k = static_cast<long>(t.size());
  t[n] = k;
  return k;
}
void reset_numbering() {
  tbls.clear();
  ids_tbl.clear();
  stamps_tbl.clear();
}
std::pair<long, bool> visit(const std::string& kind, const void* v) {
  auto& [t, c] = tbls[kind];
  if (auto it = t.find(v); it != t.end()) return {it->second, false};
  long n = c++;
  t[v] = n;
  return {n, true};
}

void bool_(bool x) { s(x ? "true" : "false"); }
void optstr(OptStr o) {
  if (!o.some) { s("None"); return; }
  s("(Some "); q(o.v); s(")");
}
template <class T, class F>
void list(const T& l, F&& f) {
  s("[");
  bool first = true;
  for (auto& x : l) {
    if (!first) s("; ");
    first = false;
    f(x);
  }
  s("]");
}
template <class P, class F>
void opt(P p, F&& f) {
  if (!p) { s("None"); return; }
  s("(Some "); f(p); s(")");
}

void pos(const Position& p) {
  s("{"); q(p.pos_fname); s(" "); i(p.pos_lnum); s(" "); i(p.pos_bol); s(" ");
  i(p.pos_cnum); s("}");
}
void loc(const Location& l) {
  s("<"); pos(l.loc_start); s(" "); pos(l.loc_end);
  if (l.loc_ghost) s(" ghost");
  s(">");
}

void hexfloat(double d) {
  if (std::isnan(d)) { s("nan"); return; }
  if (std::isinf(d)) { s(d > 0 ? "infinity" : "-infinity"); return; }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%a", d);
  s(buf);
}

void generic(const OValue* v) {
  switch (v->kind) {
    case OValue::Kind::Int: i(v->i); return;
    case OValue::Kind::String: q(v->s); return;
    case OValue::Kind::Double: hexfloat(v->d); return;
    case OValue::Kind::Block: {
      auto [n, first] = visit("G", v);
      if (!first) { s("@G"); i(n); return; }
      s("#G"); i(n); s("{"); i(v->tag);
      for (auto* x : v->fields) { s(" "); generic(x); }
      s("}");
    }
  }
}

void unscoped(ident::Unscoped* u) {
  auto [n, first] = visit("U", u);
  if (!first) { s("@U"); i(n); return; }
  s("#U"); i(n); s("{");
  if (u->state == ident::Unscoped::State::Udesc) {
    s("Udesc "); q(u->name); s(" "); i(canon(stamps_tbl, u->stamp));
  } else {
    // the oracle prints the linked cell generically; never occurs in a cmi
    s("Ulink ?");
  }
  s("}");
}

void ident_(Ident::t id) {
  using K = Ident::Kind;
  switch (id->kind) {
    case K::Local: s("Local("); q(id->name_); s(" "); i(canon(stamps_tbl, id->stamp_)); s(")"); break;
    case K::Scoped:
      s("Scoped("); q(id->name_); s(" "); i(canon(stamps_tbl, id->stamp_)); s(" "); i(id->scope_); s(")");
      break;
    case K::Global: s("Global("); q(id->name_); s(")"); break;
    case K::Predef: s("Predef("); q(id->name_); s(" "); i(id->stamp_); s(")"); break;
    case K::Unscoped: s("Unscoped("); unscoped(id->us); s(")"); break;
  }
}

void path_(Path::t p) {
  switch (p->kind) {
    case Path::Kind::Pident: s("Pident("); ident_(p->id); s(")"); break;
    case Path::Kind::Pdot: s("Pdot("); path_(p->p1); s(" "); q(p->s); s(")"); break;
    case Path::Kind::Papply: s("Papply("); path_(p->p1); s(" "); path_(p->p2); s(")"); break;
    case Path::Kind::Pextra_ty:
      s("Pextra_ty("); path_(p->p1); s(" ");
      if (p->extra == Path::Extra::Pcstr_ty) { s("Pcstr_ty "); q(p->s); }
      else s("Pext_ty");
      s(")");
      break;
  }
}

void arg_label(const ArgLabel& l) {
  switch (l.kind) {
    case ArgLabel::Kind::Nolabel: s("Nolabel"); break;
    case ArgLabel::Kind::Labelled: s("Labelled "); q(l.name); break;
    case ArgLabel::Kind::Optional: s("Optional "); q(l.name); break;
  }
}

void uid(const Uid& u) {
  switch (u.kind) {
    case Uid::Kind::Compilation_unit: s("Uid.Compilation_unit "); q(u.comp_unit); break;
    case Uid::Kind::Item:
      s("Uid.Item("); q(u.comp_unit); s(" "); i(u.id); s(" ");
      s(u.from == Uid::From::Intf ? "Intf" : "Impl"); s(")");
      break;
    case Uid::Kind::Local_opaque_item:
      s("Uid.Local_opaque_item("); q(u.comp_unit); s(" "); i(u.id); s(")");
      break;
    case Uid::Kind::Internal: s("Uid.Internal"); break;
    case Uid::Kind::Predef: s("Uid.Predef "); q(u.comp_unit); break;
  }
}

void attributes(Attributes l) {
  list(l, [](const Attribute* a) {
    s("@@"); q(a->attr_name); s(" "); loc(a->attr_name_loc); s(" "); loc(a->attr_loc);
    s(" "); generic(a->attr_payload);
  });
}

void ty(TypeExpr* t);

void commu(Commutable* c) {
  if (c->kind == Commutable::Kind::Cok) { s("Cok"); return; }
  if (c->kind == Commutable::Kind::Cunknown) { s("Cunknown"); return; }
  auto [n, first] = visit("C", c);
  if (!first) { s("@C"); i(n); return; }
  s("#C"); i(n); s("{Cvar "); commu(c->commu); s("}");
}

void field_kind(FieldKind* k) {
  switch (k->kind) {
    case FieldKind::Kind::FKprivate: s("FKprivate"); return;
    case FieldKind::Kind::FKpublic: s("FKpublic"); return;
    case FieldKind::Kind::FKabsent: s("FKabsent"); return;
    case FieldKind::Kind::FKvar: break;
  }
  auto [n, first] = visit("K", k);
  if (!first) { s("@K"); i(n); return; }
  s("#K"); i(n); s("{FKvar "); field_kind(k->field_kind); s("}");
}

void path_args(const PathArgs* pa) {
  path_(pa->path); s(" "); list(pa->args, [](TypeExpr* t) { ty(t); });
}

void name_ref(NameRef* r) {
  auto [n, first] = visit("N", r);
  if (!first) { s("@N"); i(n); return; }
  s("#N"); i(n); s("{");
  opt(r->contents, path_args);
  s("}");
}

void memo_ref(MemoRef* r);
void memo(const AbbrevMemo* m) {
  switch (m->kind) {
    case AbbrevMemo::Kind::Mnil: s("Mnil"); break;
    case AbbrevMemo::Kind::Mcons:
      s("Mcons("); s(m->privacy == PrivateFlag::Private ? "Private" : "Public");
      s(" "); path_(m->path); s(" "); ty(m->abbreviation); s(" "); ty(m->expansion);
      s(" "); memo(m->rem); s(")");
      break;
    case AbbrevMemo::Kind::Mlink: s("Mlink "); memo_ref(m->link); break;
  }
}
void memo_ref(MemoRef* r) {
  auto [n, first] = visit("M", r);
  if (!first) { s("@M"); i(n); return; }
  s("#M"); i(n); s("{"); memo(r->contents); s("}");
}

void row_field(const RowField* f) {
  switch (f->kind) {
    case RowField::Kind::RFpresent:
      s("RFpresent "); opt(f->present, ty);
      break;
    case RowField::Kind::RFabsent:
      s("RFabsent");
      break;
    case RowField::Kind::RFeither: {
      s("RFeither("); bool_(f->no_arg); s(" "); list(f->arg_type, [](TypeExpr* t) { ty(t); });
      s(" "); bool_(f->matched); s(" ");
      auto [n, first] = visit("E", f->ext);
      if (!first) { s("@E"); i(n); }
      else {
        s("#E"); i(n); s("{");
        if (f->ext->contents->kind == RowField::Kind::RFnone) s("RFnone");
        else row_field(f->ext->contents);
        s("}");
      }
      s(")");
      break;
    }
    case RowField::Kind::RFnone:
      s("RFnone?");  // never a row_field
      break;
  }
}

void fixed(const FixedExplanation* x) {
  switch (x->kind) {
    case FixedExplanation::Kind::Univar: s("Univar "); ty(x->univar); break;
    case FixedExplanation::Kind::Fixed_private: s("Fixed_private"); break;
    case FixedExplanation::Kind::Reified: s("Reified "); path_(x->reified); break;
    case FixedExplanation::Kind::Rigid: s("Rigid"); break;
  }
}

void row(const RowDesc* r) {
  s("{");
  list(r->row_fields, [](const RowFieldEntry& e) { q(e.label); s(":"); row_field(e.field); });
  s(" "); ty(r->row_more);
  s(" "); bool_(r->row_closed);
  s(" "); opt(r->row_fixed, fixed);
  s(" "); opt(r->row_name, path_args);
  s("}");
}

void package(const Package* p) {
  s("{"); path_(p->pack_path); s(" ");
  list(p->pack_constraints, [](const PackConstraint& c) {
    list(c.path, [](std::string_view x) { q(x); }); s(" "); ty(c.ty);
  });
  s("}");
}

void tylist(Slice<TypeExpr*> l) { list(l, [](TypeExpr* t) { ty(t); }); }

void ty(TypeExpr* t) {
  auto [n, first] = visit("T", t);
  if (!first) { s("@T"); i(n); return; }
  s("#T"); i(n); s("{"); i(t->level); s(" "); i(t->scope); s(" "); i(canon(ids_tbl, t->id)); s(" ");
  const TypeDesc* d = t->desc;
  switch (d->kind) {
    case DescKind::Tvar: s("Tvar "); optstr(as<Tvar>(d)->name); break;
    case DescKind::Tarrow: {
      auto* a = as<Tarrow>(d);
      s("Tarrow("); arg_label(a->label); s(" "); ty(a->t1); s(" "); ty(a->t2); s(" ");
      commu(a->commu); s(")");
      break;
    }
    case DescKind::Ttuple:
      s("Ttuple ");
      list(as<Ttuple>(d)->elems, [](const LabeledTy& e) { optstr(e.label); s(":"); ty(e.ty); });
      break;
    case DescKind::Tconstr: {
      auto* c = as<Tconstr>(d);
      s("Tconstr("); path_(c->path); s(" "); tylist(c->args); s(" "); memo_ref(c->memo);
      s(")");
      break;
    }
    case DescKind::Tobject: {
      auto* o = as<Tobject>(d);
      s("Tobject("); ty(o->fields); s(" "); name_ref(o->name); s(")");
      break;
    }
    case DescKind::Tfield: {
      auto* f = as<Tfield>(d);
      s("Tfield("); q(f->label); s(" "); field_kind(f->kind_); s(" "); ty(f->ty); s(" ");
      ty(f->rest); s(")");
      break;
    }
    case DescKind::Tnil: s("Tnil"); break;
    case DescKind::Tvariant: s("Tvariant "); row(as<Tvariant>(d)->row); break;
    case DescKind::Tunivar: s("Tunivar "); optstr(as<Tunivar>(d)->name); break;
    case DescKind::Tpoly: {
      auto* p = as<Tpoly>(d);
      s("Tpoly("); ty(p->body); s(" "); tylist(p->vars); s(")");
      break;
    }
    case DescKind::Tpackage: s("Tpackage "); package(as<Tpackage>(d)->pack); break;
    case DescKind::Tfunctor: {
      auto* f = as<Tfunctor>(d);
      s("Tfunctor("); arg_label(f->label); s(" "); unscoped(f->id); s(" "); package(f->pack);
      s(" "); ty(f->body); s(")");
      break;
    }
    case DescKind::Texpand: {
      auto* e = as<Texpand>(d);
      s("Texpand("); ty(e->ty); s(" "); path_(e->path); s(" "); tylist(e->args); s(")");
      break;
    }
    case DescKind::Tlink: s("Tlink "); ty(as<Tlink>(d)->ty); break;
    case DescKind::Tsubst: {
      auto* x = as<Tsubst>(d);
      s("Tsubst("); ty(x->ty); s(" "); opt(x->row, ty); s(")");
      break;
    }
  }
  s("}");
}

// ---- declarations ----
void private_flag(PrivateFlag p) { s(p == PrivateFlag::Private ? "Private" : "Public"); }
void mutable_flag(MutableFlag m) { s(m == MutableFlag::Immutable ? "Immutable" : "Mutable"); }
void virtual_flag(VirtualFlag v) { s(v == VirtualFlag::Virtual ? "Virtual" : "Concrete"); }
void separability(Separability x) {
  s(x == Separability::Ind ? "Ind" : x == Separability::Sep ? "Sep" : "Deepsep");
}

void native_repr(const NativeRepr& n) {
  switch (n.kind) {
    case NativeRepr::Kind::Same_as_ocaml_repr: s("Same_as_ocaml_repr"); break;
    case NativeRepr::Kind::Unboxed_float: s("Unboxed_float"); break;
    case NativeRepr::Kind::Unboxed_integer:
      s("Unboxed_integer ");
      s(n.bi == BoxedInteger::Pnativeint ? "Pnativeint"
        : n.bi == BoxedInteger::Pint32 ? "Pint32" : "Pint64");
      break;
    case NativeRepr::Kind::Untagged_immediate: s("Untagged_immediate"); break;
  }
}

void label_decl(const LabelDeclaration* l) {
  s("{"); ident_(l->ld_id); s(" "); mutable_flag(l->ld_mutable); s(" ");
  s(l->ld_atomic == AtomicFlag::Nonatomic ? "Nonatomic" : "Atomic");
  s(" "); ty(l->ld_type); s(" "); loc(l->ld_loc); s(" "); attributes(l->ld_attributes);
  s(" "); uid(l->ld_uid); s("}");
}

void cstr_args(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Cstr_tuple) { s("Cstr_tuple "); tylist(a.tuple); }
  else { s("Cstr_record "); list(a.record, label_decl); }
}

void cstr_decl(const ConstructorDeclaration* c) {
  s("{"); ident_(c->cd_id); s(" "); cstr_args(c->cd_args); s(" "); opt(c->cd_res, ty);
  s(" "); loc(c->cd_loc); s(" "); attributes(c->cd_attributes); s(" "); uid(c->cd_uid);
  s("}");
}

void type_decl(const TypeDeclaration* d) {
  s("{params="); tylist(d->type_params); s(" arity="); i(d->type_arity);
  s(" kind=");
  const TypeKind* k = d->type_kind;
  switch (k->kind) {
    case TypeKind::Kind::Type_abstract:
      s("Type_abstract ");
      switch (k->origin.kind) {
        case TypeOrigin::Kind::Definition: s("Definition"); break;
        case TypeOrigin::Kind::Rec_check_regularity: s("Rec_check_regularity"); break;
        case TypeOrigin::Kind::Approx_recmod: s("Approx_recmod"); break;
        case TypeOrigin::Kind::Existential: s("Existential "); q(k->origin.existential); break;
        case TypeOrigin::Kind::Equation:
          s("Equation("); ty(k->origin.eq1); s(" "); ty(k->origin.eq2); s(")");
          break;
      }
      break;
    case TypeKind::Kind::Type_record:
      s("Type_record("); list(k->labels, label_decl); s(" ");
      switch (k->record_repr.kind) {
        case RecordRepresentation::Kind::Record_regular: s("Record_regular"); break;
        case RecordRepresentation::Kind::Record_float: s("Record_float"); break;
        case RecordRepresentation::Kind::Record_unboxed:
          s("Record_unboxed "); bool_(k->record_repr.unboxed_inlined); break;
        case RecordRepresentation::Kind::Record_inlined:
          s("Record_inlined "); i(k->record_repr.inlined_tag); break;
        case RecordRepresentation::Kind::Record_extension:
          s("Record_extension "); path_(k->record_repr.extension); break;
      }
      s(")");
      break;
    case TypeKind::Kind::Type_variant:
      s("Type_variant("); list(k->constructors, cstr_decl); s(" ");
      s(k->variant_repr == VariantRepresentation::Variant_regular ? "Variant_regular"
                                                                   : "Variant_unboxed");
      s(")");
      break;
    case TypeKind::Kind::Type_open: s("Type_open"); break;
    case TypeKind::Kind::Type_external: s("Type_external "); q(k->external); break;
  }
  s(" private="); private_flag(d->type_private);
  s(" manifest="); opt(d->type_manifest, ty);
  s(" variance="); list(d->type_variance, [](variance::t v) { i(v); });
  s(" separability="); list(d->type_separability, separability);
  s(" newtype="); bool_(d->type_is_newtype);
  s(" expansion_scope="); i(d->type_expansion_scope);
  s(" loc="); loc(d->type_loc);
  s(" attrs="); attributes(d->type_attributes);
  s(" immediate=");
  s(d->type_immediate == TypeImmediacy::Unknown ? "Unknown"
    : d->type_immediate == TypeImmediacy::Always ? "Always" : "Always_on_64bits");
  s(" unboxed_default="); bool_(d->type_unboxed_default);
  s(" uid="); uid(d->type_uid); s("}");
}

void ext_constr(const ExtensionConstructor* e) {
  s("{"); path_(e->ext_type_path); s(" "); tylist(e->ext_type_params); s(" ");
  cstr_args(e->ext_args); s(" "); opt(e->ext_ret_type, ty); s(" ");
  private_flag(e->ext_private); s(" "); loc(e->ext_loc); s(" ");
  attributes(e->ext_attributes); s(" "); uid(e->ext_uid); s("}");
}

void class_sig(ClassSignature* c) {
  s("{"); ty(c->csig_self); s(" "); ty(c->csig_self_row); s(" ");
  field_kind(c->csig_dummy_method); s(" ");
  list(c->csig_vars.bindings(), [](const std::pair<std::string_view, VarEntry>& e) {
    q(e.first); s(":"); mutable_flag(e.second.mut); s(" "); virtual_flag(e.second.virt);
    s(" "); ty(e.second.ty);
  });
  s(" ");
  list(c->csig_meths.bindings(), [](const std::pair<std::string_view, MethEntry>& e) {
    q(e.first); s(":");
    if (!e.second.priv.is_private) s("Mpublic");
    else { s("Mprivate "); field_kind(e.second.priv.kind); }
    s(" "); virtual_flag(e.second.virt); s(" "); ty(e.second.ty);
  });
  s("}");
}

void class_type(const ClassType* c) {
  switch (c->kind) {
    case ClassType::Kind::Cty_constr:
      s("Cty_constr("); path_(c->path); s(" "); tylist(c->args); s(" "); class_type(c->cty);
      s(")");
      break;
    case ClassType::Kind::Cty_signature: s("Cty_signature "); class_sig(c->sign); break;
    case ClassType::Kind::Cty_arrow:
      s("Cty_arrow("); arg_label(c->label); s(" "); ty(c->arg); s(" "); class_type(c->cty);
      s(")");
      break;
  }
}

void value_kind(const ValueKind& k) {
  switch (k.kind) {
    case ValueKind::Kind::Val_reg: s("Val_reg"); break;
    case ValueKind::Kind::Val_prim: {
      auto* p = k.prim;
      s("Val_prim{"); q(p->prim_name); s(" "); i(p->prim_arity); s(" "); bool_(p->prim_alloc);
      s(" "); q(p->prim_native_name); s(" "); list(p->prim_native_repr_args, native_repr);
      s(" "); native_repr(p->prim_native_repr_res); s("}");
      break;
    }
    case ValueKind::Kind::Val_ivar:
      s("Val_ivar("); mutable_flag(k.ivar_mut); s(" "); q(k.ivar_name); s(")");
      break;
    case ValueKind::Kind::Val_self: s("Val_self"); break;
    case ValueKind::Kind::Val_anc: s("Val_anc"); break;
  }
}

void rec_status(RecStatus r) {
  s(r == RecStatus::Trec_not ? "Trec_not" : r == RecStatus::Trec_first ? "Trec_first"
                                                                       : "Trec_next");
}
void visibility(Visibility v) { s(v == Visibility::Exported ? "Exported" : "Hidden"); }

void signature(int depth, Signature sg);

void modtype(int depth, const ModuleType* m) {
  switch (m->kind) {
    case ModuleType::Kind::Mty_ident: s("Mty_ident "); path_(m->path); break;
    case ModuleType::Kind::Mty_signature: s("Mty_signature"); signature(depth + 1, m->sign); break;
    case ModuleType::Kind::Mty_functor:
      if (m->param.is_unit) {
        s("Mty_functor(Unit "); modtype(depth, m->res); s(")");
      } else {
        s("Mty_functor(Named("); opt(m->param.id, ident_); s(" "); modtype(depth, m->param.mty);
        s(") "); modtype(depth, m->res); s(")");
      }
      break;
    case ModuleType::Kind::Mty_alias: s("Mty_alias "); path_(m->path); break;
  }
}

void item(int depth, const SignatureItem* it) {
  using SK = SignatureItem::Kind;
  switch (it->kind) {
    case SK::Sig_value: {
      auto* vd = it->value;
      s("Sig_value "); ident_(it->id); s(" {"); ty(vd->val_type); s(" ");
      value_kind(vd->val_kind); s(" "); loc(vd->val_loc); s(" ");
      attributes(vd->val_attributes); s(" "); uid(vd->val_uid); s("} ");
      visibility(it->vis);
      break;
    }
    case SK::Sig_type:
      s("Sig_type "); ident_(it->id); s(" "); type_decl(it->type); s(" "); rec_status(it->rec);
      s(" "); visibility(it->vis);
      break;
    case SK::Sig_typext:
      s("Sig_typext "); ident_(it->id); s(" "); ext_constr(it->ext); s(" ");
      s(it->ext_status == ExtStatus::Text_first ? "Text_first"
        : it->ext_status == ExtStatus::Text_next ? "Text_next" : "Text_exception");
      s(" "); visibility(it->vis);
      break;
    case SK::Sig_module: {
      auto* md = it->md;
      s("Sig_module "); ident_(it->id); s(" ");
      s(it->presence == ModulePresence::Mp_present ? "Mp_present" : "Mp_absent");
      s(" {"); modtype(depth, md->md_type); s(" "); attributes(md->md_attributes);
      s(" "); loc(md->md_loc); s(" "); uid(md->md_uid); s("} "); rec_status(it->rec); s(" ");
      visibility(it->vis);
      break;
    }
    case SK::Sig_modtype: {
      auto* mtd = it->mtd;
      s("Sig_modtype "); ident_(it->id); s(" {");
      opt(mtd->mtd_type, [&](const ModuleType* m) { modtype(depth, m); });
      s(" "); attributes(mtd->mtd_attributes); s(" "); loc(mtd->mtd_loc); s(" ");
      uid(mtd->mtd_uid); s("} "); visibility(it->vis);
      break;
    }
    case SK::Sig_class: {
      auto* cd = it->cls;
      s("Sig_class "); ident_(it->id); s(" {"); tylist(cd->cty_params); s(" ");
      class_type(cd->cty_type); s(" "); path_(cd->cty_path); s(" ");
      opt(cd->cty_new, ty); s(" "); list(cd->cty_variance, [](variance::t v) { i(v); });
      s(" "); loc(cd->cty_loc); s(" "); attributes(cd->cty_attributes); s(" ");
      uid(cd->cty_uid); s("} "); rec_status(it->rec); s(" "); visibility(it->vis);
      break;
    }
    case SK::Sig_class_type: {
      auto* ct = it->clty;
      s("Sig_class_type "); ident_(it->id); s(" {"); tylist(ct->clty_params); s(" ");
      class_type(ct->clty_type); s(" "); path_(ct->clty_path); s(" ");
      type_decl(ct->clty_hash_type); s(" ");
      list(ct->clty_variance, [](variance::t v) { i(v); });
      s(" "); loc(ct->clty_loc); s(" "); attributes(ct->clty_attributes); s(" ");
      uid(ct->clty_uid); s("} "); rec_status(it->rec); s(" "); visibility(it->vis);
      break;
    }
  }
}

void signature(int depth, Signature sg) {
  s("[");
  for (auto* it : sg) {
    s("\n"); s(std::string(2 * depth, ' '));
    item(depth, it);
  }
  s("]");
}

std::string to_hex(const std::string& d) {
  static const char* hx = "0123456789abcdef";
  std::string o;
  for (unsigned char c : d) { o += hx[c >> 4]; o += hx[c & 15]; }
  return o;
}


// ---- stage 2: Env queries (the typing_dump.ml `env` mode) ----
void cstr_tag(const ConstructorTag& t) {
  switch (t.kind) {
    case ConstructorTag::Kind::Cstr_constant: s("Cstr_constant "); i(t.n); break;
    case ConstructorTag::Kind::Cstr_block: s("Cstr_block "); i(t.n); break;
    case ConstructorTag::Kind::Cstr_unboxed: s("Cstr_unboxed"); break;
    case ConstructorTag::Kind::Cstr_extension:
      s("Cstr_extension("); path_(t.ext_path); s(" "); bool_(t.ext_constant); s(")");
      break;
  }
}

void cstr_descr(const ConstructorDescription* c) {
  s("{"); q(c->cstr_name); s(" "); ty(c->cstr_res); s(" "); tylist(c->cstr_existentials);
  s(" "); tylist(c->cstr_args); s(" "); i(c->cstr_arity); s(" "); cstr_tag(c->cstr_tag);
  s(" "); i(c->cstr_consts); s(" "); i(c->cstr_nonconsts); s(" "); bool_(c->cstr_generalized);
  s(" "); private_flag(c->cstr_private); s(" "); loc(c->cstr_loc); s(" ");
  attributes(c->cstr_attributes); s(" "); opt(c->cstr_inlined, type_decl); s(" ");
  uid(c->cstr_uid); s("}");
}

void lbl_descr(const LabelDescription* l) {
  s("{"); q(l->lbl_name); s(" "); ty(l->lbl_res); s(" "); ty(l->lbl_arg); s(" ");
  mutable_flag(l->lbl_mut); s(" ");
  s(l->lbl_atomic == AtomicFlag::Nonatomic ? "Nonatomic" : "Atomic");
  s(" "); i(l->lbl_pos); s(" "); i(static_cast<long>(l->lbl_all.size())); s(" ");
  switch (l->lbl_repres.kind) {
    case RecordRepresentation::Kind::Record_regular: s("Record_regular"); break;
    case RecordRepresentation::Kind::Record_float: s("Record_float"); break;
    case RecordRepresentation::Kind::Record_unboxed:
      s("Record_unboxed "); bool_(l->lbl_repres.unboxed_inlined); break;
    case RecordRepresentation::Kind::Record_inlined:
      s("Record_inlined "); i(l->lbl_repres.inlined_tag); break;
    case RecordRepresentation::Kind::Record_extension:
      s("Record_extension "); path_(l->lbl_repres.extension); break;
  }
  s(" "); private_flag(l->lbl_private); s(" "); loc(l->lbl_loc); s(" ");
  attributes(l->lbl_attributes); s(" "); uid(l->lbl_uid); s("}");
}

// "A.B.c", with functor applications "F(X).t" (as typing_dump.ml)
Longident::t lid_of_string(const std::string& str) {
  std::size_t pos = 0, n = str.size();
  auto ident = [&]() {
    std::size_t st = pos;
    while (pos < n && str[pos] != '.' && str[pos] != '(' && str[pos] != ')') ++pos;
    return str.substr(st, pos - st);
  };
  std::function<Longident::t()> path = [&]() -> Longident::t {
    Longident::t l = Longident::lident(ident());
    while (pos < n) {
      if (str[pos] == '(') {
        ++pos;
        Longident::t a = path();
        ++pos;  // ')'
        l = Longident::lapply(l, location::none(), a, location::none());
      } else if (str[pos] == '.') {
        ++pos;
        l = Longident::ldot(l, location::none(), ident(), location::none());
      } else {
        break;
      }
    }
    return l;
  };
  return path();
}

int run_env(const std::string& stdlib_dir, const std::string& queries) {
  canonical = true;
  std::vector<std::string> dirs;
  for (std::size_t st = 0;;) {
    std::size_t c = stdlib_dir.find(':', st);
    dirs.push_back(stdlib_dir.substr(st, c == std::string::npos ? std::string::npos : c - st));
    if (c == std::string::npos) break;
    st = c + 1;
  }
  load_path::init(dirs, {});
  env::t e0 = env::initial();
  env::OpenResult r = env::open_pers_signature("Stdlib", e0);
  if (r.kind != env::OpenResult::Kind::Ok) {
    std::cerr << "open Stdlib failed\n";
    return 1;
  }
  env::t e = r.env;
  std::ifstream in(queries);
  std::string line;
  while (std::getline(in, line)) {
    std::size_t k = line.find(' ');
    if (k == std::string::npos) continue;
    std::string kind = line.substr(0, k), name = line.substr(k + 1);
    Longident::t lid = lid_of_string(name);
    reset_numbering();
    s(line); s(" => ");
    try {
      if (kind == "value") {
        auto [p, vd] = env::find_value_by_name(lid, e);
        path_(p); s(" "); ty(vd->val_type); s(" "); value_kind(vd->val_kind);
      } else if (kind == "type") {
        auto [p, td] = env::find_type_by_name(lid, e);
        path_(p); s(" "); type_decl(td);
      } else if (kind == "constr") {
        cstr_descr(env::find_constructor_by_name(lid, e));
      } else if (kind == "label") {
        lbl_descr(env::find_label_by_name(lid, e));
      } else if (kind == "module") {
        auto [p, md] = env::find_module_by_name(lid, e);
        path_(p); s(" "); modtype(1, md->md_type);
      } else if (kind == "modtype") {
        auto [p, mtd] = env::find_modtype_by_name(lid, e);
        path_(p); s(" "); opt(mtd->mtd_type, [](const ModuleType* m) { modtype(1, m); });
      } else if (kind == "class") {
        auto [p, cd] = env::find_class_by_name(lid, e);
        path_(p); s(" "); class_type(cd->cty_type);
      } else if (kind == "cltype") {
        auto [p, ct] = env::find_cltype_by_name(lid, e);
        path_(p); s(" "); class_type(ct->clty_type);
      } else {
        s("BADKIND");
      }
    } catch (const env::NotFound&) {
      s("NOTFOUND");
    }
    s("\n");
  }
  std::cout << b;
  return 0;
}

// ---- stage 4a: Parsetree (the typing_dump.ml `parse` mode) ----
namespace pd {
using namespace parsetree;
using parsetree::Signature;
using parsetree::Structure;
using parsetree::SignatureItem;
using parsetree::StructureItem;
using parsetree::ModuleType;
using parsetree::ModuleExpr;
using parsetree::ClassType;
using parsetree::ClassExpr;
using parsetree::TypeDeclaration;
using parsetree::Attribute;
using parsetree::Attributes;
using parsetree::Extension;
using parsetree::Payload;
using parsetree::ValueDescription;
using parsetree::PrimitiveDescription;
using parsetree::ClassSignature;
using parsetree::ClassStructure;
using parsetree::ClassDeclaration;
using parsetree::ClassTypeDeclaration;
using parsetree::ClassDescription;
using parsetree::ExtensionConstructor;
using parsetree::LabelDeclaration;
using parsetree::ConstructorDeclaration;
using parsetree::ConstructorArguments;
using parsetree::TypeKind;
using parsetree::ModuleDeclaration;
using parsetree::ModuleTypeDeclaration;
using parsetree::ModuleBinding;
using parsetree::TypeExtension;
using parsetree::TypeException;
using parsetree::Constant;
using parsetree::CoreType;
using parsetree::Pattern;
using parsetree::Expression;
using parsetree::Case;
using parsetree::ValueBinding;
using parsetree::PackageType;
using parsetree::TypeParam;
using parsetree::FunctionParam;
using parsetree::OpenDescription;
using parsetree::FunctorParameter;
using parsetree::WithConstraint;
using parsetree::ClassField;
using parsetree::ClassTypeField;
using parsetree::ClassFieldKind;
using parsetree::ValueConstraint;
using parsetree::BindingOp;
using parsetree::ArgExpression;
using parsetree::StrLoc;
using parsetree::RowField;
using parsetree::ObjectField;
using parsetree::LidLoc;
std::string_view parse_file;
bool mask_gaps = true;

void ppos(const Position& p) {
  if (p.pos_fname != parse_file) { q(p.pos_fname); s(":"); }
  i(p.pos_lnum); s(","); i(p.pos_bol); s(","); i(p.pos_cnum);
}
void loc(const Location& l) {
  s("<"); ppos(l.loc_start); s("-"); ppos(l.loc_end); if (l.loc_ghost) s(" g"); s(">");
}
void gloc(const Location& l) {
  if (mask_gaps) s("?");
  else loc(l);
}
void str_loc(const StrLoc& x) { q(x.txt); s(" "); loc(x.loc); }
void str_gloc(const StrLoc& x) { q(x.txt); s(" "); gloc(x.loc); }
void lid(Longident::t l) {
  switch (l->kind) {
    case Longident::Kind::Lident: s("Lident "); q(l->s); break;
    case Longident::Kind::Ldot:
      s("Ldot("); lid(l->l1); s(" "); loc(l->l1_loc()); s(" "); q(l->s); s(" "); loc(l->s_loc()); s(")");
      break;
    case Longident::Kind::Lapply:
      s("Lapply("); lid(l->l1); s(" "); loc(l->l1_loc()); s(" "); lid(l->l2); s(" "); loc(l->l2_loc());
      s(")");
      break;
  }
}
void lid_loc(const LidLoc& x) { s("{"); lid(x.txt); s(" "); loc(x.loc); s("}"); }
void optstr(const OptStr& o) {
  if (!o.some) s("None");
  else { s("(Some "); q(o.v); s(")"); }
}
void flag_closed(ClosedFlag c) { s(c == ClosedFlag::Closed ? "Closed" : "Open"); }
void flag_rec(RecFlag r) { s(r == RecFlag::Nonrecursive ? "Nonrec" : "Rec"); }
void flag_mut(MutableFlag m) { s(m == MutableFlag::Immutable ? "Immutable" : "Mutable"); }
void flag_priv(PrivateFlag p) { s(p == PrivateFlag::Private ? "Private" : "Public"); }
void flag_virt(VirtualFlag v) { s(v == VirtualFlag::Virtual ? "Virtual" : "Concrete"); }
void flag_ovr(OverrideFlag o) { s(o == OverrideFlag::Override ? "Override" : "Fresh"); }
void char_opt(bool has, char c) {
  if (!has) s("None");
  else { s("Some "); i(static_cast<unsigned char>(c)); }
}
template <class T, class F>
void popt(const T* p, F&& f) {
  if (!p) { s("None"); return; }
  s("(Some "); f(p); s(")");
}

void constant(const Constant& c) {
  s("{");
  const ConstantDesc& d = c.pconst_desc;
  switch (d.kind) {
    case ConstantDesc::Kind::Pconst_integer:
      s("Pconst_integer "); q(d.s); s(" "); char_opt(d.has_suffix, d.suffix); break;
    case ConstantDesc::Kind::Pconst_char: s("Pconst_char "); i(static_cast<unsigned char>(d.c)); break;
    case ConstantDesc::Kind::Pconst_string:
      s("Pconst_string "); q(d.s); s(" "); loc(d.str_loc); s(" "); optstr(d.delim); break;
    case ConstantDesc::Kind::Pconst_float:
      s("Pconst_float "); q(d.s); s(" "); char_opt(d.has_suffix, d.suffix); break;
  }
  s(" "); loc(c.pconst_loc); s("}");
}

void core_type(const CoreType* t);
void pattern(const Pattern* p);
void expression(const Expression* e);
void structure(const Structure& st);
void signature(const Signature& sg);
void structure_item(const StructureItem* it);
void module_expr(const ModuleExpr* m);
void module_type(const ModuleType* m);
void class_structure(const ClassStructure* cs);
void class_type(const ClassType* x);
void class_expr(const ClassExpr* x);
void value_binding(const ValueBinding* vb);
void type_declaration(const TypeDeclaration* d);

void payload(const Payload& p) {
  switch (p.kind) {
    case Payload::Kind::PStr: s("PStr "); structure(p.str); break;
    case Payload::Kind::PSig: s("PSig "); signature(p.sig); break;
    case Payload::Kind::PTyp: s("PTyp "); core_type(p.typ); break;
    case Payload::Kind::PPat: s("PPat "); pattern(p.pat); s(" "); popt(p.guard, expression); break;
  }
}
void attribute(const Attribute* a, bool gap_name = false) {
  s("{attr ");
  (void)gap_name;
  str_loc(a->attr_name);
  s(" "); payload(a->attr_payload); s(" "); loc(a->attr_loc); s("}");
}
void attrs(const Attributes& l) { list(l, [](const Attribute* a) { attribute(a); }); }
void extension(const Extension* e) { s("{ext "); str_loc(e->name); s(" "); payload(e->payload); s("}"); }

void package_type(const PackageType* p, bool gap) {
  s("{pack "); lid_loc(p->ppt_path); s(" ");
  list(p->ppt_constraints, [](const std::pair<LidLoc, const CoreType*>& c) {
    lid_loc(c.first); s("="); core_type(c.second);
  });
  s(" ");
  if (gap) gloc(p->ppt_loc);
  else loc(p->ppt_loc);
  s(" "); attrs(p->ppt_attrs); s("}");
}
void core_types(const Slice<const CoreType*>& l) { list(l, [](const CoreType* t) { core_type(t); }); }

void core_type(const CoreType* t) {
  s("(T "); loc(t->ptyp_loc); s(" "); attrs(t->ptyp_attributes); s(" ");
  const CoreTypeDesc* d = t->ptyp_desc;
  using K = CoreTypeDesc::Kind;
  switch (d->kind) {
    case K::Ptyp_any: s("Ptyp_any"); break;
    case K::Ptyp_var: s("Ptyp_var "); q(as<Ptyp_var>(d)->name); break;
    case K::Ptyp_arrow: {
      auto* a = as<Ptyp_arrow>(d);
      s("Ptyp_arrow "); arg_label(a->label); s(" "); core_type(a->t1); s(" "); core_type(a->t2);
      break;
    }
    case K::Ptyp_tuple:
      s("Ptyp_tuple ");
      list(as<Ptyp_tuple>(d)->tl, [](const LabeledCoreType& x) { optstr(x.label); s(":"); core_type(x.ty); });
      break;
    case K::Ptyp_constr: {
      auto* c = as<Ptyp_constr>(d);
      s("Ptyp_constr "); lid_loc(c->lid); s(" "); core_types(c->args);
      break;
    }
    case K::Ptyp_object: {
      auto* o = as<Ptyp_object>(d);
      s("Ptyp_object ");
      list(o->fields, [](const ObjectField* f) {
        s("{");
        if (auto* ot = as<Otag>(f->pof_desc)) { s("Otag "); str_loc(ot->label); s(" "); core_type(ot->ty); }
        else { s("Oinherit "); core_type(as<Oinherit>(f->pof_desc)->ty); }
        s(" "); loc(f->pof_loc); s(" "); attrs(f->pof_attributes); s("}");
      });
      s(" "); flag_closed(o->closed);
      break;
    }
    case K::Ptyp_class: {
      auto* c = as<Ptyp_class>(d);
      s("Ptyp_class "); lid_loc(c->lid); s(" "); core_types(c->args);
      break;
    }
    case K::Ptyp_alias: {
      auto* a = as<Ptyp_alias>(d);
      s("Ptyp_alias "); core_type(a->ty); s(" "); str_loc(a->name);
      break;
    }
    case K::Ptyp_variant: {
      auto* v = as<Ptyp_variant>(d);
      s("Ptyp_variant ");
      list(v->fields, [](const RowField* f) {
        s("{");
        if (auto* rt = as<Rtag>(f->prf_desc)) {
          s("Rtag "); str_loc(rt->label); s(" "); bool_(rt->constant); s(" "); core_types(rt->types);
        } else {
          s("Rinherit "); core_type(as<Rinherit>(f->prf_desc)->ty);
        }
        s(" "); loc(f->prf_loc); s(" "); attrs(f->prf_attributes); s("}");
      });
      s(" "); flag_closed(v->closed); s(" ");
      if (!v->has_labels) s("None");
      else { s("(Some "); list(v->labels, [](std::string_view l) { q(l); }); s(")"); }
      break;
    }
    case K::Ptyp_poly: {
      auto* p = as<Ptyp_poly>(d);
      s("Ptyp_poly "); list(p->vars, [](const StrLoc& v) { str_gloc(v); }); s(" "); core_type(p->ty);
      break;
    }
    case K::Ptyp_package: s("Ptyp_package "); package_type(as<Ptyp_package>(d)->pack, false); break;
    case K::Ptyp_open: {
      auto* o = as<Ptyp_open>(d);
      s("Ptyp_open "); lid_loc(o->lid); s(" "); core_type(o->ty);
      break;
    }
    case K::Ptyp_extension: s("Ptyp_extension "); extension(as<Ptyp_extension>(d)->ext); break;
    case K::Ptyp_functor: {
      auto* f = as<Ptyp_functor>(d);
      s("Ptyp_functor "); arg_label(f->label); s(" "); str_loc(f->name); s(" ");
      package_type(f->pack, true); s(" "); core_type(f->ty);
      break;
    }
  }
  s(")");
}

void patterns(const Slice<const Pattern*>& l) { list(l, [](const Pattern* p) { pattern(p); }); }
void pattern(const Pattern* p) {
  s("(P "); loc(p->ppat_loc); s(" "); attrs(p->ppat_attributes); s(" ");
  const PatternDesc* d = p->ppat_desc;
  using K = PatternDesc::Kind;
  switch (d->kind) {
    case K::Ppat_any: s("Ppat_any"); break;
    case K::Ppat_var: s("Ppat_var "); str_loc(as<Ppat_var>(d)->name); break;
    case K::Ppat_alias: {
      auto* a = as<Ppat_alias>(d);
      s("Ppat_alias "); pattern(a->pat); s(" "); str_loc(a->name);
      break;
    }
    case K::Ppat_constant: s("Ppat_constant "); constant(as<Ppat_constant>(d)->c); break;
    case K::Ppat_interval: {
      auto* v = as<Ppat_interval>(d);
      s("Ppat_interval "); constant(v->c1); s(" "); constant(v->c2);
      break;
    }
    case K::Ppat_tuple: {
      auto* t = as<Ppat_tuple>(d);
      s("Ppat_tuple ");
      list(t->pl, [](const LabeledPattern& x) { optstr(x.label); s(":"); pattern(x.pat); });
      s(" "); flag_closed(t->closed);
      break;
    }
    case K::Ppat_construct: {
      auto* c = as<Ppat_construct>(d);
      s("Ppat_construct "); lid_loc(c->lid); s(" ");
      popt(c->arg, [](const ConstructArg* a) {
        list(a->vars, [](const StrLoc& v) { str_loc(v); }); s(" "); pattern(a->pat);
      });
      break;
    }
    case K::Ppat_variant: {
      auto* v = as<Ppat_variant>(d);
      s("Ppat_variant "); q(v->label); s(" "); popt(v->arg, pattern);
      break;
    }
    case K::Ppat_record: {
      auto* r = as<Ppat_record>(d);
      s("Ppat_record ");
      list(r->fields, [](const std::pair<LidLoc, const Pattern*>& f) {
        lid_loc(f.first); s("="); pattern(f.second);
      });
      s(" "); flag_closed(r->closed);
      break;
    }
    case K::Ppat_array: s("Ppat_array "); patterns(as<Ppat_array>(d)->pats); break;
    case K::Ppat_or: {
      auto* o = as<Ppat_or>(d);
      s("Ppat_or "); pattern(o->p1); s(" "); pattern(o->p2);
      break;
    }
    case K::Ppat_constraint: {
      auto* c = as<Ppat_constraint>(d);
      s("Ppat_constraint "); pattern(c->pat); s(" "); core_type(c->ty);
      break;
    }
    case K::Ppat_type: s("Ppat_type "); lid_loc(as<Ppat_type>(d)->lid); break;
    case K::Ppat_lazy: s("Ppat_lazy "); pattern(as<Ppat_lazy>(d)->pat); break;
    case K::Ppat_unpack: {
      auto* u = as<Ppat_unpack>(d);
      s("Ppat_unpack "); optstr(u->name.txt); s(" "); loc(u->name.loc); s(" ");
      popt(u->pack, [](const PackageType* p) { package_type(p, true); });
      break;
    }
    case K::Ppat_exception: s("Ppat_exception "); pattern(as<Ppat_exception>(d)->pat); break;
    case K::Ppat_effect: {
      auto* e = as<Ppat_effect>(d);
      s("Ppat_effect "); pattern(e->eff); s(" "); pattern(e->cont);
      break;
    }
    case K::Ppat_extension: s("Ppat_extension "); extension(as<Ppat_extension>(d)->ext); break;
    case K::Ppat_open: {
      auto* o = as<Ppat_open>(d);
      s("Ppat_open "); lid_loc(o->lid); s(" "); pattern(o->pat);
      break;
    }
  }
  s(")");
}

void case_(const Case* c) {
  s("{case "); pattern(c->pc_lhs); s(" "); popt(c->pc_guard, expression); s(" ");
  expression(c->pc_rhs); s("}");
}
void cases(const Slice<const Case*>& l) { list(l, [](const Case* c) { case_(c); }); }
void binding_op(const BindingOp* b) {
  s("{bop "); str_loc(b->pbop_op); s(" "); pattern(b->pbop_pat); s(" "); expression(b->pbop_exp);
  s(" "); loc(b->pbop_loc); s("}");
}
void args(const Slice<ArgExpression>& l) {
  list(l, [](const ArgExpression& a) { arg_label(a.label); s(":"); expression(a.exp); });
}
void exprs(const Slice<const Expression*>& l) { list(l, [](const Expression* e) { expression(e); }); }
void vbs(const Slice<const ValueBinding*>& l) { list(l, [](const ValueBinding* v) { value_binding(v); }); }

void expression(const Expression* e) {
  s("(E "); loc(e->pexp_loc); s(" "); attrs(e->pexp_attributes); s(" ");
  const ExpressionDesc* d = e->pexp_desc;
  using K = ExpressionDesc::Kind;
  switch (d->kind) {
    case K::Pexp_ident: s("Pexp_ident "); lid_loc(as<Pexp_ident>(d)->lid); break;
    case K::Pexp_constant: s("Pexp_constant "); constant(as<Pexp_constant>(d)->c); break;
    case K::Pexp_let: {
      auto* l = as<Pexp_let>(d);
      s("Pexp_let "); flag_rec(l->rec); s(" "); vbs(l->vbs); s(" "); expression(l->body);
      break;
    }
    case K::Pexp_function: {
      auto* f = as<Pexp_function>(d);
      s("Pexp_function ");
      list(f->params, [](const FunctionParam* p) {
        s("{"); loc(p->pparam_loc); s(" ");
        if (p->pparam_desc.kind == FunctionParamDesc::Kind::Pparam_val) {
          s("Pparam_val "); arg_label(p->pparam_desc.label); s(" ");
          popt(p->pparam_desc.default_, expression); s(" "); pattern(p->pparam_desc.pat);
        } else {
          s("Pparam_newtype "); str_loc(p->pparam_desc.newtype);
        }
        s("}");
      });
      s(" ");
      popt(f->constraint, [](const TypeConstraint* tc) {
        if (tc->kind == TypeConstraint::Kind::Pconstraint) { s("Pconstraint "); core_type(tc->ty); }
        else { s("Pcoerce "); popt(tc->from, core_type); s(" "); core_type(tc->ty); }
      });
      s(" ");
      if (f->body->kind == FunctionBody::Kind::Pfunction_body) {
        s("Pfunction_body "); expression(f->body->body);
      } else {
        s("Pfunction_cases "); cases(f->body->cases); s(" "); loc(f->body->loc); s(" ");
        attrs(f->body->attrs);
      }
      break;
    }
    case K::Pexp_apply: {
      auto* a = as<Pexp_apply>(d);
      s("Pexp_apply "); expression(a->fn); s(" "); args(a->args);
      break;
    }
    case K::Pexp_match: {
      auto* m = as<Pexp_match>(d);
      s("Pexp_match "); expression(m->exp); s(" "); cases(m->cases);
      break;
    }
    case K::Pexp_try: {
      auto* m = as<Pexp_try>(d);
      s("Pexp_try "); expression(m->exp); s(" "); cases(m->cases);
      break;
    }
    case K::Pexp_tuple:
      s("Pexp_tuple ");
      list(as<Pexp_tuple>(d)->el, [](const LabeledExpression& x) { optstr(x.label); s(":"); expression(x.exp); });
      break;
    case K::Pexp_construct: {
      auto* c = as<Pexp_construct>(d);
      s("Pexp_construct "); lid_loc(c->lid); s(" "); popt(c->arg, expression);
      break;
    }
    case K::Pexp_variant: {
      auto* v = as<Pexp_variant>(d);
      s("Pexp_variant "); q(v->label); s(" "); popt(v->arg, expression);
      break;
    }
    case K::Pexp_record: {
      auto* r = as<Pexp_record>(d);
      s("Pexp_record ");
      list(r->fields, [](const std::pair<LidLoc, const Expression*>& f) {
        lid_loc(f.first); s("="); expression(f.second);
      });
      s(" "); popt(r->base, expression);
      break;
    }
    case K::Pexp_field: {
      auto* f = as<Pexp_field>(d);
      s("Pexp_field "); expression(f->exp); s(" "); lid_loc(f->lid);
      break;
    }
    case K::Pexp_setfield: {
      auto* f = as<Pexp_setfield>(d);
      s("Pexp_setfield "); expression(f->exp); s(" "); lid_loc(f->lid); s(" "); expression(f->value);
      break;
    }
    case K::Pexp_array: s("Pexp_array "); exprs(as<Pexp_array>(d)->el); break;
    case K::Pexp_ifthenelse: {
      auto* x = as<Pexp_ifthenelse>(d);
      s("Pexp_ifthenelse "); expression(x->cond); s(" "); expression(x->then_); s(" ");
      popt(x->else_, expression);
      break;
    }
    case K::Pexp_sequence: {
      auto* x = as<Pexp_sequence>(d);
      s("Pexp_sequence "); expression(x->e1); s(" "); expression(x->e2);
      break;
    }
    case K::Pexp_while: {
      auto* x = as<Pexp_while>(d);
      s("Pexp_while "); expression(x->cond); s(" "); expression(x->body);
      break;
    }
    case K::Pexp_for: {
      auto* x = as<Pexp_for>(d);
      s("Pexp_for "); pattern(x->pat); s(" "); expression(x->lo); s(" "); expression(x->hi); s(" ");
      s(x->dir == DirectionFlag::Upto ? "Upto" : "Downto"); s(" "); expression(x->body);
      break;
    }
    case K::Pexp_constraint: {
      auto* x = as<Pexp_constraint>(d);
      s("Pexp_constraint "); expression(x->exp); s(" "); core_type(x->ty);
      break;
    }
    case K::Pexp_coerce: {
      auto* x = as<Pexp_coerce>(d);
      s("Pexp_coerce "); expression(x->exp); s(" "); popt(x->from, core_type); s(" "); core_type(x->to);
      break;
    }
    case K::Pexp_send: {
      auto* x = as<Pexp_send>(d);
      s("Pexp_send "); expression(x->exp); s(" "); str_loc(x->meth);
      break;
    }
    case K::Pexp_new: s("Pexp_new "); lid_loc(as<Pexp_new>(d)->lid); break;
    case K::Pexp_setinstvar: {
      auto* x = as<Pexp_setinstvar>(d);
      s("Pexp_setinstvar "); str_loc(x->name); s(" "); expression(x->value);
      break;
    }
    case K::Pexp_override:
      s("Pexp_override ");
      list(as<Pexp_override>(d)->fields, [](const std::pair<StrLoc, const Expression*>& f) {
        str_loc(f.first); s("="); expression(f.second);
      });
      break;
    case K::Pexp_struct_item: {
      auto* x = as<Pexp_struct_item>(d);
      s("Pexp_struct_item "); structure_item(x->item); s(" "); expression(x->body);
      break;
    }
    case K::Pexp_assert: s("Pexp_assert "); expression(as<Pexp_assert>(d)->exp); break;
    case K::Pexp_lazy: s("Pexp_lazy "); expression(as<Pexp_lazy>(d)->exp); break;
    case K::Pexp_poly: {
      auto* x = as<Pexp_poly>(d);
      s("Pexp_poly "); expression(x->exp); s(" "); popt(x->ty, core_type);
      break;
    }
    case K::Pexp_object: s("Pexp_object "); class_structure(as<Pexp_object>(d)->cs); break;
    case K::Pexp_newtype: {
      auto* x = as<Pexp_newtype>(d);
      s("Pexp_newtype "); str_loc(x->name); s(" "); expression(x->body);
      break;
    }
    case K::Pexp_pack: {
      auto* x = as<Pexp_pack>(d);
      s("Pexp_pack "); module_expr(x->me); s(" ");
      popt(x->pack, [](const PackageType* p) { package_type(p, true); });
      break;
    }
    case K::Pexp_letop: {
      auto* x = as<Pexp_letop>(d)->letop;
      s("Pexp_letop "); binding_op(x->let_); s(" ");
      list(x->ands, [](const BindingOp* b) { binding_op(b); }); s(" "); expression(x->body);
      break;
    }
    case K::Pexp_extension: s("Pexp_extension "); extension(as<Pexp_extension>(d)->ext); break;
    case K::Pexp_unreachable: s("Pexp_unreachable"); break;
    case K::Pexp_hole: s("Pexp_hole"); break;
  }
  if (d->kind == K::Pexp_assert) {
    s(" innermost ");
    loc(e->pexp_loc_stack.empty() ? e->pexp_loc : e->pexp_loc_stack.back());
  }
  s(")");
}

void value_constraint(const ValueConstraint* vc) {
  if (vc->kind == ValueConstraint::Kind::Pvc_constraint) {
    s("Pvc_constraint "); list(vc->locally_abstract_univars, [](const StrLoc& v) { str_loc(v); });
    s(" "); core_type(vc->typ);
  } else {
    s("Pvc_coercion "); popt(vc->ground, core_type); s(" "); core_type(vc->coercion);
  }
}
void value_binding(const ValueBinding* vb) {
  s("{vb "); pattern(vb->pvb_pat); s(" "); expression(vb->pvb_expr); s(" ");
  popt(vb->pvb_constraint, value_constraint); s(" "); attrs(vb->pvb_attributes); s(" ");
  loc(vb->pvb_loc); s("}");
}
void value_description(const ValueDescription* v) {
  s("{val "); str_loc(v->pval_name); s(" "); core_type(v->pval_type); s(" ");
  attrs(v->pval_attributes); s(" "); loc(v->pval_loc); s("}");
}
void primitive(const PrimitiveDescription* p) {
  s("{prim "); str_loc(p->pprim_name); s(" ");
  if (p->pprim_kind.kind == PrimitiveKind::Kind::Pprim_decl) {
    s("Pprim_decl "); core_type(p->pprim_kind.ty); s(" ");
    list(p->pprim_kind.prims, [](std::string_view x) { q(x); });
  } else {
    s("Pprim_alias "); popt(p->pprim_kind.ty, core_type); s(" "); lid_loc(p->pprim_kind.alias);
  }
  s(" "); attrs(p->pprim_attributes); s(" "); loc(p->pprim_loc); s("}");
}
void type_param(const TypeParam& tp, bool gap) {
  core_type(tp.ty); s(" ");
  if (gap && mask_gaps) { s("?"); return; }
  switch (tp.variance) {
    case Variance::Covariant: s("+"); break;
    case Variance::Contravariant: s("-"); break;
    case Variance::NoVariance: s("."); break;
    case Variance::Bivariant: s("+-"); break;
  }
  if (tp.injectivity == Injectivity::Injective) s("!");
}
void type_params(const Slice<TypeParam>& l, bool gap) {
  list(l, [gap](const TypeParam& tp) { type_param(tp, gap); });
}
void label_decl(const LabelDeclaration* l) {
  s("{ld "); str_loc(l->pld_name); s(" "); flag_mut(l->pld_mutable); s(" "); core_type(l->pld_type);
  s(" "); loc(l->pld_loc); s(" "); attrs(l->pld_attributes); s("}");
}
void ctor_args(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Pcstr_tuple) { s("Pcstr_tuple "); core_types(a.tuple); }
  else { s("Pcstr_record "); list(a.record, [](const LabelDeclaration* l) { label_decl(l); }); }
}
void type_declaration(const TypeDeclaration* d) {
  s("{td "); str_loc(d->ptype_name); s(" "); type_params(d->ptype_params, false); s(" ");
  list(d->ptype_constraints, [](const TypeConstraintDecl& c) {
    core_type(c.t1); s("="); core_type(c.t2); s(" "); loc(c.loc);
  });
  s(" ");
  switch (d->ptype_kind.kind) {
    case TypeKind::Kind::Ptype_abstract: s("Ptype_abstract"); break;
    case TypeKind::Kind::Ptype_variant:
      s("Ptype_variant ");
      list(d->ptype_kind.constructors, [](const ConstructorDeclaration* c) {
        s("{cd "); str_loc(c->pcd_name); s(" "); list(c->pcd_vars, [](const StrLoc& v) { str_gloc(v); });
        s(" "); ctor_args(c->pcd_args); s(" "); popt(c->pcd_res, core_type); s(" "); loc(c->pcd_loc);
        s(" "); attrs(c->pcd_attributes); s("}");
      });
      break;
    case TypeKind::Kind::Ptype_record:
      s("Ptype_record "); list(d->ptype_kind.labels, [](const LabelDeclaration* l) { label_decl(l); });
      break;
    case TypeKind::Kind::Ptype_open: s("Ptype_open"); break;
    case TypeKind::Kind::Ptype_external: s("Ptype_external "); q(d->ptype_kind.external); break;
  }
  s(" "); flag_priv(d->ptype_private); s(" "); popt(d->ptype_manifest, core_type); s(" ");
  attrs(d->ptype_attributes); s(" "); loc(d->ptype_loc); s("}");
}
void type_declarations(const Slice<const TypeDeclaration*>& l) {
  list(l, [](const TypeDeclaration* d) { type_declaration(d); });
}
void extension_constructor(const ExtensionConstructor* c) {
  s("{ext "); str_loc(c->pext_name); s(" ");
  if (c->pext_kind.kind == ExtensionConstructorKind::Kind::Pext_decl) {
    s("Pext_decl "); list(c->pext_kind.vars, [](const StrLoc& v) { str_gloc(v); }); s(" ");
    ctor_args(c->pext_kind.args); s(" "); popt(c->pext_kind.res, core_type);
  } else {
    s("Pext_rebind "); lid_loc(c->pext_kind.rebind);
  }
  s(" "); loc(c->pext_loc); s(" "); attrs(c->pext_attributes); s("}");
}
void type_extension(const TypeExtension* x) {
  s("{tyext "); lid_loc(x->ptyext_path); s(" "); type_params(x->ptyext_params, true); s(" ");
  list(x->ptyext_constructors, [](const ExtensionConstructor* c) { extension_constructor(c); });
  s(" "); flag_priv(x->ptyext_private); s(" "); loc(x->ptyext_loc); s(" "); attrs(x->ptyext_attributes);
  s("}");
}
void type_exception(const TypeException* x) {
  s("{tyexn "); extension_constructor(x->ptyexn_constructor); s(" "); loc(x->ptyexn_loc); s(" ");
  attrs(x->ptyexn_attributes); s("}");
}
void open_description(const OpenDescription* o) {
  s("{open "); lid_loc(o->popen_expr); s(" "); flag_ovr(o->popen_override); s(" "); loc(o->popen_loc);
  s(" "); attrs(o->popen_attributes); s("}");
}
void class_signature(const ClassSignature* cs) {
  s("{csig "); core_type(cs->pcsig_self); s(" ");
  list(cs->pcsig_fields, [](const ClassTypeField* f) {
    s("(CTF "); loc(f->pctf_loc); s(" "); attrs(f->pctf_attributes); s(" ");
    const ClassTypeFieldDesc* d = f->pctf_desc;
    using K = ClassTypeFieldDesc::Kind;
    switch (d->kind) {
      case K::Pctf_inherit: s("Pctf_inherit "); class_type(as<Pctf_inherit>(d)->cty); break;
      case K::Pctf_val: {
        auto* v = as<Pctf_val>(d);
        s("Pctf_val "); str_loc(v->label); s(" "); flag_mut(v->mut); s(" "); flag_virt(v->virt); s(" ");
        core_type(v->ty);
        break;
      }
      case K::Pctf_method: {
        auto* v = as<Pctf_method>(d);
        s("Pctf_method "); str_loc(v->label); s(" "); flag_priv(v->priv); s(" "); flag_virt(v->virt);
        s(" "); core_type(v->ty);
        break;
      }
      case K::Pctf_constraint: {
        auto* v = as<Pctf_constraint>(d);
        s("Pctf_constraint "); core_type(v->t1); s(" "); core_type(v->t2);
        break;
      }
      case K::Pctf_attribute: s("Pctf_attribute "); attribute(as<Pctf_attribute>(d)->attr, true); break;
      case K::Pctf_extension: s("Pctf_extension "); extension(as<Pctf_extension>(d)->ext); break;
    }
    s(")");
  });
  s("}");
}
void class_type(const ClassType* x) {
  s("(CT "); loc(x->pcty_loc); s(" "); attrs(x->pcty_attributes); s(" ");
  const ClassTypeDesc* d = x->pcty_desc;
  using K = ClassTypeDesc::Kind;
  switch (d->kind) {
    case K::Pcty_constr: {
      auto* c = as<Pcty_constr>(d);
      s("Pcty_constr "); lid_loc(c->lid); s(" "); core_types(c->args);
      break;
    }
    case K::Pcty_signature: s("Pcty_signature "); class_signature(as<Pcty_signature>(d)->sign); break;
    case K::Pcty_arrow: {
      auto* a = as<Pcty_arrow>(d);
      s("Pcty_arrow "); arg_label(a->label); s(" "); core_type(a->ty); s(" "); class_type(a->cty);
      break;
    }
    case K::Pcty_extension: s("Pcty_extension "); extension(as<Pcty_extension>(d)->ext); break;
    case K::Pcty_open: {
      auto* o = as<Pcty_open>(d);
      s("Pcty_open "); open_description(o->od); s(" "); class_type(o->cty);
      break;
    }
  }
  s(")");
}
template <class A, class F>
void class_infos(const ClassInfos<A>* x, F&& f) {
  s("{ci "); flag_virt(x->pci_virt); s(" "); type_params(x->pci_params, true); s(" ");
  str_loc(x->pci_name); s(" "); f(x->pci_expr); s(" "); loc(x->pci_loc); s(" ");
  attrs(x->pci_attributes); s("}");
}
void class_field_kind(const ClassFieldKind& k) {
  if (k.kind == ClassFieldKind::Kind::Cfk_virtual) { s("Cfk_virtual "); core_type(k.ty); }
  else { s("Cfk_concrete "); flag_ovr(k.ovr); s(" "); expression(k.exp); }
}
void class_expr(const ClassExpr* x) {
  s("(CE "); loc(x->pcl_loc); s(" "); attrs(x->pcl_attributes); s(" ");
  const ClassExprDesc* d = x->pcl_desc;
  using K = ClassExprDesc::Kind;
  switch (d->kind) {
    case K::Pcl_constr: {
      auto* c = as<Pcl_constr>(d);
      s("Pcl_constr "); lid_loc(c->lid); s(" "); core_types(c->args);
      break;
    }
    case K::Pcl_structure: s("Pcl_structure "); class_structure(as<Pcl_structure>(d)->cs); break;
    case K::Pcl_fun: {
      auto* f = as<Pcl_fun>(d);
      s("Pcl_fun "); arg_label(f->label); s(" "); popt(f->default_, expression); s(" ");
      pattern(f->pat); s(" "); class_expr(f->body);
      break;
    }
    case K::Pcl_apply: {
      auto* a = as<Pcl_apply>(d);
      s("Pcl_apply "); class_expr(a->ce); s(" "); args(a->args);
      break;
    }
    case K::Pcl_let: {
      auto* l = as<Pcl_let>(d);
      s("Pcl_let "); flag_rec(l->rec); s(" "); vbs(l->vbs); s(" "); class_expr(l->body);
      break;
    }
    case K::Pcl_constraint: {
      auto* c = as<Pcl_constraint>(d);
      s("Pcl_constraint "); class_expr(c->ce); s(" "); class_type(c->cty);
      break;
    }
    case K::Pcl_extension: s("Pcl_extension "); extension(as<Pcl_extension>(d)->ext); break;
    case K::Pcl_open: {
      auto* o = as<Pcl_open>(d);
      s("Pcl_open "); open_description(o->od); s(" "); class_expr(o->ce);
      break;
    }
  }
  s(")");
}
void class_structure(const ClassStructure* cs) {
  s("{cstr "); pattern(cs->pcstr_self); s(" ");
  list(cs->pcstr_fields, [](const ClassField* f) {
    s("(CF "); loc(f->pcf_loc); s(" "); attrs(f->pcf_attributes); s(" ");
    const ClassFieldDesc* d = f->pcf_desc;
    using K = ClassFieldDesc::Kind;
    switch (d->kind) {
      case K::Pcf_inherit: {
        auto* v = as<Pcf_inherit>(d);
        s("Pcf_inherit "); flag_ovr(v->ovr); s(" "); class_expr(v->ce); s(" ");
        popt(v->as, [](const StrLoc* x) { str_loc(*x); });
        break;
      }
      case K::Pcf_val: {
        auto* v = as<Pcf_val>(d);
        s("Pcf_val "); str_loc(v->label); s(" "); flag_mut(v->mut); s(" "); class_field_kind(v->kind_);
        break;
      }
      case K::Pcf_method: {
        auto* v = as<Pcf_method>(d);
        s("Pcf_method "); str_loc(v->label); s(" "); flag_priv(v->priv); s(" "); class_field_kind(v->kind_);
        break;
      }
      case K::Pcf_constraint: {
        auto* v = as<Pcf_constraint>(d);
        s("Pcf_constraint "); core_type(v->t1); s(" "); core_type(v->t2);
        break;
      }
      case K::Pcf_initializer: s("Pcf_initializer "); expression(as<Pcf_initializer>(d)->exp); break;
      case K::Pcf_attribute: s("Pcf_attribute "); attribute(as<Pcf_attribute>(d)->attr, true); break;
      case K::Pcf_extension: s("Pcf_extension "); extension(as<Pcf_extension>(d)->ext); break;
    }
    s(")");
  });
  s("}");
}
void functor_param(const FunctorParameter& p) {
  if (p.is_unit) { s("Unit"); return; }
  s("Named "); optstr(p.name.txt); s(" "); loc(p.name.loc); s(" "); module_type(p.mty);
}
void module_type(const ModuleType* m) {
  s("(MT "); loc(m->pmty_loc); s(" "); attrs(m->pmty_attributes); s(" ");
  const ModuleTypeDesc* d = m->pmty_desc;
  using K = ModuleTypeDesc::Kind;
  switch (d->kind) {
    case K::Pmty_ident: s("Pmty_ident "); lid_loc(as<Pmty_ident>(d)->lid); break;
    case K::Pmty_signature: s("Pmty_signature "); signature(as<Pmty_signature>(d)->sg); break;
    case K::Pmty_functor: {
      auto* f = as<Pmty_functor>(d);
      s("Pmty_functor "); functor_param(f->param); s(" "); module_type(f->body);
      break;
    }
    case K::Pmty_with: {
      auto* w = as<Pmty_with>(d);
      s("Pmty_with "); module_type(w->mty); s(" ");
      list(w->cstrs, [](const WithConstraint* c) {
        using WK = WithConstraint::Kind;
        switch (c->kind) {
          case WK::Pwith_type: s("Pwith_type "); lid_loc(c->lid); s(" "); type_declaration(c->decl); break;
          case WK::Pwith_module: s("Pwith_module "); lid_loc(c->lid); s(" "); lid_loc(c->lid2); break;
          case WK::Pwith_modtype: s("Pwith_modtype "); lid_loc(c->lid); s(" "); module_type(c->mty); break;
          case WK::Pwith_modtypesubst:
            s("Pwith_modtypesubst "); lid_loc(c->lid); s(" "); module_type(c->mty); break;
          case WK::Pwith_typesubst:
            s("Pwith_typesubst "); lid_loc(c->lid); s(" "); type_declaration(c->decl); break;
          case WK::Pwith_modsubst: s("Pwith_modsubst "); lid_loc(c->lid); s(" "); lid_loc(c->lid2); break;
        }
      });
      break;
    }
    case K::Pmty_typeof: s("Pmty_typeof "); module_expr(as<Pmty_typeof>(d)->me); break;
    case K::Pmty_extension: s("Pmty_extension "); extension(as<Pmty_extension>(d)->ext); break;
    case K::Pmty_alias: s("Pmty_alias "); lid_loc(as<Pmty_alias>(d)->lid); break;
  }
  s(")");
}
void module_declaration(const ModuleDeclaration* md) {
  s("{md "); optstr(md->pmd_name.txt); s(" "); loc(md->pmd_name.loc); s(" "); module_type(md->pmd_type);
  s(" "); attrs(md->pmd_attributes); s(" "); loc(md->pmd_loc); s("}");
}
void modtype_declaration(const ModuleTypeDeclaration* m) {
  s("{mtd "); str_loc(m->pmtd_name); s(" "); popt(m->pmtd_type, module_type); s(" ");
  attrs(m->pmtd_attributes); s(" "); loc(m->pmtd_loc); s("}");
}
void signature_item(const SignatureItem* it) {
  s("(SI "); loc(it->psig_loc); s(" ");
  const SignatureItemDesc* d = it->psig_desc;
  using K = SignatureItemDesc::Kind;
  switch (d->kind) {
    case K::Psig_value: s("Psig_value "); value_description(as<Psig_value>(d)->vd); break;
    case K::Psig_primitive: s("Psig_primitive "); primitive(as<Psig_primitive>(d)->pd); break;
    case K::Psig_type: {
      auto* t = as<Psig_type>(d);
      s("Psig_type "); flag_rec(t->rec); s(" "); type_declarations(t->decls);
      break;
    }
    case K::Psig_typesubst: s("Psig_typesubst "); type_declarations(as<Psig_typesubst>(d)->decls); break;
    case K::Psig_typext: s("Psig_typext "); type_extension(as<Psig_typext>(d)->ext); break;
    case K::Psig_exception: s("Psig_exception "); type_exception(as<Psig_exception>(d)->exn); break;
    case K::Psig_module: s("Psig_module "); module_declaration(as<Psig_module>(d)->md); break;
    case K::Psig_modsubst: {
      auto* ms = as<Psig_modsubst>(d)->ms;
      s("Psig_modsubst "); str_loc(ms->pms_name); s(" "); lid_loc(ms->pms_manifest); s(" ");
      attrs(ms->pms_attributes); s(" "); loc(ms->pms_loc);
      break;
    }
    case K::Psig_recmodule:
      s("Psig_recmodule ");
      list(as<Psig_recmodule>(d)->mds, [](const ModuleDeclaration* md) { module_declaration(md); });
      break;
    case K::Psig_modtype: s("Psig_modtype "); modtype_declaration(as<Psig_modtype>(d)->mtd); break;
    case K::Psig_modtypesubst:
      s("Psig_modtypesubst "); modtype_declaration(as<Psig_modtypesubst>(d)->mtd); break;
    case K::Psig_open: s("Psig_open "); open_description(as<Psig_open>(d)->od); break;
    case K::Psig_include: {
      auto* x = as<Psig_include>(d)->incl;
      s("Psig_include "); module_type(x->pincl_mod); s(" "); loc(x->pincl_loc); s(" ");
      attrs(x->pincl_attributes);
      break;
    }
    case K::Psig_class:
      s("Psig_class ");
      list(as<Psig_class>(d)->decls, [](const ClassDescription* x) { class_infos(x, class_type); });
      break;
    case K::Psig_class_type:
      s("Psig_class_type ");
      list(as<Psig_class_type>(d)->decls, [](const ClassTypeDeclaration* x) { class_infos(x, class_type); });
      break;
    case K::Psig_attribute: s("Psig_attribute "); attribute(as<Psig_attribute>(d)->attr, true); break;
    case K::Psig_extension: {
      auto* x = as<Psig_extension>(d);
      s("Psig_extension "); extension(x->ext); s(" "); attrs(x->attrs);
      break;
    }
  }
  s(")");
}
void signature(const Signature& sg) { list(sg, [](const SignatureItem* it) { signature_item(it); }); }
void module_expr(const ModuleExpr* m) {
  s("(ME "); loc(m->pmod_loc); s(" "); attrs(m->pmod_attributes); s(" ");
  const ModuleExprDesc* d = m->pmod_desc;
  using K = ModuleExprDesc::Kind;
  switch (d->kind) {
    case K::Pmod_ident: s("Pmod_ident "); lid_loc(as<Pmod_ident>(d)->lid); break;
    case K::Pmod_structure: s("Pmod_structure "); structure(as<Pmod_structure>(d)->str); break;
    case K::Pmod_functor: {
      auto* f = as<Pmod_functor>(d);
      s("Pmod_functor "); functor_param(f->param); s(" "); module_expr(f->body);
      break;
    }
    case K::Pmod_apply: {
      auto* a = as<Pmod_apply>(d);
      s("Pmod_apply "); module_expr(a->fn); s(" "); module_expr(a->arg);
      break;
    }
    case K::Pmod_apply_unit: s("Pmod_apply_unit "); module_expr(as<Pmod_apply_unit>(d)->fn); break;
    case K::Pmod_constraint: {
      auto* c = as<Pmod_constraint>(d);
      s("Pmod_constraint "); module_expr(c->me); s(" "); module_type(c->mty);
      break;
    }
    case K::Pmod_unpack: s("Pmod_unpack "); expression(as<Pmod_unpack>(d)->exp); break;
    case K::Pmod_extension: s("Pmod_extension "); extension(as<Pmod_extension>(d)->ext); break;
    case K::Pmod_hole: s("Pmod_hole"); break;
  }
  s(")");
}
void module_binding(const ModuleBinding* mb) {
  s("{mb "); optstr(mb->pmb_name.txt); s(" "); loc(mb->pmb_name.loc); s(" "); module_expr(mb->pmb_expr);
  s(" "); attrs(mb->pmb_attributes); s(" "); loc(mb->pmb_loc); s("}");
}
void structure_item(const StructureItem* it) {
  s("(SI "); loc(it->pstr_loc); s(" ");
  const StructureItemDesc* d = it->pstr_desc;
  using K = StructureItemDesc::Kind;
  switch (d->kind) {
    case K::Pstr_eval: {
      auto* e = as<Pstr_eval>(d);
      s("Pstr_eval "); expression(e->exp); s(" "); attrs(e->attrs);
      break;
    }
    case K::Pstr_value: {
      auto* v = as<Pstr_value>(d);
      s("Pstr_value "); flag_rec(v->rec); s(" "); vbs(v->vbs);
      break;
    }
    case K::Pstr_val: s("Pstr_val "); value_description(as<Pstr_val>(d)->vd); break;
    case K::Pstr_primitive: s("Pstr_primitive "); primitive(as<Pstr_primitive>(d)->pd); break;
    case K::Pstr_type: {
      auto* t = as<Pstr_type>(d);
      s("Pstr_type "); flag_rec(t->rec); s(" "); type_declarations(t->decls);
      break;
    }
    case K::Pstr_typext: s("Pstr_typext "); type_extension(as<Pstr_typext>(d)->ext); break;
    case K::Pstr_exception: s("Pstr_exception "); type_exception(as<Pstr_exception>(d)->exn); break;
    case K::Pstr_module: s("Pstr_module "); module_binding(as<Pstr_module>(d)->mb); break;
    case K::Pstr_recmodule:
      s("Pstr_recmodule ");
      list(as<Pstr_recmodule>(d)->mbs, [](const ModuleBinding* mb) { module_binding(mb); });
      break;
    case K::Pstr_modtype: s("Pstr_modtype "); modtype_declaration(as<Pstr_modtype>(d)->mtd); break;
    case K::Pstr_open: {
      auto* o = as<Pstr_open>(d)->od;
      s("Pstr_open "); module_expr(o->popen_expr); s(" "); flag_ovr(o->popen_override); s(" ");
      loc(o->popen_loc); s(" "); attrs(o->popen_attributes);
      break;
    }
    case K::Pstr_class:
      s("Pstr_class ");
      list(as<Pstr_class>(d)->decls, [](const ClassDeclaration* x) { class_infos(x, class_expr); });
      break;
    case K::Pstr_class_type:
      s("Pstr_class_type ");
      list(as<Pstr_class_type>(d)->decls, [](const ClassTypeDeclaration* x) { class_infos(x, class_type); });
      break;
    case K::Pstr_include: {
      auto* x = as<Pstr_include>(d)->incl;
      s("Pstr_include "); module_expr(x->pincl_mod); s(" "); loc(x->pincl_loc); s(" ");
      attrs(x->pincl_attributes);
      break;
    }
    case K::Pstr_attribute: s("Pstr_attribute "); attribute(as<Pstr_attribute>(d)->attr, true); break;
    case K::Pstr_extension: {
      auto* x = as<Pstr_extension>(d);
      s("Pstr_extension "); extension(x->ext); s(" "); attrs(x->attrs);
      break;
    }
  }
  s(")\n");
}
void structure(const Structure& st) { list(st, [](const StructureItem* it) { structure_item(it); }); }
}  // namespace pd

int run_parse(const std::string& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();
  std::vector<std::string> dirfiles;
  cppcaml::ast::Structure st;
  try {
    st = cppcaml::parse_structure(src, dirfiles);
  } catch (const cppcaml::ParseError& e) {
    std::cerr << "c++typing-dump: parse error: " << e.what() << '\n';
    return 1;
  }
  pd::parse_file = zstr(file);
  pd::structure(parsetree::of_ast(st, file, dirfiles));
  s("\n");
  std::cout << b;
  return 0;
}

// ---- stage 3: Ctype operations (the typing_dump.ml `ctype` mode) ----
namespace et = errortrace;

const char* elt_name(et::Elt<et::ExpandedType>::Kind k) {
  using K = et::Elt<et::ExpandedType>::Kind;
  switch (k) {
    case K::Diff: return "Diff";
    case K::Variant: return "Variant";
    case K::Obj: return "Obj";
    case K::Escape: return "Escape";
    case K::Function_label_mismatch: return "Function_label_mismatch";
    case K::Tuple_label_mismatch: return "Tuple_label_mismatch";
    case K::Incompatible_fields: return "Incompatible_fields";
    case K::First_class_module: return "First_class_module";
    case K::Univar: return "Univar";
    case K::Rec_occur: return "Rec_occur";
  }
  return "?";
}

void expanded(const et::ExpandedType& e) { s("<"); ty(e.ty); s(" "); ty(e.expanded); s(">"); }

void err_trace(const et::ErrorTrace& tr) {
  list(tr, [](const et::Elt<et::ExpandedType>& e) {
    using K = et::Elt<et::ExpandedType>::Kind;
    s(elt_name(e.kind));
    if (e.kind == K::Diff) {
      s("("); expanded(e.diff.got); s(" "); expanded(e.diff.expected); s(")");
    } else if (e.kind == K::Incompatible_fields) {
      s("("); q(e.field_name); s(" "); ty(e.field_diff.got); s(" "); ty(e.field_diff.expected);
      s(")");
    }
  });
}

std::vector<std::string> split_dirs(const std::string& d) {
  std::vector<std::string> dirs;
  for (std::size_t st = 0;;) {
    std::size_t c = d.find(':', st);
    dirs.push_back(d.substr(st, c == std::string::npos ? std::string::npos : c - st));
    if (c == std::string::npos) break;
    st = c + 1;
  }
  return dirs;
}

int run_ctype(const std::string& stdlib_dir, const std::string& queries) {
  canonical = true;
  load_path::init(split_dirs(stdlib_dir), {});
  env::t e0 = env::initial();
  env::OpenResult r = env::open_pers_signature("Stdlib", e0);
  if (r.kind != env::OpenResult::Kind::Ok) {
    std::cerr << "open Stdlib failed\n";
    return 1;
  }
  env::t e = r.env;
  auto value = [&](const std::string& name) {
    return env::find_value_by_name(lid_of_string(name), e).second->val_type;
  };
  std::ifstream in(queries);
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> w;  // the words of the line
    for (std::size_t st = 0;;) {
      std::size_t c = line.find(' ', st);
      w.push_back(line.substr(st, c == std::string::npos ? std::string::npos : c - st));
      if (c == std::string::npos) break;
      st = c + 1;
    }
    if (line.empty()) continue;
    const std::string& op = w[0];
    std::size_t n = w.size() - 1;
    reset_numbering();
    s(line); s(" => ");
    try {
      if (op == "inst" && n == 1) {
        ty(ctype::instance(value(w[1])));
      } else if (op == "gen" && n == 1) {
        TypeExpr* vt = value(w[1]);
        ty(ctype::with_local_level_generalize([&] { return ctype::instance(vt); }));
      } else if (op == "expand" && n == 1) {
        auto [p, td] = env::find_type_by_name(lid_of_string(w[1]), e);
        std::vector<TypeExpr*> args;
        for (std::size_t k = 0; k < td->type_params.size(); ++k) args.push_back(ctype::newvar());
        TypeExpr* t = ctype::newconstr(p, slice(args));
        TypeExpr* ex = ctype::expand_head(e, t);
        ty(ex); s(" | "); ty(ctype::full_expand(false, e, t));
      } else if (op == "unify" && n == 2) {
        TypeExpr* t1 = ctype::instance(value(w[1]));
        TypeExpr* t2 = ctype::instance(value(w[2]));
        try {
          ctype::unify(e, t1, t2);
          s("OK "); ty(t1);
        } catch (const ctype::Unify& u) {
          s("ERR "); err_trace(u.err.trace);
        }
      } else if (op == "moregen" && n == 2) {
        try {
          ctype::moregeneral(e, value(w[1]), value(w[2]));
          s("OK");
        } catch (const ctype::Moregen& m) {
          s("ERR "); err_trace(m.err.trace);
        }
      } else if (op == "equal" && n == 2) {
        try {
          TypeExpr* a = value(w[1]);
          TypeExpr* bb = value(w[2]);
          ctype::equal(e, true, slice({a}), slice({bb}));
          s("OK");
        } catch (const ctype::Equality& q2) {
          s("ERR "); err_trace(q2.err.trace); s(" ");
          list(q2.err.subst, [](const std::pair<TypeExpr*, TypeExpr*>& pr) {
            ty(pr.first); s("="); ty(pr.second);
          });
        }
      } else if (op == "arrow" && n == 1) {
        auto res = ctype::filter_arrow(e, false, ctype::instance(value(w[1])), ArgLabel::nolabel(),
                                       false);
        if (res.ok) {
          s("OK "); ty(res.value.ty_param); s(" "); ty(res.value.ty_ret);
        } else {
          using FK = ctype::FilterArrowFailure::Kind;
          switch (res.error.kind) {
            case FK::Unification_error: s("ERR "); err_trace(res.error.err.trace); break;
            case FK::Label_mismatch:
              s("Label_mismatch "); arg_label(res.error.got); s(" ");
              arg_label(res.error.expected); s(" "); ty(res.error.expected_type);
              break;
            case FK::Not_a_function: s("Not_a_function"); break;
          }
        }
      } else if (op == "subtype" && n == 2) {
        TypeExpr* t1 = ctype::instance(value(w[1]));
        TypeExpr* t2 = ctype::instance(value(w[2]));
        try {
          ctype::subtype(e, t1, t2)();
          s("OK "); ty(t1); s(" "); ty(t2);
        } catch (const ctype::Subtype& st) {
          s("ERR ");
          list(st.err.trace, [](const et::Diff<et::ExpandedType>& d) {
            expanded(d.got); s(" "); expanded(d.expected);
          });
          s(" "); err_trace(st.err.unification_trace);
        }
      } else if (op == "match" && n == 2) {
        TypeExpr* t1 = ctype::instance(value(w[1]));
        TypeExpr* t2 = ctype::instance(value(w[2]));
        try {
          ctype::matches(true, e, t1, t2);
          s("OK");
        } catch (const ctype::MatchesFailure& m) {
          s("ERR "); err_trace(m.err.trace);
        }
      } else if (op == "labels" && n == 1) {
        auto [labels, is_ret_tvar] = ctype::arrow_labels(e, value(w[1]));
        list(labels, [](const ArgLabel& l) { arg_label(l); });
        s(" "); bool_(is_ret_tvar);
      } else if (op == "nongen" && n == 1) {
        auto r2 = ctype::nongen_vars_in_schema(e, ctype::instance(value(w[1])));
        if (!r2) s("None");
        else { s("Some "); list(r2->elements(), [](TypeExpr* t) { ty(t); }); }
      } else if (op == "enlarge" && n == 1) {
        auto [t, warn] = ctype::enlarge_type(e, ctype::instance(value(w[1])));
        ty(t); s(" "); bool_(warn);
      } else {
        s("BADOP");
      }
    } catch (const env::NotFound&) {
      s("NOTFOUND");
    }
    s("\n");
  }
  std::cout << b;
  return 0;
}

// ---- stage 4b: Typetexp (the typing_dump.ml `typexp` mode) ----
namespace txd {
namespace tt = typedtree;
void ctyp(const tt::CoreType* c);
void ctyps(const Slice<const tt::CoreType*>& l) { list(l, [](const tt::CoreType* c) { ctyp(c); }); }
void ctyp(const tt::CoreType* c) {
  s("(CT "); pd::loc(c->ctyp_loc); s(" ");
  const tt::CoreTypeDesc* d = c->ctyp_desc;
  using K = tt::CoreTypeDesc::Kind;
  using parsetree::as;
  switch (d->kind) {
    case K::Ttyp_any: s("Ttyp_any"); break;
    case K::Ttyp_var: s("Ttyp_var "); q(as<tt::Ttyp_var>(d)->name); break;
    case K::Ttyp_arrow: {
      auto* a = as<tt::Ttyp_arrow>(d);
      s("Ttyp_arrow "); arg_label(a->label); s(" "); ctyp(a->t1); s(" "); ctyp(a->t2);
      break;
    }
    case K::Ttyp_tuple:
      s("Ttyp_tuple ");
      list(as<tt::Ttyp_tuple>(d)->tl, [](const tt::LabeledCoreType& x) { pd::optstr(x.label); s(":"); ctyp(x.ty); });
      break;
    case K::Ttyp_constr: {
      auto* x = as<tt::Ttyp_constr>(d);
      s("Ttyp_constr "); path_(x->path); s(" "); pd::lid_loc(x->lid); s(" "); ctyps(x->args);
      break;
    }
    case K::Ttyp_object: {
      auto* o = as<tt::Ttyp_object>(d);
      s("Ttyp_object ");
      list(o->fields, [](const tt::ObjectField* f) {
        if (f->of_desc.is_tag) { s("OTtag "); pd::str_loc(f->of_desc.label); s(" "); ctyp(f->of_desc.ty); }
        else { s("OTinherit "); ctyp(f->of_desc.ty); }
      });
      s(" "); pd::flag_closed(o->closed);
      break;
    }
    case K::Ttyp_class: {
      auto* x = as<tt::Ttyp_class>(d);
      s("Ttyp_class "); path_(x->path); s(" "); pd::lid_loc(x->lid); s(" "); ctyps(x->args);
      break;
    }
    case K::Ttyp_alias: {
      auto* x = as<tt::Ttyp_alias>(d);
      s("Ttyp_alias "); ctyp(x->ty); s(" "); q(x->name.txt);
      break;
    }
    case K::Ttyp_variant: {
      auto* v = as<tt::Ttyp_variant>(d);
      s("Ttyp_variant ");
      list(v->fields, [](const tt::RowField* f) {
        if (f->rf_desc.is_tag) {
          s("Ttag "); q(f->rf_desc.label.txt); s(" "); bool_(f->rf_desc.constant); s(" ");
          ctyps(f->rf_desc.types);
        } else {
          s("Tinherit "); ctyp(f->rf_desc.inherit);
        }
      });
      s(" "); pd::flag_closed(v->closed); s(" ");
      if (!v->has_labels) s("None");
      else { s("(Some "); list(v->labels, [](std::string_view l) { q(l); }); s(")"); }
      break;
    }
    case K::Ttyp_poly: {
      auto* p = as<tt::Ttyp_poly>(d);
      s("Ttyp_poly "); list(p->vars, [](std::string_view v) { q(v); }); s(" "); ctyp(p->ty);
      break;
    }
    case K::Ttyp_package: {
      auto* p = as<tt::Ttyp_package>(d)->pack;
      s("Ttyp_package "); path_(p->tpt_path); s(" ");
      list(p->tpt_constraints, [](const std::pair<parsetree::LidLoc, const tt::CoreType*>& x) {
        pd::lid_loc(x.first); s("="); ctyp(x.second);
      });
      break;
    }
    case K::Ttyp_open: {
      auto* o = as<tt::Ttyp_open>(d);
      s("Ttyp_open "); path_(o->path); s(" "); pd::lid_loc(o->lid); s(" "); ctyp(o->ty);
      break;
    }
    case K::Ttyp_functor: {
      auto* f = as<tt::Ttyp_functor>(d);
      s("Ttyp_functor "); arg_label(f->label); s(" "); ident_(f->id); s(" "); path_(f->pack->tpt_path);
      s(" "); ctyp(f->ty);
      break;
    }
  }
  s(" : "); ty(c->ctyp_type); s(")");
}
}  // namespace txd

int run_typexp(const std::string& dirs, const std::string& modname, const std::string& file) {
  canonical = true;
  load_path::init(split_dirs(dirs), {});
  env::t e0 = env::initial();
  env::OpenResult r = env::open_pers_signature("Stdlib", e0);
  if (r.kind != env::OpenResult::Kind::Ok) {
    std::cerr << "open Stdlib failed\n";
    return 1;
  }
  env::t e = r.env;
  if (modname != "-") {
    try {
      env::OpenResult r2 = env::open_pers_signature(modname, e);
      if (r2.kind == env::OpenResult::Kind::Ok) e = r2.env;
    } catch (...) {
    }
  }
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();
  cppcaml::ast::Signature asg;
  try {
    asg = cppcaml::parse_signature(src);
  } catch (const cppcaml::ParseError& ex) {
    std::cerr << "c++typing-dump: parse error: " << ex.what() << '\n';
    return 1;
  }
  pd::parse_file = zstr(file);
  parsetree::Signature sg = parsetree::of_ast_signature(asg, file, {});
  for (const parsetree::SignatureItem* it : sg) {
    auto one = [&](std::string_view name, const parsetree::CoreType* sty) {
      reset_numbering();
      s(name); s(" => ");
      try {
        txd::ctyp(typetexp::transl_type_scheme(e, sty));
      } catch (const typetexp::Error& er) {
        s("ERR "); s(error_report::texp_error_name(er.kind)); s(" "); pd::loc(er.loc);
      } catch (const env::Error& er) {
        if (er.kind == env::Error::Kind::Lookup_error) {
          s("ERR Env."); s(error_report::lookup_error_name(er.err.kind)); s(" "); pd::loc(er.loc);
        } else {
          s("ERR Env.other");
        }
      } catch (const std::bad_function_call&) {
        s("UNSUPPORTED");
      }
      s("\n");
    };
    using SK = parsetree::SignatureItemDesc::Kind;
    if (it->psig_desc->kind == SK::Psig_value) {
      auto* vd = parsetree::as<parsetree::Psig_value>(it->psig_desc)->vd;
      one(vd->pval_name.txt, vd->pval_type);
    } else if (it->psig_desc->kind == SK::Psig_primitive) {
      auto* pd2 = parsetree::as<parsetree::Psig_primitive>(it->psig_desc)->pd;
      if (pd2->pprim_kind.ty) one(pd2->pprim_name.txt, pd2->pprim_kind.ty);
    }
  }
  std::cout << b;
  return 0;
}


// ---- stage 4c: Typecore (the typing_dump.ml `core` mode) ----


// the typing error of an exception, as typing_dump.ml's `report`
void report(std::exception_ptr ep) {
  s("ERR ");
  std::optional<error_report::Report> r = error_report::classify(ep);
  if (!r) std::rethrow_exception(ep);
  s(r->name);
  if (r->loc) { s(" "); pd::loc(*r->loc); }
  s("\n");
}

int run_core(const std::string& dirs, const std::string& file) {
  canonical = true;
  load_path::init(split_dirs(dirs), {});
  env::OpenResult r = env::open_pers_signature("Stdlib", env::initial());
  if (r.kind != env::OpenResult::Kind::Ok) {
    std::cerr << "open Stdlib failed\n";
    return 1;
  }
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();
  std::vector<std::string> dirfiles;
  cppcaml::ast::Structure ast;
  try {
    ast = cppcaml::parse_structure(src, dirfiles);
  } catch (const cppcaml::ParseError& ex) {
    std::cerr << "c++typing-dump: parse error: " << ex.what() << '\n';
    return 1;
  }
  pd::parse_file = zstr(file);
  parsetree::Structure st = parsetree::of_ast(ast, file, dirfiles);
  namespace tc = typecore;
  // attempt: run f; on an error or an unported forward, finish the output
  auto attempt = [&](const std::function<void()>& f) {
    try {
      f();
      return true;
    } catch (const std::bad_function_call&) {
      s("UNSUPPORTED\n");
    } catch (...) {
      report(std::current_exception());
    }
    std::cout << b;
    return false;
  };
  typemod::install_forward_refs();
  tc::reset_delayed_checks();
  env::t e = r.env;
  for (const parsetree::StructureItem* it : st) {
    using SK = parsetree::StructureItemDesc::Kind;
    if (auto* v = parsetree::as<parsetree::Pstr_value>(it->pstr_desc)) {
      tc::TypeBindingResult res;
      try {
        res = tc::type_binding(e, v->rec, v->vbs);
      } catch (const std::bad_function_call&) {
        s("UNSUPPORTED\n");
        std::cout << b;
        return 0;
      } catch (...) {
        report(std::current_exception());
        std::cout << b;
        return 0;
      }
      for (Ident::t id : typedtree::let_bound_idents(res.vbs)) {
        reset_numbering();
        s("val "); s(ident::name(id)); s(" : ");
        ty(env::find_value(Path::pident(id), res.env)->val_type); s("\n");
      }
      e = res.env;
    } else if (auto* ev = parsetree::as<parsetree::Pstr_eval>(it->pstr_desc)) {
      const typedtree::Expression* exp;
      try {
        exp = tc::type_expression(e, ev->exp);
      } catch (const std::bad_function_call&) {
        s("UNSUPPORTED\n");
        std::cout << b;
        return 0;
      } catch (...) {
        report(std::current_exception());
        std::cout << b;
        return 0;
      }
      reset_numbering();
      s("eval : "); ty(exp->exp_type); s("\n");
    } else if (auto* tyd = parsetree::as<parsetree::Pstr_type>(it->pstr_desc)) {
      typedecl::TranslTypeDeclResult res;
      if (!attempt([&] { res = typedecl::transl_type_decl(e, tyd->rec, tyd->decls); })) return 0;
      for (auto* d : res.decls) {
        reset_numbering();
        s("type "); s(ident::name(d->typ_id)); s(" : "); type_decl(d->typ_type); s("\n");
      }
      e = res.env;
    } else if (auto* te = parsetree::as<parsetree::Pstr_typext>(it->pstr_desc)) {
      typedecl::TranslTypeExtension res{};
      if (!attempt([&] { res = typedecl::transl_type_extension(true, e, it->pstr_loc, te->ext); })) return 0;
      for (auto* c : res.tyext->tyext_constructors) {
        reset_numbering();
        s("ext "); s(ident::name(c->ext_id)); s(" : "); ext_constr(c->ext_type); s("\n");
      }
      e = res.env;
    } else if (auto* ex = parsetree::as<parsetree::Pstr_exception>(it->pstr_desc)) {
      typedecl::TranslTypeException res{};
      if (!attempt([&] { res = typedecl::transl_type_exception(e, ex->exn); })) return 0;
      reset_numbering();
      s("exn "); s(ident::name(res.tyexn->tyexn_constructor->ext_id)); s(" : ");
      ext_constr(res.tyexn->tyexn_constructor->ext_type); s("\n");
      e = res.env;
    } else if (auto* pr = parsetree::as<parsetree::Pstr_primitive>(it->pstr_desc)) {
      std::pair<const typedtree::TPrimitiveDescription*, env::t> res;
      if (!attempt([&] { res = typedecl::transl_prim_desc(e, it->pstr_loc, pr->pd); })) return 0;
      reset_numbering();
      s("val "); s(ident::name(res.first->prim_id)); s(" : "); ty(res.first->prim_val->val_type); s(" ");
      value_kind(res.first->prim_val->val_kind); s("\n");
      e = res.second;
    } else {
      (void)SK::Pstr_eval;
      s("STOP\n");
      std::cout << b;
      return 0;
    }
  }
  try {
    tc::force_delayed_checks();
    s("END\n");
  } catch (...) {
    report(std::current_exception());
  }
  std::cout << b;
  return 0;
}
int run_struct(const std::string& dirs, const std::string& file) {
  canonical = true;
  load_path::init(split_dirs(dirs), {});
  env::OpenResult r = env::open_pers_signature("Stdlib", env::initial());
  if (r.kind != env::OpenResult::Kind::Ok) {
    std::cerr << "open Stdlib failed\n";
    return 1;
  }
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string src = ss.str();
  std::vector<std::string> dirfiles;
  cppcaml::ast::Structure ast;
  try {
    ast = cppcaml::parse_structure(src, dirfiles);
  } catch (const cppcaml::ParseError& ex) {
    std::cerr << "c++typing-dump: parse error: " << ex.what() << '\n';
    return 1;
  }
  pd::parse_file = zstr(file);
  parsetree::Structure st = parsetree::of_ast(ast, file, dirfiles);
  typemod::install_forward_refs();
  typecore::reset_delayed_checks();
  env::reset_required_globals();
  try {
    typemod::TypeStructureResult res = typemod::type_structure(r.env, st);
    Signature simple_sg = typemod::simplify(res.env, res.names, res.sg);
    typemod::check_nongen_signature(res.env, simple_sg);
    typecore::force_delayed_checks();
    reset_numbering();
    s("sig");
    signature(1, simple_sg);
    s("\nEND\n");
  } catch (const std::bad_function_call&) {
    s("UNSUPPORTED\n");
  } catch (...) {
    report(std::current_exception());
  }
  std::cout << b;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string(argv[1]) == "typexp") {
    try {
      return run_typexp(argv[2], argv[3], argv[4]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      std::cout << b;
      return 1;
    }
  }
  if (argc == 4 && std::string(argv[1]) == "struct") {
    try {
      return run_struct(argv[2], argv[3]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      std::cout << b;
      return 1;
    }
  }
  if (argc == 4 && std::string(argv[1]) == "core") {
    try {
      return run_core(argv[2], argv[3]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      std::cout << b;
      return 1;
    }
  }
  if (argc == 3 && std::string(argv[1]) == "parse") {
    try {
      return run_parse(argv[2]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      return 1;
    }
  }
  if (argc == 4 && std::string(argv[1]) == "ctype") {
    try {
      return run_ctype(argv[2], argv[3]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      std::cout << b;
      return 1;
    }
  }
  // cmic FILE.cmi: the cmi dump with ident stamps and type ids renumbered by
  // first visit (compares .cmi files written by different processes)
  if (argc == 3 && std::string(argv[1]) == "cmic") {
    canonical = true;
    argv[1] = argv[2];
    argc = 2;
  }
  if (argc == 4 && std::string(argv[1]) == "env") {
    try {
      return run_env(argv[2], argv[3]);
    } catch (const std::exception& e) {
      std::cerr << "c++typing-dump: " << e.what() << '\n';
      std::cout << b;
      return 1;
    }
  }
  if (argc != 2) {
    std::cerr << "usage: c++typing-dump <file.cmi> | env <stdlib-dir> <queries>\n";
    return 2;
  }
  try {
    auto cmi = cmi_format::read_cmi(argv[1]);
    s("name "); q(cmi.cmi_name); s("\nsig"); signature(1, cmi.cmi_sign);
    s("\ncrcs ");
    list(cmi.cmi_crcs, [](const std::pair<std::string, std::optional<std::string>>& c) {
      q(c.first); s(" ");
      if (!c.second) s("None");
      else { s("(Some "); s(to_hex(*c.second)); s(")"); }
    });
    s("\nflags ");
    list(cmi.cmi_flags, [](const cmi_format::PersFlag& f) {
      switch (f.kind) {
        case cmi_format::PersFlag::Kind::Rectypes: s("Rectypes"); break;
        case cmi_format::PersFlag::Kind::Opaque: s("Opaque"); break;
        case cmi_format::PersFlag::Kind::Alerts:
          s("Alerts ");
          list(f.alerts.bindings(), [](const std::pair<std::string_view, std::string_view>& e) {
            q(e.first); s("="); q(e.second);
          });
          break;
      }
    });
    s("\n");
    std::cout << b;
  } catch (const std::exception& e) {
    std::cerr << "c++typing-dump: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
