// See errortrace_report.hpp.
#include "cppcaml/typing/errortrace_report.hpp"

#include <algorithm>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/printtyp.hpp"

namespace cppcaml::typing::errortrace_report {

namespace et = errortrace;
namespace fd = format_doc;
using format_doc::doc_printf;
using format_doc::fprintf;
using out_type::Mode;
using ET = et::ExpandedType;
using EltE = et::Elt<ET>;
using DiffE = et::Diff<ET>;
using DiffT = et::Diff<out_type::ExpansionDiff>;

void print_pos(Formatter& ppf, et::Position pos) {
  fprintf(ppf, pos == et::Position::First ? "first" : "second");
}

namespace {

enum class TraceFormat { Unification, Equality, Moregen };

const char* incompatibility_phrase(TraceFormat f) {
  switch (f) {
    case TraceFormat::Unification: return "is not compatible with type";
    case TraceFormat::Equality: return "is not equal to type";
    case TraceFormat::Moregen: return "is not compatible with type";
  }
  return "";
}

out_type::ExpansionPair pair_of(const ET& e) { return {e.ty, e.expanded}; }
ET et_of(const out_type::ExpansionPair& e) { return {e.ty, e.expanded}; }

// map_diff (trees_of_type_expansion mode): got first
DiffT trees_of_diff(Mode mode, const DiffE& d) {
  out_type::ExpansionDiff got = out_type::trees_of_type_expansion(mode, pair_of(d.got));
  out_type::ExpansionDiff expected = out_type::trees_of_type_expansion(mode, pair_of(d.expected));
  return {got, expected};
}
std::vector<DiffT> trees_of_trace(Mode mode, const std::vector<DiffE>& tr) {
  std::vector<DiffT> r;
  for (const DiffE& d : tr) r.push_back(trees_of_diff(mode, d));
  return r;
}
DiffE prepare_expansion_diff(const DiffE& d) {
  ET got = et_of(out_type::prepare_expansion(pair_of(d.got)));
  ET expected = et_of(out_type::prepare_expansion(pair_of(d.expected)));
  return {got, expected};
}

void trace(bool fst, std::string_view txt, Formatter& ppf, const std::vector<DiffT>& tr) {
  bool first = fst;
  for (const DiffT& d : tr) {
    if (!first) fprintf(ppf, "@,");
    first = false;
    fprintf(ppf, "@[Type@;<1 2>%a@ %s@;<1 2>%a@]", [&](Formatter& f) { out_type::pp_type_expansion(f, d.got); }, txt,
            [&](Formatter& f) { out_type::pp_type_expansion(f, d.expected); });
  }
}

enum class Status { Discard, Keep, Optional_refinement };

Status diff_printing_status(const DiffE& d) {
  if (btype::is_constr_row(true, d.got.expanded) || btype::is_constr_row(true, d.expected.expanded))
    return Status::Discard;
  if (out_type::same_path(d.got.ty, d.got.expanded) && out_type::same_path(d.expected.ty, d.expected.expanded))
    return Status::Optional_refinement;
  return Status::Keep;
}

Status printing_status(const EltE& e) {
  if (e.kind == EltE::Kind::Diff) return diff_printing_status(e.diff);
  return Status::Keep;
}

// prepare_any_trace printing_status tr
template <class T, class S>
std::vector<T> prepare_any_trace(S status, const std::vector<T>& tr) {
  if (tr.empty()) return {};
  // elt :: List.fold_right clean_trace rem []
  std::vector<T> acc;  // in reverse (the fold's list is acc reversed)
  for (std::size_t i = tr.size(); i-- > 1;) {
    const T& x = tr[i];
    switch (status(x)) {
      case Status::Keep: acc.push_back(x); break;
      case Status::Optional_refinement:
        if (acc.empty()) acc.push_back(x);
        break;
      case Status::Discard: break;
    }
  }
  std::vector<T> r{tr[0]};
  for (std::size_t i = acc.size(); i-- > 0;) r.push_back(acc[i]);
  return r;
}

// prepare_trace f tr: Errortrace.map f, then prepare_any_trace
template <class F>
et::ErrorTrace prepare_trace(F f, const et::ErrorTrace& tr) {
  et::ErrorTrace mapped;
  for (const EltE& e : tr) mapped.push_back(et::map_elt<ET, ET>(f, e));
  return prepare_any_trace(printing_status, mapped);
}

// filter_trace: the Diffs, the last one split off when elidable
std::pair<std::vector<DiffE>, std::optional<DiffE>> filter_trace(const et::ErrorTrace& tr) {
  std::vector<DiffE> ds;
  for (std::size_t i = 0; i < tr.size(); ++i) {
    const EltE& e = tr[i];
    if (e.kind != EltE::Kind::Diff) continue;
    if (i + 1 == tr.size() && printing_status(e) == Status::Optional_refinement) return {ds, e.diff};
    ds.push_back(e.diff);
  }
  return {ds, std::nullopt};
}

ET may_prepare_expansion(bool compact, const ET& ty_exp) {
  DescKind k = types::get_desc(ty_exp.expanded)->kind;
  if ((k == DescKind::Tvariant || k == DescKind::Tobject) && compact) {
    out_type::reserve_names(ty_exp.ty);
    return {ty_exp.ty, ty_exp.ty};
  }
  return et_of(out_type::prepare_expansion(pair_of(ty_exp)));
}

void print_path(Formatter& ppf, Path::t p) {
  oprint::out_ident(ppf, out_type::namespaced_tree_of_path(out_type::Namespace::Type, p));
}

void print_tag(Formatter& ppf, std::string_view s) { misc::style::inline_code(ppf, "`" + std::string(s)); }

void print_tags(Formatter& ppf, const std::vector<std::string_view>& tags) {
  fd::pp_print_list(ppf, [](Formatter& f, std::string_view s) { print_tag(f, s); }, tags, fd::comma);
}

bool is_unit_param(env::t env, TypeExpr* ty0) {
  auto [ty, vars] = btype::tpoly_get_poly(ty0);
  if (!vars.empty()) return false;
  if (auto* c = as<Tconstr>(types::get_desc(ctype::expand_head(env, ty))))
    return path::same(c->path, predef::paths().unit);
  return false;
}

bool unifiable(env::t env, TypeExpr* ty1, TypeExpr* ty2) {
  types::Snapshot snap = btype::snapshot();
  bool res;
  try {
    ctype::unify(env, ty1, ty2);
    res = true;
  } catch (const ctype::Unify&) {
    res = false;
  }
  btype::backtrack(snap);
  return res;
}

std::optional<Doc> explanation_diff(env::t env, TypeExpr* t3, TypeExpr* t4) {
  const TypeDesc* d3 = types::get_desc(t3);
  const TypeDesc* d4 = types::get_desc(t4);
  if (auto* a = as<Tarrow>(d3); a && is_unit_param(env, a->t1) && unifiable(env, a->t2, t4))
    return doc_printf("@,@[@{<hint>Hint@}: Did you forget to provide %a as argument?@]", misc::style::code_str("()"));
  if (auto* a = as<Tarrow>(d4); a && is_unit_param(env, a->t1) && unifiable(env, t3, a->t2))
    return doc_printf("@,@[@{<hint>Hint@}: Did you forget to wrap the expression using %a?@]",
                      misc::style::code_str("fun () ->"));
  return std::nullopt;
}

Doc explain_fixed_row_case(const et::FixedRowCase& c) {
  if (c.kind == et::FixedRowCaseKind::Cannot_be_closed) return doc_printf("it cannot be closed");
  return doc_printf("it may not allow the tag(s) %a", [&](Formatter& f) { print_tags(f, c.tags); });
}

void pp_path(Formatter& ppf, Path::t p) { misc::style::as_inline_code(printtyp::path, ppf, p); }

Doc explain_fixed_row(et::Position pos, const FixedExplanation& expl) {
  switch (expl.kind) {
    case FixedExplanation::Kind::Fixed_private:
      return doc_printf("The %a variant type is private", [&](Formatter& f) { print_pos(f, pos); });
    case FixedExplanation::Kind::Univar:
      out_type::reserve_names(expl.univar);
      return doc_printf("The %a variant type is bound to the universal type variable %a",
                        [&](Formatter& f) { print_pos(f, pos); },
                        misc::style::code(out_type::type_expr_with_reserved_names, expl.univar));
    case FixedExplanation::Kind::Reified:
      return doc_printf("The %a variant type is bound to %a", [&](Formatter& f) { print_pos(f, pos); },
                        misc::style::code(
                            [](Formatter& f, Path::t p) {
                              out_type::internal_names::add(p);
                              print_path(f, p);
                            },
                            expl.reified));
    case FixedExplanation::Kind::Rigid: return Doc{};
  }
  return Doc{};
}

std::optional<Doc> explain_variant(const et::Variant& v) {
  using K = et::Variant::Kind;
  switch (v.kind) {
    case K::Incompatible_types_for:
      return doc_printf("@,Types for tag %a are incompatible", [&](Formatter& f) { print_tag(f, v.name); });
    case K::No_intersection: return doc_printf("@,These two variant types have no intersection");
    case K::No_tags: {
      std::vector<std::string_view> tags;
      for (auto& fe : v.tags) tags.push_back(fe.label);
      return doc_printf("@,@[The %a variant type does not allow tag(s)@ @[<hov>%a@]@]",
                        [&](Formatter& f) { print_pos(f, v.pos); }, [&](Formatter& f) { print_tags(f, tags); });
    }
    case K::Fixed_row: {
      if (v.fixed->kind == FixedExplanation::Kind::Rigid) return std::nullopt;  // never happens
      // (arguments evaluated right to left)
      Doc c = explain_fixed_row_case(v.fixed_case);
      Doc e = explain_fixed_row(v.pos, *v.fixed);
      return doc_printf("@,@[%a,@ %a@]", [&](Formatter& f) { fd::pp_doc(f, e); },
                        [&](Formatter& f) { fd::pp_doc(f, c); });
    }
    case K::Presence_not_guaranteed_for:
      return doc_printf(
          "@,@[The tag %a is guaranteed to be present in the %a variant type,@ but not in the %a@]",
          [&](Formatter& f) { print_tag(f, v.name); },
          [&](Formatter& f) { print_pos(f, et::swap_position(v.pos)); }, [&](Formatter& f) { print_pos(f, v.pos); });
    case K::Openness:
      return doc_printf("@,The %a variant type is open and the %a is not", [&](Formatter& f) { print_pos(f, v.pos); },
                        [&](Formatter& f) { print_pos(f, et::swap_position(v.pos)); });
  }
  return std::nullopt;
}

std::optional<Doc> explain_escape(const Doc& pre, const et::Escape<ET>& e) {
  using K = et::Escape<ET>::Kind;
  auto ppre = [&](Formatter& f) { fd::pp_doc(f, pre); };
  switch (e.kind) {
    case K::Univ:
      out_type::reserve_names(e.univ);
      return doc_printf("%a@,The universal variable %a would escape its scope", ppre,
                        misc::style::code(out_type::type_expr_with_reserved_names, e.univ));
    case K::Constructor:
      return doc_printf("%a@,@[The type constructor@;<1 2>%a@ would escape its scope@]", ppre,
                        fd::pr(pp_path, e.path));
    case K::Module_type:
      return doc_printf("%a@,@[The module type@;<1 2>%a@ would escape its scope@]", ppre, fd::pr(pp_path, e.path));
    case K::Module:
      return doc_printf("%a@,@[The module@;<1 2>%a@ would escape its scope@]", ppre,
                        fd::pr(pp_path, Path::pident(e.module)));
    case K::Equation: {
      TypeExpr* t = e.equation.expanded;
      out_type::reserve_names(t);
      return doc_printf("%a@ @[<hov>This instance of %a is ambiguous:@ %s@]", ppre,
                        misc::style::code(out_type::type_expr_with_reserved_names, t),
                        "it would escape the scope of its equation");
    }
    case K::Self: return doc_printf("%a@,Self type cannot escape its class", ppre);
    case K::Constraint: return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Doc> explain_object(const et::Obj& o) {
  using K = et::Obj::Kind;
  switch (o.kind) {
    case K::Missing_field:
      return doc_printf("@,@[The %a object type has no method %a@]", [&](Formatter& f) { print_pos(f, o.pos); },
                        misc::style::code_str(std::string(o.name)));
    case K::Abstract_row:
      return doc_printf("@,@[The %a object type has an abstract row, it cannot be closed@]",
                        [&](Formatter& f) { print_pos(f, o.pos); });
    case K::Self_cannot_be_closed: return doc_printf("@,Self type cannot be unified with a closed object type");
  }
  return std::nullopt;
}

Doc explain_incompatible_fields(std::string_view name, const et::Diff<TypeExpr*>& diff) {
  out_type::reserve_names(diff.got);
  out_type::reserve_names(diff.expected);
  return doc_printf("@,@[The method %a has type@ %a,@ but the expected method type was@ %a@]",
                    misc::style::code_str(std::string(name)),
                    misc::style::code(out_type::type_expr_with_reserved_names, diff.got),
                    misc::style::code(out_type::type_expr_with_reserved_names, diff.expected));
}



Doc explain_label_mismatch(std::string_view missing_label_msg, const et::Diff<ArgLabel>& d) {
  using K = ArgLabel::Kind;
  auto quoted_label = [](const ArgLabel& l) { return misc::style::code_str(string_of_label(l)); };
  const ArgLabel &got = d.got, &expected = d.expected;
  if (got.kind == K::Nolabel && expected.kind != K::Nolabel)
    return doc_printf("@,@[A label@ %a@ was expected@]", quoted_label(expected));
  if (got.kind != K::Nolabel && expected.kind == K::Nolabel) return doc_printf(missing_label_msg, quoted_label(got));
  if (got.kind == K::Labelled && expected.kind == K::Optional && got.name == expected.name)
    return doc_printf("@,@[The label@ %a@ was expected to be optional@]", quoted_label(got));
  if (got.kind == K::Optional && expected.kind == K::Labelled && got.name == expected.name)
    return doc_printf("@,@[The label@ %a@ was expected to not be optional@]", quoted_label(got));
  return doc_printf("@,@[Labels %a@ and@ %a do not match@]", quoted_label(got), quoted_label(expected));
}

std::optional<Doc> explain_first_class_module(const et::FirstClassModule& fm) {
  using K = et::FirstClassModule::Kind;
  switch (fm.kind) {
    case K::Package_cannot_scrape:
      return doc_printf("@,@[The module alias %a could not be expanded@]", fd::pr(pp_path, fm.path));
    case K::Package_inclusion:
    case K::Package_coercion: return doc_printf("@,@[%a@]", [&](Formatter& f) { fd::pp_doc(f, fm.doc); });
  }
  return std::nullopt;
}

Doc explain_univar(const EltE* prev, const et::Univar& um) {
  if (um.is_var_mismatch) {
    Doc p = (prev && prev->kind == EltE::Kind::Incompatible_fields)
                ? explain_incompatible_fields(prev->field_name, prev->field_diff)
                : Doc{};
    out_type::add_type_to_preparation(um.diff.got);
    out_type::add_type_to_preparation(um.diff.expected);
    Doc more;
    switch (um.order) {
      case et::Order::Equal: break;
      case et::Order::Less:
        more = doc_printf(
            "@ The first type variable %a was introduced in@ an@ earlier@ universal@ quantification.",
            misc::style::code(out_type::prepared_type_expr, um.diff.got));
        break;
      case et::Order::More:
        more = doc_printf(
            "@ The second type variable %a was introduced in@ an@ earlier@ universal@ quantification.",
            misc::style::code(out_type::prepared_type_expr, um.diff.expected));
        break;
    }
    return doc_printf("%a@,@[The universal variables@ %a and@ %a@ are distinct.%a@]",
                      [&](Formatter& f) { fd::pp_doc(f, p); },
                      misc::style::code(out_type::prepared_type_expr, um.diff.got),
                      misc::style::code(out_type::prepared_type_expr, um.diff.expected),
                      [&](Formatter& f) { fd::pp_doc(f, more); });
  }
  auto pp = [](Formatter& ppf, TypeExpr* ty) {
    out_type::add_type_to_preparation(ty);
    const TypeDesc* d = types::get_desc(ty);
    if (auto* u = as<Tunivar>(d)) {
      if (!u->name.some) return;
      fprintf(ppf,
              "@,@[The universal type variable %a in the first@ type@ matches@ multiple@ distinct@ variables in the "
              "second type.@]",
              misc::style::code_str("'" + std::string(u->name.v)));
    } else if (as<Tvar>(d)) {
      fprintf(ppf, "@,@[The type variable %a is not generalizable@ to@ an@ universal@ type variable.@]",
              misc::style::code(out_type::prepared_type_expr, ty));
    } else {
      fprintf(ppf, "@,@[The type %a is not a type variable.@]", misc::style::code(out_type::prepared_type_expr, ty));
    }
  };
  return doc_printf("%a", [&](Formatter& f) {
    fd::pp_print_list(f, pp, um.quantification, [](Formatter&) {});
  });
}

std::optional<Doc> explanation(const Doc& intro, const EltE* prev, env::t env, const EltE& e) {
  using K = EltE::Kind;
  switch (e.kind) {
    case K::Diff: return explanation_diff(env, e.diff.got.expanded, e.diff.expected.expanded);
    case K::Escape: {
      Doc pre;
      if (e.escape.context) {
        out_type::reserve_names(e.escape.context);
        pre = doc_printf("@[%a@;<1 2>%a@]", [&](Formatter& f) { fd::pp_doc(f, intro); },
                         misc::style::code(out_type::type_expr_with_reserved_names, e.escape.context));
      } else if (e.escape.kind == et::Escape<ET>::Kind::Univ && prev && prev->kind == K::Incompatible_fields) {
        pre = explain_incompatible_fields(prev->field_name, prev->field_diff);
      }
      return explain_escape(pre, e.escape);
    }
    case K::Incompatible_fields: return explain_incompatible_fields(e.field_name, e.field_diff);
    case K::Function_label_mismatch:
      return explain_label_mismatch(
          "@,@[The first argument is labeled@ %a,@ but an unlabeled argument was expected@]", e.label_diff);
    case K::Tuple_label_mismatch: {
      auto ast_label = [](const OptStr& o) { return o.some ? ArgLabel::labelled(o.v) : ArgLabel::nolabel(); };
      et::Diff<ArgLabel> d{ast_label(e.tuple_label_diff.got), ast_label(e.tuple_label_diff.expected)};
      return explain_label_mismatch(
          "@,@[The first tuple element is labeled@ %a,@ but an unlabeled element was expected@]", d);
    }
    case K::Variant: return explain_variant(e.variant);
    case K::Obj: return explain_object(e.obj);
    case K::First_class_module: return explain_first_class_module(e.fcm);
    case K::Rec_occur: {
      out_type::add_type_to_preparation(e.rec1);
      out_type::add_type_to_preparation(e.rec2);
      DescKind k = types::get_desc(e.rec1)->kind;
      if (k == DescKind::Tvar || k == DescKind::Tunivar)
        return doc_printf("@,@[<hov>The type variable %a occurs inside@ %a@]",
                          misc::style::code(out_type::prepared_type_expr, e.rec1),
                          misc::style::code(out_type::prepared_type_expr, e.rec2));
      return Doc{};
    }
    case K::Univar: return explain_univar(prev, e.univar);
  }
  return std::nullopt;
}

// Errortrace.explain trace f, over the reversed trace
std::optional<Doc> mismatch(const Doc& intro, env::t env, const et::ErrorTrace& tr) {
  for (std::size_t i = tr.size(); i-- > 0;) {
    const EltE* prev = i > 0 ? &tr[i - 1] : nullptr;
    std::optional<Doc> m = explanation(intro, prev, env, tr[i]);
    if (m) return m;
    if (!prev) return std::nullopt;
  }
  return std::nullopt;
}

void warn_on_missing_def(env::t env, Formatter& ppf, TypeExpr* t) {
  auto* c = as<Tconstr>(types::get_desc(t));
  if (!c) return;
  const TypeDeclaration* decl;
  try {
    decl = env::find_type(c->path, env);
  } catch (const env::NotFound&) {
    fprintf(ppf, "@,@[<hov>Type %a is abstract because@ no corresponding@ cmi file@ was found@ in path.@]",
            fd::pr(pp_path, c->path));
    return;
  }
  if (decl->type_manifest) return;
  switch (btype::type_origin(decl).kind) {
    case TypeOrigin::Kind::Rec_check_regularity:
      fprintf(ppf,
              "@,@[<hov>Type %a was considered abstract@ when checking@ constraints@ in this@ recursive type "
              "definition.@]",
              fd::pr(pp_path, c->path));
      break;
    case TypeOrigin::Kind::Approx_recmod:
      fprintf(ppf,
              "@,@[<hov>Type %a was considered abstract@ when checking@ constraints@ in this@ recursive module "
              "definition.@]",
              fd::pr(pp_path, c->path));
      break;
    default: break;
  }
}

void quoted_ident(Formatter& ppf, const outcometree::OutIdent* t) { misc::style::as_inline_code(oprint::out_ident, ppf, t); }

void error(TraceFormat trace_format, Mode mode, const std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst,
           env::t env, const et::ErrorTrace& tr0, const Doc& txt1, Formatter& ppf, const Doc& txt2,
           const Doc& ty_expect_explanation) {
  out_type::reset();
  // We want to substitute in the opposite order from [Eqtype]
  std::vector<std::pair<TypeExpr*, TypeExpr*>> sw;
  for (auto& [a, b] : subst) sw.push_back({b, a});
  out_type::add_subst(sw);
  et::ErrorTrace full_trace =
      prepare_trace([](const ET& ty_exp) { return ET{ty_exp.ty, out_type::hide_variant_name(ty_exp.expanded)}; }, tr0);
  if (full_trace.empty()) throw std::logic_error("Errortrace_report.error");
  out_type::with_labels(!clflags::classic, [&] {
    et::ErrorTrace rest(full_trace.begin() + 1, full_trace.end());
    auto [tr1, last1] = filter_trace(rest);
    std::optional<DiffE> head;
    if (full_trace[0].kind == EltE::Kind::Diff) {
      bool compact = tr1.empty() && !last1;
      const DiffE& d = full_trace[0].diff;
      ET got = may_prepare_expansion(compact, d.got);
      ET expected = may_prepare_expansion(compact, d.expected);
      head = DiffE{got, expected};
    }
    std::vector<DiffE> tr2;
    for (const DiffE& d : tr1) tr2.push_back(prepare_expansion_diff(d));
    std::optional<DiffE> last2;
    if (last1) last2 = prepare_expansion_diff(*last1);
    Doc head_error;
    if (head) {
      DiffT d = trees_of_diff(mode, *head);
      head_error = doc_printf("%a@;<1 2>%a@ %a@;<1 2>%a", [&](Formatter& f) { fd::pp_doc(f, txt1); },
                              [&](Formatter& f) { out_type::pp_type_expansion(f, d.got); },
                              [&](Formatter& f) { fd::pp_doc(f, txt2); },
                              [&](Formatter& f) { out_type::pp_type_expansion(f, d.expected); });
    }
    std::vector<DiffT> tr3 = trees_of_trace(mode, tr2);
    std::optional<DiffT> last3;
    if (last2) last3 = trees_of_diff(mode, *last2);
    std::optional<Doc> mis = mismatch(txt1, env, full_trace);
    if (!mis && last3) tr3.push_back(*last3);
    fprintf(ppf, "@[<v>@[%a%a@]%a%a@]", [&](Formatter& f) { fd::pp_doc(f, head_error); },
            [&](Formatter& f) { fd::pp_doc(f, ty_expect_explanation); },
            [&](Formatter& f) { trace(false, incompatibility_phrase(trace_format), f, tr3); },
            [&](Formatter& f) {
              if (mis) fd::pp_doc(f, *mis);
            });
    if (!env::is_empty(env) && head) {
      warn_on_missing_def(env, ppf, head->got.ty);
      warn_on_missing_def(env, ppf, head->expected.ty);
    }
    out_type::internal_names::print_explanations(env, ppf);
    out_type::ident_conflicts::err_print(ppf);
  });
}

void report_error(TraceFormat trace_format, Formatter& ppf, Mode mode, env::t env, const et::ErrorTrace& tr,
                  const std::vector<std::pair<TypeExpr*, TypeExpr*>>& subst, const Doc& type_expected_explanation,
                  const Doc& txt1, const Doc& txt2) {
  out_type::wrap_printing_env(true, env, [&] {
    error(trace_format, mode, subst, env, tr, txt1, ppf, txt2, type_expected_explanation);
  });
}

}  // namespace

void unification(Formatter& ppf, env::t env, const et::UnificationError& err, const Doc& txt1, const Doc& txt2,
                 const Doc& type_expected_explanation) {
  report_error(TraceFormat::Unification, ppf, Mode::Type, env, err.trace, {}, type_expected_explanation, txt1, txt2);
}

void equality(Formatter& ppf, Mode mode, env::t env, const et::EqualityError& err, const Doc& txt1,
              const Doc& txt2) {
  report_error(TraceFormat::Equality, ppf, mode, env, err.trace, err.subst, Doc{}, txt1, txt2);
}

void moregen(Formatter& ppf, Mode mode, env::t env, const et::MoregenError& err, const Doc& txt1, const Doc& txt2) {
  report_error(TraceFormat::Moregen, ppf, mode, env, err.trace, {}, Doc{}, txt1, txt2);
}

void comparison(Formatter& ppf, Mode mode, env::t env, const et::ComparisonError& err, const Doc& txt1,
                const Doc& txt2) {
  if (err.is_equality)
    equality(ppf, mode, env, err.equality, txt1, txt2);
  else
    moregen(ppf, mode, env, err.moregen, txt1, txt2);
}

// ---- Subtype ----

namespace {

using SubTrace = et::subtype::Trace<ET>;

Status sub_printing_status(const DiffE& d) { return diff_printing_status(d); }

// Subtype.trace filter_trace get_diff fst keep_last txt ppf tr
template <class T, class Filter, class GetDiff>
void sub_trace(Filter filter, GetDiff get_diff, bool fst, bool keep_last, std::string_view txt, Formatter& ppf,
               const std::vector<T>& tr0) {
  out_type::with_labels(!clflags::classic, [&] {
    if (tr0.empty()) return;
    std::optional<DiffT> diffed_elt = get_diff(tr0[0]);
    std::vector<T> rest(tr0.begin() + 1, tr0.end());
    auto [tr1, last] = filter(rest);
    if (keep_last && last) tr1.push_back(*last);
    std::vector<DiffE> prepared;
    for (const DiffE& d : tr1) prepared.push_back(prepare_expansion_diff(d));
    std::vector<DiffT> tr = trees_of_trace(Mode::Type, prepared);
    if (fst && diffed_elt) tr.insert(tr.begin(), *diffed_elt);
    trace(fst, txt, ppf, tr);
  });
}

std::pair<std::vector<DiffE>, std::optional<DiffE>> filter_subtype_trace(const SubTrace& tr) {
  std::vector<DiffE> ds;
  for (std::size_t i = 0; i < tr.size(); ++i) {
    if (i + 1 == tr.size() && sub_printing_status(tr[i]) == Status::Optional_refinement) return {ds, tr[i]};
    ds.push_back(tr[i]);
  }
  return {ds, std::nullopt};
}

}  // namespace

void subtype(Formatter& ppf, env::t env, const et::subtype::Error& err, std::string_view txt1) {
  out_type::wrap_printing_env(true, env, [&] {
    out_type::reset();
    // Subtype.prepare_trace prepare_expansion tr_sub
    SubTrace mapped;
    for (const DiffE& d : err.trace) mapped.push_back(prepare_expansion_diff(d));
    SubTrace tr_sub = prepare_any_trace(sub_printing_status, mapped);
    et::ErrorTrace tr_unif = prepare_trace(
        [](const ET& e) { return et_of(out_type::prepare_expansion(pair_of(e))); }, err.unification_trace);
    bool keep_first = tr_unif.empty() ||
                      (tr_unif.size() == 1 && (tr_unif[0].kind == EltE::Kind::Obj ||
                                               tr_unif[0].kind == EltE::Kind::Variant ||
                                               tr_unif[0].kind == EltE::Kind::Escape));
    fprintf(ppf, "@[<v>%a", [&](Formatter& f) {
      sub_trace(
          filter_subtype_trace,
          [](const DiffE& d) -> std::optional<DiffT> { return trees_of_diff(Mode::Type, d); }, true, keep_first,
          txt1, f, tr_sub);
    });
    if (tr_unif.empty()) {
      fprintf(ppf, "@]");
    } else {
      std::optional<Doc> mis = mismatch(doc_printf("Within this type"), env, tr_unif);
      fprintf(
          ppf, "%a%a%t@]",
          [&](Formatter& f) {
            sub_trace(
                filter_trace,
                [](const EltE& e) -> std::optional<DiffT> {
                  if (e.kind == EltE::Kind::Diff) return trees_of_diff(Mode::Type, e.diff);
                  return std::nullopt;
                },
                false, !mis, "is not compatible with type", f, tr_unif);
          },
          [&](Formatter& f) {
            if (mis) fd::pp_doc(f, *mis);
          },
          [](Formatter& f) { out_type::ident_conflicts::err_print(f); });
    }
  });
}

// ---- ambiguous_type ----

namespace {
struct TPE {  // Same p | Diff (p, p')
  const outcometree::OutIdent* p;
  const outcometree::OutIdent* p2;  // null: Same
};
void type_path_expansion(Formatter& ppf, const TPE& t) {
  if (!t.p2)
    quoted_ident(ppf, t.p);
  else
    fprintf(ppf, "@[<2>%a@ =@ %a@]", fd::pr(quoted_ident, t.p), fd::pr(quoted_ident, t.p2));
}
TPE trees_of_type_path_expansion(std::pair<Path::t, Path::t> tp) {
  auto path_tree = [](Path::t p) { return out_type::namespaced_tree_of_path(out_type::Namespace::Type, p); };
  if (path::same(tp.first, tp.second)) return {path_tree(tp.first), nullptr};
  // (Diff (path_tree tp, path_tree tp'): right to left)
  const outcometree::OutIdent* b = path_tree(tp.second);
  const outcometree::OutIdent* a = path_tree(tp.first);
  return {a, b};
}
}  // namespace

void ambiguous_type(Formatter& ppf, env::t env, std::pair<Path::t, Path::t> tp0,
                    const std::vector<std::pair<Path::t, Path::t>>& tpl, const Doc& txt1, const Doc& txt2,
                    const Doc& txt3) {
  out_type::wrap_printing_env(true, env, [&] {
    out_type::reset();
    TPE t0 = trees_of_type_path_expansion(tp0);
    if (tpl.empty()) throw std::logic_error("Errortrace_report.ambiguous_type");
    if (tpl.size() == 1) {
      TPE t = trees_of_type_path_expansion(tpl[0]);
      fprintf(ppf, "@[%a@;<1 2>%a@ %a@;<1 2>%a@]", [&](Formatter& f) { fd::pp_doc(f, txt1); },
              fd::pr(type_path_expansion, t), [&](Formatter& f) { fd::pp_doc(f, txt3); },
              fd::pr(type_path_expansion, t0));
    } else {
      std::vector<TPE> l;
      for (auto& tp : tpl) l.push_back(trees_of_type_path_expansion(tp));
      fprintf(ppf, "@[%a@;<1 2>@[<hv>%a@]@ %a@;<1 2>%a@]", [&](Formatter& f) { fd::pp_doc(f, txt2); },
              [&](Formatter& f) {
                fd::pp_print_list(f, type_path_expansion, l, [](Formatter& ff) { fd::pp_print_break(ff, 2, 0); });
              },
              [&](Formatter& f) { fd::pp_doc(f, txt3); }, fd::pr(type_path_expansion, t0));
    }
  });
}

std::optional<et::Diff<ET>> type_clash_of_trace(const et::ErrorTrace& tr) {
  for (std::size_t i = tr.size(); i-- > 0;)
    if (tr[i].kind == EltE::Kind::Diff) return tr[i].diff;
  return std::nullopt;
}

}  // namespace cppcaml::typing::errortrace_report
