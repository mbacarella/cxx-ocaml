// Port of middle_end/flambda/inlining_cost.ml: the size of Flambda terms,
// inlining thresholds and the benefit of an inlining or specialisation.
#pragma once

#include <optional>
#include <string>

#include "cppcaml/typing/flambda.hpp"

namespace cppcaml::typing::inlining_cost {

inline constexpr long direct_call_size = 4;
inline constexpr long project_size = 1;
inline constexpr long benefit_factor = 1;
inline constexpr long scale_inline_threshold_by = 8;
inline constexpr long default_toplevel_multiplier = 8;

long prim_size(const clambda::Primitive& prim, Slice<variable::t> args);
// lambda_smaller' lam ~than (nullopt: None)
std::optional<long> lambda_smaller_prime(flambda::t lam, long than);
long lambda_size(flambda::t lam);
bool lambda_smaller(flambda::t lam, long than);

// Threshold.t: Never_inline | Can_inline_if_no_larger_than of int
struct Threshold {
  bool never_inline = true;
  long size = 0;
  static Threshold never() { return {}; }
  static Threshold can_inline_if_no_larger_than(long i) { return {false, i}; }
};
namespace threshold {
Threshold add(Threshold t1, Threshold t2);
Threshold sub(Threshold t1, Threshold t2);
Threshold min(Threshold t1, Threshold t2);
bool equal(Threshold t1, Threshold t2);
}  // namespace threshold

// can_try_inlining lam threshold ~number_of_arguments
//   ~size_from_approximation
Threshold can_try_inlining(flambda::t lam, Threshold inlining_threshold, long number_of_arguments,
                           std::optional<long> size_from_approximation);
bool can_inline(flambda::t lam, Threshold inlining_threshold, long bonus);

struct Benefit {
  long remove_call = 0;
  long remove_alloc = 0;
  long remove_prim = 0;
  long remove_branch = 0;
  long direct_call_of_indirect = 0;
  long requested_inline = 0;
};
namespace benefit {
inline Benefit zero() { return {}; }
Benefit remove_call(Benefit t);
Benefit remove_alloc(Benefit t);
Benefit remove_prim(Benefit t);
Benefit remove_prims(Benefit t, long n);
Benefit remove_branch(Benefit t);
Benefit direct_call_of_indirect(Benefit t);
Benefit requested_inline(Benefit t, flambda::t size_of);
Benefit remove_code(flambda::t lam, Benefit b);
Benefit remove_code_named(flambda::named lam, Benefit b);
Benefit remove_projection(projection::t proj, Benefit b);
Benefit add_code(flambda::t lam, Benefit b);
Benefit add_code_named(flambda::named lam, Benefit b);
Benefit add_projection(projection::t proj, Benefit b);
long evaluate(const Benefit& t, long round);
Benefit plus(const Benefit& t1, const Benefit& t2);
Benefit minus(const Benefit& t1, const Benefit& t2);
Benefit max(long round, const Benefit& t1, const Benefit& t2);
void print(format::Formatter& ppf, const Benefit& b);
void print_table(format::Formatter& ppf, const Benefit& b);
}  // namespace benefit

struct WhetherSufficientBenefit {
  long round;
  Benefit benefit;
  bool toplevel;
  long branch_depth;
  bool lifting;
  long original_size;
  long new_size;
  long evaluated_benefit;
  bool estimate;
};
namespace whether_sufficient_benefit {
WhetherSufficientBenefit create(flambda::t original, bool toplevel, long branch_depth, flambda::t lam,
                                const Benefit& benefit, bool lifting, long round);
WhetherSufficientBenefit create_estimate(long original_size, bool toplevel, long branch_depth, long new_size,
                                         const Benefit& benefit, bool lifting, long round);
bool evaluate(const WhetherSufficientBenefit& t);
std::string to_string(const WhetherSufficientBenefit& t);
void print_description(format::Formatter& ppf, bool subfunctions, const WhetherSufficientBenefit& t);
}  // namespace whether_sufficient_benefit

long maximum_interesting_size_of_function_body(long num_free_variables);

// Format.pp_print_text
void pp_print_text(format::Formatter& ppf, std::string_view s);

}  // namespace cppcaml::typing::inlining_cost
