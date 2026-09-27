// Port of the pretty-printing engine of stdlib/format.ml (TYPECHECKER.md
// stage 10): boxes (h, v, hv, hov, b), break hints, the margin and max
// indent, and the scan/queue algorithm that lays them out -- so printers
// ported from the compiler (Printlambda, Printtyp) produce ocamlc's exact
// text.  A Formatter accumulates its output into a string (the engine's
// out_string / out_newline / out_spaces / out_indent all append to it).
//
// Not ported: tabulation boxes, semantic tags, and the ellipsis/max_boxes
// setters (max_boxes stays max_int, as ocamlc never changes it).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cppcaml::typing::format {

enum class BoxType { Pp_hbox, Pp_vbox, Pp_hvbox, Pp_hovbox, Pp_box, Pp_fits };

// pp_infinity: large value for default tokens size
inline constexpr long pp_infinity = 1000000010;

class Formatter {
 public:
  // pp_make_formatter: margin 78, min_space_left 10, max_indent 68, the
  // system box already open.
  Formatter();
  Formatter(const Formatter&) = delete;
  Formatter& operator=(const Formatter&) = delete;

  // the text output so far (flushed tokens only)
  const std::string& contents() const { return out_; }
  std::string take() { return std::move(out_); }

  // ---- the pp_* functions of format.ml ----
  void open_box_gen(long indent, BoxType ty);
  void open_box(long indent) { open_box_gen(indent, BoxType::Pp_box); }
  void open_hbox() { open_box_gen(0, BoxType::Pp_hbox); }
  void open_vbox(long indent) { open_box_gen(indent, BoxType::Pp_vbox); }
  void open_hvbox(long indent) { open_box_gen(indent, BoxType::Pp_hvbox); }
  void open_hovbox(long indent) { open_box_gen(indent, BoxType::Pp_hovbox); }
  void close_box();
  void print_as_size(long size, std::string_view s);
  void print_as(long isize, std::string_view s) { print_as_size(isize, s); }
  void print_string(std::string_view s);
  void print_char(char c);
  void print_int(long i);
  void print_custom_break(std::string_view fits_before, long fits_width, std::string_view fits_after,
                          std::string_view breaks_before, long breaks_offset, std::string_view breaks_after);
  void print_break(long width, long offset);
  void print_space() { print_break(1, 0); }
  void print_cut() { print_break(0, 0); }
  void force_newline();
  void print_if_newline();
  void print_newline();
  void print_flush();
  void set_margin(long n);
  long get_margin() const { return margin_; }
  void set_max_indent(long n);
  long get_max_indent() const { return max_indent_; }
  void set_min_space_left(long n);

 private:
  enum class Tok : std::uint8_t { Pp_text, Pp_break, Pp_begin, Pp_end, Pp_newline, Pp_if_newline };
  struct QueueElem {
    long size;  // Size.t: known when >= 0
    Tok token;
    long length;
    std::string text;  // Pp_text
    // Pp_break {fits = (fb, fw, fa); breaks = (bb, bo, ba)}
    std::string fb, fa, bb, ba;
    long fw = 0, bo = 0;
    long indent = 0;  // Pp_begin
    BoxType box = BoxType::Pp_box;
  };
  struct ScanElem {
    long left_total;
    QueueElem* queue_elem;
  };
  struct FormatElem {
    BoxType box_type;
    long width;
  };

  QueueElem* new_elem(long size, Tok token, long length);
  long string_width(std::string_view s) const;
  void output_string(std::string_view s) { out_.append(s); }
  void output_newline() { out_.push_back('\n'); }
  void output_spaces(long n);
  void output_indent(long n) { output_spaces(n); }
  void format_pp_text(long size, std::string_view text);
  void format_string(std::string_view s);
  void break_new_line(std::string_view before, long offset, std::string_view after, long width);
  void break_line(long width) { break_new_line("", 0, "", width); }
  void break_same_line(std::string_view before, long width, std::string_view after);
  void force_break_line();
  void skip_token();
  void format_pp_token(long size, QueueElem* e);
  void advance_left();
  void enqueue(QueueElem* e);
  void enqueue_advance(QueueElem* e);
  void enqueue_string_as(long size, std::string_view s);
  void enqueue_string(std::string_view s);
  void initialize_scan_stack();
  void set_size(bool break_hint);
  void enqueue_break(QueueElem* e);
  void scan_push(bool break_hint, QueueElem* e);
  void open_sys_box() { open_box_gen(0, BoxType::Pp_hovbox); }
  void clear_queue();
  void rinit();
  void flush_queue(bool end_with_newline);

  std::vector<std::unique_ptr<QueueElem>> pool_;  // owns every queue element
  std::vector<ScanElem> scan_stack_;
  std::vector<FormatElem> format_stack_;
  long margin_;
  long min_space_left_;
  long max_indent_;
  long space_left_;
  long current_indent_;
  bool is_new_line_;
  long left_total_;
  long right_total_;
  long curr_depth_;
  long max_boxes_;
  std::string ellipsis_;
  std::deque<QueueElem*> queue_;
  std::string out_;
};

// ---- fprintf ------------------------------------------------------------------
// Interprets an OCaml format string on a formatter: the @-formatting items
// (@[<hov n>, @], "@ ", @,, @;<n m>, @., @\n, @?, @@, @%%) and the conversions
// %s %d %i %S %c %C %a %t %%, with the l / L / n size prefixes of %d / %i.
// A %a argument is one callable `void(Formatter&)` (the printer applied to
// its value: use `pr(printer, value)`), as is a %t argument.
struct Arg {
  enum class Kind : std::uint8_t { Str, Int, Char, Fn };
  Kind kind = Kind::Str;
  std::string_view s{};
  long long i = 0;
  char c = 0;
  std::function<void(Formatter&)> fn{};
};

inline Arg to_arg(std::string_view s) {
  Arg a;
  a.s = s;
  return a;
}
inline Arg to_arg(const std::string& s) { return to_arg(std::string_view(s)); }
inline Arg to_arg(const char* s) { return to_arg(std::string_view(s)); }
inline Arg to_arg(char c) {
  Arg a;
  a.kind = Arg::Kind::Char;
  a.c = c;
  return a;
}
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char>, int> = 0>
Arg to_arg(T n) {
  Arg a;
  a.kind = Arg::Kind::Int;
  a.i = static_cast<long long>(n);
  return a;
}
template <class F, std::enable_if_t<std::is_invocable_v<F&, Formatter&>, int> = 0>
Arg to_arg(F&& f) {
  Arg a;
  a.kind = Arg::Kind::Fn;
  a.fn = std::forward<F>(f);
  return a;
}

void vfprintf(Formatter& ppf, std::string_view fmt, const std::vector<Arg>& args);

template <class... Args>
void fprintf(Formatter& ppf, std::string_view fmt, Args&&... args) {
  // (OCaml evaluates the arguments before printing; so does C++ here)
  std::vector<Arg> v;
  v.reserve(sizeof...(Args));
  (v.push_back(to_arg(std::forward<Args>(args))), ...);
  vfprintf(ppf, fmt, v);
}

// `%a` printer + value as one argument
template <class P, class V>
auto pr(P printer, V value) {
  return [printer, value](Formatter& ppf) { printer(ppf, value); };
}

// CamlinternalFormat.open_box_of_string: "hov 2" -> (2, Pp_hovbox)
std::pair<long, BoxType> open_box_of_string(std::string_view str);

// String.escaped / Char.escaped (the %S and %C conversions)
std::string string_escaped(std::string_view s);
std::string char_escaped(char c);

// utf_8_scalar_width: the formatter's out_width
long utf_8_scalar_width(std::string_view s);

}  // namespace cppcaml::typing::format
