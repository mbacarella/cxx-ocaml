// Env.report_error / report_lookup_error, Typetexp.report_error,
// Builtin_attributes.error_of_extension and the small reporters (Primitive,
// Attr_helper, Syntaxerr's Variable_in_scope): see reporters.hpp.
#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/primitive.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typetexp.hpp"

namespace cppcaml::typing::reporters {

namespace fd = format_doc;
namespace pt = parsetree;
using fd::Doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using misc::style::code_str;
using out_type::Mode;

// Builtin_attributes.error_of_extension
Report error_of_extension(const pt::Extension* ext) {
  std::string_view main_txt = ext->name.txt;
  const Location& main_loc = ext->name.loc;
  auto is_error = [](std::string_view t) { return t == "ocaml.error" || t == "error"; };
  // the string of `PStr [Pstr_eval (Pexp_constant (Pconst_string s))]`-like items
  auto string_of_item = [](const pt::StructureItem* it) -> std::optional<std::string_view> {
    if (auto* ev = pt::as<pt::Pstr_eval>(it->pstr_desc))
      if (auto* c = pt::as<pt::Pexp_constant>(ev->exp->pexp_desc))
        if (c->c.pconst_desc.kind == pt::ConstantDesc::Kind::Pconst_string) return c->c.pconst_desc.s;
    return std::nullopt;
  };
  auto submessage_from = [&](const pt::StructureItem* it) -> Msg {
    if (auto* e = pt::as<pt::Pstr_extension>(it->pstr_desc)) {
      const pt::Extension* x = e->ext;
      if (is_error(x->name.txt)) {
        const pt::Payload& p = x->payload;
        if (p.kind == pt::Payload::Kind::PStr && p.str.size() == 1)
          if (std::optional<std::string_view> m = string_of_item(p.str[0])) {
            std::string msg(*m);
            return location::msg(x->name.loc, "%a", [msg](Formatter& f) { fd::pp_print_text(f, msg); });
          }
        return location::msg(x->name.loc, "Invalid syntax for sub-message of extension %a.",
                             code_str(std::string(main_txt)));
      }
      return location::msg(x->name.loc, "Uninterpreted extension '%a'.", code_str(std::string(x->name.txt)));
    }
    return location::msg(main_loc, "Invalid syntax for sub-message of extension %a.", code_str(std::string(main_txt)));
  };
  if (is_error(main_txt)) {
    const pt::Payload& p = ext->payload;
    if (p.kind == pt::Payload::Kind::PStr && p.str.empty()) throw location::AlreadyDisplayed();
    if (p.kind == pt::Payload::Kind::PStr)
      if (std::optional<std::string_view> m = string_of_item(p.str[0])) {
        std::vector<Msg> sub;
        for (std::size_t i = 1; i < p.str.size(); ++i) sub.push_back(submessage_from(p.str[i]));
        std::string msg(*m);
        return location::error_of_printer(
            main_loc, [msg](Formatter& f) { fd::pp_print_text(f, msg); }, std::move(sub));
      }
    return location::errorf(main_loc, "Invalid syntax for extension '%s'.", main_txt);
  }
  return location::errorf(main_loc, "Uninterpreted extension '%s'.", main_txt);
}

namespace {

auto quoted_longident(Longident::t lid) { return misc::style::code(pprintast::longident, lid); }
auto quoted_constr(Longident::t lid) { return misc::style::code(pprintast::constr, lid); }

// ---- Env ----

using Extract = std::function<std::vector<std::string>(Longident::t, env::t)>;

std::optional<Doc> spellcheck(const Extract& extract, env::t env, Longident::t lid) {
  switch (lid->kind) {
    case Longident::Kind::Lapply: return std::nullopt;
    case Longident::Kind::Lident: return misc::did_you_mean(misc::spellcheck(extract(nullptr, env), lid->s));
    case Longident::Kind::Ldot: {
      Longident::t r = lid->l1;
      Location rloc = lid->l1_loc;
      auto pp = [r, rloc](Formatter& ppf, const std::string& s) {
        misc::style::as_inline_code(pprintast::longident, ppf, Longident::ldot(r, rloc, s, location::none()));
      };
      return misc::did_you_mean(misc::spellcheck(extract(r, env), lid->s), pp);
    }
  }
  return std::nullopt;
}

std::vector<std::string> extract_values(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_values([&](std::string_view n, Path::t, const ValueDescription*) { r.push_back(std::string(n)); }, p, env);
  return r;
}
std::vector<std::string> extract_types(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_types([&](std::string_view n, Path::t, const TypeDeclaration*) { r.push_back(std::string(n)); }, p, env);
  return r;
}
std::vector<std::string> extract_modules(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_modules([&](std::string_view n, Path::t, const ModuleDeclaration*) { r.push_back(std::string(n)); }, p,
                    env);
  return r;
}
std::vector<std::string> extract_constructors(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_constructors([&](const ConstructorDescription* c) { r.push_back(std::string(c->cstr_name)); }, p, env);
  return r;
}
std::vector<std::string> extract_labels(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_labels([&](const LabelDescription* l) { r.push_back(std::string(l->lbl_name)); }, p, env);
  return r;
}
std::vector<std::string> extract_classes(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_classes([&](std::string_view n, Path::t, const ClassDeclaration*) { r.push_back(std::string(n)); }, p,
                    env);
  return r;
}
std::vector<std::string> extract_modtypes(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_modtypes([&](std::string_view n, Path::t, const ModtypeDeclaration*) { r.push_back(std::string(n)); }, p,
                     env);
  return r;
}
std::vector<std::string> extract_cltypes(Longident::t p, env::t env) {
  std::vector<std::string> r;
  env::fold_cltypes([&](std::string_view n, Path::t, const ClassTypeDeclaration*) { r.push_back(std::string(n)); },
                    p, env);
  return r;
}
std::vector<std::string> extract_instance_variables(env::t env) {
  std::vector<std::string> r;
  env::fold_values(
      [&](std::string_view n, Path::t, const ValueDescription* d) {
        if (d->val_kind.kind == ValueKind::Kind::Val_ivar) r.push_back(std::string(n));
      },
      nullptr, env);
  return r;
}

void pp_path(Formatter& ppf, Path::t p) { printtyp::path(ppf, p); }

bool current_unit_is_path(Path::t p) {
  if (p->kind != Path::Kind::Pident) return false;
  return ident::persistent(p->id) && env::get_current_unit_name() == ident::name(p->id);
}

Report report_lookup_error(const Location& loc, env::t env, const env::LookupError& e) {
  using K = env::LookupError::Kind;
  switch (e.kind) {
    case K::Unbound_value: {
      std::vector<Msg> sub;
      if (e.missing_rec) {
        long line = e.hint_loc.loc_start.pos_lnum;
        sub.push_back(location::msg_noloc(
            "@[@{<hint>Hint@}: If this is a recursive definition,@ you should add the %a keyword on line %i@]",
            code_str("rec"), line));
      }
      // (aligned_error_hint's arguments, right to left: the hint first)
      std::optional<Doc> hint = spellcheck(extract_values, env, e.lid);
      return location::aligned_error_hint(loc, std::move(sub),
                                          doc_printf("@{<ralign>Unbound value @}%a", quoted_longident(e.lid)), hint);
    }
    case K::Unbound_type: {
      std::optional<Doc> hint = spellcheck(extract_types, env, e.lid);
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>Unbound type constructor @}%a", quoted_longident(e.lid)), hint);
    }
    case K::Unbound_module: {
      Doc main = doc_printf("@{<ralign>Unbound module @}%a", quoted_longident(e.lid));
      bool found = true;
      try {
        env::find_modtype_by_name(e.lid, env);
      } catch (const env::NotFound&) {
        found = false;
      }
      if (!found)
        return location::aligned_error_hint(loc, {}, doc_printf("%a", [&](Formatter& f) { fd::pp_doc(f, main); }),
                                            spellcheck(extract_modules, env, e.lid));
      return location::errorf_sub(
          loc,
          {location::msg_noloc("@{<hint>Hint@}: There is a module type named %a,@ but module types are not modules",
                               quoted_longident(e.lid))},
          "%a", [&](Formatter& f) { fd::pp_doc(f, main); });
    }
    case K::Unbound_constructor: {
      std::optional<Doc> hint = spellcheck(extract_constructors, env, e.lid);
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>Unbound constructor @}%a", quoted_constr(e.lid)), hint);
    }
    case K::Unbound_label: {
      std::optional<Doc> hint = spellcheck(extract_labels, env, e.lid);
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>Unbound record field @}%a", quoted_longident(e.lid)), hint);
    }
    case K::Unbound_class: {
      Doc main = doc_printf("@{<ralign>Unbound class @}%a", quoted_longident(e.lid));
      bool found = true;
      try {
        env::find_cltype_by_name(e.lid, env);
      } catch (const env::NotFound&) {
        found = false;
      }
      if (!found)
        return location::aligned_error_hint(loc, {}, doc_printf("%a", [&](Formatter& f) { fd::pp_doc(f, main); }),
                                            spellcheck(extract_classes, env, e.lid));
      return location::errorf_sub(
          loc,
          {location::msg_noloc("@{<hint>Hint@}: There is a class type named %a,@ but classes are not class types.",
                               quoted_longident(e.lid))},
          "%a", [&](Formatter& f) { fd::pp_doc(f, main); });
    }
    case K::Unbound_modtype: {
      Doc main = doc_printf("@{<ralign>Unbound module type @}%a", quoted_longident(e.lid));
      bool found = true;
      try {
        env::find_module_by_name(e.lid, env);
      } catch (const env::NotFound&) {
        found = false;
      }
      if (!found)
        return location::aligned_error_hint(loc, {}, doc_printf("%a", [&](Formatter& f) { fd::pp_doc(f, main); }),
                                            spellcheck(extract_modtypes, env, e.lid));
      return location::errorf_sub(
          loc,
          {location::msg_noloc("@{<hint>Hint@}: There is a module named %a,@ but modules are not module types",
                               quoted_longident(e.lid))},
          "%a", [&](Formatter& f) { fd::pp_doc(f, main); });
    }
    case K::Unbound_cltype: {
      std::optional<Doc> hint = spellcheck(extract_cltypes, env, e.lid);
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>Unbound class type @}%a", quoted_longident(e.lid)), hint);
    }
    case K::Unbound_instance_variable: {
      std::string s(e.name);
      std::optional<Doc> hint = misc::did_you_mean(misc::spellcheck(extract_instance_variables(env), s));
      return location::aligned_error_hint(loc, {}, doc_printf("@{<ralign>Unbound instance variable @}%a", code_str(s)),
                                          hint);
    }
    case K::Not_an_instance_variable: {
      std::string s(e.name);
      std::optional<Doc> hint = misc::did_you_mean(misc::spellcheck(extract_instance_variables(env), s));
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>The value @}%a is not an instance variable", code_str(s)), hint);
    }
    case K::Masked_instance_variable:
      return location::errorf(
          loc, "The instance variable %a@ cannot@ be@ accessed@ from@ the@ definition@ of@ another instance variable",
          quoted_longident(e.lid));
    case K::Masked_self_variable:
      return location::errorf(
          loc, "The self variable %a@ cannot@ be@ accessed@ from@ the@ definition of an instance variable",
          quoted_longident(e.lid));
    case K::Masked_ancestor_variable:
      return location::errorf(
          loc, "The ancestor variable %a@ cannot@ be@ accessed@ from@ the definition of an instance variable",
          quoted_longident(e.lid));
    case K::Illegal_reference_to_recursive_module: {
      std::string container = e.container.some ? std::string(e.container.v) : "_";
      std::string unbound(e.unbound);
      bool same = container == unbound;
      auto self_or_definition = [&](Formatter& f) {
        if (same)
          fprintf(f, "its own definition");
        else
          fprintf(f, "the definition of the module %a", code_str(container));
      };
      auto self_or_unbound = [&](Formatter& f) {
        if (same)
          fprintf(f, "itself");
        else
          fprintf(f, "the module type of %a", code_str(unbound));
      };
      return location::errorf(
          loc,
          "@[<hov>This module type is recursive.@ This use of the recursive module %a@ within %t@ makes the module "
          "type of %a depend on@ %t.@ Such recursive definitions of module types are not allowed.@]",
          code_str(unbound), self_or_definition, code_str(container), self_or_unbound);
    }
    case K::Illegal_reference_to_recursive_class_type: {
      std::string container = e.container.some ? std::string(e.container.v) : "_";
      std::string unbound(e.unbound);
      auto self_or_unbound = [&](Formatter& f) {
        if (container == unbound)
          fprintf(f, "itself");
        else
          fprintf(f, "the module type of %a", code_str(unbound));
      };
      return location::errorf(
          loc,
          "@[<hov>This class type is recursive.@ This use of the class type %a@ from the recursive module %a@ "
          "within the definition of@ the class type %a@ in the recursive module %a@ makes the module type of %a@ "
          "depend on %t.@ Such recursive definitions of@ class types within recursive modules@ are not allowed.@]",
          quoted_longident(e.unbound_class_type), code_str(unbound), code_str(std::string(e.container_class_type)),
          code_str(container), code_str(container), self_or_unbound);
    }
    case K::Structure_used_as_functor:
      return location::errorf(loc, "The module %a is a structure, it cannot be applied", quoted_longident(e.lid));
    case K::Abstract_used_as_functor:
      return location::errorf(loc, "The module %a is abstract, it cannot be applied", quoted_longident(e.lid));
    case K::Functor_used_as_structure:
      return location::errorf(loc, "The module %a is a functor, it cannot have any components",
                              quoted_longident(e.lid));
    case K::Abstract_used_as_structure:
      return location::errorf(loc, "The module %a is abstract, it cannot have any components",
                              quoted_longident(e.lid));
    case K::Generative_used_as_applicative:
      return location::errorf(loc, "The functor %a is generative,@ it@ cannot@ be@ applied@ in@ type@ expressions",
                              quoted_longident(e.lid));
    case K::Cannot_scrape_alias: {
      const char* cause = current_unit_is_path(e.alias) ? "is the current compilation unit" : "is missing";
      return location::errorf(loc, "The module %a is an alias for module %a, which %s", quoted_longident(e.lid),
                              misc::style::code(pp_path, e.alias), cause);
    }
  }
  return location::errorf(loc, "?");
}

Report report_env_error(const env::Error& e) {
  switch (e.kind) {
    case env::Error::Kind::Missing_module: {
      Path::t path1 = e.path1, path2 = e.path2;
      auto pp_paths = [&](Formatter& ppf) {
        if (path::same(path1, path2))
          fprintf(ppf, "Internal path@ %a@ is dangling.", code_str(path::name(path1)));
        else
          fprintf(ppf, "Internal path@ %a@ expands to@ %a@ which is dangling.", code_str(path::name(path1)),
                  code_str(path::name(path2)));
      };
      return location::errorf(e.loc, "%t@ @[The compiled interface for module@ %a@ was not found.@]", pp_paths,
                              code_str(std::string(ident::name(path::head(path2)))));
    }
    case env::Error::Kind::Illegal_value_name:
      return location::errorf(e.loc, "%a is not a valid value identifier.", code_str(e.name));
    case env::Error::Kind::Lookup_error: return report_lookup_error(e.loc, e.env, e.err);
  }
  return location::errorf(e.loc, "?");
}

// ---- Typetexp ----

void pp_tag(Formatter& ppf, const std::string& t) { fprintf(ppf, "`%s", t); }
void pp_out_type(Formatter& ppf, const outcometree::OutType* ty) {
  misc::style::as_inline_code(oprint::out_type, ppf, ty);
}

Report report_typetexp_error(const Location& loc, env::t env, const typetexp::Error& e) {
  using K = typetexp::Error::Kind;
  switch (e.kind) {
    case K::Unbound_type_variable: {
      std::optional<Doc> hint = misc::did_you_mean(misc::spellcheck(e.names, e.name));
      return location::aligned_error_hint(
          loc, {}, doc_printf("@{<ralign>The type variable @}%a is unbound in this type declaration.", code_str(e.name)),
          hint);
    }
    case K::No_type_wildcards:
      return location::errorf(loc, "A type wildcard %a is not allowed in this type declaration.", code_str("_"));
    case K::Undefined_type_constructor:
      return location::errorf(loc, "The type constructor@ %a@ is not yet completely defined",
                              misc::style::code(printtyp::path, e.path));
    case K::Type_arity_mismatch:
      return location::errorf(loc, "The type constructor %a@ expects %i argument(s),@ but is here applied to %i argument(s)",
                              misc::style::code(pprintast::longident, e.lid), e.expected, e.provided);
    case K::Bound_type_variable:
      return location::errorf(loc, "Already bound type parameter %a", misc::style::code(pprintast::tyvar, e.name));
    case K::Recursive_type: return location::errorf(loc, "This type is recursive");
    case K::Type_mismatch:
      return location::errorf(loc, "%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, env::empty(), e.trace, doc_printf("This type"),
                                       doc_printf("should be an instance of type"));
      });
    case K::Alias_type_mismatch:
      return location::errorf(loc, "%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, env::empty(), e.trace, doc_printf("This alias is bound to type"),
                                       doc_printf("but is used as an instance of type"));
      });
    case K::Present_has_conjunction:
      return location::errorf(loc, "The present constructor %a has a conjunctive type", code_str(e.name));
    case K::Present_has_no_type: {
      std::vector<Msg> sub{location::msg_noloc(
          "@{<hint>Hint@}: Either add %a in the upper bound,@ or@ remove@ it@ from the lower bound.",
          misc::style::code(pp_tag, e.name))};
      return location::errorf_sub(
          loc, std::move(sub),
          "The constructor %a is missing from the upper bound@ (between %a@ and %a)@ of this polymorphic variant@ "
          "but is present in@ its lower bound (after %a).",
          misc::style::code(pp_tag, e.name), code_str("<"), code_str(">"), code_str(">"));
    }
    case K::Constructor_mismatch: {
      Report r;
      printtyp::wrap_printing_env(true, env, [&] {
        out_type::prepare_for_printing({e.ty1, e.ty2});
        // (the two trees: arguments right to left)
        const outcometree::OutType* t2 = out_type::tree_of_typexp(Mode::Type, e.ty2);
        const outcometree::OutType* t1 = out_type::tree_of_typexp(Mode::Type, e.ty1);
        r = location::errorf(loc, "This variant type contains a constructor %a@ which should be@ %a",
                             fd::pr(pp_out_type, t1), fd::pr(pp_out_type, t2));
      });
      return r;
    }
    case K::Not_a_variant: {
      std::optional<Doc> hint;
      if (auto* v = as<Tvar>(types::get_desc(e.ty1)); v && v->name.some)
        hint = misc::did_you_mean({"`" + std::string(v->name.v)});
      return location::aligned_error_hint(
          loc, {},
          doc_printf("@{<ralign>The type @}%a@ does not expand to a polymorphic variant type",
                     fd::pr(printtyp::type_expr, e.ty1)),
          hint);
    }
    case K::Variant_tags:
      return location::errorf(loc, "Variant tags %a@ and %a have the same hash value.@ Change one of them.",
                              misc::style::code(pp_tag, e.name), misc::style::code(pp_tag, e.name2));
    case K::Invalid_variable_name:
      return location::errorf(loc, "The type variable name %a is not allowed in programs", code_str(e.name));
    case K::Cannot_quantify: {
      TypeExpr* v = e.ty1;
      auto explanation = [v](Formatter& ppf) {
        DescKind k = types::get_desc(v)->kind;
        if (k == DescKind::Tvar)
          fprintf(ppf, "it escapes its scope.");
        else if (k == DescKind::Tunivar)
          fprintf(ppf, "it is already bound to another variable.");
        else
          fprintf(ppf, "it is bound to@ %a.", fd::pr(printtyp::type_expr, v));
      };
      return location::errorf(loc, "The universal type variable %a cannot be generalized:@ %a",
                              misc::style::code(pprintast::tyvar, e.name), explanation);
    }
    case K::Multiple_constraints_on_type:
      return location::errorf(loc, "Multiple constraints for type %a", misc::style::code(pprintast::longident, e.lid));
    case K::Method_mismatch: {
      Report r;
      printtyp::wrap_printing_env(true, env, [&] {
        r = location::errorf(loc, "Method %a has type %a,@ which should be %a", code_str(e.name),
                             fd::pr(printtyp::type_expr, e.ty1), fd::pr(printtyp::type_expr, e.ty2));
      });
      return r;
    }
    case K::Opened_object: {
      Path::t nm = e.path;
      return location::errorf(loc, "Illegal open object type%a", [nm](Formatter& ppf) {
        if (nm)
          fprintf(ppf, "@ %a", misc::style::code(printtyp::path, nm));
        else
          fprintf(ppf, "");
      });
    }
    case K::Not_an_object:
      return location::errorf(loc, "@[The type %a@ is not an object type@]", fd::pr(printtyp::type_expr, e.ty1));
    case K::Repeated_tuple_label:
      return location::errorf(loc, "@[This tuple type has two labels named %a@]", code_str(e.name));
    case K::Polymorphic_optional_param:
      return location::errorf(loc, "@[Optional parameter %a cannot be polymorphic@]", code_str(e.name));
    case K::Functor_optional_param:
      return location::errorf(loc, "@[Module-dependent parameter %a cannot be optional@]", code_str(e.name));
  }
  return location::errorf(loc, "?");
}

}  // namespace

void register_env() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const env::Error& e) {
      return report_env_error(e);
    } catch (...) {
    }
    return std::nullopt;
  });
}

void register_typetexp() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const typetexp::Error& e) {
      return report_typetexp_error(e.loc, e.env, e);
    } catch (const typetexp::ErrorForward& e) {
      return error_of_extension(e.ext);
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
