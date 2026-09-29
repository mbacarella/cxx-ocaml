// Port of typing/typecore.ml, part 1: errors, constants, unification
// helpers, pattern variables, as-types, constraint solving during typing of
// patterns, type paths and name disambiguation (up to "Typing of
// patterns").
#include <algorithm>
#include <climits>
#include <cstdint>

#include "typecore_internal.hpp"

namespace cppcaml::typing::typecore {

using namespace types;
using namespace btype;
using pt::as;
using PK = tt::PatternDesc::Kind;
using XK = tt::ExpressionDesc::Kind;

// forward declarations (set by Typemod / Typeclass)
std::function<std::pair<const tt::ModuleExpr*, const void*>(env::t, const pt::ModuleExpr*)>
    type_module;
std::function<std::pair<const tt::StructureItem*, env::t>(env::t, const pt::StructureItem*)>
    type_str_item;
std::function<std::pair<Path::t, env::t>(std::shared_ptr<bool>, OverrideFlag, env::t, const Location&,
                                         const pt::LidLoc&)>
    type_open;
std::function<TypeOpenDeclResult(std::shared_ptr<bool>, env::t, const pt::OpenDeclaration*)> type_open_decl;
std::function<std::pair<const tt::ModuleExpr*, const Package*>(env::t, const pt::ModuleExpr*,
                                                              const Package*)>
    type_package;
std::function<void(const Location&, env::t, TypeExpr*,
                   const std::vector<std::pair<std::vector<std::string_view>, TypeExpr*>>&)>
    check_package_closed;
std::function<std::pair<const tt::ClassStructure*, std::vector<std::string_view>>(
    env::t, const Location&, const pt::ClassStructure*)>
    type_object;

[[noreturn]] void raise_variant_tags(const Location& loc, env::t env, const ctype::Tags& t) {
  typetexp::Error e(loc, env, typetexp::Error::Kind::Variant_tags);
  e.name = std::string(t.l1);
  e.name2 = std::string(t.l2);
  typing_recovery::log_and_raise(e);
}

WrongKindSort wrong_kind_sort_of_constructor(Longident::t lid) {
  std::string_view n;
  if (lid->kind == Longident::Kind::Lident || lid->kind == Longident::Kind::Ldot) n = lid->s;
  else return WrongKindSort::Constructor;
  if (n == "true" || n == "false") return WrongKindSort::Boolean;
  if (n == "[]" || n == "::") return WrongKindSort::List;
  if (n == "()") return WrongKindSort::Unit;
  return WrongKindSort::Constructor;
}

void check_scope_escape(const Location& loc, env::t env, long level, TypeExpr* ty) {
  try {
    ctype::check_scope_escape(env, level, ty);
  } catch (const ctype::Escape& esc) {
    // We don't expand the type here because if we do, we might expand to the
    // type that escaped, leading to confusing error messages.
    auto x = et::Elt<et::ExpandedType>::mk(et::Elt<et::ExpandedType>::Kind::Escape);
    x.escape.kind = static_cast<et::Escape<et::ExpandedType>::Kind>(esc.esc.kind);
    x.escape.path = esc.esc.path;
    x.escape.univ = esc.esc.univ;
    x.escape.module = esc.esc.module;
    x.escape.context = esc.esc.context;
    if (esc.esc.kind == et::Escape<TypeExpr*>::Kind::Equation)
      x.escape.equation = et::trivial_expansion(esc.esc.equation);
    Error e = err(loc, env, EK::Pattern_type_clash);
    e.trace = et::UnificationError{{x}};
    raise_error(e);  // Error.log_or_raise
  }
}

Error error_of_filter_arrow_failure(const Location& loc, env::t env, Explanation explanation,
                                    bool first, TypeExpr* ty_fun,
                                    const ctype::FilterArrowFailure& f) {
  using FK = ctype::FilterArrowFailure::Kind;
  switch (f.kind) {
    case FK::Unification_error: {
      Error e = err(loc, env, EK::Expr_type_clash);
      e.trace = f.err;
      e.explanation = explanation;
      return e;
    }
    case FK::Label_mismatch: {
      Error e = err(loc, env, EK::Abstract_wrong_label);
      e.label = f.got;
      e.label2 = f.expected;
      e.ty = f.expected_type;
      e.explanation = explanation;
      return e;
    }
    case FK::Not_a_function: {
      Error e = err(loc, env, first ? EK::Not_a_function : EK::Too_many_arguments);
      e.ty = ty_fun;
      e.explanation = explanation;
      return e;
    }
  }
  throw std::logic_error("error_of_filter_arrow_failure");
}

// ---- typing of constants ----------------------------------------------------------------
TypeExpr* type_constant(const tt::Constant& c) {
  using CK = tt::Constant::Kind;
  switch (c.kind) {
    case CK::Const_int: return ctype::instance(predef::type_int());
    case CK::Const_char: return ctype::instance(predef::type_char());
    case CK::Const_string: return ctype::instance(predef::type_string());
    case CK::Const_float: return ctype::instance(predef::type_float());
    case CK::Const_int32: return ctype::instance(predef::type_int32());
    case CK::Const_int64: return ctype::instance(predef::type_int64());
    case CK::Const_nativeint: return ctype::instance(predef::type_nativeint());
  }
  throw std::logic_error("type_constant");
}

// runtime/ints.c parse_intnat (int_of_string, Int32/Int64/Nativeint.of_string)
struct IntFailure {};
static int parse_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
static std::int64_t parse_intnat(std::string_view s, int nbits) {
  std::size_t p = 0;
  int sign = 1, base = 10, signedness = 1;
  if (p < s.size() && s[p] == '-') {
    sign = -1;
    p++;
  } else if (p < s.size() && s[p] == '+') {
    p++;
  }
  if (p < s.size() && s[p] == '0' && p + 1 < s.size()) {
    switch (s[p + 1]) {
      case 'x': case 'X': base = 16; signedness = 0; p += 2; break;
      case 'o': case 'O': base = 8; signedness = 0; p += 2; break;
      case 'b': case 'B': base = 2; signedness = 0; p += 2; break;
      case 'u': case 'U': signedness = 0; p += 2; break;
    }
  }
  using U = std::uint64_t;
  U threshold = UINT64_MAX / static_cast<U>(base);
  int d = p < s.size() ? parse_digit(s[p]) : -1;
  if (d < 0 || d >= base) throw IntFailure{};
  U res = static_cast<U>(d);
  for (p++;; p++) {
    if (p >= s.size()) break;
    char c = s[p];
    if (c == '_') continue;
    d = parse_digit(c);
    if (d < 0 || d >= base) break;
    // Detect overflow in multiplication base * res
    if (res > threshold) throw IntFailure{};
    res = static_cast<U>(base) * res + static_cast<U>(d);
    // Detect overflow in addition (base * res) + d
    if (res < static_cast<U>(d)) throw IntFailure{};
  }
  if (p != s.size()) throw IntFailure{};
  if (signedness) {
    // Signed representation expected, allow -2^(nbits-1) to 2^(nbits-1) - 1
    if (sign >= 0) {
      if (res >= (U{1} << (nbits - 1))) throw IntFailure{};
    } else {
      if (res > (U{1} << (nbits - 1))) throw IntFailure{};
    }
  } else {
    // Unsigned representation expected, allow 0 to 2^nbits - 1 and
    // tolerate -(2^nbits - 1) to 0
    if (nbits < 64 && res >= (U{1} << nbits)) throw IntFailure{};
  }
  return sign < 0 ? -static_cast<std::int64_t>(res) : static_cast<std::int64_t>(res);
}
// Misc.Int_literal_converter: allow max_int + 1 (PR#4210)
static std::int64_t cvt_int_aux(std::string_view str, int nbits) {
  if (str.empty() || str[0] == '-') return parse_intnat(str, nbits);
  std::string neg = "-" + std::string(str);
  std::int64_t v = parse_intnat(neg, nbits);
  return -v;  // (~-) wraps at the width, as below
}
static std::int64_t wrap_bits(std::int64_t v, int nbits) {
  if (nbits >= 64) return v;
  std::uint64_t m = (std::uint64_t{1} << nbits) - 1;
  std::uint64_t u = static_cast<std::uint64_t>(v) & m;
  if (u >> (nbits - 1)) u |= ~m;  // sign-extend
  return static_cast<std::int64_t>(u);
}

std::optional<tt::Constant> constant(const pt::Constant& cst, Error* err_out) {
  using CK = tt::Constant::Kind;
  const pt::ConstantDesc& d = cst.pconst_desc;
  tt::Constant c{};
  auto overflow = [&](const char* ty) {
    Error e(location::none(), nullptr, EK::Literal_overflow);
    e.name = ty;
    *err_out = e;
    return std::nullopt;
  };
  switch (d.kind) {
    case pt::ConstantDesc::Kind::Pconst_integer:
      try {
        if (!d.has_suffix) {
          c.kind = CK::Const_int;
          c.i = static_cast<long>(wrap_bits(cvt_int_aux(d.s, 63), 63));
        } else if (d.suffix == 'l') {
          c.kind = CK::Const_int32;
          c.boxed = wrap_bits(cvt_int_aux(d.s, 32), 32);
          c.box = fresh_identity();
        } else if (d.suffix == 'L') {
          c.kind = CK::Const_int64;
          c.boxed = cvt_int_aux(d.s, 64);
          c.box = fresh_identity();
        } else if (d.suffix == 'n') {
          c.kind = CK::Const_nativeint;
          c.boxed = cvt_int_aux(d.s, 64);
          c.box = fresh_identity();
        } else {
          Error e(location::none(), nullptr, EK::Unknown_literal);
          e.name = std::string(d.s);
          e.c = d.suffix;
          *err_out = e;
          return std::nullopt;
        }
      } catch (const IntFailure&) {
        if (!d.has_suffix) return overflow("int");
        if (d.suffix == 'l') return overflow("int32");
        if (d.suffix == 'L') return overflow("int64");
        return overflow("nativeint");
      }
      return c;
    case pt::ConstantDesc::Kind::Pconst_char:
      c.kind = CK::Const_char;
      c.i = static_cast<unsigned char>(d.c);
      return c;
    case pt::ConstantDesc::Kind::Pconst_string:
      c.kind = CK::Const_string;
      c.s = d.s;
      c.str_loc = d.str_loc;
      c.delim = d.delim;
      return c;
    case pt::ConstantDesc::Kind::Pconst_float:
      if (d.has_suffix) {
        Error e(location::none(), nullptr, EK::Unknown_literal);
        e.name = std::string(d.s);
        e.c = d.suffix;
        *err_out = e;
        return std::nullopt;
      }
      c.kind = CK::Const_float;
      c.s = d.s;
      return c;
  }
  throw std::logic_error("constant");
}

tt::Constant constant_or_raise(env::t env, const Location& loc, const pt::Constant& cst) {
  Error e(loc, env, EK::Literal_overflow);
  std::optional<tt::Constant> c = constant(cst, &e);
  if (c) return *c;
  e.loc = loc;
  e.env = env;
  raise_error(e);
}

// Specific version of type_option, using newty rather than newgenty
TypeExpr* type_option(TypeExpr* ty) {
  return ctype::newty(tconstr(predef::paths().option, slice({ty}), make<MemoRef>(mnil())));
}

tt::Expression* mkexp(const tt::ExpressionDesc* d, TypeExpr* ty, const Location& loc, env::t env) {
  return make<tt::Expression>(d, loc, Slice<tt::ExpExtraItem>{}, ty, env, tt::Attributes{});
}

static pt::LidLoc mknoloc_lid(Longident::t l) { return pt::LidLoc{l, location::none()}; }

const tt::Expression* option_none(env::t env, TypeExpr* ty, const Location& loc) {
  Longident::t lid = Longident::lident(ocaml_literal("typing/typecore.ml", "None"));
  const ConstructorDescription* cnone = env::find_ident_constructor(predef::idents().none, env);
  return mkexp(make<tt::Texp_construct>(
                   tt::Texp_construct{{XK::Texp_construct}, mknoloc_lid(lid), cnone, {}}),
               ty, loc, env);
}

const tt::Expression* option_some(env::t env, const tt::Expression* texp) {
  Longident::t lid = Longident::lident(ocaml_literal("typing/typecore.ml", "Some"));
  const ConstructorDescription* csome = env::find_ident_constructor(predef::idents().some, env);
  return mkexp(make<tt::Texp_construct>(
                   tt::Texp_construct{{XK::Texp_construct}, mknoloc_lid(lid), csome, slice({texp})}),
               type_option(texp->exp_type), texp->exp_loc, texp->exp_env);
}

TypeExpr* extract_option_type(env::t env, TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ctype::expand_head(env, ty)));
  if (c && c->args.size() == 1 && path::same(c->path, predef::paths().option)) return c->args[0];
  throw std::logic_error("extract_option_type");
}

bool is_floatarray_type(env::t env, TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ctype::expand_head(env, ty)));
  return c && c->args.empty() && path::same(c->path, predef::paths().floatarray);
}
bool is_iarray_type(env::t env, TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ctype::expand_head(env, ty)));
  return c && c->args.size() == 1 && path::same(c->path, predef::paths().iarray);
}

TypeExpr* protect_expansion(env::t env, TypeExpr* ty) {
  return env::has_local_constraints(env) ? ctype::generic_instance(ty) : ty;
}

static ctype::TypedeclExtraction extract_concrete_typedecl_protected(env::t env, TypeExpr* ty) {
  return ctype::extract_concrete_typedecl(env, protect_expansion(env, ty));
}

RecordExtraction extract_concrete_record(env::t env, TypeExpr* ty) {
  using TK = ctype::TypedeclExtraction::Kind;
  ctype::TypedeclExtraction x = extract_concrete_typedecl_protected(env, ty);
  if (x.kind == TK::Typedecl && x.decl->type_kind->kind == TypeKind::Kind::Type_record)
    return {RecordExtraction::Kind::Record_type, x.p, x.p2, x.decl->type_kind->labels};
  if (x.kind == TK::May_have_typedecl) return {RecordExtraction::Kind::Maybe_a_record_type};
  return {RecordExtraction::Kind::Not_a_record_type};
}

VariantExtraction extract_concrete_variant(env::t env, TypeExpr* ty) {
  using TK = ctype::TypedeclExtraction::Kind;
  ctype::TypedeclExtraction x = extract_concrete_typedecl_protected(env, ty);
  if (x.kind == TK::Typedecl && x.decl->type_kind->kind == TypeKind::Kind::Type_variant)
    return {VariantExtraction::Kind::Variant_type, x.p, x.p2, x.decl->type_kind->constructors};
  if (x.kind == TK::Typedecl && x.decl->type_kind->kind == TypeKind::Kind::Type_open)
    return {VariantExtraction::Kind::Variant_type, x.p, x.p2, {}};
  if (x.kind == TK::May_have_typedecl) return {VariantExtraction::Kind::Maybe_a_variant_type};
  return {VariantExtraction::Kind::Not_a_variant_type};
}

std::vector<Ident::t> extract_label_names(env::t env, TypeExpr* ty) {
  RecordExtraction r = extract_concrete_record(env, ty);
  if (r.kind != RecordExtraction::Kind::Record_type) throw std::logic_error("extract_label_names");
  std::vector<Ident::t> out;
  for (auto* l : r.fields) out.push_back(l->ld_id);
  return out;
}

bool is_principal(TypeExpr* ty) { return !clflags::principal || get_level(ty) == generic_level; }

ArrayInfo disambiguate_array_literal(const Location& loc, env::t env, TypeExpr* expected_ty) {
  auto ret = [&](TypeExpr* ty_elt, MutableFlag mut) {
    if (!is_principal(expected_ty)) prerr_warning(loc, not_principal("this type-based array disambiguation"));
    return ArrayInfo{ty_elt, mut};
  };
  if (is_floatarray_type(env, expected_ty)) return ret(ctype::instance(predef::type_float()), MutableFlag::Mutable);
  if (is_iarray_type(env, expected_ty)) return ret(nullptr, MutableFlag::Immutable);
  return ArrayInfo{nullptr, MutableFlag::Mutable};
}

bool has_poly_constraint(const pt::Pattern* spat) {
  if (auto* c = as<pt::Ppat_constraint>(spat->ppat_desc))
    return c->ty->ptyp_desc->kind == pt::CoreTypeDesc::Kind::Ptyp_poly;
  return false;
}

bool check_poly_constraint(const pt::Pattern* spat, env::t env, const ArgLabel& arg_label) {
  bool has_poly = has_poly_constraint(spat);
  if (has_poly && arg_label.kind == ArgLabel::Kind::Optional) {
    Error e = err(spat->ppat_loc, env, EK::Optional_poly_param);
    e.name = std::string(arg_label.name);
    raise_error(e);
  }
  return has_poly;
}

// ---- typing of patterns: helpers -------------------------------------------------------
// Simplified patterns for effect continuations
std::optional<ContinuationVar> type_continuation_pat(env::t env, TypeExpr* expected_ty,
                                                     const pt::Pattern* sp) {
  const Location& loc = sp->ppat_loc;
  const pt::PatternDesc* d = sp->ppat_desc;
  if (d->kind == pt::PatternDesc::Kind::Ppat_any) return std::nullopt;
  if (auto* v = as<pt::Ppat_var>(d)) {
    Ident::t id = Ident::create_local(v->name.txt);
    auto* desc = make<ValueDescription>(expected_ty, ValueKind{}, loc, Attributes{}, uid::mk(env::get_current_unit()));
    return ContinuationVar{id, desc};
  }
  if (auto* x = as<pt::Ppat_extension>(d)) throw ErrorForward(x->ext);
  raise_error(err(loc, env, EK::Invalid_continuation_pattern));
}

// unification inside type_exp and type_expect ([sexp] is used by error
// messages to report literals in their original formatting)
void unify_exp_types(const Location& loc, env::t env, TypeExpr* ty, TypeExpr* expected_ty,
                     const pt::Expression* sexp) {
  try {
    ctype::unify(env, ty, expected_ty);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Expr_type_clash);
    e.trace = u.err;
    e.sexp = sexp;
    raise_error(e);
  } catch (const ctype::Tags& t) {
    raise_variant_tags(loc, env, t);
  }
}

// Getting proper location of already typed expressions (see typecore.ml)
Location proper_exp_loc(const tt::Expression* exp) {
  for (auto& x : exp->exp_extra)
    if (x.extra.kind == tt::ExpExtra::Kind::Texp_constraint ||
        x.extra.kind == tt::ExpExtra::Kind::Texp_coerce)
      return x.loc;
  return exp->exp_loc;
}

void unify_exp(const pt::Expression* sexp, env::t env, const tt::Expression* exp,
               TypeExpr* expected_ty) {
  Location loc = proper_exp_loc(exp);
  unify_exp_types(loc, env, exp->exp_type, expected_ty, sexp);
}

// Unification inside type_pat.  If [penv] is available, calling this
// function requires [penv.in_counterexample = false]
void unify_pat_types(const Location& loc, env::t env, TypeExpr* ty, TypeExpr* ty2,
                     const pt::PatternDesc* sdesc_for_hint) {
  try {
    ctype::unify(env, ty, ty2);
  } catch (const ctype::Unify& u) {
    Error e = err(loc, env, EK::Pattern_type_clash);
    e.trace = u.err;
    e.sdesc_for_hint = sdesc_for_hint;
    raise_error(e);
  } catch (const ctype::Tags& t) {
    raise_variant_tags(loc, env, t);
  }
}

// GADT unification inside solve_Ppat_construct and check_counter_example_pat
static btype::TypePairs* nothing_equated() {
  static btype::TypePairs* tp = new btype::TypePairs();
  return tp;
}
btype::TypePairs* unify_pat_types_return_equated_pairs(bool refine, const Location& loc,
                                                       ctype::PatternEnv* penv, TypeExpr* pat,
                                                       TypeExpr* expected) {
  try {
    if (refine || penv->in_counterexample) return ctype::unify_gadt(penv, pat, expected);
    ctype::unify(penv->env, pat, expected);
    return nothing_equated();
  } catch (const ctype::Unify& u) {
    Error e = err(loc, penv->env, EK::Pattern_type_clash);
    e.trace = u.err;
    raise_error(e);
  } catch (const ctype::Tags& t) {
    raise_variant_tags(loc, penv->env, t);
  }
}

// Unify pattern types in functions that can be called either from
// [type_pat] or [check_counter_example_pat] (see typecore.ml)
void unify_pat_types_penv(const Location& loc, ctype::PatternEnv* penv, TypeExpr* ty,
                          TypeExpr* ty2) {
  unify_pat_types_return_equated_pairs(false, loc, penv, ty, ty2);
}

void unify_pat(env::t env, const tt::Pattern* pat, TypeExpr* expected_ty,
               const pt::PatternDesc* sdesc_for_hint) {
  unify_pat_types(pat->pat_loc, env, pat->pat_type, expected_ty, sdesc_for_hint);
}

// unification of a type with a Tconstr with freshly created arguments
void unify_head_only(const Location& loc, ctype::PatternEnv* penv,
                     const ConstructorDescription* constr, TypeExpr* expected) {
  Path::t path = data_types::cstr_res_type_path(constr);
  const TypeDeclaration* decl = env::find_type(path, penv->env);
  std::vector<TypeExpr*> params =
      ctype::instance_list(std::vector<TypeExpr*>(decl->type_params.begin(), decl->type_params.end()));
  TypeExpr* ty2 = ctype::newconstr(path, slice(params));
  unify_pat_types_penv(loc, penv, ty2, expected);
}

// Creating new conjunctive types is not allowed when typing patterns: make
// all Reither present in open variants
static void finalize_variant(const tt::Pattern* pat, std::string_view tag, const tt::Pattern* opat,
                             tt::RowDescRef* r) {
  auto* v = as<Tvariant>(get_desc(ctype::expand_head(pat->pat_env, pat->pat_type)));
  if (!v) throw std::logic_error("finalize_variant");
  const RowDesc* row = v->row;
  r->contents = row;
  const RowField* f = get_row_field(tag, row);
  RowFieldView fv = row_field_repr(f);
  using RK = RowFieldView::Kind;
  if (fv.kind == RK::Rabsent) return;  // assert false
  if (fv.kind == RK::Reither && fv.constant && fv.arg_types.empty() && !row_closed(row)) {
    link_row_field_ext(f, RF_PRESENT_NONE_LIT());
  } else if (fv.kind == RK::Reither && !fv.constant && !fv.arg_types.empty() && !row_closed(row)) {
    TypeExpr* ty = fv.arg_types[0];
    link_row_field_ext(f, rf_present(ty));
    if (!opat) throw std::logic_error("finalize_variant");
    for (TypeExpr* t : fv.arg_types) unify_pat(opat->pat_env, opat, t);
  } else if (fv.kind == RK::Reither && fv.matched && !has_fixed_explanation(row)) {
    link_row_field_ext(f, rf_either(nullptr, fv.constant, {}, false));
  }
}

bool has_variants(const tt::Pattern* p) {
  return tt::exists_general_pattern(
      [](const tt::Pattern* q) { return q->pat_desc->kind == PK::Tpat_variant; }, p);
}

void finalize_variants(const tt::Pattern* p) {
  tt::iter_general_pattern(
      [](const tt::Pattern* q) {
        if (auto* v = as<tt::Tpat_variant>(q->pat_desc)) finalize_variant(q, v->label, v->arg, v->row);
      },
      p);
}

// ---- pattern variables -------------------------------------------------------------------
static std::vector<PatternVariable> continuation_variable(const std::optional<ContinuationVar>& c) {
  if (!c) return {};
  return {PatternVariable{c->id, c->desc->val_type, c->desc->val_loc, PatternVariableKind::Continuation_var,
                          pt::Attributes{}, c->desc->val_uid}};
}

std::shared_ptr<TypePatState> create_type_pat_state(const std::optional<ContinuationVar>& cont,
                                                    const ModulePatternsRestriction& allow_modules) {
  ModuleVariables mv;
  using MK = ModulePatternsRestriction::Kind;
  switch (allow_modules.kind) {
    case MK::Modules_allowed:
      mv = ModuleVariables{ModuleVariables::Kind::Modvars_allowed, allow_modules.scope, {}};
      break;
    case MK::Modules_ignored: mv = ModuleVariables{ModuleVariables::Kind::Modvars_ignored}; break;
    case MK::Modules_rejected: mv = ModuleVariables{ModuleVariables::Kind::Modvars_rejected}; break;
  }
  auto s = std::make_shared<TypePatState>();
  s->tps_pattern_variables = continuation_variable(cont);
  s->tps_module_variables = mv;
  return s;
}

TypePatState copy_type_pat_state(const TypePatState& s) { return s; }
void blit_type_pat_state(const TypePatState& src, TypePatState& dst) { dst = src; }

env::t maybe_add_pattern_variables_ghost(const Location& loc_let, env::t env,
                                         const std::vector<PatternVariable>& pv) {
  // List.fold_right
  for (auto it = pv.rbegin(); it != pv.rend(); ++it) {
    std::string_view name = ident::name(it->pv_id);
    if (!env::bound_value(name, env)) {
      env::ValueUnboundReason r{};
      r.kind = env::ValueUnboundReason::Kind::Val_unbound_ghost_recursive;
      r.ghost_loc = loc_let;
      env = env::enter_unbound_value(name, r, env);
    }
  }
  return env;
}

std::pair<Ident::t, Uid> enter_variable(TypePatState& tps, const Location& loc,
                                        const pt::StrLoc& name, TypeExpr* ty,
                                        const pt::Attributes& attrs, bool is_module,
                                        bool is_as_variable) {
  for (auto& v : tps.tps_pattern_variables)
    if (ident::name(v.pv_id) == name.txt) {
      Error e = err(loc, env::empty(), EK::Multiply_bound_variable);
      e.name = std::string(name.txt);
      raise_error(e);  // Error.log_or_raise
    }
  Ident::t id;
  if (is_module) {
    // Unpack patterns result in both a module declaration and a value
    // variable of the same name being entered into the environment.
    ModuleVariables& mv = tps.tps_module_variables;
    switch (mv.kind) {
      case ModuleVariables::Kind::Modvars_ignored: id = Ident::create_local(name.txt); break;
      case ModuleVariables::Kind::Modvars_rejected:
        raise_error(err(loc, env::empty(), EK::Modules_not_allowed));  // log_or_raise
      case ModuleVariables::Kind::Modvars_allowed: {
        id = Ident::create_scoped(mv.scope, name.txt);
        mv.module_variables.insert(mv.module_variables.begin(),
                                   ModuleVariable{id, name, loc, uid::mk(env::get_current_unit())});
        break;
      }
    }
  } else {
    id = Ident::create_local(name.txt);
  }
  Uid pv_uid = uid::mk(env::get_current_unit());
  tps.tps_pattern_variables.insert(
      tps.tps_pattern_variables.begin(),
      PatternVariable{id, ty, loc,
                      is_as_variable ? PatternVariableKind::As_var : PatternVariableKind::Std_var,
                      attrs, pv_uid});
  return {id, pv_uid};
}

static std::vector<PatternVariable> sort_pattern_variables(const std::vector<PatternVariable>& vs) {
  return ocaml_list::stable_sort(
      [](const PatternVariable& a, const PatternVariable& b) {
        int c = ident::name(a.pv_id).compare(ident::name(b.pv_id));
        return c < 0 ? -1 : c > 0 ? 1 : 0;
      },
      vs);
}

std::vector<std::pair<Ident::t, Ident::t>> enter_orpat_variables(
    const Location& loc, env::t env, const std::vector<PatternVariable>& p1_vs0,
    const std::vector<PatternVariable>& p2_vs0) {
  // unify_vars operate on sorted lists
  std::vector<PatternVariable> p1_vs = sort_pattern_variables(p1_vs0);
  std::vector<PatternVariable> p2_vs = sort_pattern_variables(p2_vs0);
  auto vars = [](const std::vector<PatternVariable>& vs, std::size_t from) {
    std::vector<Ident::t> out;
    for (std::size_t k = from; k < vs.size(); ++k) out.push_back(vs[k].pv_id);
    return out;
  };
  std::vector<std::pair<Ident::t, Ident::t>> out;
  std::size_t i = 0, j = 0;
  for (;;) {
    if (i < p1_vs.size() && j < p2_vs.size() && ident::equal(p1_vs[i].pv_id, p2_vs[j].pv_id)) {
      Ident::t x1 = p1_vs[i].pv_id, x2 = p2_vs[j].pv_id;
      if (x1 != x2) {
        TypeExpr* t1 = p1_vs[i].pv_type;
        TypeExpr* t2 = p2_vs[j].pv_type;
        try {
          ctype::unify_var(env, ctype::newvar(), t1);
          ctype::unify(env, t1, t2);
        } catch (const ctype::Unify& u) {
          Error e = err(loc, env, EK::Or_pattern_type_clash);
          e.id = x1;
          e.trace = u.err;
          raise_error(e);
        }
        out.push_back({x2, x1});
      }
      ++i;
      ++j;
      continue;
    }
    if (i == p1_vs.size() && j == p2_vs.size()) return out;
    if (i == p1_vs.size() || j == p2_vs.size()) {
      Error e = err(loc, env, EK::Orpat_vars);
      e.id = i < p1_vs.size() ? p1_vs[i].pv_id : p2_vs[j].pv_id;
      raise_error(e);
    }
    Ident::t x = p1_vs[i].pv_id, y = p2_vs[j].pv_id;
    Error e = err(loc, env, EK::Orpat_vars);
    if (ident::name(x) < ident::name(y)) {
      e.id = x;
      e.ids = vars(p2_vs, j);
    } else {
      e.id = y;
      e.ids = vars(p1_vs, i);
    }
    raise_error(e);
  }
}

// Create two instances with identical variables but independent structure
// (see typecore.ml)
static std::pair<TypeExpr*, TypeExpr*> instance_unshared(TypeExpr* ty0) {
  TypeExpr* ty = ctype::with_local_level_generalize_structure([&] { return ctype::instance(ty0); });
  // (instance ty, instance ty): right to left
  TypeExpr* b = ctype::instance(ty);
  TypeExpr* a = ctype::instance(ty);
  return {a, b};
}

static TypeExpr* build_as_type_extra(env::t env, const tt::Pattern* p, std::size_t k);
static TypeExpr* build_as_type_aux(env::t env, const tt::Pattern* p);

TypeExpr* build_as_type(env::t env, const tt::Pattern* p) { return build_as_type_extra(env, p, 0); }

static TypeExpr* build_as_type_extra_inner(env::t env, const tt::Pattern* p, TypeExpr* ty,
                                           std::size_t rest) {
  // If the type constraint is ground, then this is the best type we can
  // return, so just return an instance (cf. #12313)
  if (ctype::closed_type_expr(ty)) return ctype::instance(ty);
  // Otherwise we combine the inferred type for the pattern with the
  // non-ground constraint in a non-ambivalent way
  TypeExpr* as_ty = build_as_type_extra(env, p, rest);
  auto [ty1, ty2] = instance_unshared(ty);
  // This call to unify may only fail due to missing GADT equations
  unify_pat_types(p->pat_loc, env, ctype::instance(as_ty), ty1);
  return ty2;
}

static TypeExpr* build_as_type_extra(env::t env, const tt::Pattern* p, std::size_t k) {
  if (k == p->pat_extra.size()) return build_as_type_aux(env, p);
  const tt::PatExtra& x = p->pat_extra[k].extra;
  using XE = tt::PatExtra::Kind;
  switch (x.kind) {
    case XE::Tpat_type:
    case XE::Tpat_open: return build_as_type_extra(env, p, k + 1);
    case XE::Tpat_unpack:
      if (!x.pack) return build_as_type_extra(env, p, k + 1);
      return build_as_type_extra_inner(env, p, newgenty(tpackage(x.pack->tpt_type)), k + 1);
    case XE::Tpat_constraint: return build_as_type_extra_inner(env, p, x.cty->ctyp_type, k + 1);
  }
  throw std::logic_error("build_as_type_extra");
}

static const tt::Pattern* with_type(const tt::Pattern* p, TypeExpr* ty) {
  tt::Pattern* q = make<tt::Pattern>(*p);
  q->pat_type = ty;
  return q;
}

static TypeExpr* build_as_type_aux(env::t env, const tt::Pattern* p) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_alias: return build_as_type(env, as<tt::Tpat_alias>(d)->pat);
    case PK::Tpat_tuple: {
      std::vector<LabeledTy> tys;
      for (auto& x : as<tt::Tpat_tuple>(d)->pats) tys.push_back({x.label, build_as_type(env, x.pat)});
      return ctype::newty(ttuple(slice(tys)));
    }
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      bool keep = c->cstr->cstr_private == PrivateFlag::Private ||
                  c->annot != nullptr;  // be lazy and keep the type for node constraints
      if (keep) return p->pat_type;
      std::vector<TypeExpr*> tyl;
      for (auto* q : c->args) tyl.push_back(build_as_type(env, q));
      ctype::InstancedConstructor ic =
          ctype::instance_constructor(ctype::ExistentialTreatment{nullptr}, c->cstr);
      // [p] is a valid result of type inference (see typecore.ml): this
      // unification should not fail
      if (c->args.size() != ic.args.size()) throw std::invalid_argument("List.iter2");
      for (std::size_t k = 0; k < c->args.size(); ++k)
        unify_pat(env, with_type(c->args[k], tyl[k]), ic.args[k]);
      return ic.res;
    }
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      TypeExpr* ty = v->arg ? build_as_type(env, v->arg) : nullptr;
      RowFieldEntry f{v->label, rf_present(ty)};
      TypeExpr* more = ctype::newvar();
      return ctype::newty(tvariant(create_row(slice({f}), more, false, nullptr, nullptr)));
    }
    case PK::Tpat_record: {
      auto* r = as<tt::Tpat_record>(d);
      const LabelDescription* lbl = r->fields[0].label;
      if (lbl->lbl_private == PrivateFlag::Private) return p->pat_type;
      TypeExpr* ty = ctype::newvar();
      std::vector<std::pair<long, const tt::Pattern*>> ppl;
      for (auto& f : r->fields) ppl.push_back({f.label->lbl_pos, f.pat});
      auto assoc = [&](long pos) -> const tt::Pattern* {
        for (auto& [k, q] : ppl)
          if (k == pos) return q;
        return nullptr;
      };
      for (const LabelDescription* l : lbl->lbl_all) {
        ctype::InstancedLabel il = ctype::instance_label(false, l);
        unify_pat(env, with_type(p, ty), il.res);
        bool refinable = l->lbl_mut == MutableFlag::Immutable && assoc(l->lbl_pos) &&
                         get_desc(l->lbl_arg)->kind != DescKind::Tpoly;
        if (refinable) {
          const tt::Pattern* arg = assoc(l->lbl_pos);
          unify_pat(env, with_type(arg, build_as_type(env, arg)), il.arg);
        } else {
          ctype::InstancedLabel il2 = ctype::instance_label(false, l);
          unify_pat_types(p->pat_loc, env, il.arg, il2.arg);
          unify_pat(env, p, il2.res);
        }
      }
      return ty;
    }
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      if (!o->row) {
        // `let ty1 = .. and ty2 = ..`: left to right
        TypeExpr* ty1 = build_as_type(env, o->p1);
        TypeExpr* ty2 = build_as_type(env, o->p2);
        unify_pat(env, with_type(o->p2, ty2), ty1);
        return ty1;
      }
      RowDescRepr r = row_repr(o->row);
      TypeExpr* more = ctype::newvar();
      return ctype::newty(tvariant(create_row(slice(r.fields), more, false, r.fixed, r.name)));
    }
    case PK::Tpat_any:
    case PK::Tpat_var:
    case PK::Tpat_constant:
    case PK::Tpat_array:
    case PK::Tpat_lazy: return p->pat_type;
    default: throw std::logic_error("build_as_type_aux: computation pattern");
  }
}

// ---- constraint solving during typing of patterns --------------------------------------
// To avoid false-positives of the escape check for existentials, we need
// to raise levels above the highest scope inside the pattern (see
// typecore.ml).
TypeExpr* solve_Ppat_alias(env::t env, const tt::Pattern* pat) {
  return ctype::with_local_level_generalize([&] {
    return ctype::with_level(generic_level - 10, [&] { return build_as_type(env, pat); });
  });
}

// Extracts the first element from a list matching a label
static std::optional<std::pair<const pt::Pattern*, std::vector<pt::LabeledPattern>>> extract_pat(
    OptStr label, const std::vector<pt::LabeledPattern>& patl) {
  for (std::size_t k = 0; k < patl.size(); ++k)
    if (label == patl[k].label) {
      std::vector<pt::LabeledPattern> rest(patl.begin(), patl.begin() + static_cast<long>(k));
      rest.insert(rest.end(), patl.begin() + static_cast<long>(k) + 1, patl.end());
      return std::make_pair(patl[k].pat, rest);
    }
  return std::nullopt;
}

static std::optional<std::pair<const pt::Pattern*, std::vector<pt::LabeledPattern>>>
extract_or_mk_pat(OptStr label, const std::vector<pt::LabeledPattern>& rem, ClosedFlag closed) {
  auto r = extract_pat(label, rem);
  if (r) return r;  // Take the first match from patl
  if (closed == ClosedFlag::Open) {
    // No match, but the partial pattern allows us to generate a _
    // (Ast_helper.Pat.mk Ppat_any: !default_loc)
    auto* p = make<pt::Pattern>(make<pt::Ppat_any>(pt::PatternDesc::Kind::Ppat_any), location::none(),
                                pt::LocationStack{}, pt::Attributes{});
    return std::make_pair(static_cast<const pt::Pattern*>(p), rem);
  }
  return std::nullopt;
}

// Reorders [patl] to match the label order in [labeled_tl] (see typecore.ml)
std::vector<pt::LabeledPattern> reorder_pat(const Location& loc, ctype::PatternEnv* penv,
                                            const std::vector<pt::LabeledPattern>& patl,
                                            ClosedFlag closed, Slice<LabeledTy> labeled_tl,
                                            TypeExpr* expected_ty) {
  std::vector<pt::LabeledPattern> taken;  // head first
  std::vector<pt::LabeledPattern> rem = patl;
  for (auto& lt : labeled_tl) {
    auto r = extract_or_mk_pat(lt.label, rem, closed);
    if (!r) {
      Error e = err(loc, penv->env, EK::Missing_tuple_label);
      e.optlabel = lt.label;
      e.ty = expected_ty;
      raise_error(e);
    }
    taken.insert(taken.begin(), pt::LabeledPattern{lt.label, r->first});
    rem = r->second;
  }
  if (!rem.empty()) {
    Error e = err(loc, penv->env, EK::Extra_tuple_label);
    e.optlabel = rem[0].label;
    e.ty = expected_ty;
    raise_error(e);
  }
  if (closed == ClosedFlag::Open && labeled_tl.size() == patl.size())
    prerr_warning(loc, WK::Unnecessarily_partial_tuple_pattern);
  std::reverse(taken.begin(), taken.end());
  return taken;
}

// This assumes the [args] have already been reordered according to the
// [expected_ty], if needed.
std::vector<LabeledTy> solve_Ppat_tuple(const Location& loc, ctype::PatternEnv* env,
                                        const std::vector<pt::LabeledPattern>& args,
                                        TypeExpr* expected_ty) {
  std::vector<LabeledTy> vars;
  for (auto& a : args) vars.push_back({a.label, newgenvar()});
  TypeExpr* ty = newgenty(ttuple(slice(vars)));
  TypeExpr* e = ctype::generic_instance(expected_ty);
  unify_pat_types_penv(loc, env, ty, e);
  return vars;
}

struct IdDecl {
  pt::StrLoc name;       // {name with txt = id}: the location, with the ident below
  Ident::t id;
  const TypeDeclaration* decl;
  TypeExpr* tv;
};

static std::pair<std::vector<TypeExpr*>, const tt::ConstructTypeAnnot*> solve_constructor_annotation(
    TypePatState& tps, ctype::PatternEnv* penv, Slice<pt::StrLoc> name_list, const pt::CoreType* sty,
    const std::vector<TypeExpr*>& ty_args, const std::vector<TypeExpr*>& ty_ex,
    const std::function<void()>& unify_res) {
  if (penv->in_counterexample) throw std::logic_error("solve_constructor_annotation");
  long expansion_scope = penv->equations_scope;
  // Introduce fresh type names that expand to type variables.  They should
  // eventually be bound to ground types.
  std::vector<IdDecl> ids_decls;
  for (auto& name : name_list) {
    TypeExpr* tv = ctype::newvar();
    const TypeDeclaration* decl = ctype::new_local_type(TypeOrigin{}, name.loc, tv, ident::lowest_scope);
    auto [id, new_env] = env::enter_type(static_cast<int>(expansion_scope), name.txt, decl, penv->env);
    penv->set_env(new_env);
    ids_decls.push_back(IdDecl{name, id, decl, tv});
  }
  // Translate the type annotation using these type names.
  typetexp::Delayed dl = ctype::with_local_level_generalize_structure(
      [&] { return typetexp::transl_simple_type_delayed(penv->env, sty); });
  const tt::CoreType* cty = dl.cty;
  TypeExpr* ty = dl.ty;
  tps.tps_pattern_force.insert(tps.tps_pattern_force.begin(), dl.force);
  // Only unify the return type after generating the ids
  unify_res();
  std::vector<TypeExpr*> ty_args2;
  {
    // `let ty1 = instance ty and ty2 = instance ty`: left to right
    TypeExpr* ty1 = ctype::instance(ty);
    TypeExpr* ty2 = ctype::instance(ty);
    if (ty_args.empty()) throw std::logic_error("solve_constructor_annotation");
    if (ty_args.size() == 1) {
      unify_pat_types(cty->ctyp_loc, penv->env, ty1, ty_args[0]);
      ty_args2 = {ty2};
    } else {
      std::vector<LabeledTy> tl;
      for (TypeExpr* t : ty_args) tl.push_back({OptStr::none(), t});
      unify_pat_types(cty->ctyp_loc, penv->env, ty1, ctype::newty(ttuple(slice(tl))));
      auto* tu = as<Ttuple>(get_desc(ctype::expand_head(penv->env, ty2)));
      if (!tu) throw std::logic_error("solve_constructor_annotation");
      for (auto& e : tu->elems) ty_args2.push_back(e.ty);
    }
  }
  if (!ids_decls.empty()) {
    std::vector<Ident::t> ids;
    for (auto& x : ids_decls) ids.push_back(x.id);
    // First process the existentials introduced by this constructor.  Just
    // need to make their definitions abstract.
    std::vector<IdDecl> rem = ids_decls;
    auto assoc = [](std::vector<IdDecl>& l, Ident::t id) -> IdDecl* {
      for (auto& x : l)
        if (ident::same(x.id, id)) return &x;
      return nullptr;
    };
    for (TypeExpr* tv : ty_ex) {
      const TypeDesc* desc = get_desc(tv);
      auto* c = as<Tconstr>(desc);
      IdDecl* found = nullptr;
      if (c && c->path->kind == Path::Kind::Pident && c->args.empty()) found = assoc(rem, c->path->id);
      if (!found) {
        Error e = err(cty->ctyp_loc, penv->env, EK::Unbound_existential);
        e.ids = ids;
        e.ty = ty;
        raise_error(e);
      }
      IdDecl* dm = assoc(ids_decls, c->path->id);
      TypeDeclaration* d2 = make<TypeDeclaration>(*dm->decl);
      d2->type_manifest = nullptr;
      env::t env = env::add_type(false, c->path->id, d2, penv->env);
      penv->set_env(env);
      // We have changed the definition, so clean up
      cleanup_abbrev_memo();
      // Since id is now abstract, this does not create a cycle
      unify_pat_types(cty->ctyp_loc, env, tv, dm->tv);
      Ident::t rid = c->path->id;
      rem.erase(std::remove_if(rem.begin(), rem.end(),
                               [&](const IdDecl& x) { return ident::same(x.id, rid); }),
                rem.end());
    }
    // The other type names should be bound to newly introduced existentials.
    std::vector<Ident::t> bound_ids = ids;  // head first
    for (auto& x : rem) {
      TypeExpr* tv2 = ctype::expand_head(penv->env, x.tv);
      auto* c = as<Tconstr>(get_desc(tv2));
      if (c && c->path->kind == Path::Kind::Pident && c->args.empty()) {
        Ident::t id2 = c->path->id;
        for (Ident::t b : bound_ids)
          if (ident::same(id2, b)) {
            Error e = err(cty->ctyp_loc, penv->env, EK::Bind_existential);
            e.binding = ExistentialBinding::Bind_already_bound;
            e.id = x.id;
            e.ty = tv2;
            raise_error(e);
          }
        // Both id and id' are Scoped identifiers, so their stamps grow
        if (ident::scope(id2) != penv->equations_scope || ident::compare_stamp(x.id, id2) > 0) {
          Error e = err(cty->ctyp_loc, penv->env, EK::Bind_existential);
          e.binding = ExistentialBinding::Bind_not_in_scope;
          e.id = x.id;
          e.ty = tv2;
          raise_error(e);
        }
        bound_ids.insert(bound_ids.begin(), id2);
      } else {
        Error e = err(cty->ctyp_loc, penv->env, EK::Bind_existential);
        e.binding = ExistentialBinding::Bind_non_locally_abstract;
        e.id = x.id;
        e.ty = tv2;
        raise_error(e);
      }
      TypeDeclaration* d2 = make<TypeDeclaration>(*x.decl);
      d2->type_manifest = ctype::duplicate_type(tv2);
      d2->manifest_obj.reset();  // a new Some block
      penv->set_env(env::add_type(false, x.id, d2, penv->env));
    }
    if (!rem.empty()) cleanup_abbrev_memo();
  }
  std::vector<std::pair<Ident::t, Location>> vars;
  for (auto& x : ids_decls) vars.push_back({x.id, x.name.loc});
  if (ids_decls.empty()) return {ty_args2, make<tt::ConstructTypeAnnot>(tt::ConstructTypeAnnot{{}, cty})};
  return {ty_args2, make<tt::ConstructTypeAnnot>(tt::ConstructTypeAnnot{slice(vars), cty})};
}

SolvedConstruct solve_Ppat_construct(TypePatState& tps, ctype::PatternEnv* penv,
                                     const Location& loc, const ConstructorDescription* constr,
                                     std::optional<ExistentialRestriction> no_existentials,
                                     const ExistentialStyp* existential_styp, TypeExpr* expected_ty) {
  // if constructor is gadt, we must verify that the expected type has the
  // correct head
  if (constr->cstr_generalized) unify_head_only(loc, penv, constr, ctype::instance(expected_ty));
  // PR#7214: do not use gadt unification for toplevel lets
  auto unify_res = [&](TypeExpr* ty_res, TypeExpr* e) {
    bool refine = constr->cstr_generalized && !no_existentials;
    // Here [ty_res] contains only fresh (non-leaking) type variables
    return unify_pat_types_return_equated_pairs(refine, loc, penv, ty_res, e);
  };
  struct R {
    std::vector<TypeExpr*> ty_args;
    btype::TypePairs* equated_types;
    const tt::ConstructTypeAnnot* existential_ctyp;
  };
  R r = ctype::with_local_level_generalize_structure([&] {
    TypeExpr* e = ctype::instance(expected_ty);
    std::vector<TypeExpr*> ty_args;
    TypeExpr* ty_res;
    btype::TypePairs* equated_types;
    const tt::ConstructTypeAnnot* existential_ctyp = nullptr;
    if (!existential_styp) {
      ctype::InstancedConstructor ic = ctype::instance_constructor(ctype::ExistentialTreatment{penv}, constr);
      ty_args = ic.args;
      ty_res = ic.res;
      equated_types = unify_res(ty_res, e);
    } else {
      ctype::ExistentialTreatment treatment{existential_styp->name_list.empty() ? penv : nullptr};
      ctype::InstancedConstructor ic = ctype::instance_constructor(treatment, constr);
      ty_res = ic.res;
      btype::TypePairs* eq = nullptr;
      bool forced = false;
      auto force = [&] {
        if (!forced) {
          eq = unify_res(ty_res, e);
          forced = true;
        }
        return eq;
      };
      auto [ta, ec] = solve_constructor_annotation(tps, penv, existential_styp->name_list,
                                                   existential_styp->sty, ic.args, ic.existentials,
                                                   [&] { force(); });
      ty_args = ta;
      existential_ctyp = ec;
      equated_types = force();
    }
    if (!constr->cstr_existentials.empty())
      ctype::lower_variables_only(penv->env, penv->equations_scope, ty_res);
    return R{ty_args, equated_types, existential_ctyp};
  });
  if (clflags::principal && !penv->in_counterexample) {
    // Do not warn for counter-examples
    struct WarnOnlyOnce {};
    try {
      r.equated_types->iter([&](TypeExpr* t1, TypeExpr* t2) {
        if (!(ctype::fully_generic(t1) && ctype::fully_generic(t2))) {
          prerr_warning(loc, not_principal("typing this pattern requires considering@ @[%a@]@ and@ @[%a@]@ as@ "
                                           "equal.@ But@ the@ knowledge@ of@ these@ types",
                                           inline_type_expr(t1), inline_type_expr(t2)));
          throw WarnOnlyOnce{};
        }
      });
    } catch (const WarnOnlyOnce&) {
    }
  }
  return SolvedConstruct{r.ty_args, r.existential_ctyp};
}

TypeExpr* solve_Ppat_record_field(const Location& loc, ctype::PatternEnv* penv,
                                  const LabelDescription* label, const pt::LidLoc& label_lid,
                                  TypeExpr* record_ty) {
  return ctype::with_local_level_generalize_structure([&] {
    ctype::InstancedLabel il = ctype::instance_label(false, label);
    try {
      unify_pat_types_penv(loc, penv, il.res, ctype::instance(record_ty));
    } catch (const Error& e) {
      if (e.kind != EK::Pattern_type_clash) throw;
      Error e2 = err(label_lid.loc, penv->env, EK::Label_mismatch);
      e2.lid = label_lid.txt;
      e2.trace = e.trace;
      raise_error(e2);
    }
    return il.arg;
  });
}

std::pair<TypeExpr*, MutableFlag> solve_Ppat_array(const Location& loc, ctype::PatternEnv* env,
                                                   TypeExpr* expected_ty) {
  TypeExpr* e = ctype::generic_instance(expected_ty);
  ArrayInfo ai = disambiguate_array_literal(loc, env->env, e);
  if (ai.ty_elt) return {ai.ty_elt, ai.mut};
  TypeExpr* ty_elt = newgenvar();
  TypeExpr* at = ai.mut == MutableFlag::Immutable ? predef::type_iarray(ty_elt) : predef::type_array(ty_elt);
  unify_pat_types_penv(loc, env, at, e);
  return {ty_elt, ai.mut};
}

TypeExpr* solve_Ppat_lazy(const Location& loc, ctype::PatternEnv* env, TypeExpr* expected_ty) {
  TypeExpr* nv = newgenvar();
  // (unify_pat_types_penv loc env (type_lazy_t nv) (generic_instance e)):
  // application arguments evaluate right to left
  TypeExpr* e = ctype::generic_instance(expected_ty);
  unify_pat_types_penv(loc, env, predef::type_lazy_t(nv), e);
  return nv;
}

SolvedConstraint solve_Ppat_constraint(TypePatState& tps, const Location& loc, env::t env,
                                       const pt::CoreType* sty, TypeExpr* expected_ty) {
  typetexp::Delayed dl = ctype::with_local_level_generalize_structure(
      [&] { return typetexp::transl_simple_type_delayed(env, sty); });
  tps.tps_pattern_force.insert(tps.tps_pattern_force.begin(), dl.force);
  // `let ty, expected_ty' = instance ty, ty`: a tuple, right to left
  TypeExpr* expected_ty2 = dl.ty;
  TypeExpr* ty = ctype::instance(dl.ty);
  unify_pat_types(loc, env, ty, ctype::instance(expected_ty));
  expected_ty2 = ctype::maybe_instance_poly(expected_ty2);
  return {dl.cty, ty, expected_ty2};
}

SolvedVariant solve_Ppat_variant(const Location& loc, ctype::PatternEnv* env,
                                 std::string_view tag, bool no_arg, TypeExpr* expected_ty) {
  std::vector<TypeExpr*> arg_type;
  if (!no_arg) arg_type.push_back(newgenvar());
  std::vector<RowFieldEntry> fields{{zborrow(tag), rf_either(nullptr, no_arg, slice(arg_type), true)}};
  auto make_row = [&](TypeExpr* more) {
    return create_row(slice(fields), more, false, nullptr, nullptr);
  };
  const RowDesc* row = make_row(newgenvar());
  TypeExpr* e = ctype::generic_instance(expected_ty);
  // PR#7404: allow some_private_tag blindly, as it would not unify with the
  // abstract row variable
  if (tag != some_private_tag) unify_pat_types_penv(loc, env, newgenty(tvariant(row)), e);
  // (arg_type, make_row (newvar ()), instance expected_ty): right to left
  TypeExpr* ie = ctype::instance(e);
  const RowDesc* row2 = make_row(ctype::newvar());
  return {arg_type, row2, ie};
}

// Building the or-pattern corresponding to a polymorphic variant type
std::pair<Path::t, const tt::Pattern*> build_or_pat(env::t env, const Location& loc,
                                                    const pt::LidLoc& lid) {
  auto [path, decl] = env::lookup_type(true, lid.loc, lid.txt, env);
  std::vector<TypeExpr*> tyl;
  for (std::size_t k = 0; k < decl->type_params.size(); ++k) tyl.push_back(ctype::newvar());
  const RowDesc* row0;
  {
    TypeExpr* ty = ctype::expand_head(
        env, ctype::newty(tconstr(path, slice(tyl), make<MemoRef>(mnil()))));
    auto* v = as<Tvariant>(get_desc(ty));
    if (!(v && static_row(v->row))) {
      Error e = err(lid.loc, env, EK::Not_a_polymorphic_variant_type);
      e.lid = lid.txt;
      raise_error(e);
    }
    row0 = v->row;
  }
  std::vector<std::pair<std::string_view, const tt::Pattern*>> pats;  // head first
  std::vector<RowFieldEntry> fields;                                   // head first
  for ([[maybe_unused]] auto& [l, f, l_obj] : row_fields(row0)) {
    RowFieldView fv = row_field_repr(f);
    if (fv.kind != RowFieldView::Kind::Rpresent) continue;
    if (!fv.present) {
      const RowField* f2 = rf_either(nullptr, true, {}, true);
      pats.insert(pats.begin(), {l, nullptr});
      fields.insert(fields.begin(), {l, f2});
    } else {
      const RowField* f2 = rf_either(nullptr, false, slice({fv.present}), true);
      auto* any = make<tt::Pattern>(make<tt::Tpat_any>(PK::Tpat_any), location::none(),
                                    Slice<tt::PatExtraItem>{}, fv.present, env, tt::Attributes{});
      pats.insert(pats.begin(), {l, any});
      fields.insert(fields.begin(), {l, f2});
    }
  }
  std::reverse(fields.begin(), fields.end());
  const PathArgs* name = make<PathArgs>(path, slice(tyl));
  // make_row closes over one [fields] list: both rows share it
  Slice<RowFieldEntry> fields_list = slice(fields);
  auto make_row = [&](TypeExpr* more) { return create_row(fields_list, more, false, nullptr, name); };
  TypeExpr* ty = ctype::newty(tvariant(make_row(ctype::newvar())));
  Location gloc = loc;
  gloc.loc_ghost = true;
  gloc = location::distinct_record(gloc);  // {loc with loc_ghost = true}
  tt::RowDescRef* row2 = make<tt::RowDescRef>(tt::RowDescRef{make_row(ctype::newvar())});
  std::vector<const tt::Pattern*> ps;
  for (auto& [l, p] : pats)
    ps.push_back(make<tt::Pattern>(make<tt::Tpat_variant>(tt::Tpat_variant{{PK::Tpat_variant}, l, p, row2}),
                                   gloc, Slice<tt::PatExtraItem>{}, ty, env, tt::Attributes{}));
  if (ps.empty()) {
    // empty polymorphic variants: not possible with the concrete language
    // but valid at the ast level
    Error e = err(lid.loc, env, EK::Not_a_polymorphic_variant_type);
    e.lid = lid.txt;
    raise_error(e);
  }
  const tt::Pattern* r = ps[0];
  for (std::size_t k = 1; k < ps.size(); ++k)
    r = make<tt::Pattern>(make<tt::Tpat_or>(tt::Tpat_or{{PK::Tpat_or}, ps[k], r, row0}), gloc,
                          Slice<tt::PatExtraItem>{}, ty, env, tt::Attributes{});
  tt::Pattern* rr = make<tt::Pattern>(*r);
  rr->pat_loc = loc;
  return {path, rp(rr)};
}

// ---- type paths ------------------------------------------------------------------------
Path::t expand_path(env::t env, Path::t p) {
  for (;;) {
    const TypeDeclaration* decl = nullptr;
    try {
      decl = env::find_type(p, env);
    } catch (const env::NotFound&) {
    }
    if (decl && decl->type_manifest) {
      auto* c = as<Tconstr>(get_desc(decl->type_manifest));
      if (!c) throw std::logic_error("expand_path");
      p = c->path;
      continue;
    }
    Path::t p2 = env::normalize_type_path(nullptr, env, p);
    if (path::same(p, p2)) return p;
    p = p2;
  }
}

bool compare_type_path(env::t env, Path::t tpath1, Path::t tpath2) {
  // Path.same's arguments right to left: expanding may force components,
  // which creates type nodes
  Path::t p2 = expand_path(env, tpath2);
  Path::t p1 = expand_path(env, tpath1);
  return path::same(p1, p2);
}

Path::t get_constr_type_path(TypeExpr* ty) {
  auto* c = as<Tconstr>(get_desc(ty));
  if (!c) throw std::logic_error("get_constr_type_path");
  return c->path;
}

// ---- NameChoice ------------------------------------------------------------------------
// The functor, as a template over the name's description type.
struct LabelName {
  using T = LabelDescription;
  using Usage = env::LabelUsage;
  static constexpr DatatypeKind kind = DatatypeKind::Record;
  static std::string_view get_name(const T* l) { return l->lbl_name; }
  static TypeExpr* get_type(const T* l) { return l->lbl_res; }
  static std::vector<std::pair<const T*, std::function<void()>>> lookup_all_from_type(
      const Location& loc, Usage usage, Path::t path, env::t env) {
    return env::lookup_all_labels_from_type(true, loc, usage, path, env);
  }
  static bool in_env(const T* lbl) {
    using RK = RecordRepresentation::Kind;
    switch (lbl->lbl_repres.kind) {
      case RK::Record_regular:
      case RK::Record_float: return true;
      case RK::Record_unboxed: return !lbl->lbl_repres.unboxed_inlined;
      default: return false;
    }
  }
};
struct ConstructorName {
  using T = ConstructorDescription;
  using Usage = env::ConstructorUsage;
  static constexpr DatatypeKind kind = DatatypeKind::Variant;
  static std::string_view get_name(const T* c) { return c->cstr_name; }
  static TypeExpr* get_type(const T* c) { return c->cstr_res; }
  static std::vector<std::pair<const T*, std::function<void()>>> lookup_all_from_type(
      const Location& loc, Usage usage, Path::t path, env::t env) {
    auto x = env::lookup_all_constructors_from_type(true, loc, usage, path, env);
    if (!x.empty()) return x;
    if (env::find_type(path, env)->type_kind->kind == TypeKind::Kind::Type_open) {
      // Extension constructors cannot be found by looking at the type
      // declaration.  We scan the whole environment to get an accurate
      // spellchecking hint in the subsequent error message
      std::vector<std::pair<const T*, std::function<void()>>> acc;  // head first
      env::fold_constructors(
          [&](const T* c) {
            if (compare_type_path(env, path, get_constr_type_path(get_type(c))))
              acc.insert(acc.begin(), {c, [] {}});
          },
          nullptr, env);
      return acc;
    }
    return {};
  }
  static bool in_env(const T*) { return true; }
};

template <class Name>
struct NameChoice {
  using T = typename Name::T;
  using Warn = std::function<void(const Location&, const warnings::Warning&)>;
  using Candidate = std::pair<const T*, std::function<void()>>;
  using Candidates = std::vector<Candidate>;
  // (candidate list, lookup error)
  struct Scope {
    bool ok;
    Candidates cands;
    Location err_loc;
    env::t err_env;
    env::LookupError err;
  };
  // nonempty_candidate_filter: Ok result | Error candidates
  struct Filtered {
    bool ok;
    Candidates cands;
  };
  using Filter = std::function<Filtered(const Candidates&)>;

  static Path::t get_type_path(const T* d) { return get_constr_type_path(Name::get_type(d)); }

  // raises Not_found when the lid is not an Lident
  struct NotFound {};
  static const T* lookup_from_type(env::t env, Path::t type_path, typename Name::Usage usage,
                                   const pt::LidLoc& lid) {
    Candidates descrs = Name::lookup_all_from_type(lid.loc, usage, type_path, env);
    if (lid.txt->kind != Longident::Kind::Lident) throw NotFound{};
    std::string_view name = lid.txt->s;
    for (auto& [nd, use] : descrs)
      if (Name::get_name(nd) == name) {
        use();
        return nd;
      }
    std::vector<std::string_view> valid_names;
    for (auto& [nd, _] : descrs) valid_names.push_back(Name::get_name(nd));
    throw WrongNameDisambiguation{env, WrongName{type_path, Name::kind, pt::StrLoc{name, lid.loc},
                                                 valid_names}};
  }

  static Candidate disambiguate_by_type(env::t env, Path::t tpath, const Scope& lbls) {
    if (!lbls.ok) throw NotFound{};
    for (auto& c : lbls.cands)
      if (compare_type_path(env, tpath, get_type_path(c.first))) return c;
    throw NotFound{};
  }

  // ---- warnings ----
  static std::vector<Path::t> unique(env::t env, std::vector<Path::t> acc, const std::vector<Path::t>& l) {
    for (Path::t x : l) {
      bool dup = false;
      for (Path::t a : acc)
        if (compare_type_path(env, x, a)) dup = true;
      if (!dup) acc.push_back(x);
    }
    return acc;
  }
  static std::vector<std::string> ambiguous_types(env::t env, const T* lbl, const Candidates& others) {
    Path::t tpath = get_type_path(lbl);
    std::vector<Path::t> others_p;
    for (auto& c : others) others_p.push_back(get_type_path(c.first));
    std::vector<Path::t> tpaths = unique(env, {tpath}, others_p);
    if (tpaths.size() == 1) return {};
    std::vector<std::string> r;
    printtyp::wrap_printing_env(true, env, [&] {
      out_type::reset();
      r = printtyp::strings_of_paths(out_type::Namespace::Type, tpaths);
    });
    return r;
  }
  // warn if there are several distinct candidates in scope
  static void warn_if_ambiguous(const Warn& warn, const pt::LidLoc& lid, env::t env, const T* lbl,
                                const Candidates& rest) {
    if (!warnings::is_active(41)) return;
    out_type::ident_conflicts::reset();
    std::vector<std::string> paths = ambiguous_types(env, lbl, rest);
    std::string expansion;
    if (std::optional<format_doc::Doc> msg = out_type::ident_conflicts::err_msg())
      expansion = format_doc::asprintf("%a", [&](format_doc::Formatter& f) { format_doc::pp_doc(f, *msg); });
    if (!paths.empty()) {
      warnings::Warning w = warnings::Warning::make(WK::Ambiguous_name);
      w.l = {std::string(longident::last(lid.txt))};
      w.l2 = paths;
      w.b = false;
      w.s = expansion;
      warn(lid.loc, w);
    }
  }
  // a non-principal type was used for disambiguation
  static void warn_non_principal(const Warn& warn, const pt::LidLoc& lid) {
    const char* name = Name::kind == DatatypeKind::Record ? "field" : "constructor";
    warn(lid.loc, not_principal("this type-based %s disambiguation", name));
  }
  // we selected a name out of the lexical scope
  static void warn_out_of_scope(const Warn& warn, const pt::LidLoc& lid, env::t env, Path::t tpath) {
    if (!warnings::is_active(40)) return;
    std::string path_s;
    printtyp::wrap_printing_env(true, env, [&] {
      path_s = format_doc::asprintf("%a", [&](format_doc::Formatter& f) { printtyp::type_path(f, tpath); });
    });
    warnings::Warning w = warnings::Warning::make(WK::Name_out_of_scope);
    w.s = path_s;
    w.l = {std::string(longident::last(lid.txt))};
    w.b = false;
    warn(lid.loc, w);
  }
  // warn if the selected name is not the last introduced in scope
  static void warn_if_disambiguated_name(const Warn& warn, const pt::LidLoc& lid, const T* lbl, const Scope& scope) {
    if (scope.ok && !scope.cands.empty() && scope.cands[0].first == lbl) return;
    warn(lid.loc, warnings::Warning::with_s(WK::Disambiguated_name, std::string(Name::get_name(lbl))));
  }

  [[noreturn]] static void force_error_raise(const Scope& s) {
    env::Error e(env::Error::Kind::Lookup_error);
    e.loc = s.err_loc;
    e.env = s.err_env;
    e.err = s.err;
    throw e;
  }
  static const Candidates& force_error(const Scope& s) {
    if (!s.ok) force_error_raise(s);
    return s.cands;
  }

  // [disambiguate] selects a concrete description for [lid] (see
  // typecore.ml)
  static const T* disambiguate(const Warn& warn, const Filter& filter, typename Name::Usage usage,
                               const pt::LidLoc& lid, env::t env,
                               const std::optional<ExpectedTypePath>& expected_type,
                               const Scope& candidates_in_scope) {
    const T* lbl = disambiguate_(warn, filter, usage, lid, env, expected_type, candidates_in_scope);
    // warn only on nominal labels
    if (Name::in_env(lbl)) warn_if_disambiguated_name(warn, lid, lbl, candidates_in_scope);
    return lbl;
  }
  static const T* disambiguate_(const Warn& warn, const Filter& filter, typename Name::Usage usage,
                                const pt::LidLoc& lid, env::t env,
                                const std::optional<ExpectedTypePath>& expected_type,
                                const Scope& candidates_in_scope) {
    if (!expected_type) {
      // no expected type => no disambiguation
      Filtered f = filter(force_error(candidates_in_scope));
      if (f.cands.empty()) throw std::logic_error("NameChoice.disambiguate");
      if (!f.ok) return f.cands[0].first;  // will fail later
      f.cands[0].second();
      Candidates rest(f.cands.begin() + 1, f.cands.end());
      warn_if_ambiguous(warn, lid, env, f.cands[0].first, rest);
      return f.cands[0].first;
    }
    Path::t tpath0 = expected_type->tpath0, tpath = expected_type->tpath;
    bool principal = expected_type->principal;
    std::optional<Candidate> by_type;
    try {
      by_type = disambiguate_by_type(env, tpath, candidates_in_scope);
    } catch (const NotFound&) {
    }
    if (by_type) {
      const T* lbl = by_type->first;
      by_type->second();
      if (!principal) {
        // Check if non-principal type is affecting result
        if (!candidates_in_scope.ok) {
          warn_non_principal(warn, lid);
        } else {
          Filtered f = filter(candidates_in_scope.cands);
          if (!f.ok) {
            warn_non_principal(warn, lid);
          } else {
            if (f.cands.empty()) throw std::logic_error("NameChoice.disambiguate");
            Path::t lbl_tpath = get_type_path(f.cands[0].first);
            // no principality warning if the non-principal type-based
            // selection corresponds to the last definition in scope
            if (!compare_type_path(env, tpath, lbl_tpath)) {
              warn_non_principal(warn, lid);
            } else {
              Candidates rest(f.cands.begin() + 1, f.cands.end());
              warn_if_ambiguous(warn, lid, env, lbl, rest);
            }
          }
        }
      }
      return lbl;
    }
    // look outside the lexical scope
    const T* found = nullptr;
    try {
      found = lookup_from_type(env, tpath, usage, lid);
    } catch (const NotFound&) {
    }
    if (found) {
      // warn only on nominal labels; structural labels cannot be qualified anyway
      if (Name::in_env(found)) warn_out_of_scope(warn, lid, env, tpath);
      if (!principal) warn_non_principal(warn, lid);
      return found;
    }
    Filtered f = filter(force_error(candidates_in_scope));
    std::pair<Path::t, Path::t> tp{tpath0, expand_path(env, tpath)};
    std::vector<std::pair<Path::t, Path::t>> tpl;
    for (auto& c : f.cands) {
      Path::t tp0 = get_type_path(c.first);
      tpl.push_back({tp0, expand_path(env, tp0)});
    }
    Error e = err(lid.loc, env, EK::Name_type_mismatch);
    e.dkind = Name::kind;
    e.lid = lid.txt;
    e.tp = tp;
    e.tpl = tpl;
    raise_error(e);
  }
};

using Label = NameChoice<LabelName>;
using Constructor = NameChoice<ConstructorName>;

static Label::Scope label_scope(const env::LookupAllLabels& l) {
  return {l.ok, l.lbls, l.err_loc, l.err_env, l.err};
}
static Constructor::Scope cstr_scope(const env::LookupAllCstrs& l) {
  return {l.ok, l.cstrs, l.err_loc, l.err_env, l.err};
}

// In record-construction expressions and patterns, we have many labels at
// once; find a candidate type in the intersection of the candidates of
// each label (see typecore.ml).
static Label::Filtered disambiguate_label_by_ids(bool closed, const std::vector<std::string_view>& ids,
                                                 const Label::Candidates& labels) {
  auto check_ids = [&](const Label::Candidate& c) {
    for (auto id : ids) {
      bool found = false;
      for (auto* l : c.first->lbl_all)
        if (l->lbl_name == id) found = true;
      if (!found) return false;
    }
    return true;
  };
  auto check_closed = [&](const Label::Candidate& c) {
    return !closed || ids.size() == c.first->lbl_all.size();
  };
  Label::Candidates l1;
  for (auto& c : labels)
    if (check_ids(c)) l1.push_back(c);
  if (l1.empty()) return {false, labels};
  Label::Candidates l2;
  for (auto& c : l1)
    if (check_closed(c)) l2.push_back(c);
  if (l2.empty()) return {false, l1};
  return {true, l2};
}

const LabelDescription* disambiguate_label(env::LabelUsage usage, const pt::LidLoc& lid, env::t env,
                                           const std::optional<ExpectedTypePath>& expected_type,
                                           const env::LookupAllLabels& candidates_in_scope,
                                           const std::vector<std::string_view>* filter_ids,
                                           bool filter_closed, const LabelWarn& warn) {
  Label::Filter filter = [](const Label::Candidates& c) { return Label::Filtered{true, c}; };
  if (filter_ids) {
    std::vector<std::string_view> ids = *filter_ids;
    filter = [ids, filter_closed](const Label::Candidates& c) {
      return disambiguate_label_by_ids(filter_closed, ids, c);
    };
  }
  return Label::disambiguate(warn ? warn : Label::Warn(location::prerr_warning), filter, usage, lid, env, expected_type,
                             label_scope(candidates_in_scope));
}

const ConstructorDescription* disambiguate_constructor(
    env::ConstructorUsage usage, const pt::LidLoc& lid, env::t env,
    const std::optional<ExpectedTypePath>& expected_type,
    const env::LookupAllCstrs& candidates_in_scope) {
  Constructor::Filter filter = [](const Constructor::Candidates& c) {
    return Constructor::Filtered{true, c};
  };
  return Constructor::disambiguate(Constructor::Warn(location::prerr_warning), filter, usage, lid, env, expected_type,
                                   cstr_scope(candidates_in_scope));
}

// Only issue warnings once per record constructor/pattern
std::vector<const LabelDescription*> disambiguate_lid_list(
    const Location& loc, bool closed, env::t env, env::LabelUsage usage,
    const std::optional<ExpectedTypePath>& expected_type, const std::vector<pt::LidLoc>& lids) {
  std::vector<std::string_view> ids;
  for (auto& lid : lids) ids.push_back(longident::last(lid.txt));
  bool w_pr = false;
  struct Amb {
    std::string s;
    std::vector<std::string> l;
    std::string ex;
  };
  std::vector<Amb> w_amb;             // head first
  std::vector<std::string> w_scope;  // head first
  std::string w_scope_ty;
  LabelWarn warn = [&](const Location& l, const warnings::Warning& msg) {
    if (msg.k == WK::Not_principal) {
      w_pr = true;
    } else if (msg.k == WK::Ambiguous_name && msg.l.size() == 1) {
      w_amb.insert(w_amb.begin(), Amb{msg.l[0], msg.l2, msg.s});
    } else if (msg.k == WK::Name_out_of_scope && msg.l.size() == 1) {
      w_scope.insert(w_scope.begin(), msg.l[0]);
      w_scope_ty = msg.s;
    } else {
      location::prerr_warning(l, msg);
    }
  };
  auto process_label = [&](const pt::LidLoc& lid) {
    env::LookupAllLabels scope = env::lookup_all_labels(true, lid.loc, usage, lid.txt, env);
    return disambiguate_label(usage, lid, env, expected_type, scope, &ids, closed, warn);
  };
  // If one label is qualified [{ foo = ...; M.bar = ... }], we will
  // disambiguate all labels using one of the qualifying modules (see
  // typecore.ml, #11630): the qualified labels are processed first.
  std::vector<const LabelDescription*> lbl_list(lids.size(), nullptr);
  for (std::size_t k = 0; k < lids.size(); ++k)
    if (lids[k].txt->kind == Longident::Kind::Ldot) lbl_list[k] = process_label(lids[k]);
  // Find a module prefix (if any) to qualify unqualified labels
  const Longident* qual = nullptr;
  Location qual_loc;
  for (auto& lid : lids)
    if (lid.txt->kind == Longident::Kind::Ldot) {
      qual = lid.txt->l1;
      qual_loc = lid.txt->l1_loc();
      break;
    }
  std::vector<const LabelDescription*> out;
  for (std::size_t k = 0; k < lids.size(); ++k) {
    if (lbl_list[k]) {
      out.push_back(lbl_list[k]);
      continue;
    }
    pt::LidLoc qual_lid = lids[k];
    if (qual && lids[k].txt->kind == Longident::Kind::Lident) {
      // {lid with txt = Ldot (modname, {lid with txt = s})}
      qual_lid.txt = Longident::ldot(qual, qual_loc, lids[k].txt->s, lids[k].loc);
    }
    out.push_back(process_label(qual_lid));
  }
  if (w_pr) {
    location::prerr_warning(loc, not_principal("this type-based record disambiguation"));
  } else if (!w_amb.empty()) {
    std::vector<Amb> amb(w_amb.rbegin(), w_amb.rend());  // List.rev !w_amb
    std::vector<Path::t> paths;
    for (const LabelDescription* l : out) paths.push_back(get_constr_type_path(l->lbl_res));
    Path::t path = paths[0];
    bool all_same = true;
    for (std::size_t i = 1; i < paths.size(); ++i)
      if (!compare_type_path(env, path, paths[i])) all_same = false;
    if (all_same) {
      warnings::Warning w = warnings::Warning::make(WK::Ambiguous_name);
      for (auto& a : amb) w.l.push_back(a.s);
      w.l2 = amb[0].l;
      w.b = true;
      w.s = amb[0].ex;
      location::prerr_warning(loc, w);
    } else {
      for (auto& a : amb) {
        warnings::Warning w = warnings::Warning::make(WK::Ambiguous_name);
        w.l = {a.s};
        w.l2 = a.l;
        w.b = false;
        w.s = a.ex;
        location::prerr_warning(loc, w);
      }
    }
  }
  if (!w_scope.empty()) {
    warnings::Warning w = warnings::Warning::make(WK::Name_out_of_scope);
    w.s = w_scope_ty;
    w.l.assign(w_scope.rbegin(), w_scope.rend());
    w.b = true;
    location::prerr_warning(loc, w);
  }
  return out;
}

// Checks over the labels mentioned in a record pattern: no duplicate
// definitions (error); properly closed (warning)
void check_recordpat_labels(const Location& loc,
                            const std::vector<tt::RecordPatField>& lbl_pat_list, ClosedFlag closed) {
  if (lbl_pat_list.empty()) return;  // should not happen
  const LabelDescription* label1 = lbl_pat_list[0].label;
  std::vector<bool> defined(label1->lbl_all.size(), false);
  for (auto& f : lbl_pat_list) {
    if (defined[static_cast<std::size_t>(f.label->lbl_pos)]) {
      Error e = err(loc, env::empty(), EK::Label_multiply_defined);
      e.name = std::string(f.label->lbl_name);
      raise_error(e);  // log_or_raise
    }
    defined[static_cast<std::size_t>(f.label->lbl_pos)] = true;
  }
  if (closed == ClosedFlag::Closed && warnings::is_active(9)) {
    std::vector<std::string> undefined;  // head first
    for (std::size_t i = 0; i < label1->lbl_all.size(); ++i)
      if (!defined[i]) undefined.insert(undefined.begin(), std::string(label1->lbl_all[i]->lbl_name));
    if (!undefined.empty()) {
      std::string u;
      for (std::size_t i = undefined.size(); i-- > 0;) {
        u += undefined[i];
        if (i > 0) u += ", ";
      }
      prerr_warning(loc, WK::Missing_record_field_pattern, u);
    }
  }
}

}  // namespace cppcaml::typing::typecore
