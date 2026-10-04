// Port of middle_end/flambda/simple_value_approx.ml (see
// simple_value_approx.hpp).
#include "cppcaml/typing/simple_value_approx.hpp"

#include <cstdio>
#include <cstring>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/effect_analysis.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/inlining_cost.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::simple_value_approx {

using format::Formatter;
using format::fprintf;
namespace F = flambda;
namespace Names = internal_variable_names;

namespace {
// Format.pp_print_float (string_of_float)
std::string string_of_float(double f) {
  char b[64];
  std::snprintf(b, sizeof b, "%.12g", f);
  std::string s = b;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || c == '-')) return s;
  return s + ".";
}

t mk(const Approx& a) { return make<Approx>(a); }
Descr d_of(DK k) {
  Descr d;
  d.kind = k;
  return d;
}

void print_var_set(Formatter& ppf, const variable::Set& s) { variable::print_set(ppf, s); }
}  // namespace

// ---- printers ---------------------------------------------------------------------
void print_value_set_of_closures(Formatter& ppf, const ValueSetOfClosures* s) {
  auto funs = [&](Formatter& f) {
    s->function_decls->funs.iter([&](variable::t id, const FunctionDeclaration*) { variable::print(f, id); });
  };
  // Variable.Map.print Variable.Set.print
  auto invariant_params = [&](Formatter& f) {
    auto elts = [&](Formatter& g) {
      s->invariant_params.force().iter([&](variable::t id, const variable::Set& v) {
        fprintf(g, "@ (@[%a@ %a@])", pr(variable::print, id), pr(print_var_set, v));
      });
    };
    fprintf(f, "@[<1>{@[%a@ @]}@]", elts);
  };
  auto size = [&](Formatter& f) {
    auto elts = [&](Formatter& g) {
      s->size.force().iter([&](variable::t id, const std::optional<long>& v) {
        fprintf(g, "@ (@[%a@ %a@])", pr(variable::print, id), [&](Formatter& h) {
          if (!v) fprintf(h, "None");
          else fprintf(h, "Some %d", *v);
        });
      });
    };
    fprintf(f, "@[<1>{@[%a@ @]}@]", elts);
  };
  fprintf(ppf, "(set_of_closures:@ %a invariant_params=%a freshening=%a size=%a)", funs, invariant_params,
          [&](Formatter& f) { freshening::project_var::print(f, s->freshening); }, size);
}

namespace {
void print_unresolved_value(Formatter& ppf, const UnresolvedValue& v) {
  if (v.set_of_closures_id) fprintf(ppf, "Set_of_closures_id %a", pr(unit_id::print, v.set_of_closures_id));
  else fprintf(ppf, "Symbol %a", pr(symbol::print, v.sym));
}

void print_function_declaration(Formatter& ppf, variable::t var, const FunctionDeclaration* f) {
  auto params = [&](Formatter& g) {
    for (const Parameter& p : f->params) fprintf(g, "@ %a", pr(variable::print, p.var));
  };
  const FunctionBody* b = f->function_body;
  if (!b) {
    fprintf(ppf, "@[<2>(%a@ =@ fun@[<2>%a@])@]@ ", pr(variable::print, var), params);
    return;
  }
  const char* stub = b->stub ? " *stub*" : "";
  const char* is_a_functor = b->is_a_functor ? " *functor*" : "";
  const char* inline_ = "";
  switch (b->inline_.kind) {
    case lambda::InlineAttribute::Kind::Always_inline:
    case lambda::InlineAttribute::Kind::Hint_inline: inline_ = " *inline*"; break;
    case lambda::InlineAttribute::Kind::Never_inline: inline_ = " *never_inline*"; break;
    case lambda::InlineAttribute::Kind::Unroll: inline_ = " *unroll*"; break;
    case lambda::InlineAttribute::Kind::Default_inline: break;
  }
  const char* specialise = b->specialise == lambda::SpecialiseAttribute::Always_specialise  ? " *specialise*"
                           : b->specialise == lambda::SpecialiseAttribute::Never_specialise ? " *never_specialise*"
                                                                                            : "";
  fprintf(ppf, "@[<2>(%a%s%s%s%s@ =@ fun@[<2>%a@] ->@ @[<2><%a>@])@]@ ", pr(variable::print, var), stub,
          is_a_functor, inline_, specialise, params, [](Formatter& g) { fprintf(g, "<Function Body>"); });
}
}  // namespace

void print_function_declarations(Formatter& ppf, const FunctionDeclarations* fd) {
  auto funs = [&](Formatter& f) {
    fd->funs.iter([&](variable::t var, const FunctionDeclaration* d) { print_function_declaration(f, var, d); });
  };
  fprintf(ppf, "@[<2>(%a)@]", funs);
}

void print_descr(Formatter& ppf, const Descr& d) {
  switch (d.kind) {
    case DK::Value_int: fprintf(ppf, "%d", d.i); break;
    case DK::Value_char: fprintf(ppf, "%c", static_cast<char>(d.i)); break;
    case DK::Value_block: {
      auto p = [&](Formatter& f) {
        for (t v : d.fields) fprintf(f, "%a@ ", pr(print, v));
      };
      fprintf(ppf, "[%i:@ @[<1>%a@]]", d.tag, p);
      break;
    }
    case DK::Value_unknown:
      if (d.unknown.unresolved)
        fprintf(ppf, "?(due to unresolved %a)", pr(print_unresolved_value, d.unknown.value));
      else fprintf(ppf, "?");
      break;
    case DK::Value_bottom: fprintf(ppf, "bottom"); break;
    case DK::Value_extern: fprintf(ppf, "_%a_", pr(unit_id::print, d.ex)); break;
    case DK::Value_symbol: fprintf(ppf, "%a", pr(symbol::print, d.sym)); break;
    case DK::Value_closure:
      fprintf(ppf, "(closure:@ %a from@ %a)", pr(variable::print, d.closure.closure_id),
              pr(print, d.closure.set_of_closures));
      break;
    case DK::Value_set_of_closures: print_value_set_of_closures(ppf, d.set); break;
    case DK::Value_unresolved: fprintf(ppf, "(unresolved %a)", pr(print_unresolved_value, d.unresolved)); break;
    case DK::Value_float:
      if (d.f) fprintf(ppf, "%s", string_of_float(*d.f));
      else fprintf(ppf, "float");
      break;
    case DK::Value_string:
      if (!d.str.contents) fprintf(ppf, "string %i", d.str.size);
      else {
        std::string s(*d.str.contents);
        if (d.str.size > 10) s = s.substr(0, 8) + "...";
        fprintf(ppf, "string %i %S", d.str.size, s);
      }
      break;
    case DK::Value_float_array:
      if (!d.float_array.contents_known) fprintf(ppf, "float_array %i", d.float_array.size);
      else fprintf(ppf, "float_array_imm %i", d.float_array.size);
      break;
    case DK::Value_boxed_int:
      switch (d.bi) {
        case BoxedInt::Int32: fprintf(ppf, "%li", static_cast<std::int32_t>(d.bival)); break;
        case BoxedInt::Int64: fprintf(ppf, "%Li", d.bival); break;
        case BoxedInt::Nativeint: fprintf(ppf, "%ni", d.bival); break;
      }
      break;
  }
}

void print(Formatter& ppf, t a) {
  auto sym = [&](Formatter& f) {
    if (!a->symbol) symbol::print_opt(f, nullptr);
    else if (!a->symbol->field) symbol::print(f, a->symbol->sym);
    else fprintf(f, "%a.(%i)", pr(symbol::print, a->symbol->sym), *a->symbol->field);
  };
  fprintf(ppf, "{ descr=%a var=%a symbol=%a }", [&](Formatter& f) { print_descr(f, a->descr); },
          pr(variable::print_opt, a->var), sym);
}

// ---- constructors ------------------------------------------------------------------
t approx(const Descr& d) { return mk(Approx{d, nullptr, std::nullopt}); }
t augment_with_variable(t a, variable::t var) {
  Approx r = *a;
  r.var = var;
  return mk(r);
}
t augment_with_symbol(t a, symbol::t sym) {
  Approx r = *a;
  r.symbol = SymbolRef{sym, std::nullopt};
  return mk(r);
}
t augment_with_symbol_field(t a, symbol::t sym, long field) {
  if (a->symbol) return a;
  Approx r = *a;
  r.symbol = SymbolRef{sym, field};
  return mk(r);
}
t replace_description(t a, const Descr& d) {
  Approx r = *a;
  r.descr = d;
  return mk(r);
}

t augment_with_kind(t a, const lambda::ValueKind& kind) {
  if (kind.kind != lambda::ValueKind::Kind::Pfloatval) return a;
  switch (a->descr.kind) {
    case DK::Value_float: return a;
    case DK::Value_unknown: case DK::Value_unresolved: {
      Descr d = d_of(DK::Value_float);
      return replace_description(a, d);
    }
    case DK::Value_block: case DK::Value_int: case DK::Value_char: case DK::Value_boxed_int:
    case DK::Value_set_of_closures: case DK::Value_closure: case DK::Value_string: case DK::Value_float_array:
    case DK::Value_bottom:
      // Unreachable
      return replace_description(a, d_of(DK::Value_bottom));
    case DK::Value_extern: case DK::Value_symbol:
      // We don't know yet
      return a;
  }
  return a;
}

lambda::ValueKind augment_kind_with_approx(t a, const lambda::ValueKind& kind) {
  switch (a->descr.kind) {
    case DK::Value_float: return lambda::ValueKind::floatval();
    case DK::Value_int: return lambda::ValueKind::intval();
    case DK::Value_boxed_int:
      switch (a->descr.bi) {
        // (literals of simple_value_approx.ml: its own static blocks)
        case BoxedInt::Int32: return lambda::ValueKind::boxedint_literal(__FILE__, BoxedInteger::Pint32);
        case BoxedInt::Int64: return lambda::ValueKind::boxedint_literal(__FILE__, BoxedInteger::Pint64);
        case BoxedInt::Nativeint: return lambda::ValueKind::boxedint_literal(__FILE__, BoxedInteger::Pnativeint);
      }
      return kind;
    default: return kind;
  }
}

t value_unknown(UnknownBecauseOf reason) {
  Descr d = d_of(DK::Value_unknown);
  d.unknown = reason;
  return approx(d);
}
t value_int(long i) {
  Descr d = d_of(DK::Value_int);
  d.i = i;
  return approx(d);
}
t value_char(long c) {
  Descr d = d_of(DK::Value_char);
  d.i = c;
  return approx(d);
}
t value_float(double f) {
  Descr d = d_of(DK::Value_float);
  d.f = f;
  return approx(d);
}
t value_any_float() { return approx(d_of(DK::Value_float)); }
t value_boxed_int(BoxedInt bi, std::int64_t i) {
  Descr d = d_of(DK::Value_boxed_int);
  d.bi = bi;
  d.bival = i;
  return approx(d);
}

t value_closure(const ValueSetOfClosures* s, variable::t closure_id, variable::t closure_var,
                variable::t set_of_closures_var, symbol::t set_of_closures_symbol) {
  Descr ds = d_of(DK::Value_set_of_closures);
  ds.set = s;
  std::optional<SymbolRef> sym;
  if (set_of_closures_symbol) sym = SymbolRef{set_of_closures_symbol, std::nullopt};
  t approx_set_of_closures = mk(Approx{ds, set_of_closures_var, sym});
  Descr d = d_of(DK::Value_closure);
  d.closure = ValueClosure{approx_set_of_closures, closure_id};
  return mk(Approx{d, closure_var, std::nullopt});
}

const ValueSetOfClosures* create_value_set_of_closures(
    const FunctionDeclarations* function_decls, variable::Map<t> bound_vars,
    variable::Map<F::SpecialisedTo> free_vars, Lazy<variable::Map<variable::Set>> invariant_params,
    Lazy<variable::Set> recursive, variable::Map<F::SpecialisedTo> specialised_args,
    freshening::project_var::T freshening, variable::Map<variable::t> direct_call_surrogates) {
  auto size = Lazy<variable::Map<std::optional<long>>>::of_fun([function_decls]() {
    variable::Set functions = function_decls->funs.keys();
    return function_decls->funs.fold(
        [&](variable::t fun_var, const FunctionDeclaration* d, variable::Map<std::optional<long>> sizes) {
          const FunctionBody* b = d->function_body;
          if (!b) return sizes;
          variable::Set params = parameter::set_vars(d->params);
          variable::Set fv = variable::Set::diff(variable::Set::diff(b->free_variables, params), functions);
          long num_free_vars = fv.cardinal();
          long max_size = inlining_cost::maximum_interesting_size_of_function_body(num_free_vars);
          std::optional<long> size = inlining_cost::lambda_smaller_prime(b->body, max_size);
          return sizes.add(fun_var, size);
        },
        variable::Map<std::optional<long>>{});
  });
  return make<ValueSetOfClosures>(ValueSetOfClosures{function_decls, bound_vars, free_vars, invariant_params, recursive,
                                                     size, specialised_args, freshening, direct_call_surrogates});
}

const ValueSetOfClosures* update_freshening_of_value_set_of_closures(const ValueSetOfClosures* s,
                                                                     freshening::project_var::T freshening) {
  // CR-someday mshinwell: We could maybe check that [freshening] is
  // reasonable.
  ValueSetOfClosures r = *s;
  r.freshening = freshening;
  return make<ValueSetOfClosures>(r);
}

t value_set_of_closures(const ValueSetOfClosures* s, variable::t set_of_closures_var) {
  Descr d = d_of(DK::Value_set_of_closures);
  d.set = s;
  return mk(Approx{d, set_of_closures_var, std::nullopt});
}

t value_block(tag::t tag, Slice<t> fields) {
  Descr d = d_of(DK::Value_block);
  d.tag = tag;
  d.fields = fields;
  return approx(d);
}
t value_extern(export_id::t ex) {
  Descr d = d_of(DK::Value_extern);
  d.ex = ex;
  return approx(d);
}
t value_symbol(symbol::t sym) {
  Descr d = d_of(DK::Value_symbol);
  d.sym = sym;
  return mk(Approx{d, nullptr, SymbolRef{sym, std::nullopt}});
}
t value_bottom() { return approx(d_of(DK::Value_bottom)); }
t value_unresolved(UnresolvedValue value) {
  Descr d = d_of(DK::Value_unresolved);
  d.unresolved = value;
  return approx(d);
}
t value_string(long size, std::optional<std::string_view> contents) {
  Descr d = d_of(DK::Value_string);
  d.str = ValueString{contents, size};
  return approx(d);
}
t value_mutable_float_array(long size) {
  Descr d = d_of(DK::Value_float_array);
  d.float_array.size = size;
  return approx(d);
}
t value_immutable_float_array(Slice<t> contents) {
  long size = static_cast<long>(contents.size());
  std::vector<t> c;  // Array.map: in order
  for (t x : contents) c.push_back(augment_with_kind(x, lambda::ValueKind::floatval()));
  Descr d = d_of(DK::Value_float_array);
  d.float_array.contents_known = true;
  d.float_array.contents = slice(c);
  d.float_array.size = size;
  return approx(d);
}

// ---- make_const ------------------------------------------------------------------
namespace {
std::pair<F::t, t> name_expr_fst(std::pair<F::named, t> p, Names::t name) {
  return {flambda_utils::name_expr(p.first, name), p.second};
}
}  // namespace

std::pair<F::named, t> make_const_int_named(long n) { return {F::n_const(F::const_int(n)), value_int(n)}; }
std::pair<F::t, t> make_const_int(long n) {
  Names::t name = n == 0 ? Names::const_zero : n == 1 ? Names::const_one : Names::const_int;
  return name_expr_fst(make_const_int_named(n), name);
}
std::pair<F::named, t> make_const_char_named(long c) {
  return {F::n_const(F::Const{F::Const::Kind::Char, c}), value_char(c)};
}
std::pair<F::t, t> make_const_char(long c) { return name_expr_fst(make_const_char_named(c), Names::const_char); }
std::pair<F::named, t> make_const_bool_named(bool b) { return make_const_int_named(b ? 1 : 0); }
std::pair<F::t, t> make_const_bool(bool b) { return name_expr_fst(make_const_bool_named(b), Names::const_bool); }
std::pair<F::named, t> make_const_float_named(double f) {
  return {F::n_allocated_const(allocated_const::float_(f)), value_float(f)};
}
std::pair<F::t, t> make_const_float(double f) { return name_expr_fst(make_const_float_named(f), Names::const_float); }
std::pair<F::named, t> make_const_boxed_int_named(BoxedInt bi, std::int64_t i) {
  allocated_const::t c = bi == BoxedInt::Int32   ? allocated_const::int32(static_cast<std::int32_t>(i))
                         : bi == BoxedInt::Int64 ? allocated_const::int64(i)
                                                 : allocated_const::nativeint(i);
  return {F::n_allocated_const(c), value_boxed_int(bi, i)};
}
std::pair<F::t, t> make_const_boxed_int(BoxedInt bi, std::int64_t i) {
  return name_expr_fst(make_const_boxed_int_named(bi, i), Names::const_boxed_int);
}

// ---- simplify ------------------------------------------------------------------------
SimplificationResult simplify(t a, F::t lam) {
  using S = SimplificationSummary;
  if (!effect_analysis::no_effects(lam)) return {lam, S::Nothing_done, a};
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_int: {
      auto [c, ap] = make_const_int(d.i);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_char: {
      auto [c, ap] = make_const_char(d.i);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_float:
      if (d.f) {
        auto [c, ap] = make_const_float(*d.f);
        return {c, S::Replaced_term, ap};
      }
      return {lam, S::Nothing_done, a};
    case DK::Value_boxed_int: {
      auto [c, ap] = make_const_boxed_int(d.bi, d.bival);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_symbol: return {flambda_utils::name_expr(F::n_symbol(d.sym), Names::symbol), S::Replaced_term, a};
    default: return {lam, S::Nothing_done, a};
  }
}

SimplificationResultNamed simplify_named(t a, F::named named) {
  using S = SimplificationSummary;
  if (!effect_analysis::no_effects_named(named)) return {named, S::Nothing_done, a};
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_int: {
      auto [c, ap] = make_const_int_named(d.i);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_char: {
      auto [c, ap] = make_const_char_named(d.i);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_float:
      if (d.f) {
        auto [c, ap] = make_const_float_named(*d.f);
        return {c, S::Replaced_term, ap};
      }
      return {named, S::Nothing_done, a};
    case DK::Value_boxed_int: {
      auto [c, ap] = make_const_boxed_int_named(d.bi, d.bival);
      return {c, S::Replaced_term, ap};
    }
    case DK::Value_symbol: return {F::n_symbol(d.sym), S::Replaced_term, a};
    default: return {named, S::Nothing_done, a};
  }
}

std::optional<std::pair<F::named, t>> simplify_var(t a) {
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_int: return make_const_int_named(d.i);
    case DK::Value_char: return make_const_char_named(d.i);
    case DK::Value_float:
      if (d.f) return make_const_float_named(*d.f);
      break;
    case DK::Value_boxed_int: return make_const_boxed_int_named(d.bi, d.bival);
    case DK::Value_symbol: return std::pair<F::named, t>{F::n_symbol(d.sym), a};
    default: break;
  }
  if (!a->symbol) return std::nullopt;
  if (!a->symbol->field) return std::pair<F::named, t>{F::n_symbol(a->symbol->sym), a};
  return std::pair<F::named, t>{F::n_read_symbol_field(a->symbol->sym, *a->symbol->field), a};
}

namespace {
SimplificationSummary join_summaries(SimplificationSummary summary, bool replaced_by_var_or_symbol) {
  if (replaced_by_var_or_symbol || summary == SimplificationSummary::Replaced_term)
    return SimplificationSummary::Replaced_term;
  return SimplificationSummary::Nothing_done;
}
}  // namespace

SimplificationResult simplify_using_env(t a, FnRef<bool(variable::t)> is_present_in_env, F::t flam) {
  bool replaced = false;
  if (a->var && is_present_in_env(a->var)) {
    replaced = true;
    flam = F::var(a->var);
  } else if (a->symbol) {
    replaced = true;
    if (!a->symbol->field) flam = flambda_utils::name_expr(F::n_symbol(a->symbol->sym), Names::symbol);
    else
      flam = flambda_utils::name_expr(F::n_read_symbol_field(a->symbol->sym, *a->symbol->field), Names::symbol_field);
  }
  SimplificationResult r = simplify(a, flam);
  r.summary = join_summaries(r.summary, replaced);
  return r;
}

SimplificationResultNamed simplify_named_using_env(t a, FnRef<bool(variable::t)> is_present_in_env, F::named named) {
  bool replaced = false;
  if (a->var && is_present_in_env(a->var)) {
    replaced = true;
    named = F::n_expr(F::var(a->var));
  } else if (a->symbol) {
    replaced = true;
    if (!a->symbol->field) named = F::n_symbol(a->symbol->sym);
    else named = F::n_read_symbol_field(a->symbol->sym, *a->symbol->field);
  }
  SimplificationResultNamed r = simplify_named(a, named);
  r.summary = join_summaries(r.summary, replaced);
  return r;
}

variable::t simplify_var_to_var_using_env(t a, FnRef<bool(variable::t)> is_present_in_env) {
  if (a->var && is_present_in_env(a->var)) return a->var;
  return nullptr;
}

bool known(t a) {
  DK k = a->descr.kind;
  return k != DK::Value_unresolved && k != DK::Value_unknown;
}

bool useful(t a) {
  DK k = a->descr.kind;
  return k != DK::Value_unresolved && k != DK::Value_unknown && k != DK::Value_bottom;
}

bool all_not_useful(Slice<t> ts) {
  for (t x : ts)
    if (useful(x)) return false;
  return true;
}

bool warn_on_mutation(t a) {
  if (!clflags::flambda_invariant_checks) return false;
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_block: return !d.fields.empty();
    case DK::Value_string: return d.str.contents.has_value();
    case DK::Value_int: case DK::Value_char: case DK::Value_set_of_closures: case DK::Value_float:
    case DK::Value_boxed_int: case DK::Value_closure: return true;
    case DK::Value_float_array: case DK::Value_unresolved: case DK::Value_unknown: case DK::Value_bottom: return false;
    case DK::Value_extern: case DK::Value_symbol: misc::fatal_error("Simple_value_approx.warn_on_mutation");
  }
  return false;
}

t get_field(t a, long i) {
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_block:
      if (i >= 0 && i < static_cast<long>(d.fields.size())) return d.fields[static_cast<std::size_t>(i)];
      // This (unfortunately) cannot be a fatal error; it can happen if a
      // .cmx file is missing.
      return nullptr;
    case DK::Value_bottom: case DK::Value_int: case DK::Value_char: return value_bottom();
    case DK::Value_float_array: return value_unknown(other());
    case DK::Value_string: case DK::Value_float: case DK::Value_boxed_int: return nullptr;
    case DK::Value_set_of_closures: case DK::Value_closure: case DK::Value_symbol: case DK::Value_extern:
      return value_unknown(other());
    case DK::Value_unknown: return value_unknown(d.unknown);
    case DK::Value_unresolved:
      // We don't know anything, but we must remember that it comes from
      // another compilation unit in case it contains a closure.
      return value_unknown(UnknownBecauseOf{true, d.unresolved});
  }
  return nullptr;
}

std::optional<BlockApprox> check_approx_for_block(t a) {
  if (a->descr.kind == DK::Value_block) return BlockApprox{a->descr.tag, a->descr.fields};
  return std::nullopt;
}

bool equal_boxed_int(BoxedInt bi1, std::int64_t i1, BoxedInt bi2, std::int64_t i2) {
  return bi1 == bi2 && i1 == i2;
}

namespace {
bool equal_floats(const std::optional<double>& f1, const std::optional<double>& f2) {
  if (!f1 || !f2) return !f1 && !f2;
  // Allocated_const.compare_floats: the bit patterns
  std::int64_t a, b;
  std::memcpy(&a, &*f1, sizeof a);
  std::memcpy(&b, &*f2, sizeof b);
  return a == b;
}

Descr meet_descr(FnRef<t(t)> really_import_approx, const Descr& d1, const Descr& d2) {
  if (d1.kind == d2.kind) {
    switch (d1.kind) {
      case DK::Value_int:
        if (d1.i == d2.i) return d1;
        break;
      case DK::Value_symbol:
        if (symbol::equal(d1.sym, d2.sym)) return d1;
        break;
      case DK::Value_extern:
        if (unit_id::compare(d1.ex, d2.ex) == 0) return d1;
        break;
      case DK::Value_float:
        if (equal_floats(d1.f, d2.f)) return d1;
        break;
      case DK::Value_boxed_int:
        if (equal_boxed_int(d1.bi, d1.bival, d2.bi, d2.bival)) return d1;
        break;
      case DK::Value_block:
        if (d1.tag == d2.tag && d1.fields.size() == d2.fields.size()) {
          std::vector<t> fields;  // Array.mapi: in order
          for (std::size_t k = 0; k < d1.fields.size(); ++k)
            fields.push_back(meet(really_import_approx, d1.fields[k], d2.fields[k]));
          Descr r = d1;
          r.fields = slice(fields);
          return r;
        }
        break;
      default: break;
    }
  }
  return value_unknown(other())->descr;
}
}  // namespace

t meet(FnRef<t(t)> really_import_approx, t a1, t a2) {
  if (a1->descr.kind == DK::Value_bottom) return a2;
  if (a2->descr.kind == DK::Value_bottom) return a1;
  auto ext = [](t a) { return a->descr.kind == DK::Value_symbol || a->descr.kind == DK::Value_extern; };
  if (ext(a1) || ext(a2)) {
    // (the arguments right to left: a2 imported first)
    t i2 = really_import_approx(a2);
    t i1 = really_import_approx(a1);
    return meet(really_import_approx, i1, i2);
  }
  variable::t var = nullptr;
  if (a1->var && a2->var && variable::equal(a1->var, a2->var)) var = a1->var;
  std::optional<SymbolRef> sym;
  if (a1->symbol && a2->symbol && symbol::equal(a1->symbol->sym, a2->symbol->sym)) {
    const auto& f1 = a1->symbol->field;
    const auto& f2 = a2->symbol->field;
    if ((!f1 && !f2) || (f1 && f2 && *f1 == *f2)) sym = a1->symbol;
  }
  return mk(Approx{meet_descr(really_import_approx, a1->descr, a2->descr), var, sym});
}

variable::t freshen_and_check_closure_id(const ValueSetOfClosures* s, variable::t closure_id) {
  closure_id = freshening::project_var::apply_closure_id(s->freshening, closure_id);
  if (s->function_decls->funs.mem(closure_id)) return closure_id;
  Formatter f;
  fprintf(f, "Function %a not found in the set of closures@ %a@.%a@.", pr(variable::print, closure_id),
          pr(print_value_set_of_closures, s), pr(print_function_declarations, s->function_decls));
  misc::fatal_error(f.contents());
}

CheckedSetOfClosures check_approx_for_set_of_closures(t a) {
  using K = CheckedSetOfClosures::Kind;
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_unresolved: return {K::Unresolved, d.unresolved};
    case DK::Value_unknown:
      if (d.unknown.unresolved) return {K::Unknown_because_of_unresolved_value, d.unknown.value};
      return {K::Wrong};
    case DK::Value_set_of_closures:
      // Note that [var] might be [None]; we might be reaching the set of
      // closures via approximations only, with the variable originally
      // bound to the set now out of scope.
      return {K::Ok, {}, a->var, d.set};
    default: return {K::Wrong};
  }
}

CheckedSetOfClosures strict_check_approx_for_set_of_closures(t a) {
  CheckedSetOfClosures r = check_approx_for_set_of_closures(a);
  if (r.kind == CheckedSetOfClosures::Kind::Ok) return r;
  return {CheckedSetOfClosures::Kind::Wrong};
}

CheckedClosure check_approx_for_closure_allowing_unresolved(t a) {
  using K = CheckedClosure::Kind;
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_closure: {
      t soc = d.closure.set_of_closures;
      if (soc->descr.kind != DK::Value_set_of_closures) return {K::Wrong};
      symbol::t sym = soc->symbol && !soc->symbol->field ? soc->symbol->sym : nullptr;
      return {K::Ok, {}, &d.closure, soc->var, sym, soc->descr.set};
    }
    case DK::Value_unknown:
      // CR-soon mshinwell: This should be unwound once the reason for a
      // value being unknown can be correctly propagated through the export
      // info.
      if (d.unknown.unresolved) return {K::Unknown_because_of_unresolved_value, d.unknown.value};
      return {K::Unknown};
    case DK::Value_unresolved: return {K::Unresolved, d.unresolved};
    default: return {K::Wrong};
  }
}

CheckedClosure check_approx_for_closure(t a) {
  CheckedClosure r = check_approx_for_closure_allowing_unresolved(a);
  if (r.kind == CheckedClosure::Kind::Ok) return r;
  return {CheckedClosure::Kind::Wrong};
}

t approx_for_bound_var(const ValueSetOfClosures* s, variable::t var) {
  if (const t* a = s->bound_vars.find_opt(var)) return *a;
  Formatter f;
  fprintf(f, "The set-of-closures approximation %a@ does not bind the variable %a@.%s@.",
          pr(print_value_set_of_closures, s), pr(variable::print, var), "");
  misc::fatal_error(f.contents());
}

std::optional<double> check_approx_for_float(t a) {
  if (a->descr.kind == DK::Value_float) return a->descr.f;
  return std::nullopt;
}

std::optional<std::vector<double>> float_array_as_constant(const ValueFloatArray& fa) {
  if (!fa.contents_known) return std::nullopt;
  // Array.fold_right: the last element first
  std::vector<double> acc;
  for (std::size_t k = fa.contents.size(); k-- > 0;) {
    const Descr& d = fa.contents[k]->descr;
    if (d.kind != DK::Value_float || !d.f) return std::nullopt;
    acc.push_back(*d.f);
  }
  return std::vector<double>(acc.rbegin(), acc.rend());
}

std::optional<std::string_view> check_approx_for_string(t a) {
  if (a->descr.kind == DK::Value_string) return a->descr.str.contents;
  return std::nullopt;
}

SwitchBranchSelection potentially_taken_const_switch_branch(t a, long branch) {
  using S = SwitchBranchSelection;
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_unresolved: case DK::Value_unknown: case DK::Value_extern: case DK::Value_symbol:
      // In theory symbol cannot contain integers but this shouldn't matter
      // as this will always be an imported approximation
      return S::Can_be_taken;
    case DK::Value_int: case DK::Value_char: return d.i == branch ? S::Must_be_taken : S::Cannot_be_taken;
    default: return S::Cannot_be_taken;
  }
}

SwitchBranchSelection potentially_taken_block_switch_branch(t a, long tag) {
  using S = SwitchBranchSelection;
  constexpr long closure_tag = 247, infix_tag = 249, string_tag = 252, double_tag = 253, double_array_tag = 254,
                 custom_tag = 255;
  const Descr& d = a->descr;
  switch (d.kind) {
    case DK::Value_unresolved: case DK::Value_unknown: case DK::Value_extern: case DK::Value_symbol:
      return S::Can_be_taken;
    case DK::Value_int: case DK::Value_char: return S::Cannot_be_taken;
    case DK::Value_block: return d.tag == tag ? S::Must_be_taken : S::Cannot_be_taken;
    case DK::Value_float: return tag == double_tag ? S::Must_be_taken : S::Cannot_be_taken;
    case DK::Value_float_array: return tag == double_array_tag ? S::Must_be_taken : S::Cannot_be_taken;
    case DK::Value_string: return tag == string_tag ? S::Must_be_taken : S::Cannot_be_taken;
    case DK::Value_closure: case DK::Value_set_of_closures:
      return tag == closure_tag || tag == infix_tag ? S::Can_be_taken : S::Cannot_be_taken;
    case DK::Value_boxed_int: return tag == custom_tag ? S::Must_be_taken : S::Cannot_be_taken;
    case DK::Value_bottom: return S::Cannot_be_taken;
  }
  return S::Cannot_be_taken;
}

const FunctionDeclarations* function_declarations_approx(
    FnRef<bool(variable::t, const F::FunctionDeclaration*)> keep_body, const F::FunctionDeclarations* fd) {
  variable::Map<const FunctionDeclaration*> funs =
      fd->funs.mapi([&](variable::t fun_var, const F::FunctionDeclaration* d) -> const FunctionDeclaration* {
        const FunctionBody* body = nullptr;
        if (keep_body(fun_var, d))
          body = make<FunctionBody>(FunctionBody{d->free_variables, d->free_symbols, d->stub, d->dbg, d->inline_,
                                                 d->specialise, d->is_a_functor, d->body, d->poll});
        return make<FunctionDeclaration>(FunctionDeclaration{d->closure_origin, d->params, body});
      });
  return make<FunctionDeclarations>(
      FunctionDeclarations{fd->is_classic_mode, fd->set_of_closures_id, fd->set_of_closures_origin, funs});
}

const FunctionDeclarations* import_function_declarations_for_pack(
    const FunctionDeclarations* fd, FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_id,
    FnRef<set_of_closures_id::t(set_of_closures_id::t)> import_origin) {
  // (the record's fields right to left: the origin first)
  set_of_closures_id::t origin = import_origin(fd->set_of_closures_origin);
  set_of_closures_id::t id = import_id(fd->set_of_closures_id);
  return make<FunctionDeclarations>(FunctionDeclarations{fd->is_classic_mode, id, origin, fd->funs});
}

const FunctionDeclarations* update_function_declarations(const FunctionDeclarations* fd,
                                                         variable::Map<const FunctionDeclaration*> funs) {
  set_of_closures_id::t id = set_of_closures_id::create(compilation_unit::get_current_exn());
  return make<FunctionDeclarations>(FunctionDeclarations{fd->is_classic_mode, id, fd->set_of_closures_origin, funs});
}

const FunctionDeclarations* clear_function_bodies(const FunctionDeclarations* fd) {
  variable::Map<const FunctionDeclaration*> funs =
      fd->funs.map([](const FunctionDeclaration* d) -> const FunctionDeclaration* {
        if (!d->function_body || d->function_body->stub) return d;
        return make<FunctionDeclaration>(FunctionDeclaration{d->closure_origin, d->params, nullptr});
      });
  FunctionDeclarations r = *fd;
  r.funs = funs;
  return make<FunctionDeclarations>(r);
}

const FunctionDeclaration* update_function_declaration_body(const FunctionDeclaration* d,
                                                            FnRef<F::t(F::t)> f) {
  if (!d->function_body) return d;
  F::t body = f(d->function_body->body);
  variable::Set free_variables = F::free_variables(body);
  symbol::Set free_symbols = F::free_symbols(body);
  FunctionBody nb = *d->function_body;
  nb.free_variables = free_variables;
  nb.free_symbols = free_symbols;
  nb.body = body;
  return make<FunctionDeclaration>(FunctionDeclaration{d->closure_origin, d->params, make<FunctionBody>(nb)});
}

variable::Map<const FunctionDeclarations*> make_closure_map(
    const set_of_closures_id::Map<const FunctionDeclarations*>& input) {
  variable::Map<const FunctionDeclarations*> map;
  input.iter([&](set_of_closures_id::t, const FunctionDeclarations* fd) {
    fd->funs.iter([&](variable::t var, const FunctionDeclaration*) { map = map.add(var, fd); });
  });
  return map;
}

}  // namespace cppcaml::typing::simple_value_approx
