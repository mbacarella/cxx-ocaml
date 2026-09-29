// Port of typing/errortrace_report.ml (cxx/PORTING.md stage 9): the
// printing of unification, equality, moregen and subtyping traces -- the
// "This expression has type ... but an expression was expected of type"
// bodies and their explanations.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cppcaml/typing/errortrace.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/out_type.hpp"

namespace cppcaml::typing::errortrace_report {

using format_doc::Doc;
using format_doc::Formatter;

// unification ppf env err ?type_expected_explanation txt1 txt2
void unification(Formatter& ppf, env::t env, const errortrace::UnificationError& err, const Doc& txt1,
                 const Doc& txt2, const Doc& type_expected_explanation = Doc{});
void equality(Formatter& ppf, out_type::Mode mode, env::t env, const errortrace::EqualityError& err,
              const Doc& txt1, const Doc& txt2);
void moregen(Formatter& ppf, out_type::Mode mode, env::t env, const errortrace::MoregenError& err, const Doc& txt1,
             const Doc& txt2);
void comparison(Formatter& ppf, out_type::Mode mode, env::t env, const errortrace::ComparisonError& err,
                const Doc& txt1, const Doc& txt2);
// subtype ppf env err txt1
void subtype(Formatter& ppf, env::t env, const errortrace::subtype::Error& err, std::string_view txt1);
// ambiguous_type ppf env tp0 tpl txt1 txt2 txt3
void ambiguous_type(Formatter& ppf, env::t env, std::pair<Path::t, Path::t> tp0,
                    const std::vector<std::pair<Path::t, Path::t>>& tpl, const Doc& txt1, const Doc& txt2,
                    const Doc& txt3);

// Errortrace.print_pos
void print_pos(Formatter& ppf, errortrace::Position pos);
// the first Diff of an error trace (Typecore's type_clash_of_trace)
std::optional<errortrace::Diff<errortrace::ExpandedType>> type_clash_of_trace(const errortrace::ErrorTrace& tr);

}  // namespace cppcaml::typing::errortrace_report
