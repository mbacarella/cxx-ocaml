// Port of the reporting half of parsing/location.ml (TYPECHECKER.md stage
// 9): the location printer (Location.Doc.loc), the quoting of the source
// (highlight_quote: the "N | ..." excerpt and its carets), reports
// (errors, warnings, alerts) and the batch-mode report printer, and the
// error_of_exn registry behind Location.report_exception.
//
// A C++ exception stands for an OCaml one: an error_of_exn function
// rethrows the exception_ptr and catches its own types.
#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "cppcaml/typing/format.hpp"
#include "cppcaml/typing/format_doc.hpp"
#include "cppcaml/typing/support.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::location {

namespace fd = format_doc;

// Location.input_name, and the text of !input_lexbuf (Pparse reads the
// whole source into a from_string lexbuf; None when there is none)
extern std::string input_name;
extern std::optional<std::string> input_source;

// Warnings.ghost_loc_in_file / in_file
Location in_file(std::string_view name);
// get_pos_info pos = (pos_fname, pos_lnum, pos_cnum - pos_bol)
std::tuple<std::string_view, long, long> get_pos_info(const Position& pos);
// rewrite_absolute_path: BUILD_PATH_PREFIX_MAP's rewriting (the path
// itself without it).  (rewrite_find_first_existing /
// rewrite_find_all_existing_dirs serve the toplevel only: not ported.)
std::string rewrite_absolute_path(const std::string& path);
std::string absolute_path(const std::string& s);
std::string show_filename(const std::string& file);

// Location.Doc
namespace doc {
void filename(fd::Formatter& ppf, std::string_view file);
void loc(fd::Formatter& ppf, const Location& l);
void locs(fd::Formatter& ppf, const std::vector<Location>& ls);
void quoted_filename(fd::Formatter& ppf, std::string_view file);
}  // namespace doc

// ---- reports ----
struct Msg {  // msg = Format_doc.t loc
  Location loc;
  fd::Doc txt;
};
enum class ReportKind : std::uint8_t {
  Report_error, Report_warning, Report_warning_as_error, Report_alert, Report_alert_as_error
};
struct Report {
  ReportKind kind = ReportKind::Report_error;
  std::string kind_arg;  // Report_warning / _as_error / Report_alert / _as_error
  Msg main;
  std::vector<Msg> sub;
  std::optional<fd::Doc> footnote;
};

// msg ?loc fmt
template <class... Args>
Msg msg(const Location& loc, std::string_view fmt, Args&&... args) {
  return Msg{loc, fd::doc_printf(fmt, std::forward<Args>(args)...)};
}
template <class... Args>
Msg msg_noloc(std::string_view fmt, Args&&... args) {
  return Msg{none(), fd::doc_printf(fmt, std::forward<Args>(args)...)};
}
Msg mknoloc_msg(fd::Doc txt);

Report mkerror(const Location& loc, std::vector<Msg> sub, std::optional<fd::Doc> footnote, fd::Doc txt);
// errorf ?loc ?sub ?footnote fmt
template <class... Args>
Report errorf(const Location& loc, std::string_view fmt, Args&&... args) {
  return mkerror(loc, {}, std::nullopt, fd::doc_printf(fmt, std::forward<Args>(args)...));
}
template <class... Args>
Report errorf_sub(const Location& loc, std::vector<Msg> sub, std::string_view fmt, Args&&... args) {
  return mkerror(loc, std::move(sub), std::nullopt, fd::doc_printf(fmt, std::forward<Args>(args)...));
}
// aligned_error_hint ?loc ?sub fmt ... hint: [main] is the document the
// format builds
Report aligned_error_hint(const Location& loc, std::vector<Msg> sub, const fd::Doc& main,
                          const std::optional<fd::Doc>& hint);
// error ?loc ?sub msg_str
Report error(const Location& loc, std::string_view msg_str, std::vector<Msg> sub = {});
// error_of_printer ?loc ?sub pp x (the printer writes the document)
Report error_of_printer(const Location& loc, const std::function<void(fd::Formatter&)>& pp,
                        std::vector<Msg> sub = {});
Report error_of_printer_file(const std::function<void(fd::Formatter&)>& pp);
// multiple_errors ?loc ?footnote errors
Report multiple_errors(const std::vector<Report>& errors);

// the batch-mode report printer (Location.print_report)
void print_report(format::Formatter& ppf, const Report& report);

// Location.Error of error
struct Error : std::runtime_error {
  Report report;
  explicit Error(Report r) : std::runtime_error("Location.Error"), report(std::move(r)) {}
};
// Warnings.Errors (Already_displayed_error)
struct AlreadyDisplayed : std::runtime_error {
  AlreadyDisplayed() : std::runtime_error("Warnings.Errors") {}
};

// register_error_of_exn f: f rethrows the exception and returns the report
// for the types it knows, None otherwise
using ErrorOfExn = std::function<std::optional<Report>(std::exception_ptr)>;
void register_error_of_exn(ErrorOfExn f);
// report_exception ppf exn: false when no error_of_exn knows it (ocamlc
// then re-raises); a reporter that raises is retried on its exception, as
// ocamlc does (5 times)
bool report_exception(format::Formatter& ppf, std::exception_ptr ep);

// Format.err_formatter: the formatter warnings and errors go to; its text
// goes to stderr at each flush (err_flush)
format::Formatter& err_formatter();
void err_flush();

// ---- warnings and alerts (the default reporters) ----
// report_warning loc w / report_alert loc a: None when inactive
std::optional<Report> report_warning(const Location& loc, const warnings::Warning& w);
std::optional<Report> report_alert(const Location& loc, const warnings::Alert& a);
// print_warning loc ppf w / prerr_warning loc w (on formatter_for_warnings,
// Format.err_formatter)
void print_warning(const Location& loc, format::Formatter& ppf, const warnings::Warning& w);
void prerr_warning(const Location& loc, const warnings::Warning& w);
void print_alert(const Location& loc, format::Formatter& ppf, const warnings::Alert& a);
void prerr_alert(const Location& loc, const warnings::Alert& a);
// alert ?def ?use ~kind loc message / deprecated ?def ?use loc message
void alert(const Location& loc, std::string kind, std::string message, const Location& def = none(),
           const Location& use = none());
void deprecated(const Location& loc, std::string message, const Location& def = none(),
                const Location& use = none());

}  // namespace cppcaml::typing::location
