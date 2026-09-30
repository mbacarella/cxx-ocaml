// Port of typing/oprint.ml (cxx/PORTING.md stage 9): types, class types,
// module types and signature items.
#include "cppcaml/typing/oprint.hpp"

#include "cppcaml/typing/utf8_lexeme.hpp"

#include <functional>

#include "cppcaml/typing/path.hpp"

namespace cppcaml::typing {

namespace outcometree {

const OutIdent* oide_ident(OutName* n) {
  auto* i = make<OutIdent>();
  i->k = OutIdent::K::Oide_ident;
  i->name = n;
  return i;
}
const OutIdent* oide_dot(const OutIdent* p, std::string s) {
  auto* i = make<OutIdent>();
  i->k = OutIdent::K::Oide_dot;
  i->a = p;
  i->s = std::move(s);
  return i;
}
const OutIdent* oide_apply(const OutIdent* f, const OutIdent* x) {
  auto* i = make<OutIdent>();
  i->k = OutIdent::K::Oide_apply;
  i->a = f;
  i->b = x;
  return i;
}
OutName* out_name_create(std::string s) {
  auto* n = make<OutName>();
  n->printed_name = std::move(s);
  return n;
}
OutType* otyp(OutType::K k) {
  auto* t = make<OutType>();
  t->k = k;
  return t;
}

namespace {
bool eq_ident(const OutIdent* a, const OutIdent* b) {
  if (a == b) return true;
  if (!a || !b || a->k != b->k) return false;
  switch (a->k) {
    case OutIdent::K::Oide_ident: return a->name->printed_name == b->name->printed_name;
    case OutIdent::K::Oide_dot: return a->s == b->s && eq_ident(a->a, b->a);
    case OutIdent::K::Oide_apply: return eq_ident(a->a, b->a) && eq_ident(a->b, b->b);
  }
  return false;
}
bool eq_list(const std::vector<const OutType*>& a, const std::vector<const OutType*>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (!equal(a[i], b[i])) return false;
  return true;
}
bool eq_pack(const OutPackage* a, const OutPackage* b) {
  if (a == b) return true;
  if (!a || !b || !eq_ident(a->opack_path, b->opack_path) || a->opack_constraints.size() != b->opack_constraints.size())
    return false;
  for (std::size_t i = 0; i < a->opack_constraints.size(); ++i)
    if (a->opack_constraints[i].first != b->opack_constraints[i].first ||
        !equal(a->opack_constraints[i].second, b->opack_constraints[i].second))
      return false;
  return true;
}
bool eq_label(const ArgLabel& a, const ArgLabel& b) { return a.k == b.k && a.s == b.s; }
bool eq_constr(const OutConstructor& a, const OutConstructor& b) {
  return a.ocstr_name == b.ocstr_name && eq_list(a.ocstr_args, b.ocstr_args) &&
         equal(a.ocstr_return_type, b.ocstr_return_type);
}
}  // namespace

bool equal(const OutType* a, const OutType* b) {
  if (a == b) return true;
  if (!a || !b || a->k != b->k) return false;
  using K = OutType::K;
  switch (a->k) {
    case K::Otyp_abstract:
    case K::Otyp_open: return true;
    case K::Otyp_alias: return a->non_gen == b->non_gen && equal(a->t1, b->t1) && a->s == b->s;
    case K::Otyp_arrow: return eq_label(a->label, b->label) && equal(a->t1, b->t1) && equal(a->t2, b->t2);
    case K::Otyp_class:
    case K::Otyp_constr: return eq_ident(a->id, b->id) && eq_list(a->args, b->args);
    case K::Otyp_manifest: return equal(a->t1, b->t1) && equal(a->t2, b->t2);
    case K::Otyp_object:
      if (a->fields.size() != b->fields.size() || a->row.k != b->row.k || !equal(a->row.ty, b->row.ty)) return false;
      for (std::size_t i = 0; i < a->fields.size(); ++i)
        if (a->fields[i].first != b->fields[i].first || !equal(a->fields[i].second, b->fields[i].second)) return false;
      return true;
    case K::Otyp_record:
      if (a->labels.size() != b->labels.size()) return false;
      for (std::size_t i = 0; i < a->labels.size(); ++i) {
        const auto &x = a->labels[i], &y = b->labels[i];
        if (x.olab_name != y.olab_name || x.olab_mutable != y.olab_mutable || x.olab_atomic != y.olab_atomic ||
            !equal(x.olab_type, y.olab_type))
          return false;
      }
      return true;
    case K::Otyp_stuff:
    case K::Otyp_external: return a->s == b->s;
    case K::Otyp_sum:
      if (a->constrs.size() != b->constrs.size()) return false;
      for (std::size_t i = 0; i < a->constrs.size(); ++i)
        if (!eq_constr(a->constrs[i], b->constrs[i])) return false;
      return true;
    case K::Otyp_tuple:
      if (a->tuple.size() != b->tuple.size()) return false;
      for (std::size_t i = 0; i < a->tuple.size(); ++i)
        if (a->tuple[i].first != b->tuple[i].first || !equal(a->tuple[i].second, b->tuple[i].second)) return false;
      return true;
    case K::Otyp_var: return a->non_gen == b->non_gen && a->s == b->s;
    case K::Otyp_variant: {
      if (a->closed != b->closed || a->tags != b->tags || a->variant.is_typ != b->variant.is_typ) return false;
      if (a->variant.is_typ) return equal(a->variant.typ, b->variant.typ);
      if (a->variant.fields.size() != b->variant.fields.size()) return false;
      for (std::size_t i = 0; i < a->variant.fields.size(); ++i) {
        const auto &x = a->variant.fields[i], &y = b->variant.fields[i];
        if (x.label != y.label || x.amp != y.amp || !eq_list(x.tys, y.tys)) return false;
      }
      return true;
    }
    case K::Otyp_poly: return a->vars == b->vars && equal(a->t1, b->t1);
    case K::Otyp_module: return eq_pack(a->pack, b->pack);
    case K::Otyp_attribute: return equal(a->t1, b->t1) && a->attr.oattr_name == b->attr.oattr_name;
    case K::Otyp_functor:
      return eq_label(a->label, b->label) && eq_ident(a->id, b->id) && eq_pack(a->pack, b->pack) &&
             equal(a->t1, b->t1);
  }
  return false;
}

}  // namespace outcometree

namespace oprint {

using namespace format_doc;
using OT = ot::OutType::K;

// ---- identifiers ----

void print_lident(Formatter& ppf, std::string_view s) {
  if (s == "::")
    pp_print_string(ppf, "(::)");
  else if (path::is_keyword(s))
    fprintf(ppf, "\\#%s", s);
  else
    pp_print_string(ppf, s);
}

void out_ident(Formatter& ppf, const ot::OutIdent* id) {
  switch (id->k) {
    case ot::OutIdent::K::Oide_ident: print_lident(ppf, id->name->printed_name); break;
    case ot::OutIdent::K::Oide_dot:
      out_ident(ppf, id->a);
      pp_print_char(ppf, '.');
      print_lident(ppf, id->s);
      break;
    case ot::OutIdent::K::Oide_apply:
      fprintf(ppf, "%a(%a)", pr(out_ident, id->a), pr(out_ident, id->b));
      break;
  }
}


bool is_valid_identifier(std::string_view s) { return utf8_lexeme::is_valid_identifier(s); }

bool parenthesized_ident(std::string_view name) {
  for (const char* k : {"or", "mod", "land", "lor", "lxor", "lsl", "lsr", "asr"})
    if (name == k) return true;
  return !is_valid_identifier(name);
}

void value_ident(Formatter& ppf, std::string_view name) {
  if (parenthesized_ident(name))
    fprintf(ppf, "( %s )", name);
  else if (path::is_keyword(name))
    fprintf(ppf, "\\#%s", name);
  else
    pp_print_string(ppf, name);
}

// ---- types ----

std::string tyvar_of_name(std::string_view s) {
  if (s.size() >= 2 && s[1] == '\'') return "' " + std::string(s);
  if (path::is_keyword(s)) return "'\\#" + std::string(s);
  if (s == "_") return std::string(s);
  return "'" + std::string(s);
}

void tyvar(Formatter& ppf, std::string_view s) { fprintf(ppf, "%s", tyvar_of_name(s)); }

namespace {

void ty_var(Formatter& ppf, bool non_gen, const std::string& s) { tyvar(ppf, non_gen ? "_" + s : s); }

template <class T, class P, class S>
void print_list(P pr_, S sep, Formatter& ppf, const std::vector<T>& l) {
  for (std::size_t i = 0; i < l.size(); ++i) {
    pr_(ppf, l[i]);
    if (i + 1 < l.size()) sep(ppf);
  }
}

template <class T, class P, class S>
void print_list_init(P pr_, S sep, Formatter& ppf, const std::vector<T>& l) {
  for (const T& a : l) {
    sep(ppf);
    pr_(ppf, a);
  }
}

void pr_present(Formatter& ppf, const std::vector<std::string>& l) {
  print_list<std::string>([](Formatter& f, const std::string& s) { fprintf(f, "`%s", s); },
                          [](Formatter& f) { fprintf(f, "@ "); }, ppf, l);
}

void pr_vars(Formatter& ppf, const std::vector<std::string>& l) {
  print_list<std::string>([](Formatter& f, const std::string& s) { tyvar(f, s); }, [](Formatter& f) { fprintf(f, "@ "); },
                          ppf, l);
}

void print_arg_label(Formatter& ppf, const ot::ArgLabel& lbl) {
  switch (lbl.k) {
    case ot::ArgLabel::K::Nolabel: break;
    case ot::ArgLabel::K::Labelled: fprintf(ppf, "%a:", pr(print_lident, std::string_view(lbl.s))); break;
    case ot::ArgLabel::K::Optional: fprintf(ppf, "?%a:", pr(print_lident, std::string_view(lbl.s))); break;
  }
}

void print_label_type(Formatter& ppf, const std::optional<std::string>& l) {
  if (l) {
    pp_print_string(ppf, *l);
    pp_print_string(ppf, ":");
  }
}

void print_out_type_1(Formatter& ppf, const ot::OutType* ty);
void print_out_type_2(bool arg, Formatter& ppf, const ot::OutType* ty);
void print_simple_out_type(Formatter& ppf, const ot::OutType* ty);
void print_package(Formatter& ppf, const ot::OutPackage* pack);
void print_record_decl(Formatter& ppf, const std::vector<ot::OutLabel>& lbls);
void print_object_fields(const ot::OutRow& row, Formatter& ppf,
                         const std::vector<std::pair<std::string, const ot::OutType*>>& fields, std::size_t from);
void print_row_field(Formatter& ppf, const ot::OutVariant::Field& f);

template <class T, class P>
void print_typlist(P print_elem, std::string_view sep, Formatter& ppf, const std::vector<T>& tyl) {
  for (std::size_t i = 0; i < tyl.size(); ++i) {
    print_elem(ppf, tyl[i]);
    if (i + 1 < tyl.size()) {
      pp_print_string(ppf, sep);
      pp_print_space(ppf);
    }
  }
}

void print_typargs(Formatter& ppf, const std::vector<const ot::OutType*>& tyl) {
  if (tyl.empty()) return;
  if (tyl.size() == 1) {
    print_simple_out_type(ppf, tyl[0]);
    pp_print_space(ppf);
    return;
  }
  pp_open_box(ppf, 1);
  pp_print_char(ppf, '(');
  print_typlist<const ot::OutType*>(out_type, ",", ppf, tyl);
  pp_print_char(ppf, ')');
  pp_close_box(ppf);
  pp_print_space(ppf);
}

void print_out_type_1(Formatter& ppf, const ot::OutType* ty) {
  switch (ty->k) {
    case OT::Otyp_arrow:
      pp_open_box(ppf, 0);
      print_arg_label(ppf, ty->label);
      print_out_type_2(true, ppf, ty->t1);
      pp_print_string(ppf, " ->");
      pp_print_space(ppf);
      print_out_type_1(ppf, ty->t2);
      pp_close_box(ppf);
      break;
    case OT::Otyp_functor:
      pp_open_box(ppf, 0);
      print_arg_label(ppf, ty->label);
      pp_print_string(ppf, "(module ");
      out_ident(ppf, ty->id);
      pp_print_string(ppf, " : ");
      print_package(ppf, ty->pack);
      pp_print_string(ppf, ") ->");
      pp_print_space(ppf);
      print_out_type_1(ppf, ty->t1);
      pp_close_box(ppf);
      break;
    default: print_out_type_2(false, ppf, ty); break;
  }
}

void print_out_type_2(bool arg, Formatter& ppf, const ot::OutType* ty) {
  if (ty->k != OT::Otyp_tuple) {
    print_simple_out_type(ppf, ty);
    return;
  }
  // Tuples require parens in argument function argument position (~arg)
  // when the first element has a label.
  bool parens = !ty->tuple.empty() && ty->tuple[0].first ? arg : false;
  if (parens) pp_print_char(ppf, '(');
  using Elem = std::pair<std::optional<std::string>, const ot::OutType*>;
  auto print_elem = [](Formatter& f, const Elem& e) {
    pp_open_box(f, 0);
    print_label_type(f, e.first);
    print_simple_out_type(f, e.second);
    pp_close_box(f);
  };
  fprintf(ppf, "@[<0>%a@]", [&](Formatter& f) { print_typlist<Elem>(print_elem, " *", f, ty->tuple); });
  if (parens) pp_print_char(ppf, ')');
}

void print_simple_out_type(Formatter& ppf, const ot::OutType* ty) {
  switch (ty->k) {
    case OT::Otyp_class:
      fprintf(ppf, "@[%a#%a@]", pr(print_typargs, ty->args), pr(out_ident, ty->id));
      break;
    case OT::Otyp_constr:
      pp_open_box(ppf, 0);
      print_typargs(ppf, ty->args);
      out_ident(ppf, ty->id);
      pp_close_box(ppf);
      break;
    case OT::Otyp_object:
      fprintf(ppf, "@[<2>< %a >@]", [&](Formatter& f) { print_object_fields(ty->row, f, ty->fields, 0); });
      break;
    case OT::Otyp_stuff: pp_print_string(ppf, ty->s); break;
    case OT::Otyp_var: ty_var(ppf, ty->non_gen, ty->s); break;
    case OT::Otyp_variant: {
      bool closed = ty->closed;
      const auto& tags = ty->tags;
      auto print_present = [&](Formatter& f) {
        if (!tags || tags->empty()) return;
        fprintf(f, "@;<1 -2>> @[<hov>%a@]", pr(pr_present, *tags));
      };
      auto print_fields = [&](Formatter& f) {
        if (!ty->variant.is_typ) {
          print_list<ot::OutVariant::Field>(print_row_field, [](Formatter& g) { fprintf(g, "@;<1 -2>| "); }, f,
                                            ty->variant.fields);
        } else {
          print_simple_out_type(f, ty->variant.typ);
        }
      };
      fprintf(ppf, "@[<hov>[%s@[<hv>@[<hv>%a@]%a@]@ ]@]",
              closed ? (!tags ? " " : "< ") : (!tags ? "> " : "? "), print_fields, print_present);
      break;
    }
    case OT::Otyp_alias:
    case OT::Otyp_poly:
    case OT::Otyp_arrow:
    case OT::Otyp_functor:
    case OT::Otyp_tuple:
      pp_open_box(ppf, 1);
      pp_print_char(ppf, '(');
      out_type(ppf, ty);
      pp_print_char(ppf, ')');
      pp_close_box(ppf);
      break;
    case OT::Otyp_abstract:
    case OT::Otyp_open:
    case OT::Otyp_external:
    case OT::Otyp_sum:
    case OT::Otyp_manifest: break;
    case OT::Otyp_record: print_record_decl(ppf, ty->labels); break;
    case OT::Otyp_module: fprintf(ppf, "@[<1>(module %a)@]", pr(print_package, ty->pack)); break;
    case OT::Otyp_attribute:
      fprintf(ppf, "@[<1>(%a [@@%s])@]", pr(out_type, ty->t1), ty->attr.oattr_name);
      break;
  }
}

void print_package(Formatter& ppf, const ot::OutPackage* pack) {
  fprintf(ppf, "%a", pr(out_ident, pack->opack_path));
  bool first = true;
  for (const auto& [s, t] : pack->opack_constraints) {
    const char* sep = first ? "with" : "and";
    first = false;
    fprintf(ppf, " %s type %s = %a", sep, s, pr(out_type, t));
  }
}

void print_record_decl(Formatter& ppf, const std::vector<ot::OutLabel>& lbls) {
  fprintf(ppf, "{%a@;<1 -2>}", [&](Formatter& f) {
    print_list_init<ot::OutLabel>(out_label, [](Formatter& g) { fprintf(g, "@ "); }, f, lbls);
  });
}

void print_object_fields(const ot::OutRow& row, Formatter& ppf,
                         const std::vector<std::pair<std::string, const ot::OutType*>>& fields, std::size_t from) {
  std::size_t n = fields.size() - from;
  if (n == 0) {
    switch (row.k) {
      case ot::OutRowK::Orow_closed: break;
      case ot::OutRowK::Orow_open_anonymous: fprintf(ppf, ".."); break;
      case ot::OutRowK::Orow_open: fprintf(ppf, ".. as %a", pr(out_type, row.ty)); break;
    }
    return;
  }
  const auto& [s, t] = fields[from];
  if (n == 1) {
    fprintf(ppf, "%a : %a", pr(print_lident, std::string_view(s)), pr(out_type, t));
    if (row.k != ot::OutRowK::Orow_closed) fprintf(ppf, ";@ ");
    print_object_fields(row, ppf, fields, fields.size());
    return;
  }
  fprintf(ppf, "%s : %a;@ %a", s, pr(out_type, t),
          [&](Formatter& f) { print_object_fields(row, f, fields, from + 1); });
}

void print_row_field(Formatter& ppf, const ot::OutVariant::Field& fl) {
  auto pr_of = [&](Formatter& f) {
    if (fl.amp)
      fprintf(f, " of@ &@ ");
    else if (!fl.tys.empty())
      fprintf(f, " of@ ");
    else
      fprintf(f, "");
  };
  fprintf(ppf, "@[<hv 2>`%a%t%a@]", pr(print_lident, std::string_view(fl.label)), pr_of,
          [&](Formatter& f) { print_typlist<const ot::OutType*>(out_type, " &", f, fl.tys); });
}

}  // namespace

void out_type(Formatter& ppf, const ot::OutType* ty) {
  switch (ty->k) {
    case OT::Otyp_alias:
      fprintf(ppf, "@[%a@ as %a@]", pr(out_type, ty->t1),
              [&](Formatter& f) { ty_var(f, ty->non_gen, ty->s); });
      break;
    case OT::Otyp_poly:
      fprintf(ppf, "@[<hov 2>%a.@ %a@]", pr(pr_vars, ty->vars), pr(out_type, ty->t1));
      break;
    default: print_out_type_1(ppf, ty); break;
  }
}

void out_type_args(Formatter& ppf, const std::vector<const ot::OutType*>& tyl) { print_typargs(ppf, tyl); }

void out_label(Formatter& ppf, const ot::OutLabel& l) {
  fprintf(ppf, "@[<2>%s%a :@ %a%s@];", l.olab_mutable ? "mutable " : "",
          pr(print_lident, std::string_view(l.olab_name)), pr(out_type, l.olab_type),
          l.olab_atomic ? " [@atomic]" : "");
}

// ---- class types ----

namespace {

void print_type_parameter(Formatter& ppf, bool non_gen, const std::string& s) {
  if (s == "_")
    fprintf(ppf, "_");
  else
    ty_var(ppf, non_gen, s);
}

void print_out_class_params(Formatter& ppf, const std::vector<ot::OutTypeParam>& tyl) {
  if (tyl.empty()) return;
  fprintf(ppf, "@[<1>[%a]@]@ ", [&](Formatter& f) {
    print_list<ot::OutTypeParam>(type_parameter, [](Formatter& g) { fprintf(g, ", "); }, f, tyl);
  });
}

void print_out_class_sig_item(Formatter& ppf, const ot::OutClassSigItem* it) {
  switch (it->k) {
    case ot::OutClassSigItem::K::Ocsg_constraint:
      fprintf(ppf, "@[<2>constraint %a =@ %a@]", pr(out_type, it->t1), pr(out_type, it->t2));
      break;
    case ot::OutClassSigItem::K::Ocsg_method:
      fprintf(ppf, "@[<2>method %s%s%a :@ %a@]", it->b1 ? "private " : "", it->b2 ? "virtual " : "",
              pr(print_lident, std::string_view(it->name)), pr(out_type, it->t1));
      break;
    case ot::OutClassSigItem::K::Ocsg_value:
      fprintf(ppf, "@[<2>val %s%s%a :@ %a@]", it->b1 ? "mutable " : "", it->b2 ? "virtual " : "",
              pr(print_lident, std::string_view(it->name)), pr(out_type, it->t1));
      break;
  }
}

}  // namespace

void type_parameter(Formatter& ppf, const ot::OutTypeParam& p) {
  const char* v = "";
  switch (p.ot_variance.first) {
    case ot::Variance::Covariant: v = "+"; break;
    case ot::Variance::Contravariant: v = "-"; break;
    case ot::Variance::NoVariance: v = ""; break;
    case ot::Variance::Bivariant: v = "+-"; break;
  }
  fprintf(ppf, "%s%s%a", v, p.ot_variance.second == ot::Injectivity::Injective ? "!" : "",
          [&](Formatter& f) { print_type_parameter(f, p.ot_non_gen, p.ot_name); });
}

void out_class_type(Formatter& ppf, const ot::OutClassType* cty) {
  switch (cty->k) {
    case ot::OutClassType::K::Octy_constr: {
      auto pr_tyl = [&](Formatter& f) {
        if (cty->tys.empty()) return;
        fprintf(f, "@[<1>[%a]@]@ ", [&](Formatter& g) { print_typlist<const ot::OutType*>(out_type, ",", g, cty->tys); });
      };
      fprintf(ppf, "@[%a%a@]", pr_tyl, pr(out_ident, cty->id));
      break;
    }
    case ot::OutClassType::K::Octy_arrow:
      fprintf(ppf, "@[%a%a ->@ %a@]", pr(print_arg_label, cty->label),
              [&](Formatter& f) { print_out_type_2(true, f, cty->ty); }, pr(out_class_type, cty->cty));
      break;
    case ot::OutClassType::K::Octy_signature: {
      auto pr_param = [&](Formatter& f) {
        if (cty->ty) fprintf(f, "@ @[(%a)@]", pr(out_type, cty->ty));
      };
      fprintf(ppf, "@[<hv 2>@[<2>object%a@]@ %a@;<1 -2>end@]", pr_param, [&](Formatter& f) {
        print_list<const ot::OutClassSigItem*>(print_out_class_sig_item, [](Formatter& g) { fprintf(g, "@ "); }, f,
                                               cty->csil);
      });
      break;
    }
  }
}

// ---- signatures ----

namespace {

ot::OutConstructor constructor_of_extension_constructor(const ot::OutExtensionConstructor& ext) {
  return ot::OutConstructor{ext.oext_name, ext.oext_args, ext.oext_ret_type};
}

// split_anon_functor_arguments: the anonymous suffix apart
std::pair<std::vector<ot::OutFunctorParam>, std::vector<ot::OutFunctorParam>> split_anon_functor_arguments(
    const std::vector<ot::OutFunctorParam>& params) {
  std::size_t k = params.size();
  while (k > 0 && params[k - 1].some && !params[k - 1].name) --k;
  return {std::vector<ot::OutFunctorParam>(params.begin(), params.begin() + static_cast<long>(k)),
          std::vector<ot::OutFunctorParam>(params.begin() + static_cast<long>(k), params.end())};
}

void print_simple_out_module_type(Formatter& ppf, const ot::OutModuleType* mty);

void print_out_functor(Formatter& ppf, const ot::OutModuleType* t) {
  std::vector<ot::OutFunctorParam> params;
  const ot::OutModuleType* non_functor = t;
  while (non_functor->k == ot::OutModuleType::K::Omty_functor) {
    params.push_back(non_functor->param);
    non_functor = non_functor->res;
  }
  fprintf(ppf, "@[<2>%a%a@]", pr(out_functor_parameters, params), pr(print_simple_out_module_type, non_functor));
}

void print_simple_out_module_type(Formatter& ppf, const ot::OutModuleType* mty) {
  switch (mty->k) {
    case ot::OutModuleType::K::Omty_abstract: break;
    case ot::OutModuleType::K::Omty_ident: fprintf(ppf, "%a", pr(out_ident, mty->id)); break;
    case ot::OutModuleType::K::Omty_signature:
      if (mty->sg.empty())
        fprintf(ppf, "sig end");
      else
        fprintf(ppf, "@[<hv 2>sig@ %a@;<1 -2>end@]", pr(out_signature, mty->sg));
      break;
    case ot::OutModuleType::K::Omty_alias: fprintf(ppf, "(module %a)", pr(out_ident, mty->id)); break;
    case ot::OutModuleType::K::Omty_functor: fprintf(ppf, "(%a)", pr(out_module_type, mty)); break;
  }
}

void print_out_type_decl(std::string_view kwd, Formatter& ppf, const ot::OutTypeDecl* td) {
  auto print_constraints = [&](Formatter& f) {
    for (const auto& [ty1, ty2] : td->otype_constraints)
      fprintf(f, "@ @[<2>constraint %a =@ %a@]", pr(out_type, ty1), pr(out_type, ty2));
  };
  auto type_defined = [&](Formatter& f) {
    if (td->otype_params.empty())
      print_lident(f, td->otype_name);
    else if (td->otype_params.size() == 1)
      fprintf(f, "@[%a@ %a@]", pr(type_parameter, td->otype_params[0]),
              pr(print_lident, std::string_view(td->otype_name)));
    else
      fprintf(f, "@[(@[%a)@]@ %a@]",
              [&](Formatter& g) {
                print_list<ot::OutTypeParam>(type_parameter, [](Formatter& h) { fprintf(h, ",@ "); }, g,
                                             td->otype_params);
              },
              pr(print_lident, std::string_view(td->otype_name)));
  };
  auto print_manifest = [&](Formatter& f, const ot::OutType* t) {
    if (t->k == OT::Otyp_manifest) fprintf(f, " =@ %a", pr(out_type, t->t1));
  };
  auto print_name_params = [&](Formatter& f) {
    fprintf(f, "%s %t%a", kwd, type_defined, [&](Formatter& g) { print_manifest(g, td->otype_type); });
  };
  const ot::OutType* ty = td->otype_type->k == OT::Otyp_manifest ? td->otype_type->t2 : td->otype_type;
  auto print_private = [&](Formatter& f, bool priv) {
    if (priv) fprintf(f, " private");
  };
  auto print_immediate = [&](Formatter& f) {
    switch (td->otype_immediate) {
      case ot::TypeImmediacyO::Unknown: break;
      case ot::TypeImmediacyO::Always: fprintf(f, " [%@%@immediate]"); break;
      case ot::TypeImmediacyO::Always_on_64bits: fprintf(f, " [%@%@immediate64]"); break;
    }
  };
  auto print_unboxed = [&](Formatter& f) {
    if (td->otype_unboxed) fprintf(f, " [%@%@unboxed]");
  };
  auto print_out_tkind = [&](Formatter& f, const ot::OutType* t) {
    switch (t->k) {
      case OT::Otyp_abstract: break;
      case OT::Otyp_record:
        fprintf(f, " =%a %a", [&](Formatter& g) { print_private(g, td->otype_private); },
                pr(print_record_decl, t->labels));
        break;
      case OT::Otyp_sum: {
        auto variants = [&](Formatter& g) {
          if (t->constrs.empty())
            fprintf(g, "|");
          else
            fprintf(g, "%a", [&](Formatter& h) {
              print_list<ot::OutConstructor>(out_constr, [](Formatter& i) { fprintf(i, "@ | "); }, h, t->constrs);
            });
        };
        fprintf(f, " =%a@;<1 2>%a", [&](Formatter& g) { print_private(g, td->otype_private); }, variants);
        break;
      }
      case OT::Otyp_open:
        fprintf(f, " =%a ..", [&](Formatter& g) { print_private(g, td->otype_private); });
        break;
      case OT::Otyp_external: fprintf(f, " =@ external %S", t->s); break;
      default:
        fprintf(f, " =%a@;<1 2>%a", [&](Formatter& g) { print_private(g, td->otype_private); }, pr(out_type, t));
        break;
    }
  };
  fprintf(ppf, "@[<2>@[<hv 2>%t%a@]%t%t%t@]", print_name_params, [&](Formatter& f) { print_out_tkind(f, ty); },
          print_constraints, print_immediate, print_unboxed);
}

void print_extended_params(Formatter& f, const std::vector<ot::OutTypeParam>& params, const std::string& name) {
  if (params.empty())
    fprintf(f, "%a", pr(print_lident, std::string_view(name)));
  else if (params.size() == 1)
    fprintf(f, "@[%a@ %a@]", pr(type_parameter, params[0]), pr(print_lident, std::string_view(name)));
  else
    fprintf(f, "@[(@[%a)@]@ %a@]",
            [&](Formatter& g) {
              print_list<ot::OutTypeParam>(type_parameter, [](Formatter& h) { fprintf(h, ",@ "); }, g, params);
            },
            pr(print_lident, std::string_view(name)));
}

void print_out_extension_constructor(Formatter& ppf, const ot::OutExtensionConstructor* ext) {
  fprintf(ppf, "@[<hv 2>type %t +=%s@;<1 2>%a@]",
          [&](Formatter& f) { print_extended_params(f, ext->oext_type_params, ext->oext_type_name); },
          ext->oext_private ? " private" : "", pr(out_constr, constructor_of_extension_constructor(*ext)));
}

}  // namespace

void out_functor_parameters(Formatter& ppf, const std::vector<ot::OutFunctorParam>& l) {
  auto print_nonanon_arg = [](Formatter& f, const ot::OutFunctorParam& p) {
    if (!p.some)
      fprintf(f, "()");
    else
      fprintf(f, "(%s : %a)", p.name ? *p.name : std::string("_"), pr(out_module_type, p.mty));
  };
  std::function<void(Formatter&, std::size_t)> print_args = [&](Formatter& f, std::size_t from) {
    if (from >= l.size()) return;
    const ot::OutFunctorParam& p = l[from];
    if (p.some && !p.name) {
      fprintf(f, "%a ->@ %a", pr(print_simple_out_module_type, p.mty), [&](Formatter& g) { print_args(g, from + 1); });
      return;
    }
    std::vector<ot::OutFunctorParam> rest(l.begin() + static_cast<long>(from), l.end());
    auto [args, anons] = split_anon_functor_arguments(rest);
    fprintf(f, "@[%a@]@ ->@ %a",
            [&](Formatter& g) { pp_print_list<ot::OutFunctorParam>(g, print_nonanon_arg, args, pp_print_space); },
            [&](Formatter& g) { out_functor_parameters(g, anons); });
  };
  print_args(ppf, 0);
}

void out_module_type(Formatter& ppf, const ot::OutModuleType* mty) { print_out_functor(ppf, mty); }

void out_signature(Formatter& ppf, const std::vector<const ot::OutSigItem*>& sg) {
  std::size_t i = 0;
  while (true) {
    std::size_t rem = sg.size() - i;
    if (rem == 0) return;
    if (rem == 1) {
      out_sig_item(ppf, sg[i]);
      return;
    }
    const ot::OutSigItem* item = sg[i];
    if (item->k == ot::OutSigItem::K::Osig_typext && item->es == ot::OutExtStatus::Oext_first) {
      // Gather together the extension constructors
      std::vector<ot::OutConstructor> exts{constructor_of_extension_constructor(*item->ext)};
      std::size_t j = i + 1;
      while (j < sg.size() && sg[j]->k == ot::OutSigItem::K::Osig_typext && sg[j]->es == ot::OutExtStatus::Oext_next)
        exts.push_back(constructor_of_extension_constructor(*sg[j++]->ext));
      ot::OutTypeExtension te{item->ext->oext_type_name, item->ext->oext_type_params, exts, item->ext->oext_private};
      std::vector<const ot::OutSigItem*> items(sg.begin() + static_cast<long>(j), sg.end());
      fprintf(ppf, "%a@ %a", pr(out_type_extension, te), pr(out_signature, items));
      return;
    }
    std::vector<const ot::OutSigItem*> items(sg.begin() + static_cast<long>(i) + 1, sg.end());
    fprintf(ppf, "%a@ %a", pr(out_sig_item, item), pr(out_signature, items));
    return;
  }
}

void out_sig_item(Formatter& ppf, const ot::OutSigItem* it) {
  using K = ot::OutSigItem::K;
  switch (it->k) {
    case K::Osig_class:
      fprintf(ppf, "@[<2>%s%s@ %a%a@ :@ %a@]", it->rs == ot::OutRecStatus::Orec_next ? "and" : "class",
              it->virt ? " virtual" : "", pr(print_out_class_params, it->params),
              pr(print_lident, std::string_view(it->name)), pr(out_class_type, it->clt));
      break;
    case K::Osig_class_type:
      fprintf(ppf, "@[<2>%s%s@ %a%a@ =@ %a@]", it->rs == ot::OutRecStatus::Orec_next ? "and" : "class type",
              it->virt ? " virtual" : "", pr(print_out_class_params, it->params),
              pr(print_lident, std::string_view(it->name)), pr(out_class_type, it->clt));
      break;
    case K::Osig_typext:
      if (it->es == ot::OutExtStatus::Oext_exception)
        fprintf(ppf, "@[<2>exception %a@]", pr(out_constr, constructor_of_extension_constructor(*it->ext)));
      else
        print_out_extension_constructor(ppf, it->ext);
      break;
    case K::Osig_modtype:
      if (it->mty->k == ot::OutModuleType::K::Omty_abstract)
        fprintf(ppf, "@[<2>module type %s@]", it->name);
      else
        fprintf(ppf, "@[<2>module type %s =@ %a@]", it->name, pr(out_module_type, it->mty));
      break;
    case K::Osig_module:
      if (it->mty->k == ot::OutModuleType::K::Omty_alias)
        fprintf(ppf, "@[<2>module %s =@ %a@]", it->name, pr(out_ident, it->mty->id));
      else
        fprintf(ppf, "@[<2>%s %s :@ %a@]",
                it->rs == ot::OutRecStatus::Orec_not     ? "module"
                : it->rs == ot::OutRecStatus::Orec_first ? "module rec"
                                                         : "and",
                it->name, pr(out_module_type, it->mty));
      break;
    case K::Osig_type:
      print_out_type_decl(it->rs == ot::OutRecStatus::Orec_not     ? "type nonrec"
                          : it->rs == ot::OutRecStatus::Orec_first ? "type"
                                                                   : "and",
                          ppf, it->td);
      break;
    case K::Osig_value: {
      const ot::OutValDecl* vd = it->vd;
      const char* kwd = vd->oval_prims.empty() ? "val" : "external";
      auto pr_prims = [&](Formatter& f) {
        if (vd->oval_prims.empty()) return;
        fprintf(f, "@ = \"%s\"", vd->oval_prims[0]);
        for (std::size_t i = 1; i < vd->oval_prims.size(); ++i) fprintf(f, "@ \"%s\"", vd->oval_prims[i]);
      };
      auto pr_attrs = [&](Formatter& f) {
        for (const auto& a : vd->oval_attributes) fprintf(f, "@ [@@@@%s]", a.oattr_name);
      };
      fprintf(ppf, "@[<2>%s %a :@ %a%a%a@]", kwd, pr(value_ident, std::string_view(vd->oval_name)),
              pr(out_type, vd->oval_type), pr_prims, pr_attrs);
      break;
    }
    case K::Osig_ellipsis: fprintf(ppf, "..."); break;
  }
}

void out_constr(Formatter& ppf, const ot::OutConstructor& c) {
  std::string name = c.ocstr_name == "::" ? "(::)" : c.ocstr_name;  // #7200
  auto typlist = [&](Formatter& f) { print_typlist<const ot::OutType*>(print_simple_out_type, " *", f, c.ocstr_args); };
  if (!c.ocstr_return_type) {
    if (c.ocstr_args.empty())
      pp_print_string(ppf, name);
    else
      fprintf(ppf, "@[<2>%s of@ %a@]", name, typlist);
  } else {
    if (c.ocstr_args.empty())
      fprintf(ppf, "@[<2>%s :@ %a@]", name, pr(print_simple_out_type, c.ocstr_return_type));
    else
      fprintf(ppf, "@[<2>%s :@ %a -> %a@]", name, typlist, pr(print_simple_out_type, c.ocstr_return_type));
  }
}

void out_type_extension(Formatter& ppf, const ot::OutTypeExtension& te) {
  fprintf(ppf, "@[<hv 2>type %t +=%s@;<1 2>%a@]",
          [&](Formatter& f) { print_extended_params(f, te.otyext_params, te.otyext_name); },
          te.otyext_private ? " private" : "", [&](Formatter& f) {
            print_list<ot::OutConstructor>(out_constr, [](Formatter& g) { fprintf(g, "@ | "); }, f,
                                           te.otyext_constructors);
          });
}

}  // namespace oprint

}  // namespace cppcaml::typing
