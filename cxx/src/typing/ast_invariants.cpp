// Port of parsing/ast_invariants.ml (see ast_invariants.hpp), with
// parsing/ast_iterator.ml's default_iterator traversal order: which ill
// formed node is reported first depends on it.  Locations and strings are
// visited by no-op methods there and are skipped here.
#include "cppcaml/typing/ast_invariants.hpp"

#include "cppcaml/typing/builtin_attributes.hpp"

namespace cppcaml::typing::ast_invariants {

namespace {

using namespace parsetree;
// (the parsetree's, not the Types records' of support.hpp)
using parsetree::Attribute;
using parsetree::Attributes;

[[noreturn]] void err(const Location& loc, const char* s) { throw syntaxerr::IllFormedAst(loc, s); }

void empty_record(const Location& loc) { err(loc, "Records cannot be empty."); }
void invalid_tuple(const Location& loc) { err(loc, "Tuples must have at least 2 components."); }
void empty_open_tuple_pat(const Location& loc) { err(loc, "Open tuple patterns must have at least one component."); }
void short_closed_tuple_pat(const Location& loc) {
  err(loc, "Closed tuple patterns must have at least two components.");
}
void no_args(const Location& loc) { err(loc, "Function application with no argument."); }
void empty_let(const Location& loc) { err(loc, "Let with no bindings."); }
void empty_type(const Location& loc) { err(loc, "Type declarations cannot be empty."); }
void empty_poly_binder(const Location& loc) { err(loc, "Explicit universal type quantification cannot be empty."); }
void complex_id(const Location& loc) { err(loc, "Functor application not allowed here."); }
void module_type_substitution_missing_rhs(const Location& loc) {
  err(loc, "Module type substitution with no right hand side");
}
void function_without_value_parameters(const Location& loc) { err(loc, "Function without any value parameters"); }
void invalid_struct_item(const Location& loc) {
  err(loc, "This kind of structure item is not allowed in this context.");
}
void optional_label_on_functor(const Location& loc) { err(loc, "Optional argument for a module dependent function."); }

bool is_simple(Longident::t l) {
  switch (l->kind) {
    case Longident::Kind::Lident: return true;
    case Longident::Kind::Ldot: return is_simple(l->l1);
    case Longident::Kind::Lapply: return false;
  }
  return false;
}
void simple_longident(const LidLoc& id) {
  if (!is_simple(id.txt)) complex_id(id.loc);
}
void not_optional_label(const Location& loc, const ArgLabel& l) {
  if (l.kind == ArgLabel::Kind::Optional) optional_label_on_functor(loc);
}

// Ast_iterator.default_iterator, with Ast_invariants' overrides
class Iterator {
 public:
  // ---- attributes, extensions, payloads ----
  // Ast_invariants' attribute: the default one with
  // {self with attribute = super.attribute} (no registration within the
  // payload), then register_attr Invariant_check
  void attribute(const Attribute* a) {
    if (in_attr_) {
      super_attribute(a);
      return;
    }
    in_attr_ = true;
    super_attribute(a);
    in_attr_ = false;
    builtin_attributes::register_attr(a->attr_name.txt, a->attr_name.loc);
  }
  void attributes(const Attributes& l) {
    for (const Attribute* a : l) attribute(a);
  }
  void extension(const Extension* e) { payload(e->payload); }
  void payload(const Payload& p) {
    switch (p.kind) {
      case Payload::Kind::PStr: structure(p.str); break;
      case Payload::Kind::PSig: signature(p.sig); break;
      case Payload::Kind::PTyp: typ(p.typ); break;
      case Payload::Kind::PPat:
        pat(p.pat);
        if (p.guard) expr(p.guard);
        break;
    }
  }

  // ---- core types ----
  void typ(const CoreType* ty) {
    super_typ(ty);
    const Location& loc = ty->ptyp_loc;
    using K = CoreTypeDesc::Kind;
    switch (ty->ptyp_desc->kind) {
      case K::Ptyp_tuple:
        if (as<Ptyp_tuple>(ty->ptyp_desc)->tl.size() < 2) invalid_tuple(loc);
        break;
      case K::Ptyp_package:
        for (auto& c : as<Ptyp_package>(ty->ptyp_desc)->pack->ppt_constraints) simple_longident(c.first);
        break;
      case K::Ptyp_functor: {
        auto* f = as<Ptyp_functor>(ty->ptyp_desc);
        not_optional_label(loc, f->label);
        for (auto& c : f->pack->ppt_constraints) simple_longident(c.first);
        break;
      }
      case K::Ptyp_poly:
        if (as<Ptyp_poly>(ty->ptyp_desc)->vars.empty()) empty_poly_binder(loc);
        break;
      default: break;
    }
  }
  void package_type(const PackageType* p) {
    for (auto& c : p->ppt_constraints) typ(c.second);
    attributes(p->ppt_attrs);
  }

  // ---- patterns ----
  void pat(const Pattern* p) {
    const Pattern* sub = p;
    if (auto* c = as<Ppat_construct>(p->ppat_desc);
        c && c->arg && as<Ppat_tuple>(c->arg->pat->ppat_desc) && builtin_attributes::explicit_arity(p->ppat_attributes))
      sub = c->arg->pat;  // allow unary tuple, see GPR#523
    super_pat(sub);
    const Location& loc = p->ppat_loc;
    using K = PatternDesc::Kind;
    switch (p->ppat_desc->kind) {
      case K::Ppat_tuple: {
        auto* t = as<Ppat_tuple>(p->ppat_desc);
        if (t->closed == ClosedFlag::Closed && t->pl.size() < 2) short_closed_tuple_pat(loc);
        if (t->closed == ClosedFlag::Open && t->pl.empty()) empty_open_tuple_pat(loc);
        break;
      }
      case K::Ppat_record: {
        auto* r = as<Ppat_record>(p->ppat_desc);
        if (r->fields.empty()) empty_record(loc);
        for (auto& f : r->fields) simple_longident(f.first);
        break;
      }
      case K::Ppat_construct: simple_longident(as<Ppat_construct>(p->ppat_desc)->lid); break;
      default: break;
    }
  }

  // ---- expressions ----
  void expr(const Expression* e) {
    const Expression* sub = e;
    if (auto* c = as<Pexp_construct>(e->pexp_desc);
        c && c->arg && as<Pexp_tuple>(c->arg->pexp_desc) && builtin_attributes::explicit_arity(e->pexp_attributes))
      sub = c->arg;  // allow unary tuple, see GPR#523
    super_expr(sub);
    const Location& loc = e->pexp_loc;
    using K = ExpressionDesc::Kind;
    const ExpressionDesc* d = e->pexp_desc;
    switch (d->kind) {
      case K::Pexp_tuple:
        if (as<Pexp_tuple>(d)->el.size() < 2) invalid_tuple(loc);
        break;
      case K::Pexp_record: {
        auto* r = as<Pexp_record>(d);
        if (r->fields.empty()) empty_record(loc);
        for (auto& f : r->fields) simple_longident(f.first);
        break;
      }
      case K::Pexp_apply:
        if (as<Pexp_apply>(d)->args.empty()) no_args(loc);
        break;
      case K::Pexp_let:
        if (as<Pexp_let>(d)->vbs.empty()) empty_let(loc);
        break;
      case K::Pexp_ident: simple_longident(as<Pexp_ident>(d)->lid); break;
      case K::Pexp_construct: simple_longident(as<Pexp_construct>(d)->lid); break;
      case K::Pexp_field: simple_longident(as<Pexp_field>(d)->lid); break;
      case K::Pexp_setfield: simple_longident(as<Pexp_setfield>(d)->lid); break;
      case K::Pexp_new: simple_longident(as<Pexp_new>(d)->lid); break;
      case K::Pexp_function: {
        auto* f = as<Pexp_function>(d);
        if (f->body->kind == FunctionBody::Kind::Pfunction_body) {
          bool all_newtype = true;
          for (const FunctionParam* p : f->params)
            if (p->pparam_desc.kind == FunctionParamDesc::Kind::Pparam_val) all_newtype = false;
          if (all_newtype) function_without_value_parameters(loc);
        }
        break;
      }
      case K::Pexp_struct_item: {
        const StructureItem* si = as<Pexp_struct_item>(d)->item;
        using SK = StructureItemDesc::Kind;
        SK k = si->pstr_desc->kind;
        if (k == SK::Pstr_eval || k == SK::Pstr_value || k == SK::Pstr_include) invalid_struct_item(si->pstr_loc);
        break;
      }
      default: break;
    }
  }
  void cases(Slice<const Case*> l) {
    for (const Case* c : l) {
      pat(c->pc_lhs);
      if (c->pc_guard) expr(c->pc_guard);
      expr(c->pc_rhs);
    }
  }
  void value_binding(const ValueBinding* vb) {
    pat(vb->pvb_pat);
    expr(vb->pvb_expr);
    if (const ValueConstraint* c = vb->pvb_constraint) {
      if (c->kind == ValueConstraint::Kind::Pvc_constraint) {
        typ(c->typ);
      } else {
        if (c->ground) typ(c->ground);
        typ(c->coercion);
      }
    }
    attributes(vb->pvb_attributes);
  }
  void binding_op(const BindingOp* b) {
    pat(b->pbop_pat);
    expr(b->pbop_exp);
  }

  // ---- declarations ----
  void type_declaration(const TypeDeclaration* td) {
    for (const TypeParam& p : td->ptype_params) typ(p.ty);
    for (const TypeConstraintDecl& c : td->ptype_constraints) {
      typ(c.t1);
      typ(c.t2);
    }
    switch (td->ptype_kind.kind) {
      case TypeKind::Kind::Ptype_variant:
        for (const ConstructorDeclaration* c : td->ptype_kind.constructors) constructor_declaration(c);
        break;
      case TypeKind::Kind::Ptype_record:
        for (const LabelDeclaration* l : td->ptype_kind.labels) label_declaration(l);
        break;
      default: break;
    }
    if (td->ptype_manifest) typ(td->ptype_manifest);
    attributes(td->ptype_attributes);
    if (td->ptype_kind.kind == TypeKind::Kind::Ptype_record && td->ptype_kind.labels.empty())
      empty_record(td->ptype_loc);
  }
  void constructor_arguments(const ConstructorArguments& a) {
    if (a.kind == ConstructorArguments::Kind::Pcstr_tuple) {
      for (const CoreType* t : a.tuple) typ(t);
    } else {
      for (const LabelDeclaration* l : a.record) label_declaration(l);
    }
  }
  void constructor_declaration(const ConstructorDeclaration* c) {
    constructor_arguments(c->pcd_args);
    if (c->pcd_res) typ(c->pcd_res);
    attributes(c->pcd_attributes);
  }
  void label_declaration(const LabelDeclaration* l) {
    typ(l->pld_type);
    attributes(l->pld_attributes);
  }
  void extension_constructor(const ExtensionConstructor* ec) {
    if (ec->pext_kind.kind == ExtensionConstructorKind::Kind::Pext_decl) {
      constructor_arguments(ec->pext_kind.args);
      if (ec->pext_kind.res) typ(ec->pext_kind.res);
    }
    attributes(ec->pext_attributes);
    if (ec->pext_kind.kind == ExtensionConstructorKind::Kind::Pext_rebind) simple_longident(ec->pext_kind.rebind);
  }
  void type_extension(const TypeExtension* te) {
    for (const ExtensionConstructor* ec : te->ptyext_constructors) extension_constructor(ec);
    for (const TypeParam& p : te->ptyext_params) typ(p.ty);
    attributes(te->ptyext_attributes);
  }
  void type_exception(const TypeException* te) {
    extension_constructor(te->ptyexn_constructor);
    attributes(te->ptyexn_attributes);
  }
  void value_description(const ValueDescription* vd) {
    typ(vd->pval_type);
    attributes(vd->pval_attributes);
  }

  // ---- classes ----
  void class_type(const ClassType* ct) {
    attributes(ct->pcty_attributes);
    const ClassTypeDesc* d = ct->pcty_desc;
    switch (d->kind) {
      case ClassTypeDesc::Kind::Pcty_constr:
        for (const CoreType* t : as<Pcty_constr>(d)->args) typ(t);
        break;
      case ClassTypeDesc::Kind::Pcty_signature: {
        const ClassSignature* s = as<Pcty_signature>(d)->sign;
        typ(s->pcsig_self);
        for (const ClassTypeField* f : s->pcsig_fields) class_type_field(f);
        break;
      }
      case ClassTypeDesc::Kind::Pcty_arrow: {
        auto* a = as<Pcty_arrow>(d);
        typ(a->ty);
        class_type(a->cty);
        break;
      }
      case ClassTypeDesc::Kind::Pcty_extension: extension(as<Pcty_extension>(d)->ext); break;
      case ClassTypeDesc::Kind::Pcty_open: {
        auto* o = as<Pcty_open>(d);
        open_description(o->od);
        class_type(o->cty);
        break;
      }
    }
  }
  void class_type_field(const ClassTypeField* f) {
    attributes(f->pctf_attributes);
    const ClassTypeFieldDesc* d = f->pctf_desc;
    using K = ClassTypeFieldDesc::Kind;
    switch (d->kind) {
      case K::Pctf_inherit: class_type(as<Pctf_inherit>(d)->cty); break;
      case K::Pctf_val: typ(as<Pctf_val>(d)->ty); break;
      case K::Pctf_method: typ(as<Pctf_method>(d)->ty); break;
      case K::Pctf_constraint:
        typ(as<Pctf_constraint>(d)->t1);
        typ(as<Pctf_constraint>(d)->t2);
        break;
      case K::Pctf_attribute: attribute(as<Pctf_attribute>(d)->attr); break;
      case K::Pctf_extension: extension(as<Pctf_extension>(d)->ext); break;
    }
  }
  template <class A, class F>
  void class_infos(const ClassInfos<A>* c, F&& f) {
    for (const TypeParam& p : c->pci_params) typ(p.ty);
    f(c->pci_expr);
    attributes(c->pci_attributes);
  }
  void class_expr(const ClassExpr* ce) {
    attributes(ce->pcl_attributes);
    const ClassExprDesc* d = ce->pcl_desc;
    using K = ClassExprDesc::Kind;
    switch (d->kind) {
      case K::Pcl_constr:
        for (const CoreType* t : as<Pcl_constr>(d)->args) typ(t);
        break;
      case K::Pcl_structure: class_structure(as<Pcl_structure>(d)->cs); break;
      case K::Pcl_fun: {
        auto* f = as<Pcl_fun>(d);
        if (f->default_) expr(f->default_);
        pat(f->pat);
        class_expr(f->body);
        break;
      }
      case K::Pcl_apply: {
        auto* a = as<Pcl_apply>(d);
        class_expr(a->ce);
        for (const ArgExpression& x : a->args) expr(x.exp);
        break;
      }
      case K::Pcl_let: {
        auto* l = as<Pcl_let>(d);
        for (const ValueBinding* vb : l->vbs) value_binding(vb);
        class_expr(l->body);
        break;
      }
      case K::Pcl_constraint: {
        auto* c = as<Pcl_constraint>(d);
        class_expr(c->ce);
        class_type(c->cty);
        break;
      }
      case K::Pcl_extension: extension(as<Pcl_extension>(d)->ext); break;
      case K::Pcl_open: {
        auto* o = as<Pcl_open>(d);
        open_description(o->od);
        class_expr(o->ce);
        break;
      }
    }
    const Location& loc = ce->pcl_loc;
    if (auto* a = as<Pcl_apply>(d); a && a->args.empty()) no_args(loc);
    if (auto* c = as<Pcl_constr>(d)) simple_longident(c->lid);
  }
  void class_structure(const ClassStructure* cs) {
    pat(cs->pcstr_self);
    for (const ClassField* f : cs->pcstr_fields) class_field(f);
  }
  void class_field_kind(const ClassFieldKind& k) {
    if (k.kind == ClassFieldKind::Kind::Cfk_concrete) expr(k.exp);
    else typ(k.ty);
  }
  void class_field(const ClassField* f) {
    attributes(f->pcf_attributes);
    const ClassFieldDesc* d = f->pcf_desc;
    using K = ClassFieldDesc::Kind;
    switch (d->kind) {
      case K::Pcf_inherit: class_expr(as<Pcf_inherit>(d)->ce); break;
      case K::Pcf_val: class_field_kind(as<Pcf_val>(d)->kind_); break;
      case K::Pcf_method: class_field_kind(as<Pcf_method>(d)->kind_); break;
      case K::Pcf_constraint:
        typ(as<Pcf_constraint>(d)->t1);
        typ(as<Pcf_constraint>(d)->t2);
        break;
      case K::Pcf_initializer: expr(as<Pcf_initializer>(d)->exp); break;
      case K::Pcf_attribute: attribute(as<Pcf_attribute>(d)->attr); break;
      case K::Pcf_extension: extension(as<Pcf_extension>(d)->ext); break;
    }
  }

  // ---- modules ----
  void functor_param(const FunctorParameter& p) {
    if (!p.is_unit) module_type(p.mty);
  }
  void module_type(const ModuleType* mty) {
    attributes(mty->pmty_attributes);
    const ModuleTypeDesc* d = mty->pmty_desc;
    using K = ModuleTypeDesc::Kind;
    switch (d->kind) {
      case K::Pmty_ident:
      case K::Pmty_alias: break;
      case K::Pmty_signature: signature(as<Pmty_signature>(d)->sg); break;
      case K::Pmty_functor: {
        auto* f = as<Pmty_functor>(d);
        functor_param(f->param);
        module_type(f->body);
        break;
      }
      case K::Pmty_with: {
        auto* w = as<Pmty_with>(d);
        module_type(w->mty);
        for (const WithConstraint* c : w->cstrs) with_constraint(c);
        break;
      }
      case K::Pmty_typeof: module_expr(as<Pmty_typeof>(d)->me); break;
      case K::Pmty_extension: extension(as<Pmty_extension>(d)->ext); break;
    }
    if (auto* a = as<Pmty_alias>(d)) simple_longident(a->lid);
  }
  void with_constraint(const WithConstraint* wc) {
    using K = WithConstraint::Kind;
    switch (wc->kind) {
      case K::Pwith_type:
      case K::Pwith_typesubst: type_declaration(wc->decl); break;
      case K::Pwith_modtype:
      case K::Pwith_modtypesubst: module_type(wc->mty); break;
      default: break;
    }
    if (wc->kind == K::Pwith_type || wc->kind == K::Pwith_module) simple_longident(wc->lid);
  }
  void module_expr(const ModuleExpr* me) {
    attributes(me->pmod_attributes);
    const ModuleExprDesc* d = me->pmod_desc;
    using K = ModuleExprDesc::Kind;
    switch (d->kind) {
      case K::Pmod_ident: break;
      case K::Pmod_structure: structure(as<Pmod_structure>(d)->str); break;
      case K::Pmod_functor: {
        auto* f = as<Pmod_functor>(d);
        functor_param(f->param);
        module_expr(f->body);
        break;
      }
      case K::Pmod_apply: {
        auto* a = as<Pmod_apply>(d);
        module_expr(a->fn);
        module_expr(a->arg);
        break;
      }
      case K::Pmod_apply_unit: module_expr(as<Pmod_apply_unit>(d)->fn); break;
      case K::Pmod_constraint: {
        auto* c = as<Pmod_constraint>(d);
        module_expr(c->me);
        module_type(c->mty);
        break;
      }
      case K::Pmod_unpack: expr(as<Pmod_unpack>(d)->exp); break;
      case K::Pmod_extension: extension(as<Pmod_extension>(d)->ext); break;
    }
    if (auto* i = as<Pmod_ident>(d)) simple_longident(i->lid);
  }
  void open_description(const OpenDescription* od) { attributes(od->popen_attributes); }
  void module_declaration(const ModuleDeclaration* md) {
    module_type(md->pmd_type);
    attributes(md->pmd_attributes);
  }
  void module_type_declaration(const ModuleTypeDeclaration* mtd) {
    if (mtd->pmtd_type) module_type(mtd->pmtd_type);
    attributes(mtd->pmtd_attributes);
  }
  void module_binding(const ModuleBinding* mb) {
    module_expr(mb->pmb_expr);
    attributes(mb->pmb_attributes);
  }

  void structure_item(const StructureItem* st) {
    const StructureItemDesc* d = st->pstr_desc;
    using K = StructureItemDesc::Kind;
    switch (d->kind) {
      case K::Pstr_eval: {
        auto* e = as<Pstr_eval>(d);
        attributes(e->attrs);
        expr(e->exp);
        break;
      }
      case K::Pstr_value:
        for (const ValueBinding* vb : as<Pstr_value>(d)->vbs) value_binding(vb);
        break;
      case K::Pstr_primitive: value_description(as<Pstr_primitive>(d)->vd); break;
      case K::Pstr_type:
        for (const TypeDeclaration* td : as<Pstr_type>(d)->decls) type_declaration(td);
        break;
      case K::Pstr_typext: type_extension(as<Pstr_typext>(d)->ext); break;
      case K::Pstr_exception: type_exception(as<Pstr_exception>(d)->exn); break;
      case K::Pstr_module: module_binding(as<Pstr_module>(d)->mb); break;
      case K::Pstr_recmodule:
        for (const ModuleBinding* mb : as<Pstr_recmodule>(d)->mbs) module_binding(mb);
        break;
      case K::Pstr_modtype: module_type_declaration(as<Pstr_modtype>(d)->mtd); break;
      case K::Pstr_open: {
        const OpenDeclaration* od = as<Pstr_open>(d)->od;
        module_expr(od->popen_expr);
        attributes(od->popen_attributes);
        break;
      }
      case K::Pstr_class:
        for (const ClassDeclaration* c : as<Pstr_class>(d)->decls)
          class_infos(c, [&](const ClassExpr* e) { class_expr(e); });
        break;
      case K::Pstr_class_type:
        for (const ClassTypeDeclaration* c : as<Pstr_class_type>(d)->decls)
          class_infos(c, [&](const ClassType* t) { class_type(t); });
        break;
      case K::Pstr_include: {
        const IncludeDeclaration* i = as<Pstr_include>(d)->incl;
        module_expr(i->pincl_mod);
        attributes(i->pincl_attributes);
        break;
      }
      case K::Pstr_extension: {
        auto* e = as<Pstr_extension>(d);
        attributes(e->attrs);
        extension(e->ext);
        break;
      }
      case K::Pstr_attribute: attribute(as<Pstr_attribute>(d)->attr); break;
    }
    const Location& loc = st->pstr_loc;
    if (auto* t = as<Pstr_type>(d); t && t->decls.empty()) empty_type(loc);
    if (auto* v = as<Pstr_value>(d); v && v->vbs.empty()) empty_let(loc);
  }
  void structure(Structure s) {
    for (const StructureItem* it : s) structure_item(it);
  }

  void signature_item(const SignatureItem* sg) {
    const SignatureItemDesc* d = sg->psig_desc;
    using K = SignatureItemDesc::Kind;
    switch (d->kind) {
      case K::Psig_value: value_description(as<Psig_value>(d)->vd); break;
      case K::Psig_type:
        for (const TypeDeclaration* td : as<Psig_type>(d)->decls) type_declaration(td);
        break;
      case K::Psig_typesubst:
        for (const TypeDeclaration* td : as<Psig_typesubst>(d)->decls) type_declaration(td);
        break;
      case K::Psig_typext: type_extension(as<Psig_typext>(d)->ext); break;
      case K::Psig_exception: type_exception(as<Psig_exception>(d)->exn); break;
      case K::Psig_module: module_declaration(as<Psig_module>(d)->md); break;
      case K::Psig_modsubst: attributes(as<Psig_modsubst>(d)->ms->pms_attributes); break;
      case K::Psig_recmodule:
        for (const ModuleDeclaration* md : as<Psig_recmodule>(d)->mds) module_declaration(md);
        break;
      case K::Psig_modtype: module_type_declaration(as<Psig_modtype>(d)->mtd); break;
      case K::Psig_modtypesubst: module_type_declaration(as<Psig_modtypesubst>(d)->mtd); break;
      case K::Psig_open: open_description(as<Psig_open>(d)->od); break;
      case K::Psig_include: {
        const IncludeDescription* i = as<Psig_include>(d)->incl;
        module_type(i->pincl_mod);
        attributes(i->pincl_attributes);
        break;
      }
      case K::Psig_class:
        for (const ClassDescription* c : as<Psig_class>(d)->decls)
          class_infos(c, [&](const ClassType* t) { class_type(t); });
        break;
      case K::Psig_class_type:
        for (const ClassTypeDeclaration* c : as<Psig_class_type>(d)->decls)
          class_infos(c, [&](const ClassType* t) { class_type(t); });
        break;
      case K::Psig_extension: {
        auto* e = as<Psig_extension>(d);
        attributes(e->attrs);
        extension(e->ext);
        break;
      }
      case K::Psig_attribute: attribute(as<Psig_attribute>(d)->attr); break;
    }
    const Location& loc = sg->psig_loc;
    if (auto* t = as<Psig_type>(d); t && t->decls.empty()) empty_type(loc);
    if (auto* m = as<Psig_modtypesubst>(d); m && !m->mtd->pmtd_type) module_type_substitution_missing_rhs(loc);
  }
  void signature(Signature s) {
    for (const SignatureItem* it : s) signature_item(it);
  }

 private:
  bool in_attr_ = false;

  void super_attribute(const Attribute* a) { payload(a->attr_payload); }

  // T.iter (its row_field / object_field are T's own, not the iterator's)
  void super_typ(const CoreType* ty) {
    attributes(ty->ptyp_attributes);
    const CoreTypeDesc* d = ty->ptyp_desc;
    using K = CoreTypeDesc::Kind;
    switch (d->kind) {
      case K::Ptyp_any:
      case K::Ptyp_var: break;
      case K::Ptyp_arrow: {
        auto* a = as<Ptyp_arrow>(d);
        typ(a->t1);
        typ(a->t2);
        break;
      }
      case K::Ptyp_tuple:
        for (const LabeledCoreType& t : as<Ptyp_tuple>(d)->tl) typ(t.ty);
        break;
      case K::Ptyp_constr:
        for (const CoreType* t : as<Ptyp_constr>(d)->args) typ(t);
        break;
      case K::Ptyp_object:
        for (const ObjectField* f : as<Ptyp_object>(d)->fields) {
          attributes(f->pof_attributes);
          if (auto* o = as<Otag>(f->pof_desc)) typ(o->ty);
          else typ(as<Oinherit>(f->pof_desc)->ty);
        }
        break;
      case K::Ptyp_class:
        for (const CoreType* t : as<Ptyp_class>(d)->args) typ(t);
        break;
      case K::Ptyp_alias: typ(as<Ptyp_alias>(d)->ty); break;
      case K::Ptyp_variant:
        for (const RowField* f : as<Ptyp_variant>(d)->fields) {
          attributes(f->prf_attributes);
          if (auto* r = as<Rtag>(f->prf_desc)) {
            for (const CoreType* t : r->types) typ(t);
          } else {
            typ(as<Rinherit>(f->prf_desc)->ty);
          }
        }
        break;
      case K::Ptyp_poly: typ(as<Ptyp_poly>(d)->ty); break;
      case K::Ptyp_package: package_type(as<Ptyp_package>(d)->pack); break;
      case K::Ptyp_open: typ(as<Ptyp_open>(d)->ty); break;
      case K::Ptyp_extension: extension(as<Ptyp_extension>(d)->ext); break;
      case K::Ptyp_functor: {
        auto* f = as<Ptyp_functor>(d);
        package_type(f->pack);
        typ(f->ty);
        break;
      }
    }
  }

  // P.iter
  void super_pat(const Pattern* p) {
    attributes(p->ppat_attributes);
    const PatternDesc* d = p->ppat_desc;
    using K = PatternDesc::Kind;
    switch (d->kind) {
      case K::Ppat_alias: pat(as<Ppat_alias>(d)->pat); break;
      case K::Ppat_tuple:
        for (const LabeledPattern& x : as<Ppat_tuple>(d)->pl) pat(x.pat);
        break;
      case K::Ppat_construct:
        if (const ConstructArg* a = as<Ppat_construct>(d)->arg) pat(a->pat);
        break;
      case K::Ppat_variant:
        if (const Pattern* a = as<Ppat_variant>(d)->arg) pat(a);
        break;
      case K::Ppat_record:
        for (auto& f : as<Ppat_record>(d)->fields) pat(f.second);
        break;
      case K::Ppat_array:
        for (const Pattern* x : as<Ppat_array>(d)->pats) pat(x);
        break;
      case K::Ppat_or:
        pat(as<Ppat_or>(d)->p1);
        pat(as<Ppat_or>(d)->p2);
        break;
      case K::Ppat_constraint:
        pat(as<Ppat_constraint>(d)->pat);
        typ(as<Ppat_constraint>(d)->ty);
        break;
      case K::Ppat_lazy: pat(as<Ppat_lazy>(d)->pat); break;
      case K::Ppat_unpack:
        if (const PackageType* pt = as<Ppat_unpack>(d)->pack) package_type(pt);
        break;
      case K::Ppat_effect:
        pat(as<Ppat_effect>(d)->eff);
        pat(as<Ppat_effect>(d)->cont);
        break;
      case K::Ppat_exception: pat(as<Ppat_exception>(d)->pat); break;
      case K::Ppat_extension: extension(as<Ppat_extension>(d)->ext); break;
      case K::Ppat_open: pat(as<Ppat_open>(d)->pat); break;
      default: break;  // Ppat_any, Ppat_var, Ppat_constant, Ppat_interval, Ppat_type
    }
  }

  // E.iter
  void super_expr(const Expression* e) {
    attributes(e->pexp_attributes);
    const ExpressionDesc* d = e->pexp_desc;
    using K = ExpressionDesc::Kind;
    switch (d->kind) {
      case K::Pexp_ident:
      case K::Pexp_constant:
      case K::Pexp_new:
      case K::Pexp_unreachable: break;
      case K::Pexp_let: {
        auto* l = as<Pexp_let>(d);
        for (const ValueBinding* vb : l->vbs) value_binding(vb);
        expr(l->body);
        break;
      }
      case K::Pexp_function: {
        auto* f = as<Pexp_function>(d);
        for (const FunctionParam* p : f->params) {
          if (p->pparam_desc.kind == FunctionParamDesc::Kind::Pparam_val) {
            if (p->pparam_desc.default_) expr(p->pparam_desc.default_);
            pat(p->pparam_desc.pat);
          }
        }
        if (const TypeConstraint* c = f->constraint) {
          if (c->kind == TypeConstraint::Kind::Pconstraint) {
            typ(c->ty);
          } else {
            if (c->from) typ(c->from);
            typ(c->ty);
          }
        }
        if (f->body->kind == FunctionBody::Kind::Pfunction_body) {
          expr(f->body->body);
        } else {
          cases(f->body->cases);
          attributes(f->body->attrs);
        }
        break;
      }
      case K::Pexp_apply: {
        auto* a = as<Pexp_apply>(d);
        expr(a->fn);
        for (const ArgExpression& x : a->args) expr(x.exp);
        break;
      }
      case K::Pexp_match:
        expr(as<Pexp_match>(d)->exp);
        cases(as<Pexp_match>(d)->cases);
        break;
      case K::Pexp_try:
        expr(as<Pexp_try>(d)->exp);
        cases(as<Pexp_try>(d)->cases);
        break;
      case K::Pexp_tuple:
        for (const LabeledExpression& x : as<Pexp_tuple>(d)->el) expr(x.exp);
        break;
      case K::Pexp_construct:
        if (const Expression* a = as<Pexp_construct>(d)->arg) expr(a);
        break;
      case K::Pexp_variant:
        if (const Expression* a = as<Pexp_variant>(d)->arg) expr(a);
        break;
      case K::Pexp_record: {
        auto* r = as<Pexp_record>(d);
        for (auto& f : r->fields) expr(f.second);
        if (r->base) expr(r->base);
        break;
      }
      case K::Pexp_field: expr(as<Pexp_field>(d)->exp); break;
      case K::Pexp_setfield:
        expr(as<Pexp_setfield>(d)->exp);
        expr(as<Pexp_setfield>(d)->value);
        break;
      case K::Pexp_array:
        for (const Expression* x : as<Pexp_array>(d)->el) expr(x);
        break;
      case K::Pexp_ifthenelse: {
        auto* i = as<Pexp_ifthenelse>(d);
        expr(i->cond);
        expr(i->then_);
        if (i->else_) expr(i->else_);
        break;
      }
      case K::Pexp_sequence:
        expr(as<Pexp_sequence>(d)->e1);
        expr(as<Pexp_sequence>(d)->e2);
        break;
      case K::Pexp_while:
        expr(as<Pexp_while>(d)->cond);
        expr(as<Pexp_while>(d)->body);
        break;
      case K::Pexp_for: {
        auto* f = as<Pexp_for>(d);
        pat(f->pat);
        expr(f->lo);
        expr(f->hi);
        expr(f->body);
        break;
      }
      case K::Pexp_coerce: {
        auto* c = as<Pexp_coerce>(d);
        expr(c->exp);
        if (c->from) typ(c->from);
        typ(c->to);
        break;
      }
      case K::Pexp_constraint:
        expr(as<Pexp_constraint>(d)->exp);
        typ(as<Pexp_constraint>(d)->ty);
        break;
      case K::Pexp_send: expr(as<Pexp_send>(d)->exp); break;
      case K::Pexp_setinstvar: expr(as<Pexp_setinstvar>(d)->value); break;
      case K::Pexp_override:
        for (auto& f : as<Pexp_override>(d)->fields) expr(f.second);
        break;
      case K::Pexp_assert: expr(as<Pexp_assert>(d)->exp); break;
      case K::Pexp_lazy: expr(as<Pexp_lazy>(d)->exp); break;
      case K::Pexp_poly: {
        auto* p = as<Pexp_poly>(d);
        expr(p->exp);
        if (p->ty) typ(p->ty);
        break;
      }
      case K::Pexp_object: class_structure(as<Pexp_object>(d)->cs); break;
      case K::Pexp_newtype: expr(as<Pexp_newtype>(d)->body); break;
      case K::Pexp_pack: {
        auto* p = as<Pexp_pack>(d);
        module_expr(p->me);
        if (p->pack) package_type(p->pack);
        break;
      }
      case K::Pexp_letop: {
        const Letop* l = as<Pexp_letop>(d)->letop;
        binding_op(l->let_);
        for (const BindingOp* b : l->ands) binding_op(b);
        expr(l->body);
        break;
      }
      case K::Pexp_extension: extension(as<Pexp_extension>(d)->ext); break;
      case K::Pexp_struct_item: {
        auto* s = as<Pexp_struct_item>(d);
        structure_item(s->item);
        expr(s->body);
        break;
      }
    }
  }
};

}  // namespace

void structure(parsetree::Structure st) {
  Iterator it;
  it.structure(st);
}
void signature(parsetree::Signature sg) {
  Iterator it;
  it.signature(sg);
}

}  // namespace cppcaml::typing::ast_invariants
