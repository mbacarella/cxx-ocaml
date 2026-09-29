// See warnings.hpp: utils/warnings.ml's warnings (Warnings.t), what they
// say (message) and their reporting (report, report_alert, check_fatal).
#include <algorithm>

#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::warnings {

namespace fd = format_doc;
using K = Warning::K;

int number(const Warning& w) {
  switch (w.k) {
    case K::Comment_start: return 1;
    case K::Comment_not_end: return 2;
    case K::Fragile_match: return 4;
    case K::Ignored_partial_application: return 5;
    case K::Labels_omitted: return 6;
    case K::Method_override: return 7;
    case K::Partial_match: return 8;
    case K::Missing_record_field_pattern: return 9;
    case K::Non_unit_statement: return 10;
    case K::Redundant_case: return 11;
    case K::Redundant_subpat: return 12;
    case K::Instance_variable_override: return 13;
    case K::Illegal_backslash: return 14;
    case K::Implicit_public_methods: return 15;
    case K::Unerasable_optional_argument: return 16;
    case K::Undeclared_virtual_method: return 17;
    case K::Not_principal: return 18;
    case K::Non_principal_labels: return 19;
    case K::Ignored_extra_argument: return 20;
    case K::Nonreturning_statement: return 21;
    case K::Preprocessor: return 22;
    case K::Useless_record_with: return 23;
    case K::Bad_module_name: return 24;
    case K::All_clauses_guarded: return 8;  // used to be 25
    case K::Unused_var: return 26;
    case K::Unused_var_strict: return 27;
    case K::Wildcard_arg_to_constant_constr: return 28;
    case K::Eol_in_string: return 29;
    case K::Duplicate_definitions: return 30;
    case K::Unused_value_declaration: return 32;
    case K::Unused_open: return 33;
    case K::Unused_type_declaration: return 34;
    case K::Unused_for_index: return 35;
    case K::Unused_ancestor: return 36;
    case K::Unused_constructor: return 37;
    case K::Unused_extension: return 38;
    case K::Unused_rec_flag: return 39;
    case K::Name_out_of_scope: return 40;
    case K::Ambiguous_name: return 41;
    case K::Disambiguated_name: return 42;
    case K::Nonoptional_label: return 43;
    case K::Open_shadow_identifier: return 44;
    case K::Open_shadow_label_constructor: return 45;
    case K::Bad_env_variable: return 46;
    case K::Attribute_payload: return 47;
    case K::Eliminated_optional_arguments: return 48;
    case K::No_cmi_file: return 49;
    case K::Unexpected_docstring: return 50;
    case K::Wrong_tailcall_expectation: return 51;
    case K::Fragile_literal_pattern: return 52;
    case K::Misplaced_attribute: return 53;
    case K::Duplicated_attribute: return 54;
    case K::Inlining_impossible: return 55;
    case K::Unreachable_case: return 56;
    case K::Ambiguous_var_in_pattern_guard: return 57;
    case K::No_cmx_file: return 58;
    case K::Flambda_assignment_to_non_mutable_value: return 59;
    case K::Unused_module: return 60;
    case K::Unboxable_type_in_prim_decl: return 61;
    case K::Constraint_on_gadt: return 62;
    case K::Erroneous_printed_signature: return 63;
    case K::Unsafe_array_syntax_without_parsing: return 64;
    case K::Redefining_unit: return 65;
    case K::Unused_open_bang: return 66;
    case K::Unused_functor_parameter: return 67;
    case K::Match_on_mutable_state_prevent_uncurry: return 68;
    case K::Unused_field: return 69;
    case K::Missing_mli: return 70;
    case K::Unused_tmc_attribute: return 71;
    case K::Tmc_breaks_tailcall: return 72;
    case K::Generative_application_expects_unit: return 73;
    case K::Degraded_to_partial_match: return 74;
    case K::Unnecessarily_partial_tuple_pattern: return 75;
  }
  return 0;
}

bool is_active(const Warning& w) { return is_active(number(w)); }
bool is_error(const Warning& w) { return is_error(number(w)); }
bool alert_is_active(const Alert& a) { return alert_is_active(std::string_view(a.kind)); }
bool alert_is_error(const Alert& a) { return alert_is_error(std::string_view(a.kind)); }

namespace {

// the first name of the warning's description, if any
const char* description_name(int n) {
  static const char* const names[last_warning_number + 1] = {
    nullptr,
    "comment-start", "comment-not-end", nullptr, "fragile-match", "ignored-partial-application",
    "labels-omitted", "method-override", "partial-match", "missing-record-field-pattern",
    "non-unit-statement", "redundant-case", "redundant-subpat", "instance-variable-override",
    "illegal-backslash", "implicit-public-methods", "unerasable-optional-argument",
    "undeclared-virtual-method", "not-principal", "non-principal-labels", "ignored-extra-argument",
    "nonreturning-statement", "preprocessor", "useless-record-with", "bad-module-name", nullptr,
    "unused-var", "unused-var-strict", "wildcard-arg-to-constant-constr", "eol-in-string",
    "duplicate-definitions", "module-linked-twice", "unused-value-declaration", "unused-open",
    "unused-type-declaration", "unused-for-index", "unused-ancestor", "unused-constructor",
    "unused-extension", "unused-rec-flag", "name-out-of-scope", "ambiguous-name",
    "disambiguated-name", "nonoptional-label", "open-shadow-identifier",
    "open-shadow-label-constructor", "bad-env-variable", "attribute-payload",
    "eliminated-optional-arguments", "no-cmi-file", "unexpected-docstring",
    "wrong-tailcall-expectation", "fragile-literal-pattern", "misplaced-attribute",
    "duplicated-attribute", "inlining-impossible", "unreachable-case",
    "ambiguous-var-in-pattern-guard", "no-cmx-file", "flambda-assignment-to-non-mutable-value",
    "unused-module", "unboxable-type-in-prim-decl", "constraint-on-gadt",
    "erroneous-printed-signature", "unsafe-array-syntax-without-parsing", "redefining-unit",
    "unused-open-bang", "unused-functor-parameter", "match-on-mutable-state-prevent-uncurry",
    "unused-field", "missing-mli", "unused-tmc-attribute", "tmc-breaks-tailcall",
    "generative-application-expects-unit", "degraded-to-partial-match",
    "unnecessarily-partial-tuple-pattern"};
  return n >= 0 && n <= last_warning_number ? names[n] : nullptr;
}

void inline_code(fd::Formatter& ppf, const std::string& s) { misc::style::inline_code(ppf, s); }
auto code(const std::string& s) {
  return [s](fd::Formatter& ppf) { misc::style::inline_code(ppf, s); };
}
auto hint() {
  return [](fd::Formatter& ppf) { misc::style::hint(ppf); };
}
// comma_inline_list
auto comma_inline_list(const std::vector<std::string>& l) {
  return [l](fd::Formatter& ppf) {
    fd::pp_print_list(ppf, inline_code, l, [](fd::Formatter& f) { fd::comma(f); });
  };
}
// space_inline_list ppf l = fprintf ppf "@[%a@]" (pp_print_list ~pp_sep:pp_print_space inline_code) l
auto space_inline_list(const std::vector<std::string>& l) {
  return [l](fd::Formatter& ppf) {
    fd::fprintf(ppf, "@[%a@]", [&](fd::Formatter& f) {
      fd::pp_print_list(f, inline_code, l, [](fd::Formatter& g) { fd::pp_print_space(g); });
    });
  };
}
// expand ppf s = if s = "" then () else fprintf ppf "@ %s" s
auto expand(const std::string& s) {
  return [s](fd::Formatter& ppf) {
    if (!s.empty()) fd::fprintf(ppf, "@ %s", s);
  };
}
auto see_manual(std::vector<long> ref) {
  return [ref](fd::Formatter& ppf) { misc::print_see_manual(ppf, ref); };
}
auto pp_doc(const fd::Doc& d) {
  return [d](fd::Formatter& ppf) { fd::pp_doc(ppf, d); };
}

std::string normalise_eol(std::string_view s) {
  std::string r;
  for (char c : s)
    if (c != '\r') r += c;
  return r;
}

}  // namespace

fd::Doc message(const Warning& w) {
  using fd::doc_printf;
  switch (w.k) {
    case K::Comment_start:
      return doc_printf(
          "this %a is the start of a comment.@ %t: Did you forget spaces when writing the infix operator %a?",
          code("(*"), hint(), code("( * )"));
    case K::Comment_not_end: return doc_printf("this is not the end of a comment.");
    case K::Fragile_match:
      if (w.s.empty()) return doc_printf("this pattern-matching is fragile.");
      return doc_printf(
          "this pattern-matching is fragile.@ It will remain exhaustive when constructors are added to type %a.",
          code(w.s));
    case K::Ignored_partial_application:
      return doc_printf("this function application is partial,@ maybe@ some@ arguments@ are@ missing.");
    case K::Labels_omitted:
      if (w.l.size() == 1)
        return doc_printf("label %a@ was omitted@ in@ the@ application@ of@ this@ function.", code(w.l[0]));
      return doc_printf("labels %a@ were omitted@ in@ the@ application@ of@ this@ function.",
                        comma_inline_list(w.l));
    case K::Method_override:
      if (w.l.size() == 1) return doc_printf("the method %a is overridden.", code(w.l[0]));
      return doc_printf("the following methods are overridden@ by@ the@ class@ %a:@;<1 2>%a", code(w.l[0]),
                        space_inline_list(std::vector<std::string>(w.l.begin() + 1, w.l.end())));
    case K::Partial_match:
      if (w.doc.empty()) return doc_printf("this pattern-matching is not exhaustive.");
      return doc_printf(
          "this pattern-matching is not exhaustive.@ @[Here is an example of a case that is not matched:@;<1 2>%a@]",
          pp_doc(w.doc));
    case K::Missing_record_field_pattern:
      return doc_printf(
          "the following labels are not bound@ in@ this@ record@ pattern:@;<1 2>%a.@ @[Either bind these labels "
          "explicitly or add %a to the pattern.@]",
          code(w.s), code("; _"));
    case K::Non_unit_statement: return doc_printf("this expression should have type unit.");
    case K::Redundant_case: return doc_printf("this match case is unused.");
    case K::Redundant_subpat: return doc_printf("this sub-pattern is unused.");
    case K::Instance_variable_override:
      if (w.l.size() == 1) return doc_printf("the instance variable %a is overridden.", code(w.l[0]));
      return doc_printf("the following instance variables@ are overridden@ by the class %a:@;<1 2>%a",
                        code(w.l[0]), space_inline_list(std::vector<std::string>(w.l.begin() + 1, w.l.end())));
    case K::Illegal_backslash:
      return doc_printf(
          "illegal backslash escape in string.@ %t: Single backslashes %a are reserved for escape sequences@ (%a, "
          "%a, ...).@ Did you check the list of OCaml escape sequences?@ To get a backslash character, escape it "
          "with a second backslash: %a.",
          hint(), code("\\"), code("\\n"), code("\\r"), code("\\\\"));
    case K::Implicit_public_methods:
      return doc_printf("the following private methods@ were@ made@ public@ implicitly:@;<1 2>%a.",
                        space_inline_list(w.l));
    case K::Unerasable_optional_argument: return doc_printf("this optional argument cannot be erased.");
    case K::Undeclared_virtual_method: return doc_printf("the virtual method %a is not declared.", code(w.s));
    case K::Not_principal: return doc_printf("%a@ is@ not@ principal.", pp_doc(w.doc));
    case K::Non_principal_labels: return doc_printf("%s without principality.", w.s);
    case K::Ignored_extra_argument: return doc_printf("this argument will not be used by the function.");
    case K::Nonreturning_statement: return doc_printf("this statement never returns (or has an unsound type.)");
    case K::Preprocessor: return doc_printf("%s", w.s);
    case K::Useless_record_with:
      return doc_printf("all the fields are explicitly listed in this record:@ the %a clause is useless.",
                        code("with"));
    case K::Bad_module_name:
      return doc_printf("bad source file name: %a is not a valid module name.", code(w.s));
    case K::All_clauses_guarded:
      return doc_printf(
          "this pattern-matching is not exhaustive.@ All clauses in this pattern-matching are guarded.");
    case K::Unused_var:
    case K::Unused_var_strict: return doc_printf("unused variable %a.", code(w.s));
    case K::Wildcard_arg_to_constant_constr:
      return doc_printf("wildcard pattern given as argument to a constant constructor");
    case K::Eol_in_string:
      return doc_printf("unescaped end-of-line in a string constant@ (non-portable behavior before OCaml 5.2)");
    case K::Duplicate_definitions:
      return doc_printf("the %s %a is defined in both types %a and %a.", w.s, code(w.s2), code(w.s3), code(w.s4));
    case K::Unused_value_declaration: return doc_printf("unused value %a.", code(w.s));
    case K::Unused_open: return doc_printf("unused open %a.", code(w.s));
    case K::Unused_open_bang: return doc_printf("unused open! %a.", code(w.s));
    case K::Unused_type_declaration:
      if (w.tdusage == TypeDeclarationUsage::Declaration) return doc_printf("unused type %a.", code(w.s));
      return doc_printf("unused type alias %a.", code(w.s));
    case K::Unused_for_index: return doc_printf("unused for-loop index %a.", code(w.s));
    case K::Unused_ancestor: return doc_printf("unused ancestor variable %a.", code(w.s));
    case K::Unused_constructor:
      switch (w.cusage) {
        case ConstructorUsage::Unused: return doc_printf("unused constructor %a.", code(w.s));
        case ConstructorUsage::Not_constructed:
          return doc_printf(
              "constructor %a is never used to build values.@ (However, this constructor appears in patterns.)",
              code(w.s));
        case ConstructorUsage::Only_exported_private:
          return doc_printf("constructor %a is never used to build values.@ Its type is exported as a private type.",
                            code(w.s));
      }
      break;
    case K::Unused_extension: {
      std::string kind = w.b ? "exception" : "extension constructor";
      switch (w.cusage) {
        case ConstructorUsage::Unused: return doc_printf("unused %s %a", kind, code(w.s));
        case ConstructorUsage::Not_constructed:
          return doc_printf(
              "%s %a is never used@ to@ build@ values.@ (However, this constructor appears in patterns.)", kind,
              code(w.s));
        case ConstructorUsage::Only_exported_private:
          return doc_printf(
              "%s %a is never used@ to@ build@ values.@ It is exported or rebound as a private extension.", kind,
              code(w.s));
      }
      break;
    }
    case K::Unused_rec_flag: return doc_printf("unused rec flag.");
    case K::Name_out_of_scope:
      if (!w.b)
        return doc_printf(
            "%a was selected from type %a.@ @[It is not visible in the current scope,@ and@ will@ not@ be@ "
            "selected@ if the type becomes unknown@].",
            code(w.l.at(0)), code(w.s));
      return doc_printf(
          "this record of type %a@ contains@ fields@ that@ are@ not@ visible in the current scope:@;<1 2>%a.@ "
          "@[They will not be selected@ if the type@ becomes@ unknown.@]",
          code(w.s), space_inline_list(w.l));
    case K::Ambiguous_name:
      if (!w.b)
        return doc_printf(
            "%a belongs to several types:@;<1 2>%a.@ The first one was selected.@ @[Please disambiguate@ if@ "
            "this@ is wrong.%a@]",
            code(w.l.at(0)), space_inline_list(w.l2), expand(w.s));
      return doc_printf(
          "these field labels belong to several types:@;<1 2>%a.@ @[The first one was selected.@ Please "
          "disambiguate@ if@ this@ is@ wrong.%a@]",
          space_inline_list(w.l2), expand(w.s));
    case K::Disambiguated_name:
      return doc_printf(
          "this use of %a@ relies@ on@ type-directed@ disambiguation,@ @[it@ will@ not@ compile@ with@ OCaml@ "
          "4.00@ or@ earlier.@]",
          code(w.s));
    case K::Nonoptional_label: return doc_printf("the label %a is not optional.", code(w.s));
    case K::Open_shadow_identifier:
      return doc_printf("this open statement shadows@ the@ %s identifier@ %a@ (which is later used)", w.s,
                        code(w.s2));
    case K::Open_shadow_label_constructor:
      return doc_printf("this open statement shadows@ the@ %s %a@ (which is later used)", w.s, code(w.s2));
    case K::Bad_env_variable: return doc_printf("illegal environment variable %a : %s", code(w.s), w.s2);
    case K::Attribute_payload: return doc_printf("illegal payload for attribute %a.@ %s", code(w.s), w.s2);
    case K::Eliminated_optional_arguments:
      return doc_printf("implicit elimination@ of optional argument%s@ %a", w.l.size() == 1 ? "" : "s",
                        comma_inline_list(w.l));
    case K::No_cmi_file:
      if (!w.opt) return doc_printf("no cmi file was found@ in path for module %a", code(w.s));
      return doc_printf("no valid cmi file was found@ in path for module %a.@ %s", code(w.s), *w.opt);
    case K::Unexpected_docstring:
      if (w.b) return doc_printf("unattached documentation comment (ignored)");
      return doc_printf("ambiguous documentation comment");
    case K::Wrong_tailcall_expectation: return doc_printf("expected %s", w.b ? "tailcall" : "non-tailcall");
    case K::Fragile_literal_pattern:
      return doc_printf(
          "Code should not depend@ on@ the@ actual@ values of@ this@ constructor's arguments.@ @[They are only "
          "for@ information@ and@ may@ change@ in@ future versions.@ %a@]",
          see_manual({13, 5, 3}));
    case K::Unreachable_case:
      return doc_printf("this match case is unreachable.@ Consider replacing it with a refutation case %a",
                        code("<pat> -> ."));
    case K::Misplaced_attribute: return doc_printf("the %a attribute cannot appear in this context", code(w.s));
    case K::Duplicated_attribute:
      return doc_printf("the %a attribute is used more than once@ on@ this@ expression", code(w.s));
    case K::Inlining_impossible: return doc_printf("Cannot inline:@ %s", w.s);
    case K::Ambiguous_var_in_pattern_guard: {
      std::vector<std::string> vars = w.l;
      std::sort(vars.begin(), vars.end());
      auto vars_explanation = [vars](fd::Formatter& ppf) {
        if (vars.size() == 1)
          fd::fprintf(ppf, "variable %a appears in@ different@ places@ in@ different@ or-pattern@ alternatives.",
                      code(vars[0]));
        else
          fd::fprintf(ppf, "variables %a appear in@ different@ places@ in@ different@ or-pattern@ alternatives.",
                      comma_inline_list(vars));
      };
      return doc_printf(
          "Ambiguous or-pattern variables under@ guard;@ %t@ @[Only the first match will be used to evaluate@ "
          "the@ guard@ expression.@ %a@]",
          vars_explanation, see_manual({13, 5, 4}));
    }
    case K::No_cmx_file:
      return doc_printf(
          "no cmx file was found@ in@ path@ for@ module@ %a,@ and@ its@ interface@ was@ not@ compiled@ with %a",
          code(w.s), code("-opaque"));
    case K::Flambda_assignment_to_non_mutable_value:
      return doc_printf(
          "A potential@ assignment@ to@ a@ non-mutable@ value@ was@ detected@ in@ this@ source@ file.@ Such@ "
          "assignments@ may@ generate@ incorrect@ code@ when@ using@ Flambda.");
    case K::Unused_module: return doc_printf("unused module %a.", code(w.s));
    case K::Unboxable_type_in_prim_decl:
      return doc_printf(
          "This primitive declaration uses type %a,@ whose@ representation@ may be either boxed or unboxed.@ "
          "Without@ an@ annotation@ to@ indicate@ which@ representation@ is@ intended,@ the@ boxed@ "
          "representation@ has@ been@ selected@ by@ default.@ This@ default@ choice@ may@ change@ in@ future@ "
          "versions@ of@ the@ compiler,@ breaking@ the@ primitive@ implementation.@ You@ should@ explicitly@ "
          "annotate@ the@ declaration@ of@ %a@ with@ %a@ or@ %a,@ so@ that@ its@ external@ interface@ remains@ "
          "stable@ in@ the future.",
          code(w.s), code(w.s), code("[@@boxed]"), code("[@@unboxed]"));
    case K::Constraint_on_gadt: return doc_printf("Type constraints do not apply to@ GADT@ cases@ of@ variant types.");
    case K::Erroneous_printed_signature:
      return doc_printf(
          "The printed@ interface@ differs@ from@ the@ inferred@ interface.@ The@ inferred@ interface@ "
          "contained@ items@ which@ could@ not@ be@ printed@ properly@ due@ to@ name@ collisions@ between@ "
          "identifiers.@ %s@ Beware@ that@ this@ warning@ is@ purely@ informational@ and@ will@ not@ catch@ all@ "
          "instances@ of@ erroneous@ printed@ interface.",
          w.s);
    case K::Unsafe_array_syntax_without_parsing:
      return doc_printf("option@ %a@ used with a preprocessor returning@ a@ syntax tree", code("-unsafe"));
    case K::Redefining_unit: {
      std::string name = w.s;
      auto def = [name](fd::Formatter& ppf) {
        misc::style::as_inline_code([](fd::Formatter& f, const std::string& n) { fd::fprintf(f, "type %s = unit", n); },
                                    ppf, name);
      };
      return doc_printf(
          "This type declaration is@ defining@ a new %a constructor@ which@ shadows@ the@ existing@ one.@ %t: Did "
          "you mean %a?",
          code("()"), hint(), def);
    }
    case K::Unused_functor_parameter: return doc_printf("unused functor parameter %a.", code(w.s));
    case K::Match_on_mutable_state_prevent_uncurry:
      return doc_printf(
          "This pattern depends on@ mutable@ state.@ It prevents@ the@ remaining@ arguments@ from@ being@ "
          "uncurried,@ which will@ cause@ additional@ closure@ allocations.");
    case K::Unused_field:
      switch (w.fusage) {
        case FieldUsage::Unused: return doc_printf("unused record field %a.", code(w.s));
        case FieldUsage::Not_read:
          return doc_printf(
              "record field %a is never read.@ (However, this field is used to build or mutate values.)",
              code(w.s));
        case FieldUsage::Not_mutated: return doc_printf("mutable record field %a is never mutated.", code(w.s));
      }
      break;
    case K::Missing_mli: return doc_printf("Cannot find interface file.");
    case K::Unused_tmc_attribute:
      return doc_printf("This function is marked %a@ but is never applied in TMC position.",
                        code("@tail_mod_cons"));
    case K::Tmc_breaks_tailcall:
      return doc_printf(
          "This call@ is@ in@ tail-modulo-cons@ position@ in@ a@ TMC@ function,@ but@ the@ function@ called@ is@ "
          "not@ itself@ specialized@ for@ TMC,@ so@ the@ call@ will@ not@ be@ transformed@ into@ a@ tail@ call.@ "
          "@[Please@ either@ mark@ the@ called@ function@ with@ the %a@ attribute,@ or@ mark@ this@ call@ with@ "
          "the@ %a@ attribute@ to@ make@ its@ non-tailness@ explicit.@]",
          code("[@tail_mod_cons]"), code("[@tailcall false]"));
    case K::Generative_application_expects_unit:
      return doc_printf("A generative functor@ should be applied@ to@ %a;@ using@ %a@ is deprecated.", code("()"),
                        code("(struct end)"));
    case K::Degraded_to_partial_match:
      return doc_printf(
          "This pattern-matching@ is@ compiled@ as@ partial,@ even@ if@ it@ appears@ to@ be@ total.@ It@ may@ "
          "generate@ a@ %a@ exception.@ This@ typically@ occurs@ due@ to@ complex@ matches@ on@ mutable@ "
          "fields.@ %a",
          code("Match_failure"), see_manual({13, 5, 5}));
    case K::Unnecessarily_partial_tuple_pattern:
      return doc_printf(
          "This tuple pattern@ unnecessarily@ ends in %a,@ as@ it@ explicitly@ matches@ all@ components@ of@ its@ "
          "expected@ type.",
          code(".."));
  }
  throw std::logic_error("Warnings.message");
}

int nerrors = 0;

namespace {
std::string id_name(const Warning& w) {
  int n = number(w);
  if (const char* s = description_name(n)) return std::to_string(n) + " [" + s + "]";
  return std::to_string(n);
}
}  // namespace

std::optional<ReportingInformation> report(const Warning& w) {
  if (!is_active(w)) return std::nullopt;
  if (is_error(w)) ++nerrors;
  return ReportingInformation{id_name(w), message(w), is_error(w), {}};
}

std::optional<ReportingInformation> report_alert(const Alert& a) {
  if (!alert_is_active(a)) return std::nullopt;
  bool err = alert_is_error(a);
  if (err) ++nerrors;
  fd::Doc msg = fd::doc_printf("%s", normalise_eol(a.message));
  std::vector<std::pair<Location, fd::Doc>> sub_locs;
  if (!a.def.loc_ghost && !a.use.loc_ghost) {
    sub_locs.emplace_back(a.def, fd::doc_printf("Definition"));
    sub_locs.emplace_back(a.use, fd::doc_printf("Expected signature"));
  }
  return ReportingInformation{a.kind, msg, err, sub_locs};
}

void reset_fatal() { nerrors = 0; }

void check_fatal() {
  if (nerrors > 0) {
    nerrors = 0;
    throw location::AlreadyDisplayed();
  }
}

}  // namespace cppcaml::typing::warnings
