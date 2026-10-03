// Port of middle_end/flambda/inlining_cost.ml (see inlining_cost.hpp).
#include "cppcaml/typing/inlining_cost.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inlining_cost {

using namespace flambda;
using format::Formatter;
using format::fprintf;

// Simple approximation of the space cost of a primitive.
long prim_size(const clambda::Primitive& prim, Slice<variable::t> args) {
  using K = clambda::Primitive::K;
  long n = static_cast<long>(args.size());
  switch (prim.kind) {
    case K::Pmakeblock: return 5 + n;
    case K::Pmakelazyblock: return 6;
    case K::Pfield: return 1;
    case K::Psetfield:
      if (prim.init == lambda::InitializationOrAssignment::Root_initialization) return 1;  // no write barrier
      return prim.ptr == lambda::ImmediateOrPointer::Pointer ? 4 : 1;
    case K::Pfloatfield: return 1;
    case K::Psetfloatfield: return 1;
    case K::Pduprecord: return 10 + n;
    case K::Pccall: return (prim.ccall->prim_alloc ? 10 : 4) + n;
    case K::Praise: return 4;
    case K::Pstringlength: return 5;
    case K::Pbyteslength: return 5;
    case K::Pstringrefs: return 6;
    case K::Pbytesrefs: case K::Pbytessets: return 6;
    case K::Pmakearray: return 5 + n;
    case K::Parraylength: return prim.array == lambda::ArrayKind::Pgenarray ? 6 : 2;
    case K::Parrayrefu: return prim.array == lambda::ArrayKind::Pgenarray ? 12 : 2;
    case K::Parraysetu: return prim.array == lambda::ArrayKind::Pgenarray ? 16 : 4;
    case K::Parrayrefs: return prim.array == lambda::ArrayKind::Pgenarray ? 18 : 8;
    case K::Parraysets: return prim.array == lambda::ArrayKind::Pgenarray ? 22 : 10;
    case K::Pbigarrayref: case K::Pbigarrayset: return 4 + prim.n * 6;
    case K::Psequand: case K::Psequor:
      misc::fatal_error(
          "Psequand and Psequor are not allowed in Prim expressions; translate out instead (cf. "
          "closure_conversion.ml)");
    default: return 2;  // arithmetic and comparisons
  }
}

namespace {
struct Sizer {
  long size = 0;
  long threshold;
  // false: Exit raised
  bool lambda_size(t lam) {
    for (;;) {
      if (size > threshold) return false;
      switch (lam->kind) {
        case EK::Var: return true;
        case EK::Apply: size += static_cast<const Apply*>(lam)->call_kind.direct ? direct_call_size : 6; return true;
        case EK::Assign: ++size; return true;
        case EK::Send: size += 8; return true;
        case EK::Proved_unreachable: return true;
        case EK::Let: {
          auto* l = static_cast<const Let*>(lam);
          if (!lambda_named_size(l->defining_expr)) return false;
          lam = l->body;
          continue;
        }
        case EK::Let_mutable: lam = static_cast<const Let_mutable*>(lam)->body; continue;
        case EK::Switch: {
          auto* s = static_cast<const Switch*>(lam);
          auto cost = [](std::size_t n) -> long { return n <= 1 ? 0 : 3 + static_cast<long>(n); };
          size += cost(s->consts.size()) + cost(s->blocks.size());
          for (const SwitchCase& c : s->consts)
            if (!lambda_size(c.action)) return false;
          for (const SwitchCase& c : s->blocks)
            if (!lambda_size(c.action)) return false;
          if (!s->failaction) return true;
          lam = s->failaction;
          continue;
        }
        case EK::String_switch: {
          auto* s = static_cast<const String_switch*>(lam);
          for (const StringCase& c : s->cases) {
            size += 2;
            if (!lambda_size(c.action)) return false;
          }
          if (!s->def) return true;
          lam = s->def;
          continue;
        }
        case EK::Static_raise: return true;
        case EK::Static_catch: {
          auto* c = static_cast<const Static_catch*>(lam);
          ++size;
          if (!lambda_size(c->body)) return false;
          lam = c->handler;
          continue;
        }
        case EK::Try_with: {
          auto* tw = static_cast<const Try_with*>(lam);
          size += 8;
          if (!lambda_size(tw->body)) return false;
          lam = tw->handler;
          continue;
        }
        case EK::If_then_else: {
          auto* i = static_cast<const If_then_else*>(lam);
          size += 2;
          if (!lambda_size(i->ifso)) return false;
          lam = i->ifnot;
          continue;
        }
        case EK::While: {
          auto* w = static_cast<const While*>(lam);
          size += 2;
          if (!lambda_size(w->cond)) return false;
          lam = w->body;
          continue;
        }
        case EK::For: size += 4; lam = static_cast<const For*>(lam)->body; continue;
      }
    }
  }
  bool lambda_named_size(named n) {
    if (size > threshold) return false;
    switch (n->kind) {
      case NK::Symbol: case NK::Read_mutable: return true;
      case NK::Const: case NK::Allocated_const: ++size; return true;
      case NK::Read_symbol_field: ++size; return true;
      case NK::Set_of_closures: {
        bool ok = true;
        static_cast<const NSet_of_closures*>(n)->set->function_decls->funs.iter(
            [&](variable::t, const FunctionDeclaration* d) {
              if (ok) ok = lambda_size(d->body);
            });
        return ok;
      }
      case NK::Project_closure: case NK::Project_var: size += project_size; return true;
      case NK::Move_within_set_of_closures: ++size; return true;
      case NK::Prim: {
        auto* p = static_cast<const NPrim*>(n);
        size += prim_size(*p->prim, p->args);
        return true;
      }
      case NK::Expr: return lambda_size(static_cast<const NExpr*>(n)->expr);
    }
    return true;
  }
};
}  // namespace

std::optional<long> lambda_smaller_prime(t lam, long than) {
  Sizer s{0, than};
  if (!s.lambda_size(lam)) return std::nullopt;
  if (s.size <= than) return s.size;
  return std::nullopt;
}

long lambda_size(t lam) {
  std::optional<long> s = lambda_smaller_prime(lam, std::numeric_limits<long>::max() >> 1);
  // There is no way that an expression of size max_int could fit in memory.
  if (!s) misc::fatal_error("Inlining_cost.lambda_size");
  return *s;
}

bool lambda_smaller(t lam, long than) { return lambda_smaller_prime(lam, than).has_value(); }

namespace threshold {
Threshold add(Threshold t1, Threshold t2) {
  if (t1.never_inline) return t2;
  if (t2.never_inline) return t1;
  return Threshold::can_inline_if_no_larger_than(t1.size + t2.size);
}
Threshold sub(Threshold t1, Threshold t2) {
  if (t1.never_inline) return Threshold::never();
  if (t2.never_inline) return t1;
  if (t1.size > t2.size) return Threshold::can_inline_if_no_larger_than(t1.size - t2.size);
  return Threshold::never();
}
Threshold min(Threshold t1, Threshold t2) {
  if (t1.never_inline || t2.never_inline) return Threshold::never();
  return Threshold::can_inline_if_no_larger_than(std::min(t1.size, t2.size));
}
bool equal(Threshold t1, Threshold t2) {
  if (t1.never_inline || t2.never_inline) return t1.never_inline && t2.never_inline;
  return t1.size == t2.size;
}
}  // namespace threshold

Threshold can_try_inlining(t lam, Threshold inlining_threshold, long number_of_arguments,
                           std::optional<long> size_from_approximation) {
  if (inlining_threshold.never_inline) return Threshold::never();
  // removing a call will reduce the size by at least the number of
  // arguments
  long bonus = number_of_arguments;
  long than = inlining_threshold.size + bonus;
  std::optional<long> size;
  if (size_from_approximation) {
    if (*size_from_approximation <= than) size = size_from_approximation;
  } else {
    size = lambda_smaller_prime(lam, than);
  }
  if (!size) return Threshold::never();
  return Threshold::can_inline_if_no_larger_than(inlining_threshold.size - *size + bonus);
}

bool can_inline(t lam, Threshold inlining_threshold, long bonus) {
  if (inlining_threshold.never_inline) return false;
  return lambda_smaller(lam, inlining_threshold.size + bonus);
}

namespace {
long cost(const clflags::IntArg& flag, long round) { return arg_helper::get(round, flag); }
}  // namespace

namespace benefit {
Benefit remove_call(Benefit t) {
  ++t.remove_call;
  return t;
}
Benefit remove_alloc(Benefit t) {
  ++t.remove_alloc;
  return t;
}
Benefit remove_prim(Benefit t) {
  ++t.remove_prim;
  return t;
}
Benefit remove_prims(Benefit t, long n) {
  t.remove_prim += n;
  return t;
}
Benefit remove_branch(Benefit t) {
  ++t.remove_branch;
  return t;
}
Benefit direct_call_of_indirect(Benefit t) {
  ++t.direct_call_of_indirect;
  return t;
}
Benefit requested_inline(Benefit t, flambda::t size_of) {
  t.requested_inline += lambda_size(size_of);
  return t;
}

namespace {
void remove_code_helper(Benefit& b, flambda::t flam) {
  switch (flam->kind) {
    case EK::Assign: b = remove_prim(b); break;
    case EK::Switch: case EK::String_switch: case EK::Static_raise: case EK::Try_with: case EK::If_then_else:
    case EK::While: case EK::For: b = remove_branch(b); break;
    case EK::Apply: case EK::Send: b = remove_call(b); break;
    case EK::Let: case EK::Let_mutable: case EK::Proved_unreachable: case EK::Var: case EK::Static_catch: break;
  }
}
void remove_code_helper_named(Benefit& b, named n) {
  switch (n->kind) {
    case NK::Set_of_closures: b = remove_alloc(b); break;
    case NK::Prim: {
      using K = clambda::Primitive::K;
      K k = static_cast<const NPrim*>(n)->prim->kind;
      // CR-soon pchambart: should we consider that boxed integer and float
      // operations are allocations ?
      if (k == K::Pmakearray || k == K::Pmakeblock || k == K::Pmakelazyblock || k == K::Pduprecord)
        b = remove_alloc(b);
      else b = remove_prim(b);
      break;
    }
    case NK::Project_closure: case NK::Project_var: case NK::Move_within_set_of_closures:
    case NK::Read_symbol_field: b = remove_prim(b); break;
    case NK::Symbol: case NK::Read_mutable: case NK::Allocated_const: case NK::Const: case NK::Expr: break;
  }
}
}  // namespace

Benefit remove_code(flambda::t lam, Benefit b) {
  flambda_iterators::iter_toplevel([&](flambda::t e) { remove_code_helper(b, e); },
                                   [&](named n) { remove_code_helper_named(b, n); }, lam);
  return b;
}
Benefit remove_code_named(named lam, Benefit b) {
  flambda_iterators::iter_named_toplevel([&](flambda::t e) { remove_code_helper(b, e); },
                                         [&](named n) { remove_code_helper_named(b, n); }, lam);
  return b;
}
// They are all primitives for the moment.  The [Projection.t] argument is
// here for future expansion.
Benefit remove_projection(projection::t, Benefit b) { return remove_prim(b); }

void print(Formatter& ppf, const Benefit& b) {
  fprintf(ppf, "@[remove_call: %i@ remove_alloc: %i@ remove_prim: %i@ remove_branch: %i@ direct: %i@ requested: %i@]",
          b.remove_call, b.remove_alloc, b.remove_prim, b.remove_branch, b.direct_call_of_indirect,
          b.requested_inline);
}

long evaluate(const Benefit& t, long round) {
  return benefit_factor * (t.remove_call * cost(clflags::inline_call_cost, round) +
                           t.remove_alloc * cost(clflags::inline_alloc_cost, round) +
                           t.remove_prim * cost(clflags::inline_prim_cost, round) +
                           t.remove_branch * cost(clflags::inline_branch_cost, round) +
                           t.direct_call_of_indirect * cost(clflags::inline_indirect_cost, round)) +
         t.requested_inline;
}

Benefit plus(const Benefit& t1, const Benefit& t2) {
  return {t1.remove_call + t2.remove_call,       t1.remove_alloc + t2.remove_alloc,
          t1.remove_prim + t2.remove_prim,       t1.remove_branch + t2.remove_branch,
          t1.direct_call_of_indirect + t2.direct_call_of_indirect, t1.requested_inline + t2.requested_inline};
}
Benefit minus(const Benefit& t1, const Benefit& t2) {
  return {t1.remove_call - t2.remove_call,       t1.remove_alloc - t2.remove_alloc,
          t1.remove_prim - t2.remove_prim,       t1.remove_branch - t2.remove_branch,
          t1.direct_call_of_indirect - t2.direct_call_of_indirect, t1.requested_inline - t2.requested_inline};
}
Benefit max(long round, const Benefit& t1, const Benefit& t2) {
  long c1 = evaluate(t1, round);
  long c2 = evaluate(t2, round);
  return c1 > c2 ? t1 : t2;
}
Benefit add_code(flambda::t lam, Benefit b) { return minus(b, remove_code(lam, zero())); }
Benefit add_code_named(named lam, Benefit b) { return minus(b, remove_code_named(lam, zero())); }
Benefit add_projection(projection::t proj, Benefit b) { return minus(b, remove_projection(proj, zero())); }

// Print out a benefit as a table
namespace {
struct Column {
  const char* header;
  long Benefit::*accessor;
};
const Column benefit_table[] = {{"Calls", &Benefit::remove_call},
                                {"Allocs", &Benefit::remove_alloc},
                                {"Prims", &Benefit::remove_prim},
                                {"Branches", &Benefit::remove_branch},
                                {"Indirect calls", &Benefit::direct_call_of_indirect}};
}  // namespace

void print_table(Formatter& ppf, const Benefit& b) {
  std::string table_line = "|-", table_headers = "| ";
  bool first = true;
  for (const Column& c : benefit_table) {
    if (!first) {
      table_line += "-+-";
      table_headers += " | ";
    }
    first = false;
    table_line += std::string(std::string_view(c.header).size(), '-');
    table_headers += c.header;
  }
  table_line += "-|";
  table_headers += " |";
  auto values = [&](Formatter& f) {
    for (const Column& c : benefit_table) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%*ld", static_cast<int>(std::string_view(c.header).size()), b.*c.accessor);
      fprintf(f, "| %s ", std::string(buf));
    }
    fprintf(f, "|");
  };
  fprintf(ppf, "@[<v>@[<h>%s@]@;@[<h>%s@]@;@[<h>%s@]@;@[<h>%a@]@;@[<h>%s@]@]", table_line, table_headers, table_line,
          values, table_line);
}
}  // namespace benefit

namespace whether_sufficient_benefit {
WhetherSufficientBenefit create(flambda::t original, bool toplevel, long branch_depth, flambda::t lam,
                                const Benefit& b, bool lifting, long round) {
  long evaluated_benefit = benefit::evaluate(b, round);
  // (the record's fields right to left: new_size before original_size)
  long new_size = lambda_size(lam);
  long original_size = lambda_size(original);
  return {round, b, toplevel, branch_depth, lifting, original_size, new_size, evaluated_benefit, false};
}

WhetherSufficientBenefit create_estimate(long original_size, bool toplevel, long branch_depth, long new_size,
                                         const Benefit& b, bool lifting, long round) {
  long evaluated_benefit = benefit::evaluate(b, round);
  return {round, b, toplevel, branch_depth, lifting, original_size, new_size, evaluated_benefit, true};
}

namespace {
double estimated_benefit(const WhetherSufficientBenefit& t) {
  if (t.toplevel && t.lifting && t.branch_depth == 0) {
    long lifting_benefit = arg_helper::get(t.round, clflags::inline_lifting_benefit);
    return static_cast<double>(t.evaluated_benefit + lifting_benefit);
  }
  // The estimated benefit is the evaluated benefit times an estimation of
  // the probability that the branch does actually matter for performance
  // (i.e. is hot).  (inlining_cost.ml)
  double factor = arg_helper::get(t.round, clflags::inline_branch_factor);
  double inline_branch_factor;
  if (std::isnan(factor)) inline_branch_factor = clflags::default_inline_branch_factor;
  else if (factor < 0.) inline_branch_factor = 0.;
  else inline_branch_factor = factor;
  double branch_taken_estimated_probability = 1. / (1. + inline_branch_factor);
  double call_estimated_probability = std::pow(branch_taken_estimated_probability, static_cast<double>(t.branch_depth));
  return static_cast<double>(t.evaluated_benefit) * call_estimated_probability;
}
// Float.compare
int float_compare(double a, double b) {
  if (a < b) return -1;
  if (a > b) return 1;
  if (a == b) return 0;
  if (a != a) return b != b ? 0 : -1;
  return 1;
}
}  // namespace

bool evaluate(const WhetherSufficientBenefit& t) {
  return float_compare(static_cast<double>(t.new_size) - estimated_benefit(t), static_cast<double>(t.original_size)) <=
         0;
}

std::string to_string(const WhetherSufficientBenefit& t) {
  bool lifting = t.toplevel && t.lifting && t.branch_depth == 0;
  long evaluated_benefit = t.evaluated_benefit;
  if (lifting) evaluated_benefit += arg_helper::get(t.round, clflags::inline_lifting_benefit);
  const char* estimate = t.estimate ? "<" : "=";
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "{benefit%s{call=%ld,alloc=%ld,prim=%ld,branch=%ld,indirect=%ld,req=%ld,lifting=%s}, "
                "orig_size=%ld,new_size=%ld,eval_size=%ld,eval_benefit%s%ld,branch_depth=%ld}=%s",
                estimate, t.benefit.remove_call, t.benefit.remove_alloc, t.benefit.remove_prim,
                t.benefit.remove_branch, t.benefit.direct_call_of_indirect, t.benefit.requested_inline,
                lifting ? "true" : "false", t.original_size, t.new_size, t.original_size - t.new_size, estimate,
                evaluated_benefit, t.branch_depth, evaluate(t) ? "yes" : "no");
  return buf;
}

void print_description(Formatter& ppf, bool subfunctions, const WhetherSufficientBenefit& t) {
  auto pr_intro = [&](Formatter& f) {
    const char* estimate = t.estimate ? " at most" : "";
    pp_print_text(f, "Specialisation of the function body");
    if (subfunctions) pp_print_text(f, ", including speculative inlining of other functions,");
    pp_print_text(f, " removed");
    pp_print_text(f, estimate);
    pp_print_text(f, " the following operations:");
  };
  bool lifting = t.toplevel && t.lifting && t.branch_depth == 0;
  long requested = t.benefit.requested_inline;
  auto pr_requested = [&](Formatter& f) {
    if (requested > 0) {
      f.open_box(0);
      pp_print_text(f, "and inlined user-annotated functions worth ");
      fprintf(f, "%d.", requested);
      f.close_box();
      f.print_cut();
      f.print_cut();
    }
  };
  auto pr_lifting = [&](Formatter& f) {
    if (lifting) {
      f.open_box(0);
      pp_print_text(f, "Inlining the function would also lift some definitions to toplevel.");
      f.close_box();
      f.print_cut();
      f.print_cut();
    }
  };
  long total_benefit = t.evaluated_benefit;
  if (lifting) total_benefit += arg_helper::get(t.round, clflags::inline_lifting_benefit);
  double expected_benefit = estimated_benefit(t);
  long size_change = t.new_size - t.original_size;
  const char* result = evaluate(t) ? "less" : "greater";
  auto pr_conclusion = [&](Formatter& f) {
    pp_print_text(f, "This gives a total benefit of ");
    f.print_int(total_benefit);
    pp_print_text(f, ".  At a branch depth of ");
    f.print_int(t.branch_depth);
    pp_print_text(f, " this produces an expected benefit of ");
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", expected_benefit);
    f.print_string(buf);
    pp_print_text(f, ".  The new code has size ");
    f.print_int(t.new_size);
    pp_print_text(f, ", giving a change in code size of ");
    f.print_int(size_change);
    pp_print_text(f, ".  The change in code size is ");
    pp_print_text(f, result);
    pp_print_text(f, " than the expected benefit.");
  };
  fprintf(ppf, "%t@,@[<v>@[<v 2>@;%a@]@;@;%t%t@]%t", pr_intro,
          [&](Formatter& f) { benefit::print_table(f, t.benefit); }, pr_requested, pr_lifting, pr_conclusion);
}
}  // namespace whether_sufficient_benefit

long maximum_interesting_size_of_function_body(long num_free_variables) {
  // (lazy: computed on first use, with the rounds then in force)
  static long base = -1, multiplier = -1;
  if (base < 0) {
    long max_cost = 0;
    for (long round = 0; round <= clflags::rounds() - 1; ++round)
      max_cost = std::max(max_cost, direct_call_size + cost(clflags::inline_call_cost, round) * benefit_factor);
    base = max_cost;
  }
  if (multiplier < 0) {
    long max_cost = 0;
    for (long round = 0; round <= clflags::rounds() - 1; ++round)
      max_cost = std::max(max_cost, cost(clflags::inline_prim_cost, round) * benefit_factor);
    multiplier = max_cost;
  }
  return base + num_free_variables * multiplier;
}

void pp_print_text(Formatter& ppf, std::string_view s) {
  std::size_t len = s.size(), left = 0, right = 0;
  auto flush = [&]() {
    ppf.print_string(s.substr(left, right - left));
    ++right;
    left = right;
  };
  while (right != len) {
    char c = s[right];
    if (c == '\n') {
      flush();
      ppf.force_newline();
    } else if (c == ' ') {
      flush();
      ppf.print_space();
    } else {
      ++right;
    }
  }
  if (left != len) flush();
}

}  // namespace cppcaml::typing::inlining_cost
