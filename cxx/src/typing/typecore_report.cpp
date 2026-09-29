// Typecore.report_error and its error_of_exn (typecore.ml, "Error report"):
// see reporters.hpp.
#include <algorithm>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/printpat.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/typecore.hpp"

namespace cppcaml::typing::reporters {

namespace {

namespace fd = format_doc;
namespace et = errortrace;
namespace tt = typedtree;
namespace pt = parsetree;
using fd::Doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using EK = typecore::Error::Kind;
using misc::style::code_str;
using out_type::Mode;

// Style.as_inline_code Pprintast.Doc.longident / constr
auto quoted_longident(Longident::t lid) { return misc::style::code(pprintast::longident, lid); }
auto quoted_constr(Longident::t lid) { return misc::style::code(pprintast::constr, lid); }

std::optional<Doc> spellcheck(std::string_view unbound_name, const std::vector<std::string>& valid_names) {
  return misc::did_you_mean(misc::spellcheck(valid_names, unbound_name));
}
std::optional<Doc> spellcheck_idents(Ident::t unbound, const std::vector<Ident::t>& valid_idents) {
  std::vector<std::string> names;
  for (Ident::t id : valid_idents) names.push_back(std::string(ident::name(id)));
  return spellcheck(ident::name(unbound), names);
}

void tuple_component(bool print_article, Formatter& ppf, const OptStr& lbl) {
  const char* article = !print_article ? "" : lbl.some ? "a " : "an ";
  if (lbl.some)
    fprintf(ppf, "%scomponent with label %a", article, code_str(std::string(lbl.v)));
  else
    fprintf(ppf, "%sunlabeled component", article);
}

// pp_exp_denom
void pp_exp_denom(Formatter& ppf, const pt::Expression* pexp) {
  auto d_expression = [&](const char* w) { fprintf(ppf, "%a expression", code_str(w)); };
  const pt::ExpressionDesc* d = pexp->pexp_desc;
  using PK = pt::ExpressionDesc::Kind;
  switch (d->kind) {
    case PK::Pexp_constant: fd::pp_print_string(ppf, "constant"); break;
    case PK::Pexp_ident: fd::pp_print_string(ppf, "value"); break;
    case PK::Pexp_construct:
    case PK::Pexp_variant: fd::pp_print_string(ppf, "constructor"); break;
    case PK::Pexp_field: fd::pp_print_string(ppf, "field access"); break;
    case PK::Pexp_send: fd::pp_print_string(ppf, "method call"); break;
    case PK::Pexp_while: d_expression("while"); break;
    case PK::Pexp_for: d_expression("for"); break;
    case PK::Pexp_ifthenelse: d_expression("if-then-else"); break;
    case PK::Pexp_match: d_expression("match"); break;
    case PK::Pexp_try: d_expression("try-with"); break;
    default: fd::pp_print_string(ppf, "expression"); break;
  }
}

// report_this_pexp_has_type denom ppf exp
void report_this_pexp_has_type(const char* denom, Formatter& ppf, const pt::Expression* exp) {
  auto pden = [&](Formatter& f) {
    if (denom)
      fprintf(f, "%s", denom);
    else if (exp)
      pp_exp_denom(f, exp);
    else
      fprintf(f, "expression");
  };
  std::optional<Doc> nexp;
  if (exp) nexp = pprintast::nominal_exp(exp);
  if (nexp)
    fprintf(ppf, "The %t %a has type", pden, misc::style::code(fd::pp_doc, *nexp));
  else
    fprintf(ppf, "This %t has type", pden);
}

// Untypeast.untype_expression, as far as nominal_exp looks at it
std::optional<Doc> nominal_texp(const tt::Expression* e) {
  if (!e->exp_extra.empty() || !e->exp_attributes.empty()) return std::nullopt;
  Formatter f;
  const tt::ExpressionDesc* d = e->exp_desc;
  if (auto* i = tt::as<tt::Texp_ident>(d)) {
    pprintast::value_longident(f, i->lid.txt);
    return std::move(f.doc);
  }
  if (auto* v = tt::as<tt::Texp_variant>(d)) {
    if (v->arg) return std::nullopt;
    fprintf(f, "`%s", v->label);
    return std::move(f.doc);
  }
  if (auto* c = tt::as<tt::Texp_construct>(d)) {
    if (!c->args.empty()) return std::nullopt;
    pprintast::constr(f, c->lid.txt);
    return std::move(f.doc);
  }
  if (auto* fl = tt::as<tt::Texp_field>(d)) {
    std::optional<Doc> p = nominal_texp(fl->exp);
    if (!p) return std::nullopt;
    fd::pp_doc(f, *p);
    fprintf(f, ".%t", [&](Formatter& ff) { pprintast::value_longident(ff, fl->lid.txt); });
    return std::move(f.doc);
  }
  if (auto* s = tt::as<tt::Texp_send>(d)) {
    std::optional<Doc> p = nominal_texp(s->obj);
    if (!p) return std::nullopt;
    fd::pp_doc(f, *p);
    std::string_view name = s->meth.kind == tt::Meth::Kind::Tmeth_name ? s->meth.name : ident::name(s->meth.id);
    fprintf(f, "#%s", name);
    return std::move(f.doc);
  }
  if (auto* c = tt::as<tt::Texp_constant>(d)) {
    using CK = tt::Constant::Kind;
    switch (c->c.kind) {
      case CK::Const_string: return std::nullopt;
      case CK::Const_char: fprintf(f, "%C", static_cast<char>(c->c.i)); break;
      case CK::Const_int: fprintf(f, "%s", std::to_string(c->c.i)); break;
      case CK::Const_int32: fprintf(f, "%sl", std::to_string(c->c.boxed)); break;
      case CK::Const_int64: fprintf(f, "%sL", std::to_string(c->c.boxed)); break;
      case CK::Const_nativeint: fprintf(f, "%sn", std::to_string(c->c.boxed)); break;
      case CK::Const_float: fprintf(f, "%s", c->c.s); break;
    }
    return std::move(f.doc);
  }
  return std::nullopt;
}

// report_this_texp_has_type denom ppf texp
void report_this_texp_has_type(const char* denom, Formatter& ppf, const tt::Expression* texp) {
  // the denomination of the untyped expression
  auto pden = [&](Formatter& f) {
    if (denom) {
      fprintf(f, "%s", denom);
      return;
    }
    const tt::ExpressionDesc* d = texp->exp_desc;
    if (!texp->exp_extra.empty()) {
      fd::pp_print_string(f, "expression");
      return;
    }
    using TK = tt::ExpressionDesc::Kind;
    switch (d->kind) {
      case TK::Texp_constant: fd::pp_print_string(f, "constant"); break;
      case TK::Texp_ident: fd::pp_print_string(f, "value"); break;
      case TK::Texp_construct:
      case TK::Texp_variant: fd::pp_print_string(f, "constructor"); break;
      case TK::Texp_field: fd::pp_print_string(f, "field access"); break;
      case TK::Texp_send: fd::pp_print_string(f, "method call"); break;
      case TK::Texp_while: fprintf(f, "%a expression", code_str("while")); break;
      case TK::Texp_for: fprintf(f, "%a expression", code_str("for")); break;
      case TK::Texp_ifthenelse: fprintf(f, "%a expression", code_str("if-then-else")); break;
      case TK::Texp_match: fprintf(f, "%a expression", code_str("match")); break;
      case TK::Texp_try: fprintf(f, "%a expression", code_str("try-with")); break;
      default: fd::pp_print_string(f, "expression"); break;
    }
  };
  std::optional<Doc> nexp = nominal_texp(texp);
  if (nexp)
    fprintf(ppf, "The %t %a has type", pden, misc::style::code(fd::pp_doc, *nexp));
  else
    fprintf(ppf, "This %t has type", pden);
}

// report_literal_type_constraint expected_type const
std::vector<Msg> report_literal_type_constraint(Path::t expected_type, const pt::Constant& cst) {
  const pt::ConstantDesc& cd = cst.pconst_desc;
  if (cd.kind != pt::ConstantDesc::Kind::Pconst_integer) return {};
  const predef::Paths& P = predef::paths();
  char suffix;
  if (path::same(expected_type, P.int32))
    suffix = 'l';
  else if (path::same(expected_type, P.int64))
    suffix = 'L';
  else if (path::same(expected_type, P.nativeint))
    suffix = 'n';
  else if (path::same(expected_type, P.float_))
    suffix = '.';
  else
    return {};
  std::string c(cd.s);
  return {location::msg_noloc("@[@{<hint>Hint@}: Did you mean %a?@]", misc::style::code(
                                                                        [](Formatter& f, std::pair<std::string, char> cs) {
                                                                          fprintf(f, "%s%c", cs.first, cs.second);
                                                                        },
                                                                        std::make_pair(c, suffix)))};
}

std::vector<Msg> report_literal_type_constraint(const pt::Constant& cst,
                                                const std::optional<et::Diff<et::ExpandedType>>& tr) {
  if (!tr) return {};
  if (auto* c = as<Tconstr>(types::get_desc(tr->expected.ty)))
    if (c->args.empty()) return report_literal_type_constraint(c->path, cst);
  return {};
}

std::vector<Msg> report_partial_application(const std::optional<et::Diff<et::ExpandedType>>& tr) {
  if (!tr) return {};
  DescKind k = types::get_desc(tr->got.expanded)->kind;
  if (k == DescKind::Tarrow || k == DescKind::Tfunctor)
    return {location::msg_noloc(
        "@[@{<hint>Hint@}:@ This function application is partial,@ maybe@ some@ arguments@ are missing.@]")};
  return {};
}

std::vector<Msg> report_expr_type_clash_hints(const pt::Expression* exp,
                                              const std::optional<et::Diff<et::ExpandedType>>& diff) {
  if (!exp) return {};
  if (auto* c = pt::as<pt::Pexp_constant>(exp->pexp_desc)) return report_literal_type_constraint(c->c, diff);
  if (exp->pexp_desc->kind == pt::ExpressionDesc::Kind::Pexp_apply) return report_partial_application(diff);
  return {};
}

std::vector<Msg> report_pattern_type_clash_hints(const pt::PatternDesc* pat,
                                                 const std::optional<et::Diff<et::ExpandedType>>& diff) {
  if (!pat) return {};
  if (auto* c = pt::as<pt::Ppat_constant>(pat)) return report_literal_type_constraint(c->c, diff);
  return {};
}

Doc report_type_expected_explanation(typecore::TypeForcingContext expl) {
  auto because = [](const char* s) { return doc_printf("@ because it is in %s", s); };
  using C = typecore::TypeForcingContext;
  switch (expl) {
    case C::If_conditional: return because("the condition of an if-statement");
    case C::If_no_else_branch: return because("the result of a conditional with no else branch");
    case C::While_loop_conditional: return because("the condition of a while-loop");
    case C::While_loop_body: return because("the body of a while-loop");
    case C::For_loop_start_index: return because("a for-loop start index");
    case C::For_loop_stop_index: return because("a for-loop stop index");
    case C::For_loop_body: return because("the body of a for-loop");
    case C::Assert_condition: return because("the condition of an assertion");
    case C::Sequence_left_hand_side: return because("the left-hand side of a sequence");
    case C::When_guard: return because("a when-guard");
  }
  return Doc{};
}
Doc report_type_expected_explanation_opt(const typecore::Explanation& e) {
  if (!e) return Doc{};
  return report_type_expected_explanation(*e);
}

Report report_unification_error(const Location& loc, std::vector<Msg> sub, env::t env,
                                const et::UnificationError& err, const Doc& type_expected_explanation,
                                const Doc& txt1, const Doc& txt2) {
  return location::error_of_printer(
      loc,
      [&](Formatter& ppf) {
        errortrace_report::unification(ppf, env, err, txt1, txt2, type_expected_explanation);
      },
      std::move(sub));
}

Report report_too_many_arg_error(const tt::Expression* funct, const out_type::ExpansionPair& func_ty,
                                 const Location& previous_arg_loc, const Location& extra_arg_loc, bool returns_unit,
                                 const Location& loc) {
  auto cnum_offset = [](long off, Position pos) {
    pos.pos_cnum += off;
    return pos;
  };
  Location app_loc{loc.loc_start, extra_arg_loc.loc_end, false};
  Position arg_end = previous_arg_loc.loc_end;
  Location tail_loc{cnum_offset(-1, arg_end), cnum_offset(1, arg_end), false};
  // (~sub is built before the message: application arguments right to left)
  std::vector<Msg> sub;
  if (returns_unit) sub.push_back(location::msg(tail_loc, "@{<hint>Hint@}: Did you forget a %a?", code_str(";")));
  sub.push_back(location::msg(extra_arg_loc, "This extra argument is not expected."));
  return location::errorf_sub(app_loc, std::move(sub), "@[<v>@[<2>%a@ %a@]@ It is applied to too many arguments@]",
                              [&](Formatter& f) { report_this_texp_has_type("function", f, funct); },
                              [&](Formatter& f) { printtyp::type_expansion(Mode::Type, f, func_ty); });
}

out_type::ExpansionPair expand_type(env::t env, TypeExpr* ty) { return {ty, ctype::full_expand(true, env, ty)}; }

std::string prefixed_label_name(const ArgLabel& l) { return btype::prefixed_label_name(l); }

Report report_error(const Location& loc, env::t env, const typecore::Error& err) {
  auto print_expanded = [env](TypeExpr* ty) {
    return [env, ty](Formatter& fmt) {
      out_type::prepare_for_printing({});
      out_type::ExpansionPair ty_exp = expand_type(env, ty);
      ty_exp = out_type::prepare_expansion(ty_exp);
      printtyp::type_expansion(Mode::Type, fmt, ty_exp);
    };
  };
  auto msg = [](auto&&... a) { return doc_printf(std::forward<decltype(a)>(a)...); };
  switch (err.kind) {
    case EK::Constructor_arity_mismatch:
      return location::errorf(
          loc, "@[The constructor %a@ expects %i argument(s),@ but is applied here to %i argument(s)@]",
          quoted_constr(err.lid), err.n1, err.n2);
    case EK::Constructor_labeled_arg:
      return location::errorf(loc,
                              "Constructors cannot have labeled arguments.@ Consider using an inline record instead.");
    case EK::Partial_tuple_pattern_bad_type:
      return location::errorf(loc, "Could not determine the type of this partial tuple pattern.");
    case EK::Extra_tuple_label:
      return location::errorf(
          loc, "This pattern was expected to match values of type@ %a,@ but it contains an extra %a.",
          print_expanded(err.ty), [&](Formatter& f) { tuple_component(false, f, err.optlabel); });
    case EK::Missing_tuple_label: {
      bool labeled = err.optlabel.some;
      return location::errorf(
          loc, "This pattern was expected to match values of type@ %a,@ but it is missing %a.%a",
          print_expanded(err.ty), [&](Formatter& f) { tuple_component(true, f, err.optlabel); },
          [labeled](Formatter& f) {
            if (labeled) fprintf(f, "@ Hint: use .. to ignore some components.");
          });
    }
    case EK::Label_mismatch:
      return report_unification_error(loc, {}, env, err.trace, Doc{},
                                      msg("The record field %a@ belongs to the type", quoted_longident(err.lid)),
                                      msg("but is mixed here with fields of type"));
    case EK::Pattern_type_clash: {
      auto diff = errortrace_report::type_clash_of_trace(err.trace.trace);
      std::vector<Msg> sub = report_pattern_type_clash_hints(err.sdesc_for_hint, diff);
      return report_unification_error(loc, std::move(sub), env, err.trace, Doc{},
                                      msg("This pattern matches values of type"),
                                      msg("but a pattern was expected which matches values of type"));
    }
    case EK::Or_pattern_type_clash:
      return report_unification_error(
          loc, {}, env, err.trace, Doc{},
          msg("The variable %a on the left-hand side of this or-pattern has type",
              code_str(std::string(ident::name(err.id)))),
          msg("but on the right-hand side it has type"));
    case EK::Multiply_bound_variable:
      return location::errorf(loc, "Variable %a is bound several times in this matching", code_str(err.name));
    case EK::Orpat_vars:
      return location::aligned_error_hint(
          loc, {},
          doc_printf("@{<ralign>Variable @}%a must occur on both sides of this %a pattern",
                     code_str(std::string(ident::name(err.id))), code_str("|")),
          spellcheck_idents(err.id, err.ids));
    case EK::Expr_type_clash: {
      auto diff = errortrace_report::type_clash_of_trace(err.trace.trace);
      std::vector<Msg> sub = report_expr_type_clash_hints(err.sexp, diff);
      Doc expl = report_type_expected_explanation_opt(err.explanation);
      return report_unification_error(
          loc, std::move(sub), env, err.trace, expl,
          msg("%a", [&](Formatter& f) { report_this_pexp_has_type(nullptr, f, err.sexp); }),
          msg("but an expression was expected of type"));
    }
    case EK::Function_arity_type_clash: {
      TypeExpr* type_with_local_equation = nullptr;
      for (std::size_t i = err.trace.trace.size(); i-- > 0;)
        if (err.trace.trace[i].kind == et::Elt<et::ExpandedType>::Kind::Diff) {
          type_with_local_equation = err.trace.trace[i].diff.expected.ty;
          break;
        }
      return location::errorf(
          loc,
          "@[@[The syntactic arity of the function doesn't match the type constraint:@ @[<2>This function has %d "
          "syntactic arguments, but its type is constrained to@ %a.@]@ @]@ @[@[<2>@{<hint>Hint@}: consider "
          "splitting the function definition into@ %a@ where %a is the pattern with the GADT constructor that@ "
          "introduces the local type equation%t.@]",
          err.n1, print_expanded(err.ty), code_str("fun ... gadt_pat -> fun ..."), code_str("gadt_pat"),
          [&](Formatter& f) {
            if (type_with_local_equation) fprintf(f, " on %a", print_expanded(type_with_local_equation));
          });
    }
    case EK::Apply_non_function: {
      DescKind k = types::get_desc(err.ty)->kind;
      if (k == DescKind::Tarrow || k == DescKind::Tfunctor) {
        bool returns_unit = false;
        if (auto* c = as<Tconstr>(types::get_desc(err.ty2))) returns_unit = path::same(c->path, predef::paths().unit);
        out_type::ExpansionPair func_ty = expand_type(env, err.ty);
        return report_too_many_arg_error(err.texp, func_ty, err.loc2, err.loc3, returns_unit, loc);
      }
      return location::errorf(loc, "@[<v>@[<2>This expression has type@ %a@]@ %s@]", print_expanded(err.ty),
                              "This is not a function; it cannot be applied.");
    }
    case EK::Apply_wrong_label: {
      auto print_label = [](Formatter& f, const ArgLabel& l) {
        if (l.kind == ArgLabel::Kind::Nolabel)
          fprintf(f, "without label");
        else
          fprintf(f, "with label %a", code_str(prefixed_label_name(l)));
      };
      std::vector<Msg> extra_info;
      if (err.flag)
        extra_info.push_back(
            location::msg_noloc("Since OCaml 4.11, optional arguments do not commute when -nolabels is given"));
      return location::errorf_sub(
          loc, std::move(extra_info),
          "@[<v>@[<2>The function applied to this argument has type@ %a@]@.This argument cannot be applied %a@]",
          fd::pr(printtyp::type_expr, err.ty), fd::pr(print_label, err.label));
    }
    case EK::Label_multiply_defined:
      return location::errorf(loc, "The record field label %s is defined several times", err.name);
    case EK::Label_missing: {
      std::vector<Ident::t> labels = err.ids;
      return location::errorf(loc, "@[<hov>Some record fields are undefined:%a@]", [labels](Formatter& f) {
        for (Ident::t l : labels) fprintf(f, "@ %a", code_str(std::string(ident::name(l))));
      });
    }
    case EK::Label_not_mutable:
      return location::errorf(loc, "The record field %a is not mutable", quoted_longident(err.lid));
    case EK::Wrong_name: {
      Report r;
      printtyp::wrap_printing_env(true, env, [&] {
        const typecore::WrongName& w = err.wrong_name;
        std::vector<std::string> valid;
        for (std::string_view s : w.valid_names) valid.push_back(std::string(s));
        if (path::is_constructor_typath(w.type_path)) {
          r = location::aligned_error_hint(
              loc, {},
              doc_printf("@{<ralign>The field @}%a is not part of the record argument for the %a constructor",
                         code_str(std::string(w.name.txt)), misc::style::code(printtyp::type_path, w.type_path)),
              spellcheck(w.name.txt, valid));
        } else {
          const typecore::TypeExpected& te = err.expected;
          std::string eorp = err.name;
          auto intro = [&](Formatter& f) {
            fprintf(f, "@[%s type@;<1 2>%a%a@]@\n", eorp, print_expanded(te.ty),
                    [&](Formatter& ff) { fd::pp_doc(ff, report_type_expected_explanation_opt(te.explanation)); });
          };
          Doc main = doc_printf("@{<ralign>There is no %s @}%a within type %a",
                                w.kind == typecore::DatatypeKind::Record ? "field" : "constructor",
                                code_str(std::string(w.name.txt)), misc::style::code(printtyp::type_path, w.type_path));
          std::vector<Msg> sub;
          if (std::optional<Doc> hint = spellcheck(w.name.txt, valid)) {
            auto [m, h] = misc::align_error_hint(main, *hint);
            main = m;
            sub.push_back(location::mknoloc_msg(h));
          }
          r = location::errorf_sub(loc, std::move(sub), "%t%a", intro, [&](Formatter& f) { fd::pp_doc(f, main); });
        }
      });
      return r;
    }
    case EK::Name_type_mismatch: {
      bool record = err.dkind == typecore::DatatypeKind::Record;
      const char* type_name = record ? "record" : "variant";
      const char* name = record ? "field" : "constructor";
      auto pr = [&](Formatter& f) {
        if (record)
          misc::style::as_inline_code(pprintast::longident, f, err.lid);
        else
          misc::style::as_inline_code(pprintast::constr, f, err.lid);
      };
      return location::errorf(loc, "%t", [&](Formatter& ppf) {
        // (the three messages: application arguments, right to left)
        Doc t3 = msg("but a %s was expected belonging to the %s type", name, type_name);
        Doc t2 = msg("The %s %a@ belongs to one of the following %s types:", name, pr, type_name);
        Doc t1 = msg("The %s %a@ belongs to the %s type", name, pr, type_name);
        errortrace_report::ambiguous_type(ppf, env, err.tp, err.tpl, t1, t2, t3);
      });
    }
    case EK::Invalid_format: return location::errorf(loc, "%s", err.name);
    case EK::Not_an_object:
      return location::errorf(loc, "This expression is not an object;@ it has type %a%a", print_expanded(err.ty),
                              [&](Formatter& f) { fd::pp_doc(f, report_type_expected_explanation_opt(err.explanation)); });
    case EK::Undefined_method: {
      Report r;
      printtyp::wrap_printing_env(true, env, [&] {
        TypeExpr* ty = err.texp->exp_type;
        auto intro = [&](Formatter& f) {
          fprintf(f, "@[<v>@[This expression has type@;<1 2>%a@]@,@]", print_expanded(ty));
        };
        Doc main = doc_printf("@{<ralign>It has no method @}%a", code_str(err.name));
        std::vector<Msg> sub;
        std::optional<Doc> hint;
        if (err.has_names) hint = spellcheck(err.name, err.names);
        if (hint) {
          auto [m, h] = misc::align_error_hint(main, *hint);
          main = m;
          sub.push_back(location::mknoloc_msg(h));
        }
        r = location::errorf_sub(loc, std::move(sub), "%t%a", intro, [&](Formatter& f) { fd::pp_doc(f, main); });
      });
      return r;
    }
    case EK::Undefined_self_method:
      return location::aligned_error_hint(loc, {},
                                          doc_printf("@{<ralign>This expression has no method @}%a", code_str(err.name)),
                                          spellcheck(err.name, err.names));
    case EK::Virtual_class:
      return location::errorf(loc, "Cannot instantiate the virtual class %a", quoted_longident(err.lid));
    case EK::Unbound_instance_variable:
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>Unbound instance variable @}%a", code_str(err.name)),
          spellcheck(err.name, err.names));
    case EK::Instance_variable_not_mutable:
      return location::errorf(loc, "The instance variable %a is not mutable", code_str(err.name));
    case EK::Not_subtype:
      return location::errorf(loc, "%t",
                              [&](Formatter& ppf) { errortrace_report::subtype(ppf, env, err.subtype, "is not a subtype of"); });
    case EK::Outside_class:
      return location::errorf(loc, "This object duplication occurs outside a method definition");
    case EK::Value_multiply_overridden:
      return location::errorf(loc, "The instance variable %a is overridden several times", code_str(err.name));
    case EK::Coercion_failure: {
      Doc intro;
      {
        out_type::ExpansionPair ty_exp = out_type::prepare_expansion({err.expanded.ty, err.expanded.expanded});
        intro = doc_printf("This expression cannot be coerced to type@;<1 2>%a;@ it has type",
                           misc::style::code(
                               [](Formatter& f, out_type::ExpansionPair p) { printtyp::type_expansion(Mode::Type, f, p); },
                               ty_exp));
      }
      std::vector<Msg> sub;
      if (err.flag) {
        // (the list's elements: right to left)
        Msg m2 = location::msg_noloc("@{<hint>Hint@}: Consider using a fully explicit coercion@ of the form: %a",
                                     code_str("(foo : ty1 :> ty2)"));
        Msg m1 = location::msg_noloc("This simple coercion was not fully general");
        sub = {m1, m2};
      }
      return location::errorf_sub(loc, std::move(sub), "%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, env, err.trace, intro, doc_printf("but is here used with type"));
      });
    }
    case EK::Not_a_function:
      return location::errorf(loc, "This expression should not be a function,@ the expected type is@ %a%a",
                              print_expanded(err.ty),
                              [&](Formatter& f) { fd::pp_doc(f, report_type_expected_explanation_opt(err.explanation)); });
    case EK::Too_many_arguments:
      return location::errorf(loc, "This function expects too many arguments,@ it should have type@ %a%a",
                              print_expanded(err.ty),
                              [&](Formatter& f) { fd::pp_doc(f, report_type_expected_explanation_opt(err.explanation)); });
    case EK::Abstract_wrong_label: {
      const ArgLabel &got = err.label, &expected = err.label2;
      auto label = [](bool long_, const ArgLabel& l) {
        return [long_, l](Formatter& f) {
          if (l.kind == ArgLabel::Kind::Nolabel)
            fprintf(f, "unlabeled");
          else if (long_)
            fprintf(f, "labeled %a", code_str(prefixed_label_name(l)));
          else
            misc::style::inline_code(f, prefixed_label_name(l));
        };
      };
      bool second_long = got.kind == ArgLabel::Kind::Nolabel || expected.kind == ArgLabel::Kind::Nolabel;
      return location::errorf(
          loc,
          "@[<v>@[<2>This function should have type@ %a%a@]@,@[but its first argument is %a@ instead of %s%a@]@]",
          print_expanded(err.ty),
          [&](Formatter& f) { fd::pp_doc(f, report_type_expected_explanation_opt(err.explanation)); },
          label(true, got), second_long ? "being " : "", label(second_long, expected));
    }
    case EK::Private_type:
      return location::errorf(loc, "Cannot create values of the private type %a", print_expanded(err.ty));
    case EK::Private_label:
      return location::errorf(loc, "Cannot assign field %a of the private type %a", quoted_longident(err.lid),
                              print_expanded(err.ty));
    case EK::Private_constructor:
      return location::errorf(loc, "Cannot use private constructor %a to create values of type %a",
                              code_str(std::string(err.cstr->cstr_name)), print_expanded(err.ty));
    case EK::Not_a_polymorphic_variant_type:
      return location::errorf(loc, "The type %a@ is not a variant type", quoted_longident(err.lid));
    case EK::Incoherent_label_order:
      return location::errorf(loc,
                              "This function is applied to arguments@ in an order different from other calls.@ This "
                              "is only allowed when the real type is known.");
    case EK::Less_general:
      return report_unification_error(loc, {}, env, err.trace, Doc{}, doc_printf("This %s has type", err.name),
                                      doc_printf("which is less general than"));
    case EK::Modules_not_allowed: return location::errorf(loc, "Modules are not allowed in this pattern.");
    case EK::Cannot_infer_signature:
      return location::errorf(loc, "The signature for this packaged module couldn't be inferred.");
    case EK::Not_a_packed_module:
      return location::errorf(loc, "This expression is packed module, but the expected type is@ %a",
                              print_expanded(err.ty));
    case EK::Unexpected_existential: {
      using R = typecore::ExistentialRestriction;
      auto reason_str = [&](Formatter& f) {
        switch (err.restriction) {
          case R::In_class_args: fprintf(f, "Existential types are not allowed in class arguments"); break;
          case R::In_class_def:
            fprintf(f, "Existential types are not allowed in bindings inside class definition");
            break;
          case R::In_self_pattern: fprintf(f, "Existential types are not allowed in self patterns"); break;
          case R::At_toplevel: fprintf(f, "Existential types are not allowed in toplevel bindings"); break;
          case R::In_group:
            fprintf(f, "Existential types are not allowed in grouped (%a) bindings", code_str("let ... and ..."));
            break;
          case R::In_rec: fprintf(f, "Existential types are not allowed in recursive bindings"); break;
          case R::With_attributes:
            fprintf(f, "Existential types are not allowed in presence of attributes");
            break;
        }
      };
      return location::errorf(loc, "%t,@ but the constructor %a introduces existential types.", reason_str,
                              code_str(err.name));
    }
    case EK::Invalid_interval:
      return location::errorf(loc, "@[Only character intervals are supported in patterns.@]");
    case EK::Invalid_for_loop_index:
      return location::errorf(loc, "@[Invalid for-loop index: only variables and %a are allowed.@]", code_str("_"));
    case EK::No_value_clauses:
      return location::errorf(loc, "None of the patterns in this %a expression match values.", code_str("match"));
    case EK::Exception_pattern_disallowed:
      return location::errorf(loc, "@[Exception patterns are not allowed in this position.@]");
    case EK::Mixed_value_and_exception_patterns_under_guard:
      return location::errorf(loc, "@[Mixing value and exception patterns under when-guards is not supported.@]");
    case EK::Effect_pattern_below_toplevel:
      return location::errorf(loc, "@[Effect patterns must be at the top level of a match case.@]");
    case EK::Invalid_continuation_pattern:
      return location::errorf(loc, "@[Invalid continuation pattern: only variables and _ are allowed .@]");
    case EK::Inlined_record_escape:
      return location::errorf(loc, "@[This form is not allowed as the type of the inlined record could escape.@]");
    case EK::Inlined_record_expected:
      return location::errorf(loc, "@[This constructor expects an inlined record argument.@]");
    case EK::Unrefuted_pattern:
      return location::errorf(loc, "@[%s@ %s@ @[%a@]@]", "This match case could not be refuted.",
                              "Here is an example of a value that would reach it:",
                              misc::style::code(printpat::top_pretty, err.pat));
    case EK::Invalid_extension_constructor_payload:
      return location::errorf(loc, "Invalid %a payload, a constructor is expected.",
                              code_str("[%extension_constructor]"));
    case EK::Not_an_extension_constructor:
      return location::errorf(loc, "This constructor is not an extension constructor.");
    case EK::Invalid_atomic_loc_payload:
      return location::errorf(loc, "Invalid %a payload, a record field access is expected.",
                              code_str("[%atomic.loc]"));
    case EK::Label_not_atomic:
      return location::errorf(loc, "The record field %a is not atomic", quoted_longident(err.lid));
    case EK::Atomic_in_pattern:
      return location::errorf(
          loc,
          "Atomic fields (here %a) are forbidden in patterns,@ as it is difficult to reason about when the atomic "
          "read@ will happen during pattern matching:@ the field may be read@ zero, one or several times depending "
          "on the patterns around it.",
          quoted_longident(err.lid));
    case EK::Literal_overflow:
      return location::errorf(loc, "Integer literal exceeds the range of representable integers of type %a",
                              code_str(err.name));
    case EK::Unknown_literal: {
      std::string n = err.name;
      char m = err.c;
      return location::errorf(loc, "Unknown modifier %a for literal %a",
                              misc::style::code([](Formatter& f, char c) { fd::pp_print_char(f, c); }, m),
                              misc::style::code([](Formatter& f, std::pair<std::string, char> p) {
                                fprintf(f, "%s%c", p.first, p.second);
                              }, std::make_pair(n, m)));
    }
    case EK::Illegal_letrec_pat:
      return location::errorf(loc, "Only variables are allowed as left-hand side of %a", code_str("let rec"));
    case EK::Illegal_letrec_expr:
      return location::errorf(loc, "This kind of expression is not allowed as right-hand side of %a",
                              code_str("let rec"));
    case EK::Illegal_class_expr:
      return location::errorf(loc, "This kind of recursive class expression is not allowed");
    case EK::Letop_type_clash:
    case EK::Andop_type_clash:
      return report_unification_error(loc, {}, env, err.trace, Doc{},
                                      msg("The operator %a has type", code_str(err.name)),
                                      msg("but it was expected to have type"));
    case EK::Bindings_type_clash:
      return report_unification_error(loc, {}, env, err.trace, Doc{}, doc_printf("These bindings have type"),
                                      doc_printf("but bindings were expected of type"));
    case EK::Unbound_existential: {
      std::vector<Ident::t> ids = err.ids;
      TypeExpr* ty = err.ty;
      auto pp_type = [](Formatter& f, std::pair<std::vector<Ident::t>, TypeExpr*> p) {
        fprintf(f, "@[type %a.@ %a@]@]",
                [&](Formatter& ff) {
                  fd::pp_print_list(ff, [](Formatter& g, Ident::t id) { fd::pp_print_string(g, ident::name(id)); },
                                    p.first, fd::pp_print_space);
                },
                fd::pr(printtyp::type_expr, p.second));
      };
      return location::errorf(loc, "@[<2>%s:@ %a@]", "This type does not bind all existentials in the constructor",
                              misc::style::code(pp_type, std::make_pair(ids, ty)));
    }
    case EK::Bind_existential: {
      const char *reason1, *reason2;
      switch (err.binding) {
        case typecore::ExistentialBinding::Bind_already_bound:
          reason1 = "the name";
          reason2 = "that is already bound";
          break;
        case typecore::ExistentialBinding::Bind_not_in_scope:
          reason1 = "the name";
          reason2 = "that was defined before";
          break;
        default:
          reason1 = "the type";
          reason2 = "that is not a locally abstract type";
          break;
      }
      return location::errorf(loc, "@[<hov0>The local name@ %a@ %s@ %s.@ %s@ %s@ %a@ %s.@]",
                              misc::style::code(printtyp::ident, err.id),
                              "can only be given to an existential variable",
                              "introduced by this GADT constructor", "The type annotation tries to bind it to",
                              reason1, print_expanded(err.ty), reason2);
    }
    case EK::Missing_type_constraint:
      return location::errorf(loc, "@[%s@ %s@]", "Existential types introduced in a constructor pattern",
                              "must be bound by a type constraint on the argument.");
    case EK::Wrong_expected_kind: {
      const char* ctx = err.ctx.is_pattern ? "pattern" : "expression";
      typecore::Explanation explanation = err.ctx.is_pattern ? std::nullopt : err.ctx.explanation;
      const char* sort = "";
      switch (err.sort) {
        case typecore::WrongKindSort::Constructor: sort = "constructor"; break;
        case typecore::WrongKindSort::Boolean: sort = "boolean literal"; break;
        case typecore::WrongKindSort::List: sort = "list literal"; break;
        case typecore::WrongKindSort::Unit: sort = "unit literal"; break;
        case typecore::WrongKindSort::Record: sort = "record"; break;
      }
      return location::errorf(loc, "This %s should not be a %s,@ the expected type is@ %a%a", ctx, sort,
                              print_expanded(err.ty),
                              [&](Formatter& f) { fd::pp_doc(f, report_type_expected_explanation_opt(explanation)); });
    }
    case EK::Expr_not_a_record_type:
      return location::errorf(loc, "This expression has type %a@ which is not a record type.",
                              print_expanded(err.ty));
    case EK::Repeated_tuple_exp_label:
      return location::errorf(loc, "@[This tuple expression has two labels named %a@]", code_str(err.name));
    case EK::Repeated_tuple_pat_label:
      return location::errorf(loc, "@[This tuple pattern has two labels named %a@]", code_str(err.name));
    case EK::Optional_poly_param:
      return location::errorf(loc, "@[The optional parameter %a cannot have a polymorphic type.@]",
                              code_str(err.name));
    case EK::Cannot_unify_tfunctor_to_tarrow: {
      std::vector<Msg> sub{location::msg_noloc(
          "@[This function is module-dependent. The dependency is preserved@ when the function is passed a static "
          "module argument %a@ or %a. Its argument here is not static, so the type-checker@ tried instead to change "
          "the function type to be non-dependent.@]",
          code_str("(module M : S)"), code_str("(module M)"))};
      return report_unification_error(loc, std::move(sub), env, err.trace, Doc{}, msg("This expression has type"),
                                      msg("but an expression was expected of type"));
    }
    case EK::Cannot_omit_tfunctor_argument:
      return location::errorf(
          loc, "@[<v>@[<2>This function has type@ %a@]@ The module argument %a cannot be omitted in this application.@]",
          print_expanded(err.ty), code_str(std::string(ident::Unscoped::name_of(err.us))));
  }
  return location::errorf(loc, "?");
}

}  // namespace

// Builtin_attributes.error_of_extension
Report error_of_extension(const pt::Extension* ext);

void register_typecore() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const typecore::Error& e) {
      Report r;
      printtyp::wrap_printing_env(true, e.env, [&] { r = report_error(e.loc, e.env, e); });
      return r;
    } catch (const typecore::ErrorForward& e) {
      return error_of_extension(e.ext);
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
