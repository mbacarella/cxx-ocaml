// c++typing-dump: the C++ half of the stage-1 oracle for the typing/ port
// (TYPECHECKER.md).  Decodes a .cmi with typing::cmi_format::read_cmi and
// prints the same structural dump as cxx/harness/typing_cmidump.ml, statement
// for statement, so first-visit numbering follows the same traversal order.
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>

#include "cppcaml/typing/cmi_format.hpp"

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
    s("Udesc "); q(u->name); s(" "); i(u->stamp);
  } else {
    // the oracle prints the linked cell generically; never occurs in a cmi
    s("Ulink ?");
  }
  s("}");
}

void ident_(Ident::t id) {
  using K = Ident::Kind;
  switch (id->kind) {
    case K::Local: s("Local("); q(id->name_); s(" "); i(id->stamp_); s(")"); break;
    case K::Scoped:
      s("Scoped("); q(id->name_); s(" "); i(id->stamp_); s(" "); i(id->scope_); s(")");
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
  s("#T"); i(n); s("{"); i(t->level); s(" "); i(t->scope); s(" "); i(t->id); s(" ");
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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: c++typing-dump <file.cmi>\n";
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
