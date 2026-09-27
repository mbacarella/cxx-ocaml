// Typeclass.report_error_doc and its error_of_exn (typeclass.ml): see
// reporters.hpp.
#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/includeclass.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/typeclass.hpp"

namespace cppcaml::typing::reporters {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Report;
using misc::style::code_str;
using out_type::Mode;
using EK = typeclass::Error::K;

const char* non_virtual_string_of_kind(typeclass::Kind k) {
  switch (k) {
    case typeclass::Kind::Object: return "object";
    case typeclass::Kind::Class: return "non-virtual class";
    case typeclass::Kind::Class_type: return "non-virtual class type";
  }
  return "";
}

void out_type_c(Formatter& ppf, const outcometree::OutType* t) { misc::style::as_inline_code(oprint::out_type, ppf, t); }
auto quoted_type(TypeExpr* t) { return misc::style::code(printtyp::type_expr, t); }

void pp_args(Formatter& ppf, const std::vector<TypeExpr*>& args) {
  std::vector<const outcometree::OutType*> ts;
  for (TypeExpr* t : args) ts.push_back(out_type::tree_of_typexp(Mode::Type, t));
  misc::style::as_inline_code(oprint::out_type_args, ppf, ts);
}

void pp_codes(Formatter& ppf, const std::vector<std::string_view>& l) {
  fd::pp_print_list(ppf, [](Formatter& f, std::string_view s) { misc::style::inline_code(f, s); }, l,
                    fd::pp_print_space);
}

void report_error_doc(env::t env, Formatter& ppf, const typeclass::Error& e) {
  switch (e.kind) {
    case EK::Repeated_parameter: fprintf(ppf, "A type parameter occurs several times"); break;
    case EK::Unconsistent_constraint:
      fprintf(ppf, "@[<v>The class constraints are not consistent.@ ");
      errortrace_report::unification(ppf, env, e.trace, doc_printf("Type"), doc_printf("is not compatible with type"));
      fprintf(ppf, "@]");
      break;
    case EK::Field_type_mismatch:
      errortrace_report::unification(ppf, env, e.trace, doc_printf("The %s %a@ has type", e.name, code_str(e.name2)),
                                     doc_printf("but is expected to have type"));
      break;
    case EK::Unexpected_field:
      fprintf(ppf, "@[@[<2>This object is expected to have type :@ %a@]@ This type does not have a method %a.",
              quoted_type(e.ty), code_str(e.name));
      break;
    case EK::Structure_expected:
      fprintf(ppf, "@[This class expression is not a class structure; it has type@ %a@]",
              misc::style::code(printtyp::class_type, e.cty));
      break;
    case EK::Cannot_apply:
      fprintf(ppf, "This class expression is not a class function, it cannot be applied");
      break;
    case EK::Apply_wrong_label: {
      ArgLabel l = e.label;
      fprintf(ppf, "This argument cannot be applied %a", [l](Formatter& f) {
        if (l.kind == ArgLabel::Kind::Nolabel)
          fprintf(f, "without label");
        else
          fprintf(f, "with label %a", code_str(btype::prefixed_label_name(l)));
      });
      break;
    }
    case EK::Pattern_type_clash:
      fprintf(ppf, "@[%s@ %a@]", "This pattern cannot match self: it only matches values of type", quoted_type(e.ty));
      break;
    case EK::Unbound_class_2:
      fprintf(ppf, "@[The class@ %a@ is not yet completely defined@]", misc::style::code(printtyp::longident, e.lid));
      break;
    case EK::Unbound_class_type_2:
      fprintf(ppf, "@[The class type@ %a@ is not yet completely defined@]",
              misc::style::code(printtyp::longident, e.lid));
      break;
    case EK::Abbrev_type_clash: {
      out_type::prepare_for_printing({e.ty, e.ty2, e.ty3});
      // (the trees: arguments right to left)
      const outcometree::OutType* t3 = out_type::tree_of_typexp(Mode::Type, e.ty3);
      const outcometree::OutType* t2 = out_type::tree_of_typexp(Mode::Type, e.ty2);
      const outcometree::OutType* t1 = out_type::tree_of_typexp(Mode::Type, e.ty);
      fprintf(ppf, "@[The abbreviation@ %a@ expands to type@ %a@ but is used with type@ %a@]", fd::pr(out_type_c, t1),
              fd::pr(out_type_c, t2), fd::pr(out_type_c, t3));
      break;
    }
    case EK::Constructor_type_mismatch:
      errortrace_report::unification(ppf, env, e.trace,
                                     doc_printf("The expression %a has type", code_str("new " + e.name)),
                                     doc_printf("but is used with type"));
      break;
    case EK::Virtual_class: {
      const char* kind = non_virtual_string_of_kind(e.class_kind);
      const char* missings = e.names.empty() ? "variables" : e.names2.empty() ? "methods" : "methods and variables";
      std::vector<std::string_view> all = e.names;
      all.insert(all.end(), e.names2.begin(), e.names2.end());
      fprintf(ppf, "@[This %s has virtual %s.@ @[<2>The following %s are virtual : %a@]@]", kind, missings, missings,
              [&](Formatter& f) { pp_codes(f, all); });
      break;
    }
    case EK::Undeclared_methods:
      fprintf(ppf,
              "@[This %s has undeclared virtual methods.@ @[<2>The following methods were not declared : %a@]@]",
              non_virtual_string_of_kind(e.class_kind), [&](Formatter& f) { pp_codes(f, e.names); });
      break;
    case EK::Parameter_arity_mismatch:
      fprintf(ppf,
              "@[The class constructor %a@ expects %i type argument(s),@ but is here applied to %i type "
              "argument(s)@]",
              misc::style::code(printtyp::longident, e.lid), e.n1, e.n2);
      break;
    case EK::Parameter_mismatch:
      errortrace_report::unification(ppf, env, e.trace, doc_printf("The type parameter"),
                                     doc_printf("does not meet its constraint: it should be"));
      break;
    case EK::Bad_parameters: {
      std::vector<TypeExpr*> all = e.tys;
      all.insert(all.end(), e.tys2.begin(), e.tys2.end());
      out_type::prepare_for_printing(all);
      fprintf(ppf,
              "@[The abbreviation %a@ is used with parameter(s)@ %a@ which are incompatible with constraint(s)@ %a@]",
              misc::style::code(printtyp::ident, e.id), [&](Formatter& f) { pp_args(f, e.tys); },
              [&](Formatter& f) { pp_args(f, e.tys2); });
      break;
    }
    case EK::Bad_class_type_parameters: {
      std::vector<TypeExpr*> all = e.tys;
      all.insert(all.end(), e.tys2.begin(), e.tys2.end());
      out_type::prepare_for_printing(all);
      auto pp_hash = [](Formatter& f, Ident::t id) { fprintf(f, "#%a", fd::pr(printtyp::ident, id)); };
      fprintf(ppf,
              "@[The class type %a@ is used with parameter(s)@ %a,@ whereas the class type definition@ "
              "constrains@ those parameters to be@ %a@]",
              misc::style::code(pp_hash, e.id), [&](Formatter& f) { pp_args(f, e.tys); },
              [&](Formatter& f) { pp_args(f, e.tys2); });
      break;
    }
    case EK::Class_match_failure: includeclass::report_error_doc(Mode::Type, ppf, e.failures); break;
    case EK::Unbound_val: fprintf(ppf, "Unbound instance variable %a", code_str(e.name)); break;
    case EK::Unbound_type_var: {
      const ctype::ClosedClassFailure& r = *e.closed_failure;
      auto print_reason = [&](Formatter& f) {
        TypeExpr* ty0 = r.free_variable;
        TypeExpr* ty1 = r.kind == ctype::VariableKind::Type_variable
                            ? ty0
                            : btype::newgenty(types::tobject(ty0, make<NameRef>(nullptr)));
        out_type::add_type_to_preparation(r.meth_ty);
        out_type::add_type_to_preparation(ty1);
        const outcometree::OutType* t0 = out_type::tree_of_typexp(Mode::Type, ty0);
        const outcometree::OutType* tm = out_type::tree_of_typexp(Mode::Type, r.meth_ty);
        fprintf(f, "The method %a@ has type@;<1 2>%a@ where@ %a@ is unbound", code_str(std::string(r.meth)),
                fd::pr(out_type_c, tm), fd::pr(out_type_c, t0));
      };
      fprintf(ppf, "@[<v>@[Some type variables are unbound in this type:@;<1 2>%a@]@ @[%a@]@]",
              [&](Formatter& f) { fd::pp_doc(f, e.decl_doc); }, print_reason);
      break;
    }
    case EK::Non_generalizable_class:
      out_type::prepare_for_printing(e.tys);
      fprintf(ppf,
              "@[The type of this class,@ %a,@ contains the non-generalizable type variable(s): %a.@ %a@]",
              misc::style::code([id = e.id](Formatter& f, const ClassDeclaration* c) {
                printtyp::class_declaration(id, f, c);
              }, e.clty),
              [&](Formatter& f) {
                fd::pp_print_list(
                    f, [](Formatter& ff, TypeExpr* t) { misc::style::as_inline_code(out_type::prepared_type_scheme, ff, t); },
                    e.tys, [](Formatter& ff) { fprintf(ff, ",@ "); });
              },
              [](Formatter& f) { misc::print_see_manual(f, {6, 1, 2}); });
      break;
    case EK::Cannot_coerce_self:
      fprintf(ppf,
              "@[The type of self cannot be coerced to@ the type of the current class:@ %a.@.Some occurrences are "
              "contravariant@]",
              misc::style::code(printtyp::type_scheme, e.ty));
      break;
    case EK::Non_collapsable_conjunction:
      fprintf(ppf,
              "@[The type of this class,@ %a,@ contains non-collapsible conjunctive types in constraints.@ %t@]",
              misc::style::code([id = e.id](Formatter& f, const ClassDeclaration* c) {
                printtyp::class_declaration(id, f, c);
              }, e.clty),
              [&](Formatter& f) {
                errortrace_report::unification(f, env, e.trace, doc_printf("Type"),
                                               doc_printf("is not compatible with type"));
              });
      break;
    case EK::Self_clash:
      errortrace_report::unification(ppf, env, e.trace, doc_printf("This object is expected to have type"),
                                     doc_printf("but actually has type"));
      break;
    case EK::Mutability_mismatch: {
      bool imm = e.mut == MutableFlag::Immutable;
      fprintf(ppf, "@[The instance variable is %s;@ it cannot be redefined as %s@]", imm ? "mutable" : "immutable",
              imm ? "immutable" : "mutable");
      break;
    }
    case EK::No_overriding:
      if (e.name2.empty())
        fprintf(ppf,
                "@[This inheritance does not override any methods@ or instance variables@ but is explicitly marked "
                "as@ overriding with %a.@]",
                code_str("!"));
      else
        fprintf(ppf, "@[The %s %a@ has no previous definition@]", e.name, code_str(e.name2));
      break;
    case EK::Duplicate:
      fprintf(ppf, "@[The %s %a@ has multiple definitions in this object@]", e.name, code_str(e.name2));
      break;
    case EK::Closing_self_type:
      fprintf(ppf,
              "@[Cannot close type of object literal:@ %a@,it has been unified with the self type of a class that "
              "is not yet@ completely defined.@]",
              misc::style::code(printtyp::type_scheme, e.sign->csig_self));
      break;
    case EK::Polymorphic_class_parameter: fprintf(ppf, "Class parameters cannot be polymorphic."); break;
  }
}

}  // namespace

void register_typeclass() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const typeclass::Error& e) {
      return location::error_of_printer(e.loc, [&](Formatter& ppf) {
        printtyp::wrap_printing_env(true, e.env, [&] { report_error_doc(e.env, ppf, e); });
      });
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
