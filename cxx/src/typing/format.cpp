// Port of the pretty-printing engine of stdlib/format.ml and of Format's
// interpretation of format strings (camlinternalFormat.ml's parser +
// format.ml's output_acc).  See format.hpp.
#include "cppcaml/typing/format.hpp"

#include <algorithm>
#include <stdexcept>

namespace cppcaml::typing::format {

// ---- Bytes.get_utf_8_uchar's decode length ----------------------------------
namespace {
// the number of bytes the decode at i consumes (valid or not)
long utf_8_decode_length(std::string_view b, std::size_t i) {
  auto get = [&](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(b[k])); };
  unsigned b0 = get(i);
  std::size_t max = b.size() - 1;
  auto not_in_x80_to_xBF = [](unsigned x) { return (x >> 6) != 0b10; };
  auto not_in_xA0_to_xBF = [](unsigned x) { return (x >> 5) != 0b101; };
  auto not_in_x80_to_x9F = [](unsigned x) { return (x >> 5) != 0b100; };
  auto not_in_x90_to_xBF = [](unsigned x) { return x < 0x90 || 0xBF < x; };
  auto not_in_x80_to_x8F = [](unsigned x) { return (x >> 4) != 0x8; };
  if (b0 <= 0x7F) return 1;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    std::size_t k = i + 1;
    if (k > max) return 1;
    if (not_in_x80_to_xBF(get(k))) return 1;
    return 2;
  }
  // three- and four-byte sequences: the first continuation byte's range
  // depends on b0; the rest are x80..xBF
  bool (*first)(unsigned) = nullptr;
  int n = 0;
  if (b0 == 0xE0) {
    first = +not_in_xA0_to_xBF, n = 3;
  } else if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF) {
    first = +not_in_x80_to_xBF, n = 3;
  } else if (b0 == 0xED) {
    first = +not_in_x80_to_x9F, n = 3;
  } else if (b0 == 0xF0) {
    first = +not_in_x90_to_xBF, n = 4;
  } else if (b0 >= 0xF1 && b0 <= 0xF3) {
    first = +not_in_x80_to_xBF, n = 4;
  } else if (b0 == 0xF4) {
    first = +not_in_x80_to_x8F, n = 4;
  } else {
    return 1;
  }
  std::size_t k = i + 1;
  if (k > max) return 1;
  if (first(get(k))) return 1;
  for (int m = 2; m < n; ++m) {
    k = k + 1;
    if (k > max) return m;
    if (not_in_x80_to_xBF(get(k))) return m;
  }
  return n;
}
}  // namespace

long utf_8_scalar_width(std::string_view s) {
  long count = 0;
  std::size_t current = 0, stop = s.size();
  while (current < stop) {
    long advance = utf_8_decode_length(s, current);
    count = count + 1;
    current = current + advance;
  }
  return count;
}

// ---- the engine -----------------------------------------------------------------
Formatter::QueueElem* Formatter::new_elem(long size, Tok token, long length) {
  pool_.push_back(std::make_unique<QueueElem>());
  QueueElem* e = pool_.back().get();
  e->size = size;
  e->token = token;
  e->length = length;
  return e;
}

Formatter::Formatter() {
  // The initial state of the formatter contains a dummy box.
  QueueElem* sys_tok = new_elem(-1, Tok::Pp_begin, 0);
  sys_tok->indent = 0;
  sys_tok->box = BoxType::Pp_hovbox;
  queue_.push_back(sys_tok);
  initialize_scan_stack();
  scan_stack_.push_back({1, sys_tok});
  margin_ = 78;
  min_space_left_ = 10;
  max_indent_ = margin_ - min_space_left_;
  space_left_ = margin_;
  current_indent_ = 0;
  is_new_line_ = true;
  left_total_ = 1;
  right_total_ = 1;
  curr_depth_ = 1;
  max_boxes_ = INT64_MAX;
  ellipsis_ = ".";
}

long Formatter::string_width(std::string_view s) const { return utf_8_scalar_width(s); }

// display_blanks
void Formatter::output_spaces(long n) {
  if (n > 0) out_.append(static_cast<std::size_t>(n), ' ');
}

// Format a textual token
void Formatter::format_pp_text(long size, std::string_view text) {
  space_left_ = space_left_ - size;
  output_string(text);
  is_new_line_ = false;
}

// Format a string by its length, if not empty
void Formatter::format_string(std::string_view s) {
  if (!s.empty()) format_pp_text(string_width(s), s);
}

// To format a break, indenting a new line.
void Formatter::break_new_line(std::string_view before, long offset, std::string_view after, long width) {
  format_string(before);
  output_newline();
  is_new_line_ = true;
  long indent = margin_ - width + offset;
  // Don't indent more than pp_max_indent.
  long real_indent = std::min(max_indent_, indent);
  current_indent_ = real_indent;
  space_left_ = margin_ - current_indent_;
  output_indent(current_indent_);
  format_string(after);
}

// To format a break that fits on the current line.
void Formatter::break_same_line(std::string_view before, long width, std::string_view after) {
  format_string(before);
  space_left_ = space_left_ - width;
  output_spaces(width);
  format_string(after);
}

// To indent no more than pp_max_indent, if one tries to open a box beyond
// pp_max_indent, then the box is rejected on the left by simulating a break.
void Formatter::force_break_line() {
  if (format_stack_.empty()) {
    output_newline();
    return;
  }
  FormatElem top = format_stack_.back();
  if (top.width > space_left_) {
    switch (top.box_type) {
      case BoxType::Pp_fits:
      case BoxType::Pp_hbox:
        break;
      case BoxType::Pp_vbox:
      case BoxType::Pp_hvbox:
      case BoxType::Pp_hovbox:
      case BoxType::Pp_box:
        break_line(top.width);
        break;
    }
  }
}

// To skip a token, if the previous line has been broken.
void Formatter::skip_token() {
  if (queue_.empty()) return;  // print_if_newline must have been the last printing command
  QueueElem* e = queue_.front();
  queue_.pop_front();
  left_total_ = left_total_ - e->length;
  space_left_ = space_left_ + e->size;
}

// Formatting a token with a given size.
void Formatter::format_pp_token(long size, QueueElem* e) {
  switch (e->token) {
    case Tok::Pp_text:
      format_pp_text(size, e->text);
      return;
    case Tok::Pp_begin: {
      long insertion_point = margin_ - space_left_;
      if (insertion_point > max_indent_)
        // can not open a box right there.
        force_break_line();
      long width = space_left_ - e->indent;
      BoxType box_type =
          e->box == BoxType::Pp_vbox ? BoxType::Pp_vbox : (size > space_left_ ? e->box : BoxType::Pp_fits);
      format_stack_.push_back({box_type, width});
      return;
    }
    case Tok::Pp_end:
      if (!format_stack_.empty()) format_stack_.pop_back();
      return;
    case Tok::Pp_newline:
      if (format_stack_.empty())
        output_newline();  // No open box.
      else
        break_line(format_stack_.back().width);
      return;
    case Tok::Pp_if_newline:
      if (current_indent_ != margin_ - space_left_) skip_token();
      return;
    case Tok::Pp_break: {
      if (format_stack_.empty()) return;  // No open box.
      FormatElem top = format_stack_.back();
      long width = top.width;
      switch (top.box_type) {
        case BoxType::Pp_hovbox:
          if (size + string_width(e->bb) > space_left_)
            break_new_line(e->bb, e->bo, e->ba, width);
          else
            break_same_line(e->fb, e->fw, e->fa);
          return;
        case BoxType::Pp_box:
          // Have the line just been broken here ?
          if (is_new_line_)
            break_same_line(e->fb, e->fw, e->fa);
          else if (size + string_width(e->bb) > space_left_)
            break_new_line(e->bb, e->bo, e->ba, width);
          // break the line here leads to new indentation ?
          else if (current_indent_ > margin_ - width + e->bo)
            break_new_line(e->bb, e->bo, e->ba, width);
          else
            break_same_line(e->fb, e->fw, e->fa);
          return;
        case BoxType::Pp_hvbox:
          break_new_line(e->bb, e->bo, e->ba, width);
          return;
        case BoxType::Pp_fits:
          break_same_line(e->fb, e->fw, e->fa);
          return;
        case BoxType::Pp_vbox:
          break_new_line(e->bb, e->bo, e->ba, width);
          return;
        case BoxType::Pp_hbox:
          break_same_line(e->fb, e->fw, e->fa);
          return;
      }
      return;
    }
  }
}

// Print if token size is known else printing is delayed.
void Formatter::advance_left() {
  while (!queue_.empty()) {
    QueueElem* e = queue_.front();
    long pending_count = right_total_ - left_total_;
    if (!(e->size >= 0 || pending_count >= space_left_)) return;
    queue_.pop_front();
    long size = e->size >= 0 ? e->size : pp_infinity;
    format_pp_token(size, e);
    left_total_ = e->length + left_total_;
  }
}

// Enter a token in the pretty-printer queue.
void Formatter::enqueue(QueueElem* e) {
  right_total_ = right_total_ + e->length;
  queue_.push_back(e);
}

void Formatter::enqueue_advance(QueueElem* e) {
  enqueue(e);
  advance_left();
}

void Formatter::enqueue_string_as(long size, std::string_view s) {
  QueueElem* e = new_elem(size, Tok::Pp_text, size);
  e->text = std::string(s);
  enqueue_advance(e);
}

void Formatter::enqueue_string(std::string_view s) { enqueue_string_as(string_width(s), s); }

// The scan_stack is never empty.
void Formatter::initialize_scan_stack() {
  scan_stack_.clear();
  QueueElem* e = new_elem(-1, Tok::Pp_text, 0);
  scan_stack_.push_back({-1, e});
}

// Setting the size of boxes on scan stack.
void Formatter::set_size(bool break_hint) {
  if (scan_stack_.empty()) return;
  ScanElem top = scan_stack_.back();
  long size = top.queue_elem->size;
  // test if scan stack contains any data that is not obsolete.
  if (top.left_total < left_total_) {
    initialize_scan_stack();
    return;
  }
  switch (top.queue_elem->token) {
    case Tok::Pp_break:
      if (break_hint) {
        top.queue_elem->size = right_total_ + size;
        scan_stack_.pop_back();
      }
      return;
    case Tok::Pp_begin:
      if (!break_hint) {
        top.queue_elem->size = right_total_ + size;
        scan_stack_.pop_back();
      }
      return;
    default:
      return;  // scan_push is only used for breaks and boxes.
  }
}

// Enter a break hint, increasing the rightward position *after* updating the
// pending break.
void Formatter::enqueue_break(QueueElem* e) {
  queue_.push_back(e);
  set_size(true);
  right_total_ = right_total_ + e->length;
}

void Formatter::scan_push(bool break_hint, QueueElem* e) {
  if (break_hint)
    enqueue_break(e);
  else
    enqueue(e);
  scan_stack_.push_back({right_total_, e});
}

void Formatter::open_box_gen(long indent, BoxType ty) {
  curr_depth_ = curr_depth_ + 1;
  if (curr_depth_ < max_boxes_) {
    QueueElem* e = new_elem(-right_total_, Tok::Pp_begin, 0);
    e->indent = indent;
    e->box = ty;
    scan_push(false, e);
  } else if (curr_depth_ == max_boxes_) {
    enqueue_string(ellipsis_);
  }
}

void Formatter::close_box() {
  if (curr_depth_ > 1) {
    if (curr_depth_ < max_boxes_) {
      enqueue(new_elem(0, Tok::Pp_end, 0));
      set_size(true);
      set_size(false);
    }
    curr_depth_ = curr_depth_ - 1;
  }
}

void Formatter::clear_queue() {
  left_total_ = 1;
  right_total_ = 1;
  queue_.clear();
}

void Formatter::rinit() {
  clear_queue();
  // every element still referenced is dropped here: the queue is empty and the
  // scan stack is reinitialized
  pool_.clear();
  initialize_scan_stack();
  format_stack_.clear();
  current_indent_ = 0;
  curr_depth_ = 0;
  space_left_ = margin_;
  open_sys_box();
}

void Formatter::flush_queue(bool end_with_newline) {
  while (curr_depth_ > 1) close_box();
  right_total_ = pp_infinity;
  advance_left();
  if (end_with_newline) output_newline();
  rinit();
}

void Formatter::print_as_size(long size, std::string_view s) {
  if (curr_depth_ < max_boxes_) enqueue_string_as(size, s);
}

void Formatter::print_string(std::string_view s) { print_as(string_width(s), s); }

void Formatter::print_char(char c) { print_as(1, std::string_view(&c, 1)); }

void Formatter::print_int(long i) { print_string(std::to_string(i)); }

void Formatter::print_newline() { flush_queue(true); }

void Formatter::print_flush() { flush_queue(false); }

void Formatter::force_newline() {
  if (curr_depth_ < max_boxes_) enqueue_advance(new_elem(0, Tok::Pp_newline, 0));
}

void Formatter::print_if_newline() {
  if (curr_depth_ < max_boxes_) enqueue_advance(new_elem(0, Tok::Pp_if_newline, 0));
}

void Formatter::print_custom_break(std::string_view fits_before, long fits_width, std::string_view fits_after,
                                   std::string_view breaks_before, long breaks_offset,
                                   std::string_view breaks_after) {
  if (curr_depth_ < max_boxes_) {
    long size = -right_total_;
    long length = string_width(fits_before) + fits_width + string_width(fits_after);
    QueueElem* e = new_elem(size, Tok::Pp_break, length);
    e->fb = std::string(fits_before);
    e->fw = fits_width;
    e->fa = std::string(fits_after);
    e->bb = std::string(breaks_before);
    e->bo = breaks_offset;
    e->ba = std::string(breaks_after);
    scan_push(true, e);
  }
}

void Formatter::print_break(long width, long offset) { print_custom_break("", width, "", "", offset, ""); }

// To set the margin of pretty-printer.
static long pp_limit(long n) { return n < pp_infinity ? n : pp_infinity - 1; }

void Formatter::set_min_space_left(long n) {
  if (n >= 1) {
    n = pp_limit(n);
    min_space_left_ = n;
    max_indent_ = margin_ - min_space_left_;
    rinit();
  }
}

void Formatter::set_max_indent(long n) {
  if (n > 1) set_min_space_left(margin_ - n);
}

void Formatter::set_margin(long n) {
  if (n >= 1) {
    n = pp_limit(n);
    margin_ = n;
    long new_max_indent =
        // Try to maintain max_indent to its actual value.
        max_indent_ <= margin_ ? max_indent_
                               // If possible maintain pp_min_space_left to its actual value, if this
                               // leads to a too small max_indent, take half of the new margin, if it
                               // is greater than 1.
                               : std::max(std::max(margin_ - min_space_left_, margin_ / 2), 1L);
    // Rebuild invariants.
    set_max_indent(new_max_indent);
  }
}

// ---- String.escaped / Char.escaped ----------------------------------------------
static void escape_decimal(std::string& r, unsigned char c) {
  r.push_back('\\');
  r.push_back(static_cast<char>('0' + c / 100));
  r.push_back(static_cast<char>('0' + (c / 10) % 10));
  r.push_back(static_cast<char>('0' + c % 10));
}

std::string string_escaped(std::string_view s) {
  std::string r;
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\b': r += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~')
          r.push_back(ch);
        else
          escape_decimal(r, c);
    }
  }
  return r;
}

std::string char_escaped(char ch) {
  unsigned char c = static_cast<unsigned char>(ch);
  std::string r;
  switch (c) {
    case '\'': return "\\'";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\t': return "\\t";
    case '\r': return "\\r";
    case '\b': return "\\b";
    default:
      if (c >= ' ' && c <= '~')
        r.push_back(ch);
      else
        escape_decimal(r, c);
      return r;
  }
}

// ---- fprintf ----------------------------------------------------------------------
namespace {
// open_box_of_string
std::pair<long, BoxType> open_box_of_string(std::string_view str) {
  if (str.empty()) return {0, BoxType::Pp_box};
  std::size_t len = str.size();
  auto invalid_box = [&]() -> void {
    throw std::runtime_error("invalid box description \"" + string_escaped(str) + "\"");
  };
  auto parse_spaces = [&](std::size_t i) {
    while (i < len && (str[i] == ' ' || str[i] == '\t')) ++i;
    return i;
  };
  std::size_t wstart = parse_spaces(0);
  std::size_t wend = wstart;
  while (wend < len && str[wend] >= 'a' && str[wend] <= 'z') ++wend;
  std::string_view box_name = str.substr(wstart, wend - wstart);
  std::size_t nstart = parse_spaces(wend);
  std::size_t nend = nstart;
  while (nend < len && ((str[nend] >= '0' && str[nend] <= '9') || str[nend] == '-')) ++nend;
  long indent = 0;
  if (nstart != nend) {
    try {
      std::size_t used = 0;
      std::string num(str.substr(nstart, nend - nstart));
      indent = std::stol(num, &used);
      if (used != num.size()) invalid_box();
    } catch (const std::logic_error&) {
      invalid_box();
    }
  }
  std::size_t exp_end = parse_spaces(nend);
  if (exp_end != len) invalid_box();
  BoxType box_type = BoxType::Pp_box;
  if (box_name.empty() || box_name == "b")
    box_type = BoxType::Pp_box;
  else if (box_name == "h")
    box_type = BoxType::Pp_hbox;
  else if (box_name == "v")
    box_type = BoxType::Pp_vbox;
  else if (box_name == "hv")
    box_type = BoxType::Pp_hvbox;
  else if (box_name == "hov")
    box_type = BoxType::Pp_hovbox;
  else
    invalid_box();
  return {indent, box_type};
}

bool is_digit_or_minus(char c) { return (c >= '0' && c <= '9') || c == '-'; }

// parse_integer: an optional '-' then digits
bool parse_integer(std::string_view s, std::size_t& i, long& out) {
  std::size_t j = i;
  bool neg = false;
  if (j < s.size() && s[j] == '-') neg = true, ++j;
  std::size_t d = j;
  long v = 0;
  while (j < s.size() && s[j] >= '0' && s[j] <= '9') v = v * 10 + (s[j++] - '0');
  if (j == d) return false;
  out = neg ? -v : v;
  i = j;
  return true;
}

std::size_t skip_spaces(std::string_view s, std::size_t i) {
  while (i < s.size() && s[i] == ' ') ++i;
  return i;
}
}  // namespace

void vfprintf(Formatter& ppf, std::string_view fmt, const std::vector<Arg>& args) {
  std::size_t argi = 0;
  auto next_arg = [&]() -> const Arg& {
    if (argi >= args.size()) throw std::runtime_error("format::fprintf: too few arguments");
    return args[argi++];
  };
  std::size_t n = fmt.size();
  std::size_t lit_start = 0;
  std::size_t i = 0;
  // add_literal: a one-char literal is a Char_literal (pp_print_char)
  auto flush_literal = [&](std::size_t end) {
    std::size_t size = end - lit_start;
    if (size == 1)
      ppf.print_char(fmt[lit_start]);
    else if (size > 1)
      ppf.print_string(fmt.substr(lit_start, size));
  };
  while (i < n) {
    char c = fmt[i];
    if (c == '@') {
      flush_literal(i);
      std::size_t j = i + 1;
      if (j == n) {
        ppf.print_char('@');
        i = j;
        lit_start = i;
        continue;
      }
      char d = fmt[j];
      switch (d) {
        case '[': {
          std::size_t k = j + 1;
          std::string_view spec;
          if (k < n && fmt[k] == '<') {
            std::size_t close = fmt.find('>', k + 1);
            if (close != std::string_view::npos) {
              spec = fmt.substr(k + 1, close - k - 1);
              k = close + 1;
            }
          }
          auto [indent, ty] = open_box_of_string(spec);
          ppf.open_box_gen(indent, ty);
          i = k;
          break;
        }
        case ']': ppf.close_box(); i = j + 1; break;
        case '{': case '}': i = j + 1; break;  // semantic tags: not ported (not marked, not printed)
        case ',': ppf.print_break(0, 0); i = j + 1; break;
        case ' ': ppf.print_break(1, 0); i = j + 1; break;
        case ';': {
          // parse_good_break: "@;<width [offset]>" else Break ("@;", 1, 0)
          std::size_t k = j + 1;
          long width = 1, offset = 0;
          std::size_t next = k;
          if (k < n && fmt[k] == '<') {
            std::size_t p = skip_spaces(fmt, k + 1);
            long w;
            if (p < n && is_digit_or_minus(fmt[p]) && parse_integer(fmt, p, w)) {
              p = skip_spaces(fmt, p);
              if (p < n && fmt[p] == '>') {
                width = w, offset = 0, next = p + 1;
              } else if (p < n && is_digit_or_minus(fmt[p])) {
                long o;
                if (parse_integer(fmt, p, o)) {
                  p = skip_spaces(fmt, p);
                  if (p < n && fmt[p] == '>') width = w, offset = o, next = p + 1;
                }
              }
            }
          }
          ppf.print_break(width, offset);
          i = next;
          break;
        }
        case '?': ppf.print_flush(); i = j + 1; break;
        case '\n': ppf.force_newline(); i = j + 1; break;
        case '.': ppf.print_newline(); i = j + 1; break;
        case '@': ppf.print_char('@'); i = j + 1; break;
        case '%':
          if (j + 1 < n && fmt[j + 1] == '%') {
            ppf.print_char('%');
            i = j + 2;
          } else {
            ppf.print_char('@');
            i = j;
          }
          break;
        default:
          // Scan_indic c (and "@<n>" magic sizes, not ported)
          ppf.print_char('@');
          ppf.print_char(d);
          i = j + 1;
          break;
      }
      lit_start = i;
      continue;
    }
    if (c == '%') {
      flush_literal(i);
      std::size_t j = i + 1;
      if (j >= n) throw std::runtime_error("format::fprintf: unexpected end of format");
      char size_prefix = 0;
      if ((fmt[j] == 'l' || fmt[j] == 'L' || fmt[j] == 'n') && j + 1 < n &&
          (fmt[j + 1] == 'd' || fmt[j + 1] == 'i')) {
        size_prefix = fmt[j];
        ++j;
      }
      char conv = fmt[j];
      switch (conv) {
        case '%': ppf.print_char('%'); break;
        case 's': {
          const Arg& a = next_arg();
          ppf.print_string(a.s);
          break;
        }
        case 'S': {
          const Arg& a = next_arg();
          ppf.print_string("\"" + string_escaped(a.s) + "\"");
          break;
        }
        case 'c': ppf.print_char(next_arg().c); break;
        case 'C': ppf.print_string("'" + char_escaped(next_arg().c) + "'"); break;
        case 'd':
        case 'i': {
          const Arg& a = next_arg();
          ppf.print_string(std::to_string(a.i));
          break;
        }
        case 'a':
        case 't': {
          const Arg& a = next_arg();
          a.fn(ppf);
          break;
        }
        default:
          throw std::runtime_error(std::string("format::fprintf: unsupported conversion %") + conv);
      }
      (void)size_prefix;
      i = j + 1;
      lit_start = i;
      continue;
    }
    ++i;
  }
  flush_literal(n);
  if (argi != args.size()) throw std::runtime_error("format::fprintf: too many arguments");
}

}  // namespace cppcaml::typing::format
