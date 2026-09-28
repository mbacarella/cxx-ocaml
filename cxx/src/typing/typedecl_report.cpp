// Typedecl.report_error and its error_of_exn (typedecl.ml, Reaching_path,
// explain_unbound*, the variance messages): see reporters.hpp.
#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/errortrace_report.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/oprint.hpp"
#include "cppcaml/typing/pprintast.hpp"
#include "cppcaml/typing/printtyp.hpp"
#include "cppcaml/typing/reporters.hpp"
#include "cppcaml/typing/typedecl.hpp"
#include "cppcaml/typing/typedecl_unboxed.hpp"

namespace cppcaml::typing::reporters {

namespace {

namespace fd = format_doc;
using fd::doc_printf;
using fd::Formatter;
using fd::fprintf;
using location::Msg;
using location::Report;
using misc::style::code_str;
using out_type::Mode;
using EK = typedecl::Error::Kind;
using Step = typedecl::ReachingTypeStep;
using SK = typedecl::ReachingTypeStep::Kind;

// ---- Reaching_path ----
typedecl::ReachingTypePath simplify(const typedecl::ReachingTypePath& path) {
  typedecl::ReachingTypePath r;
  for (const Step& s : path) {
    // Contains (ty1, _ty2) :: Contains (_ty2', ty3) :: rest -> Contains (ty1, ty3)
    if (s.kind == SK::Contains && !r.empty() && r.back().kind == SK::Contains) {
      r.back().t2 = s.t2;
      continue;
    }
    r.push_back(s);
  }
  return r;
}

void add_to_preparation(const typedecl::ReachingTypePath& path) {
  for (const Step& s : path) switch (s.kind) {
      case SK::Contains:
      case SK::Expands_to:
        out_type::add_type_to_preparation(s.t1);
        out_type::add_type_to_preparation(s.t2);
        break;
      case SK::Parameter: out_type::add_type_to_preparation(s.t1); break;
      case SK::Considered_abstract: break;
    }
}

auto prepared(TypeExpr* t) { return misc::style::code(out_type::prepared_type_expr, t); }

void pp_reaching_path(Formatter& ppf, const typedecl::ReachingTypePath& path) {
  auto pp_step = [](Formatter& f, const Step& s) {
    switch (s.kind) {
      case SK::Expands_to: fprintf(f, "%a = %a", prepared(s.t1), prepared(s.t2)); break;
      case SK::Contains: fprintf(f, "%a contains %a", prepared(s.t1), prepared(s.t2)); break;
      case SK::Parameter: {
        long i = s.n + 1;
        fprintf(f, "the %i%s type parameter of %a is constrained to %a", i, misc::ordinal_suffix(i),
                code_str(path::name(s.path)), [&](Formatter& ff) {
                  printtyp::type_expansion(Mode::Type, ff, out_type::prepare_expansion({s.t1, s.t1}));
                });
        break;
      }
      case SK::Considered_abstract:
        fprintf(f, "the type %a is considered abstract", code_str(path::name(s.path)));
        break;
    }
  };
  fd::pp_print_list(ppf, pp_step, path, fd::comma);
}

void pp_colon(Formatter& ppf, const typedecl::ReachingTypePath& path) {
  fprintf(ppf, ":@\n  @[<v>%a@]", [&](Formatter& f) { pp_reaching_path(f, path); });
}

void quoted_out_type(Formatter& ppf, const outcometree::OutType* ty) {
  misc::style::as_inline_code(oprint::out_type, ppf, ty);
}
auto quoted_type(TypeExpr* ty) { return misc::style::code(printtyp::type_expr, ty); }
auto quoted_constr(Longident::t lid) { return misc::style::code(pprintast::constr, lid); }

// ---- explain_unbound ----
template <class T>
void explain_unbound_gen(Formatter& ppf, const std::vector<TypeExpr*>& params, TypeExpr* tv, const std::vector<T>& tl,
                         const std::function<TypeExpr*(const T&)>& typ, std::string_view kwd,
                         const std::function<void(Formatter&, const T&)>& pr) {
  const T* ti = nullptr;
  for (const T& x : tl)
    if (btype::deep_occur(tv, typ(x))) {
      ti = &x;
      break;
    }
  if (!ti) return;
  // Hack to force aliasing when needed
  TypeExpr* ty0 = btype::newgenty(types::tobject(tv, make<NameRef>(nullptr)));
  std::vector<TypeExpr*> tys = params;
  tys.push_back(typ(*ti));
  tys.push_back(ty0);
  out_type::prepare_for_printing(tys);
  fprintf(ppf, ".@ @[<hov2>In %s@ %a@;<1 -2>the variable %a is unbound@]", kwd,
          [&](Formatter& f) { misc::style::as_inline_code(pr, f, *ti); }, prepared(tv));
}

template <class T>
void explain_unbound(Formatter& ppf, const std::vector<TypeExpr*>& params, TypeExpr* tv, const std::vector<T>& tl,
                     const std::function<TypeExpr*(const T&)>& typ, std::string_view kwd,
                     const std::function<std::string(const T&)>& lab) {
  explain_unbound_gen<T>(ppf, params, tv, tl, typ, kwd, [&](Formatter& f, const T& ti) {
    fprintf(f, "%s%a", lab(ti), fd::pr(out_type::prepared_type_expr, typ(ti)));
  });
}

void explain_unbound_single(Formatter& ppf, const std::vector<TypeExpr*>& params, TypeExpr* tv, TypeExpr* ty) {
  auto trivial = [&](TypeExpr* t) {
    explain_unbound<TypeExpr*>(ppf, params, tv, {t}, [](TypeExpr* const& x) { return x; }, "type",
                               [](TypeExpr* const&) { return std::string(); });
  };
  const TypeDesc* d = types::get_desc(ty);
  if (auto* o = as<Tobject>(d)) {
    auto [tl, rv] = ctype::flatten_fields(o->fields);
    if (types::eq_type(rv, tv)) return trivial(ty);
    explain_unbound<ctype::FieldEntry>(
        ppf, params, tv, tl, [](const ctype::FieldEntry& f) { return f.ty; }, "method",
        [](const ctype::FieldEntry& f) { return std::string(f.name) + ": "; });
    return;
  }
  if (auto* v = as<Tvariant>(d)) {
    if (types::eq_type(types::row_more(v->row), tv)) return trivial(ty);
    explain_unbound<RowFieldEntry>(
        ppf, params, tv, [&] { auto f = types::row_fields(v->row); return std::vector<RowFieldEntry>(f.begin(), f.end()); }(),
        [](const RowFieldEntry& e) -> TypeExpr* {
          types::RowFieldView f = types::row_field_repr(e.field);
          if (f.kind == types::RowFieldView::Kind::Rpresent && f.present) return f.present;
          if (f.kind == types::RowFieldView::Kind::Reither) {
            if (f.arg_types.size() == 1) return f.arg_types[0];
            std::vector<LabeledTy> l;
            for (TypeExpr* t : f.arg_types) l.push_back(LabeledTy{OptStr{}, t});
            return btype::newgenty(types::ttuple(slice(l)));
          }
          return btype::newgenty(types::ttuple(Slice<LabeledTy>{}));
        },
        "case", [](const RowFieldEntry& e) { return "`" + std::string(e.label) + " of "; });
    return;
  }
  trivial(ty);
}

void explain_unbounded(const std::vector<TypeExpr*>& params, TypeExpr* ty, const TypeDeclaration* decl,
                       Formatter& ppf) {
  using TK = TypeKind::Kind;
  switch (decl->type_kind->kind) {
    case TK::Type_variant: {
      std::vector<const ConstructorDeclaration*> tl(decl->type_kind->constructors.begin(),
                                                    decl->type_kind->constructors.end());
      explain_unbound_gen<const ConstructorDeclaration*>(
          ppf, params, ty, tl,
          [](const ConstructorDeclaration* const& c) {
            std::vector<LabeledTy> l;
            if (c->cd_args.kind == ConstructorArguments::Kind::Cstr_tuple)
              for (TypeExpr* t : c->cd_args.tuple) l.push_back(LabeledTy{OptStr{}, t});
            else
              for (const LabelDeclaration* ld : c->cd_args.record) l.push_back(LabeledTy{OptStr{}, ld->ld_type});
            return btype::newgenty(types::ttuple(slice(l)));
          },
          "case", [](Formatter& f, const ConstructorDeclaration* const& c) {
            fprintf(f, "%a of %a", fd::pr(printtyp::ident, c->cd_id), fd::pr(printtyp::constructor_arguments, c->cd_args));
          });
      return;
    }
    case TK::Type_record: {
      std::vector<const LabelDeclaration*> tl(decl->type_kind->labels.begin(), decl->type_kind->labels.end());
      explain_unbound<const LabelDeclaration*>(
          ppf, params, ty, tl, [](const LabelDeclaration* const& l) { return l->ld_type; }, "field",
          [](const LabelDeclaration* const& l) { return std::string(ident::name(l->ld_id)) + ": "; });
      return;
    }
    case TK::Type_abstract:
      if (decl->type_manifest) explain_unbound_single(ppf, params, ty, decl->type_manifest);
      return;
    default: return;
  }
}

std::string variance(const typedecl_variance::SurfaceVariance& v) {
  std::string inj = v.inj ? "injective " : "";
  if (v.co && v.cn) return inj + "invariant";
  if (v.co) return inj + "covariant";
  if (v.cn) return inj + "contravariant";
  return inj.empty() ? "unrestricted" : inj;
}

fd::Doc variance_context(const typedecl_variance::VarianceVariableContext& c) {
  using K = typedecl_variance::VarianceVariableContext::Kind;
  switch (c.kind) {
    case K::Type_declaration:
      out_type::add_type_declaration_to_preparation(c.id, c.decl);
      return doc_printf("In the definition@\n  @[%a@]@\n", misc::style::code(
                                                                [id = c.id](Formatter& f, const TypeDeclaration* d) {
                                                                  out_type::prepared_type_declaration(id, f, d);
                                                                },
                                                                c.decl));
    case K::Gadt_constructor:
      out_type::add_constructor_to_preparation(c.cd);
      return doc_printf("In the GADT constructor@\n  @[%a@]@\n", misc::style::code(out_type::prepared_constructor, c.cd));
    case K::Extension_constructor:
      out_type::add_extension_constructor_to_preparation(c.ext);
      return doc_printf("In the extension constructor@\n  @[%a@]@\n", [&](Formatter& f) {
        out_type::prepared_extension_constructor(c.id, f, c.ext);
      });
  }
  return fd::Doc{};
}

void variance_variable_error(const typedecl_variance::SurfaceVariance& v1, const typedecl_variance::SurfaceVariance& v2,
                             TypeExpr* variable, typedecl_variance::VarianceVariableError error, Formatter& ppf) {
  using E = typedecl_variance::VarianceVariableError;
  switch (error) {
    case E::Variance_not_reflected:
      fprintf(ppf,
              "the type variable@ %a@ has a variance that@ is not reflected by its occurrence in type parameters.@ "
              "It was expected to be %s,@ but it is %s.",
              prepared(variable), variance(v2), variance(v1));
      break;
    case E::No_variable:
      fprintf(ppf, "the type variable@ %a@ cannot be deduced@ from the type parameters.", prepared(variable));
      break;
    case E::Variance_not_deducible:
      fprintf(ppf,
              "the type variable@ %a@ has a variance that@ cannot be deduced from the type parameters.@ It was "
              "expected to be %s,@ but it is %s.",
              prepared(variable), variance(v2), variance(v1));
      break;
  }
}

Report variance_error(const Location& loc, const typedecl_variance::SurfaceVariance& v1,
                      const typedecl_variance::SurfaceVariance& v2, const typedecl_variance::VarianceError& e) {
  if (!e.not_satisfied) {
    out_type::prepare_for_printing({e.variable});
    fd::Doc intro = variance_context(e.context);
    return location::errorf(loc, "%a%t", [&](Formatter& f) { fd::pp_doc(f, intro); },
                            [&](Formatter& f) { variance_variable_error(v1, v2, e.variable, e.error, f); });
  }
  return location::errorf(loc,
                          "In this definition, expected parameter@ variances are not satisfied.@ The %d%s type "
                          "parameter was expected to be %s,@ but it is %s.",
                          e.n, misc::ordinal_suffix(e.n), variance(v2), variance(v1));
}

Report report_error(const Location& loc, const typedecl::Error& err) {
  switch (err.kind) {
    case EK::Repeated_parameter: return location::errorf(loc, "A type parameter occurs several times");
    case EK::Duplicate_constructor: return location::errorf(loc, "Two constructors are named %a", code_str(err.name));
    case EK::Too_many_constructors:
      return location::errorf(loc, "Too many non-constant constructors@ -- maximum is %i non-constant constructors@]",
                              246);  // Config.max_tag + 1
    case EK::Duplicate_label:
      return location::errorf(location::none(), "Two labels are named %a", code_str(err.name));
    case EK::Recursive_abbrev:
    case EK::Cycle_in_def: {
      typedecl::ReachingTypePath reaching_path = simplify(err.reaching_path);
      Report r;
      printtyp::wrap_printing_env(true, err.env, [&] {
        out_type::reset();
        add_to_preparation(reaching_path);
        r = location::errorf(loc,
                             err.kind == EK::Recursive_abbrev ? "The type abbreviation %a is cyclic%a"
                                                               : "The definition of %a contains a cycle%a",
                             code_str(err.name), [&](Formatter& f) { pp_colon(f, reaching_path); });
      });
      return r;
    }
    case EK::Definition_mismatch: {
      auto e = [&](Formatter& ppf) {
        if (!err.mismatch) return;
        fprintf(ppf, "@\n@[<v>%a@]", [&](Formatter& f) {
          includecore::report_type_mismatch("the original", "this", "definition", err.env, f, *err.mismatch);
        });
      };
      return location::errorf(loc, "@[This variant or record definition@ does not match that of type@;<1 2>%a@]%t",
                              quoted_type(err.ty), e);
    }
    case EK::Constraint_failed:
      return location::errorf(loc, "Constraints are not satisfied in this type.@\n%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, err.env, err.trace, doc_printf("Type"),
                                       doc_printf("should be an instance of"));
      });
    case EK::Non_regular: {
      typedecl::ReachingTypePath reaching_path = simplify(err.reaching_path);
      TypeExpr* used_as = err.ty;
      TypeExpr* defined_as = err.ty2;
      out_type::prepare_for_printing({used_as, defined_as});
      add_to_preparation(reaching_path);
      // (the trees: arguments right to left)
      const outcometree::OutType* t_used = out_type::tree_of_typexp(Mode::Type, used_as);
      const outcometree::OutType* t_defined = out_type::tree_of_typexp(Mode::Type, defined_as);
      bool is_expansion = false;
      for (const Step& s : reaching_path)
        if (s.kind == SK::Expands_to) is_expansion = true;
      return location::errorf(
          loc,
          "This recursive type is not regular.@ @[<v>The type constructor %a is defined as@;<1 2>type %a@ but it is "
          "used as@;<1 2>%a%t@,All uses need to match the definition for the recursive type to be regular.@]",
          code_str(path::name(err.path)), fd::pr(quoted_out_type, t_defined), fd::pr(quoted_out_type, t_used),
          [&](Formatter& pp) {
            if (is_expansion)
              fprintf(pp, "@ after the following expansion(s)%a", [&](Formatter& f) { pp_colon(f, reaching_path); });
            else
              fprintf(pp, ".");
          });
    }
    case EK::Inconsistent_constraint:
      return location::errorf(loc, "The type constraints are not consistent.@\n%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, err.env, err.trace, doc_printf("Type"),
                                       doc_printf("is not compatible with type"));
      });
    case EK::Type_clash:
      return location::errorf(loc, "%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, err.env, err.trace, doc_printf("This type constructor expands to type"),
                                       doc_printf("but is used here with type"));
      });
    case EK::Null_arity_external: return location::errorf(loc, "External identifiers must be functions");
    case EK::Missing_native_external:
      return location::errorf(loc,
                              "An external function with more than 5 arguments requires a second stub function@\n"
                              "         for native-code compilation");
    case EK::Unbound_type_var:
      return location::errorf(loc, "A type variable is unbound in this type declaration%t",
                              [&](Formatter& f) { explain_unbounded(err.params, err.ty, err.decl, f); });
    case EK::Cannot_extend_private_type:
      return location::errorf(loc, "Cannot extend private type definition@ %a", fd::pr(printtyp::path, err.path));
    case EK::Not_extensible_type:
      return location::errorf(loc, "Type definition@ %a@ is not extensible@]", misc::style::code(printtyp::path, err.path));
    case EK::Extension_mismatch:
      return location::errorf(loc, "@[This extension@ does not match the definition of type@;<1 2>%a@]@\n@[<v>%a@]",
                              code_str(path::name(err.path)), [&](Formatter& f) {
                                includecore::report_type_mismatch("the type", "this extension", "definition", err.env,
                                                                  f, *err.mismatch);
                              });
    case EK::Rebind_wrong_type:
      return location::errorf(loc, "%t", [&](Formatter& ppf) {
        errortrace_report::unification(ppf, err.env, err.trace,
                                       doc_printf("The constructor %a@ has type", quoted_constr(err.lid)),
                                       doc_printf("but was expected to be of type"));
      });
    case EK::Rebind_mismatch:
      return location::errorf(loc,
                              "The constructor@ %a@ extends type@ %a@ whose declaration does not match@ the "
                              "declaration of type@ %a",
                              quoted_constr(err.lid), code_str(path::name(err.path)), code_str(path::name(err.path2)));
    case EK::Rebind_private: return location::errorf(loc, "The constructor@ %a@ is private", quoted_constr(err.lid));
    case EK::Variance: {
      const typedecl_variance::Error& v = *err.variance;
      if (v.kind == typedecl_variance::Error::Kind::Bad_variance) return variance_error(loc, v.s1, v.s2, v.variance);
      TypeExpr* ty = v.anon.ty;
      bool constrained = v.anon.constrained;
      auto reason_text = [ty, constrained](Formatter& f) {
        if (constrained)
          fprintf(f,
                  ", because the type variable %a appears@ in other parameters.@ In GADTS, covariant or "
                  "contravariant type parameters@ must not depend@ on other parameters.",
                  quoted_type(ty));
        else
          fprintf(f,
                  ", because it is instantiated to the type %a.@ Covariant or contravariant type parameters@ may only "
                  "appear@ as type variables@ in GADT constructor definitions.",
                  quoted_type(ty));
      };
      return location::errorf(
          loc, "In this GADT constructor definition,@ the variance of the@ %d%s parameter@ cannot be checked%t", v.n,
          misc::ordinal_suffix(v.n), reason_text);
    }
    case EK::Unavailable_type_constructor:
      return location::errorf(loc, "The definition of type %a@ is unavailable",
                              misc::style::code(printtyp::path, err.path));
    case EK::Multiple_native_repr_attributes:
      return location::errorf(loc, "Too many %a/%a attributes", code_str("[@@unboxed]"), code_str("[@@untagged]"));
    case EK::Cannot_unbox_or_untag_type:
      if (err.repr == typedecl::NativeReprKind::Unboxed)
        return location::errorf(loc, "Don't know how to unbox this type.@ Only %a, %a, %a, and %a can be unboxed.",
                                code_str("float"), code_str("int32"), code_str("int64"), code_str("nativeint"));
      return location::errorf(loc,
                              "Don't know how to untag this type. Only %a@ and other immediate types can be untagged.",
                              code_str("int"));
    case EK::Deep_unbox_or_untag_attribute:
      return location::errorf(
          loc,
          "The attribute %a should be attached to@ a direct argument or result of the primitive,@ it should not "
          "occur deeply into its type.",
          code_str(err.repr == typedecl::NativeReprKind::Unboxed ? "@unboxed" : "@untagged"));
    case EK::Type_cannot_be_external:
      return location::errorf(loc, "The type@ %a@ cannot be used to annotate an external function.", quoted_type(err.ty));
    case EK::Immediacy:
      if (err.sub == static_cast<int>(type_immediacy::Violation::Not_always_immediate))
        return location::errorf(
            loc, "Types@ marked@ with@ the@ immediate@ attribute@ must@ be@ non-pointer@ types@ like@ %a@ or@ %a.",
            code_str("int"), code_str("bool"));
      return location::errorf(loc,
                              "Types@ marked@ with@ the@ %a@ attribute@ must@ be@ produced@ using@ the@ %a@ functor.",
                              code_str("immediate64"), code_str("Stdlib.Sys.Immediate64.Make"));
    case EK::Bad_unboxed_attribute:
      return location::errorf(loc, "This type cannot be unboxed because@ %s.", err.name);
    case EK::Separability: {
      std::string evar = err.name;
      auto pp_evar = [evar](Formatter& f) {
        if (evar.empty())
          fprintf(f, "an unnamed existential variable");
        else
          fprintf(f, "the existential variable %a", misc::style::code(pprintast::tyvar, evar));
      };
      return location::errorf(loc,
                              "This type cannot be unboxed because@ it might contain both float and non-float "
                              "values,@ depending on the instantiation of %a.@ You should annotate it with %a.",
                              pp_evar, code_str("[@@ocaml.boxed]"));
    }
    case EK::Boxed_and_unboxed:
      return location::errorf(loc, "A type cannot be boxed and unboxed at the same time.");
    case EK::Nonrec_gadt:
      return location::errorf(loc, "GADT case syntax cannot be used in a %a block.", code_str("nonrec"));
    case EK::Invalid_private_row_declaration: {
      TypeExpr* ty = err.ty;
      auto pp_private = [](Formatter& f, TypeExpr* t) { fprintf(f, "private %a", fd::pr(printtyp::type_expr, t)); };
      std::vector<Msg> sub{location::msg_noloc(
          "@[<hv>@[@{<hint>Hint@}: If you intended to define a private type abbreviation,@ write explicitly@]@;<1 "
          "2>%a@]",
          misc::style::code(pp_private, ty))};
      return location::errorf_sub(loc, std::move(sub),
                                  "This private row type declaration is invalid.@\n@[<v>The type expression on the "
                                  "right-hand side reduces to@;<1 2>%a@ which does not have a free row type "
                                  "variable.@]",
                                  quoted_type(ty));
    }
    case EK::Atomic_field_must_be_mutable:
      return location::errorf(loc, "@[The label %a must be mutable to be declared atomic.@]", code_str(err.name));
    case EK::External_with_non_syntactic_arity:
      return location::errorf(loc,
                              "This external declaration has a non-syntactic arity,@ its arity is greater than its "
                              "syntactic arity.");
    case EK::Primitive_alias_does_not_refer_to_primitive: {
      const char* what = "";
      switch (err.value_kind.kind) {
        case ValueKind::Kind::Val_reg: what = "a regular value"; break;
        case ValueKind::Kind::Val_ivar: what = "an instance variable"; break;
        case ValueKind::Kind::Val_self: what = "the self object"; break;
        case ValueKind::Kind::Val_anc: what = "an ancestor object"; break;
        case ValueKind::Kind::Val_prim: throw std::logic_error("Typedecl.report_error: value is a primitive");
      }
      return location::errorf(loc, "@[This@ identifier@ should@ be@ a@ primitive,@ but@ it@ is@ bound@ to@ %s.@]",
                              what);
    }
    case EK::Primitive_type_mismatch:
      return location::errorf(loc, "@[<v>The type of this alias does not match that of the aliased primitive.@,%t@]",
                              [&](Formatter& ppf) {
                                errortrace_report::unification(ppf, err.env, err.trace, doc_printf("Type"),
                                                               doc_printf("is not compatible with type"));
                              });
  }
  return location::errorf(loc, "?");
}

}  // namespace

Report typedecl_report_error(const Location& loc, const typedecl::Error& err) { return report_error(loc, err); }

void register_typedecl() {
  location::register_error_of_exn([](std::exception_ptr ep) -> std::optional<Report> {
    try {
      std::rethrow_exception(ep);
    } catch (const typedecl::Error& e) {
      return report_error(e.loc, e);
    } catch (...) {
    }
    return std::nullopt;
  });
}

}  // namespace cppcaml::typing::reporters
