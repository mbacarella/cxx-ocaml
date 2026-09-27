// See location.hpp.
#include "cppcaml/typing/location.hpp"

#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::location {

std::string input_name = "_none_";
std::optional<std::string> input_source;

Location in_file(std::string_view name) {
  Position loc{zborrow(name), 0, 0, -1};  // Lexing.dummy_pos with pos_fname
  return Location{loc, loc, true};
}

std::tuple<std::string_view, long, long> get_pos_info(const Position& pos) {
  return {pos.pos_fname, pos.pos_lnum, pos.pos_cnum - pos.pos_bol};
}

namespace {

// Filename (Unix): generic_basename / generic_dirname
std::string basename_(const std::string& name) {
  long n = static_cast<long>(name.size()) - 1;
  while (n >= 0 && name[n] == '/') --n;  // find_end
  if (n < 0) return "/";
  long p = n;
  while (p >= 0 && name[p] != '/') --p;  // find_beg
  return name.substr(p + 1, n - p);
}
std::string dirname_(const std::string& name) {
  long n = static_cast<long>(name.size()) - 1;
  while (n >= 0 && name[n] == '/') --n;  // trailing_sep
  if (n < 0) return "/";
  while (n >= 0 && name[n] != '/') --n;  // base
  if (n < 0) return ".";
  while (n >= 0 && name[n] == '/') --n;  // intermediate_sep
  if (n < 0) return "/";
  return name.substr(0, n + 1);
}
std::string concat_(const std::string& dirname, const std::string& filename) {
  if (dirname.empty() || dirname.back() == '/') return dirname + filename;
  return dirname + "/" + filename;
}

}  // namespace

std::string absolute_path(const std::string& s0) {
  std::string s = s0;
  if (s.empty() || s[0] != '/') {
    char buf[PATH_MAX];
    std::string cwd = ::getcwd(buf, sizeof buf) ? buf : "";
    s = concat_(cwd, s);
  }
  // (rewrite_absolute_path: BUILD_PATH_PREFIX_MAP is not ported)
  std::function<std::string(const std::string&)> aux = [&](const std::string& s) -> std::string {
    std::string base = basename_(s);
    std::string dir = dirname_(s);
    if (dir == s) return dir;
    if (base == ".") return aux(dir);
    if (base == "..") return dirname_(aux(dir));
    return concat_(aux(dir), base);
  };
  return aux(s);
}

std::string show_filename(const std::string& file) {
  return clflags::absname ? absolute_path(file) : file;
}

namespace doc {

void filename(fd::Formatter& ppf, std::string_view file) {
  fd::pp_print_string(ppf, show_filename(std::string(file)));
}

void loc(fd::Formatter& ppf, const Location& loc) {
  misc::style::setup();  // Location.setup_tags
  auto file_valid = [](const std::string& f) {
    if (f == "_none_") return true;  // printed anyway, to please editors
    return !(f.empty() || f == "//toplevel//");
  };
  std::string file = loc.loc_start.pos_fname.empty() ? input_name : std::string(loc.loc_start.pos_fname);
  long startline = loc.loc_start.pos_lnum;
  long endline = loc.loc_end.pos_lnum;
  long startchar = loc.loc_start.pos_cnum - loc.loc_start.pos_bol;
  long endchar = loc.loc_end.pos_cnum - loc.loc_end.pos_bol;
  bool first = true;
  auto capitalize = [&](std::string s) {
    if (first) {
      first = false;
      s[0] = static_cast<char>(s[0] - 'a' + 'A');
    }
    return s;
  };
  auto comma = [&] {
    if (!first) fd::fprintf(ppf, ", ");
  };
  fd::fprintf(ppf, "@{<loc>");
  if (file_valid(file)) fd::fprintf(ppf, "%s \"%a\"", capitalize("file"), [&](fd::Formatter& f) { filename(f, file); });
  comma();
  if (startline <= 0) startline = 1;
  if (endline <= 0) endline = startline;
  if (startline == endline)
    fd::fprintf(ppf, "%s %i", capitalize("line"), startline);
  else
    fd::fprintf(ppf, "%s %i-%i", capitalize("lines"), startline, endline);
  if (startchar != -1 && endchar != -1) {
    comma();
    fd::fprintf(ppf, "%s %i-%i", capitalize("characters"), startchar, endchar);
  }
  fd::fprintf(ppf, "@}");
}

void locs(fd::Formatter& ppf, const std::vector<Location>& ls) {
  fd::pp_print_list(ppf, [](fd::Formatter& f, const Location& l) { loc(f, l); }, ls,
                    [](fd::Formatter& f) { fd::fprintf(f, ",@ "); });
}

void quoted_filename(fd::Formatter& ppf, std::string_view file) {
  fd::pp_open_stag(ppf, "inline_code");
  filename(ppf, file);
  fd::pp_close_stag(ppf);
}

}  // namespace doc

// ---- highlight_quote -------------------------------------------------------------

namespace {

// ISet over (position, cnum) bounds; the intervals are maximal, sorted
struct Bound {
  Position pos;
  long x;
};
using ISet = std::vector<std::pair<Bound, Bound>>;

ISet iset_of_intervals(const std::vector<std::pair<Bound, Bound>>& intervals) {
  struct Ev {
    Bound b;
    int kind;  // 0 = `S, 1 = `E
  };
  std::vector<Ev> pos;
  for (auto& [a, b] : intervals)
    if (!(a.x > b.x)) {
      pos.push_back({a, 0});
      pos.push_back({b, 1});
    }
  std::stable_sort(pos.begin(), pos.end(), [](const Ev& p, const Ev& q) {
    if (p.b.x != q.b.x) return p.b.x < q.b.x;
    return p.kind < q.kind;
  });
  ISet acc;
  bool inside = false;
  Bound s{};
  long n = 0;
  for (const Ev& e : pos) {
    if (e.kind == 0) {
      if (!inside) {
        inside = true;
        s = e.b;
        n = 0;
      } else {
        ++n;
      }
    } else {
      if (n == 0) {
        inside = false;
        acc.push_back({s, e.b});
      } else {
        --n;
      }
    }
  }
  return acc;
}
bool iset_mem(const ISet& is, long pos) {
  for (auto& [s, e] : is)
    if (s.x <= pos && pos <= e.x) return true;
  return false;
}
std::optional<Bound> iset_find_bound_in(const ISet& is, long start, long end) {
  for (auto& [a, b] : is) {
    if (start <= a.x && a.x <= end) return a;
    if (start <= b.x && b.x <= end) return b;
  }
  return std::nullopt;
}
bool iset_is_start(const ISet& is, long pos) {
  for (auto& [a, b] : is)
    if (pos == a.x) return true;
  return false;
}
bool iset_is_end(const ISet& is, long pos) {
  for (auto& [a, b] : is)
    if (pos == b.x) return true;
  return false;
}

struct InputLine {
  std::string text;
  long start_pos;
};

// lines_around ~start_pos ~end_pos ~seek ~read_char, over the source text
std::vector<InputLine> lines_around(const Position& start_pos, const Position& end_pos, const std::string& src) {
  std::vector<InputLine> lines;
  long pos = start_pos.pos_bol;
  auto read_char = [&]() -> std::optional<char> {
    if (pos < 0 || pos >= static_cast<long>(src.size())) return std::nullopt;
    return src[pos++];
  };
  long bol = start_pos.pos_bol, cur = start_pos.pos_bol;
  std::string b;
  auto add_line = [&] {
    if (bol < cur) {
      lines.push_back({b, bol});
      b.clear();
      bol = cur;
    }
  };
  while (true) {
    if (bol >= end_pos.pos_cnum) break;
    std::optional<char> c = read_char();
    if (!c) {
      add_line();
      break;
    }
    ++cur;
    if (*c == '\r') continue;
    if (*c == '\n') {
      add_line();
      continue;
    }
    b.push_back(*c);
  }
  return lines;
}

std::vector<InputLine> lines_around_from_current_input(const Position& start_pos, const Position& end_pos) {
  if (!input_source) return {};
  // lines_around_from_lexbuf: lex_abs_pos = 0 (a from_string lexbuf)
  if (start_pos.pos_bol < 0) return {};
  return lines_around(start_pos, end_pos, *input_source);
}

// infer_line_numbers
std::vector<std::pair<std::optional<long>, InputLine>> infer_line_numbers(
    std::vector<std::pair<std::optional<long>, InputLine>> lines) {
  long i = 0;
  std::optional<long> offset;
  bool consistent = true;
  for (auto& [lnum, _] : lines) {
    if (lnum) {
      if (!offset)
        offset = *lnum - i;
      else
        consistent = consistent && *lnum == *offset + i;
    }
    ++i;
  }
  if (offset && consistent) {
    long k = 0;
    for (auto& l : lines) l.first = *offset + k++;
  }
  return lines;
}

void pp_two_columns(fd::Formatter& ppf, std::string_view sep, std::optional<long> max_lines,
                    const std::vector<std::pair<std::string, std::string>>& lines) {
  long left_column_size = 0;
  for (auto& [s, _] : lines) left_column_size = std::max(left_column_size, static_cast<long>(s.size()));
  long lines_nb = static_cast<long>(lines.size());
  long ellipsed_first = -1, ellipsed_last = -1;
  if (max_lines && lines_nb > *max_lines) {
    long printed_lines = *max_lines - 1;
    long lines_before = printed_lines / 2 + printed_lines % 2;
    long lines_after = printed_lines / 2;
    ellipsed_first = lines_before;
    ellipsed_last = lines_nb - lines_after - 1;
  }
  fd::fprintf(ppf, "@[<v>");
  long k = 0;
  for (auto& [l, r] : lines) {
    if (k == ellipsed_first) fd::fprintf(ppf, "...@,");
    if (!(ellipsed_first <= k && k <= ellipsed_last)) fd::fprintf(ppf, "%*s %s %s@,", left_column_size, l, sep, r);
    ++k;
  }
  fd::fprintf(ppf, "@]");
}

void highlight_quote(fd::Formatter& ppf, std::string_view highlight_tag, const std::vector<Location>& locs,
                     long max_lines = 10) {
  std::vector<std::pair<Bound, Bound>> ivs;
  for (const Location& l : locs) {
    const Position &s = l.loc_start, &e = l.loc_end;
    if (s.pos_cnum == -1 || e.pos_cnum == -1) continue;
    ivs.push_back({{s, s.pos_cnum}, {e, e.pos_cnum - 1}});
  }
  ISet iset = iset_of_intervals(ivs);
  if (iset.empty()) return;
  const Position& leftmost = iset.front().first.pos;
  const Position& rightmost = iset.back().second.pos;
  std::vector<std::pair<std::optional<long>, InputLine>> numbered;
  for (InputLine& line : lines_around_from_current_input(leftmost, rightmost)) {
    long end_pos = line.start_pos + static_cast<long>(line.text.size()) - 1;
    std::optional<Bound> b = iset_find_bound_in(iset, line.start_pos, end_pos);
    std::optional<long> nb;
    if (b) nb = b->pos.pos_lnum;
    numbered.push_back({nb, std::move(line)});
  }
  numbered = infer_line_numbers(std::move(numbered));
  struct L {
    std::string text, nb;
    long start;
  };
  std::vector<L> lines;
  for (auto& [n, line] : numbered) lines.push_back({line.text, n ? std::to_string(*n) : std::string(), line.start_pos});
  fd::fprintf(ppf, "@[<v>");
  if (lines.empty() || (lines.size() == 1 && lines[0].text.empty())) {
  } else if (lines.size() == 1) {
    const L& l = lines[0];
    fd::fprintf(ppf, "%s | %s@,", l.nb, l.text);
    fd::fprintf(ppf, "%*s   ", static_cast<long>(l.nb.size()), "");
    for (long i = 0; i <= rightmost.pos_cnum - l.start - 1; ++i) {
      long pos = l.start + i;
      if (iset_is_start(iset, pos)) fd::pp_open_stag(ppf, highlight_tag);  // "@{<%s>"
      if (iset_mem(iset, pos))
        fd::pp_print_char(ppf, '^');
      else if (i < static_cast<long>(l.text.size())) {
        if (l.text[i] == '\t')
          fd::pp_print_char(ppf, '\t');
        else
          fd::pp_print_char(ppf, ' ');
      }
      if (iset_is_end(iset, pos)) fd::pp_close_stag(ppf);
    }
    fd::pp_close_stag(ppf);
    fd::pp_print_cut(ppf);
  } else {
    std::vector<std::pair<std::string, std::string>> cols;
    for (const L& l : lines) {
      std::string t = l.text;
      for (std::size_t i = 0; i < t.size(); ++i)
        if (!iset_mem(iset, l.start + static_cast<long>(i))) t[i] = '.';
      cols.push_back({l.nb, t});
    }
    pp_two_columns(ppf, "|", max_lines, cols);
  }
  fd::fprintf(ppf, "@]");
}

bool is_dummy_loc(const Location& l) { return l.loc_start.pos_cnum == -1 || l.loc_end.pos_cnum == -1; }
bool is_quotable_loc(const Location& l) {
  return !is_dummy_loc(l) && l.loc_start.pos_fname == input_name && l.loc_end.pos_fname == input_name;
}

// Format.fprintf ppf "%a" print_loc loc: the document laid out
void print_loc(format::Formatter& ppf, const Location& l) {
  fd::Formatter f;
  doc::loc(f, l);
  fd::format(ppf, f.doc);
}

// batch_mode_printer.pp_loc
void pp_loc(format::Formatter& ppf, const Report& report, const Location& l) {
  std::string_view tag =
      (report.kind == ReportKind::Report_warning || report.kind == ReportKind::Report_alert) ? "warning" : "error";
  print_loc(ppf, l);
  ppf.print_string(":");
  ppf.print_space();
  // (Fmt.compat highlight) loc: the excerpt under Misc.Error_style.Contextual
  // (the default), nothing under Short
  fd::Formatter f;
  bool contextual = !clflags::error_style || *clflags::error_style == clflags::ErrorStyle::Contextual;
  if (contextual && is_quotable_loc(l)) highlight_quote(f, tag, {l});
  fd::format(ppf, f.doc);
}

void pp_txt(format::Formatter& ppf, const fd::Doc& txt) { fd::format(ppf, txt); }

// Format's @{<tag>...@}: the tag's marks once Misc.Style.setup has run
static void mark_open(format::Formatter& ppf, const std::string& tag) {
  if (!ppf.mark_tags || !misc::style::marks_enabled()) return;
  std::string m = misc::style::mark_open_tag(tag);
  if (!m.empty()) ppf.print_as(0, m);
}
static void mark_close(format::Formatter& ppf, const std::string& tag) {
  if (!ppf.mark_tags || !misc::style::marks_enabled()) return;
  std::string m = misc::style::mark_close_tag(tag);
  if (!m.empty()) ppf.print_as(0, m);
}

void pp_report_kind(format::Formatter& ppf, const Report& r) {
  auto tagged = [&](const char* tag, const char* text) {
    mark_open(ppf, tag);
    ppf.print_string(text);
    mark_close(ppf, tag);
  };
  switch (r.kind) {
    case ReportKind::Report_error: tagged("error", "Error"); break;
    case ReportKind::Report_warning:
      tagged("warning", "Warning");
      format::fprintf(ppf, " %s", r.kind_arg);
      break;
    case ReportKind::Report_warning_as_error:
      tagged("error", "Error");
      format::fprintf(ppf, " (warning %s)", r.kind_arg);
      break;
    case ReportKind::Report_alert:
      tagged("warning", "Alert");
      format::fprintf(ppf, " %s", r.kind_arg);
      break;
    case ReportKind::Report_alert_as_error:
      tagged("error", "Error");
      format::fprintf(ppf, " (alert %s)", r.kind_arg);
      break;
  }
}

void pp_submsg(format::Formatter& ppf, const Report& report, const Msg& m) {
  if (m.loc.loc_ghost) {
    ppf.open_box(0);
    pp_txt(ppf, m.txt);
    ppf.close_box();
  } else {
    pp_loc(ppf, report, m.loc);  // pp_submsg_loc
    ppf.print_string("  ");
    ppf.open_box(0);
    pp_txt(ppf, m.txt);
    ppf.close_box();
  }
}

void pp_submsgs(format::Formatter& ppf, const Report& report) {
  for (const Msg& m : report.sub) {
    ppf.print_cut();
    pp_submsg(ppf, report, m);
  }
}

void pp_footnote(format::Formatter& ppf, const Report& report) {
  if (report.footnote) {
    ppf.print_cut();
    pp_txt(ppf, *report.footnote);
  }
}

}  // namespace

// num_loc_lines: the lines printed by the reports of the current batch;
// a follow-up report is separated from the previous one by a blank line
static long num_loc_lines = 0;

static void print_report_(format::Formatter& ppf, const Report& report);

void print_report(format::Formatter& ppf, const Report& report) {
  misc::style::setup();  // batch_mode_printer.pp: setup_tags
  // separate_new_message
  if (num_loc_lines != 0) {
    ppf.print_newline();
    ++num_loc_lines;
  }
  // print_updating_num_loc_lines: count the newlines the report outputs
  std::size_t before = ppf.contents().size();
  print_report_(ppf, report);
  const std::string& out = ppf.contents();
  for (std::size_t i = before; i < out.size(); ++i)
    if (out[i] == '\n') ++num_loc_lines;
}

static void print_report_(format::Formatter& ppf, const Report& report) {
  if (report.kind == ReportKind::Report_error) {
    // "@[<v>%a%a%a: %a@[%a@]%a%a%a@]@."
    ppf.open_vbox(0);
    ppf.open_tbox();
    pp_loc(ppf, report, report.main.loc);  // pp_main_loc
    pp_report_kind(ppf, report);
    ppf.print_string(": ");
    ppf.set_tab();
    ppf.open_box(0);
    pp_txt(ppf, report.main.txt);
    ppf.close_box();
    pp_submsgs(ppf, report);
    pp_footnote(ppf, report);
    ppf.close_tbox();
    ppf.close_box();
    ppf.print_newline();
  } else {
    // "@[<v>%a@[<b 2>%a: %a@]%a%a@]@."
    ppf.open_vbox(0);
    pp_loc(ppf, report, report.main.loc);
    ppf.open_box(2);
    pp_report_kind(ppf, report);
    ppf.print_string(": ");
    pp_txt(ppf, report.main.txt);
    ppf.close_box();
    pp_submsgs(ppf, report);
    pp_footnote(ppf, report);
    ppf.close_box();
    ppf.print_newline();
  }
}

Msg mknoloc_msg(fd::Doc txt) { return Msg{none(), std::move(txt)}; }

Report mkerror(const Location& loc, std::vector<Msg> sub, std::optional<fd::Doc> footnote, fd::Doc txt) {
  Report r;
  r.kind = ReportKind::Report_error;
  r.main = Msg{loc, std::move(txt)};
  r.sub = std::move(sub);
  r.footnote = std::move(footnote);
  return r;
}

Report aligned_error_hint(const Location& loc, std::vector<Msg> sub, const fd::Doc& main,
                          const std::optional<fd::Doc>& hint) {
  if (!hint) return mkerror(loc, std::move(sub), std::nullopt, main);
  auto [m, h] = misc::align_error_hint(main, *hint);
  sub.insert(sub.begin(), mknoloc_msg(h));
  return mkerror(loc, std::move(sub), std::nullopt, m);
}

Report error(const Location& loc, std::string_view msg_str, std::vector<Msg> sub) {
  fd::Formatter f;
  fd::pp_print_string(f, msg_str);
  return mkerror(loc, std::move(sub), std::nullopt, std::move(f.doc));
}

Report error_of_printer(const Location& loc, const std::function<void(fd::Formatter&)>& pp, std::vector<Msg> sub) {
  return mkerror(loc, std::move(sub), std::nullopt, fd::doc_printf("%a", pp));
}

Report error_of_printer_file(const std::function<void(fd::Formatter&)>& pp) {
  return error_of_printer(in_file(input_name), pp);
}

Report multiple_errors(const std::vector<Report>& errors) {
  if (errors.size() == 1) return errors[0];
  std::vector<Msg> sub;
  for (const Report& e : errors) {
    sub.push_back(e.main);
    for (const Msg& m : e.sub) sub.push_back(m);
  }
  return errorf_sub(none(), std::move(sub), "Multiple errors were encountered@,");
}

// ---- error_of_exn -----------------------------------------------------------------

namespace {
std::vector<ErrorOfExn>& registry() {
  static std::vector<ErrorOfExn> r;
  return r;
}
}  // namespace

void register_error_of_exn(ErrorOfExn f) { registry().insert(registry().begin(), std::move(f)); }

bool report_exception(format::Formatter& ppf, std::exception_ptr ep) {
  for (int n = 5;; --n) {
    try {
      // error_of_exn exn
      try {
        std::rethrow_exception(ep);
      } catch (const AlreadyDisplayed&) {
        return true;
      } catch (const Error& e) {
        print_report(ppf, e.report);
        return true;
      } catch (...) {
      }
      for (const ErrorOfExn& f : registry()) {
        std::optional<Report> r = f(ep);
        if (r) {
          print_report(ppf, *r);
          return true;
        }
      }
      return false;
    } catch (...) {
      if (getenv("CPPCAML_REPORT_DEBUG")) {
        try {
          throw;
        } catch (const std::exception& e) {
          fprintf(stderr, "(error reporter raised: %s)\n", e.what());
        } catch (...) {
        }
      }
      if (n <= 0) throw;
      ep = std::current_exception();
    }
  }
}

format::Formatter& err_formatter() {
  static format::Formatter f;
  f.mark_tags = true;  // (marks only once Misc.Style.setup has run)
  return f;
}

void err_flush() {
  std::string s = err_formatter().take();
  std::fwrite(s.data(), 1, s.size(), stderr);
  std::fflush(stderr);
}

// ---- warnings and alerts ------------------------------------------------------------

namespace {
// default_warning_alert_reporter report mk loc w
std::optional<Report> reporting(const Location& loc, const std::optional<warnings::ReportingInformation>& ri,
                                ReportKind k_ok, ReportKind k_err) {
  if (!ri) return std::nullopt;
  Report r;
  r.kind = ri->is_error ? k_err : k_ok;
  r.kind_arg = ri->id;
  r.main = Msg{loc, ri->message};
  for (auto& [l, m] : ri->sub_locs) r.sub.push_back(Msg{l, m});
  return r;
}
}  // namespace

std::optional<Report> report_warning(const Location& loc, const warnings::Warning& w) {
  return reporting(loc, warnings::report(w), ReportKind::Report_warning, ReportKind::Report_warning_as_error);
}
std::optional<Report> report_alert(const Location& loc, const warnings::Alert& a) {
  return reporting(loc, warnings::report_alert(a), ReportKind::Report_alert, ReportKind::Report_alert_as_error);
}

void print_warning(const Location& loc, format::Formatter& ppf, const warnings::Warning& w) {
  if (std::optional<Report> r = report_warning(loc, w)) print_report(ppf, *r);
}
// formatter_for_warnings = Format.err_formatter (its text reaches stderr
// at the report's flush)
void prerr_warning(const Location& loc, const warnings::Warning& w) {
  print_warning(loc, err_formatter(), w);
  err_flush();
}
void print_alert(const Location& loc, format::Formatter& ppf, const warnings::Alert& a) {
  if (std::optional<Report> r = report_alert(loc, a)) print_report(ppf, *r);
}
void prerr_alert(const Location& loc, const warnings::Alert& a) {
  print_alert(loc, err_formatter(), a);
  err_flush();
}
void alert(const Location& loc, std::string kind, std::string message, const Location& def, const Location& use) {
  prerr_alert(loc, warnings::Alert{std::move(kind), std::move(message), def, use});
}
void deprecated(const Location& loc, std::string message, const Location& def, const Location& use) {
  alert(loc, "deprecated", std::move(message), def, use);
}

}  // namespace cppcaml::typing::location
