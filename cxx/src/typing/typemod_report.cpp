// Typemod.report_error and the error_of_exn of Typemod and Includemod
// (typemod.ml, includemod_errorprinter.ml's register): see reporters.hpp.
#include "cppcaml/typing/includemod_errorprinter.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/typemod.hpp"

namespace cppcaml::typing::reporters {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using misc::style::code_str;
using EK = typemod::Error::Kind;

auto modtype_c(const ModuleType* m) { return misc::style::code(printtyp::modtype, m); }
auto longident_c(Longident::t l) { return misc::style::code(printtyp::longident, l); }
auto type_expr_c(TypeExpr* t) { return misc::style::code(printtyp::type_expr, t); }
auto path_c(Path::t p) { return misc::style::code(printtyp::path, p); }

std::string capitalize(std::string_view s) {
  std::string r(s);
  if (!r.empty() && r[0] >= 'a' && r[0] <= 'z') r[0] = static_cast<char>(r[0] - 'a' + 'A');
  return r;
}

// Location.errorf ~loc ~footnote:Out_type.Ident_conflicts.err_msg: the
// footnote after the message
Report with_footnote(const Location& loc, fd::Doc txt) {
  return location::mkerror(loc, {}, out_type::ident_conflicts::err_msg(), std::move(txt));
}

Report report_error(const Location& loc, env::t, const typemod::Error& err) {
  switch (err.kind) {
    case EK::Cannot_apply:
      return location::errorf(loc, "@[This module is not a functor; it has type@ %a@]", modtype_c(err.mty));
    case EK::Not_included: {
      fd::Doc txt = doc_printf("@[<v>Signature mismatch:@ %a@]", [&](Formatter& f) {
        includemod_errorprinter::err_msgs(f, *err.explanation);
      });
      return with_footnote(loc, txt);
    }
    case EK::Cannot_eliminate_dependency:
      return location::errorf(loc,
                              "@[This functor has type@ %a@ The parameter cannot be eliminated in the result type.@ "
                              "Please bind the argument to a module identifier.@]",
                              modtype_c(err.mty));
    case EK::Signature_expected: return location::errorf(loc, "This module type is not a signature");
    case EK::Structure_expected:
      return location::errorf(loc, "@[This module is not a structure; it has type@ %a", modtype_c(err.mty));
    case EK::With_no_component:
      return location::errorf(loc, "@[The signature constrained by %a has no component named %a@]", code_str("with"),
                              longident_c(err.lid));
    case EK::With_mismatch: {
      fd::Doc txt = doc_printf(
          "@[<v>@[In this %a constraint, the new definition of %a@ does not match its original definition@ in the "
          "constrained signature:@]@ %a@]",
          code_str("with"), longident_c(err.lid),
          [&](Formatter& f) { includemod_errorprinter::err_msgs(f, *err.explanation); });
      return with_footnote(loc, txt);
    }
    case EK::With_makes_applicative_functor_ill_typed: {
      fd::Doc txt = doc_printf(
          "@[<v>@[This %a constraint on %a makes the applicative functor @ type %a ill-typed in the constrained "
          "signature:@]@ %a@]",
          code_str("with"), longident_c(err.lid), code_str(path::name(err.path)),
          [&](Formatter& f) { includemod_errorprinter::err_msgs(f, *err.explanation); });
      return with_footnote(loc, txt);
    }
    case EK::With_changes_module_alias:
      return location::errorf(loc,
                              "@[<v>@[This %a constraint on %a changes %a, which is aliased @ in the constrained "
                              "signature (as %a)@].@]",
                              code_str("with"), longident_c(err.lid), code_str(path::name(err.path)),
                              code_str(std::string(ident::name(err.id))));
    case EK::With_creates_invalid_aliases:
      return location::errorf(loc,
                              "In this %a constraint,@ replacing %a@ by %a@ would @ introduce an invalid alias@ at %a",
                              code_str("with"), code_str(path::name(err.path)), code_str(path::name(err.path2)),
                              code_str(std::string(ident::name(err.id))));
    case EK::With_cannot_remove_constrained_type:
      return location::errorf(loc,
                              "@[<v>Destructive substitutions are not supported for constrained @ types (other than "
                              "when replacing a type constructor with @ a type constructor with the same "
                              "arguments).@]");
    case EK::With_cannot_remove_packed_modtype: {
      Path::t p = err.path;
      const ModuleType* mty = err.mty;
      auto pp_constraint = [](Formatter& f, std::pair<Path::t, const ModuleType*> x) {
        fprintf(f, "%s := %a", path::name(x.first), fd::pr(printtyp::modtype, x.second));
      };
      return location::errorf(loc, "This %a constraint@ %a@ makes a packed module ill-formed.@ %a", code_str("with"),
                              misc::style::code(pp_constraint, std::make_pair(p, mty)),
                              [](Formatter& f) { misc::print_see_manual(f, {12, 7, 3}); });
    }
    case EK::With_package_manifest:
      return location::errorf(loc,
                              "In the constrained signature, type %a is defined to be %a.@ Package %a constraints "
                              "may only be used on abstract types.",
                              longident_c(err.lid), type_expr_c(err.ty), code_str("with"));
    case EK::Repeated_name:
      return location::errorf(loc,
                              "@[Multiple definition of the %s name %a.@ Names must be unique in a given structure "
                              "or signature.@]",
                              shape::to_string(err.component), code_str(err.name));
    case EK::Non_generalizable: {
      out_type::prepare_for_printing(err.vars);
      out_type::add_type_to_preparation(err.ty);
      std::vector<TypeExpr*> vars = err.vars;
      return location::errorf(
          loc, "@[The type of this expression,@ %a,@ contains the non-generalizable type variable(s): %a.@ %a@]",
          misc::style::code(out_type::prepared_type_scheme, err.ty),
          [vars](Formatter& f) {
            fd::pp_print_list(
                f, [](Formatter& ff, TypeExpr* t) { misc::style::as_inline_code(out_type::prepared_type_scheme, ff, t); },
                vars, [](Formatter& ff) { fprintf(ff, ",@ "); });
          },
          [](Formatter& f) { misc::print_see_manual(f, {6, 1, 2}); });
    }
    case EK::Non_generalizable_module: {
      out_type::prepare_for_printing(err.vars);
      out_type::add_type_to_preparation(err.item->val_type);
      std::vector<TypeExpr*> vars = err.vars;
      // (~sub first: the arguments of errorf, right to left)
      std::vector<Msg> sub{location::msg(
          err.item->val_loc,
          "The type of this value,@ %a,@ contains the non-generalizable type variable(s) %a.",
          misc::style::code(out_type::prepared_type_scheme, err.item->val_type), [vars](Formatter& f) {
            fd::pp_print_list(
                f, [](Formatter& ff, TypeExpr* t) { misc::style::as_inline_code(out_type::prepared_type_scheme, ff, t); },
                vars, [](Formatter& ff) { fprintf(ff, ",@ "); });
          })};
      return location::errorf_sub(
          loc, std::move(sub),
          "@[The type of this module,@ %a,@ contains non-generalizable type variable(s).@ %a@]",
          fd::pr(printtyp::modtype, err.mty), [](Formatter& f) { misc::print_see_manual(f, {6, 1, 2}); });
    }
    case EK::Implementation_is_required:
      return location::errorf(loc,
                              "@[The interface %a@ declares values, not just types.@ An implementation must be "
                              "provided.@]",
                              [&](Formatter& f) { location::doc::quoted_filename(f, err.name); });
    case EK::Interface_not_compiled:
      return location::errorf(loc, "@[Could not find the .cmi file for interface@ %a.@]",
                              [&](Formatter& f) { location::doc::quoted_filename(f, err.name); });
    case EK::Not_allowed_in_functor_body:
      return location::errorf(loc, "@[This expression creates fresh types.@ %s@]",
                              "It is not allowed inside applicative functors.");
    case EK::Not_a_packed_module:
      return location::errorf(loc, "This expression is not a packed module. It has type@ %a", type_expr_c(err.ty));
    case EK::Incomplete_packed_module:
      return location::errorf(loc, "The type of this packed module contains variables:@ %a", type_expr_c(err.ty));
    case EK::Scoping_pack:
      return location::errorf(loc,
                              "The type %a in this module cannot be exported.@ Its type contains local "
                              "dependencies:@ %a",
                              longident_c(err.lid), type_expr_c(err.ty));
    case EK::Recursive_module_require_explicit_type:
      return location::errorf(loc, "Recursive modules require an explicit module type.");
    case EK::Apply_generative:
      return location::errorf(loc, "This is a generative functor. It can only be applied to %a", code_str("()"));
    case EK::Cannot_scrape_alias:
      return location::errorf(loc, "This is an alias for module %a, which is missing", path_c(err.path));
    case EK::Cannot_alias:
      return location::errorf(loc,
                              "Functor arguments and@ recursive modules@ (within the@ recursive definition),@ such "
                              "as %a,@ cannot be aliased",
                              path_c(err.path));
    case EK::Cannot_scrape_package_type:
      return location::errorf(loc, "The type of this packed module refers to %a, which is missing", path_c(err.path));
    case EK::Badly_formed_signature: {
      Report report = typedecl_report_error(loc, *err.typedecl_error);
      std::string context = err.name;
      fd::Doc main = report.main.txt;
      report.main.txt = doc_printf("In %s:@ %a", context, [&](Formatter& f) { fd::pp_doc(f, main); });
      return report;
    }
    case EK::Cannot_hide_id: {
      const typemod::HidingError& h = *err.hiding;
      if (h.kind == typemod::HidingError::Kind::Illegal_shadowing) {
        auto ns = [](shape::SigComponentKind k) {
          using SCK = shape::SigComponentKind;
          using NS = out_type::Namespace;
          switch (k) {
            case SCK::Value: return NS::Value;
            case SCK::Type: return NS::Type;
            case SCK::Constructor: return NS::Constructor;
            case SCK::Label: return NS::Label;
            case SCK::Module: return NS::Module;
            case SCK::Module_type: return NS::Module_type;
            case SCK::Extension_constructor: return NS::Extension_constructor;
            case SCK::Class: return NS::Class;
            case SCK::Class_type: return NS::Class_type;
          }
          return NS::Value;
        };
        std::string shadowed = fd::to_string(fd::doc_printf(
            "%a", [&](Formatter& f) { printtyp::namespaced_ident(ns(h.shadowed_item_kind), f, h.shadowed_item_id); }));
        std::string shadower = fd::to_string(fd::doc_printf(
            "%a", [&](Formatter& f) { printtyp::namespaced_ident(ns(h.shadowed_item_kind), f, h.shadower_id); }));
        std::string kind(shape::to_string(h.shadowed_item_kind));
        Msg shadowed_msg = location::msg(h.shadowed_item_loc, "@[%s %a came from this include.@]", capitalize(kind),
                                         code_str(shadowed));
        Msg user_msg = location::msg(h.user_loc, "@[The %s %a has no valid type@ if %a is shadowed.@]",
                                     shape::to_string(h.user_kind), code_str(std::string(ident::name(h.user_id))),
                                     code_str(shadowed));
        return location::errorf_sub(loc, {shadowed_msg, user_msg}, "Illegal shadowing of included %s %a@ by %a.",
                                    kind, code_str(shadowed), code_str(shadower));
      }
      std::string kind(shape::to_string(h.opened_item_kind));
      std::string opened_id(ident::name(h.opened_item_id));
      Msg user_msg = location::msg(h.user_loc, "@[The %s %a has no valid type@ if %a is hidden.@]",
                                   shape::to_string(h.user_kind), code_str(std::string(ident::name(h.user_id))),
                                   code_str(opened_id));
      return location::errorf_sub(loc, {user_msg}, "The %s %a introduced by this open appears in the signature.",
                                  kind, code_str(opened_id));
    }
    case EK::Invalid_type_subst_rhs:
      return location::errorf(loc, "Only type synonyms are allowed on the right of %a", code_str(":="));
    case EK::Non_packable_local_modtype_subst:
      return location::errorf(loc,
                              "The module type@ %a@ is not a valid type for a packed module:@ it is defined as a "
                              "local substitution (temporary name)@ for an anonymous module type.@ %a",
                              code_str(path::name(err.path)), [](Formatter& f) { misc::print_see_manual(f, {12, 7, 3}); });
  }
  return location::errorf(loc, "?");
}

}  // namespace

void register_typemod() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const typemod::Error& e) {
      Report r;
      printtyp::wrap_printing_env(true, e.env, [&] { r = report_error(e.loc, e.env, e); });
      return r;
    } catch (...) {
    }
    return std::nullopt;
  });
}

void register_includemod() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const includemod::Error& e) {
      return includemod_errorprinter::report_error_doc(e.expl);
    } catch (const includemod::ApplyError& e) {
      Report r;
      printtyp::wrap_printing_env(true, e.env, [&] {
        r = includemod_errorprinter::report_apply_error_doc(e.loc, e.env, e.app_name, e.mty_f, e.args);
      });
      return r;
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
