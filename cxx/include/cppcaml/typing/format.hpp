// Port of the pretty-printing engine of stdlib/format.ml (TYPECHECKER.md
// stage 10): boxes (h, v, hv, hov, b), break hints, the margin and max
// indent, and the scan/queue algorithm that lays them out -- so printers
// ported from the compiler (Printlambda, Printtyp) produce ocamlc's exact
// text.  A Formatter accumulates into a string.
#pragma once

#include <string>
#include <string_view>

namespace cppcaml::typing::format {

enum class BoxType { Pp_hbox, Pp_vbox, Pp_hvbox, Pp_hovbox, Pp_box, Pp_fits };

class Formatter;  // defined by the engine port (format.cpp's state)

// The engine port defines Formatter with at least these operations (the
// names of format.ml's pp_* functions):
//   open_box_gen(indent, BoxType), close_box(), print_string(s),
//   print_as(width, s), print_break(nspaces, offset), print_space(),
//   print_cut(), force_newline(), print_newline(), print_flush(),
//   set_margin(n), set_max_indent(n), and
//   fprintf(ppf, fmt, args...) interpreting an OCaml format string's @-boxes
//   and %-conversions (%s %d %i %S %c %a ...).

}  // namespace cppcaml::typing::format
