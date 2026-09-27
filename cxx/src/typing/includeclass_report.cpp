// Includeclass.report_error_doc (includeclass.ml): see includeclass.hpp.
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/includeclass.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printtyp.hpp"

namespace cppcaml::typing::includeclass {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using CMK = ctype::ClassMatchFailure::Kind;

void include_err(out_type::Mode mode, Formatter& ppf, const ctype::ClassMatchFailure& e) {
  switch (e.kind) {
    case CMK::CM_Virtual_class: fprintf(ppf, "A class cannot be changed from virtual to concrete"); break;
    case CMK::CM_Parameter_arity_mismatch:
      fprintf(ppf, "The classes do not have the same number of type parameters");
      break;
    case CMK::CM_Type_parameter_mismatch:
      errortrace_report::equality(ppf, mode, e.env, e.equality,
                                  doc_printf("The %d%s type parameter has type", e.index, misc::ordinal_suffix(e.index)),
                                  doc_printf("but is expected to have type"));
      break;
    case CMK::CM_Class_type_mismatch:
      printtyp::wrap_printing_env(true, e.env, [&] {
        fprintf(ppf, "@[The class type@;<1 2>%a@ %s@;<1 2>%a@]", fd::pr(printtyp::class_type, e.cty1),
                "is not matched by the class type", fd::pr(printtyp::class_type, e.cty2));
      });
      break;
    case CMK::CM_Parameter_mismatch:
      errortrace_report::moregen(ppf, mode, e.env, e.moregen,
                                 doc_printf("The %d%s parameter has type", e.index, misc::ordinal_suffix(e.index)),
                                 doc_printf("but is expected to have type"));
      break;
    case CMK::CM_Val_type_mismatch:
      errortrace_report::comparison(ppf, mode, e.env, e.comparison,
                                    doc_printf("The instance variable %s@ has type", e.label),
                                    doc_printf("but is expected to have type"));
      break;
    case CMK::CM_Meth_type_mismatch:
      errortrace_report::comparison(ppf, mode, e.env, e.comparison, doc_printf("The method %s@ has type", e.label),
                                    doc_printf("but is expected to have type"));
      break;
    case CMK::CM_Non_mutable_value:
      fprintf(ppf, "@[The non-mutable instance variable %s cannot become mutable@]", e.label);
      break;
    case CMK::CM_Non_concrete_value:
      fprintf(ppf, "@[The virtual instance variable %s cannot become concrete@]", e.label);
      break;
    case CMK::CM_Missing_value: fprintf(ppf, "@[The first class type has no instance variable %s@]", e.label); break;
    case CMK::CM_Missing_method: fprintf(ppf, "@[The first class type has no method %s@]", e.label); break;
    case CMK::CM_Hide_public: fprintf(ppf, "@[The public method %s cannot be hidden@]", e.label); break;
    case CMK::CM_Hide_virtual: fprintf(ppf, "@[The virtual %s %s cannot be hidden@]", e.kind_name, e.label); break;
    case CMK::CM_Public_method: fprintf(ppf, "@[The public method %s cannot become private@]", e.label); break;
    case CMK::CM_Virtual_method: fprintf(ppf, "@[The virtual method %s cannot become concrete@]", e.label); break;
    case CMK::CM_Private_method: fprintf(ppf, "@[The private method %s cannot become public@]", e.label); break;
  }
}

}  // namespace

void report_error_doc(out_type::Mode mode, Formatter& ppf, const std::vector<ctype::ClassMatchFailure>& errs) {
  if (errs.empty()) return;
  fprintf(ppf, "@[<v>%a%a@]", [&](Formatter& f) { include_err(mode, f, errs[0]); },
          [&](Formatter& f) {
            for (std::size_t i = 1; i < errs.size(); ++i)
              fprintf(f, "@ %a", [&](Formatter& ff) { include_err(mode, ff, errs[i]); });
          });
}

}  // namespace cppcaml::typing::includeclass
