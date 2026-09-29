// Parsetree values in OCaml's runtime representation (see
// parsetree_ovalue.hpp).  Tags follow the declaration order of
// parsing/parsetree.mli, asttypes.mli and longident.mli: constant
// constructors and non-constant ones are numbered separately.
#include "cppcaml/typing/parsetree_ovalue.hpp"

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace cppcaml::typing::parsetree {

namespace {

using V = const OValue*;

V I(long n) {
  auto* o = make<OValue>();
  o->kind = OValue::Kind::Int;
  o->i = n;
  return o;
}
V S(std::string_view s) {
  auto* o = make<OValue>();
  o->kind = OValue::Kind::String;
  o->s = s;
  return o;
}
V B(unsigned tag, std::initializer_list<V> fields) {
  auto* o = make<OValue>();
  o->kind = OValue::Kind::Block;
  o->tag = tag;
  o->fields = slice(std::vector<V>(fields));
  return o;
}
V none() { return I(0); }
V some(V v) { return B(0, {v}); }
V opt(V v) { return v ? some(v) : none(); }
V boolean(bool b) { return I(b ? 1 : 0); }
template <class L, class F>
V list(const L& l, F&& f) {
  V r = I(0);
  std::vector<V> items;
  for (auto& x : l) items.push_back(f(x));
  for (std::size_t k = items.size(); k-- > 0;) r = B(0, {items[k], r});
  return r;
}
V strlist(Slice<std::string_view> l) {
  return list(l, [](std::string_view s) { return S(s); });
}

// OCaml's parser shares location data physically, and Marshal keeps that
// sharing: the lexer makes one position per token boundary (equal
// positions of one payload are one value here), mkstrexp reuses an
// expression's loc as its item's (structure_item), and Docstrings builds a
// doc attribute's constant, expression and item from one loc (loc_memo is
// only set for those).
// A record with an identity (same_record: read by input_value, from a
// binary AST or a .cmi) keeps the sharing input_value gave it instead: one
// value per record (id_memo), whatever its contents equal.
using PosKey = std::tuple<std::string, long, long, long>;
std::map<PosKey, V>* pos_memo = nullptr;
std::map<std::tuple<PosKey, PosKey, bool>, V>* loc_memo = nullptr;
std::map<const void*, V>* id_memo = nullptr;
PosKey pos_key(const Position& p) { return {std::string(p.pos_fname), p.pos_lnum, p.pos_bol, p.pos_cnum}; }
V position(const Position& p) {
  if (id_memo && same_record(p)) {
    auto [it, fresh] = id_memo->try_emplace(p.obj, nullptr);
    if (fresh) it->second = B(0, {S(p.pos_fname), I(p.pos_lnum), I(p.pos_bol), I(p.pos_cnum)});
    return it->second;
  }
  if (!pos_memo) return B(0, {S(p.pos_fname), I(p.pos_lnum), I(p.pos_bol), I(p.pos_cnum)});
  auto [it, fresh] = pos_memo->try_emplace(pos_key(p), nullptr);
  if (fresh) it->second = B(0, {S(p.pos_fname), I(p.pos_lnum), I(p.pos_bol), I(p.pos_cnum)});
  return it->second;
}
// Whether [a] and [b] are one location record: records input_value read
// are one exactly when they are the same record; the parser's by value.
bool same_loc(const Location& a, const Location& b) {
  if (same_record(a) && same_record(b)) return a.obj == b.obj;
  return pos_key(a.loc_start) == pos_key(b.loc_start) && pos_key(a.loc_end) == pos_key(b.loc_end) &&
         a.loc_ghost == b.loc_ghost;
}
V loc_block(const Location& l, V a, V b) {
  V r = B(0, {a, b, boolean(l.loc_ghost)});
  if (same_record(l)) const_cast<OValue*>(r)->loc_rec = make<Location>(l);
  return r;
}
// Writing a whole parsetree (Pparse.write_ast): every location is written
// as the writer writes the same location of a typed tree (loc_val)
bool whole_ast = false;
V loc(const Location& l) {
  if (whole_ast && !same_record(l)) {
    V r = B(0, {position(l.loc_start), position(l.loc_end), boolean(l.loc_ghost)});
    const_cast<OValue*>(r)->loc_val = make<Location>(l);
    return r;
  }
  if (id_memo && same_record(l)) {
    auto [it, fresh] = id_memo->try_emplace(l.obj, nullptr);
    if (fresh) {
      V a = position(l.loc_start);
      V b = position(l.loc_end);
      it->second = loc_block(l, a, b);
    }
    return it->second;
  }
  if (!loc_memo) return loc_block(l, position(l.loc_start), position(l.loc_end));
  auto [it, fresh] = loc_memo->try_emplace({pos_key(l.loc_start), pos_key(l.loc_end), l.loc_ghost}, nullptr);
  if (fresh) {
    V a = position(l.loc_start);
    V b = position(l.loc_end);
    it->second = loc_block(l, a, b);
  }
  return it->second;
}
V locs(LocationStack s) {
  return list(s, [](const Location& l) { return loc(l); });
}
V strloc(const StrLoc& s) { return B(0, {S(s.txt), loc(s.loc)}); }
V optstr(const OptStr& o) { return o.some ? some(S(o.v)) : none(); }
V optstrloc(const OptStrLoc& s) { return B(0, {optstr(s.txt), loc(s.loc)}); }
V lident(Longident::t l) {
  switch (l->kind) {
    case Longident::Kind::Lident: return B(0, {S(l->s)});
    case Longident::Kind::Ldot:
      return B(1, {B(0, {lident(l->l1), loc(l->l1_loc())}), B(0, {S(l->s), loc(l->s_loc())})});
    case Longident::Kind::Lapply:
      return B(2, {B(0, {lident(l->l1), loc(l->l1_loc())}), B(0, {lident(l->l2), loc(l->l2_loc())})});
  }
  return I(0);
}
V lidloc(const LidLoc& l) { return B(0, {lident(l.txt), loc(l.loc)}); }
V arg_label(const ArgLabel& l) {
  switch (l.kind) {
    case ArgLabel::Kind::Nolabel: return I(0);
    case ArgLabel::Kind::Labelled: return B(0, {S(l.name)});
    case ArgLabel::Kind::Optional: return B(1, {S(l.name)});
  }
  return I(0);
}
template <class E>
V flag(E e) {
  return I(static_cast<long>(e));
}
V char_opt(bool has, char c) { return has ? some(I(static_cast<unsigned char>(c))) : none(); }

V constant(const Constant& c) {
  const ConstantDesc& d = c.pconst_desc;
  V desc = I(0);
  switch (d.kind) {
    case ConstantDesc::Kind::Pconst_integer: desc = B(0, {S(d.s), char_opt(d.has_suffix, d.suffix)}); break;
    case ConstantDesc::Kind::Pconst_char: desc = B(1, {I(static_cast<unsigned char>(d.c))}); break;
    case ConstantDesc::Kind::Pconst_string: desc = B(2, {S(d.s), loc(d.str_loc), optstr(d.delim)}); break;
    case ConstantDesc::Kind::Pconst_float: desc = B(3, {S(d.s), char_opt(d.has_suffix, d.suffix)}); break;
  }
  return B(0, {desc, loc(c.pconst_loc)});
}

// forward declarations
V payload(const Payload& p);
V attrs(Attributes a);
V core_type(const CoreType* t);
V pattern(const Pattern* p);
V expression(const Expression* e);
V structure(Structure s);
V signature(Signature s);
V module_expr(const ModuleExpr* m);
V module_type(const ModuleType* m);
V class_expr(const ClassExpr* c);
V class_type(const ClassType* c);
V class_structure(const ClassStructure* c);
V type_declaration(const TypeDeclaration* d);
V value_binding(const ValueBinding* vb);
V structure_item(const StructureItem* it);

V attribute(const Attribute* a) { return B(0, {strloc(a->attr_name), payload(a->attr_payload), loc(a->attr_loc)}); }
V attrs(Attributes a) {
  return list(a, [](const Attribute* x) { return attribute(x); });
}
V extension(const Extension* e) { return B(0, {strloc(e->name), payload(e->payload)}); }

V payload(const Payload& p) {
  switch (p.kind) {
    case Payload::Kind::PStr: return B(0, {structure(p.str)});
    case Payload::Kind::PSig: return B(1, {signature(p.sig)});
    case Payload::Kind::PTyp: return B(2, {core_type(p.typ)});
    case Payload::Kind::PPat: return B(3, {pattern(p.pat), opt(p.guard ? expression(p.guard) : nullptr)});
  }
  return I(0);
}

V package_type(const PackageType* p) {
  return B(0, {lidloc(p->ppt_path),
               list(p->ppt_constraints,
                    [](const std::pair<LidLoc, const CoreType*>& c) { return B(0, {lidloc(c.first), core_type(c.second)}); }),
               loc(p->ppt_loc), attrs(p->ppt_attrs)});
}

V core_types(Slice<const CoreType*> l) {
  return list(l, [](const CoreType* t) { return core_type(t); });
}

V core_type_desc(const CoreTypeDesc* d) {
  using K = CoreTypeDesc::Kind;
  switch (d->kind) {
    case K::Ptyp_any: return I(0);
    case K::Ptyp_var: return B(0, {S(as<Ptyp_var>(d)->name)});
    case K::Ptyp_arrow: {
      auto* a = as<Ptyp_arrow>(d);
      return B(1, {arg_label(a->label), core_type(a->t1), core_type(a->t2)});
    }
    case K::Ptyp_tuple:
      return B(2, {list(as<Ptyp_tuple>(d)->tl,
                        [](const LabeledCoreType& x) { return B(0, {optstr(x.label), core_type(x.ty)}); })});
    case K::Ptyp_constr: {
      auto* c = as<Ptyp_constr>(d);
      return B(3, {lidloc(c->lid), core_types(c->args)});
    }
    case K::Ptyp_object: {
      auto* o = as<Ptyp_object>(d);
      return B(4, {list(o->fields,
                        [](const ObjectField* f) {
                          V desc;
                          if (auto* t = as<Otag>(f->pof_desc)) desc = B(0, {strloc(t->label), core_type(t->ty)});
                          else desc = B(1, {core_type(as<Oinherit>(f->pof_desc)->ty)});
                          return B(0, {desc, loc(f->pof_loc), attrs(f->pof_attributes)});
                        }),
                   flag(o->closed)});
    }
    case K::Ptyp_class: {
      auto* c = as<Ptyp_class>(d);
      return B(5, {lidloc(c->lid), core_types(c->args)});
    }
    case K::Ptyp_alias: {
      auto* a = as<Ptyp_alias>(d);
      return B(6, {core_type(a->ty), strloc(a->name)});
    }
    case K::Ptyp_variant: {
      auto* v = as<Ptyp_variant>(d);
      return B(7, {list(v->fields,
                        [](const RowField* f) {
                          V desc;
                          if (auto* t = as<Rtag>(f->prf_desc))
                            desc = B(0, {strloc(t->label), boolean(t->constant), core_types(t->types)});
                          else desc = B(1, {core_type(as<Rinherit>(f->prf_desc)->ty)});
                          return B(0, {desc, loc(f->prf_loc), attrs(f->prf_attributes)});
                        }),
                   flag(v->closed), v->has_labels ? some(strlist(v->labels)) : none()});
    }
    case K::Ptyp_poly: {
      auto* p = as<Ptyp_poly>(d);
      return B(8, {list(p->vars, [](const StrLoc& s) { return strloc(s); }), core_type(p->ty)});
    }
    case K::Ptyp_package: return B(9, {package_type(as<Ptyp_package>(d)->pack)});
    case K::Ptyp_open: {
      auto* o = as<Ptyp_open>(d);
      return B(10, {lidloc(o->lid), core_type(o->ty)});
    }
    case K::Ptyp_extension: return B(11, {extension(as<Ptyp_extension>(d)->ext)});
    case K::Ptyp_functor: {
      auto* f = as<Ptyp_functor>(d);
      return B(12, {arg_label(f->label), strloc(f->name), package_type(f->pack), core_type(f->ty)});
    }
  }
  return I(0);
}
V core_type(const CoreType* t) {
  return B(0, {core_type_desc(t->ptyp_desc), loc(t->ptyp_loc), locs(t->ptyp_loc_stack), attrs(t->ptyp_attributes)});
}

V patterns(Slice<const Pattern*> l) {
  return list(l, [](const Pattern* p) { return pattern(p); });
}
V pattern_desc(const PatternDesc* d) {
  using K = PatternDesc::Kind;
  switch (d->kind) {
    case K::Ppat_any: return I(0);
    case K::Ppat_var: return B(0, {strloc(as<Ppat_var>(d)->name)});
    case K::Ppat_alias: {
      auto* a = as<Ppat_alias>(d);
      return B(1, {pattern(a->pat), strloc(a->name)});
    }
    case K::Ppat_constant: return B(2, {constant(as<Ppat_constant>(d)->c)});
    case K::Ppat_interval: {
      auto* i = as<Ppat_interval>(d);
      return B(3, {constant(i->c1), constant(i->c2)});
    }
    case K::Ppat_tuple: {
      auto* t = as<Ppat_tuple>(d);
      return B(4, {list(t->pl, [](const LabeledPattern& x) { return B(0, {optstr(x.label), pattern(x.pat)}); }),
                   flag(t->closed)});
    }
    case K::Ppat_construct: {
      auto* c = as<Ppat_construct>(d);
      V arg = none();
      if (c->arg)
        arg = some(B(0, {list(c->arg->vars, [](const StrLoc& s) { return strloc(s); }), pattern(c->arg->pat)}));
      return B(5, {lidloc(c->lid), arg});
    }
    case K::Ppat_variant: {
      auto* v = as<Ppat_variant>(d);
      return B(6, {S(v->label), opt(v->arg ? pattern(v->arg) : nullptr)});
    }
    case K::Ppat_record: {
      auto* r = as<Ppat_record>(d);
      return B(7, {list(r->fields,
                        [](const std::pair<LidLoc, const Pattern*>& f) {
                          return B(0, {lidloc(f.first), pattern(f.second)});
                        }),
                   flag(r->closed)});
    }
    case K::Ppat_array: return B(8, {patterns(as<Ppat_array>(d)->pats)});
    case K::Ppat_or: {
      auto* o = as<Ppat_or>(d);
      return B(9, {pattern(o->p1), pattern(o->p2)});
    }
    case K::Ppat_constraint: {
      auto* c = as<Ppat_constraint>(d);
      return B(10, {pattern(c->pat), core_type(c->ty)});
    }
    case K::Ppat_type: return B(11, {lidloc(as<Ppat_type>(d)->lid)});
    case K::Ppat_lazy: return B(12, {pattern(as<Ppat_lazy>(d)->pat)});
    case K::Ppat_unpack: {
      auto* u = as<Ppat_unpack>(d);
      return B(13, {optstrloc(u->name), opt(u->pack ? package_type(u->pack) : nullptr)});
    }
    case K::Ppat_exception: return B(14, {pattern(as<Ppat_exception>(d)->pat)});
    case K::Ppat_effect: {
      auto* e = as<Ppat_effect>(d);
      return B(15, {pattern(e->eff), pattern(e->cont)});
    }
    case K::Ppat_extension: return B(16, {extension(as<Ppat_extension>(d)->ext)});
    case K::Ppat_open: {
      auto* o = as<Ppat_open>(d);
      return B(17, {lidloc(o->lid), pattern(o->pat)});
    }
  }
  return I(0);
}
V pattern(const Pattern* p) {
  return B(0, {pattern_desc(p->ppat_desc), loc(p->ppat_loc), locs(p->ppat_loc_stack), attrs(p->ppat_attributes)});
}

V opt_exp(const Expression* e) { return e ? some(expression(e)) : none(); }
V opt_ty(const CoreType* t) { return t ? some(core_type(t)) : none(); }
V case_(const Case* c) { return B(0, {pattern(c->pc_lhs), opt_exp(c->pc_guard), expression(c->pc_rhs)}); }
V cases(Slice<const Case*> l) {
  return list(l, [](const Case* c) { return case_(c); });
}
V value_bindings(Slice<const ValueBinding*> l) {
  return list(l, [](const ValueBinding* vb) { return value_binding(vb); });
}
V binding_op(const BindingOp* b) {
  return B(0, {strloc(b->pbop_op), pattern(b->pbop_pat), expression(b->pbop_exp), loc(b->pbop_loc)});
}
V arg_expressions(Slice<ArgExpression> l) {
  return list(l, [](const ArgExpression& a) { return B(0, {arg_label(a.label), expression(a.exp)}); });
}
V expressions(Slice<const Expression*> l) {
  return list(l, [](const Expression* e) { return expression(e); });
}

V expression_desc(const ExpressionDesc* d) {
  using K = ExpressionDesc::Kind;
  switch (d->kind) {
    case K::Pexp_ident: return B(0, {lidloc(as<Pexp_ident>(d)->lid)});
    case K::Pexp_constant: return B(1, {constant(as<Pexp_constant>(d)->c)});
    case K::Pexp_let: {
      auto* l = as<Pexp_let>(d);
      return B(2, {flag(l->rec), value_bindings(l->vbs), expression(l->body)});
    }
    case K::Pexp_function: {
      auto* f = as<Pexp_function>(d);
      V params = list(f->params, [](const FunctionParam* p) {
        const FunctionParamDesc& pd = p->pparam_desc;
        V desc;
        if (pd.kind == FunctionParamDesc::Kind::Pparam_val)
          desc = B(0, {arg_label(pd.label), opt_exp(pd.default_), pattern(pd.pat)});
        else desc = B(1, {strloc(pd.newtype)});
        return B(0, {loc(p->pparam_loc), desc});
      });
      V cstr = none();
      if (const TypeConstraint* c = f->constraint) {
        if (c->kind == TypeConstraint::Kind::Pconstraint) cstr = some(B(0, {core_type(c->ty)}));
        else cstr = some(B(1, {opt_ty(c->from), core_type(c->ty)}));
      }
      V body;
      if (f->body->kind == FunctionBody::Kind::Pfunction_body) body = B(0, {expression(f->body->body)});
      else body = B(1, {cases(f->body->cases), loc(f->body->loc), attrs(f->body->attrs)});
      return B(3, {params, cstr, body});
    }
    case K::Pexp_apply: {
      auto* a = as<Pexp_apply>(d);
      return B(4, {expression(a->fn), arg_expressions(a->args)});
    }
    case K::Pexp_match: {
      auto* m = as<Pexp_match>(d);
      return B(5, {expression(m->exp), cases(m->cases)});
    }
    case K::Pexp_try: {
      auto* t = as<Pexp_try>(d);
      return B(6, {expression(t->exp), cases(t->cases)});
    }
    case K::Pexp_tuple:
      return B(7, {list(as<Pexp_tuple>(d)->el,
                        [](const LabeledExpression& x) { return B(0, {optstr(x.label), expression(x.exp)}); })});
    case K::Pexp_construct: {
      auto* c = as<Pexp_construct>(d);
      return B(8, {lidloc(c->lid), opt_exp(c->arg)});
    }
    case K::Pexp_variant: {
      auto* v = as<Pexp_variant>(d);
      return B(9, {S(v->label), opt_exp(v->arg)});
    }
    case K::Pexp_record: {
      auto* r = as<Pexp_record>(d);
      return B(10, {list(r->fields,
                         [](const std::pair<LidLoc, const Expression*>& f) {
                           return B(0, {lidloc(f.first), expression(f.second)});
                         }),
                    opt_exp(r->base)});
    }
    case K::Pexp_field: {
      auto* f = as<Pexp_field>(d);
      return B(11, {expression(f->exp), lidloc(f->lid)});
    }
    case K::Pexp_setfield: {
      auto* f = as<Pexp_setfield>(d);
      return B(12, {expression(f->exp), lidloc(f->lid), expression(f->value)});
    }
    case K::Pexp_array: return B(13, {expressions(as<Pexp_array>(d)->el)});
    case K::Pexp_ifthenelse: {
      auto* i = as<Pexp_ifthenelse>(d);
      return B(14, {expression(i->cond), expression(i->then_), opt_exp(i->else_)});
    }
    case K::Pexp_sequence: {
      auto* s = as<Pexp_sequence>(d);
      return B(15, {expression(s->e1), expression(s->e2)});
    }
    case K::Pexp_while: {
      auto* w = as<Pexp_while>(d);
      return B(16, {expression(w->cond), expression(w->body)});
    }
    case K::Pexp_for: {
      auto* f = as<Pexp_for>(d);
      return B(17, {pattern(f->pat), expression(f->lo), expression(f->hi), flag(f->dir), expression(f->body)});
    }
    case K::Pexp_constraint: {
      auto* c = as<Pexp_constraint>(d);
      return B(18, {expression(c->exp), core_type(c->ty)});
    }
    case K::Pexp_coerce: {
      auto* c = as<Pexp_coerce>(d);
      return B(19, {expression(c->exp), opt_ty(c->from), core_type(c->to)});
    }
    case K::Pexp_send: {
      auto* s = as<Pexp_send>(d);
      return B(20, {expression(s->exp), strloc(s->meth)});
    }
    case K::Pexp_new: return B(21, {lidloc(as<Pexp_new>(d)->lid)});
    case K::Pexp_setinstvar: {
      auto* s = as<Pexp_setinstvar>(d);
      return B(22, {strloc(s->name), expression(s->value)});
    }
    case K::Pexp_override:
      return B(23, {list(as<Pexp_override>(d)->fields, [](const std::pair<StrLoc, const Expression*>& f) {
        return B(0, {strloc(f.first), expression(f.second)});
      })});
    case K::Pexp_struct_item: {
      auto* s = as<Pexp_struct_item>(d);
      return B(24, {structure_item(s->item), expression(s->body)});
    }
    case K::Pexp_assert: return B(25, {expression(as<Pexp_assert>(d)->exp)});
    case K::Pexp_lazy: return B(26, {expression(as<Pexp_lazy>(d)->exp)});
    case K::Pexp_poly: {
      auto* p = as<Pexp_poly>(d);
      return B(27, {expression(p->exp), opt_ty(p->ty)});
    }
    case K::Pexp_object: return B(28, {class_structure(as<Pexp_object>(d)->cs)});
    case K::Pexp_newtype: {
      auto* n = as<Pexp_newtype>(d);
      return B(29, {strloc(n->name), expression(n->body)});
    }
    case K::Pexp_pack: {
      auto* p = as<Pexp_pack>(d);
      return B(30, {module_expr(p->me), opt(p->pack ? package_type(p->pack) : nullptr)});
    }
    case K::Pexp_letop: {
      const Letop* l = as<Pexp_letop>(d)->letop;
      return B(31, {B(0, {binding_op(l->let_), list(l->ands, [](const BindingOp* b) { return binding_op(b); }),
                          expression(l->body)})});
    }
    case K::Pexp_extension: return B(32, {extension(as<Pexp_extension>(d)->ext)});
    case K::Pexp_unreachable: return I(0);
    case K::Pexp_hole: return I(1);
  }
  return I(0);
}
V expression(const Expression* e) {
  return B(0, {expression_desc(e->pexp_desc), loc(e->pexp_loc), locs(e->pexp_loc_stack), attrs(e->pexp_attributes)});
}

V value_binding(const ValueBinding* vb) {
  V cstr = none();
  if (const ValueConstraint* c = vb->pvb_constraint) {
    if (c->kind == ValueConstraint::Kind::Pvc_constraint)
      cstr = some(B(0, {list(c->locally_abstract_univars, [](const StrLoc& s) { return strloc(s); }),
                        core_type(c->typ)}));
    else cstr = some(B(1, {opt_ty(c->ground), core_type(c->coercion)}));
  }
  return B(0, {pattern(vb->pvb_pat), expression(vb->pvb_expr), cstr, attrs(vb->pvb_attributes), loc(vb->pvb_loc)});
}

// ---- declarations -------------------------------------------------------------------------
V type_params(Slice<TypeParam> l) {
  return list(l, [](const TypeParam& p) {
    return B(0, {core_type(p.ty), B(0, {flag(p.variance), flag(p.injectivity)})});
  });
}
V label_declaration(const LabelDeclaration* l) {
  return B(0, {strloc(l->pld_name), flag(l->pld_mutable), core_type(l->pld_type), loc(l->pld_loc),
               attrs(l->pld_attributes)});
}
V label_declarations(Slice<const LabelDeclaration*> l) {
  return list(l, [](const LabelDeclaration* x) { return label_declaration(x); });
}
V constructor_arguments(const ConstructorArguments& a) {
  if (a.kind == ConstructorArguments::Kind::Pcstr_tuple) return B(0, {core_types(a.tuple)});
  return B(1, {label_declarations(a.record)});
}
V strlocs(Slice<StrLoc> l) {
  return list(l, [](const StrLoc& s) { return strloc(s); });
}
V type_declaration(const TypeDeclaration* d) {
  V kind = I(0);
  switch (d->ptype_kind.kind) {
    case TypeKind::Kind::Ptype_abstract: kind = I(0); break;
    case TypeKind::Kind::Ptype_open: kind = I(1); break;
    case TypeKind::Kind::Ptype_variant:
      kind = B(0, {list(d->ptype_kind.constructors, [](const ConstructorDeclaration* c) {
                 return B(0, {strloc(c->pcd_name), strlocs(c->pcd_vars), constructor_arguments(c->pcd_args),
                              opt_ty(c->pcd_res), loc(c->pcd_loc), attrs(c->pcd_attributes)});
               })});
      break;
    case TypeKind::Kind::Ptype_record: kind = B(1, {label_declarations(d->ptype_kind.labels)}); break;
    case TypeKind::Kind::Ptype_external: kind = B(2, {S(d->ptype_kind.external)}); break;
  }
  return B(0, {strloc(d->ptype_name), type_params(d->ptype_params),
               list(d->ptype_constraints,
                    [](const TypeConstraintDecl& c) { return B(0, {core_type(c.t1), core_type(c.t2), loc(c.loc)}); }),
               kind, flag(d->ptype_private), opt_ty(d->ptype_manifest), attrs(d->ptype_attributes),
               loc(d->ptype_loc)});
}
V type_declarations(Slice<const TypeDeclaration*> l) {
  return list(l, [](const TypeDeclaration* d) { return type_declaration(d); });
}
V extension_constructor(const ExtensionConstructor* e) {
  V kind;
  if (e->pext_kind.kind == ExtensionConstructorKind::Kind::Pext_decl)
    kind = B(0, {strlocs(e->pext_kind.vars), constructor_arguments(e->pext_kind.args), opt_ty(e->pext_kind.res)});
  else kind = B(1, {lidloc(e->pext_kind.rebind)});
  return B(0, {strloc(e->pext_name), kind, loc(e->pext_loc), attrs(e->pext_attributes)});
}
V type_extension(const TypeExtension* t) {
  return B(0, {lidloc(t->ptyext_path), type_params(t->ptyext_params),
               list(t->ptyext_constructors, [](const ExtensionConstructor* e) { return extension_constructor(e); }),
               flag(t->ptyext_private), loc(t->ptyext_loc), attrs(t->ptyext_attributes)});
}
V type_exception(const TypeException* t) {
  return B(0, {extension_constructor(t->ptyexn_constructor), loc(t->ptyexn_loc), attrs(t->ptyexn_attributes)});
}
V value_description(const ValueDescription* v) {
  return B(0, {strloc(v->pval_name), core_type(v->pval_type), attrs(v->pval_attributes), loc(v->pval_loc)});
}
V primitive_description(const PrimitiveDescription* p) {
  V kind;
  if (p->pprim_kind.kind == PrimitiveKind::Kind::Pprim_decl)
    kind = B(0, {core_type(p->pprim_kind.ty), strlist(p->pprim_kind.prims)});
  else kind = B(1, {opt_ty(p->pprim_kind.ty), lidloc(p->pprim_kind.alias)});
  return B(0, {strloc(p->pprim_name), kind, attrs(p->pprim_attributes), loc(p->pprim_loc)});
}

// ---- classes ------------------------------------------------------------------------------
template <class A, class F>
V open_infos(const OpenInfos<A>* o, F&& f) {
  return B(0, {f(o->popen_expr), flag(o->popen_override), loc(o->popen_loc), attrs(o->popen_attributes)});
}
V open_description(const OpenDescription* o) {
  return open_infos(o, [](const LidLoc& l) { return lidloc(l); });
}
template <class A, class F>
V class_infos(const ClassInfos<A>* c, F&& f) {
  return B(0, {flag(c->pci_virt), type_params(c->pci_params), strloc(c->pci_name), f(c->pci_expr), loc(c->pci_loc),
               attrs(c->pci_attributes)});
}
V class_type_infos(const ClassInfos<const ClassType*>* c) {
  return class_infos(c, [](const ClassType* t) { return class_type(t); });
}

V class_type(const ClassType* c) {
  using K = ClassTypeDesc::Kind;
  const ClassTypeDesc* d = c->pcty_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Pcty_constr: {
      auto* x = as<Pcty_constr>(d);
      desc = B(0, {lidloc(x->lid), core_types(x->args)});
      break;
    }
    case K::Pcty_signature: {
      const ClassSignature* s = as<Pcty_signature>(d)->sign;
      V fields = list(s->pcsig_fields, [](const ClassTypeField* f) {
        using FK = ClassTypeFieldDesc::Kind;
        const ClassTypeFieldDesc* fd = f->pctf_desc;
        V fdesc = I(0);
        switch (fd->kind) {
          case FK::Pctf_inherit: fdesc = B(0, {class_type(as<Pctf_inherit>(fd)->cty)}); break;
          case FK::Pctf_val: {
            auto* v = as<Pctf_val>(fd);
            fdesc = B(1, {B(0, {strloc(v->label), flag(v->mut), flag(v->virt), core_type(v->ty)})});
            break;
          }
          case FK::Pctf_method: {
            auto* m = as<Pctf_method>(fd);
            fdesc = B(2, {B(0, {strloc(m->label), flag(m->priv), flag(m->virt), core_type(m->ty)})});
            break;
          }
          case FK::Pctf_constraint: {
            auto* k = as<Pctf_constraint>(fd);
            fdesc = B(3, {B(0, {core_type(k->t1), core_type(k->t2)})});
            break;
          }
          case FK::Pctf_attribute: fdesc = B(4, {attribute(as<Pctf_attribute>(fd)->attr)}); break;
          case FK::Pctf_extension: fdesc = B(5, {extension(as<Pctf_extension>(fd)->ext)}); break;
        }
        return B(0, {fdesc, loc(f->pctf_loc), attrs(f->pctf_attributes)});
      });
      desc = B(1, {B(0, {core_type(s->pcsig_self), fields})});
      break;
    }
    case K::Pcty_arrow: {
      auto* a = as<Pcty_arrow>(d);
      desc = B(2, {arg_label(a->label), core_type(a->ty), class_type(a->cty)});
      break;
    }
    case K::Pcty_extension: desc = B(3, {extension(as<Pcty_extension>(d)->ext)}); break;
    case K::Pcty_open: {
      auto* o = as<Pcty_open>(d);
      desc = B(4, {open_description(o->od), class_type(o->cty)});
      break;
    }
  }
  return B(0, {desc, loc(c->pcty_loc), attrs(c->pcty_attributes)});
}

V class_field_kind(const ClassFieldKind& k) {
  if (k.kind == ClassFieldKind::Kind::Cfk_virtual) return B(0, {core_type(k.ty)});
  return B(1, {flag(k.ovr), expression(k.exp)});
}
V class_structure(const ClassStructure* c) {
  V fields = list(c->pcstr_fields, [](const ClassField* f) {
    using FK = ClassFieldDesc::Kind;
    const ClassFieldDesc* fd = f->pcf_desc;
    V fdesc = I(0);
    switch (fd->kind) {
      case FK::Pcf_inherit: {
        auto* i = as<Pcf_inherit>(fd);
        fdesc = B(0, {flag(i->ovr), class_expr(i->ce), i->as ? some(strloc(*i->as)) : none()});
        break;
      }
      case FK::Pcf_val: {
        auto* v = as<Pcf_val>(fd);
        fdesc = B(1, {B(0, {strloc(v->label), flag(v->mut), class_field_kind(v->kind_)})});
        break;
      }
      case FK::Pcf_method: {
        auto* m = as<Pcf_method>(fd);
        fdesc = B(2, {B(0, {strloc(m->label), flag(m->priv), class_field_kind(m->kind_)})});
        break;
      }
      case FK::Pcf_constraint: {
        auto* k = as<Pcf_constraint>(fd);
        fdesc = B(3, {B(0, {core_type(k->t1), core_type(k->t2)})});
        break;
      }
      case FK::Pcf_initializer: fdesc = B(4, {expression(as<Pcf_initializer>(fd)->exp)}); break;
      case FK::Pcf_attribute: fdesc = B(5, {attribute(as<Pcf_attribute>(fd)->attr)}); break;
      case FK::Pcf_extension: fdesc = B(6, {extension(as<Pcf_extension>(fd)->ext)}); break;
    }
    return B(0, {fdesc, loc(f->pcf_loc), attrs(f->pcf_attributes)});
  });
  return B(0, {pattern(c->pcstr_self), fields});
}

V class_expr(const ClassExpr* c) {
  using K = ClassExprDesc::Kind;
  const ClassExprDesc* d = c->pcl_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Pcl_constr: {
      auto* x = as<Pcl_constr>(d);
      desc = B(0, {lidloc(x->lid), core_types(x->args)});
      break;
    }
    case K::Pcl_structure: desc = B(1, {class_structure(as<Pcl_structure>(d)->cs)}); break;
    case K::Pcl_fun: {
      auto* f = as<Pcl_fun>(d);
      desc = B(2, {arg_label(f->label), opt_exp(f->default_), pattern(f->pat), class_expr(f->body)});
      break;
    }
    case K::Pcl_apply: {
      auto* a = as<Pcl_apply>(d);
      desc = B(3, {class_expr(a->ce), arg_expressions(a->args)});
      break;
    }
    case K::Pcl_let: {
      auto* l = as<Pcl_let>(d);
      desc = B(4, {flag(l->rec), value_bindings(l->vbs), class_expr(l->body)});
      break;
    }
    case K::Pcl_constraint: {
      auto* k = as<Pcl_constraint>(d);
      desc = B(5, {class_expr(k->ce), class_type(k->cty)});
      break;
    }
    case K::Pcl_extension: desc = B(6, {extension(as<Pcl_extension>(d)->ext)}); break;
    case K::Pcl_open: {
      auto* o = as<Pcl_open>(d);
      desc = B(7, {open_description(o->od), class_expr(o->ce)});
      break;
    }
  }
  return B(0, {desc, loc(c->pcl_loc), attrs(c->pcl_attributes)});
}

// ---- modules ------------------------------------------------------------------------------
V functor_parameter(const FunctorParameter& p) {
  if (p.is_unit) return I(0);
  return B(0, {optstrloc(p.name), module_type(p.mty)});
}
V module_type(const ModuleType* m) {
  using K = ModuleTypeDesc::Kind;
  const ModuleTypeDesc* d = m->pmty_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Pmty_ident: desc = B(0, {lidloc(as<Pmty_ident>(d)->lid)}); break;
    case K::Pmty_signature: desc = B(1, {signature(as<Pmty_signature>(d)->sg)}); break;
    case K::Pmty_functor: {
      auto* f = as<Pmty_functor>(d);
      desc = B(2, {functor_parameter(f->param), module_type(f->body)});
      break;
    }
    case K::Pmty_with: {
      auto* w = as<Pmty_with>(d);
      V cstrs = list(w->cstrs, [](const WithConstraint* c) {
        using WK = WithConstraint::Kind;
        switch (c->kind) {
          case WK::Pwith_type: return B(0, {lidloc(c->lid), type_declaration(c->decl)});
          case WK::Pwith_module: return B(1, {lidloc(c->lid), lidloc(c->lid2)});
          case WK::Pwith_modtype: return B(2, {lidloc(c->lid), module_type(c->mty)});
          case WK::Pwith_modtypesubst: return B(3, {lidloc(c->lid), module_type(c->mty)});
          case WK::Pwith_typesubst: return B(4, {lidloc(c->lid), type_declaration(c->decl)});
          case WK::Pwith_modsubst: return B(5, {lidloc(c->lid), lidloc(c->lid2)});
        }
        return I(0);
      });
      desc = B(3, {module_type(w->mty), cstrs});
      break;
    }
    case K::Pmty_typeof: desc = B(4, {module_expr(as<Pmty_typeof>(d)->me)}); break;
    case K::Pmty_extension: desc = B(5, {extension(as<Pmty_extension>(d)->ext)}); break;
    case K::Pmty_alias: desc = B(6, {lidloc(as<Pmty_alias>(d)->lid)}); break;
  }
  return B(0, {desc, loc(m->pmty_loc), attrs(m->pmty_attributes)});
}

V module_expr(const ModuleExpr* m) {
  using K = ModuleExprDesc::Kind;
  const ModuleExprDesc* d = m->pmod_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Pmod_ident: desc = B(0, {lidloc(as<Pmod_ident>(d)->lid)}); break;
    case K::Pmod_structure: desc = B(1, {structure(as<Pmod_structure>(d)->str)}); break;
    case K::Pmod_functor: {
      auto* f = as<Pmod_functor>(d);
      desc = B(2, {functor_parameter(f->param), module_expr(f->body)});
      break;
    }
    case K::Pmod_apply: {
      auto* a = as<Pmod_apply>(d);
      desc = B(3, {module_expr(a->fn), module_expr(a->arg)});
      break;
    }
    case K::Pmod_apply_unit: desc = B(4, {module_expr(as<Pmod_apply_unit>(d)->fn)}); break;
    case K::Pmod_constraint: {
      auto* c = as<Pmod_constraint>(d);
      desc = B(5, {module_expr(c->me), module_type(c->mty)});
      break;
    }
    case K::Pmod_unpack: desc = B(6, {expression(as<Pmod_unpack>(d)->exp)}); break;
    case K::Pmod_extension: desc = B(7, {extension(as<Pmod_extension>(d)->ext)}); break;
    case K::Pmod_hole: desc = I(0); break;  // the only constant constructor
  }
  return B(0, {desc, loc(m->pmod_loc), attrs(m->pmod_attributes)});
}

V module_declaration(const ModuleDeclaration* md) {
  return B(0, {optstrloc(md->pmd_name), module_type(md->pmd_type), attrs(md->pmd_attributes), loc(md->pmd_loc)});
}
V module_type_declaration(const ModuleTypeDeclaration* m) {
  return B(0, {strloc(m->pmtd_name), opt(m->pmtd_type ? module_type(m->pmtd_type) : nullptr), attrs(m->pmtd_attributes),
               loc(m->pmtd_loc)});
}
V module_binding(const ModuleBinding* mb) {
  return B(0, {optstrloc(mb->pmb_name), module_expr(mb->pmb_expr), attrs(mb->pmb_attributes), loc(mb->pmb_loc)});
}

V signature_item(const SignatureItem* it) {
  using K = SignatureItemDesc::Kind;
  const SignatureItemDesc* d = it->psig_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Psig_value: desc = B(0, {value_description(as<Psig_value>(d)->vd)}); break;
    case K::Psig_primitive: desc = B(1, {primitive_description(as<Psig_primitive>(d)->pd)}); break;
    case K::Psig_type: {
      auto* t = as<Psig_type>(d);
      desc = B(2, {flag(t->rec), type_declarations(t->decls)});
      break;
    }
    case K::Psig_typesubst: desc = B(3, {type_declarations(as<Psig_typesubst>(d)->decls)}); break;
    case K::Psig_typext: desc = B(4, {type_extension(as<Psig_typext>(d)->ext)}); break;
    case K::Psig_exception: desc = B(5, {type_exception(as<Psig_exception>(d)->exn)}); break;
    case K::Psig_module: desc = B(6, {module_declaration(as<Psig_module>(d)->md)}); break;
    case K::Psig_modsubst: {
      const ModuleSubstitution* ms = as<Psig_modsubst>(d)->ms;
      desc = B(7, {B(0, {strloc(ms->pms_name), lidloc(ms->pms_manifest), attrs(ms->pms_attributes), loc(ms->pms_loc)})});
      break;
    }
    case K::Psig_recmodule:
      desc = B(8, {list(as<Psig_recmodule>(d)->mds, [](const ModuleDeclaration* m) { return module_declaration(m); })});
      break;
    case K::Psig_modtype: desc = B(9, {module_type_declaration(as<Psig_modtype>(d)->mtd)}); break;
    case K::Psig_modtypesubst: desc = B(10, {module_type_declaration(as<Psig_modtypesubst>(d)->mtd)}); break;
    case K::Psig_open: desc = B(11, {open_description(as<Psig_open>(d)->od)}); break;
    case K::Psig_include: {
      const IncludeDescription* i = as<Psig_include>(d)->incl;
      desc = B(12, {B(0, {module_type(i->pincl_mod), loc(i->pincl_loc), attrs(i->pincl_attributes)})});
      break;
    }
    case K::Psig_class:
      desc = B(13, {list(as<Psig_class>(d)->decls, [](const ClassDescription* c) { return class_type_infos(c); })});
      break;
    case K::Psig_class_type:
      desc = B(14, {list(as<Psig_class_type>(d)->decls,
                         [](const ClassTypeDeclaration* c) { return class_type_infos(c); })});
      break;
    case K::Psig_attribute: desc = B(15, {attribute(as<Psig_attribute>(d)->attr)}); break;
    case K::Psig_extension: {
      auto* e = as<Psig_extension>(d);
      desc = B(16, {extension(e->ext), attrs(e->attrs)});
      break;
    }
  }
  return B(0, {desc, loc(it->psig_loc)});
}
V signature(Signature s) {
  return list(s, [](const SignatureItem* it) { return signature_item(it); });
}

V structure_item(const StructureItem* it) {
  using K = StructureItemDesc::Kind;
  const StructureItemDesc* d = it->pstr_desc;
  V desc = I(0);
  switch (d->kind) {
    case K::Pstr_eval: {
      auto* e = as<Pstr_eval>(d);
      V ev = expression(e->exp);
      desc = B(0, {ev, attrs(e->attrs)});
      // mkstrexp: pstr_loc = e.pexp_loc
      if (same_loc(it->pstr_loc, e->exp->pexp_loc)) return B(0, {desc, ev->fields[1]});
      break;
    }
    case K::Pstr_value: {
      auto* v = as<Pstr_value>(d);
      desc = B(1, {flag(v->rec), value_bindings(v->vbs)});
      break;
    }
    case K::Pstr_val: desc = B(2, {value_description(as<Pstr_val>(d)->vd)}); break;
    case K::Pstr_primitive: desc = B(3, {primitive_description(as<Pstr_primitive>(d)->pd)}); break;
    case K::Pstr_type: {
      auto* t = as<Pstr_type>(d);
      desc = B(4, {flag(t->rec), type_declarations(t->decls)});
      break;
    }
    case K::Pstr_typext: desc = B(5, {type_extension(as<Pstr_typext>(d)->ext)}); break;
    case K::Pstr_exception: desc = B(6, {type_exception(as<Pstr_exception>(d)->exn)}); break;
    case K::Pstr_module: desc = B(7, {module_binding(as<Pstr_module>(d)->mb)}); break;
    case K::Pstr_recmodule:
      desc = B(8, {list(as<Pstr_recmodule>(d)->mbs, [](const ModuleBinding* m) { return module_binding(m); })});
      break;
    case K::Pstr_modtype: desc = B(9, {module_type_declaration(as<Pstr_modtype>(d)->mtd)}); break;
    case K::Pstr_open:
      desc = B(10, {open_infos(as<Pstr_open>(d)->od, [](const ModuleExpr* m) { return module_expr(m); })});
      break;
    case K::Pstr_class:
      desc = B(11, {list(as<Pstr_class>(d)->decls, [](const ClassDeclaration* c) {
                 return class_infos(c, [](const ClassExpr* e) { return class_expr(e); });
               })});
      break;
    case K::Pstr_class_type:
      desc = B(12, {list(as<Pstr_class_type>(d)->decls,
                         [](const ClassTypeDeclaration* c) { return class_type_infos(c); })});
      break;
    case K::Pstr_include: {
      const IncludeDeclaration* i = as<Pstr_include>(d)->incl;
      desc = B(13, {B(0, {module_expr(i->pincl_mod), loc(i->pincl_loc), attrs(i->pincl_attributes)})});
      break;
    }
    case K::Pstr_attribute: desc = B(14, {attribute(as<Pstr_attribute>(d)->attr)}); break;
    case K::Pstr_extension: {
      auto* e = as<Pstr_extension>(d);
      desc = B(15, {extension(e->ext), attrs(e->attrs)});
      break;
    }
  }
  return B(0, {desc, loc(it->pstr_loc)});
}
V structure(Structure s) {
  return list(s, [](const StructureItem* it) { return structure_item(it); });
}

}  // namespace

const OValue* ovalue_of_location(const Location& l) { return loc(l); }
const OValue* ovalue_of_longident(Longident::t lid) { return lident(lid); }
const OValue* ovalue_of_payload(const Payload& p, bool docstring) {
  std::map<PosKey, V> pm;
  std::map<std::tuple<PosKey, PosKey, bool>, V> lm;
  std::map<const void*, V> im;
  auto* sp = pos_memo;
  auto* sl = loc_memo;
  auto* si = id_memo;
  pos_memo = &pm;
  loc_memo = docstring ? &lm : nullptr;
  id_memo = &im;
  const OValue* r = payload(p);
  pos_memo = sp;
  loc_memo = sl;
  id_memo = si;
  return r;
}
const OValue* ovalue_of_attribute(const Attribute* a) { return attribute(a); }
const OValue* ovalue_of_structure(Structure s) { return structure(s); }
const OValue* ovalue_of_ast_structure(Structure s) {
  bool saved = whole_ast;
  whole_ast = true;
  const OValue* r = structure(s);
  whole_ast = saved;
  return r;
}
const OValue* ovalue_of_ast_signature(Signature s) {
  bool saved = whole_ast;
  whole_ast = true;
  const OValue* r = signature(s);
  whole_ast = saved;
  return r;
}
const OValue* ovalue_of_signature(Signature s) { return signature(s); }
const OValue* ovalue_of_core_type(const CoreType* t) { return core_type(t); }
const OValue* ovalue_of_expression(const Expression* e) { return expression(e); }
const OValue* ovalue_of_pattern(const Pattern* p) { return pattern(p); }

}  // namespace cppcaml::typing::parsetree
