// Port of utils/format_doc.ml (TYPECHECKER.md stage 9).
#include "cppcaml/typing/format_doc.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace cppcaml::typing::format_doc {

namespace {

Element el(Element::K k) {
  Element e;
  e.k = k;
  return e;
}

BoxType box_type(format::BoxType b) {
  switch (b) {
    case format::BoxType::Pp_fits:
    case format::BoxType::Pp_hbox: return BoxType::H;
    case format::BoxType::Pp_vbox: return BoxType::V;
    case format::BoxType::Pp_hovbox: return BoxType::HoV;
    case format::BoxType::Pp_hvbox: return BoxType::HV;
    case format::BoxType::Pp_box: return BoxType::B;
  }
  return BoxType::B;
}

// Misc.Style.style_of_tag: the tags whose markup is "" with colours off;
// any other tag gets Format's default markers "<t>" / "</t>"
bool known_style_tag(const std::string& t) {
  return t == "error" || t == "warning" || t == "loc" || t == "hint" || t == "inline_code" || t == "ralign" ||
         t == "@style";  // Misc.Style.Style (diffing.hpp)
}

}  // namespace

// ---- Doc ------------------------------------------------------------------------

void format(format::Formatter& ppf, const Doc& doc) {
  std::vector<std::string> tag_stack;
  const auto& l = doc.els;
  for (std::size_t i = 0; i < l.size(); ++i) {
    const Element& e = l[i];
    switch (e.k) {
      case Element::K::Text: ppf.print_string(e.text); break;
      case Element::K::With_size:
        // With_size size :: Text text -> pp_print_as size text
        if (i + 1 < l.size() && l[i + 1].k == Element::K::Text) {
          ppf.print_as(e.a, l[i + 1].text);
          ++i;
        }
        break;
      case Element::K::Open_box:
        switch (e.box) {
          case BoxType::H: ppf.open_hbox(); break;
          case BoxType::V: ppf.open_vbox(e.a); break;
          case BoxType::HV: ppf.open_hvbox(e.a); break;
          case BoxType::HoV: ppf.open_hovbox(e.a); break;
          case BoxType::B: ppf.open_box(e.a); break;
        }
        break;
      case Element::K::Close_box: ppf.close_box(); break;
      case Element::K::Open_tag:
        tag_stack.push_back(e.text);
        if (!known_style_tag(e.text)) ppf.print_as(0, "<" + e.text + ">");
        break;
      case Element::K::Close_tag:
        if (!tag_stack.empty()) {
          std::string t = std::move(tag_stack.back());
          tag_stack.pop_back();
          if (!known_style_tag(t)) ppf.print_as(0, "</" + t + ">");
        }
        break;
      case Element::K::Open_tbox:
      case Element::K::Tab_break:
      case Element::K::Set_tab:
      case Element::K::Close_tbox: break;  // tabulation boxes: not ported
      case Element::K::Simple_break: ppf.print_break(e.a, e.b); break;
      case Element::K::Break:
        ppf.print_custom_break(e.text, e.a, e.fits_after, e.breaks_before, e.b, e.breaks_after);
        break;
      case Element::K::Flush:
        if (e.newline) ppf.print_newline();
        else ppf.print_flush();
        break;
      case Element::K::Newline: ppf.force_newline(); break;
      case Element::K::If_newline: ppf.print_if_newline(); break;
      case Element::K::Deprecated: e.deprecated(ppf); break;
    }
  }
}

std::string to_string(const Doc& doc) {
  std::string r;
  for (const Element& e : doc.els)
    if (e.k == Element::K::Text) r += e.text;
  return r;
}

Doc append(const Doc& left, const Doc& right) {
  Doc d = left;
  d.els.insert(d.els.end(), right.els.begin(), right.els.end());
  return d;
}

// ---- primitives ----

void pp_print_string(Formatter& ppf, std::string_view s) {
  Element e = el(Element::K::Text);
  e.text = std::string(s);
  ppf.add(std::move(e));
}

void pp_print_as(Formatter& ppf, long size, std::string_view s) {
  Element w = el(Element::K::With_size);
  w.a = size;
  ppf.add(std::move(w));
  pp_print_string(ppf, s);
}

// Doc.text: free-flowing text, spaces as break hints and newlines forced
void pp_print_text(Formatter& ppf, std::string_view s) {
  std::size_t len = s.size(), left = 0, right = 0;
  while (true) {
    if (right == len) {
      if (left != len) pp_print_string(ppf, s.substr(left, right - left));
      return;
    }
    char c = s[right];
    if (c == '\n' || c == ' ') {
      pp_print_string(ppf, s.substr(left, right - left));
      if (c == '\n') pp_force_newline(ppf);
      else pp_print_space(ppf);
      left = right = right + 1;
    } else {
      ++right;
    }
  }
}

void pp_print_char(Formatter& ppf, char c) { pp_print_string(ppf, std::string(1, c)); }
void pp_print_int(Formatter& ppf, long n) { pp_print_string(ppf, std::to_string(n)); }
void pp_print_bool(Formatter& ppf, bool b) { pp_print_string(ppf, b ? "true" : "false"); }

void pp_open_box_gen(Formatter& ppf, long indent, format::BoxType bty) {
  Element e = el(Element::K::Open_box);
  e.box = box_type(bty);
  e.a = indent;
  ppf.add(std::move(e));
}
void pp_open_box(Formatter& ppf, long indent) { pp_open_box_gen(ppf, indent, format::BoxType::Pp_box); }
void pp_open_hbox(Formatter& ppf) { pp_open_box_gen(ppf, 0, format::BoxType::Pp_hbox); }
void pp_open_vbox(Formatter& ppf, long indent) { pp_open_box_gen(ppf, indent, format::BoxType::Pp_vbox); }
void pp_open_hvbox(Formatter& ppf, long indent) { pp_open_box_gen(ppf, indent, format::BoxType::Pp_hvbox); }
void pp_open_hovbox(Formatter& ppf, long indent) { pp_open_box_gen(ppf, indent, format::BoxType::Pp_hovbox); }
void pp_close_box(Formatter& ppf) { ppf.add(el(Element::K::Close_box)); }

void pp_open_stag(Formatter& ppf, std::string_view string_tag) {
  Element e = el(Element::K::Open_tag);
  e.text = std::string(string_tag);
  ppf.add(std::move(e));
}
void pp_close_stag(Formatter& ppf) { ppf.add(el(Element::K::Close_tag)); }

void pp_print_break(Formatter& ppf, long spaces, long indent) {
  Element e = el(Element::K::Simple_break);
  e.a = spaces;
  e.b = indent;
  ppf.add(std::move(e));
}
void pp_print_custom_break(Formatter& ppf, std::string_view fits_before, long fits_width,
                           std::string_view fits_after, std::string_view breaks_before, long breaks_offset,
                           std::string_view breaks_after) {
  Element e = el(Element::K::Break);
  e.text = std::string(fits_before);
  e.a = fits_width;
  e.fits_after = std::string(fits_after);
  e.breaks_before = std::string(breaks_before);
  e.b = breaks_offset;
  e.breaks_after = std::string(breaks_after);
  ppf.add(std::move(e));
}
void pp_print_space(Formatter& ppf) { pp_print_break(ppf, 1, 0); }
void pp_print_cut(Formatter& ppf) { pp_print_break(ppf, 0, 0); }
void pp_print_flush(Formatter& ppf) {
  Element e = el(Element::K::Flush);
  e.newline = false;
  ppf.add(std::move(e));
}
void pp_force_newline(Formatter& ppf) { ppf.add(el(Element::K::Newline)); }
void pp_print_newline(Formatter& ppf) {
  Element e = el(Element::K::Flush);
  e.newline = true;
  ppf.add(std::move(e));
}
void pp_print_if_newline(Formatter& ppf) { ppf.add(el(Element::K::If_newline)); }
void pp_doc(Formatter& ppf, const Doc& doc) {
  ppf.doc.els.insert(ppf.doc.els.end(), doc.els.begin(), doc.els.end());
}
void deprecated_printer(Formatter& ppf, std::function<void(format::Formatter&)> pr) {
  Element e = el(Element::K::Deprecated);
  e.deprecated = std::move(pr);
  ppf.add(std::move(e));
}

void comma(Formatter& ppf) { fprintf(ppf, ",@ "); }
void semicolon(Formatter& ppf) { fprintf(ppf, ";@ "); }

// ---- fprintf ----------------------------------------------------------------------

namespace {

bool parse_int(std::string_view s, std::size_t& i, long& out) {
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

// fix_padding: Right (default) / Left ('-') / Zeros ('0')
std::string pad(std::string s, long width, bool left, bool zeros) {
  if (width < 0) left = true, width = -width;
  if (static_cast<long>(s.size()) >= width) return s;
  std::size_t n = static_cast<std::size_t>(width) - s.size();
  if (left) return s + std::string(n, ' ');
  if (zeros) {
    std::size_t sign = (!s.empty() && (s[0] == '-' || s[0] == '+' || s[0] == ' ')) ? 1 : 0;
    return s.substr(0, sign) + std::string(n, '0') + s.substr(sign);
  }
  return std::string(n, ' ') + s;
}

std::string to_base(unsigned long long v, int base, bool upper) {
  if (v == 0) return "0";
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string r;
  while (v) {
    r.push_back(digits[v % static_cast<unsigned>(base)]);
    v /= static_cast<unsigned>(base);
  }
  std::reverse(r.begin(), r.end());
  return r;
}

}  // namespace

void vfprintf(Formatter& ppf, std::string_view fmt, const std::vector<Arg>& args) {
  std::size_t argi = 0;
  auto next_arg = [&]() -> const Arg& {
    if (argi >= args.size()) throw std::runtime_error("format_doc::fprintf: too few arguments");
    return args[argi++];
  };
  std::optional<long> magic;  // a pending Magic_size for the next string / char
  auto out_string = [&](std::string_view s) {
    if (magic) {
      pp_print_as(ppf, *magic, s);
      magic.reset();
    } else {
      pp_print_string(ppf, s);
    }
  };
  std::size_t n = fmt.size();
  std::size_t lit_start = 0;
  std::size_t i = 0;
  auto flush_literal = [&](std::size_t end) {
    if (end > lit_start) out_string(fmt.substr(lit_start, end - lit_start));
  };
  // a formatting item other than a literal / data: a pending magic size is dropped
  auto item = [&]() { magic.reset(); };
  while (i < n) {
    char c = fmt[i];
    if (c == '@') {
      flush_literal(i);
      std::size_t j = i + 1;
      if (j == n) {
        out_string("@");
        i = j;
        lit_start = i;
        continue;
      }
      char d = fmt[j];
      switch (d) {
        case '[': {
          item();
          std::size_t k = j + 1;
          std::string_view spec;
          if (k < n && fmt[k] == '<') {
            std::size_t close = fmt.find('>', k + 1);
            if (close != std::string_view::npos) {
              spec = fmt.substr(k + 1, close - k - 1);
              k = close + 1;
            }
          }
          auto [indent, ty] = format::open_box_of_string(spec);
          pp_open_box_gen(ppf, indent, ty);
          i = k;
          break;
        }
        case ']': item(); pp_close_box(ppf); i = j + 1; break;
        case '{': {
          item();
          std::size_t k = j + 1;
          std::string tag;
          if (k < n && fmt[k] == '<') {
            std::size_t close = fmt.find('>', k + 1);
            if (close != std::string_view::npos) {
              tag = std::string(fmt.substr(k + 1, close - k - 1));
              k = close + 1;
            }
          }
          pp_open_stag(ppf, tag);
          i = k;
          break;
        }
        case '}': item(); pp_close_stag(ppf); i = j + 1; break;
        case ',': item(); pp_print_break(ppf, 0, 0); i = j + 1; break;
        case ' ': item(); pp_print_break(ppf, 1, 0); i = j + 1; break;
        case ';': {
          item();
          // parse_good_break: "@;<width [offset]>" else Break ("@;", 1, 0)
          std::size_t k = j + 1;
          long width = 1, offset = 0;
          std::size_t next = k;
          if (k < n && fmt[k] == '<') {
            std::size_t p = skip_spaces(fmt, k + 1);
            long w;
            if (p < n && parse_int(fmt, p, w)) {
              p = skip_spaces(fmt, p);
              if (p < n && fmt[p] == '>') {
                width = w, offset = 0, next = p + 1;
              } else {
                long o;
                if (p < n && parse_int(fmt, p, o)) {
                  p = skip_spaces(fmt, p);
                  if (p < n && fmt[p] == '>') width = w, offset = o, next = p + 1;
                }
              }
            }
          }
          pp_print_break(ppf, width, offset);
          i = next;
          break;
        }
        case '<': {
          // parse_magic_size: "@<n>" else Scan_indic '<'
          std::size_t p = skip_spaces(fmt, j + 1);
          long size;
          std::size_t q = p;
          if (p < n && (std::isdigit(static_cast<unsigned char>(fmt[p])) || fmt[p] == '-') && parse_int(fmt, q, size)) {
            q = skip_spaces(fmt, q);
            if (q < n && fmt[q] == '>') {
              magic = size;
              i = q + 1;
              break;
            }
          }
          out_string("@");
          out_string("<");
          i = j + 1;
          break;
        }
        case '?': item(); pp_print_flush(ppf); i = j + 1; break;
        case '\n': item(); pp_force_newline(ppf); i = j + 1; break;
        case '.': item(); pp_print_newline(ppf); i = j + 1; break;
        case '@': out_string("@"); i = j + 1; break;
        case '%':
          if (j + 1 < n && fmt[j + 1] == '%') {
            out_string("%");
            i = j + 2;
          } else {
            out_string("@");
            i = j;
          }
          break;
        default:
          // Scan_indic c
          out_string("@");
          out_string(std::string(1, d));
          i = j + 1;
          break;
      }
      lit_start = i;
      continue;
    }
    if (c == '%') {
      flush_literal(i);
      std::size_t j = i + 1;
      if (j >= n) throw std::runtime_error("format_doc::fprintf: unexpected end of format");
      if (fmt[j] == '@') {  // %@: a literal '@'
        out_string("@");
        i = j + 1;
        lit_start = i;
        continue;
      }
      if (fmt[j] == '%') {
        out_string("%");
        i = j + 1;
        lit_start = i;
        continue;
      }
      if (fmt[j] == '!') {
        pp_print_flush(ppf);
        i = j + 1;
        lit_start = i;
        continue;
      }
      bool left = false, zeros = false, plus = false, space = false, hash = false;
      while (j < n && (fmt[j] == '-' || fmt[j] == '0' || fmt[j] == '+' || fmt[j] == ' ' || fmt[j] == '#')) {
        if (fmt[j] == '-') left = true;
        else if (fmt[j] == '0') zeros = true;
        else if (fmt[j] == '+') plus = true;
        else if (fmt[j] == ' ') space = true;
        else hash = true;
        ++j;
      }
      std::optional<long> width;
      if (j < n && fmt[j] == '*') {
        width = static_cast<long>(next_arg().i);
        ++j;
      } else {
        long w;
        std::size_t k = j;
        if (k < n && std::isdigit(static_cast<unsigned char>(fmt[k])) && parse_int(fmt, k, w)) width = w, j = k;
      }
      std::optional<long> prec;
      if (j < n && fmt[j] == '.') {
        ++j;
        if (j < n && fmt[j] == '*') {
          prec = static_cast<long>(next_arg().i);
          ++j;
        } else {
          long p = 0;
          std::size_t k = j;
          if (parse_int(fmt, k, p)) j = k;
          prec = p;
        }
      }
      if ((fmt[j] == 'l' || fmt[j] == 'L' || fmt[j] == 'n') && j + 1 < n &&
          std::string_view("diuxXo").find(fmt[j + 1]) != std::string_view::npos)
        ++j;
      char conv = fmt[j];
      (void)hash;
      auto padded = [&](std::string s) { return width ? pad(std::move(s), *width, left, zeros) : s; };
      switch (conv) {
        case 's': out_string(padded(std::string(next_arg().s))); break;
        case 'S': out_string(padded("\"" + format::string_escaped(next_arg().s) + "\"")); break;
        case 'c': out_string(std::string(1, next_arg().c)); break;
        case 'C': out_string(padded("'" + format::char_escaped(next_arg().c) + "'")); break;
        case 'd':
        case 'i': {
          long long v = next_arg().i;
          std::string s = std::to_string(v);
          if (v >= 0 && plus) s = "+" + s;
          else if (v >= 0 && space) s = " " + s;
          out_string(padded(s));
          break;
        }
        case 'u': out_string(padded(to_base(static_cast<unsigned long long>(next_arg().i), 10, false))); break;
        case 'x': out_string(padded(to_base(static_cast<unsigned long long>(next_arg().i), 16, false))); break;
        case 'X': out_string(padded(to_base(static_cast<unsigned long long>(next_arg().i), 16, true))); break;
        case 'o': out_string(padded(to_base(static_cast<unsigned long long>(next_arg().i), 8, false))); break;
        case 'B': out_string(padded(next_arg().i ? "true" : "false")); break;
        case 'a':
        case 't': {
          item();
          const Arg& a = next_arg();
          a.fn(ppf);
          break;
        }
        default:
          throw std::runtime_error(std::string("format_doc::fprintf: unsupported conversion %") + conv);
      }
      i = j + 1;
      lit_start = i;
      continue;
    }
    ++i;
  }
  flush_literal(n);
  if (argi != args.size()) throw std::runtime_error("format_doc::fprintf: too many arguments");
}

// ---- align_prefix -------------------------------------------------------------------

namespace {

struct RalignSplit {
  long close_pos;
  std::vector<Element> before, mid, after;
};

// approx_len: the printed width of a break-free prefix, None otherwise
std::optional<long> approx_len(long acc, const std::vector<Element>& l) {
  for (std::size_t i = 0; i < l.size(); ++i) {
    const Element& e = l[i];
    switch (e.k) {
      case Element::K::Text: acc += format::utf_8_scalar_width(e.text); break;
      case Element::K::With_size:
        if (i + 1 < l.size() && l[i + 1].k == Element::K::Text) {
          acc += e.a;
          ++i;
        }
        break;
      case Element::K::Open_box:
      case Element::K::Close_box:
      case Element::K::Open_tag:
      case Element::K::Close_tag:
      case Element::K::Open_tbox:
      case Element::K::Close_tbox:
      case Element::K::Set_tab: break;
      default: return std::nullopt;
    }
  }
  return acc;
}

}  // namespace

std::vector<Doc> align_prefix(const std::vector<std::pair<Doc, long>>& l) {
  // split_ralign
  std::vector<std::optional<RalignSplit>> splits;
  for (const auto& [doc, shift] : l) {
    const auto& els = doc.els;
    std::size_t k = 0;
    RalignSplit r;
    while (k < els.size() && !(els[k].k == Element::K::Open_tag && els[k].text == "ralign")) r.before.push_back(els[k++]);
    if (k < els.size()) ++k;
    long opened = 0;
    while (k < els.size()) {
      const Element& e = els[k++];
      if (e.k == Element::K::Open_tag) {
        ++opened;
        r.mid.push_back(e);
      } else if (e.k == Element::K::Close_tag) {
        if (opened == 0) break;
        --opened;
        r.mid.push_back(e);
      } else {
        r.mid.push_back(e);
      }
    }
    while (k < els.size()) r.after.push_back(els[k++]);
    std::optional<long> len = approx_len(0, r.before);
    if (len) len = approx_len(*len, r.mid);
    if (!len) {
      splits.push_back(std::nullopt);
      continue;
    }
    r.close_pos = shift + *len;
    splits.push_back(std::move(r));
  }
  long max_pos = 0;
  for (auto& r : splits)
    if (r) max_pos = std::max(max_pos, r->close_pos);
  std::vector<Doc> out;
  for (std::size_t i = 0; i < l.size(); ++i) {
    if (!splits[i]) {
      out.push_back(l[i].first);
      continue;
    }
    const RalignSplit& r = *splits[i];
    // align_doc: before, the tag, the padding, mid, the close, after
    Doc d;
    d.els = r.before;
    Element ot = el(Element::K::Open_tag);
    ot.text = "ralign";
    d.els.push_back(std::move(ot));
    if (r.close_pos < max_pos) {
      Element t = el(Element::K::Text);
      t.text = std::string(static_cast<std::size_t>(max_pos - r.close_pos), ' ');
      d.els.push_back(std::move(t));
    }
    d.els.insert(d.els.end(), r.mid.begin(), r.mid.end());
    d.els.push_back(el(Element::K::Close_tag));
    d.els.insert(d.els.end(), r.after.begin(), r.after.end());
    out.push_back(std::move(d));
  }
  return out;
}

std::pair<Doc, Doc> align_prefix2(const std::pair<Doc, long>& x, const std::pair<Doc, long>& y) {
  auto r = align_prefix({x, y});
  return {r[0], r[1]};
}

}  // namespace cppcaml::typing::format_doc
