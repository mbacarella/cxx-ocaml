// Parsetree values from OCaml's runtime representation: the inverse of
// parsetree_ovalue.cpp, for an AST that input_value read (Pparse: a binary
// AST file, a -ppx rewriter's output).  Tags follow the declaration order
// of parsing/parsetree.mli, asttypes.mli and longident.mli.
//
// What input_value shares stays shared: one node per marshaled block, one
// Location / Position record per block (their identity, Location::obj --
// the writers share by it), one Slice per marshaled list, one string per
// marshaled string (the Reader's), one identity per `Some` block.
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "cppcaml/typing/parsetree_ovalue.hpp"

namespace cppcaml::typing::parsetree {

namespace {

using V = const OValue*;

struct Corrupt : std::runtime_error {
  Corrupt() : std::runtime_error("Parsetree: ill-formed marshaled AST") {}
};

template <class D, class... A>
const D* mk(A&&... a) {
  return make<D>(D{{D::K}, std::forward<A>(a)...});
}

class Decoder {
 public:
  // ---- the value's shape ----
  static bool is_int(V x) { return x->kind == OValue::Kind::Int; }
  static long ival(V x) {
    if (!is_int(x)) throw Corrupt{};
    return x->i;
  }
  static unsigned tag(V x) {
    if (x->kind != OValue::Kind::Block) throw Corrupt{};
    return x->tag;
  }
  static V f(V x, std::size_t k) {
    if (x->kind != OValue::Kind::Block || k >= x->fields.size()) throw Corrupt{};
    return x->fields[k];
  }
  static std::string_view str(V x) {
    if (x->kind != OValue::Kind::String) throw Corrupt{};
    return x->s;
  }
  static bool boolean(V x) { return ival(x) != 0; }
  template <class E>
  static E flag(V x) {
    return static_cast<E>(ival(x));
  }

  // one node per marshaled block
  template <class T, class F>
  const T* node(V x, F&& build) {
    if (auto it = nodes_.find(x); it != nodes_.end()) return static_cast<const T*>(it->second);
    const T* r = build();
    nodes_[x] = r;
    return r;
  }
  // one Slice per marshaled list
  template <class T, class F>
  Slice<T> list(V x, F&& elt) {
    if (is_int(x)) return {};
    if (auto it = lists_.find(x); it != lists_.end())
      return Slice<T>{static_cast<const T*>(it->second.first), it->second.second};
    std::vector<T> out;
    for (V c = x; !is_int(c); c = f(c, 1)) out.push_back(elt(f(c, 0)));
    Slice<T> r = slice(out);
    lists_[x] = {static_cast<const void*>(r.p), r.n};
    return r;
  }
  template <class F>
  auto opt(V x, F&& elt) -> decltype(elt(x)) {
    if (is_int(x)) return nullptr;
    return elt(f(x, 0));
  }

  // ---- locations: one record per marshaled block ----
  Position position(V x) {
    if (x->pos) return *x->pos;  // the Reader's record (its identity)
    if (auto it = poss_.find(x); it != poss_.end()) return *it->second;
    auto* p = make<Position>(mkpos(str(f(x, 0)), ival(f(x, 1)), ival(f(x, 2)), ival(f(x, 3))));
    p->obj = p;
    poss_[x] = p;
    return *p;
  }
  Location loc(V x) {
    if (auto it = locs_.find(x); it != locs_.end()) return *it->second;
    auto* l = make<Location>(Location{position(f(x, 0)), position(f(x, 1)), boolean(f(x, 2))});
    l->obj = loc_record(*l);
    locs_[x] = l;
    return *l;
  }
  LocationStack locs(V x) {
    return list<Location>(x, [&](V l) { return loc(l); });
  }
  OptStr optstr(V x) {
    if (is_int(x)) return OptStr::none();
    auto [it, fresh] = somes_.try_emplace(x, nullptr);
    if (fresh) it->second = fresh_identity();
    return OptStr{true, str(f(x, 0)), it->second};
  }
  StrLoc strloc(V x) { return StrLoc{str(f(x, 0)), loc(f(x, 1))}; }
  OptStrLoc optstrloc(V x) { return OptStrLoc{optstr(f(x, 0)), loc(f(x, 1))}; }
  Slice<StrLoc> strlocs(V x) {
    return list<StrLoc>(x, [&](V s) { return strloc(s); });
  }
  Slice<std::string_view> strlist(V x) {
    return list<std::string_view>(x, [&](V s) { return str(s); });
  }
  Longident::t lident(V x) {
    return node<Longident>(x, [&]() -> const Longident* {
      auto* l = make<Longident>();
      switch (tag(x)) {
        case 0:
          l->kind = Longident::Kind::Lident;
          l->s = str(f(x, 0));
          break;
        case 1: {  // Ldot of t loc * string loc
          V a = f(x, 0), b = f(x, 1);
          auto* locs = static_cast<Location*>(zone().alloc(2 * sizeof(Location), alignof(Location)));
          l->kind = Longident::Kind::Ldot;
          l->l1 = lident(f(a, 0));
          new (locs) Location(loc(f(a, 1)));      // l1_loc
          l->s = str(f(b, 0));
          new (locs + 1) Location(loc(f(b, 1)));  // s_loc
          l->locs = locs;
          break;
        }
        case 2: {  // Lapply of t loc * t loc
          V a = f(x, 0), b = f(x, 1);
          auto* locs = static_cast<Location*>(zone().alloc(2 * sizeof(Location), alignof(Location)));
          l->kind = Longident::Kind::Lapply;
          l->l1 = lident(f(a, 0));
          new (locs) Location(loc(f(a, 1)));      // l1_loc
          l->l2 = lident(f(b, 0));
          new (locs + 1) Location(loc(f(b, 1)));  // l2_loc
          l->locs = locs;
          break;
        }
        default: throw Corrupt{};
      }
      return l;
    });
  }
  LidLoc lidloc(V x) { return LidLoc{lident(f(x, 0)), loc(f(x, 1))}; }
  ArgLabel arg_label(V x) {
    if (is_int(x)) return ArgLabel::nolabel();
    // one label object per marshaled block (a ppx may share the string)
    auto [it, fresh] = labels_.try_emplace(x, nullptr);
    if (fresh) it->second = ArgLabel::fresh_obj();
    return ArgLabel{tag(x) == 0 ? ArgLabel::Kind::Labelled : ArgLabel::Kind::Optional, str(f(x, 0)), it->second};
  }
  std::unordered_map<V, const void*> labels_;
  void char_opt(V x, bool& has, char& c) {
    has = !is_int(x);
    if (has) c = static_cast<char>(ival(f(x, 0)));
  }

  Constant constant(V x) {
    Constant c{};
    V d = f(x, 0);
    ConstantDesc& cd = c.pconst_desc;
    switch (tag(d)) {
      case 0:
        cd.kind = ConstantDesc::Kind::Pconst_integer;
        cd.s = str(f(d, 0));
        char_opt(f(d, 1), cd.has_suffix, cd.suffix);
        break;
      case 1:
        cd.kind = ConstantDesc::Kind::Pconst_char;
        cd.c = static_cast<char>(ival(f(d, 0)));
        break;
      case 2:
        cd.kind = ConstantDesc::Kind::Pconst_string;
        cd.s = str(f(d, 0));
        cd.str_loc = loc(f(d, 1));
        cd.delim = optstr(f(d, 2));
        break;
      case 3:
        cd.kind = ConstantDesc::Kind::Pconst_float;
        cd.s = str(f(d, 0));
        char_opt(f(d, 1), cd.has_suffix, cd.suffix);
        break;
      default: throw Corrupt{};
    }
    c.pconst_loc = loc(f(x, 1));
    return c;
  }

  // ---- attributes, extensions ----
  const Attribute* attribute(V x) {
    return node<Attribute>(x, [&] { return make<Attribute>(Attribute{strloc(f(x, 0)), payload(f(x, 1)), loc(f(x, 2))}); });
  }
  Attributes attrs(V x) {
    return list<const Attribute*>(x, [&](V a) { return attribute(a); });
  }
  const Extension* extension(V x) {
    return node<Extension>(x, [&] { return make<Extension>(Extension{strloc(f(x, 0)), payload(f(x, 1))}); });
  }
  Payload payload(V x) {
    Payload p{};
    switch (tag(x)) {
      case 0: p.kind = Payload::Kind::PStr; p.str = structure(f(x, 0)); break;
      case 1: p.kind = Payload::Kind::PSig; p.sig = signature(f(x, 0)); break;
      case 2: p.kind = Payload::Kind::PTyp; p.typ = core_type(f(x, 0)); break;
      case 3:
        p.kind = Payload::Kind::PPat;
        p.pat = pattern(f(x, 0));
        p.guard = opt(f(x, 1), [&](V e) { return expression(e); });
        break;
      default: throw Corrupt{};
    }
    return p;
  }

  // ---- type expressions ----
  const PackageType* package_type(V x) {
    return node<PackageType>(x, [&] {
      return make<PackageType>(PackageType{
          lidloc(f(x, 0)),
          list<std::pair<LidLoc, const CoreType*>>(
              f(x, 1), [&](V c) { return std::pair<LidLoc, const CoreType*>{lidloc(f(c, 0)), core_type(f(c, 1))}; }),
          loc(f(x, 2)), attrs(f(x, 3))});
    });
  }
  Slice<const CoreType*> core_types(V x) {
    return list<const CoreType*>(x, [&](V t) { return core_type(t); });
  }
  const CoreTypeDesc* core_type_desc(V x) {
    if (is_int(x)) {
      if (ival(x) != 0) throw Corrupt{};
      return mk<Ptyp_any>();
    }
    switch (tag(x)) {
      case 0: return mk<Ptyp_var>(str(f(x, 0)));
      case 1: return mk<Ptyp_arrow>(arg_label(f(x, 0)), core_type(f(x, 1)), core_type(f(x, 2)));
      case 2:
        return mk<Ptyp_tuple>(list<LabeledCoreType>(
            f(x, 0), [&](V e) { return LabeledCoreType{optstr(f(e, 0)), core_type(f(e, 1))}; }));
      case 3: return mk<Ptyp_constr>(lidloc(f(x, 0)), core_types(f(x, 1)));
      case 4:
        return mk<Ptyp_object>(list<const ObjectField*>(f(x, 0),
                                                        [&](V of) {
                                                          return node<ObjectField>(of, [&] {
                                                            V d = f(of, 0);
                                                            const ObjectFieldDesc* desc;
                                                            if (tag(d) == 0) desc = mk<Otag>(strloc(f(d, 0)), core_type(f(d, 1)));
                                                            else desc = mk<Oinherit>(core_type(f(d, 0)));
                                                            return make<ObjectField>(ObjectField{desc, loc(f(of, 1)), attrs(f(of, 2))});
                                                          });
                                                        }),
                               flag<ClosedFlag>(f(x, 1)));
      case 5: return mk<Ptyp_class>(lidloc(f(x, 0)), core_types(f(x, 1)));
      case 6: return mk<Ptyp_alias>(core_type(f(x, 0)), strloc(f(x, 1)));
      case 7: {
        Slice<const RowField*> fields = list<const RowField*>(f(x, 0), [&](V rf) {
          return node<RowField>(rf, [&] {
            V d = f(rf, 0);
            const RowFieldDesc* desc;
            if (tag(d) == 0) desc = mk<Rtag>(strloc(f(d, 0)), boolean(f(d, 1)), core_types(f(d, 2)));
            else desc = mk<Rinherit>(core_type(f(d, 0)));
            return make<RowField>(RowField{desc, loc(f(rf, 1)), attrs(f(rf, 2))});
          });
        });
        ClosedFlag closed = flag<ClosedFlag>(f(x, 1));
        V lo = f(x, 2);
        bool has = !is_int(lo);
        Slice<std::string_view> labels = has ? strlist(f(lo, 0)) : Slice<std::string_view>{};
        return mk<Ptyp_variant>(fields, closed, has, labels);
      }
      case 8: return mk<Ptyp_poly>(strlocs(f(x, 0)), core_type(f(x, 1)));
      case 9: return mk<Ptyp_package>(package_type(f(x, 0)));
      case 10: return mk<Ptyp_open>(lidloc(f(x, 0)), core_type(f(x, 1)));
      case 11: return mk<Ptyp_extension>(extension(f(x, 0)));
      case 12:
        return mk<Ptyp_functor>(arg_label(f(x, 0)), strloc(f(x, 1)), package_type(f(x, 2)), core_type(f(x, 3)));
    }
    throw Corrupt{};
  }
  const CoreType* core_type(V x) {
    return node<CoreType>(x, [&] {
      return make<CoreType>(CoreType{core_type_desc(f(x, 0)), loc(f(x, 1)), locs(f(x, 2)), attrs(f(x, 3))});
    });
  }

  // ---- patterns ----
  Slice<const Pattern*> patterns(V x) {
    return list<const Pattern*>(x, [&](V p) { return pattern(p); });
  }
  const PatternDesc* pattern_desc(V x) {
    if (is_int(x)) {
      if (ival(x) != 0) throw Corrupt{};
      return mk<Ppat_any>();
    }
    switch (tag(x)) {
      case 0: return mk<Ppat_var>(strloc(f(x, 0)));
      case 1: return mk<Ppat_alias>(pattern(f(x, 0)), strloc(f(x, 1)));
      case 2: return mk<Ppat_constant>(constant(f(x, 0)));
      case 3: return mk<Ppat_interval>(constant(f(x, 0)), constant(f(x, 1)));
      case 4:
        return mk<Ppat_tuple>(
            list<LabeledPattern>(f(x, 0), [&](V e) { return LabeledPattern{optstr(f(e, 0)), pattern(f(e, 1))}; }),
            flag<ClosedFlag>(f(x, 1)));
      case 5: {
        LidLoc lid = lidloc(f(x, 0));
        const ConstructArg* arg = opt(f(x, 1), [&](V a) {
          return node<ConstructArg>(a, [&] { return make<ConstructArg>(ConstructArg{strlocs(f(a, 0)), pattern(f(a, 1))}); });
        });
        return mk<Ppat_construct>(lid, arg);
      }
      case 6: return mk<Ppat_variant>(str(f(x, 0)), opt(f(x, 1), [&](V p) { return pattern(p); }));
      case 7:
        return mk<Ppat_record>(list<std::pair<LidLoc, const Pattern*>>(f(x, 0),
                                                                       [&](V e) {
                                                                         return std::pair<LidLoc, const Pattern*>{
                                                                             lidloc(f(e, 0)), pattern(f(e, 1))};
                                                                       }),
                               flag<ClosedFlag>(f(x, 1)));
      case 8: return mk<Ppat_array>(patterns(f(x, 0)));
      case 9: return mk<Ppat_or>(pattern(f(x, 0)), pattern(f(x, 1)));
      case 10: return mk<Ppat_constraint>(pattern(f(x, 0)), core_type(f(x, 1)));
      case 11: return mk<Ppat_type>(lidloc(f(x, 0)));
      case 12: return mk<Ppat_lazy>(pattern(f(x, 0)));
      case 13: return mk<Ppat_unpack>(optstrloc(f(x, 0)), opt(f(x, 1), [&](V p) { return package_type(p); }));
      case 14: return mk<Ppat_exception>(pattern(f(x, 0)));
      case 15: return mk<Ppat_effect>(pattern(f(x, 0)), pattern(f(x, 1)));
      case 16: return mk<Ppat_extension>(extension(f(x, 0)));
      case 17: return mk<Ppat_open>(lidloc(f(x, 0)), pattern(f(x, 1)));
    }
    throw Corrupt{};
  }
  const Pattern* pattern(V x) {
    return node<Pattern>(x, [&] {
      return make<Pattern>(Pattern{pattern_desc(f(x, 0)), loc(f(x, 1)), locs(f(x, 2)), attrs(f(x, 3))});
    });
  }

  // ---- expressions ----
  const Expression* opt_exp(V x) {
    return opt(x, [&](V e) { return expression(e); });
  }
  const CoreType* opt_ty(V x) {
    return opt(x, [&](V t) { return core_type(t); });
  }
  const Case* case_(V x) {
    return node<Case>(x, [&] { return make<Case>(Case{pattern(f(x, 0)), opt_exp(f(x, 1)), expression(f(x, 2))}); });
  }
  Slice<const Case*> cases(V x) {
    return list<const Case*>(x, [&](V c) { return case_(c); });
  }
  Slice<const ValueBinding*> value_bindings(V x) {
    return list<const ValueBinding*>(x, [&](V vb) { return value_binding(vb); });
  }
  const BindingOp* binding_op(V x) {
    return node<BindingOp>(x, [&] {
      return make<BindingOp>(BindingOp{strloc(f(x, 0)), pattern(f(x, 1)), expression(f(x, 2)), loc(f(x, 3))});
    });
  }
  Slice<ArgExpression> arg_expressions(V x) {
    return list<ArgExpression>(x, [&](V a) { return ArgExpression{arg_label(f(a, 0)), expression(f(a, 1))}; });
  }
  Slice<const Expression*> expressions(V x) {
    return list<const Expression*>(x, [&](V e) { return expression(e); });
  }
  const FunctionParam* function_param(V x) {
    return node<FunctionParam>(x, [&] {
      FunctionParam p{};
      p.pparam_loc = loc(f(x, 0));
      V d = f(x, 1);
      FunctionParamDesc& pd = p.pparam_desc;
      if (tag(d) == 0) {
        pd.kind = FunctionParamDesc::Kind::Pparam_val;
        pd.label = arg_label(f(d, 0));
        pd.default_ = opt_exp(f(d, 1));
        pd.pat = pattern(f(d, 2));
      } else {
        pd.kind = FunctionParamDesc::Kind::Pparam_newtype;
        pd.newtype = strloc(f(d, 0));
      }
      return make<FunctionParam>(p);
    });
  }
  const ExpressionDesc* expression_desc(V x) {
    if (is_int(x)) {
      if (ival(x) != 0) throw Corrupt{};
      return mk<Pexp_unreachable>();
    }
    switch (tag(x)) {
      case 0: return mk<Pexp_ident>(lidloc(f(x, 0)));
      case 1: return mk<Pexp_constant>(constant(f(x, 0)));
      case 2: return mk<Pexp_let>(flag<RecFlag>(f(x, 0)), value_bindings(f(x, 1)), expression(f(x, 2)));
      case 3: {
        Slice<const FunctionParam*> params =
            list<const FunctionParam*>(f(x, 0), [&](V p) { return function_param(p); });
        const TypeConstraint* cstr = opt(f(x, 1), [&](V c) -> const TypeConstraint* {
          return node<TypeConstraint>(c, [&] {
            TypeConstraint t{};
            if (tag(c) == 0) {
              t.kind = TypeConstraint::Kind::Pconstraint;
              t.ty = core_type(f(c, 0));
            } else {
              t.kind = TypeConstraint::Kind::Pcoerce;
              t.from = opt_ty(f(c, 0));
              t.ty = core_type(f(c, 1));
            }
            return make<TypeConstraint>(t);
          });
        });
        V b = f(x, 2);
        const FunctionBody* body = node<FunctionBody>(b, [&] {
          FunctionBody fb{};
          if (tag(b) == 0) {
            fb.kind = FunctionBody::Kind::Pfunction_body;
            fb.body = expression(f(b, 0));
          } else {
            fb.kind = FunctionBody::Kind::Pfunction_cases;
            fb.cases = cases(f(b, 0));
            fb.loc = loc(f(b, 1));
            fb.attrs = attrs(f(b, 2));
          }
          return make<FunctionBody>(fb);
        });
        return mk<Pexp_function>(params, cstr, body);
      }
      case 4: return mk<Pexp_apply>(expression(f(x, 0)), arg_expressions(f(x, 1)));
      case 5: return mk<Pexp_match>(expression(f(x, 0)), cases(f(x, 1)));
      case 6: return mk<Pexp_try>(expression(f(x, 0)), cases(f(x, 1)));
      case 7:
        return mk<Pexp_tuple>(list<LabeledExpression>(
            f(x, 0), [&](V e) { return LabeledExpression{optstr(f(e, 0)), expression(f(e, 1))}; }));
      case 8: return mk<Pexp_construct>(lidloc(f(x, 0)), opt_exp(f(x, 1)));
      case 9: return mk<Pexp_variant>(str(f(x, 0)), opt_exp(f(x, 1)));
      case 10:
        return mk<Pexp_record>(list<std::pair<LidLoc, const Expression*>>(f(x, 0),
                                                                          [&](V e) {
                                                                            return std::pair<LidLoc, const Expression*>{
                                                                                lidloc(f(e, 0)), expression(f(e, 1))};
                                                                          }),
                               opt_exp(f(x, 1)));
      case 11: return mk<Pexp_field>(expression(f(x, 0)), lidloc(f(x, 1)));
      case 12: return mk<Pexp_setfield>(expression(f(x, 0)), lidloc(f(x, 1)), expression(f(x, 2)));
      case 13: return mk<Pexp_array>(expressions(f(x, 0)));
      case 14: return mk<Pexp_ifthenelse>(expression(f(x, 0)), expression(f(x, 1)), opt_exp(f(x, 2)));
      case 15: return mk<Pexp_sequence>(expression(f(x, 0)), expression(f(x, 1)));
      case 16: return mk<Pexp_while>(expression(f(x, 0)), expression(f(x, 1)));
      case 17:
        return mk<Pexp_for>(pattern(f(x, 0)), expression(f(x, 1)), expression(f(x, 2)), flag<DirectionFlag>(f(x, 3)),
                            expression(f(x, 4)));
      case 18: return mk<Pexp_constraint>(expression(f(x, 0)), core_type(f(x, 1)));
      case 19: return mk<Pexp_coerce>(expression(f(x, 0)), opt_ty(f(x, 1)), core_type(f(x, 2)));
      case 20: return mk<Pexp_send>(expression(f(x, 0)), strloc(f(x, 1)));
      case 21: return mk<Pexp_new>(lidloc(f(x, 0)));
      case 22: return mk<Pexp_setinstvar>(strloc(f(x, 0)), expression(f(x, 1)));
      case 23:
        return mk<Pexp_override>(list<std::pair<StrLoc, const Expression*>>(
            f(x, 0), [&](V e) { return std::pair<StrLoc, const Expression*>{strloc(f(e, 0)), expression(f(e, 1))}; }));
      case 24: return mk<Pexp_struct_item>(structure_item(f(x, 0)), expression(f(x, 1)));
      case 25: return mk<Pexp_assert>(expression(f(x, 0)));
      case 26: return mk<Pexp_lazy>(expression(f(x, 0)));
      case 27: return mk<Pexp_poly>(expression(f(x, 0)), opt_ty(f(x, 1)));
      case 28: return mk<Pexp_object>(class_structure(f(x, 0)));
      case 29: return mk<Pexp_newtype>(strloc(f(x, 0)), expression(f(x, 1)));
      case 30: return mk<Pexp_pack>(module_expr(f(x, 0)), opt(f(x, 1), [&](V p) { return package_type(p); }));
      case 31: {
        V l = f(x, 0);
        const Letop* letop = node<Letop>(l, [&] {
          return make<Letop>(Letop{binding_op(f(l, 0)),
                                   list<const BindingOp*>(f(l, 1), [&](V b) { return binding_op(b); }),
                                   expression(f(l, 2))});
        });
        return mk<Pexp_letop>(letop);
      }
      case 32: return mk<Pexp_extension>(extension(f(x, 0)));
    }
    throw Corrupt{};
  }
  const Expression* expression(V x) {
    return node<Expression>(x, [&] {
      return make<Expression>(Expression{expression_desc(f(x, 0)), loc(f(x, 1)), locs(f(x, 2)), attrs(f(x, 3))});
    });
  }
  const ValueBinding* value_binding(V x) {
    return node<ValueBinding>(x, [&] {
      const ValueConstraint* cstr = opt(f(x, 2), [&](V c) -> const ValueConstraint* {
        return node<ValueConstraint>(c, [&] {
          ValueConstraint v{};
          if (tag(c) == 0) {
            v.kind = ValueConstraint::Kind::Pvc_constraint;
            v.locally_abstract_univars = strlocs(f(c, 0));
            v.typ = core_type(f(c, 1));
          } else {
            v.kind = ValueConstraint::Kind::Pvc_coercion;
            v.ground = opt_ty(f(c, 0));
            v.coercion = core_type(f(c, 1));
          }
          return make<ValueConstraint>(v);
        });
      });
      return make<ValueBinding>(ValueBinding{pattern(f(x, 0)), expression(f(x, 1)), cstr, attrs(f(x, 3)), loc(f(x, 4))});
    });
  }

  // ---- declarations ----
  Slice<TypeParam> type_params(V x) {
    return list<TypeParam>(x, [&](V p) {
      V vi = f(p, 1);
      return TypeParam{core_type(f(p, 0)), flag<Variance>(f(vi, 0)), flag<Injectivity>(f(vi, 1))};
    });
  }
  const LabelDeclaration* label_declaration(V x) {
    return node<LabelDeclaration>(x, [&] {
      return make<LabelDeclaration>(LabelDeclaration{strloc(f(x, 0)), flag<MutableFlag>(f(x, 1)), core_type(f(x, 2)),
                                                     loc(f(x, 3)), attrs(f(x, 4))});
    });
  }
  Slice<const LabelDeclaration*> label_declarations(V x) {
    return list<const LabelDeclaration*>(x, [&](V l) { return label_declaration(l); });
  }
  ConstructorArguments constructor_arguments(V x) {
    ConstructorArguments a{};
    if (tag(x) == 0) {
      a.kind = ConstructorArguments::Kind::Pcstr_tuple;
      a.tuple = core_types(f(x, 0));
    } else {
      a.kind = ConstructorArguments::Kind::Pcstr_record;
      a.record = label_declarations(f(x, 0));
    }
    return a;
  }
  const TypeDeclaration* type_declaration(V x) {
    return node<TypeDeclaration>(x, [&] {
      TypeDeclaration d{};
      d.ptype_name = strloc(f(x, 0));
      d.ptype_params = type_params(f(x, 1));
      d.ptype_constraints = list<TypeConstraintDecl>(
          f(x, 2), [&](V c) { return TypeConstraintDecl{core_type(f(c, 0)), core_type(f(c, 1)), loc(f(c, 2))}; });
      V k = f(x, 3);
      TypeKind& tk = d.ptype_kind;
      if (is_int(k)) {
        tk.kind = ival(k) == 0 ? TypeKind::Kind::Ptype_abstract : TypeKind::Kind::Ptype_open;
      } else {
        switch (tag(k)) {
          case 0:
            tk.kind = TypeKind::Kind::Ptype_variant;
            tk.constructors = list<const ConstructorDeclaration*>(f(k, 0), [&](V c) {
              return node<ConstructorDeclaration>(c, [&] {
                return make<ConstructorDeclaration>(ConstructorDeclaration{strloc(f(c, 0)), strlocs(f(c, 1)),
                                                                           constructor_arguments(f(c, 2)), opt_ty(f(c, 3)),
                                                                           loc(f(c, 4)), attrs(f(c, 5))});
              });
            });
            break;
          case 1:
            tk.kind = TypeKind::Kind::Ptype_record;
            tk.labels = label_declarations(f(k, 0));
            break;
          case 2:
            tk.kind = TypeKind::Kind::Ptype_external;
            tk.external = str(f(k, 0));
            break;
          default: throw Corrupt{};
        }
      }
      d.ptype_private = flag<PrivateFlag>(f(x, 4));
      d.ptype_manifest = opt_ty(f(x, 5));
      d.ptype_attributes = attrs(f(x, 6));
      d.ptype_loc = loc(f(x, 7));
      return make<TypeDeclaration>(d);
    });
  }
  Slice<const TypeDeclaration*> type_declarations(V x) {
    return list<const TypeDeclaration*>(x, [&](V d) { return type_declaration(d); });
  }
  const ExtensionConstructor* extension_constructor(V x) {
    return node<ExtensionConstructor>(x, [&] {
      ExtensionConstructorKind k{};
      V kv = f(x, 1);
      if (tag(kv) == 0) {
        k.kind = ExtensionConstructorKind::Kind::Pext_decl;
        k.vars = strlocs(f(kv, 0));
        k.args = constructor_arguments(f(kv, 1));
        k.res = opt_ty(f(kv, 2));
      } else {
        k.kind = ExtensionConstructorKind::Kind::Pext_rebind;
        k.rebind = lidloc(f(kv, 0));
      }
      return make<ExtensionConstructor>(ExtensionConstructor{strloc(f(x, 0)), k, loc(f(x, 2)), attrs(f(x, 3))});
    });
  }
  const TypeExtension* type_extension(V x) {
    return node<TypeExtension>(x, [&] {
      return make<TypeExtension>(TypeExtension{
          lidloc(f(x, 0)), type_params(f(x, 1)),
          list<const ExtensionConstructor*>(f(x, 2), [&](V e) { return extension_constructor(e); }),
          flag<PrivateFlag>(f(x, 3)), loc(f(x, 4)), attrs(f(x, 5))});
    });
  }
  const TypeException* type_exception(V x) {
    return node<TypeException>(x, [&] {
      return make<TypeException>(TypeException{extension_constructor(f(x, 0)), loc(f(x, 1)), attrs(f(x, 2))});
    });
  }
  const ValueDescription* value_description(V x) {
    return node<ValueDescription>(x, [&] {
      StrLoc name = strloc(f(x, 0));
      const CoreType* ty = core_type(f(x, 1));
      Slice<std::string_view> prims = strlist(f(x, 2));
      return make<ValueDescription>(ValueDescription{name, ty, prims, attrs(f(x, 3)), loc(f(x, 4))});
    });
  }

  // ---- classes ----
  const OpenDescription* open_description(V x) {
    return node<OpenDescription>(x, [&] {
      return make<OpenDescription>(
          OpenDescription{lidloc(f(x, 0)), flag<OverrideFlag>(f(x, 1)), loc(f(x, 2)), attrs(f(x, 3))});
    });
  }
  const ClassInfos<const ClassType*>* class_type_infos(V x) {
    return node<ClassInfos<const ClassType*>>(x, [&] {
      return make<ClassInfos<const ClassType*>>(ClassInfos<const ClassType*>{
          flag<VirtualFlag>(f(x, 0)), type_params(f(x, 1)), strloc(f(x, 2)), class_type(f(x, 3)), loc(f(x, 4)),
          attrs(f(x, 5))});
    });
  }
  const ClassType* class_type(V x) {
    return node<ClassType>(x, [&] {
      V d = f(x, 0);
      const ClassTypeDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Pcty_constr>(lidloc(f(d, 0)), core_types(f(d, 1))); break;
        case 1: {
          V s = f(d, 0);
          const ClassSignature* sign = node<ClassSignature>(s, [&] {
            Slice<const ClassTypeField*> fields = list<const ClassTypeField*>(f(s, 1), [&](V fv) {
              return node<ClassTypeField>(fv, [&] {
                V fd = f(fv, 0);
                const ClassTypeFieldDesc* fdesc = nullptr;
                switch (tag(fd)) {
                  case 0: fdesc = mk<Pctf_inherit>(class_type(f(fd, 0))); break;
                  case 1: {
                    V t = f(fd, 0);
                    fdesc = mk<Pctf_val>(strloc(f(t, 0)), flag<MutableFlag>(f(t, 1)), flag<VirtualFlag>(f(t, 2)),
                                         core_type(f(t, 3)));
                    break;
                  }
                  case 2: {
                    V t = f(fd, 0);
                    fdesc = mk<Pctf_method>(strloc(f(t, 0)), flag<PrivateFlag>(f(t, 1)), flag<VirtualFlag>(f(t, 2)),
                                            core_type(f(t, 3)));
                    break;
                  }
                  case 3: {
                    V t = f(fd, 0);
                    fdesc = mk<Pctf_constraint>(core_type(f(t, 0)), core_type(f(t, 1)));
                    break;
                  }
                  case 4: fdesc = mk<Pctf_attribute>(attribute(f(fd, 0))); break;
                  case 5: fdesc = mk<Pctf_extension>(extension(f(fd, 0))); break;
                  default: throw Corrupt{};
                }
                return make<ClassTypeField>(ClassTypeField{fdesc, loc(f(fv, 1)), attrs(f(fv, 2))});
              });
            });
            return make<ClassSignature>(ClassSignature{core_type(f(s, 0)), fields});
          });
          desc = mk<Pcty_signature>(sign);
          break;
        }
        case 2: desc = mk<Pcty_arrow>(arg_label(f(d, 0)), core_type(f(d, 1)), class_type(f(d, 2))); break;
        case 3: desc = mk<Pcty_extension>(extension(f(d, 0))); break;
        case 4: desc = mk<Pcty_open>(open_description(f(d, 0)), class_type(f(d, 1))); break;
        default: throw Corrupt{};
      }
      return make<ClassType>(ClassType{desc, loc(f(x, 1)), attrs(f(x, 2))});
    });
  }
  ClassFieldKind class_field_kind(V x) {
    ClassFieldKind k{};
    if (tag(x) == 0) {
      k.kind = ClassFieldKind::Kind::Cfk_virtual;
      k.ty = core_type(f(x, 0));
    } else {
      k.kind = ClassFieldKind::Kind::Cfk_concrete;
      k.ovr = flag<OverrideFlag>(f(x, 0));
      k.exp = expression(f(x, 1));
    }
    return k;
  }
  const ClassStructure* class_structure(V x) {
    return node<ClassStructure>(x, [&] {
      Slice<const ClassField*> fields = list<const ClassField*>(f(x, 1), [&](V fv) {
        return node<ClassField>(fv, [&] {
          V fd = f(fv, 0);
          const ClassFieldDesc* fdesc = nullptr;
          switch (tag(fd)) {
            case 0: {
              const StrLoc* as = opt(f(fd, 2), [&](V s) { return make<StrLoc>(strloc(s)); });
              fdesc = mk<Pcf_inherit>(flag<OverrideFlag>(f(fd, 0)), class_expr(f(fd, 1)), as);
              break;
            }
            case 1: {
              V t = f(fd, 0);
              fdesc = mk<Pcf_val>(strloc(f(t, 0)), flag<MutableFlag>(f(t, 1)), class_field_kind(f(t, 2)));
              break;
            }
            case 2: {
              V t = f(fd, 0);
              fdesc = mk<Pcf_method>(strloc(f(t, 0)), flag<PrivateFlag>(f(t, 1)), class_field_kind(f(t, 2)));
              break;
            }
            case 3: {
              V t = f(fd, 0);
              fdesc = mk<Pcf_constraint>(core_type(f(t, 0)), core_type(f(t, 1)));
              break;
            }
            case 4: fdesc = mk<Pcf_initializer>(expression(f(fd, 0))); break;
            case 5: fdesc = mk<Pcf_attribute>(attribute(f(fd, 0))); break;
            case 6: fdesc = mk<Pcf_extension>(extension(f(fd, 0))); break;
            default: throw Corrupt{};
          }
          return make<ClassField>(ClassField{fdesc, loc(f(fv, 1)), attrs(f(fv, 2))});
        });
      });
      return make<ClassStructure>(ClassStructure{pattern(f(x, 0)), fields});
    });
  }
  const ClassExpr* class_expr(V x) {
    return node<ClassExpr>(x, [&] {
      V d = f(x, 0);
      const ClassExprDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Pcl_constr>(lidloc(f(d, 0)), core_types(f(d, 1))); break;
        case 1: desc = mk<Pcl_structure>(class_structure(f(d, 0))); break;
        case 2:
          desc = mk<Pcl_fun>(arg_label(f(d, 0)), opt_exp(f(d, 1)), pattern(f(d, 2)), class_expr(f(d, 3)));
          break;
        case 3: desc = mk<Pcl_apply>(class_expr(f(d, 0)), arg_expressions(f(d, 1))); break;
        case 4: desc = mk<Pcl_let>(flag<RecFlag>(f(d, 0)), value_bindings(f(d, 1)), class_expr(f(d, 2))); break;
        case 5: desc = mk<Pcl_constraint>(class_expr(f(d, 0)), class_type(f(d, 1))); break;
        case 6: desc = mk<Pcl_extension>(extension(f(d, 0))); break;
        case 7: desc = mk<Pcl_open>(open_description(f(d, 0)), class_expr(f(d, 1))); break;
        default: throw Corrupt{};
      }
      return make<ClassExpr>(ClassExpr{desc, loc(f(x, 1)), attrs(f(x, 2))});
    });
  }

  // ---- modules ----
  FunctorParameter functor_parameter(V x) {
    if (is_int(x)) return FunctorParameter{true, {}, nullptr};
    return FunctorParameter{false, optstrloc(f(x, 0)), module_type(f(x, 1))};
  }
  const WithConstraint* with_constraint(V x) {
    return node<WithConstraint>(x, [&] {
      WithConstraint w{};
      w.lid = lidloc(f(x, 0));
      switch (tag(x)) {
        case 0: w.kind = WithConstraint::Kind::Pwith_type; w.decl = type_declaration(f(x, 1)); break;
        case 1: w.kind = WithConstraint::Kind::Pwith_module; w.lid2 = lidloc(f(x, 1)); break;
        case 2: w.kind = WithConstraint::Kind::Pwith_modtype; w.mty = module_type(f(x, 1)); break;
        case 3: w.kind = WithConstraint::Kind::Pwith_modtypesubst; w.mty = module_type(f(x, 1)); break;
        case 4: w.kind = WithConstraint::Kind::Pwith_typesubst; w.decl = type_declaration(f(x, 1)); break;
        case 5: w.kind = WithConstraint::Kind::Pwith_modsubst; w.lid2 = lidloc(f(x, 1)); break;
        default: throw Corrupt{};
      }
      return make<WithConstraint>(w);
    });
  }
  const ModuleType* module_type(V x) {
    return node<ModuleType>(x, [&] {
      V d = f(x, 0);
      const ModuleTypeDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Pmty_ident>(lidloc(f(d, 0))); break;
        case 1: desc = mk<Pmty_signature>(signature(f(d, 0))); break;
        case 2: desc = mk<Pmty_functor>(functor_parameter(f(d, 0)), module_type(f(d, 1))); break;
        case 3:
          desc = mk<Pmty_with>(module_type(f(d, 0)),
                               list<const WithConstraint*>(f(d, 1), [&](V c) { return with_constraint(c); }));
          break;
        case 4: desc = mk<Pmty_typeof>(module_expr(f(d, 0))); break;
        case 5: desc = mk<Pmty_extension>(extension(f(d, 0))); break;
        case 6: desc = mk<Pmty_alias>(lidloc(f(d, 0))); break;
        default: throw Corrupt{};
      }
      return make<ModuleType>(ModuleType{desc, loc(f(x, 1)), attrs(f(x, 2))});
    });
  }
  const ModuleExpr* module_expr(V x) {
    return node<ModuleExpr>(x, [&] {
      V d = f(x, 0);
      const ModuleExprDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Pmod_ident>(lidloc(f(d, 0))); break;
        case 1: desc = mk<Pmod_structure>(structure(f(d, 0))); break;
        case 2: desc = mk<Pmod_functor>(functor_parameter(f(d, 0)), module_expr(f(d, 1))); break;
        case 3: desc = mk<Pmod_apply>(module_expr(f(d, 0)), module_expr(f(d, 1))); break;
        case 4: desc = mk<Pmod_apply_unit>(module_expr(f(d, 0))); break;
        case 5: desc = mk<Pmod_constraint>(module_expr(f(d, 0)), module_type(f(d, 1))); break;
        case 6: desc = mk<Pmod_unpack>(expression(f(d, 0))); break;
        case 7: desc = mk<Pmod_extension>(extension(f(d, 0))); break;
        default: throw Corrupt{};
      }
      return make<ModuleExpr>(ModuleExpr{desc, loc(f(x, 1)), attrs(f(x, 2))});
    });
  }
  const ModuleDeclaration* module_declaration(V x) {
    return node<ModuleDeclaration>(x, [&] {
      return make<ModuleDeclaration>(
          ModuleDeclaration{optstrloc(f(x, 0)), module_type(f(x, 1)), attrs(f(x, 2)), loc(f(x, 3))});
    });
  }
  const ModuleTypeDeclaration* module_type_declaration(V x) {
    return node<ModuleTypeDeclaration>(x, [&] {
      return make<ModuleTypeDeclaration>(ModuleTypeDeclaration{
          strloc(f(x, 0)), opt(f(x, 1), [&](V m) { return module_type(m); }), attrs(f(x, 2)), loc(f(x, 3))});
    });
  }
  const ModuleBinding* module_binding(V x) {
    return node<ModuleBinding>(x, [&] {
      return make<ModuleBinding>(ModuleBinding{optstrloc(f(x, 0)), module_expr(f(x, 1)), attrs(f(x, 2)), loc(f(x, 3))});
    });
  }

  const SignatureItem* signature_item(V x) {
    return node<SignatureItem>(x, [&] {
      V d = f(x, 0);
      const SignatureItemDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Psig_value>(value_description(f(d, 0))); break;
        case 1: desc = mk<Psig_type>(flag<RecFlag>(f(d, 0)), type_declarations(f(d, 1))); break;
        case 2: desc = mk<Psig_typesubst>(type_declarations(f(d, 0))); break;
        case 3: desc = mk<Psig_typext>(type_extension(f(d, 0))); break;
        case 4: desc = mk<Psig_exception>(type_exception(f(d, 0))); break;
        case 5: desc = mk<Psig_module>(module_declaration(f(d, 0))); break;
        case 6: {
          V m = f(d, 0);
          const ModuleSubstitution* ms = node<ModuleSubstitution>(m, [&] {
            return make<ModuleSubstitution>(
                ModuleSubstitution{strloc(f(m, 0)), lidloc(f(m, 1)), attrs(f(m, 2)), loc(f(m, 3))});
          });
          desc = mk<Psig_modsubst>(ms);
          break;
        }
        case 7:
          desc = mk<Psig_recmodule>(
              list<const ModuleDeclaration*>(f(d, 0), [&](V m) { return module_declaration(m); }));
          break;
        case 8: desc = mk<Psig_modtype>(module_type_declaration(f(d, 0))); break;
        case 9: desc = mk<Psig_modtypesubst>(module_type_declaration(f(d, 0))); break;
        case 10: desc = mk<Psig_open>(open_description(f(d, 0))); break;
        case 11: {
          V i = f(d, 0);
          const IncludeDescription* incl = node<IncludeDescription>(i, [&] {
            return make<IncludeDescription>(IncludeDescription{module_type(f(i, 0)), loc(f(i, 1)), attrs(f(i, 2))});
          });
          desc = mk<Psig_include>(incl);
          break;
        }
        case 12:
          desc = mk<Psig_class>(
              list<const ClassDescription*>(f(d, 0), [&](V c) { return class_type_infos(c); }));
          break;
        case 13:
          desc = mk<Psig_class_type>(
              list<const ClassTypeDeclaration*>(f(d, 0), [&](V c) { return class_type_infos(c); }));
          break;
        case 14: desc = mk<Psig_attribute>(attribute(f(d, 0))); break;
        case 15: desc = mk<Psig_extension>(extension(f(d, 0)), attrs(f(d, 1))); break;
        default: throw Corrupt{};
      }
      return make<SignatureItem>(SignatureItem{desc, loc(f(x, 1))});
    });
  }
  Signature signature(V x) {
    return list<const SignatureItem*>(x, [&](V it) { return signature_item(it); });
  }

  const StructureItem* structure_item(V x) {
    return node<StructureItem>(x, [&] {
      V d = f(x, 0);
      const StructureItemDesc* desc = nullptr;
      switch (tag(d)) {
        case 0: desc = mk<Pstr_eval>(expression(f(d, 0)), attrs(f(d, 1))); break;
        case 1: desc = mk<Pstr_value>(flag<RecFlag>(f(d, 0)), value_bindings(f(d, 1))); break;
        case 2: desc = mk<Pstr_primitive>(value_description(f(d, 0))); break;
        case 3: desc = mk<Pstr_type>(flag<RecFlag>(f(d, 0)), type_declarations(f(d, 1))); break;
        case 4: desc = mk<Pstr_typext>(type_extension(f(d, 0))); break;
        case 5: desc = mk<Pstr_exception>(type_exception(f(d, 0))); break;
        case 6: desc = mk<Pstr_module>(module_binding(f(d, 0))); break;
        case 7:
          desc = mk<Pstr_recmodule>(list<const ModuleBinding*>(f(d, 0), [&](V m) { return module_binding(m); }));
          break;
        case 8: desc = mk<Pstr_modtype>(module_type_declaration(f(d, 0))); break;
        case 9: {
          V o = f(d, 0);
          const OpenDeclaration* od = node<OpenDeclaration>(o, [&] {
            return make<OpenDeclaration>(
                OpenDeclaration{module_expr(f(o, 0)), flag<OverrideFlag>(f(o, 1)), loc(f(o, 2)), attrs(f(o, 3))});
          });
          desc = mk<Pstr_open>(od);
          break;
        }
        case 10:
          desc = mk<Pstr_class>(list<const ClassDeclaration*>(f(d, 0), [&](V c) {
            return node<ClassDeclaration>(c, [&] {
              return make<ClassDeclaration>(ClassDeclaration{flag<VirtualFlag>(f(c, 0)), type_params(f(c, 1)),
                                                             strloc(f(c, 2)), class_expr(f(c, 3)), loc(f(c, 4)),
                                                             attrs(f(c, 5))});
            });
          }));
          break;
        case 11:
          desc = mk<Pstr_class_type>(
              list<const ClassTypeDeclaration*>(f(d, 0), [&](V c) { return class_type_infos(c); }));
          break;
        case 12: {
          V i = f(d, 0);
          const IncludeDeclaration* incl = node<IncludeDeclaration>(i, [&] {
            return make<IncludeDeclaration>(IncludeDeclaration{module_expr(f(i, 0)), loc(f(i, 1)), attrs(f(i, 2))});
          });
          desc = mk<Pstr_include>(incl);
          break;
        }
        case 13: desc = mk<Pstr_attribute>(attribute(f(d, 0))); break;
        case 14: desc = mk<Pstr_extension>(extension(f(d, 0)), attrs(f(d, 1))); break;
        default: throw Corrupt{};
      }
      return make<StructureItem>(StructureItem{desc, loc(f(x, 1))});
    });
  }
  Structure structure(V x) {
    return list<const StructureItem*>(x, [&](V it) { return structure_item(it); });
  }

 private:
  std::unordered_map<V, const void*> nodes_;
  std::unordered_map<V, std::pair<const void*, std::size_t>> lists_;
  std::unordered_map<V, Position*> poss_;
  std::unordered_map<V, Location*> locs_;
  std::unordered_map<V, const void*> somes_;
};

}  // namespace

Structure structure_of_ovalue(const OValue* v) {
  Decoder d;
  return d.structure(v);
}
Signature signature_of_ovalue(const OValue* v) {
  Decoder d;
  return d.signature(v);
}

}  // namespace cppcaml::typing::parsetree
