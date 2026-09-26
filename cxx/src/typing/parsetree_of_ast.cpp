// The C++ parser's ast:: -> the typer's parsetree:: (parsetree.hpp).
//
// ast:: is validated byte for byte against `ocamlc -dparsetree`, but it has
// its own representation (unique_ptr trees, std::variant) and it does not
// keep every location parser.mly records.  Fields it lacks are rebuilt the
// way parser.mly builds them where the information is there (an item's
// span for the declarations it wraps), and otherwise set to `gap_loc()`:
// Location.none marked with pos_cnum = -2, so the parsetree dump can print
// them as masked.  TYPECHECKER.md lists the gaps.
#include "cppcaml/typing/parsetree_ovalue.hpp"
#include <stdexcept>

#include "cppcaml/typing/parsetree.hpp"

namespace cppcaml::typing::parsetree {

typing::Attributes types_attributes(const Attributes& l) {
  std::vector<const typing::Attribute*> out;
  for (const Attribute* a : l) {
    bool doc = a->attr_name.txt == "ocaml.doc" || a->attr_name.txt == "ocaml.text";
    out.push_back(make<typing::Attribute>(
        typing::Attribute{a->attr_name.txt, a->attr_name.loc, ovalue_of_payload(a->attr_payload, doc), a->attr_loc, a}));
  }
  return slice(out);
}

Location gap_loc() {
  Location l = location::none();
  l.loc_start.pos_cnum = -2;
  l.loc_end.pos_cnum = -2;
  return l;
}
bool is_gap_loc(const Location& l) { return l.loc_start.pos_cnum == -2; }

namespace {

template <class T, class V, class F>
Slice<T> map_slice(const V& v, F&& f) {
  std::vector<T> out;
  out.reserve(v.size());
  for (auto& x : v) out.push_back(f(x));
  return slice(out);
}

struct Conv {
  std::string_view fname;
  const std::vector<std::string>& dirfiles;

  // ---- locations ----
  // One zone string per file name: the lexer's positions all carry the one
  // pos_fname string of their lexbuf (Location.init), which the .cmo's
  // Assert_failure / Match_failure literals share.
  mutable std::string_view zfname, znone;
  mutable std::vector<std::string_view> zdirfiles;
  Position pos(const ast::Position& p) const {
    std::string_view f;
    if (p.cnum == -1) {
      if (!znone.data()) znone = zborrow("_none_");
      f = znone;
    } else if (p.file_id > 0 && p.file_id <= static_cast<int>(dirfiles.size())) {
      if (zdirfiles.empty()) zdirfiles.resize(dirfiles.size());
      std::string_view& z = zdirfiles[p.file_id - 1];
      if (!z.data()) z = zborrow(dirfiles[p.file_id - 1]);
      f = z;
    } else {
      if (!zfname.data()) zfname = zborrow(fname);
      f = zfname;
    }
    return Position{f, p.lnum, p.bol, p.cnum};
  }
  Location loc(const ast::Location& l) const {
    return Location{pos(l.start), pos(l.end), l.ghost};
  }
  StrLoc str(const ast::StringLoc& s) const { return {zborrow(s.txt), loc(s.loc)}; }
  StrLoc str_gap(std::string_view s) const { return {zborrow(s), gap_loc()}; }
  OptStrLoc optstr(const ast::StrOptLoc& s) const {
    return {s.txt ? OptStr::of(*s.txt) : OptStr::none(), loc(s.loc)};
  }
  static OptStr optstr_of(const std::optional<std::string>& o) {
    return o ? OptStr::of(*o) : OptStr::none();
  }

  // Longident inner locations: a component location the parser did not
  // record (zero-initialized) is a gap.
  Location inner(const ast::Location& l) const {
    bool unset = l.start.cnum == 0 && l.end.cnum == 0 && l.start.lnum <= 1 && !l.ghost;
    return unset ? gap_loc() : loc(l);
  }
  Longident::t lid(const ast::Longident& l) const {
    if (auto* i = std::get_if<ast::Lident>(&l.v)) return Longident::lident(i->name);
    if (auto* d = std::get_if<ast::Ldot>(&l.v))
      return Longident::ldot(lid(*d->prefix), inner(d->prefix_loc), d->name, inner(d->name_loc));
    auto& a = std::get<ast::Lapply>(l.v);
    return Longident::lapply(lid(*a.f), inner(a.f_loc), lid(*a.x), inner(a.x_loc));
  }
  LidLoc lidloc(const ast::LongidentLoc& l) const { return {lid(l.txt), loc(l.loc)}; }

  static ArgLabel label(const ast::ArgLabel& l) {
    if (auto* p = std::get_if<ast::Labelled>(&l)) return ArgLabel::labelled(p->name);
    if (auto* p = std::get_if<ast::Optional>(&l)) return ArgLabel::optional(p->name);
    return ArgLabel::nolabel();
  }
  static ClosedFlag closed(ast::ClosedFlag c) {
    return c == ast::ClosedFlag::Closed ? ClosedFlag::Closed : ClosedFlag::Open;
  }
  static RecFlag rec(ast::RecFlag r) {
    return r == ast::RecFlag::Recursive ? RecFlag::Recursive : RecFlag::Nonrecursive;
  }
  static MutableFlag mut(ast::MutableFlag m) {
    return m == ast::MutableFlag::Mutable ? MutableFlag::Mutable : MutableFlag::Immutable;
  }
  static PrivateFlag priv(ast::PrivateFlag p) {
    return p == ast::PrivateFlag::Private ? PrivateFlag::Private : PrivateFlag::Public;
  }
  static VirtualFlag virt(ast::VirtualFlag v) {
    return v == ast::VirtualFlag::Virtual ? VirtualFlag::Virtual : VirtualFlag::Concrete;
  }
  static OverrideFlag ovr(ast::OverrideFlag o) {
    return o == ast::OverrideFlag::Override ? OverrideFlag::Override : OverrideFlag::Fresh;
  }

  // ---- attributes / payloads ----
  Payload payload_str(const ast::Structure& s) const {
    Payload p{Payload::Kind::PStr};
    p.str = structure(s);
    return p;
  }
  Payload ext_payload(const ast::ExtPayload& e) const {
    Payload p{};
    if (e.typ) {
      p.kind = Payload::Kind::PTyp;
      p.typ = core_type(*e.typ);
    } else if (e.pat) {
      p.kind = Payload::Kind::PPat;
      p.pat = pattern(*e.pat);
      if (e.guard) p.guard = expression(*e.guard);
    } else if (e.is_sig) {
      p.kind = Payload::Kind::PSig;
      p.sig = signature(*e.sig);
    } else {
      p.kind = Payload::Kind::PStr;
      p.str = structure(e.str);
    }
    return p;
  }
  const Attribute* attribute(const ast::Attribute& a) const {
    Payload p{};
    if (a.typ) {
      p.kind = Payload::Kind::PTyp;
      p.typ = core_type(*a.typ);
    } else if (a.pat) {
      p.kind = Payload::Kind::PPat;
      p.pat = pattern(*a.pat);
      if (a.guard) p.guard = expression(*a.guard);
    } else {
      p = payload_str(a.payload);
    }
    // docstrings.ml builds its ocaml.doc / ocaml.text attributes with a
    // Location.none name; the ast leaves that name location unset (zero).
    bool unset = a.name_loc.start.cnum == 0 && a.name_loc.end.cnum == 0;
    // ... and its attr_loc is the docstring's, which is its payload item's.
    Location al = loc(a.loc);
    if (a.loc.start.cnum == 0 && a.loc.end.cnum == 0 && p.kind == Payload::Kind::PStr &&
        !p.str.empty())
      al = p.str[0]->pstr_loc;
    return make<Attribute>(StrLoc{zborrow(a.name), unset ? location::none() : loc(a.name_loc)}, p, al);
  }
  Attributes attrs(const ast::Attributes& l) const {
    return map_slice<const Attribute*>(l, [&](const ast::Attribute& a) { return attribute(a); });
  }
  // Pstr_attribute / Psig_attribute / Pctf_attribute / Pcf_attribute: the
  // ast keeps only the name and the payload; the attribute spans the item.
  const Attribute* item_attribute(const std::string& name, const ast::Structure& payload,
                                  const Location& item_loc) const {
    return make<Attribute>(StrLoc{zborrow(name), gap_loc()}, payload_str(payload), item_loc);
  }
  const Extension* extension(const ast::ExtName& name, const ast::ExtPayload& p) const {
    return make<Extension>(StrLoc{zborrow(name), name.has_loc ? loc(name.loc) : gap_loc()}, ext_payload(p));
  }

  // ---- constants ----
  Constant constant(const ast::Constant& c) const {
    ConstantDesc d{};
    if (auto* i = std::get_if<ast::Pconst_integer>(&c.desc)) {
      d.kind = ConstantDesc::Kind::Pconst_integer;
      d.s = zborrow(i->value);
      d.has_suffix = i->suffix.has_value();
      d.suffix = i->suffix.value_or('\0');
    } else if (auto* ch = std::get_if<ast::Pconst_char>(&c.desc)) {
      d.kind = ConstantDesc::Kind::Pconst_char;
      d.c = static_cast<char>(ch->code);
    } else if (auto* s = std::get_if<ast::Pconst_string>(&c.desc)) {
      d.kind = ConstantDesc::Kind::Pconst_string;
      d.s = zborrow(s->s);
      d.str_loc = loc(s->strloc);
      d.delim = optstr_of(s->delim);
    } else {
      auto& f = std::get<ast::Pconst_float>(c.desc);
      d.kind = ConstantDesc::Kind::Pconst_float;
      d.s = zborrow(f.value);
      d.has_suffix = f.suffix.has_value();
      d.suffix = f.suffix.value_or('\0');
    }
    return Constant{d, loc(c.loc)};
  }

  // ---- core types ----
  const PackageType* package(const ast::Ptyp_package& p, const Location& fallback) const {
    auto cs = map_slice<std::pair<LidLoc, const CoreType*>>(p.constraints, [&](auto& c) {
      return std::make_pair(lidloc(c.first), core_type(*c.second));
    });
    // ppt_loc is the package's module_type span (parser.mly package_type_):
    // from the path to the last constraint, when the parser did not record it.
    (void)fallback;
    bool has_loc = p.loc.start.cnum != 0 || p.loc.end.cnum != 0;
    Location pl;
    if (has_loc) {
      pl = loc(p.loc);
    } else {
      pl = loc(p.path.loc);
      if (!cs.empty()) pl.loc_end = cs[cs.size() - 1].second->ptyp_loc.loc_end;
      pl.loc_ghost = false;
    }
    return make<PackageType>(lidloc(p.path), cs, pl, attrs(p.attrs));
  }
  Slice<const CoreType*> core_types(const std::vector<ast::CoreTypeBox>& l) const {
    return map_slice<const CoreType*>(l, [&](const ast::CoreTypeBox& t) { return core_type(*t); });
  }
  const CoreType* core_type(const ast::CoreType& t) const {
    Location l = loc(t.loc);
    const CoreTypeDesc* d = nullptr;
    using K = CoreTypeDesc::Kind;
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Ptyp_any>) {
            d = make<Ptyp_any>(K::Ptyp_any);
          } else if constexpr (std::is_same_v<T, ast::Ptyp_var>) {
            d = make<Ptyp_var>(Ptyp_var{{K::Ptyp_var}, zborrow(v.name)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_arrow>) {
            d = make<Ptyp_arrow>(Ptyp_arrow{{K::Ptyp_arrow}, label(v.label), core_type(*v.dom),
                                            core_type(*v.cod)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_tuple>) {
            std::vector<LabeledCoreType> tl;
            for (std::size_t k = 0; k < v.elems.size(); ++k)
              tl.push_back({k < v.labels.size() ? optstr_of(v.labels[k]) : OptStr::none(),
                            core_type(*v.elems[k])});
            d = make<Ptyp_tuple>(Ptyp_tuple{{K::Ptyp_tuple}, slice(tl)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_constr>) {
            d = make<Ptyp_constr>(Ptyp_constr{{K::Ptyp_constr}, lidloc(v.id), core_types(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_class>) {
            d = make<Ptyp_class>(Ptyp_class{{K::Ptyp_class}, lidloc(v.id), core_types(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_alias>) {
            d = make<Ptyp_alias>(Ptyp_alias{{K::Ptyp_alias}, core_type(*v.type), StrLoc{zborrow(v.name), loc(v.name_loc)}});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_poly>) {
            auto vars = map_slice<StrLoc>(v.vars, [&](const std::string& s) { return str_gap(s); });
            d = make<Ptyp_poly>(Ptyp_poly{{K::Ptyp_poly}, vars, core_type(*v.type)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_package>) {
            d = make<Ptyp_package>(Ptyp_package{{K::Ptyp_package}, package(v, l)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_open>) {
            d = make<Ptyp_open>(Ptyp_open{{K::Ptyp_open}, lidloc(v.mod_), core_type(*v.type)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_extension>) {
            d = make<Ptyp_extension>(Ptyp_extension{{K::Ptyp_extension}, extension(v.name, v.payload)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_functor>) {
            d = make<Ptyp_functor>(Ptyp_functor{{K::Ptyp_functor}, label(v.label), str(v.name),
                                                package(v.pkg, gap_loc()), core_type(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_variant>) {
            std::vector<const RowField*> rows;
            for (auto& r : v.rows) {
              if (auto* rt = std::get_if<ast::Rtag>(&r)) {
                auto* desc = make<Rtag>(Rtag{{RowFieldDesc::Kind::Rtag}, str_gap(rt->name),
                                             rt->constant, core_types(rt->types)});
                rows.push_back(make<RowField>(desc, gap_loc(), attrs(rt->attrs)));
              } else {
                auto& ri = std::get<ast::Rinherit>(r);
                auto* desc = make<Rinherit>(Rinherit{{RowFieldDesc::Kind::Rinherit}, core_type(*ri.ct)});
                rows.push_back(make<RowField>(desc, gap_loc(), Attributes{}));
              }
            }
            Slice<std::string_view> labels;
            if (v.labels)
              labels = map_slice<std::string_view>(*v.labels, [](const std::string& s) { return zborrow(s); });
            d = make<Ptyp_variant>(Ptyp_variant{{K::Ptyp_variant}, slice(rows), closed(v.closed),
                                                v.labels.has_value(), labels});
          } else if constexpr (std::is_same_v<T, ast::Ptyp_object>) {
            std::vector<const ObjectField*> fs;
            for (auto& f : v.fields) {
              if (auto* ot = std::get_if<ast::Otag>(&f)) {
                auto* desc = make<Otag>(Otag{{ObjectFieldDesc::Kind::Otag}, str(ot->name),
                                             core_type(*ot->type)});
                fs.push_back(make<ObjectField>(desc, gap_loc(), attrs(ot->attrs)));
              } else {
                auto& oi = std::get<ast::Oinherit>(f);
                auto* desc = make<Oinherit>(Oinherit{{ObjectFieldDesc::Kind::Oinherit}, core_type(*oi.type)});
                fs.push_back(make<ObjectField>(desc, gap_loc(), Attributes{}));
              }
            }
            d = make<Ptyp_object>(Ptyp_object{{K::Ptyp_object}, slice(fs), closed(v.closed)});
          }
        },
        t.desc);
    return make<CoreType>(d, l, LocationStack{}, attrs(t.attrs));
  }

  // ---- patterns ----
  Slice<const Pattern*> patterns(const std::vector<ast::PatBox>& l) const {
    return map_slice<const Pattern*>(l, [&](const ast::PatBox& p) { return pattern(*p); });
  }
  const Pattern* pattern(const ast::Pattern& p) const {
    using K = PatternDesc::Kind;
    const PatternDesc* d = nullptr;
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Ppat_any>) {
            d = make<Ppat_any>(K::Ppat_any);
          } else if constexpr (std::is_same_v<T, ast::Ppat_var>) {
            d = make<Ppat_var>(Ppat_var{{K::Ppat_var}, str(v.name)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_alias>) {
            d = make<Ppat_alias>(Ppat_alias{{K::Ppat_alias}, pattern(*v.p), str(v.name)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_constant>) {
            d = make<Ppat_constant>(Ppat_constant{{K::Ppat_constant}, constant(v.c)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_interval>) {
            d = make<Ppat_interval>(Ppat_interval{{K::Ppat_interval}, constant(v.c1), constant(v.c2)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_tuple>) {
            std::vector<LabeledPattern> pl;
            for (std::size_t k = 0; k < v.elems.size(); ++k)
              pl.push_back({k < v.labels.size() ? optstr_of(v.labels[k]) : OptStr::none(),
                            pattern(*v.elems[k])});
            d = make<Ppat_tuple>(Ppat_tuple{{K::Ppat_tuple}, slice(pl), closed(v.closed)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_construct>) {
            const ConstructArg* arg = nullptr;
            if (v.arg) {
              auto vars = map_slice<StrLoc>(v.vars, [&](const ast::StringLoc& s) { return str(s); });
              arg = make<ConstructArg>(vars, pattern(**v.arg));
            }
            d = make<Ppat_construct>(Ppat_construct{{K::Ppat_construct}, lidloc(v.id), arg});
          } else if constexpr (std::is_same_v<T, ast::Ppat_variant>) {
            d = make<Ppat_variant>(Ppat_variant{{K::Ppat_variant}, zborrow(v.label),
                                                v.arg ? pattern(**v.arg) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Ppat_record>) {
            auto fs = map_slice<std::pair<LidLoc, const Pattern*>>(v.fields, [&](auto& f) {
              return std::make_pair(lidloc(f.first), pattern(*f.second));
            });
            d = make<Ppat_record>(Ppat_record{{K::Ppat_record}, fs, closed(v.closed)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_array>) {
            d = make<Ppat_array>(Ppat_array{{K::Ppat_array}, patterns(v.elems)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_or>) {
            d = make<Ppat_or>(Ppat_or{{K::Ppat_or}, pattern(*v.l), pattern(*v.r)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_constraint>) {
            d = make<Ppat_constraint>(Ppat_constraint{{K::Ppat_constraint}, pattern(*v.p), core_type(*v.t)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_type>) {
            d = make<Ppat_type>(Ppat_type{{K::Ppat_type}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_lazy>) {
            d = make<Ppat_lazy>(Ppat_lazy{{K::Ppat_lazy}, pattern(*v.p)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_unpack>) {
            d = make<Ppat_unpack>(Ppat_unpack{{K::Ppat_unpack}, optstr(v.name),
                                              v.pkg ? package(*v.pkg, gap_loc()) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Ppat_exception>) {
            d = make<Ppat_exception>(Ppat_exception{{K::Ppat_exception}, pattern(*v.p)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_effect>) {
            d = make<Ppat_effect>(Ppat_effect{{K::Ppat_effect}, pattern(*v.eff), pattern(*v.cont)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_extension>) {
            d = make<Ppat_extension>(Ppat_extension{{K::Ppat_extension}, extension(v.name, v.payload)});
          } else if constexpr (std::is_same_v<T, ast::Ppat_open>) {
            d = make<Ppat_open>(Ppat_open{{K::Ppat_open}, lidloc(v.mod_), pattern(*v.p)});
          }
        },
        p.desc);
    return make<Pattern>(d, loc(p.loc), LocationStack{}, attrs(p.attrs));
  }

  // ---- expressions ----
  Slice<const Case*> cases(const std::vector<ast::Case>& cs) const {
    return map_slice<const Case*>(cs, [&](const ast::Case& c) {
      return make<Case>(pattern(c.lhs), c.guard ? expression(**c.guard) : nullptr,
                        expression(*c.rhs));
    });
  }
  Slice<ArgExpression> args(const std::vector<std::pair<ast::ArgLabel, ast::ExprBox>>& l) const {
    return map_slice<ArgExpression>(l, [&](auto& a) {
      return ArgExpression{label(a.first), expression(*a.second)};
    });
  }
  const ValueConstraint* value_constraint(const ast::ValueConstraint& vc) const {
    if (auto* c = std::get_if<ast::Pvc_constraint>(&vc)) {
      auto us = map_slice<StrLoc>(c->univars, [&](const ast::StringLoc& s) { return str(s); });
      return make<ValueConstraint>(ValueConstraint::Kind::Pvc_constraint, us, core_type(*c->typ));
    }
    auto& co = std::get<ast::Pvc_coercion>(vc);
    return make<ValueConstraint>(ValueConstraint::Kind::Pvc_coercion, Slice<StrLoc>{}, nullptr,
                                 co.ground ? core_type(**co.ground) : nullptr,
                                 core_type(*co.coercion));
  }
  // pvb_loc: the binding's span where the parser records it, else (gap) a
  // lone binding of a structure item's is the item's.
  Slice<const ValueBinding*> value_bindings(const std::vector<ast::ValueBinding>& l,
                                            const Location* lone_loc) const {
    return map_slice<const ValueBinding*>(l, [&](const ast::ValueBinding& vb) {
      Location vl = vb.loc ? loc(*vb.loc) : (lone_loc && l.size() == 1) ? *lone_loc : gap_loc();
      return make<ValueBinding>(pattern(vb.pat), expression(*vb.expr),
                                vb.constraint_ ? value_constraint(*vb.constraint_) : nullptr,
                                attrs(vb.attrs), vl);
    });
  }
  const BindingOp* binding_op(const ast::BindingOp& b) const {
    return make<BindingOp>(str(b.op), pattern(b.pat), expression(*b.exp), loc(b.loc));
  }
  const FunctionParam* function_param(const ast::FunctionParam& fp) const {
    if (auto* nt = std::get_if<ast::Pparam_newtype>(&fp.desc)) {
      FunctionParamDesc d{FunctionParamDesc::Kind::Pparam_newtype};
      d.newtype = str(nt->name);
      return make<FunctionParam>(loc(nt->loc), d);
    }
    auto& pv = std::get<ast::Pparam_val>(fp.desc);
    FunctionParamDesc d{FunctionParamDesc::Kind::Pparam_val};
    d.label = label(pv.label);
    d.default_ = pv.default_ ? expression(**pv.default_) : nullptr;
    d.pat = pattern(pv.pat);
    return make<FunctionParam>(loc(pv.loc), d);
  }
  const Expression* expression(const ast::Expression& e) const {
    using K = ExpressionDesc::Kind;
    const ExpressionDesc* d = nullptr;
    Location l = loc(e.loc);
    LocationStack stack;
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pexp_ident>) {
            d = make<Pexp_ident>(Pexp_ident{{K::Pexp_ident}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_constant>) {
            d = make<Pexp_constant>(Pexp_constant{{K::Pexp_constant}, constant(v.c)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_let>) {
            d = make<Pexp_let>(Pexp_let{{K::Pexp_let}, rec(v.rf), value_bindings(v.bindings, nullptr),
                                        expression(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_function>) {
            auto ps = map_slice<const FunctionParam*>(v.params, [&](const ast::FunctionParam& fp) {
              return function_param(fp);
            });
            const TypeConstraint* tc = nullptr;
            if (v.constraint_) {
              if (auto* pc = std::get_if<ast::Pconstraint>(&*v.constraint_))
                tc = make<TypeConstraint>(TypeConstraint::Kind::Pconstraint, core_type(*pc->type));
              else {
                auto& co = std::get<ast::Pcoerce>(*v.constraint_);
                tc = make<TypeConstraint>(TypeConstraint::Kind::Pcoerce, core_type(*co.to_),
                                          co.from ? core_type(**co.from) : nullptr);
              }
            }
            FunctionBody* fb;
            if (auto* b = std::get_if<ast::Pfunction_body>(&v.body->v)) {
              fb = make<FunctionBody>(FunctionBody::Kind::Pfunction_body, expression(*b->e));
            } else {
              auto& c = std::get<ast::Pfunction_cases>(v.body->v);
              fb = make<FunctionBody>(FunctionBody::Kind::Pfunction_cases, nullptr, cases(c.cases),
                                      loc(c.loc), attrs(c.attrs));
            }
            d = make<Pexp_function>(Pexp_function{{K::Pexp_function}, ps, tc, fb});
          } else if constexpr (std::is_same_v<T, ast::Pexp_apply>) {
            d = make<Pexp_apply>(Pexp_apply{{K::Pexp_apply}, expression(*v.fn), args(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_match>) {
            d = make<Pexp_match>(Pexp_match{{K::Pexp_match}, expression(*v.e), cases(v.cases)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_try>) {
            d = make<Pexp_try>(Pexp_try{{K::Pexp_try}, expression(*v.e), cases(v.cases)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_tuple>) {
            std::vector<LabeledExpression> el;
            for (std::size_t k = 0; k < v.elems.size(); ++k)
              el.push_back({k < v.labels.size() ? optstr_of(v.labels[k]) : OptStr::none(),
                            expression(*v.elems[k])});
            d = make<Pexp_tuple>(Pexp_tuple{{K::Pexp_tuple}, slice(el)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_construct>) {
            d = make<Pexp_construct>(Pexp_construct{{K::Pexp_construct}, lidloc(v.id),
                                                    v.arg ? expression(**v.arg) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_variant>) {
            d = make<Pexp_variant>(Pexp_variant{{K::Pexp_variant}, zborrow(v.label),
                                                v.arg ? expression(**v.arg) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_record>) {
            auto fs = map_slice<std::pair<LidLoc, const Expression*>>(v.fields, [&](auto& f) {
              return std::make_pair(lidloc(f.first), expression(*f.second));
            });
            d = make<Pexp_record>(Pexp_record{{K::Pexp_record}, fs,
                                              v.base ? expression(**v.base) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_field>) {
            d = make<Pexp_field>(Pexp_field{{K::Pexp_field}, expression(*v.e), lidloc(v.field)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_setfield>) {
            d = make<Pexp_setfield>(Pexp_setfield{{K::Pexp_setfield}, expression(*v.obj),
                                                  lidloc(v.field), expression(*v.value)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_array>) {
            auto el = map_slice<const Expression*>(v.elems, [&](const ast::ExprBox& x) {
              return expression(*x);
            });
            d = make<Pexp_array>(Pexp_array{{K::Pexp_array}, el});
          } else if constexpr (std::is_same_v<T, ast::Pexp_ifthenelse>) {
            d = make<Pexp_ifthenelse>(Pexp_ifthenelse{{K::Pexp_ifthenelse}, expression(*v.cond),
                                                      expression(*v.then_),
                                                      v.else_ ? expression(**v.else_) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_sequence>) {
            d = make<Pexp_sequence>(Pexp_sequence{{K::Pexp_sequence}, expression(*v.e1), expression(*v.e2)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_while>) {
            d = make<Pexp_while>(Pexp_while{{K::Pexp_while}, expression(*v.cond), expression(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_for>) {
            d = make<Pexp_for>(Pexp_for{{K::Pexp_for}, pattern(v.var), expression(*v.lo),
                                        expression(*v.hi),
                                        v.dir == ast::DirectionFlag::Upto ? DirectionFlag::Upto
                                                                          : DirectionFlag::Downto,
                                        expression(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_constraint>) {
            d = make<Pexp_constraint>(Pexp_constraint{{K::Pexp_constraint}, expression(*v.e), core_type(*v.t)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_coerce>) {
            d = make<Pexp_coerce>(Pexp_coerce{{K::Pexp_coerce}, expression(*v.e),
                                              v.from ? core_type(**v.from) : nullptr, core_type(*v.to_)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_send>) {
            d = make<Pexp_send>(Pexp_send{{K::Pexp_send}, expression(*v.obj), str(v.meth)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_new>) {
            d = make<Pexp_new>(Pexp_new{{K::Pexp_new}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_setinstvar>) {
            d = make<Pexp_setinstvar>(Pexp_setinstvar{{K::Pexp_setinstvar}, str(v.name), expression(*v.value)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_override>) {
            auto fs = map_slice<std::pair<StrLoc, const Expression*>>(v.fields, [&](auto& f) {
              return std::make_pair(str(f.first), expression(*f.second));
            });
            d = make<Pexp_override>(Pexp_override{{K::Pexp_override}, fs});
          } else if constexpr (std::is_same_v<T, ast::Pexp_struct_item>) {
            d = make<Pexp_struct_item>(Pexp_struct_item{{K::Pexp_struct_item}, structure_item(*v.item),
                                                        expression(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_assert>) {
            // The parser keeps the innermost location of the `assert` node
            // (pexp_loc_stack's last element, all Typecore reads).
            Location kw = loc(v.kw_loc);
            // (a relocation widens the span; an equal span means none)
            if (!(kw.loc_start.pos_cnum == l.loc_start.pos_cnum &&
                  kw.loc_end.pos_cnum == l.loc_end.pos_cnum))
              stack = slice({kw});
            d = make<Pexp_assert>(Pexp_assert{{K::Pexp_assert}, expression(*v.e)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_lazy>) {
            d = make<Pexp_lazy>(Pexp_lazy{{K::Pexp_lazy}, expression(*v.e)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_poly>) {
            d = make<Pexp_poly>(Pexp_poly{{K::Pexp_poly}, expression(*v.e), v.t ? core_type(**v.t) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_object>) {
            d = make<Pexp_object>(Pexp_object{{K::Pexp_object}, class_structure(*v.cs)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_newtype>) {
            d = make<Pexp_newtype>(Pexp_newtype{{K::Pexp_newtype}, str(v.name), expression(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_pack>) {
            d = make<Pexp_pack>(Pexp_pack{{K::Pexp_pack}, module_expr(*v.me),
                                          v.pkg ? package(*v.pkg, gap_loc()) : nullptr});
          } else if constexpr (std::is_same_v<T, ast::Pexp_letop>) {
            auto ands = map_slice<const BindingOp*>(v.ands, [&](const ast::BindingOp& b) {
              return binding_op(b);
            });
            d = make<Pexp_letop>(Pexp_letop{{K::Pexp_letop},
                                            make<Letop>(binding_op(v.let_), ands, expression(*v.body))});
          } else if constexpr (std::is_same_v<T, ast::Pexp_extension>) {
            d = make<Pexp_extension>(Pexp_extension{{K::Pexp_extension}, extension(v.name, v.payload)});
          } else if constexpr (std::is_same_v<T, ast::Pexp_unreachable>) {
            d = make<Pexp_unreachable>(K::Pexp_unreachable);
          }
        },
        e.desc);
    return make<Expression>(d, l, stack, attrs(e.attrs));
  }

  // ---- type declarations ----
  static TypeParam type_param(const CoreType* t, int v) {
    Variance var;
    if (v & 16) return TypeParam{t, Variance::Bivariant,
                                 (v & 8) ? Injectivity::Injective : Injectivity::NoInjectivity};
    switch (v & 7) {
      case 1: var = Variance::Covariant; break;
      case 6: var = Variance::Contravariant; break;
      default: var = Variance::NoVariance; break;
    }
    return TypeParam{t, var, (v & 8) ? Injectivity::Injective : Injectivity::NoInjectivity};
  }
  Slice<TypeParam> type_params(const std::vector<ast::CoreTypeBox>& ps,
                               const std::vector<int>& vs) const {
    std::vector<TypeParam> out;
    for (std::size_t k = 0; k < ps.size(); ++k)
      out.push_back(type_param(core_type(*ps[k]), k < vs.size() ? vs[k] : 7));
    return slice(out);
  }
  const LabelDeclaration* label_decl(const ast::LabelDecl& d) const {
    return make<LabelDeclaration>(str(d.name), mut(d.mut), core_type(*d.type), loc(d.loc), attrs(d.attrs));
  }
  ConstructorArguments ctor_args(const ast::ConstructorArguments& a) const {
    ConstructorArguments r{};
    if (auto* t = std::get_if<ast::Pcstr_tuple>(&a)) {
      r.kind = ConstructorArguments::Kind::Pcstr_tuple;
      r.tuple = core_types(t->elems);
    } else {
      r.kind = ConstructorArguments::Kind::Pcstr_record;
      r.record = map_slice<const LabelDeclaration*>(std::get<ast::Pcstr_record>(a).fields,
                                                    [&](const ast::LabelDecl& d) { return label_decl(d); });
    }
    return r;
  }
  const TypeDeclaration* type_declaration(const ast::TypeDeclaration& d) const {
    TypeKind k{};
    if (std::holds_alternative<ast::Ptype_abstract>(d.kind)) {
      k.kind = TypeKind::Kind::Ptype_abstract;
    } else if (std::holds_alternative<ast::Ptype_open>(d.kind)) {
      k.kind = TypeKind::Kind::Ptype_open;
    } else if (auto* e = std::get_if<ast::Ptype_external>(&d.kind)) {
      k.kind = TypeKind::Kind::Ptype_external;
      k.external = zborrow(e->s);
    } else if (auto* v = std::get_if<ast::Ptype_variant>(&d.kind)) {
      k.kind = TypeKind::Kind::Ptype_variant;
      k.constructors = map_slice<const ConstructorDeclaration*>(v->ctors, [&](const ast::ConstructorDecl& c) {
        auto vars = map_slice<StrLoc>(c.vars, [&](const std::string& s) { return str_gap(s); });
        return make<ConstructorDeclaration>(str(c.name), vars, ctor_args(c.args),
                                            c.res ? core_type(**c.res) : nullptr, loc(c.loc),
                                            attrs(c.attrs));
      });
    } else {
      k.kind = TypeKind::Kind::Ptype_record;
      k.labels = map_slice<const LabelDeclaration*>(std::get<ast::Ptype_record>(d.kind).fields,
                                                    [&](const ast::LabelDecl& ld) { return label_decl(ld); });
    }
    auto cs = map_slice<TypeConstraintDecl>(d.constraints, [&](const ast::TypeConstraint& c) {
      return TypeConstraintDecl{core_type(*c.t1), core_type(*c.t2), loc(c.loc)};
    });
    return make<TypeDeclaration>(str(d.name), type_params(d.params, d.written_variances), cs, k,
                                 priv(d.priv), d.manifest ? core_type(**d.manifest) : nullptr,
                                 attrs(d.attrs), loc(d.loc));
  }
  Slice<const TypeDeclaration*> type_declarations(const std::vector<ast::TypeDeclaration>& l) const {
    return map_slice<const TypeDeclaration*>(l, [&](const ast::TypeDeclaration& d) {
      return type_declaration(d);
    });
  }
  const ExtensionConstructor* extension_constructor(const ast::ExtensionConstructor& c) const {
    ExtensionConstructorKind k{};
    if (auto* d = std::get_if<ast::Pext_decl>(&c.kind)) {
      k.kind = ExtensionConstructorKind::Kind::Pext_decl;
      k.vars = map_slice<StrLoc>(d->vars, [&](const std::string& s) { return str_gap(s); });
      k.args = ctor_args(d->args);
      k.res = d->res ? core_type(**d->res) : nullptr;
    } else {
      k.kind = ExtensionConstructorKind::Kind::Pext_rebind;
      k.rebind = lidloc(std::get<ast::Pext_rebind>(c.kind).id);
    }
    return make<ExtensionConstructor>(str(c.name), k, loc(c.loc), attrs(c.attrs));
  }
  const TypeExtension* type_extension(const ast::TypeExtension& x, const Location& item) const {
    auto cs = map_slice<const ExtensionConstructor*>(x.ctors, [&](const ast::ExtensionConstructor& c) {
      return extension_constructor(c);
    });
    return make<TypeExtension>(lidloc(x.path), type_params(x.params, {}), cs, priv(x.priv), item,
                               attrs(x.attrs));
  }
  // ptyexn_loc: parser.mly gives the exception the constructor's location.
  const TypeException* type_exception(const ast::TypeException& x, const Location&) const {
    const ExtensionConstructor* c = extension_constructor(x.ctor);
    return make<TypeException>(c, c->pext_loc, attrs(x.attrs));
  }
  const ValueDescription* value_description(const ast::ValueDescription& v) const {
    return make<ValueDescription>(str(v.name), core_type(*v.type), attrs(v.attrs), loc(v.loc));
  }
  const PrimitiveDescription* primitive(const ast::PrimitiveDescription& p) const {
    PrimitiveKind k{};
    if (p.alias) {
      k.kind = PrimitiveKind::Kind::Pprim_alias;
      k.ty = p.type ? core_type(*p.type) : nullptr;
      if (p.alias_lid) {
        k.alias = lidloc(*p.alias_lid);
      } else {  // `( op )`: an operator name
        k.alias = LidLoc{Longident::lident(p.alias->txt), loc(p.alias->loc)};
      }
    } else {
      k.kind = PrimitiveKind::Kind::Pprim_decl;
      k.ty = core_type(*p.type);
      k.prims = map_slice<std::string_view>(p.prims, [](const std::string& s) { return zborrow(s); });
    }
    return make<PrimitiveDescription>(str(p.name), k, attrs(p.attrs), loc(p.loc));
  }

  // ---- class language ----
  const OpenDescription* open_description(ast::OverrideFlag o, const ast::LongidentLoc& id,
                                          const Location& l, const Attributes& at) const {
    return make<OpenDescription>(lidloc(id), ovr(o), l, at);
  }
  const ClassType* class_type(const ast::ClassType& x) const {
    using K = ClassTypeDesc::Kind;
    const ClassTypeDesc* d = nullptr;
    Location l = loc(x.loc);
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pcty_constr>) {
            d = make<Pcty_constr>(Pcty_constr{{K::Pcty_constr}, lidloc(v.id), core_types(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Pcty_signature>) {
            d = make<Pcty_signature>(Pcty_signature{{K::Pcty_signature}, class_signature(v.cs)});
          } else if constexpr (std::is_same_v<T, ast::Pcty_arrow>) {
            d = make<Pcty_arrow>(Pcty_arrow{{K::Pcty_arrow}, label(v.label), core_type(*v.dom), class_type(*v.cod)});
          } else if constexpr (std::is_same_v<T, ast::Pcty_open>) {
            d = make<Pcty_open>(Pcty_open{{K::Pcty_open}, open_description(v.ovr, v.id, loc(v.open_loc), {}),
                                          class_type(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pcty_extension>) {
            d = make<Pcty_extension>(Pcty_extension{{K::Pcty_extension}, extension(v.name, v.payload)});
          }
        },
        x.desc);
    return make<ClassType>(d, l, attrs(x.attrs));
  }
  const ClassSignature* class_signature(const ast::ClassSignature& cs) const {
    auto fs = map_slice<const ClassTypeField*>(cs.fields, [&](const ast::ClassTypeField& f) {
      using K = ClassTypeFieldDesc::Kind;
      const ClassTypeFieldDesc* d = nullptr;
      Location l = loc(f.loc);
      std::visit(
          [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, ast::Pctf_inherit>) {
              d = make<Pctf_inherit>(Pctf_inherit{{K::Pctf_inherit}, class_type(*v.ct)});
            } else if constexpr (std::is_same_v<T, ast::Pctf_val>) {
              d = make<Pctf_val>(Pctf_val{{K::Pctf_val}, str(v.name), mut(v.mut), virt(v.virt), core_type(*v.type)});
            } else if constexpr (std::is_same_v<T, ast::Pctf_method>) {
              d = make<Pctf_method>(Pctf_method{{K::Pctf_method}, str(v.name), priv(v.priv), virt(v.virt),
                                                core_type(*v.type)});
            } else if constexpr (std::is_same_v<T, ast::Pctf_constraint>) {
              d = make<Pctf_constraint>(Pctf_constraint{{K::Pctf_constraint}, core_type(*v.t1), core_type(*v.t2)});
            } else if constexpr (std::is_same_v<T, ast::Pctf_attribute>) {
              d = make<Pctf_attribute>(Pctf_attribute{{K::Pctf_attribute}, item_attribute(v.name, v.payload, l)});
            } else if constexpr (std::is_same_v<T, ast::Pctf_extension>) {
              d = make<Pctf_extension>(Pctf_extension{{K::Pctf_extension}, extension(v.name, v.payload)});
            }
          },
          f.desc);
      return make<ClassTypeField>(d, l, attrs(f.attrs));
    });
    return make<ClassSignature>(core_type(*cs.self), fs);
  }
  const ClassExpr* class_expr(const ast::ClassExpr& x) const {
    using K = ClassExprDesc::Kind;
    const ClassExprDesc* d = nullptr;
    Location l = loc(x.loc);
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pcl_constr>) {
            d = make<Pcl_constr>(Pcl_constr{{K::Pcl_constr}, lidloc(v.id), core_types(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_structure>) {
            d = make<Pcl_structure>(Pcl_structure{{K::Pcl_structure}, class_structure(v.cs)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_fun>) {
            d = make<Pcl_fun>(Pcl_fun{{K::Pcl_fun}, label(v.label),
                                      v.default_ ? expression(**v.default_) : nullptr, pattern(v.pat),
                                      class_expr(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_apply>) {
            d = make<Pcl_apply>(Pcl_apply{{K::Pcl_apply}, class_expr(*v.ce), args(v.args)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_let>) {
            d = make<Pcl_let>(Pcl_let{{K::Pcl_let}, rec(v.rf), value_bindings(v.bindings, nullptr),
                                      class_expr(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_constraint>) {
            d = make<Pcl_constraint>(Pcl_constraint{{K::Pcl_constraint}, class_expr(*v.ce), class_type(*v.ct)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_open>) {
            d = make<Pcl_open>(Pcl_open{{K::Pcl_open}, open_description(v.ovr, v.id, loc(v.open_loc), {}),
                                        class_expr(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pcl_extension>) {
            d = make<Pcl_extension>(Pcl_extension{{K::Pcl_extension}, extension(v.name, v.payload)});
          }
        },
        x.desc);
    return make<ClassExpr>(d, l, attrs(x.attrs));
  }
  ClassFieldKind class_field_kind(const ast::ClassFieldKind& k) const {
    ClassFieldKind r{};
    if (auto* c = std::get_if<ast::Cfk_concrete>(&k)) {
      r.kind = ClassFieldKind::Kind::Cfk_concrete;
      r.ovr = ovr(c->ovr);
      r.exp = expression(*c->e);
    } else {
      r.kind = ClassFieldKind::Kind::Cfk_virtual;
      r.ty = core_type(*std::get<ast::Cfk_virtual>(k).type);
    }
    return r;
  }
  const ClassStructure* class_structure(const ast::ClassStructure& cs) const {
    auto fs = map_slice<const ClassField*>(cs.fields, [&](const ast::ClassField& f) {
      using K = ClassFieldDesc::Kind;
      const ClassFieldDesc* d = nullptr;
      Location l = loc(f.loc);
      std::visit(
          [&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, ast::Pcf_inherit>) {
              const StrLoc* as = v.as_ ? make<StrLoc>(str(*v.as_)) : nullptr;
              d = make<Pcf_inherit>(Pcf_inherit{{K::Pcf_inherit}, ovr(v.ovr), class_expr(*v.ce), as});
            } else if constexpr (std::is_same_v<T, ast::Pcf_val>) {
              d = make<Pcf_val>(Pcf_val{{K::Pcf_val}, str(v.name), mut(v.mut), class_field_kind(v.kind)});
            } else if constexpr (std::is_same_v<T, ast::Pcf_method>) {
              d = make<Pcf_method>(Pcf_method{{K::Pcf_method}, str(v.name), priv(v.priv), class_field_kind(v.kind)});
            } else if constexpr (std::is_same_v<T, ast::Pcf_constraint>) {
              d = make<Pcf_constraint>(Pcf_constraint{{K::Pcf_constraint}, core_type(*v.t1), core_type(*v.t2)});
            } else if constexpr (std::is_same_v<T, ast::Pcf_initializer>) {
              d = make<Pcf_initializer>(Pcf_initializer{{K::Pcf_initializer}, expression(*v.e)});
            } else if constexpr (std::is_same_v<T, ast::Pcf_attribute>) {
              d = make<Pcf_attribute>(Pcf_attribute{{K::Pcf_attribute}, item_attribute(v.name, v.payload, l)});
            } else if constexpr (std::is_same_v<T, ast::Pcf_extension>) {
              d = make<Pcf_extension>(Pcf_extension{{K::Pcf_extension}, extension(v.name, v.payload)});
            }
          },
          f.desc);
      return make<ClassField>(d, l, attrs(f.attrs));
    });
    return make<ClassStructure>(pattern(cs.self), fs);
  }
  template <class A, class D, class F>
  const ClassInfos<A>* class_infos(const D& x, F&& expr) const {
    Slice<TypeParam> ps = type_params(x.params, {});
    return make<ClassInfos<A>>(virt(x.virt), ps, str(x.name), expr(x.expr), loc(x.loc), attrs(x.attrs));
  }

  // ---- module language ----
  FunctorParameter functor_param(const ast::FunctorParam& p) const {
    if (std::holds_alternative<ast::Functor_unit>(p)) return FunctorParameter{true, {}, nullptr};
    auto& n = std::get<ast::Functor_named>(p);
    return FunctorParameter{false, optstr(n.name), module_type(*n.type)};
  }
  const ModuleType* module_type(const ast::ModuleType& m) const {
    using K = ModuleTypeDesc::Kind;
    const ModuleTypeDesc* d = nullptr;
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pmty_ident>) {
            d = make<Pmty_ident>(Pmty_ident{{K::Pmty_ident}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Pmty_signature>) {
            d = make<Pmty_signature>(Pmty_signature{{K::Pmty_signature}, signature(v.items)});
          } else if constexpr (std::is_same_v<T, ast::Pmty_functor>) {
            d = make<Pmty_functor>(Pmty_functor{{K::Pmty_functor}, functor_param(v.param), module_type(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pmty_with>) {
            auto cs = map_slice<const WithConstraint*>(v.constraints, [&](const ast::WithConstraint& w) {
              return with_constraint(w);
            });
            d = make<Pmty_with>(Pmty_with{{K::Pmty_with}, module_type(*v.mt), cs});
          } else if constexpr (std::is_same_v<T, ast::Pmty_typeof>) {
            d = make<Pmty_typeof>(Pmty_typeof{{K::Pmty_typeof}, module_expr(*v.me)});
          } else if constexpr (std::is_same_v<T, ast::Pmty_alias>) {
            d = make<Pmty_alias>(Pmty_alias{{K::Pmty_alias}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Pmty_extension>) {
            d = make<Pmty_extension>(Pmty_extension{{K::Pmty_extension}, extension(v.name, v.payload)});
          }
        },
        m.desc);
    return make<ModuleType>(d, loc(m.loc), attrs(m.attrs));
  }
  const WithConstraint* with_constraint(const ast::WithConstraint& w) const {
    using K = WithConstraint::Kind;
    WithConstraint* r = make<WithConstraint>(K::Pwith_type);
    if (auto* p = std::get_if<ast::Pwith_type>(&w)) {
      r->kind = K::Pwith_type; r->lid = lidloc(p->lid); r->decl = type_declaration(*p->td);
    } else if (auto* p = std::get_if<ast::Pwith_typesubst>(&w)) {
      r->kind = K::Pwith_typesubst; r->lid = lidloc(p->lid); r->decl = type_declaration(*p->td);
    } else if (auto* p = std::get_if<ast::Pwith_module>(&w)) {
      r->kind = K::Pwith_module; r->lid = lidloc(p->lid1); r->lid2 = lidloc(p->lid2);
    } else if (auto* p = std::get_if<ast::Pwith_modsubst>(&w)) {
      r->kind = K::Pwith_modsubst; r->lid = lidloc(p->lid1); r->lid2 = lidloc(p->lid2);
    } else if (auto* p = std::get_if<ast::Pwith_modtype>(&w)) {
      r->kind = K::Pwith_modtype; r->lid = lidloc(p->lid); r->mty = module_type(*p->mty);
    } else {
      auto& p2 = std::get<ast::Pwith_modtypesubst>(w);
      r->kind = K::Pwith_modtypesubst; r->lid = lidloc(p2.lid); r->mty = module_type(*p2.mty);
    }
    return r;
  }
  const ModuleExpr* module_expr(const ast::ModuleExpr& m) const {
    using K = ModuleExprDesc::Kind;
    const ModuleExprDesc* d = nullptr;
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pmod_ident>) {
            d = make<Pmod_ident>(Pmod_ident{{K::Pmod_ident}, lidloc(v.id)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_structure>) {
            d = make<Pmod_structure>(Pmod_structure{{K::Pmod_structure}, structure(v.items)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_functor>) {
            d = make<Pmod_functor>(Pmod_functor{{K::Pmod_functor}, functor_param(v.param), module_expr(*v.body)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_constraint>) {
            d = make<Pmod_constraint>(Pmod_constraint{{K::Pmod_constraint}, module_expr(*v.me), module_type(*v.mt)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_apply>) {
            d = make<Pmod_apply>(Pmod_apply{{K::Pmod_apply}, module_expr(*v.f), module_expr(*v.arg)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_apply_unit>) {
            d = make<Pmod_apply_unit>(Pmod_apply_unit{{K::Pmod_apply_unit}, module_expr(*v.f)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_unpack>) {
            d = make<Pmod_unpack>(Pmod_unpack{{K::Pmod_unpack}, expression(*v.e)});
          } else if constexpr (std::is_same_v<T, ast::Pmod_extension>) {
            d = make<Pmod_extension>(Pmod_extension{{K::Pmod_extension}, extension(v.name, v.payload)});
          }
        },
        m.desc);
    return make<ModuleExpr>(d, loc(m.loc), attrs(m.attrs));
  }
  // A module binding / declaration keeps its own span in a `module rec`
  // group only; a plain one spans its item (see ast.hpp).
  Location own_or(const ast::Location& own, const Location& item) const {
    bool set = own.start.cnum != 0 || own.end.cnum != 0;
    return set ? loc(own) : item;
  }
  const ModuleDeclaration* module_declaration(const ast::ModuleDeclaration& md,
                                              const Location& item) const {
    return make<ModuleDeclaration>(optstr(md.name), module_type(*md.type), attrs(md.attrs),
                                   own_or(md.loc, item));
  }
  const ModuleBinding* module_binding(const ast::ModuleBinding& mb, const Location& item) const {
    return make<ModuleBinding>(optstr(mb.name), module_expr(mb.expr), attrs(mb.attrs),
                               own_or(mb.loc, item));
  }

  const SignatureItem* signature_item(const ast::SignatureItem& s) const {
    using K = SignatureItemDesc::Kind;
    const SignatureItemDesc* d = nullptr;
    // Under `%ext` the item is ghost (wrap_mksig_ext's ghsig); the
    // declarations it wraps keep their own non-ghost span.
    Location l = loc(s.loc);
    l.loc_ghost = false;
    Location item_loc = loc(s.loc);
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Psig_value>) {
            d = make<Psig_value>(Psig_value{{K::Psig_value}, value_description(v.vd)});
          } else if constexpr (std::is_same_v<T, ast::Psig_primitive>) {
            d = make<Psig_primitive>(Psig_primitive{{K::Psig_primitive}, primitive(v.pd)});
          } else if constexpr (std::is_same_v<T, ast::Psig_type>) {
            d = make<Psig_type>(Psig_type{{K::Psig_type}, rec(v.rf), type_declarations(v.decls)});
          } else if constexpr (std::is_same_v<T, ast::Psig_typesubst>) {
            d = make<Psig_typesubst>(Psig_typesubst{{K::Psig_typesubst}, type_declarations(v.decls)});
          } else if constexpr (std::is_same_v<T, ast::Psig_typext>) {
            d = make<Psig_typext>(Psig_typext{{K::Psig_typext}, type_extension(v.ext, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_exception>) {
            d = make<Psig_exception>(Psig_exception{{K::Psig_exception}, type_exception(v.exn, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_module>) {
            d = make<Psig_module>(Psig_module{{K::Psig_module}, module_declaration(v.md, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_recmodule>) {
            auto mds = map_slice<const ModuleDeclaration*>(v.decls, [&](const ast::ModuleDeclaration& md) {
              return module_declaration(md, l);
            });
            d = make<Psig_recmodule>(Psig_recmodule{{K::Psig_recmodule}, mds});
          } else if constexpr (std::is_same_v<T, ast::Psig_modtype>) {
            d = make<Psig_modtype>(Psig_modtype{{K::Psig_modtype},
                make<ModuleTypeDeclaration>(str(v.name), v.type ? module_type(*v.type) : nullptr,
                                            attrs(v.attrs), l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_modtypesubst>) {
            d = make<Psig_modtypesubst>(Psig_modtypesubst{{K::Psig_modtypesubst},
                make<ModuleTypeDeclaration>(str(v.name), module_type(v.type), Attributes{}, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_modsubst>) {
            // pms_name is a string loc (not an option)
            d = make<Psig_modsubst>(Psig_modsubst{{K::Psig_modsubst},
                make<ModuleSubstitution>(StrLoc{zborrow(v.name.txt.value_or("_")), loc(v.name.loc)},
                                         lidloc(v.manifest), Attributes{}, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_open>) {
            d = make<Psig_open>(Psig_open{{K::Psig_open}, open_description(v.ovr, v.id, l, attrs(v.attrs))});
          } else if constexpr (std::is_same_v<T, ast::Psig_include>) {
            d = make<Psig_include>(Psig_include{{K::Psig_include},
                make<IncludeDescription>(module_type(v.mt), l, attrs(v.attrs))});
          } else if constexpr (std::is_same_v<T, ast::Psig_class>) {
            auto ds = map_slice<const ClassDescription*>(v.decls, [&](const ast::ClassTypeDeclaration& x) {
              return class_infos<const ClassType*>(x, [&](const ast::ClassType& ct) { return class_type(ct); });
            });
            d = make<Psig_class>(Psig_class{{K::Psig_class}, ds});
          } else if constexpr (std::is_same_v<T, ast::Psig_class_type>) {
            auto ds = map_slice<const ClassTypeDeclaration*>(v.decls, [&](const ast::ClassTypeDeclaration& x) {
              return class_infos<const ClassType*>(x, [&](const ast::ClassType& ct) { return class_type(ct); });
            });
            d = make<Psig_class_type>(Psig_class_type{{K::Psig_class_type}, ds});
          } else if constexpr (std::is_same_v<T, ast::Psig_attribute>) {
            d = make<Psig_attribute>(Psig_attribute{{K::Psig_attribute}, item_attribute(v.name, v.payload, l)});
          } else if constexpr (std::is_same_v<T, ast::Psig_extension>) {
            d = make<Psig_extension>(Psig_extension{{K::Psig_extension}, extension(v.name, v.payload),
                                                    attrs(v.attrs)});
          }
        },
        s.desc);
    return make<SignatureItem>(d, item_loc);
  }
  Signature signature(const ast::Signature& s) const {
    return map_slice<const SignatureItem*>(s, [&](const ast::SignatureItem& it) { return signature_item(it); });
  }

  const StructureItem* structure_item(const ast::StructureItem& s) const {
    using K = StructureItemDesc::Kind;
    const StructureItemDesc* d = nullptr;
    // Under `%ext` the item is ghost (wrap_mkstr_ext's ghstr); the
    // declarations it wraps keep their own non-ghost span.
    Location l = loc(s.loc);
    l.loc_ghost = false;
    Location item_loc = loc(s.loc);
    std::visit(
        [&](auto& v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, ast::Pstr_eval>) {
            d = make<Pstr_eval>(Pstr_eval{{K::Pstr_eval}, expression(*v.e), attrs(v.attrs)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_value>) {
            d = make<Pstr_value>(Pstr_value{{K::Pstr_value}, rec(v.rf), value_bindings(v.bindings, &l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_val>) {
            d = make<Pstr_val>(Pstr_val{{K::Pstr_val}, value_description(v.vd)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_primitive>) {
            d = make<Pstr_primitive>(Pstr_primitive{{K::Pstr_primitive}, primitive(v.prim)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_type>) {
            d = make<Pstr_type>(Pstr_type{{K::Pstr_type}, rec(v.rf), type_declarations(v.decls)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_typext>) {
            d = make<Pstr_typext>(Pstr_typext{{K::Pstr_typext}, type_extension(v.ext, l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_exception>) {
            d = make<Pstr_exception>(Pstr_exception{{K::Pstr_exception}, type_exception(v.exn, l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_module>) {
            d = make<Pstr_module>(Pstr_module{{K::Pstr_module}, module_binding(v.binding, l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_recmodule>) {
            auto mbs = map_slice<const ModuleBinding*>(v.bindings, [&](const ast::ModuleBinding& mb) {
              return module_binding(mb, l);
            });
            d = make<Pstr_recmodule>(Pstr_recmodule{{K::Pstr_recmodule}, mbs});
          } else if constexpr (std::is_same_v<T, ast::Pstr_modtype>) {
            d = make<Pstr_modtype>(Pstr_modtype{{K::Pstr_modtype},
                make<ModuleTypeDeclaration>(str(v.name), v.type ? module_type(*v.type) : nullptr,
                                            attrs(v.attrs), l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_open>) {
            // `M.(e)` (open_dot_declaration): Str.open_ gives the item no
            // location, and the open spans the module path.
            const ModuleExpr* me = module_expr(v.expr);
            bool dotted = s.loc.start.cnum == -1;
            d = make<Pstr_open>(Pstr_open{{K::Pstr_open},
                make<OpenDeclaration>(me, ovr(v.ovr), dotted ? me->pmod_loc : l, attrs(v.attrs))});
          } else if constexpr (std::is_same_v<T, ast::Pstr_class>) {
            auto ds = map_slice<const ClassDeclaration*>(v.decls, [&](const ast::ClassDeclaration& x) {
              return class_infos<const ClassExpr*>(x, [&](const ast::ClassExpr& ce) { return class_expr(ce); });
            });
            d = make<Pstr_class>(Pstr_class{{K::Pstr_class}, ds});
          } else if constexpr (std::is_same_v<T, ast::Pstr_class_type>) {
            auto ds = map_slice<const ClassTypeDeclaration*>(v.decls, [&](const ast::ClassTypeDeclaration& x) {
              return class_infos<const ClassType*>(x, [&](const ast::ClassType& ct) { return class_type(ct); });
            });
            d = make<Pstr_class_type>(Pstr_class_type{{K::Pstr_class_type}, ds});
          } else if constexpr (std::is_same_v<T, ast::Pstr_include>) {
            d = make<Pstr_include>(Pstr_include{{K::Pstr_include},
                make<IncludeDeclaration>(module_expr(v.expr), l, attrs(v.attrs))});
          } else if constexpr (std::is_same_v<T, ast::Pstr_attribute>) {
            d = make<Pstr_attribute>(Pstr_attribute{{K::Pstr_attribute}, item_attribute(v.name, v.payload, l)});
          } else if constexpr (std::is_same_v<T, ast::Pstr_extension>) {
            d = make<Pstr_extension>(Pstr_extension{{K::Pstr_extension}, extension(v.name, v.payload),
                                                    attrs(v.attrs)});
          }
        },
        s.desc);
    return make<StructureItem>(d, item_loc);
  }
  Structure structure(const ast::Structure& s) const {
    return map_slice<const StructureItem*>(s, [&](const ast::StructureItem& it) { return structure_item(it); });
  }
};

}  // namespace

Structure of_ast(const ast::Structure& s, std::string_view fname,
                 const std::vector<std::string>& dirfiles) {
  Conv c{zborrow(fname), dirfiles};
  return c.structure(s);
}

Signature of_ast_signature(const ast::Signature& s, std::string_view fname,
                           const std::vector<std::string>& dirfiles) {
  Conv c{zborrow(fname), dirfiles};
  return c.signature(s);
}

}  // namespace cppcaml::typing::parsetree
