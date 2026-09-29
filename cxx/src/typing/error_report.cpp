// See error_report.hpp.  Constructor names in the declaration order of the
// error types of typing/*.ml.
#include "cppcaml/typing/error_report.hpp"

#include "cppcaml/typing/builtin_attributes.hpp"
#include "cppcaml/typing/includemod.hpp"
#include "cppcaml/typing/primitive.hpp"
#include "cppcaml/typing/typeclass.hpp"
#include "cppcaml/typing/typecore.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typemod.hpp"

namespace cppcaml::typing::error_report {

const char* texp_error_name(typetexp::Error::Kind k) {
  using K = typetexp::Error::Kind;
  switch (k) {
    case K::Unbound_type_variable: return "Unbound_type_variable";
    case K::No_type_wildcards: return "No_type_wildcards";
    case K::Undefined_type_constructor: return "Undefined_type_constructor";
    case K::Type_arity_mismatch: return "Type_arity_mismatch";
    case K::Bound_type_variable: return "Bound_type_variable";
    case K::Recursive_type: return "Recursive_type";
    case K::Type_mismatch: return "Type_mismatch";
    case K::Alias_type_mismatch: return "Alias_type_mismatch";
    case K::Present_has_conjunction: return "Present_has_conjunction";
    case K::Present_has_no_type: return "Present_has_no_type";
    case K::Constructor_mismatch: return "Constructor_mismatch";
    case K::Not_a_variant: return "Not_a_variant";
    case K::Variant_tags: return "Variant_tags";
    case K::Invalid_variable_name: return "Invalid_variable_name";
    case K::Cannot_quantify: return "Cannot_quantify";
    case K::Multiple_constraints_on_type: return "Multiple_constraints_on_type";
    case K::Method_mismatch: return "Method_mismatch";
    case K::Opened_object: return "Opened_object";
    case K::Not_an_object: return "Not_an_object";
    case K::Repeated_tuple_label: return "Repeated_tuple_label";
    case K::Polymorphic_optional_param: return "Polymorphic_optional_param";
    case K::Functor_optional_param: return "Functor_optional_param";
  }
  return "?";
}
const char* lookup_error_name(env::LookupError::Kind k) {
  using K = env::LookupError::Kind;
  switch (k) {
    case K::Unbound_value: return "Unbound_value";
    case K::Unbound_type: return "Unbound_type";
    case K::Unbound_constructor: return "Unbound_constructor";
    case K::Unbound_label: return "Unbound_label";
    case K::Unbound_module: return "Unbound_module";
    case K::Unbound_class: return "Unbound_class";
    case K::Unbound_modtype: return "Unbound_modtype";
    case K::Unbound_cltype: return "Unbound_cltype";
    case K::Unbound_instance_variable: return "Unbound_instance_variable";
    case K::Not_an_instance_variable: return "Not_an_instance_variable";
    case K::Masked_instance_variable: return "Masked_instance_variable";
    case K::Masked_self_variable: return "Masked_self_variable";
    case K::Masked_ancestor_variable: return "Masked_ancestor_variable";
    case K::Structure_used_as_functor: return "Structure_used_as_functor";
    case K::Abstract_used_as_functor: return "Abstract_used_as_functor";
    case K::Functor_used_as_structure: return "Functor_used_as_structure";
    case K::Abstract_used_as_structure: return "Abstract_used_as_structure";
    case K::Generative_used_as_applicative: return "Generative_used_as_applicative";
    case K::Illegal_reference_to_recursive_module: return "Illegal_reference_to_recursive_module";
    case K::Illegal_reference_to_recursive_class_type:
      return "Illegal_reference_to_recursive_class_type";
    case K::Cannot_scrape_alias: return "Cannot_scrape_alias";
  }
  return "?";
}
extern const char* const tc_error_names[] = {
    "Constructor_arity_mismatch",
    "Label_mismatch",
    "Pattern_type_clash",
    "Or_pattern_type_clash",
    "Multiply_bound_variable",
    "Orpat_vars",
    "Expr_type_clash",
    "Function_arity_type_clash",
    "Apply_non_function",
    "Apply_wrong_label",
    "Label_multiply_defined",
    "Label_missing",
    "Label_not_mutable",
    "Wrong_name",
    "Name_type_mismatch",
    "Invalid_format",
    "Not_an_object",
    "Undefined_method",
    "Undefined_self_method",
    "Virtual_class",
    "Private_type",
    "Private_label",
    "Private_constructor",
    "Unbound_instance_variable",
    "Instance_variable_not_mutable",
    "Not_subtype",
    "Outside_class",
    "Value_multiply_overridden",
    "Coercion_failure",
    "Not_a_function",
    "Too_many_arguments",
    "Abstract_wrong_label",
    "Not_a_polymorphic_variant_type",
    "Incoherent_label_order",
    "Less_general",
    "Modules_not_allowed",
    "Cannot_infer_signature",
    "Not_a_packed_module",
    "Unexpected_existential",
    "Invalid_interval",
    "Invalid_for_loop_index",
    "No_value_clauses",
    "Exception_pattern_disallowed",
    "Mixed_value_and_exception_patterns_under_guard",
    "Effect_pattern_below_toplevel",
    "Invalid_continuation_pattern",
    "Inlined_record_escape",
    "Inlined_record_expected",
    "Unrefuted_pattern",
    "Invalid_extension_constructor_payload",
    "Not_an_extension_constructor",
    "Invalid_atomic_loc_payload",
    "Label_not_atomic",
    "Atomic_in_pattern",
    "Literal_overflow",
    "Unknown_literal",
    "Illegal_letrec_pat",
    "Illegal_letrec_expr",
    "Illegal_class_expr",
    "Letop_type_clash",
    "Andop_type_clash",
    "Bindings_type_clash",
    "Unbound_existential",
    "Bind_existential",
    "Missing_type_constraint",
    "Wrong_expected_kind",
    "Expr_not_a_record_type",
    "Constructor_labeled_arg",
    "Partial_tuple_pattern_bad_type",
    "Extra_tuple_label",
    "Missing_tuple_label",
    "Repeated_tuple_exp_label",
    "Repeated_tuple_pat_label",
    "Optional_poly_param",
    "Cannot_unify_tfunctor_to_tarrow",
    "Cannot_omit_tfunctor_argument"};
extern const char* const td_error_names[] = {
    "Repeated_parameter",
    "Duplicate_constructor",
    "Too_many_constructors",
    "Duplicate_label",
    "Recursive_abbrev",
    "Cycle_in_def",
    "Definition_mismatch",
    "Constraint_failed",
    "Inconsistent_constraint",
    "Type_clash",
    "Non_regular",
    "Null_arity_external",
    "Missing_native_external",
    "Unbound_type_var",
    "Cannot_extend_private_type",
    "Not_extensible_type",
    "Extension_mismatch",
    "Rebind_wrong_type",
    "Rebind_mismatch",
    "Rebind_private",
    "Variance",
    "Unavailable_type_constructor",
    "Unbound_type_var_ext",
    "Val_in_structure",
    "Multiple_native_repr_attributes",
    "Cannot_unbox_or_untag_type",
    "Deep_unbox_or_untag_attribute",
    "Type_cannot_be_external",
    "Immediacy",
    "Separability",
    "Bad_unboxed_attribute",
    "Boxed_and_unboxed",
    "Nonrec_gadt",
    "Invalid_private_row_declaration",
    "Atomic_field_must_be_mutable",
    "External_with_non_syntactic_arity"};
extern const char* const tm_error_names[] = {
    "Cannot_apply",
    "Not_included",
    "Cannot_eliminate_dependency",
    "Signature_expected",
    "Structure_expected",
    "With_no_component",
    "With_mismatch",
    "With_makes_applicative_functor_ill_typed",
    "With_changes_module_alias",
    "With_creates_invalid_aliases",
    "With_cannot_remove_constrained_type",
    "With_package_manifest",
    "Repeated_name",
    "Non_generalizable",
    "Non_generalizable_module",
    "Implementation_is_required",
    "Interface_not_compiled",
    "Not_allowed_in_functor_body",
    "Not_a_packed_module",
    "Incomplete_packed_module",
    "Scoping_pack",
    "Recursive_module_require_explicit_type",
    "Apply_generative",
    "Cannot_scrape_alias",
    "Cannot_scrape_package_type",
    "Badly_formed_signature",
    "Cannot_hide_id",
    "Invalid_type_subst_rhs",
    "Non_packable_local_modtype_subst",
    "With_cannot_remove_packed_modtype",
    "Cannot_alias"};
extern const char* const tcl_error_names[] = {
    "Unconsistent_constraint",
    "Field_type_mismatch",
    "Unexpected_field",
    "Structure_expected",
    "Cannot_apply",
    "Apply_wrong_label",
    "Pattern_type_clash",
    "Repeated_parameter",
    "Unbound_class_2",
    "Unbound_class_type_2",
    "Abbrev_type_clash",
    "Constructor_type_mismatch",
    "Virtual_class",
    "Undeclared_methods",
    "Parameter_arity_mismatch",
    "Parameter_mismatch",
    "Bad_parameters",
    "Bad_class_type_parameters",
    "Class_match_failure",
    "Unbound_val",
    "Unbound_type_var",
    "Non_generalizable_class",
    "Cannot_coerce_self",
    "Non_collapsable_conjunction",
    "Self_clash",
    "Mutability_mismatch",
    "No_overriding",
    "Duplicate",
    "Closing_self_type",
    "Polymorphic_class_parameter"};

std::string format_loc(const Location& loc, const std::string& input_name) {
  std::string file = loc.loc_start.pos_fname.empty() ? input_name : std::string(loc.loc_start.pos_fname);
  // "_none_" is printed anyway, to please editors
  bool file_valid = !(file.empty() || file == "//toplevel//");
  long startline = loc.loc_start.pos_lnum;
  long endline = loc.loc_end.pos_lnum;
  long startchar = loc.loc_start.pos_cnum - loc.loc_start.pos_bol;
  long endchar = loc.loc_end.pos_cnum - loc.loc_end.pos_bol;
  std::string out;
  bool first = true;
  auto word = [&](const char* w) {
    std::string x = w;
    if (first) {
      first = false;
      x[0] = static_cast<char>(x[0] - 'a' + 'A');
    }
    return x;
  };
  auto comma = [&] {
    if (!first) out += ", ";
  };
  if (file_valid) out += word("file") + " \"" + file + "\"";
  // "line 1" for a dummy line number
  comma();
  if (startline <= 0) startline = 1;
  if (endline <= 0) endline = startline;
  if (startline == endline)
    out += word("line") + " " + std::to_string(startline);
  else
    out += word("lines") + " " + std::to_string(startline) + "-" + std::to_string(endline);
  if (startchar != -1 && endchar != -1) {
    comma();
    out += word("characters") + " " + std::to_string(startchar) + "-" + std::to_string(endchar);
  }
  return out;
}

std::optional<Report> classify(std::exception_ptr ep) {
  auto at = [](std::string n, const Location& l) { return Report{std::move(n), l}; };
  try {
    std::rethrow_exception(ep);
  } catch (const typecore::Error& er) {
    Report r = at(std::string("Typecore.") + tc_error_names[static_cast<int>(er.kind)], er.loc);
    if (er.kind == typecore::Error::Kind::Apply_non_function) {
      // report_too_many_arg_error: span the application, extra argument included
      DescKind k = types::get_desc(er.ty)->kind;
      if (k == DescKind::Tarrow || k == DescKind::Tfunctor) r.printed_loc = Location{er.loc.loc_start, er.loc3.loc_end, false};
    }
    return r;
  } catch (const typetexp::Error& er) {
    return at(std::string("Typetexp.") + texp_error_name(er.kind), er.loc);
  } catch (const env::Error& er) {
    if (er.kind == env::Error::Kind::Lookup_error)
      return at(std::string("Env.") + lookup_error_name(er.err.kind), er.loc);
    return at(er.kind == env::Error::Kind::Missing_module ? "Env.Missing_module" : "Env.Illegal_value_name",
              er.loc);
  } catch (const typedecl::Error& er) {
    Report r = at(std::string("Typedecl.") + td_error_names[static_cast<int>(er.kind)], er.loc);
    if (er.kind == typedecl::Error::Kind::Duplicate_label) r.printed_loc = location::none();
    if (er.mismatch) r.detail = "type_mismatch kind " + std::to_string(static_cast<int>(er.mismatch->kind));
    return r;
  } catch (const attr_helper::Error& er) {
    return at("Attr_helper", er.loc);
  } catch (const primitive::Error& er) {
    return at("Primitive", er.loc);
  } catch (const typemod::Error& er) {
    return at(std::string("Typemod.") + tm_error_names[static_cast<int>(er.kind)], er.loc);
  } catch (const includemod::ApplyError& er) {
    return at("Includemod.Apply_error", er.loc);
  } catch (const includemod::Error&) {
    return Report{"Includemod.Error", std::nullopt};
  } catch (const typeclass::Error& er) {
    return at(std::string("Typeclass.") + tcl_error_names[static_cast<int>(er.kind)], er.loc);
  } catch (const typemod::ErrorForward& er) {
    // Builtin_attributes.error_of_extension: at the extension's name
    return at("Error_forward", er.ext->name.loc);
  } catch (const typeclass::ErrorForward& er) {
    return at("Error_forward", er.ext->name.loc);
  } catch (const typetexp::ErrorForward& er) {
    return at("Error_forward", er.ext->name.loc);
  } catch (const typecore::VariableInScope&) {
    return Report{"Syntaxerr", std::nullopt};
  } catch (...) {
  }
  return std::nullopt;
}

}  // namespace cppcaml::typing::error_report
