// Includecore's error messages (includecore.ml, the report_* functions):
// see includecore.hpp.
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/includecore.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/printtyp.hpp"

namespace cppcaml::typing::includecore {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using misc::style::code_str;
using out_type::Mode;

std::string_view choose(Position ord, std::string_view first, std::string_view second) {
  return ord == Position::First ? first : second;
}
std::string_view choose_other(Position ord, std::string_view first, std::string_view second) {
  return ord == Position::First ? second : first;
}
std::string capitalize(std::string_view s) {
  std::string r(s);
  if (!r.empty() && r[0] >= 'a' && r[0] <= 'z') r[0] = static_cast<char>(r[0] - 'a' + 'A');
  return r;
}

void report_primitive_mismatch(std::string_view first, std::string_view second, Formatter& ppf,
                               const ValueMismatch& err) {
  switch (err.prim) {
    case PrimitiveMismatch::Name: fprintf(ppf, "The names of the primitives are not the same"); break;
    case PrimitiveMismatch::Arity:
      fprintf(ppf,
              "The syntactic arities of these primitives were not the same.@ (They must have the same number of "
              "arrows present in the source.)");
      break;
    case PrimitiveMismatch::No_alloc:
      fprintf(ppf, "%s primitive is %a but %s is not", capitalize(choose(err.pos, first, second)),
              code_str("[@@noalloc]"), choose_other(err.pos, first, second));
      break;
    case PrimitiveMismatch::Native_name:
      fprintf(ppf, "The native names of the primitives are not the same");
      break;
    case PrimitiveMismatch::Result_repr:
      fprintf(ppf, "The two primitives' results have different representations");
      break;
    case PrimitiveMismatch::Argument_repr:
      fprintf(ppf, "The two primitives' %d%s arguments have different representations", err.index,
              misc::ordinal_suffix(err.index));
      break;
  }
}

void report_type_inequality(env::t env, Formatter& ppf, const et::EqualityError& err) {
  errortrace_report::equality(ppf, Mode::Type_scheme, env, err, doc_printf("The type"),
                              doc_printf("is not equal to the type"));
}

void report_privacy_mismatch(Formatter& ppf, PrivacyMismatch err) {
  bool singular = true;
  const char* item = "";
  switch (err) {
    case PrivacyMismatch::Private_type_abbreviation: item = "type abbreviation"; break;
    case PrivacyMismatch::Private_variant_type:
      singular = false;
      item = "variant constructor(s)";
      break;
    case PrivacyMismatch::Private_record_type: item = "record constructor"; break;
    case PrivacyMismatch::Private_extensible_variant: item = "extensible variant"; break;
    case PrivacyMismatch::Private_row_type: item = "row type"; break;
  }
  fprintf(ppf, "%s %s would be revealed.", singular ? "A private" : "Private", item);
}

void report_label_mismatch(std::string_view first, std::string_view second, env::t env, Formatter& ppf,
                           const LabelMismatch& err) {
  switch (err.kind) {
    case LabelMismatch::Kind::Type: report_type_inequality(env, ppf, err.err); break;
    case LabelMismatch::Kind::Mutability:
      fprintf(ppf, "%s is mutable and %s is not.", capitalize(choose(err.pos, first, second)),
              choose_other(err.pos, first, second));
      break;
    case LabelMismatch::Kind::Atomicity:
      fprintf(ppf, "%s is atomic and %s is not.", capitalize(choose(err.pos, first, second)),
              choose_other(err.pos, first, second));
      break;
  }
}

template <class Change>
using PrefixFn = std::function<void(Formatter&, const Change&)>;

void pp_record_diff(std::string_view first, std::string_view second, const PrefixFn<RecordChange>& prefix,
                    std::string_view decl, env::t env, Formatter& ppf, const RecordChange& x) {
  using K = RecordChange::K;
  auto pre = [&](Formatter& f) { prefix(f, x); };
  switch (x.k) {
    case K::Delete:
      fprintf(ppf, "%aAn extra field, %a, is provided in %s %s.", pre,
              code_str(std::string(ident::name(x.del->ld_id))), first, decl);
      break;
    case K::Insert:
      fprintf(ppf, "%aA field, %a, is missing in %s %s.", pre, code_str(std::string(ident::name(x.insert->ld_id))),
              first, decl);
      break;
    case K::Change:
      if (x.change.k == decltype(x.change)::K::Type) {
        fprintf(ppf, "@[<hv>%aFields do not match:@;<1 2>%a@ is not the same as:@;<1 2>%a@ %a@]", pre,
                misc::style::code(printtyp::label, x.change.got), misc::style::code(printtyp::label, x.change.expected),
                [&](Formatter& f) { report_label_mismatch(first, second, env, f, x.change.reason); });
      } else {
        fprintf(ppf, "%aFields have different names, %a and %a.", pre, code_str(x.change.got_name),
                code_str(x.change.expected_name));
      }
      break;
    case K::Swap:
      fprintf(ppf, "%aFields %a and %a have been swapped.", pre, code_str(x.first), code_str(x.last));
      break;
    case K::Move:
      fprintf(ppf, "@[<2>%aField %a has been moved@ from@ position %d@ to %d.@]", pre, code_str(x.name), x.expected,
              x.got);
      break;
  }
}

void report_constructor_mismatch(std::string_view first, std::string_view second, std::string_view decl,
                                 env::t env, Formatter& ppf, const ConstructorMismatch& err);

template <class Change, class Diff>
void report_patch(Diff pr_diff, std::string_view first, std::string_view second, std::string_view decl, env::t env,
                  Formatter& ppf, const std::vector<Change>& patch) {
  if (patch.size() == 1) {
    PrefixFn<Change> no_prefix = [](Formatter&, const Change&) {};
    fprintf(ppf, "@[<hv>%a@]",
            [&](Formatter& f) { pr_diff(first, second, no_prefix, decl, env, f, patch[0]); });
    return;
  }
  PrefixFn<Change> pre = [](Formatter& f, const Change& c) { diffing::prefix(f, c); };
  fprintf(ppf, "@[<hv>%a@]", [&](Formatter& f) {
    bool firstl = true;
    for (const Change& c : patch) {
      if (!firstl) fprintf(f, "@,");
      firstl = false;
      pr_diff(first, second, pre, decl, env, f, c);
    }
  });
}

void report_record_mismatch(std::string_view first, std::string_view second, std::string_view decl, env::t env,
                            Formatter& ppf, const RecordMismatch& err) {
  if (err.kind == RecordMismatch::Kind::Label_mismatch) {
    report_patch<RecordChange>(pp_record_diff, first, second, decl, env, ppf, err.changes);
    return;
  }
  fprintf(ppf, "@[<hv>Their internal representations differ:@ %s %s %s.@]", choose(err.pos, first, second), decl,
          "uses unboxed float representation");
}

void report_constructor_mismatch(std::string_view first, std::string_view second, std::string_view decl,
                                 env::t env, Formatter& ppf, const ConstructorMismatch& err) {
  switch (err.kind) {
    case ConstructorMismatch::Kind::Type: report_type_inequality(env, ppf, err.err); break;
    case ConstructorMismatch::Kind::Arity: fprintf(ppf, "They have different arities."); break;
    case ConstructorMismatch::Kind::Inline_record:
      report_patch<RecordChange>(pp_record_diff, first, second, decl, env, ppf, err.changes);
      break;
    case ConstructorMismatch::Kind::Kind_:
      fprintf(ppf, "%s uses inline records and %s doesn't.", capitalize(choose(err.pos, first, second)),
              choose_other(err.pos, first, second));
      break;
    case ConstructorMismatch::Kind::Explicit_return_type:
      fprintf(ppf, "%s has explicit return type and %s doesn't.", capitalize(choose(err.pos, first, second)),
              choose_other(err.pos, first, second));
      break;
  }
}

void pp_variant_diff(std::string_view first, std::string_view second, const PrefixFn<VariantChange>& prefix,
                     std::string_view decl, env::t env, Formatter& ppf, const VariantChange& x) {
  using K = VariantChange::K;
  auto pre = [&](Formatter& f) { prefix(f, x); };
  switch (x.k) {
    case K::Delete:
      fprintf(ppf, "%aAn extra constructor, %a, is provided in %s %s.", pre,
              code_str(std::string(ident::name(x.del->cd_id))), first, decl);
      break;
    case K::Insert:
      fprintf(ppf, "%aA constructor, %a, is missing in %s %s.", pre,
              code_str(std::string(ident::name(x.insert->cd_id))), first, decl);
      break;
    case K::Change:
      if (x.change.k == decltype(x.change)::K::Type) {
        fprintf(ppf, "@[<hv>%aConstructors do not match:@;<1 2>%a@ is not the same as:@;<1 2>%a@ %a@]", pre,
                misc::style::code(printtyp::constructor, x.change.got),
                misc::style::code(printtyp::constructor, x.change.expected),
                [&](Formatter& f) { report_constructor_mismatch(first, second, decl, env, f, x.change.reason); });
      } else {
        fprintf(ppf, "%aConstructors have different names, %a and %a.", pre, code_str(x.change.got_name),
                code_str(x.change.expected_name));
      }
      break;
    case K::Swap:
      fprintf(ppf, "%aConstructors %a and %a have been swapped.", pre, code_str(x.first), code_str(x.last));
      break;
    case K::Move:
      fprintf(ppf, "@[<2>%aConstructor %a has been moved@ from@ position %d@ to %d.@]", pre, code_str(x.name),
              x.expected, x.got);
      break;
  }
}

void report_private_variant_mismatch(std::string_view first, std::string_view second, std::string_view decl,
                                     env::t env, Formatter& ppf, const PrivateVariantMismatch& err) {
  auto pp_tag = [](Formatter& f, std::string_view x) { fprintf(f, "`%s", x); };
  switch (err.kind) {
    case PrivateVariantMismatch::Kind::Only_outer_closed:
      fprintf(ppf, "%s is private and closed, but %s is not closed", capitalize(second), first);
      break;
    case PrivateVariantMismatch::Kind::Missing:
      fprintf(ppf, "The constructor %a is only present in %s %s.", code_str(std::string(err.tag)),
              choose(err.pos, first, second), decl);
      break;
    case PrivateVariantMismatch::Kind::Presence:
      fprintf(ppf, "The tag %a is present in %s %s,@ but might not be in %s", misc::style::code(pp_tag, err.tag),
              second, decl, first);
      break;
    case PrivateVariantMismatch::Kind::Incompatible_types_for:
      fprintf(ppf, "Types for tag `%s are incompatible", err.tag);
      break;
    case PrivateVariantMismatch::Kind::Types: report_type_inequality(env, ppf, err.err); break;
  }
}

void report_private_object_mismatch(env::t env, Formatter& ppf, const PrivateObjectMismatch& err) {
  if (err.kind == PrivateObjectMismatch::Kind::Missing)
    fprintf(ppf, "The implementation is missing the method %a", code_str(std::string(err.label)));
  else
    report_type_inequality(env, ppf, err.err);
}

void report_kind_mismatch(std::string_view first, std::string_view second, Formatter& ppf, const TypeKindName& k1,
                          const TypeKindName& k2) {
  auto kind_to_string = [](const TypeKindName& k) -> std::string {
    switch (k.kind) {
      case TypeKindName::Kind::Kind_abstract: return "abstract";
      case TypeKindName::Kind::Kind_record: return "a record";
      case TypeKindName::Kind::Kind_variant: return "a variant";
      case TypeKindName::Kind::Kind_open: return "an extensible variant";
      case TypeKindName::Kind::Kind_external: return "external " + format::string_escaped(k.external).insert(0, "\"") + "\"";
    }
    return "";
  };
  fprintf(ppf, "%s is %s, but %s is %s.", capitalize(first), kind_to_string(k1), second, kind_to_string(k2));
}

}  // namespace

void report_value_mismatch(std::string_view first, std::string_view second, env::t env, Formatter& ppf,
                           const ValueMismatch& err) {
  fprintf(ppf, "@ ");
  switch (err.kind) {
    case ValueMismatch::Kind::Primitive_mismatch: report_primitive_mismatch(first, second, ppf, err); break;
    case ValueMismatch::Kind::Not_a_primitive: fprintf(ppf, "The implementation is not a primitive."); break;
    case ValueMismatch::Kind::Type:
      errortrace_report::moregen(ppf, Mode::Type_scheme, env, err.err, doc_printf("The type"),
                                 doc_printf("is not compatible with the type"));
      break;
  }
}

void report_extension_constructor_mismatch(std::string_view first, std::string_view second, std::string_view decl,
                                           env::t env, Formatter& ppf, const ExtensionConstructorMismatch& err) {
  if (err.kind == ExtensionConstructorMismatch::Kind::Constructor_privacy) {
    fprintf(ppf, "Private extension constructor(s) would be revealed.");
    return;
  }
  Ident::t id = err.id;
  auto constructor = [id](const ExtensionConstructor* e) {
    return misc::style::code([id](Formatter& f, const ExtensionConstructor* x) { printtyp::extension_only_constructor(id, f, x); },
                             e);
  };
  fprintf(ppf, "@[<hv>Constructors do not match:@;<1 2>%a@ is not the same as:@;<1 2>%a@ %a@]", constructor(err.ext1),
          constructor(err.ext2), [&](Formatter& f) { report_constructor_mismatch(first, second, decl, env, f, *err.mismatch); });
}

void report_type_mismatch(std::string_view first, std::string_view second, std::string_view decl, env::t env,
                          Formatter& ppf, const TypeMismatch& err) {
  using K = TypeMismatch::Kind;
  switch (err.kind) {
    case K::Arity: fprintf(ppf, "They have different arities."); break;
    case K::Privacy: report_privacy_mismatch(ppf, err.privacy); break;
    case K::Kind_: report_kind_mismatch(first, second, ppf, err.k1, err.k2); break;
    case K::Constraint:
      fprintf(ppf, "Their parameters differ@,");
      report_type_inequality(env, ppf, err.err);
      break;
    case K::Manifest: report_type_inequality(env, ppf, err.err); break;
    case K::Private_variant: report_private_variant_mismatch(first, second, decl, env, ppf, *err.private_variant); break;
    case K::Private_object: report_private_object_mismatch(env, ppf, *err.private_object); break;
    case K::Variance: fprintf(ppf, "Their variances do not agree."); break;
    case K::Record_mismatch: report_record_mismatch(first, second, decl, env, ppf, *err.record); break;
    case K::Variant_mismatch:
      report_patch<VariantChange>(pp_variant_diff, first, second, decl, env, ppf, err.variant_changes);
      break;
    case K::Unboxed_representation:
      fprintf(ppf, "Their internal representations differ:@ %s %s %s.", choose(err.pos, first, second), decl,
              "uses unboxed representation");
      break;
    case K::Immediate: {
      std::string f = capitalize(first);
      if (err.immediate_violation_always)
        fprintf(ppf, "%s is not an immediate type.", f);
      else
        fprintf(ppf, "%s is not a type that is always immediate on 64 bit platforms.", f);
      break;
    }
  }
}

}  // namespace cppcaml::typing::includecore
