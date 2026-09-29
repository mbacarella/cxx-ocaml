// Port of typing/printpat.ml (cxx/PORTING.md stage 9).
#include "cppcaml/typing/printpat.hpp"

#include <algorithm>

#include "cppcaml/typing/datarepr.hpp"
#include "cppcaml/typing/printlambda.hpp"

namespace cppcaml::typing::printpat {

using namespace format_doc;
namespace tt = typedtree;
using tt::as;
using PK = tt::PatternDesc::Kind;

namespace {

bool is_cons(const ConstructorDescription* c) { return c->cstr_name == "::"; }

// Ident.doc_print (~with_scope:false)
void ident_doc_print(Formatter& ppf, Ident::t id) {
  format::Formatter f;
  printlambda::ident(f, id);
  f.print_flush();
  pp_print_string(ppf, f.take());
}

// { v with pat_extra = rem }
const tt::Pattern* with_extra(const tt::Pattern* v, Slice<tt::PatExtraItem> rem) {
  auto* p = make<tt::Pattern>(*v);
  p->pat_extra = rem;
  return p;
}

void pretty_list(const std::function<void(Formatter&, const tt::Pattern*)>& print_val, std::string_view sep,
                 Formatter& ppf, const std::vector<const tt::Pattern*>& vs, std::size_t from = 0);
void pretty_vals(std::string_view sep, Formatter& ppf, const std::vector<const tt::Pattern*>& vs) {
  pretty_list(pretty_val, sep, ppf, vs);
}
void pretty_car(Formatter& ppf, const tt::Pattern* v);
void pretty_cdr(Formatter& ppf, const tt::Pattern* v);
void pretty_arg(Formatter& ppf, const tt::Pattern* v);
void pretty_or(Formatter& ppf, const tt::Pattern* v);

std::vector<const tt::Pattern*> vec(Slice<const tt::Pattern*> s) { return {s.begin(), s.end()}; }

void pretty_list(const std::function<void(Formatter&, const tt::Pattern*)>& print_val, std::string_view sep,
                 Formatter& ppf, const std::vector<const tt::Pattern*>& vs, std::size_t from) {
  std::size_t n = vs.size() - from;
  if (n == 0) return;
  if (n == 1) {
    print_val(ppf, vs[from]);
    return;
  }
  fprintf(ppf, "%a%s@ %a", [&](Formatter& f) { print_val(f, vs[from]); }, sep,
          [&](Formatter& f) { pretty_list(print_val, sep, f, vs, from + 1); });
}

void pretty_lvals(Formatter& ppf, const std::vector<const tt::RecordPatField*>& l, std::size_t from) {
  std::size_t n = l.size() - from;
  if (n == 0) return;
  const tt::RecordPatField* f0 = l[from];
  if (n == 1) {
    fprintf(ppf, "%s=%a", f0->label->lbl_name, pr(pretty_val, f0->pat));
    return;
  }
  fprintf(ppf, "%s=%a;@ %a", f0->label->lbl_name, pr(pretty_val, f0->pat),
          [&](Formatter& f) { pretty_lvals(f, l, from + 1); });
}

void pretty_car(Formatter& ppf, const tt::Pattern* v) {
  if (auto* c = as<tt::Tpat_construct>(v->pat_desc); c && c->args.size() == 2 && !c->annot && is_cons(c->cstr))
    fprintf(ppf, "(%a)", pr(pretty_val, v));
  else
    pretty_val(ppf, v);
}

void pretty_cdr(Formatter& ppf, const tt::Pattern* v) {
  if (auto* c = as<tt::Tpat_construct>(v->pat_desc); c && c->args.size() == 2 && !c->annot && is_cons(c->cstr))
    fprintf(ppf, "%a::@,%a", pr(pretty_car, c->args[0]), pr(pretty_cdr, c->args[1]));
  else
    pretty_val(ppf, v);
}

void pretty_arg(Formatter& ppf, const tt::Pattern* v) {
  auto* c = as<tt::Tpat_construct>(v->pat_desc);
  auto* vr = as<tt::Tpat_variant>(v->pat_desc);
  if ((c && !c->args.empty() && !c->annot) || (vr && vr->arg))
    fprintf(ppf, "(%a)", pr(pretty_val, v));
  else
    pretty_val(ppf, v);
}

void pretty_or(Formatter& ppf, const tt::Pattern* v) {
  if (auto* o = as<tt::Tpat_or>(v->pat_desc))
    fprintf(ppf, "%a|@,%a", pr(pretty_or, o->p1), pr(pretty_or, o->p2));
  else
    pretty_val(ppf, v);
}

}  // namespace

std::string pretty_const(const tt::Constant& c) {
  using CK = tt::Constant::Kind;
  switch (c.kind) {
    case CK::Const_int: return std::to_string(c.i);
    case CK::Const_char: return "'" + format::char_escaped(static_cast<char>(c.i)) + "'";
    case CK::Const_string: return "\"" + format::string_escaped(c.s) + "\"";
    case CK::Const_float: return std::string(c.s);
    case CK::Const_int32: return std::to_string(c.boxed) + "l";
    case CK::Const_int64: return std::to_string(c.boxed) + "L";
    case CK::Const_nativeint: return std::to_string(c.boxed) + "n";
  }
  return "";
}

void pretty_val(Formatter& ppf, const tt::Pattern* v) {
  if (!v->pat_extra.empty()) {
    const tt::PatExtraItem& extra = v->pat_extra[0];
    const tt::Pattern* rest = with_extra(v, Slice<tt::PatExtraItem>{v->pat_extra.p + 1, v->pat_extra.n - 1});
    switch (extra.extra.kind) {
      case tt::PatExtra::Kind::Tpat_unpack:
        if (!extra.extra.pack)
          fprintf(ppf, "@[(module %a)@]", pr(pretty_val, rest));
        else
          fprintf(ppf, "@[(module %a : _)@]", pr(pretty_val, rest));
        break;
      case tt::PatExtra::Kind::Tpat_constraint: fprintf(ppf, "@[(%a : _)@]", pr(pretty_val, rest)); break;
      case tt::PatExtra::Kind::Tpat_type:
      case tt::PatExtra::Kind::Tpat_open: fprintf(ppf, "@[(# %a)@]", pr(pretty_val, rest)); break;
    }
    return;
  }
  const tt::PatternDesc* d = v->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any: fprintf(ppf, "_"); break;
    case PK::Tpat_var: fprintf(ppf, "%s", ident::name(static_cast<const tt::Tpat_var*>(d)->id)); break;
    case PK::Tpat_constant: fprintf(ppf, "%s", pretty_const(static_cast<const tt::Tpat_constant*>(d)->c)); break;
    case PK::Tpat_tuple: {
      auto* t = static_cast<const tt::Tpat_tuple*>(d);
      std::vector<const tt::LabeledPattern*> vs;
      for (const auto& lp : t->pats) vs.push_back(&lp);
      std::function<void(Formatter&, std::size_t)> lst = [&](Formatter& f, std::size_t from) {
        auto elem = [&](Formatter& g, const tt::LabeledPattern* lp) {
          if (lp->label.some) fprintf(g, "~%s:", lp->label.v);
          pretty_val(g, lp->pat);
        };
        std::size_t n = vs.size() - from;
        if (n == 0) return;
        if (n == 1) {
          elem(f, vs[from]);
          return;
        }
        fprintf(f, "%a%s@ %a", [&](Formatter& g) { elem(g, vs[from]); }, ",",
                [&](Formatter& g) { lst(g, from + 1); });
      };
      fprintf(ppf, "@[(%a)@]", [&](Formatter& f) { lst(f, 0); });
      break;
    }
    case PK::Tpat_construct: {
      auto* c = static_cast<const tt::Tpat_construct*>(d);
      std::string_view name = c->cstr->cstr_name;
      if (c->args.empty()) {
        fprintf(ppf, "%s", name);
      } else if (c->args.size() == 1 && !c->annot) {
        fprintf(ppf, "@[<2>%s@ %a@]", name, pr(pretty_arg, c->args[0]));
      } else if (name == "::" && c->args.size() == 2 && !c->annot) {
        fprintf(ppf, "@[%a::@,%a@]", pr(pretty_car, c->args[0]), pr(pretty_cdr, c->args[1]));
      } else if (!c->annot) {
        fprintf(ppf, "@[<2>%s@ @[(%a)@]@]", name, [&](Formatter& f) { pretty_vals(",", f, vec(c->args)); });
      } else if (c->annot->vars.empty()) {
        fprintf(ppf, "@[<2>%s@ @[(%a : _)@]@]", name, [&](Formatter& f) { pretty_vals(",", f, vec(c->args)); });
      } else {
        std::string vars;
        for (std::size_t i = 0; i < c->annot->vars.size(); ++i) {
          if (i) vars += " ";
          vars += ident::name(c->annot->vars[i].first);
        }
        fprintf(ppf, "@[<2>%s@ (type %s)@ @[(%a : _)@]@]", name, vars,
                [&](Formatter& f) { pretty_vals(",", f, vec(c->args)); });
      }
      break;
    }
    case PK::Tpat_variant: {
      auto* vr = static_cast<const tt::Tpat_variant*>(d);
      if (!vr->arg)
        fprintf(ppf, "`%s", vr->label);
      else
        fprintf(ppf, "@[<2>`%s@ %a@]", vr->label, pr(pretty_arg, vr->arg));
      break;
    }
    case PK::Tpat_record: {
      auto* r = static_cast<const tt::Tpat_record*>(d);
      std::vector<const tt::RecordPatField*> filtered;
      for (const auto& f : r->fields)
        if (f.pat->pat_desc->kind != PK::Tpat_any) filtered.push_back(&f);  // do not show lbl=_
      if (filtered.empty()) {
        fprintf(ppf, "{ _ }");
        break;
      }
      const LabelDescription* lbl = filtered[0]->label;
      std::size_t q = filtered.size() - 1;
      auto elision_mark = [&](Formatter& f) {
        // we assume that there is no label repetitions here
        if (lbl->lbl_all.size() > 1 + q) fprintf(f, ";@ _@ ");
      };
      fprintf(ppf, "@[{%a%t}@]", [&](Formatter& f) { pretty_lvals(f, filtered, 0); }, elision_mark);
      break;
    }
    case PK::Tpat_array:
      fprintf(ppf, "@[[| %a |]@]",
              [&](Formatter& f) { pretty_vals(" ;", f, vec(static_cast<const tt::Tpat_array*>(d)->pats)); });
      break;
    case PK::Tpat_lazy: fprintf(ppf, "@[<2>lazy@ %a@]", pr(pretty_arg, static_cast<const tt::Tpat_lazy*>(d)->pat)); break;
    case PK::Tpat_alias: {
      auto* a = static_cast<const tt::Tpat_alias*>(d);
      fprintf(ppf, "@[(%a@ as %a)@]", pr(pretty_val, a->pat), pr(ident_doc_print, a->id));
      break;
    }
    case PK::Tpat_value: fprintf(ppf, "%a", pr(pretty_val, static_cast<const tt::Tpat_value*>(d)->pat)); break;
    case PK::Tpat_exception:
      fprintf(ppf, "@[<2>exception@ %a@]", pr(pretty_arg, static_cast<const tt::Tpat_exception*>(d)->pat));
      break;
    case PK::Tpat_or: fprintf(ppf, "@[(%a)@]", pr(pretty_or, v)); break;
  }
}

void top_pretty(Formatter& ppf, const tt::Pattern* v) { fprintf(ppf, "@[%a@]", pr(pretty_val, v)); }

void pretty_pat(Formatter& ppf, const tt::Pattern* p) {
  top_pretty(ppf, p);
  pp_print_flush(ppf);
}

void pretty_line(Formatter& ppf, const std::vector<const tt::Pattern*>& line) {
  fprintf(ppf, "@[");
  for (const tt::Pattern* p : line) fprintf(ppf, "<%a>@ ", pr(pretty_val, p));
  fprintf(ppf, "@]");
}

void pretty_matrix(Formatter& ppf, const std::vector<std::vector<const tt::Pattern*>>& pss) {
  fprintf(ppf, "@[<v 2>  %a@]", [&](Formatter& f) {
    pp_print_list<std::vector<const tt::Pattern*>>(f, pretty_line, pss, pp_print_cut);
  });
}

}  // namespace cppcaml::typing::printpat
