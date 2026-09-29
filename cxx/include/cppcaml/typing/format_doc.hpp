// Port of utils/format_doc.ml (cxx/PORTING.md stage 9): documents -- lists
// of formatting instructions -- built by printers and by a format-string
// interpreter, and laid out later on a Format formatter (format.hpp).  The
// compiler's message printers (Oprint, Out_type, Printtyp, the error
// reports) write Format_doc; a document is printed with `format` (Doc.format)
// or `compat` (Format_doc.compat).
//
// Semantic tags are kept in the document; laid out on a Format formatter
// they print nothing (ocamlc's formatters mark tags, and with colours off
// every style's text_open / text_close is "").  Tabulation boxes are kept
// but not laid out (the ported printers do not use them).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cppcaml/typing/format.hpp"

namespace cppcaml::typing::format_doc {

// ---- Doc ------------------------------------------------------------------------

enum class BoxType : std::uint8_t { H, V, HV, HoV, B };

struct Element {
  enum class K : std::uint8_t {
    Text, With_size, Open_box, Close_box, Open_tag, Close_tag, Open_tbox, Tab_break, Set_tab, Close_tbox,
    Simple_break, Break, Flush, Newline, If_newline, Deprecated
  };
  K k;
  std::string text;  // Text; Open_tag (the String_tag); Break: fits before
  long a = 0;        // With_size size; Open_box indent; Tab_break width; Simple_break spaces; Break fits width
  long b = 0;        // Tab_break offset; Simple_break indent; Break breaks offset
  BoxType box = BoxType::B;  // Open_box
  bool newline = false;      // Flush
  std::string fits_after, breaks_before, breaks_after;  // Break
  std::function<void(format::Formatter&)> deprecated;   // Deprecated
};

// Doc.t: the elements in printing order
struct Doc {
  std::vector<Element> els;
  bool empty() const { return els.empty(); }
};

inline Doc empty() { return {}; }
// Doc.format ppf doc: lay the document out on a Format formatter
void format(format::Formatter& ppf, const Doc& doc);
// Doc.to_string: the texts only
std::string to_string(const Doc& doc);
Doc append(const Doc& left, const Doc& right);

// ---- the formatter (a doc ref) ------------------------------------------------------

class Formatter {
 public:
  Doc doc;
  void add(Element e) { doc.els.push_back(std::move(e)); }
};

// ---- the primitive printers ----
void pp_print_string(Formatter& ppf, std::string_view s);
void pp_print_as(Formatter& ppf, long size, std::string_view s);
void pp_print_text(Formatter& ppf, std::string_view s);
void pp_print_char(Formatter& ppf, char c);
void pp_print_int(Formatter& ppf, long n);
void pp_print_bool(Formatter& ppf, bool b);
void pp_open_box_gen(Formatter& ppf, long indent, format::BoxType bty);
void pp_open_box(Formatter& ppf, long indent);
void pp_open_hbox(Formatter& ppf);
void pp_open_vbox(Formatter& ppf, long indent);
void pp_open_hvbox(Formatter& ppf, long indent);
void pp_open_hovbox(Formatter& ppf, long indent);
void pp_close_box(Formatter& ppf);
void pp_open_stag(Formatter& ppf, std::string_view string_tag);
void pp_close_stag(Formatter& ppf);
void pp_print_break(Formatter& ppf, long spaces, long indent);
void pp_print_custom_break(Formatter& ppf, std::string_view fits_before, long fits_width,
                           std::string_view fits_after, std::string_view breaks_before, long breaks_offset,
                           std::string_view breaks_after);
void pp_print_space(Formatter& ppf);
void pp_print_cut(Formatter& ppf);
void pp_print_flush(Formatter& ppf);
void pp_force_newline(Formatter& ppf);
void pp_print_newline(Formatter& ppf);
void pp_print_if_newline(Formatter& ppf);
void pp_open_tbox(Formatter& ppf);
void pp_close_tbox(Formatter& ppf);
void pp_set_tab(Formatter& ppf);
void pp_print_tbreak(Formatter& ppf, long width, long offset);
void pp_print_tab(Formatter& ppf);
void pp_doc(Formatter& ppf, const Doc& doc);
void deprecated_printer(Formatter& ppf, std::function<void(format::Formatter&)> pr);

// ---- fprintf ----------------------------------------------------------------------
// Interprets an OCaml format string (Format's @-items, tags @{<t> @}, magic
// sizes @<n>, and the conversions %s %S %c %C %d %i %u %x %X %o (with the
// l / L / n size prefixes, flags, width and precision, `*` taking an int
// argument) %a %t %%).  A %a / %t argument is a callable `void(Formatter&)`
// (`pr(printer, value)` for a printer and its value).
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
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>, int> = 0>
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
  std::vector<Arg> v;
  v.reserve(sizeof...(Args));
  (v.push_back(to_arg(std::forward<Args>(args))), ...);
  vfprintf(ppf, fmt, v);
}

// doc_printf fmt args: the document fprintf builds
template <class... Args>
Doc doc_printf(std::string_view fmt, Args&&... args) {
  Formatter f;
  fprintf(f, fmt, std::forward<Args>(args)...);
  return std::move(f.doc);
}

// asprintf: laid out on a fresh Format formatter (Format.asprintf "%a"
// Doc.format), flushed
template <class... Args>
std::string asprintf(std::string_view fmt, Args&&... args) {
  Doc d = doc_printf(fmt, std::forward<Args>(args)...);
  format::Formatter f;
  format(f, d);
  f.print_flush();
  return f.take();
}

// `%a` printer + value as one argument
template <class P, class V>
auto pr(P printer, V value) {
  return [printer, value](Formatter& ppf) { printer(ppf, value); };
}

// doc_printer f x: the document a printer writes
template <class P, class V>
Doc doc_printer(P printer, const V& x) {
  Formatter f;
  printer(f, x);
  return std::move(f.doc);
}

// compat printer: a Format printer (the printer's document, laid out)
template <class P, class V>
void compat(P printer, format::Formatter& ppf, const V& x) {
  Doc d = doc_printer(printer, x);
  format(ppf, d);
}

// pp_print_list ?(pp_sep = pp_print_cut) elt
template <class T, class P>
void pp_print_list(Formatter& ppf, P elt, const std::vector<T>& l,
                   const std::function<void(Formatter&)>& pp_sep = pp_print_cut) {
  bool first = true;
  for (const T& x : l) {
    if (!first) pp_sep(ppf);
    first = false;
    elt(ppf, x);
  }
}

void comma(Formatter& ppf);      // ",@ "
void semicolon(Formatter& ppf);  // ";@ "

// Doc.align_prefix / align_prefix2 (the "ralign" tag)
std::vector<Doc> align_prefix(const std::vector<std::pair<Doc, long>>& l);
std::pair<Doc, Doc> align_prefix2(const std::pair<Doc, long>& x, const std::pair<Doc, long>& y);

}  // namespace cppcaml::typing::format_doc
