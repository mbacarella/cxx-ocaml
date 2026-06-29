#include "cppcaml/lambda.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstdlib>

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <variant>

#include "cppcaml/cmi.hpp"
#include "cppcaml/infer_check.hpp"

namespace cppcaml::lambda {
namespace {
using namespace ast;

std::string lid_last(const Longident& x) {
  if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
  if (auto* p = std::get_if<Ldot>(&x.v)) return p->name;
  return "?";
}

// The outermost component of a path (`A.B.x` -> "A"); "" for an Lapply head.
std::string lid_head(const Longident& x) {
  const Longident* p = &x;
  while (auto* d = std::get_if<Ldot>(&p->v)) p = d->prefix.get();
  if (auto* l = std::get_if<Lident>(&p->v)) return l->name;
  return "";
}

// ---- Oppen-style pretty-printer matching OCaml's Format (hov/hv boxes) ----
// Doc = Text | Break(sep) | Box(type, offset, children).  An hov box packs
// (fill-and-wrap, like @[<2>); an hv box is all-or-none (like @[<hv 1>); a break
// indents to the box's open-column + offset (column-relative, as Format does).
struct Doc;
using DocP = std::shared_ptr<Doc>;
enum class BoxT { Box, Hov, Hv, H, V };  // Box = @[<n>] (the printlambda default)
struct Doc {
  enum T { Text, Break, Box } t;
  std::string s;            // Text content / Break separator
  BoxT bt = BoxT::Hov;      // Box
  int off = 0;              // Box offset
  std::vector<DocP> ch;     // Box children
  int bsize = 0;            // Break: Oppen size (text to next break, nesting-aware)
};
DocP text(std::string s) { auto d = std::make_shared<Doc>(); d->t = Doc::Text; d->s = std::move(s); return d; }
DocP brk(std::string sep = " ") { auto d = std::make_shared<Doc>(); d->t = Doc::Break; d->s = std::move(sep); return d; }
// A break carrying its own indent offset (Format's `@;<width off>`); a forced
// newline lands at box-open + box-offset + this offset.
DocP brk_off(std::string sep, int off) { auto d = brk(std::move(sep)); d->off = off; return d; }
DocP box(BoxT bt, int off, std::vector<DocP> ch) {
  auto d = std::make_shared<Doc>(); d->t = Doc::Box; d->bt = bt; d->off = off; d->ch = std::move(ch); return d;
}

// OCaml's pretty-printer wraps a box whose flat width would reach 78 columns, so
// the effective fit threshold (pp_space_left at column 0) is 77.
constexpr int MARGIN = 77;

int flatw(const DocP& d) {
  if (d->t == Doc::Text || d->t == Doc::Break) return (int)d->s.size();
  int w = 0; for (auto& c : d->ch) w += flatw(c); return w;
}

// Compute each break's Oppen "size" = the width of the material following it up
// to the next break AT THE SAME BOX LEVEL (a nested box counts as its full flat
// width; its own breaks are not boundaries for the outer break).  `tail` is the
// size to attribute to content that runs off the end of this box (the parent's
// continuation), so an outer break sees the whole nested content -- which is why
// `setglobal`'s break sees the entire (let ...) and breaks first.
void set_sizes(const DocP& b, int tail) {
  int n = (int)b->ch.size();
  auto run = [&](int from) {  // flat width from `from` to the next break (or end+tail)
    int s = 0; bool found = false;
    for (int j = from; j < n; ++j) {
      if (b->ch[j]->t == Doc::Break) { found = true; break; }
      s += flatw(b->ch[j]);
    }
    return s + (found ? 0 : tail);
  };
  for (int i = 0; i < n; ++i) {
    if (b->ch[i]->t == Doc::Break) b->ch[i]->bsize = run(i + 1);
    // A nested box's internal break sizes cap at that box's own close (Format
    // finalises a break's size at its box boundary), so its last break must NOT
    // inherit the parent's trailing close-parens -- pass tail 0, not run(i+1).
    else if (b->ch[i]->t == Doc::Box) set_sizes(b->ch[i], 0);
  }
}

// A faithful port of OCaml's Format break decisions (stdlib/format.ml).  On
// entering a box, if its flat width fits the remaining space it becomes "fits"
// (all breaks stay on the line); otherwise it keeps its declared type.  `@[<n>`
// is Pp_box (not hov): it breaks a hint when the next chunk overflows OR when
// the current line is already indented past the box's open column.
struct Render {
  std::ostream& out;
  int col = 0;
  int cur_indent = 0;   // indentation of the current line
  bool is_new_line = true;
  void emit(const std::string& s) { out << s; col += (int)s.size(); if (!s.empty()) is_new_line = false; }
  void newline(int indent) {
    out << '\n' << std::string(indent, ' ');
    col = indent; cur_indent = indent; is_new_line = true;
  }
  void go(const DocP& d) {
    if (d->t == Doc::Text || d->t == Doc::Break) { emit(d->s); return; }
    // Box: resolve to "fits" if the whole box fits the remaining width.
    bool fits = flatw(d) <= MARGIN - col;
    BoxT ty = d->bt;
    int box_col = col;               // column where this box opens (margin - width)
    int brk_indent = box_col + d->off;  // where a forced newline lands
    for (auto& c : d->ch) {
      if (c->t == Doc::Text) { emit(c->s); }
      else if (c->t == Doc::Box) { go(c); }
      else {  // Break (separator c->s, offset 0 for our `@ ` breaks)
        bool nl;
        if (ty == BoxT::V) nl = true;  // vbox is never collapsed to "fits"
        else if (fits) nl = false;
        else switch (ty) {
          case BoxT::H: nl = false; break;
          case BoxT::V: nl = true; break;
          case BoxT::Hv: nl = true; break;
          // A break's Oppen size includes its own blanks (c->s), not just the
          // following content -- so the threshold test counts them too.
          case BoxT::Hov: nl = c->bsize + (int)c->s.size() > MARGIN - col; break;
          default:  // Pp_box (the @[<n>] default): format.ml's Pp_box rule.
            // pp_current_indent here is the CURRENT LINE's indent (updated only
            // on a newline), not the running column -- so we test cur_indent.
            if (is_new_line) nl = false;
            else if (c->bsize + (int)c->s.size() > MARGIN - col) nl = true;
            else nl = cur_indent > brk_indent;  // > pp_margin - width + off
            break;
        }
        if (nl) newline(brk_indent + c->off);  // c->off: this break's own offset
        else emit(c->s);
      }
    }
  }
};

// Print the raw stamp (NOT normalized): the layout/line-breaking depends on the
// stamp's digit width, which must match OCaml's (its stamps are ~3 digits, like
// ours start at 300).  The harness normalizes both sides by first appearance for
// the byte comparison, so the actual values are irrelevant -- only widths matter.
struct Pr {
  std::string ident(const Ident& i) {
    // A compiler temp prints `*name*` (default `*match*` when unnamed, e.g. `*opt*`).
    std::string base = i.temp ? "*" + (i.name.empty() ? "match" : i.name) + "*" : i.name;
    return base + "/" + std::to_string(i.stamp);
  }
};

std::string kind_suffix(ValueKind k) {
  switch (k) {
    case ValueKind::Int: return "[int]";
    case ValueKind::Float: return "[float]";
    case ValueKind::Boxedint32: return "[int32]";
    case ValueKind::Boxedint64: return "[int64]";
    case ValueKind::Nativeint: return "[nativeint]";
    case ValueKind::Gen: return "";
  }
  return "";
}
std::string field_kind(ValueKind k) {
  switch (k) {
    case ValueKind::Int: return "int";
    case ValueKind::Float: return "float";
    case ValueKind::Boxedint32: return "int32";
    case ValueKind::Boxedint64: return "int64";
    case ValueKind::Nativeint: return "nativeint";
    case ValueKind::Gen: return "*";
  }
  return "*";
}
// block_shape (printlambda): omitted when empty or all-generic; otherwise
// " (k0,k1,...)".
std::string shape_suffix(const std::vector<ValueKind>& shape) {
  if (shape.empty()) return "";
  bool all_gen = true;
  for (auto k : shape) if (k != ValueKind::Gen) all_gen = false;
  if (all_gen) return "";
  std::string s = " (" + field_kind(shape[0]);
  for (size_t i = 1; i < shape.size(); ++i) s += "," + field_kind(shape[i]);
  return s + ")";
}
std::string ret_suffix(ValueKind k) {
  switch (k) {
    case ValueKind::Int: return ": int";
    case ValueKind::Float: return ": float";
    case ValueKind::Boxedint32: return ": int32";
    case ValueKind::Boxedint64: return ": int64";
    case ValueKind::Nativeint: return ": nativeint";
    case ValueKind::Gen: return "";
  }
  return "";
}

// Replicate OCaml's Char.escaped (printlambda prints a char constant as 'x').
std::string ocaml_char(int code) {
  unsigned char c = (unsigned char)code;
  switch (c) {
    case '\'': return "\\'";
    case '\\': return "\\\\";
    case '\n': return "\\n";
    case '\t': return "\\t";
    case '\r': return "\\r";
    case '\b': return "\\b";
    default:
      if (c >= ' ' && c <= '~') return std::string(1, (char)c);
      char b[5]; std::snprintf(b, sizeof b, "\\%03d", c); return b;
  }
}

// Replicate OCaml's String.escaped (used by printlambda for string constants).
std::string ocaml_escape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      case '\b': o += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') o += (char)c;
        else { char b[5]; std::snprintf(b, sizeof b, "\\%03d", c); o += b; }
    }
  }
  return o;
}

DocP to_doc(const LamPtr& l, Pr& pr);

// @[<2>(let@ @[<hv 1>(@[<2>id =vk@ val@] @ ...)@]@ body)@]
// Like printlambda's letbody loop, consecutive lets merge into one group:
// nested Let bodies are flattened into the binding list.
DocP let_doc(const LamPtr& l, Pr& pr) {
  std::vector<DocP> binds{text("(")};
  LamPtr cur = l;
  bool first = true;
  while (true) {
    for (auto& b : cur->bindings) {
      if (!first) binds.push_back(brk());
      first = false;
      binds.push_back(box(BoxT::Box, 2,
          {text(pr.ident(b.id) + " =" +
                std::string(b.mut ? "mut" : b.alias ? "a" : b.strict_opt ? "o" : "") + kind_suffix(b.kind)),
           brk(), to_doc(b.val, pr)}));
    }
    if (cur->body && cur->body->k == Lam::K::Let) cur = cur->body;
    else break;
  }
  binds.push_back(text(")"));
  DocP bindings = box(BoxT::Hv, 1, std::move(binds));
  return box(BoxT::Box, 2, {text("(let"), brk(), bindings, brk(), to_doc(cur->body, pr), text(")")});
}

// @[<2>(letrec@ (@[<hv 1>@[<2>id@ def@] @ ...@])@ body)@]  (no `=`, defs are functions)
DocP letrec_doc(const LamPtr& l, Pr& pr) {
  std::vector<DocP> binds;
  for (size_t i = 0; i < l->bindings.size(); ++i) {
    auto& b = l->bindings[i];
    if (i) binds.push_back(brk());
    binds.push_back(box(BoxT::Box, 2, {text(pr.ident(b.id)), brk(), to_doc(b.val, pr)}));
  }
  DocP bindings = box(BoxT::Hv, 1, std::move(binds));
  return box(BoxT::Box, 2, {text("(letrec"), brk(), text("("), bindings, text(")"),
                            brk(), to_doc(l->body, pr), text(")")});
}

DocP to_doc(const LamPtr& l, Pr& pr) {
  switch (l->k) {
    case Lam::K::Var: return text(pr.ident(l->var));
    case Lam::K::Mutvar: return text("*" + pr.ident(l->var));  // read a mutable local
    case Lam::K::Assign:  // (assign x e)
      return box(BoxT::Box, 2, {text("(assign " + pr.ident(l->var)), brk(),
                                to_doc(l->cond, pr), text(")")});
    case Lam::K::ConstInt: return text(std::to_string(l->int_val) + l->str_val);
    case Lam::K::ConstChar: return text("'" + ocaml_char((int)l->int_val) + "'");
    case Lam::K::ConstFloat: return text(l->str_val);
    case Lam::K::ConstString: return text("\"" + ocaml_escape(l->str_val) + "\"");
    case Lam::K::ConstBlock: {  // struct_const: [tag] or [tag: f1 f2 ...]
      std::string tag = std::to_string(l->prim_arg);
      if (l->args.empty()) return text("[" + tag + "]");
      std::vector<DocP> fs;
      for (size_t i = 0; i < l->args.size(); ++i) { if (i) fs.push_back(brk()); fs.push_back(to_doc(l->args[i], pr)); }
      return box(BoxT::Box, 1, {text("[" + tag + ":"), brk(), box(BoxT::Box, 0, std::move(fs)), text("]")});
    }
    case Lam::K::Let: return let_doc(l, pr);
    case Lam::K::Letrec: return letrec_doc(l, pr);
    case Lam::K::Prim: {
      std::string head;
      switch (l->prim) {
        case Prim::Setglobal: head = "(setglobal " + l->prim_id + "!"; break;
        case Prim::Makeblock:
          head = "(makeblock " + std::to_string(l->prim_arg) + shape_suffix(l->blk_shape); break;
        case Prim::Field: head = "(field " + std::to_string(l->prim_arg); break;
        case Prim::FieldImm: head = "(field_imm " + std::to_string(l->prim_arg); break;
        case Prim::Floatfield: head = "(floatfield " + std::to_string(l->prim_arg); break;
        case Prim::SetFloatfield:
          head = "(setfloatfield " + std::to_string(l->prim_arg); break;
        case Prim::Global:
          return text("(global " + l->prim_id +
                      (l->var.stamp ? "/" + std::to_string(l->var.stamp) : "") + "!)");
        case Prim::Addint: head = "(+"; break;
        case Prim::Subint: head = "(-"; break;
        case Prim::Mulint: head = "(*"; break;
        case Prim::NotEqInt: head = "(!="; break;
        case Prim::EqInt: head = "(=="; break;
        case Prim::Makemutable:
          head = "(makemutable " + std::to_string(l->prim_arg) + shape_suffix(l->blk_shape); break;
        case Prim::FieldInt: head = "(field_int " + std::to_string(l->prim_arg); break;
        case Prim::FieldMut: head = "(field_mut " + std::to_string(l->prim_arg); break;
        case Prim::SetfieldImm: head = "(setfield_imm " + std::to_string(l->prim_arg); break;
        case Prim::SetfieldPtr: head = "(setfield_ptr " + std::to_string(l->prim_arg); break;
        case Prim::Offsetref: head = "(+:=" + std::to_string(l->prim_arg); break;
        case Prim::Offsetint: head = "(" + std::to_string(l->prim_arg) + "+"; break;
        case Prim::Ccall: head = "(" + l->prim_id; break;
        case Prim::IntCmp: head = "(" + l->prim_id; break;
        case Prim::Raise: head = "(raise"; break;
        case Prim::Reraise: head = "(reraise"; break;
        case Prim::Makelazyblock:
          head = l->prim_arg == 250 ? "(makeforwardblock" : "(makelazyblock"; break;
        case Prim::Send: head = "(" + (l->prim_id.empty() ? "send" : l->prim_id); break;
        case Prim::FieldComputed: head = "(field_computed"; break;
        case Prim::SetfieldComputed: head = "(" + l->prim_id; break;
      }
      std::vector<DocP> xs{text(head)};
      for (auto& a : l->args) { xs.push_back(brk()); xs.push_back(to_doc(a, pr)); }
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::Apply: {
      std::vector<DocP> xs{text("(apply"), brk(), to_doc(l->fn, pr)};
      for (auto& a : l->args) { xs.push_back(brk()); xs.push_back(to_doc(a, pr)); }
      if (!l->inline_attr.empty()) { xs.push_back(brk()); xs.push_back(text(l->inline_attr)); }
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::Function: {
      std::vector<DocP> xs{text("(function")};
      for (auto& [id, k] : l->params) { xs.push_back(brk()); xs.push_back(text(pr.ident(id) + kind_suffix(k))); }
      if (!l->inline_attr.empty()) { xs.push_back(brk()); xs.push_back(text(l->inline_attr)); }
      xs.push_back(brk());
      if (l->ret_kind != ValueKind::Gen) { xs.push_back(text(ret_suffix(l->ret_kind))); xs.push_back(brk()); }
      xs.push_back(to_doc(l->body, pr));
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::IfThenElse:
      return box(BoxT::Box, 2, {text("(if"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->then_, pr), brk(), to_doc(l->else_, pr), text(")")});
    case Lam::K::Sequence: {  // flatten right-nested seqs: (seq e1 e2 ... en)
      std::vector<DocP> xs{text("(seq")};
      LamPtr cur = l;
      while (true) {
        xs.push_back(brk()); xs.push_back(to_doc(cur->cond, pr));
        if (cur->else_ && cur->else_->k == Lam::K::Sequence) cur = cur->else_;
        else { xs.push_back(brk()); xs.push_back(to_doc(cur->else_, pr)); break; }
      }
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::While:
      return box(BoxT::Box, 2, {text("(while"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->body, pr), text(")")});
    case Lam::K::For:
      return box(BoxT::Box, 2, {text("(for " + pr.ident(l->var)), brk(),
                                to_doc(l->then_, pr), brk(),
                                text(l->downto_ ? "downto" : "to"), brk(),
                                to_doc(l->else_, pr), brk(), to_doc(l->body, pr), text(")")});
    case Lam::K::Try:  // @[<2>(try@ body@;<1 -1>with exn@ handler)@]
      return box(BoxT::Box, 2, {text("(try"), brk(), to_doc(l->body, pr),
                                brk_off(" ", -1), text("with " + pr.ident(l->var)),
                                brk(), to_doc(l->then_, pr), text(")")});
    case Lam::K::Catch: {  // @[<2>(catch@ body@;<1 -1>with (N vars)@ handler)@]
      std::string w = "with (" + std::to_string(l->prim_arg);
      for (size_t vi = 0; vi < l->catch_vars.size(); ++vi)
        w += " " + pr.ident(l->catch_vars[vi]) +
             (vi < l->catch_var_kinds.size() ? kind_suffix(l->catch_var_kinds[vi])
                                             : "");
      w += ")";
      return box(BoxT::Box, 2, {text("(catch"), brk(), to_doc(l->cond, pr),
                                brk_off(" ", -1), text(w),
                                brk(), to_doc(l->then_, pr), text(")")});
    }
    case Lam::K::Staticraise: {  // @[<2>(exit@ N args)@]
      std::vector<DocP> ds{text("(exit"), brk(), text(std::to_string(l->prim_arg))};
      for (auto& a : l->args) { ds.push_back(brk()); ds.push_back(to_doc(a, pr)); }
      ds.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(ds));
    }
    case Lam::K::Switch: {
      // @[<1>(switch* larg@ @[<v 0> @[<hv 1>case int N:@ body@] @ ... @])@]
      std::vector<DocP> cases;
      bool spc = false;
      auto add = [&](const std::string& kind, int tag, const LamPtr& body) {
        if (spc) cases.push_back(brk()); else spc = true;
        cases.push_back(box(BoxT::Hv, 1,
            {text("case " + kind + " " + std::to_string(tag) + ":"), brk(), to_doc(body, pr)}));
      };
      for (auto& c : l->sw_consts) add("int", c.tag, c.body);
      for (auto& c : l->sw_blocks) add("tag", c.tag, c.body);
      if (l->sw_default) {
        if (spc) cases.push_back(brk()); else spc = true;
        cases.push_back(box(BoxT::Hv, 1, {text("default:"), brk(), to_doc(l->sw_default, pr)}));
      }
      std::string head = l->sw_default ? "(switch " : "(switch* ";
      return box(BoxT::Box, 1, {text(head), to_doc(l->cond, pr), brk(),
                                box(BoxT::V, 0, std::move(cases)), text(")")});
    }
  }
  return text("?");
}

// ---- translation (parsetree -> Lambda), slice 1 ----
int stamp_counter = 300;  // arbitrary; normalized on print

LamPtr mk(Lam::K k) { auto l = std::make_shared<Lam>(); l->k = k; return l; }

ValueKind vkind(const std::string& s) {
  if (s == "int") return ValueKind::Int;
  if (s == "float") return ValueKind::Float;
  if (s == "int32") return ValueKind::Boxedint32;
  if (s == "int64") return ValueKind::Boxedint64;
  if (s == "nativeint") return ValueKind::Nativeint;
  return ValueKind::Gen;
}

// Parse an OCaml integer literal: decimal/0x/0o/0b, `_` separators, full
// unsigned range with two's-complement wraparound (as the compiler does).
long long parse_ocaml_int(const std::string& s) {
  std::string t;
  for (char c : s) if (c != '_') t += c;
  size_t i = 0; bool neg = false;
  if (i < t.size() && (t[i] == '-' || t[i] == '+')) { neg = t[i] == '-'; ++i; }
  int base = 10;
  if (i + 1 < t.size() && t[i] == '0') {
    char b = t[i + 1];
    if (b == 'x' || b == 'X') { base = 16; i += 2; }
    else if (b == 'o' || b == 'O') { base = 8; i += 2; }
    else if (b == 'b' || b == 'B') { base = 2; i += 2; }
  }
  unsigned long long v = 0;
  try { v = std::stoull(t.substr(i), nullptr, base); } catch (...) {}
  long long r = static_cast<long long>(v);
  return neg ? -r : r;
}

LamPtr translate_const(const Constant& c) {
  if (auto* i = std::get_if<Pconst_integer>(&c.desc)) {
    auto l = mk(Lam::K::ConstInt); l->int_val = parse_ocaml_int(i->value);
    if (i->suffix) {
      l->str_val = std::string(1, *i->suffix);  // 42L / 42l / 42n
      // an int32 literal wraps to 32-bit two's complement (0xf0f0f0f0l < 0)
      if (*i->suffix == 'l') l->int_val = (long long)(std::int32_t)l->int_val;
    }
    return l;
  }
  if (auto* ch = std::get_if<Pconst_char>(&c.desc)) {  // prints as 'x', value is its byte
    auto l = mk(Lam::K::ConstChar); l->int_val = ch->code; return l;
  }
  if (auto* f = std::get_if<Pconst_float>(&c.desc)) {
    auto l = mk(Lam::K::ConstFloat); l->str_val = f->value; return l;
  }
  if (auto* s = std::get_if<Pconst_string>(&c.desc)) {
    auto l = mk(Lam::K::ConstString); l->str_val = s->s; return l;
  }
  return mk(Lam::K::ConstInt);
}

// The translator: holds the inferred value kinds and a value-variable scope so
// references resolve to their binder's stamp.
struct Translator {
  ValueKinds vk;
  int stamp = 300;  // arbitrary; normalized on print
  // True while translating the tail spine of a `let rec` RHS: suppresses the
  // `let x = E in x -> E` collapse there, because ocamlc only runs that
  // simplification (Simplif) *after* value_rec compilation -- partition_rec
  // must see the un-collapsed spine (and collapses it again afterwards).
  bool rec_spine_ = false;
  std::string stdlib_dir = "stdlib";  // where to find stdlib*.cmi (CWD-relative by default)
  std::vector<std::string> module_dirs_;  // extra -I dirs to find local module .cmi
  // Path to a module's .cmi: the stdlib naming pattern under stdlib_dir, else (for
  // a separately-compiled local module like `A`) the first `<a>.cmi`/`A.cmi`
  // found in the -I search dirs.  Enables cross-module separate compilation.
  std::string resolve_cmi(const std::string& mod) const {
    std::string sp;
    if (mod == "Stdlib") sp = stdlib_dir + "/stdlib.cmi";
    else if (mod.rfind("Camlinternal", 0) == 0)
      sp = stdlib_dir + "/" + (char)std::tolower((unsigned char)mod[0]) + mod.substr(1) + ".cmi";
    else sp = stdlib_dir + "/stdlib__" + mod + ".cmi";
    if (std::filesystem::exists(sp)) return sp;
    std::string low = (char)std::tolower((unsigned char)mod[0]) + mod.substr(1);
    for (const std::string& d : module_dirs_) {
      if (std::filesystem::exists(d + "/" + low + ".cmi")) return d + "/" + low + ".cmi";
      if (std::filesystem::exists(d + "/" + mod + ".cmi")) return d + "/" + mod + ".cmi";
    }
    return sp;  // not found: keep the stdlib path (load throws -> caller handles)
  }
  std::vector<std::unordered_map<std::string, Ident>> scope{{}};
  std::unordered_map<std::string, int> stdlib_fields;  // Stdlib value -> field index
  struct StdPrim { std::string name; int arity; };  // an external's prim_name + arity
  std::unordered_map<std::string, StdPrim> stdlib_prims;  // Stdlib value -> prim
  // A labeled/optional function's parameter signature: (label kind 0/1/2, name)
  // per parameter, keyed by the binder stamp -- used to match a call's arguments.
  using FnSig = std::vector<std::pair<int, std::string>>;
  std::map<int, FnSig> fn_sig_;
  // labelled value signatures of a functor parameter's signature (`H : SeededS`
  // -> create's `?random`), so `H.create 51` fills the omitted optional: param
  // name -> value name -> FnSig.
  std::unordered_map<std::string, std::unordered_map<std::string, FnSig>> param_value_sigs_;
  // A function's first-class-module parameters (`(module P : S)`): per
  // positional param the package module type name ("" if not one), so call
  // sites coerce un-annotated `(module M)` arguments to S's layout.
  std::unordered_map<int, std::vector<std::string>> fn_pack_params_;
  // module name ("List", "Printf", ...) -> its value -> field index, cached.
  std::unordered_map<std::string, std::unordered_map<std::string, int>> mod_fields;
  // module name -> its value -> external prim (%builtin or C name) + arity, cached.
  std::unordered_map<std::string, std::unordered_map<std::string, StdPrim>> mod_prims;
  std::unordered_map<std::string, std::string> prim_to_mod_;  // C prim -> declaring module
  // Modules required (linked) because a C external of theirs is used in `l`.
  void collect_required_globals(const LamPtr& l, std::set<std::string>& out) {
    if (!l) return;
    if (l->k == Lam::K::Prim && l->prim == Prim::Ccall)
      if (auto it = prim_to_mod_.find(l->prim_id); it != prim_to_mod_.end())
        out.insert(global_of(it->second));
    collect_required_globals(l->fn, out); collect_required_globals(l->body, out);
    collect_required_globals(l->cond, out); collect_required_globals(l->then_, out);
    collect_required_globals(l->else_, out); collect_required_globals(l->sw_default, out);
    for (auto& a : l->args) collect_required_globals(a, out);
    for (auto& b : l->bindings) collect_required_globals(b.val, out);
    for (auto& sc : l->sw_consts) collect_required_globals(sc.body, out);
    for (auto& sc : l->sw_blocks) collect_required_globals(sc.body, out);
  }
  // Locally-defined submodules: name -> its binder, and name -> field layout
  // (export value/submodule name -> field index), for resolving `M.x`.
  std::unordered_map<std::string, Ident> module_ident_;
  // A module alias `module F = M.Sub` resolves F to a pure path expression
  // (`(field_imm i M)`) inlined at use sites, instead of a fresh binding.
  std::unordered_map<std::string, LamPtr> module_alias_;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> module_layout_;
  // module_layout_ is also keyed by DOTTED paths ("X.M") for submodules
  // reachable from a binding, so alias chains and deep member access resolve.
  // A local functor's result field layout, so `Make(Arg).foo` resolves.
  // (Also keyed by dotted paths for functor members of first-class modules.)
  std::unordered_map<std::string, std::vector<std::string>> functor_result_;
  // A local functor's parameter signature layout, to coerce its argument.
  std::unordered_map<std::string, std::vector<std::string>> functor_param_;
  // The package type of an enclosing `(val e : (module S))` unpack: a `(module
  // M)` inside e with no explicit `: S` is coerced to S's layout (the match arms
  // in targetint's `(val (match.. (module Int32)|(module Int64)) : S)`).
  std::string unpack_pkg_;
  // A local module bound to a stdlib functor application (`module Subst =
  // Map.Make(..)` -> {"Map","Make"}), so `Subst.fold ~init ~f` can recover the
  // labelled signature of the functor RESULT's value for call-site reordering.
  std::unordered_map<std::string, std::pair<std::string, std::string>> module_functor_src_;
  // module type S = <mt>: the signature AST (the Structure outlives
  // translation), for layouts of nested members of first-class modules.
  std::unordered_map<std::string, const ModuleType*> modtype_ast_;
  // let x = (module .. : S): x's package module type name, so
  // `module X = (val x)` knows X's signature.
  std::unordered_map<std::string, std::string> pack_modtype_;
  // Synthetic names for generalized opens (`open F(X)`), un-spellable in source.
  int open_gen_count_ = 0;
  // A named module type's value layout, so `module F (X : S)` knows X's fields.
  std::unordered_map<std::string, std::vector<std::string>> modtype_layout_;
  // Modules brought into scope by `open M` / `M.(e)` (innermost last), so an
  // unqualified name resolves as `M.x` (a stdlib field or an external prim).
  std::vector<std::string> opened_;
  // Object-method translation state.  Inside a method body `cur_self_` is the
  // method's self parameter and `inst_vars_` maps each instance-variable name to
  // its var-id binder (so `n` -> (field_computed self n) and `n<-e` ->
  // (setfield_*_computed self n e)).  Empty outside an object.
  std::optional<Ident> cur_self_;
  std::unordered_map<std::string, Ident> inst_vars_;
  // Scope-frame index of a method body's own bindings: frames at or above this
  // are method-local (params / lets) and shadow instance variables; everything
  // below is the class-creation / module scope, which instance variables shadow.
  // (An instance var that merely shares a name with a module-level value -- e.g.
  // a class-let `let lambda = new lambda_ops ops` shadowing a global `lambda` --
  // must resolve to the per-object field, not the global.)
  size_t method_scope_floor_ = 0;
  // Method-label binders of the current object, so a self-send `self#m` lowers to
  // (sendself self m) with the bound label rather than a public (send self tag).
  std::unordered_map<std::string, Ident> cur_meth_id_;
  // `inherit parent as super`: super#m applies the parent's method closure
  // (bound from the inherits result) to self directly.
  std::string cur_super_name_;
  std::unordered_map<std::string, Ident> cur_super_mid_;
  // Each object/class definition gets a 1-based index; its methods' self parameter
  // is named `self-<index>` (Translclass's enter_class_definition depth).
  int obj_counter_ = 0;
  // Stamps of class-value bindings (`class c = ...`): `new c` reads obj_init at
  // field 0 of the class 3-tuple.
  std::set<int> class_ids_;
  // Per-class shape metadata for inheritance: instance-variable names (parent's
  // first, declaration order), all method names, and the virtual/concrete method
  // split (name-ascending) -- the arrays a child passes to CamlinternalOO.inherits.
  struct ClassMeta { std::vector<std::string> vals, meths, virt, concr; };
  std::unordered_map<std::string, ClassMeta> class_meta_;
  // Shared method/variable-name constant blocks hoisted to module top (Translobj's
  // `share`): a const-block key -> its `shared` binder, in creation order.  Wrapped
  // around the module body as `=a` bindings; single-use ones inline away (Simplif).
  std::vector<std::pair<std::string, Lam::Binding>> shared_consts_;
  std::unordered_map<std::string, Ident> shared_index_;
  // The exception binders of the enclosing try/with handlers; `raise` of the
  // innermost caught exception is a `reraise`.
  std::vector<Ident> caught_exn_;
  // User C externals: value name -> C primitive name (the `external f = "cname"`
  // string) + declared arity.  Applying one emits (cname args).
  std::unordered_map<std::string, StdPrim> externals_;
  // Locally-declared %-builtins (`external f : t -> u = "%bswap16"`): name ->
  // (primitive, arity), routed through prim_to_lam at application sites.
  std::unordered_map<std::string, std::pair<std::string, int>> local_prims_;
  // Locally-declared exceptions: name -> its binder (the makeblock-248 value).
  std::unordered_map<std::string, Ident> exn_ident_;
  // A submodule's exported exception/extension ctor: (module binder, field
  // index) -- the inner binder is out of scope outside the module.
  std::unordered_map<std::string, std::pair<Ident, int>> exn_field_;
  // Declared argument count of an exception constructor (`exception E of int *
  // string` has two).  Data-carrying predef exceptions all take one argument
  // (Failure of string; Assert_failure of a string*int*int tuple), the default.
  std::unordered_map<std::string, int> exn_arity_;
  // Inline-record exception labels in declaration order (`exception Apply_error
  // of {loc; env; ..}`): the fields are FLAT in the exn block at offsets 1.. ,
  // so a `{loc; env}` pattern binds each label to field (1 + its decl index).
  std::unordered_map<std::string, std::vector<std::string>> exn_rlabels_;
  std::string mod_path_;  // dotted module path prefix for exception names
  std::string file_name_;  // source path, for Match_failure/Assert_failure locations
  std::string unit_name_;  // the compilation unit (top module) name, for __MODULE__
  std::vector<std::string> func_path_;  // enclosing function-binding names, for __FUNCTION__
  std::set<const ast::Pexp_function*> named_funcs_;  // functions already named by a let binding
  // Predefined exception globals (Match_failure/Assert_failure): a stable stamp per
  // name so the dump's first-appearance normalization is consistent within a file.
  std::unordered_map<std::string, int> predef_global_stamp_;
  int next_exit_ = 0;  // static-exception ids (normalized in the dump, so value is free)

  // ===== The Switcher: a faithful port of lambda/switch.ml + matching.ml's
  // as_interval/call_switcher glue (see switcher_match below).  State that the
  // recursive cost optimizer threads: ok_inter (interval tests allowed only when
  // the matched values fit in [-2^16,2^16]) and a memo over case-array shapes.
  bool sw_ok_inter_ = false;
  struct Ctests { long long n = 0, ni = 0; };
  // A representation of a switch over intervals: each entry (lo,hi,act) covers a
  // contiguous run of inputs mapping to action index `act` (0 = the default).
  struct SwCase { long long lo, hi; int act; };
  struct SwCtx { long long off; LamPtr arg; };
  using ActFn = std::function<LamPtr(const SwCtx&)>;
  // t_ret: Inter(i,j) interval test, Sep(i) `x < bound`, No no test needed.
  enum class TR { No, Sep, Inter };
  struct TRet { TR k; long long i = 0, j = 0; };
  using OptRes = std::pair<TRet, std::pair<Ctests, Ctests>>;  // (tactic,(cm,ci))
  std::unordered_map<std::string, OptRes> sw_memo_;
  static constexpr int kDefaultLeaf = INT_MIN;  // sentinel exit-id for the default action

  static bool is_predef_exn_name(const std::string& n) {
    static const std::set<std::string> s = {
        "Out_of_memory", "Sys_error", "Failure", "Invalid_argument", "End_of_file",
        "Division_by_zero", "Not_found", "Match_failure", "Stack_overflow",
        "Sys_blocked_io", "Assert_failure", "Undefined_recursive_module", "Todo"};
    return s.count(n) != 0;
  }
  // `(global Name/stamp!)` for a predefined exception used by the compiler.  The
  // stamps are OCaml's fixed Predef ident stamps (the lambda dump normalizes them,
  // but -dinstr does not, so the bytecode getglobal needs the exact value).
  LamPtr predef_global(const std::string& name) {
    static const std::unordered_map<std::string, int> predef = {
      {"Match_failure", 23}, {"Assert_failure", 33}, {"Invalid_argument", 6},
      {"Failure", 4}, {"Not_found", 12}, {"Out_of_memory", 1}, {"Stack_overflow", 15},
      {"Sys_error", 3}, {"End_of_file", 9}, {"Division_by_zero", 10},
      {"Sys_blocked_io", 17}, {"Undefined_recursive_module", 35}, {"Todo", 36},
    };
    auto p = predef.find(name);
    int st = p != predef.end() ? p->second : (stamp++);
    auto g = mk(Lam::K::Prim); g->prim = Prim::Global; g->prim_id = name; g->var.stamp = st;
    return g;
  }
  // The `[0: "file" line char]` location block of a Match_failure/Assert_failure.
  LamPtr loc_block(const Location& loc) {
    return cblock(0, {cstr(file_name_), cint(loc.start.lnum),
                      cint(loc.start.cnum - loc.start.bol)});
  }
  std::string loc_string(const Location& l) {
    char buf[600];
    snprintf(buf, sizeof buf, "File \"%s\", line %d, characters %d-%d",
             file_name_.c_str(), l.start.lnum, l.start.cnum - l.start.bol,
             l.end.cnum - l.end.bol);
    return buf;
  }
  // The location ext-primitives (`__LOC__`/`__FILE__`/...): compile-time
  // constants.  `__FUNCTION__` is the module path plus the enclosing function
  // bindings.  Returns null if `name` is not one of them.
  LamPtr loc_primitive(const std::string& name, const Location& loc) {
    if (name == "__FILE__") return cstr(file_name_);
    if (name == "__LINE__") return cint(loc.start.lnum);
    if (name == "__LOC__") return cstr(loc_string(loc));
    if (name == "__MODULE__") return cstr(unit_name_);
    if (name == "__POS__")
      return cblock(0, {cstr(file_name_), cint(loc.start.lnum),
                        cint(loc.start.cnum - loc.start.bol),
                        cint(loc.end.cnum - loc.end.bol)});
    if (name == "__FUNCTION__") {
      std::string p = mod_path_;
      for (auto& f : func_path_) { if (!p.empty()) p += "."; p += f; }
      return cstr(p);
    }
    return nullptr;
  }
  // Compile a `let name = rhs` RHS, pushing `name` onto func_path_ while a
  // function RHS is compiled so `__FUNCTION__` inside it includes this binding.
  LamPtr fn_binding_rhs(const std::string& name, const Expression& rhs, const Attributes& attrs,
                        bool module_level = false) {
    // A binding whose VALUE is a function (directly, or after side-effecting
    // sequences / lets) takes the let-name for its `__FUNCTION__` scope.  BUT
    // only a DIRECT `let f = fun ..` names the function itself; when the function
    // is reached through a sequence/let (`let f = e1; fun ..`), the name covers
    // the side effects yet the function is anonymous (`f.(fun)`, like ocamlc).
    const Expression* rd = &rhs;
    while (auto* c = std::get_if<Pexp_constraint>(&rd->desc)) rd = c->e.get();
    auto* direct = std::get_if<Pexp_function>(&rd->desc);
    const Expression* ry = rd;
    for (;;) {
      if (auto* sq = std::get_if<Pexp_sequence>(&ry->desc)) { ry = sq->e2.get(); continue; }
      if (auto* le = std::get_if<Pexp_let>(&ry->desc)) { ry = le->body.get(); continue; }
      if (auto* c = std::get_if<Pexp_constraint>(&ry->desc)) { ry = c->e.get(); continue; }
      break;
    }
    bool yields = std::holds_alternative<Pexp_function>(ry->desc);
    // A module-level binding always names __FUNCTION__ in its RHS (even a plain
    // value `let r = .. __FUNCTION__ ..`); a local let only does so for a function.
    bool push = yields || module_level;
    // An operator binding `let (+@+) ..` prints as `(+@+)` in __FUNCTION__.
    std::string fname = (!name.empty() && !std::isalpha((unsigned char)name[0]) &&
                         name[0] != '_') ? "(" + name + ")" : name;
    if (push) func_path_.push_back(fname);
    if (direct) named_funcs_.insert(direct);
    LamPtr v = with_inline(expr(rhs), attrs);
    if (push) func_path_.pop_back();
    if (direct) named_funcs_.erase(direct);
    return v;
  }
  // `(raise (makeblock 0 (global Exn/s!) [0: file line char]))` for a compiler-
  // raised predefined exception (Match_failure / Assert_failure).
  LamPtr raise_predef(const std::string& exn, const Location& loc) {
    auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
    blk->args = {predef_global(exn), loc_block(loc)};
    auto r = mk(Lam::K::Prim); r->prim = Prim::Raise; r->args = {blk};
    return r;
  }

  // Locally-declared variant constructors: name -> {owning type, tag, is_block}.
  // Constant (nullary) and block (with-args) constructors are numbered
  // separately from 0 in declaration order, matching the runtime representation.
  struct CtorInfo {
    std::string type; int tag; bool is_block; int arity; bool unboxed = false;
    // inline record (`T of { pos : int }`): fields live directly in the
    // constructor block, in label order
    std::vector<std::string> rlabels;
    std::vector<ValueKind> rshape;
    std::vector<bool> rfmut;
    // The (dotted) type path of each inline-record field whose type is a named
    // record (`Pattern {penv : Pattern_env.t}` -> "Pattern_env.t"), else "".  Lets a
    // var bound to such a field resolve its own AMBIGUOUS labels by that type.
    std::vector<std::string> rftypes;
  };
  std::unordered_map<std::string, CtorInfo> ctor_info_;
  // A var bound to an inline-record field of NAMED RECORD type (`Pattern {penv}`,
  // penv : Pattern_env.t): its stamp -> that record's dotted type path, so a later
  // `penv.env` (env AMBIGUOUS) resolves through Pattern_env.t instead of find_field.
  std::unordered_map<int, std::string> var_record_path_;
  // A variable bound to a WHOLE inline record by `Ctor x` (ident.ml's `Local x`):
  // its var stamp -> that ctor's CtorInfo, so a later `x.field` reads the inline
  // record's field even when the label is AMBIGUOUS across constructors (Local/
  // Predef/Scoped all have `stamp`), where find_field bails.
  std::unordered_map<int, const CtorInfo*> inline_rec_var_;
  // Per-type constructor info, plus names defined by more than one type.  A
  // constructor name shared across types (morematch redefines `A|B|C..`) is
  // resolved in SOURCE ORDER: build_module re-registers each type's ambiguous
  // constructors when it reaches that `type` decl, so an earlier expression /
  // match sees the constructor of the type in scope at that point (OCaml
  // scoping), not the flat last-registered one.
  std::unordered_map<std::string, std::unordered_map<std::string, CtorInfo>> type_ctor_info_;
  std::set<std::string> ambiguous_ctors_;
  std::unordered_map<std::string, std::pair<int, int>> type_ctors_;  // type -> (n_const, n_block)
  std::set<std::string> immediate_local_;  // local all-constant variant type names
  std::set<std::string> gadt_types_;        // variant types with a GADT constructor

  // Polymorphic-variant type abbreviations (`type lambda = [ `Var | `App .. ]`):
  // map the type name to its directly-named tags and to the polyvariant types it
  // inherits (`[ a | b ]`), so a `#lambda` pattern can be expanded to the full
  // tag-hash set (with inheritance, resolved lazily by collect_pv_tags).
  std::unordered_map<std::string, std::vector<std::string>> pv_raw_tags_;
  std::unordered_map<std::string, std::vector<std::string>> pv_inherits_;
  void collect_pv_tags(const std::string& ty, std::set<long long>& out,
                       std::set<std::string>& seen) {
    if (!seen.insert(ty).second) return;
    auto it = pv_raw_tags_.find(ty);
    if (it != pv_raw_tags_.end())
      for (auto& t : it->second) out.insert(hash_variant(t));
    auto ii = pv_inherits_.find(ty);
    if (ii != pv_inherits_.end())
      for (auto& sub : ii->second) collect_pv_tags(sub, out, seen);
  }
  // Resolve a `#Mod.type` (or bare `#type`) pattern's tag set: try the QUALIFIED
  // key first ("Simple.view", registered for cross-module includes), else the bare
  // last component (a local abbreviation).
  void collect_pv_tags_lid(const Longident& id, std::set<long long>& out,
                           std::set<std::string>& seen) {
    std::string dotted;
    if (lid_to_dotted(id, dotted) && dotted.find('.') != std::string::npos &&
        pv_raw_tags_.count(dotted)) {
      collect_pv_tags(dotted, out, seen); return;
    }
    collect_pv_tags(lid_last(id), out, seen);
  }
  // Register the polymorphic-variant type abbreviations of an IMPORTED module path
  // (`Patterns.Simple`) under "<key_prefix>.<type>", so a `#key_prefix.type` pattern
  // resolves its tag set in a SEPARATE compilation where the type lives behind an
  // `include` (matching's `module Simple = struct include Patterns.Simple end` ->
  // `#Simple.view`; pv_raw_tags_ is otherwise only filled from local AST decls).
  void register_pv_types_from(const std::string& key_prefix, const std::string& dotted) {
    size_t dot = dotted.find('.');
    std::string head = dot == std::string::npos ? dotted : dotted.substr(0, dot);
    if (module_base(head)) return;  // a local module: its types are AST-visible
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t pos = dot; pos != std::string::npos;) {
        size_t nd = dotted.find('.', pos + 1);
        std::string comp = dotted.substr(pos + 1,
            nd == std::string::npos ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return;
        sig = next; pos = nd;
      }
      for (auto& td : sig->types) {
        cmi::TypePtr m = td.manifest;
        while (m && (m->kind == cmi::TypeExpr::Tlink || m->kind == cmi::TypeExpr::Tsubst))
          m = m->link;
        if (m && m->kind == cmi::TypeExpr::Tvariant && !m->pv_tags.empty()) {
          std::string key = key_prefix.empty() ? td.name : key_prefix + "." + td.name;
          if (!pv_raw_tags_.count(key)) pv_raw_tags_[key] = m->pv_tags;
        }
      }
    } catch (...) {}
  }

  // Locally-declared record fields: label -> {owning type, index, mutable, kind}.
  // Only UNAMBIGUOUS labels are usable (a label reused across records can't be
  // resolved without type direction, so it falls back to a generic translation).
  struct FieldInfo { std::string type; int index; bool mut; ValueKind kind; };
  std::unordered_map<std::string, FieldInfo> field_info_;
  std::set<std::string> ambiguous_fields_;
  // A record label -> the module-qualified ("Module.type") variant type of its VALUE,
  // for a field whose type is a constructor of a (single-component-qualified) module.
  // Lets a bare unresolved ctor field value (`{ vb_rec_kind = Dynamic }`, Dynamic's
  // module Value_rec_types un-opened) be resolved by type direction at construction.
  std::unordered_map<std::string, std::string> field_value_ctor_type_;
  // A record label -> the label signature of its FUNCTION-typed value, so a call
  // through a field projection (`core.value_descriptions ~loc ~direction env ..`)
  // reorders its labelled args to the parameter order, exactly as a directly-named
  // function does.  Without this the args pass verbatim and a `~direction` after a
  // positional `env` swaps the two (includemod's core_relation: every field is a
  // labelled `core_incl` arrow).
  std::unordered_map<std::string, FnSig> field_fn_sig_;
  // Per-type record fields + the labels source-order scoping has resolved at the
  // current point (so find_field uses the in-scope type's field despite the
  // label being ambiguous overall) -- the record analogue of the constructor
  // scoping (morematch: `x` in `type eber={x;y;z}` vs a later `type tg={v;x}`).
  std::unordered_map<std::string, std::unordered_map<std::string, FieldInfo>> type_field_info_;
  std::set<std::string> scoped_unambig_fields_;
  struct RecType { std::vector<std::string> labels; bool mut; std::vector<ValueKind> shape;
                   bool flat = false; };  // all-float: a flat float block, not a record
  std::unordered_map<std::string, RecType> rec_types_;  // type name -> record layout

  // The dotted type path of a core type that is a named-record constructor
  // (`Pattern_env.t`, `foo`), for type-directed field resolution of a var bound to
  // it.  Empty for builtins, type variables, arrows, tuples, etc.
  std::string coretype_record_path(const CoreType& t) {
    auto* c = std::get_if<Ptyp_constr>(&t.desc);
    if (!c) return "";
    std::string b = lid_last(c->id.txt);
    static const std::set<std::string> builtins = {
        "int","char","bool","unit","float","string","bytes","int32","int64",
        "nativeint","list","array","option","ref","exn","lazy_t"};
    if (builtins.count(b)) return "";
    std::string dotted;
    if (!lid_to_dotted(c->id.txt, dotted)) return "";
    return dotted;
  }
  // The value kind of a field/element from its syntactic core type (builtins and
  // immediate local variants; everything else is generic/boxed).
  ValueKind coretype_kind(const CoreType& t) {
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      std::string b = lid_last(c->id.txt);
      if (b == "int" || b == "char" || b == "bool" || b == "unit") return ValueKind::Int;
      if (b == "float") return ValueKind::Float;
      if (b == "int32") return ValueKind::Boxedint32;
      if (b == "int64") return ValueKind::Boxedint64;
      if (b == "nativeint") return ValueKind::Nativeint;
      if (immediate_local_.count(b)) return ValueKind::Int;
    }
    return ValueKind::Gen;
  }
  // Is this core type float, following local abbreviations (`type t = float;
  // type s = {f : t}` makes s a FLAT float record)?  Used only for the flat
  // decision, so coretype_kind's existing classifications stay untouched.
  std::unordered_map<std::string, const CoreType*> local_alias_;
  bool is_float_core(const CoreType& t, int depth = 0) {
    if (depth > 8) return false;
    if (auto* c = std::get_if<Ptyp_constr>(&t.desc)) {
      if (!c->args.empty()) return false;
      std::string b = lid_last(c->id.txt);
      if (b == "float") return true;
      if (auto a = local_alias_.find(b); a != local_alias_.end())
        return is_float_core(*a->second, depth + 1);
    }
    return false;
  }

  // Predefined variant constructors, so constructor matches over option/list/
  // result (and bool/unit) get the same tag info as local variants.
  // Constructor names that came from the predefs/stdlib (not local decls): a
  // LOCAL exception/extension ctor of the same name shadows them.
  std::set<std::string> builtin_ctors_;
  void register_predef_ctor_info() {
    for (const char* c : {"None", "Some", "[]", "::", "Ok", "Error", "false", "true", "()"})
      builtin_ctors_.insert(c);
    ctor_info_["None"]  = {"option", 0, false, 0};
    ctor_info_["Some"]  = {"option", 0, true, 1};
    ctor_info_["[]"]    = {"list", 0, false, 0};
    ctor_info_["::"]    = {"list", 0, true, 2};
    ctor_info_["Ok"]    = {"result", 0, true, 1};
    ctor_info_["Error"] = {"result", 1, true, 1};
    ctor_info_["false"] = {"bool", 0, false, 0};
    ctor_info_["true"]  = {"bool", 1, false, 0};
    ctor_info_["()"]    = {"unit", 0, false, 0};
    type_ctors_["option"] = {1, 1};
    type_ctors_["list"]   = {1, 1};
    type_ctors_["result"] = {0, 2};
    type_ctors_["bool"]   = {2, 0};
    type_ctors_["unit"]   = {1, 0};
  }

  // Register top-level Stdlib variant constructors (e.g. fpclass's FP_normal..)
  // from stdlib.cmi, so an unqualified use resolves to its tag instead of `?name`.
  // Skips names already known (predef wins) and names ambiguous across stdlib types.
  void register_stdlib_ctors() {
    try {
      auto cmi = cmi::CmiFile::load(stdlib_dir + "/stdlib.cmi");
      std::set<std::string> ambiguous;
      std::unordered_map<std::string, CtorInfo> found;
      for (auto& td : cmi.sig().types) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        bool gadt = false;
        for (auto& c : td.ctors) if (c.res) gadt = true;
        if (gadt) continue;  // GADT tag rules are subtler -- skip
        int nc = 0, nb = 0;
        for (auto& c : td.ctors) {
          bool block = !c.args.empty() || c.is_inline_record;
          int arity = c.is_inline_record ? 1 : (int)c.args.size();
          if (found.count(c.name) || ctor_info_.count(c.name)) ambiguous.insert(c.name);
          found[c.name] = {td.name, block ? nb : nc, block, arity};
          if (block) ++nb; else ++nc;
        }
        if (!gadt) type_ctors_.emplace(td.name, std::make_pair(nc, nb));
      }
      for (auto& [name, ci] : found)
        if (!ambiguous.count(name) && !ctor_info_.count(name)) {
          ctor_info_[name] = ci;
          builtin_ctors_.insert(name);
        }
    } catch (...) {}
  }

  // A stdlib module's variant constructors, loaded on demand from its cmi
  // (Arg.spec's Unit/Set/String/...): name -> CtorInfo per module.
  std::unordered_map<std::string, std::unordered_map<std::string, CtorInfo>> mod_ctor_cache_;
  const std::unordered_map<std::string, CtorInfo>& module_ctors(const std::string& mod) {
    if (auto it = mod_ctor_cache_.find(mod); it != mod_ctor_cache_.end()) return it->second;
    auto& out = mod_ctor_cache_[mod];
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.sig().types) {
        if (td.kind != cmi::TypeDecl::Variant) continue;
        // GADT constructors follow the same constant/block tag rules (Bigarray's
        // `kind`: Float32=0, Float64=1, ...), so they resolve like plain variants.
        int nc = 0, nb = 0;
        for (auto& c : td.ctors) {
          bool block = !c.args.empty() || c.is_inline_record;
          int arity = c.is_inline_record ? 1 : (int)c.args.size();
          if (!out.count(c.name)) {
            CtorInfo ci{td.name, block ? nb : nc, block, arity};
            // An INLINE-RECORD ctor of an imported module (Typedtree's
            // `Texp_record of {fields; representation; extended_expression}`):
            // carry its label order so `open Typedtree; match e with Texp_record
            // {fields; extended_expression; _}` resolves the labels via rlabels
            // (else they fall to find_field -> unresolved `?fields` -> garbage).
            if (c.is_inline_record)
              for (auto& l : c.inline_record) {
                ci.rlabels.push_back(l.name);
                ci.rshape.push_back(cmi_field_kind(l.type));
                ci.rfmut.push_back(l.mutable_);
                ci.rftypes.push_back("");
              }
            out[c.name] = std::move(ci);
          }
          if (block) ++nb; else ++nc;
        }
      }
    } catch (...) {}
    return out;
  }
  // Resolve a constructor through its explicit stdlib-module qualification, or
  // through the opened modules when bare.  Local modules take no part (their
  // ctors register through the normal paths).
  const CtorInfo* stdlib_module_ctor(const Longident& lid, const std::string& n) {
    std::vector<std::string> mods;
    if (auto* d = std::get_if<Ldot>(&lid.v)) {
      if (auto* pl = std::get_if<Lident>(&d->prefix->v))
        if (!module_base(pl->name)) mods.push_back(pl->name);
    } else {
      for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
        if (it->find('.') == std::string::npos && !module_base(*it))
          mods.push_back(*it);
    }
    for (auto& m : mods) {
      auto& mc = module_ctors(m);
      if (auto f = mc.find(n); f != mc.end()) return &f->second;
    }
    return nullptr;
  }

  // Register a qualified stdlib constructor's whole type into ctor_info_/type_ctors_
  // (e.g. a `Seq.Cons(x,_)` pattern needs Seq.node's Nil/Cons tags so the matcher
  // can bind x and decide exhaustiveness).  Idempotent; local modules are skipped.
  void register_qualified_ctor(const Longident& id) {
    std::string ctor_name, mod;
    if (auto* d = std::get_if<Ldot>(&id.v)) {  // `M.C` or `A.B.C`
      auto* pl = std::get_if<Lident>(&d->prefix->v);
      if (!pl) {
        // A NESTED-module qualifier (`Patterns.Head.Variant`): resolve the
        // submodule, which registers its variant constructors -- including
        // inline-record labels -- into ctor_info_ so the matcher binds the
        // fields instead of collapsing to `?tag`/`?cstr_row`.
        std::string prefix;
        if (lid_to_dotted(*d->prefix, prefix) &&
            prefix.find('.') != std::string::npos)
          submodule_of(prefix);
        return;
      }
      if (module_base(pl->name)) return;
      ctor_name = d->name; mod = pl->name;
    } else if (auto* l = std::get_if<Lident>(&id.v)) {  // bare `C` via an `open M`
      // A constructor brought into scope by `open CamlinternalFormatBasics` etc.
      // must register its whole type too, else the match compiler can't find the
      // type's constructor count (type_ctors_) and falls back to a degenerate
      // single-arm match (this is what broke fmtty_rel's GADT match in Printf).
      if (ctor_info_.count(l->name)) return;  // predef/local/already-known wins
      for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
        if (it->find('.') != std::string::npos || module_base(*it)) continue;
        if (module_ctors(*it).count(l->name)) { ctor_name = l->name; mod = *it; break; }
      }
      if (mod.empty()) return;
    } else return;
    if (ctor_info_.count(ctor_name)) return;
    auto& mc = module_ctors(mod);
    auto f = mc.find(ctor_name);
    if (f == mc.end()) return;
    const std::string ty = f->second.type;
    int nc = 0, nb = 0;
    for (auto& [nm, info] : mc) if (info.type == ty) (info.is_block ? nb : nc)++;
    for (auto& [nm, info] : mc)
      if (info.type == ty && !ctor_info_.count(nm)) {
        ctor_info_[nm] = info; builtin_ctors_.insert(nm);
      }
    type_ctors_.emplace(ty, std::make_pair(nc, nb));
  }
  // Register ONLY the explicitly-named constructor of a qualified `M.C`, NOT its
  // siblings.  A qualified CONSTRUCTION expression (`Longident.Lident "x"`) is
  // lowered directly via qualified_ctor_info and needs no sibling tags; eagerly
  // registering the whole type would let a sibling whose SHORT NAME collides with
  // another module's constructor (`Longident.Lapply` tag 2 vs `Lambda.Lapply`
  // tag 3) squat the flat ctor_info_ slot first, so a later `open Lambda` match
  // on `Lapply` reads the wrong tag and the arm is dropped -> Match_failure.
  void register_single_qualified_ctor(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl || module_base(pl->name)) return;
    if (ctor_info_.count(d->name)) return;
    auto& mc = module_ctors(pl->name);
    auto f = mc.find(d->name);
    if (f == mc.end()) return;
    ctor_info_[d->name] = f->second; builtin_ctors_.insert(d->name);
    // record the owning type's constant/block counts for tag/exhaustiveness tests
    int nc = 0, nb = 0;
    for (auto& [nm, info] : mc) if (info.type == f->second.type) (info.is_block ? nb : nc)++;
    type_ctors_.emplace(f->second.type, std::make_pair(nc, nb));
  }
  // The CtorInfo of a QUALIFIED variant constructor `M.C` from M's own type --
  // authoritative over a same-named LOCAL/bare ctor (typecore's `Env.Pattern`
  // -- constructor_usage.Pattern, tag 1 -- vs its local `wrong_kind_context.
  // Pattern`, tag 0).  Null for a local-module qualifier (handled elsewhere) or
  // an unknown ctor.  The returned pointer is stable (module_ctors is cached).
  const CtorInfo* qualified_ctor_info(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return nullptr;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl || module_base(pl->name)) return nullptr;
    auto& mc = module_ctors(pl->name);
    auto f = mc.find(d->name);
    return f == mc.end() ? nullptr : &f->second;
  }
  // Register ALL constructors of a module-qualified variant type ("Vmod.vis")
  // into ctor_info_/type_ctors_, given the type's full path from the inferencer.
  // Used for type-directed disambiguation of an unqualified constructor pattern
  // whose type lives in a module that is NOT opened (persistent_env matches
  // `Visible`/`Hidden` of `Load_path.visibility` with no `open Load_path`).
  void register_ctors_of_type(const std::string& path) {
    auto d = path.rfind('.');
    if (d == std::string::npos) return;
    std::string mod = path.substr(0, d), ty = path.substr(d + 1);
    if (mod.find('.') != std::string::npos) return;  // nested module: unhandled
    if (module_base(mod)) return;                     // a local module: skip
    auto& mc = module_ctors(mod);
    int nc = 0, nb = 0;
    bool found = false;
    for (auto& [nm, info] : mc) if (info.type == ty) { (info.is_block ? nb : nc)++; found = true; }
    if (!found) return;
    for (auto& [nm, info] : mc)
      if (info.type == ty && !ctor_info_.count(nm)) {
        ctor_info_[nm] = info; builtin_ctors_.insert(nm);
      }
    type_ctors_.emplace(ty, std::make_pair(nc, nb));
  }
  using CtorSave = std::vector<std::pair<std::string, std::optional<CtorInfo>>>;
  // FORCE-register an imported type's constructors with their cmi tag/arity,
  // OVERRIDING a wrong global resolution, scoped to one match.  `Named` is shared
  // by 6 types at arities 1/2/3; a `match (arg_opt : Parsetree.functor_parameter)
  // with Named (param, smty)` must use arity 2, not the last-registered arity 1.
  CtorSave force_register_type_ctors(const std::string& path) {
    CtorSave saved;
    auto d = path.rfind('.');
    if (d == std::string::npos) return saved;
    std::string mod = path.substr(0, d), ty = path.substr(d + 1);
    if (mod.find('.') != std::string::npos || module_base(mod)) return saved;
    auto& mc = module_ctors(mod);
    int nc = 0, nb = 0;
    bool any = false;
    for (auto& [nm, info] : mc) if (info.type == ty) { (info.is_block ? nb : nc)++; any = true; }
    if (!any) return saved;
    for (auto& [nm, info] : mc)
      if (info.type == ty) {
        auto it = ctor_info_.find(nm);
        if (it == ctor_info_.end() || it->second.type != ty ||
            it->second.arity != info.arity || it->second.tag != info.tag) {
          saved.emplace_back(nm, it == ctor_info_.end() ? std::optional<CtorInfo>()
                                                        : std::optional<CtorInfo>(it->second));
          ctor_info_[nm] = info;
        }
      }
    type_ctors_[ty] = std::make_pair(nc, nb);
    return saved;
  }
  // Register the type owning constructor `ctorname` from module `mod` (an imported
  // module, by name), so a BARE ctor whose module is neither opened nor qualified
  // resolves -- type-directed disambiguation by the applied function's module
  // (btype.ml's `Misc.protect_refs [R(..)] f`: `R` is Misc.ref_and_value's ctor,
  // never `open`ed).  No-op if mod is local / has no such ctor / it's already known.
  void register_module_ctor(const std::string& mod, const std::string& ctorname) {
    if (ctor_info_.count(ctorname) || mod.empty() || module_base(mod)) return;
    auto& mc = module_ctors(mod);
    auto f = mc.find(ctorname);
    if (f == mc.end()) return;
    register_ctors_of_type(mod + "." + f->second.type);
  }
  // Walk an expression for BARE unresolved constructors and register each from
  // `mod` (used at a `Mod.f arg` call site so ctors inside the args resolve).
  void scan_expr_ctors_from(const Expression& e0, const std::string& mod, int depth = 0) {
    if (depth > 6) return;
    if (auto* k = std::get_if<Pexp_construct>(&e0.desc)) {
      if (auto* l = std::get_if<Lident>(&k->id.txt.v)) register_module_ctor(mod, l->name);
      if (k->arg) scan_expr_ctors_from(**k->arg, mod, depth + 1);
    } else if (auto* t = std::get_if<Pexp_tuple>(&e0.desc)) {
      for (auto& el : t->elems) scan_expr_ctors_from(*el, mod, depth + 1);
    }
  }
  // Recursively register the qualified stdlib constructors named in a pattern,
  // so the match compiler resolves them like local/predef ones.
  void scan_pat_ctors(const Pattern& p) {
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) {
      register_qualified_ctor(k->id.txt);
      // Type-directed: an unqualified constructor still unresolved (its module is
      // not opened) -- register it through the pattern's inferred variant type.
      if (std::holds_alternative<Lident>(k->id.txt.v) &&
          !ctor_info_.count(lid_last(k->id.txt))) {
        auto it = vk.pat_constr.find(&p);
        if (it != vk.pat_constr.end()) register_ctors_of_type(it->second);
      }
      if (k->arg) { tag_ctor_record_args(lid_last(k->id.txt), k->arg->get()); scan_pat_ctors(**k->arg); }
    } else if (auto* t = std::get_if<Ppat_tuple>(&p.desc)) {
      for (auto& e : t->elems) scan_pat_ctors(*e);
    } else if (auto* o = std::get_if<Ppat_or>(&p.desc)) {
      scan_pat_ctors(*o->l); scan_pat_ctors(*o->r);
    } else if (auto* a = std::get_if<Ppat_alias>(&p.desc)) {
      scan_pat_ctors(*a->p);
    } else if (auto* c = std::get_if<Ppat_constraint>(&p.desc)) {
      scan_pat_ctors(*c->p);
    } else if (auto* r = std::get_if<Ppat_record>(&p.desc)) {
      for (auto& [lbl, sub] : r->fields) scan_pat_ctors(*sub);
    } else if (auto* ar = std::get_if<Ppat_array>(&p.desc)) {
      for (auto& e : ar->elems) scan_pat_ctors(*e);
    } else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
      if (v->arg) scan_pat_ctors(**v->arg);
    } else if (auto* lz = std::get_if<Ppat_lazy>(&p.desc)) {
      scan_pat_ctors(*lz->p);
    } else if (auto* ex = std::get_if<Ppat_exception>(&p.desc)) {
      scan_pat_ctors(*ex->p);
    }
  }

  void register_types(const Structure& s) {
    auto each_decl = [&](auto fn) {
      for (auto& item : s)
        if (auto* td = std::get_if<Pstr_type>(&item.desc))
          for (auto& d : td->decls) fn(d);
    };
    each_decl([&](const TypeDeclaration& d) {  // variants first (records may cite them)
      if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
        int nc = 0, nb = 0;
        bool all_const = !v->ctors.empty(), gadt = false;
        // `[@@unboxed]` (one one-argument constructor): the constructor is a no-op
        // wrapper -- its value IS the argument, with no allocation or field read.
        bool unboxed = has_attr(d.attrs, "unboxed") && v->ctors.size() == 1;
        for (auto& c : v->ctors) {
          bool block = true;
          int arity = 0;
          auto* r = std::get_if<Pcstr_record>(&c.args);
          if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) { arity = (int)t->elems.size(); block = arity > 0; }
          else if (r) arity = (int)r->fields.size();  // inline record: fields in the block
          if (c.res) gadt = true;
          if (block) all_const = false;
          builtin_ctors_.erase(c.name.txt);  // a local decl un-marks a builtin
          CtorInfo ci{d.name.txt, block ? nb : nc, block, arity, unboxed && arity == 1};
          if (r) {
            int ridx = 0;
            for (auto& f : r->fields) {
              ValueKind fk = coretype_kind(*f.type);
              bool fm = f.mut == MutableFlag::Mutable;
              ci.rlabels.push_back(f.name.txt);
              ci.rshape.push_back(fk);
              ci.rfmut.push_back(fm);
              ci.rftypes.push_back(coretype_record_path(*f.type));
              // the labels resolve like record labels (`r.cnt` on a bound
              // inline-record value reads the block field)
              if (field_info_.count(f.name.txt)) ambiguous_fields_.insert(f.name.txt);
              field_info_[f.name.txt] = {d.name.txt, ridx++, fm, fk};
            }
          }
          if (ctor_info_.count(c.name.txt) &&
              ctor_info_[c.name.txt].type != d.name.txt)
            ambiguous_ctors_.insert(c.name.txt);  // same name, a different type
          type_ctor_info_[d.name.txt][c.name.txt] = ci;
          ctor_info_[c.name.txt] = std::move(ci);
          if (block) ++nb; else ++nc;
        }
        type_ctors_[d.name.txt] = {nc, nb};
        if (all_const && !gadt) immediate_local_.insert(d.name.txt);
        if (gadt) gadt_types_.insert(d.name.txt);
      }
    });
    each_decl([&](const TypeDeclaration& d) {  // aliases (records resolve through them)
      if (d.manifest && !std::get_if<Ptype_variant>(&d.kind) &&
          !std::get_if<Ptype_record>(&d.kind)) {
        local_alias_.emplace(d.name.txt, d.manifest->get());
        // A local alias to an IMPORTED module's variant (`type sd =
        // Value_rec_types.recursive_binding_kind`): register that variant's ctors so
        // a bare `Static`/`Dynamic` -- used by type-directed disambiguation with no
        // `open Value_rec_types` -- resolves instead of becoming an unbound `?Static`.
        if (auto* c = std::get_if<Ptyp_constr>(&d.manifest->get()->desc))
          if (std::holds_alternative<Ldot>(c->id.txt.v)) {
            std::string dotted;
            if (lid_to_dotted(c->id.txt, dotted)) register_ctors_of_type(dotted);
          }
      }
    });
    each_decl([&](const TypeDeclaration& d) {  // polymorphic-variant abbreviations
      if (!d.manifest) return;
      auto* pv = std::get_if<Ptyp_variant>(&d.manifest->get()->desc);
      if (!pv) return;
      std::vector<std::string> tags, inh;
      for (auto& rf : pv->rows) {
        if (auto* rt = std::get_if<Rtag>(&rf)) tags.push_back(rt->name);
        else if (auto* ri = std::get_if<Rinherit>(&rf))
          if (auto* c = std::get_if<Ptyp_constr>(&ri->ct->desc))
            inh.push_back(lid_last(c->id.txt));
      }
      pv_raw_tags_[d.name.txt] = std::move(tags);
      pv_inherits_[d.name.txt] = std::move(inh);
    });
    // A top-level submodule that `include`s an imported module (matching's
    // `module Simple = struct include Patterns.Simple end`): register the
    // included module's polyvariant abbreviations under "Submod.type", so a
    // qualified `#Submod.type` pattern resolves its tag set.
    for (auto& item : s)
      if (auto* pm = std::get_if<Pstr_module>(&item.desc))
        if (pm->binding.name.txt)
          if (auto* ps = peel_to_structure(pm->binding.expr))
            for (auto& sit : ps->items)
              if (auto* inc = std::get_if<Pstr_include>(&sit.desc))
                if (auto* mi = std::get_if<Pmod_ident>(&inc->expr.desc)) {
                  std::string dotted;
                  if (lid_to_dotted(mi->id.txt, dotted))
                    register_pv_types_from(*pm->binding.name.txt, dotted);
                }
    each_decl([&](const TypeDeclaration& d) {  // then records
      if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
        RecType rt;
        rt.mut = false;
        rt.flat = !rec->fields.empty();
        int idx = 0;
        for (auto& f : rec->fields) {
          ValueKind k = coretype_kind(*f.type);
          bool m = f.mut == MutableFlag::Mutable;
          rt.mut |= m;
          rt.flat = rt.flat && is_float_core(*f.type);
          rt.labels.push_back(f.name.txt);
          rt.shape.push_back(k);
          if (field_info_.count(f.name.txt) && field_info_[f.name.txt].type != d.name.txt)
            ambiguous_fields_.insert(f.name.txt);
          FieldInfo finfo{d.name.txt, idx++, m, k};
          type_field_info_[d.name.txt][f.name.txt] = finfo;
          field_info_[f.name.txt] = finfo;
          // A field whose value is a labelled function: record its label sig so a
          // call through the projection reorders args (the local_alias_ pass above
          // already ran, so a `core_incl`-aliased arrow expands here).
          if (FnSig fs = coretype_label_sig(f.type.get()); !fs.empty())
            field_fn_sig_[f.name.txt] = std::move(fs);
        }
        rec_types_[d.name.txt] = std::move(rt);
      }
    });
    // Constructors declared inside nested module structures are referenced by a
    // bare name within that module's body (e.g. `module B = struct type t = B ..`
    // then `B`).  Register them too, but only filling names not already taken so a
    // nested type never shadows a top-level one (and clashes stay unresolved).
    std::function<void(const Structure&, const std::string&)> nested =
        [&](const Structure& items, const std::string& modpath) {
      for (auto& item : items) {
        if (auto* td = std::get_if<Pstr_type>(&item.desc))
          for (auto& d : td->decls) {
            // fill-absent registration of a submodule's aliases and records, so
            // qualified labels resolve ({ Float_record.f = .. } incl. flat
            // float records); a nested decl never shadows a top-level one
            if (d.manifest && !std::get_if<Ptype_variant>(&d.kind) &&
                !std::get_if<Ptype_record>(&d.kind))
              local_alias_.emplace(d.name.txt, d.manifest->get());
            if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
              // Register the record type's layout so its literals/projections
              // resolve.  A label already claimed by another type is left
              // pointing at that type and marked ambiguous -- as the top-level
              // records pass does -- rather than discarding the whole nested type
              // (which would leave its construction unresolvable, e.g. load_path's
              // `Dir.t` whose `hidden` label clashes with the top-level `paths`).
              // A *different* record that already claims this short type name
              // (`Persistent_signature.t` vs the top-level `'a t`) is registered
              // under a disambiguated key so the by-label-set lookup still finds it.
              std::vector<std::string> labels;
              for (auto& f : rec->fields) labels.push_back(f.name.txt);
              auto exist = rec_types_.find(d.name.txt);
              if (exist != rec_types_.end() && exist->second.labels == labels)
                continue;  // already registered (same type), nothing to do
              std::string key = d.name.txt;
              if (exist != rec_types_.end())
                key = d.name.txt + "#" + std::to_string(rec_types_.size());
              RecType rt;
              rt.mut = false;
              rt.flat = !rec->fields.empty();
              int idx = 0;
              for (auto& f : rec->fields) {
                ValueKind k = coretype_kind(*f.type);
                bool m = f.mut == MutableFlag::Mutable;
                rt.mut |= m;
                rt.flat = rt.flat && is_float_core(*f.type);
                rt.labels.push_back(f.name.txt);
                rt.shape.push_back(k);
                type_field_info_[key][f.name.txt] = {key, idx, m, k};
                if (field_info_.count(f.name.txt) &&
                    field_info_[f.name.txt].type != key)
                  ambiguous_fields_.insert(f.name.txt);  // shared label: unusable bare
                else
                  field_info_[f.name.txt] = {key, idx, m, k};
                ++idx;
              }
              rec_types_[key] = std::move(rt);
              if (!modpath.empty()) mod_record_types_[modpath].push_back(key);
            }
            if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
              int nc = 0, nb = 0; bool all_const = !v->ctors.empty(), gadt = false;
              // A nested variant whose SHORT type name collides with an already-
              // registered, DIFFERENTLY-shaped variant (includemod's top-level
              // `pos = Module|Modtype|Arg|Body`, 4 block ctors, vs Directionality's
              // `pos = Strictly_positive|Positive|Negative`, 3 const) must register
              // its ctor count under a disambiguated key, else a `match` over the
              // nested one builds a switch for the WRONG ctor set -> all arms collapse
              // to Match_failure.  Pre-count to pick the key before tagging ctors.
              int pc = 0, pb = 0;
              for (auto& c : v->ctors) {
                auto* rr = std::get_if<Pcstr_record>(&c.args);
                auto* tt = std::get_if<Pcstr_tuple>(&c.args);
                if (rr || (tt && !tt->elems.empty())) ++pb; else ++pc;
              }
              std::string tkey = d.name.txt;
              if (auto ex = type_ctors_.find(d.name.txt);
                  ex != type_ctors_.end() && ex->second != std::make_pair(pc, pb))
                tkey = d.name.txt + "#" + std::to_string(type_ctors_.size());
              for (auto& c : v->ctors) {
                int arity = 0; bool block = true;
                auto* r = std::get_if<Pcstr_record>(&c.args);
                if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) {
                  arity = (int)t->elems.size(); block = arity > 0;
                } else if (r) arity = (int)r->fields.size();  // inline record (Map's Node)
                if (c.res) gadt = true;
                if (block) all_const = false;
                // An inline-record ctor (`Open of {..}`) carries rlabels the
                // matcher needs to bind `Open {l=..}` at the right block field.
                // A same-named ctor of ANOTHER type may already be registered
                // (env.ml's TycompTbl.layer `Open` collides with Asttypes'
                // constant `closed_flag.Open`); since that one has no rlabels,
                // let the inline-record version overwrite it so `Open {..}`
                // resolves to this type's field order (the constant ctor is only
                // ever used bare, which doesn't consult rlabels).
                bool inline_over = r && ctor_info_.count(c.name.txt) &&
                                   ctor_info_[c.name.txt].rlabels.empty();
                if (!ctor_info_.count(c.name.txt) || inline_over) {
                  builtin_ctors_.erase(c.name.txt);
                  CtorInfo ci{tkey, block ? nb : nc, block, arity};
                  if (r) {  // inline-record labels, so `Node {h}` binds h to its field
                    int ridx = 0;
                    for (auto& f : r->fields) {
                      ValueKind fk = coretype_kind(*f.type);
                      bool fm = f.mut == MutableFlag::Mutable;
                      ci.rlabels.push_back(f.name.txt);
                      ci.rshape.push_back(fk);
                      ci.rfmut.push_back(fm);
                      ci.rftypes.push_back(coretype_record_path(*f.type));
                      if (!field_info_.count(f.name.txt))
                        field_info_[f.name.txt] = {d.name.txt, ridx, fm, fk};
                      ++ridx;
                    }
                  }
                  ctor_info_[c.name.txt] = std::move(ci);
                }
                if (block) ++nb; else ++nc;
              }
              type_ctors_[tkey] = std::make_pair(nc, nb);
              if (all_const && !gadt) immediate_local_.insert(tkey);
            }
          }
        if (auto* pm = std::get_if<Pstr_module>(&item.desc))
          if (auto* ps = peel_to_structure(pm->binding.expr))
            nested(ps->items, sub_modpath(modpath, pm->binding.name.txt));
        if (auto* prm = std::get_if<Pstr_recmodule>(&item.desc))
          for (auto& b : prm->bindings)
            if (auto* ps = peel_to_structure(b.expr))
              nested(ps->items, sub_modpath(modpath, b.name.txt));
      }
    };
    for (auto& item : s) {
      if (auto* pm = std::get_if<Pstr_module>(&item.desc))
        if (auto* ps = peel_to_structure(pm->binding.expr))
          nested(ps->items, sub_modpath("", pm->binding.name.txt));
      if (auto* prm = std::get_if<Pstr_recmodule>(&item.desc))
        for (auto& b : prm->bindings)
          if (auto* ps = peel_to_structure(b.expr))
            nested(ps->items, sub_modpath("", b.name.txt));
    }
  }
  // Build a dotted submodule path, or "" for an anonymous module (whose records
  // can't be named-qualified anyway).
  static std::string sub_modpath(const std::string& parent,
                                 const std::optional<std::string>& name) {
    if (!name) return "";
    return parent.empty() ? *name : parent + "." + *name;
  }
  // The structure under `M : S = struct..end` / a functor `F (X) = struct..end`
  // (a functor body's type decls register fill-absent like a submodule's).
  static const Pmod_structure* peel_to_structure(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    for (;;) {
      if (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) { me = pc->me.get(); continue; }
      if (auto* pf = std::get_if<Pmod_functor>(&me->desc)) { me = pf->body.get(); continue; }
      return std::get_if<Pmod_structure>(&me->desc);
    }
  }
  // Scope a record type declaration's labels (so an ambiguous label resolves to
  // THIS record while it is in scope), resolving the rec_types_ key by the
  // decl's label set -- the registered key may be disambiguated ("t#N") when the
  // short type name is taken by another module's type.
  void scope_record_decl(const TypeDeclaration& d) {
    auto* rec = std::get_if<Ptype_record>(&d.kind);
    if (!rec) return;
    std::vector<std::string> labels;
    for (auto& f : rec->fields) labels.push_back(f.name.txt);
    std::string key = d.name.txt;
    if (auto it = rec_types_.find(key); it == rec_types_.end() || it->second.labels != labels) {
      key.clear();
      for (auto& [k, r] : rec_types_) if (r.labels == labels) { key = k; break; }
    }
    if (key.empty()) return;
    int idx = 0;
    for (auto& f : rec->fields) {
      if (ambiguous_fields_.count(f.name.txt)) {
        field_info_[f.name.txt] =
            {key, idx, f.mut == MutableFlag::Mutable, coretype_kind(*f.type)};
        scoped_unambig_fields_.insert(f.name.txt);
      }
      ++idx;
    }
  }
  const FieldInfo* find_field(const std::string& label) {
    if (ambiguous_fields_.count(label) && !scoped_unambig_fields_.count(label)) return nullptr;
    auto it = field_info_.find(label);
    return it == field_info_.end() ? nullptr : &it->second;
  }
  // Resolve `label` of a record named by dotted `path` -- a bare local record
  // ("foo"), a LOCAL submodule record ("Pattern_env.t" via mod_record_types_), or
  // an IMPORTED module record ("Mod.t" via its cmi).  Used for a var tagged
  // (var_record_path_) with its record type when its label is AMBIGUOUS.
  std::optional<FieldInfo> resolve_field_in_record_path(const std::string& path,
                                                        const std::string& label) {
    auto in_key = [&](const std::string& key) -> std::optional<FieldInfo> {
      auto tf = type_field_info_.find(key);
      if (tf == type_field_info_.end()) return std::nullopt;
      auto f = tf->second.find(label);
      if (f == tf->second.end()) return std::nullopt;
      return f->second;
    };
    auto dpos = path.rfind('.');
    if (dpos == std::string::npos) return in_key(path);  // bare local record
    std::string mod = path.substr(0, dpos), ty = path.substr(dpos + 1);
    if (auto mr = mod_record_types_.find(mod); mr != mod_record_types_.end())
      for (auto& key : mr->second)
        if (key == ty || (key.size() > ty.size() + 1 &&
                          key.compare(0, ty.size(), ty) == 0 && key[ty.size()] == '#'))
          if (auto fi = in_key(key)) return fi;
    if (mod.find('.') == std::string::npos)  // an imported top-level module's record
      if (auto sr = stdlib_record_layout_named(mod, ty)) {
        for (int i = 0; i < (int)sr->labels.size(); ++i)
          if (sr->labels[i] == label)
            return FieldInfo{ty, i, sr->mut[i], sr->shape[i]};
      }
    return std::nullopt;
  }
  // Tag a var bound by an inline-record-ctor field pattern (`Pattern {penv}`) with
  // its field's record type, so a later `penv.env` resolves the ambiguous label.
  void tag_inline_field_var(const Pattern* sub, const CtorInfo& ci, int ix) {
    if (ix < 0 || ix >= (int)ci.rftypes.size() || ci.rftypes[ix].empty()) return;
    auto* pv = std::get_if<Ppat_var>(&effective_pat(sub)->desc);
    if (!pv) return;
    if (auto* b = lookup(pv->name.txt)) var_record_path_[b->stamp] = ci.rftypes[ix];
  }
  // A var/record-pattern NODE tagged (at pattern-scan time) with the record type its
  // enclosing constructor argument declares -- applied to var_record_path_ when the
  // var is bound, so a later `md.md_type` resolves the ambiguous label correctly.
  std::unordered_map<const void*, std::string> var_node_path_;
  void apply_var_node_path(const Pattern* p, const Ident& id) {
    if (auto it = var_node_path_.find((const void*)p); it != var_node_path_.end())
      var_record_path_[id.stamp] = it->second;
  }
  // `x.label` where x was bound to a whole inline record by `Ctor x` -- resolve
  // the field via that ctor's rlabels, so an AMBIGUOUS label (ident.ml's `stamp`,
  // shared by Local/Predef/Scoped) reads the right offset (find_field bails on it).
  std::optional<FieldInfo> inline_rec_field(const Expression* base, const std::string& label) {
    auto* id = std::get_if<Pexp_ident>(&base->desc);
    if (!id) return std::nullopt;
    auto* l = std::get_if<Lident>(&id->id.txt.v);
    if (!l) return std::nullopt;
    const Ident* b = lookup(l->name);
    if (!b) return std::nullopt;
    auto it = inline_rec_var_.find(b->stamp);
    if (it == inline_rec_var_.end()) return std::nullopt;
    const CtorInfo& ci = *it->second;
    for (int i = 0; i < (int)ci.rlabels.size(); ++i)
      if (ci.rlabels[i] == label)
        return FieldInfo{ci.type, i, ci.rfmut[i], ci.rshape[i]};
    return std::nullopt;
  }

  // (field_imm N (global G!)) for a value at field idx of global module G.
  // Whether any opened module exports `n` (so it shadows a pervasive).
  bool opened_has(const std::string& n) {
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
      if (module_base(*it) && module_layout_[*it].count(n)) return true;
      if (it->find('.') != std::string::npos) {
        if (submodule_value(*it, n)) return true;
        continue;
      }
      if (fields_of(*it).count(n)) return true;
    }
    return false;
  }
  LamPtr field_of(const std::string& global, int idx) {
    auto g = mk(Lam::K::Prim); g->prim = Prim::Global; g->prim_id = global;
    auto f = mk(Lam::K::Prim); f->prim = Prim::FieldImm; f->prim_arg = idx; f->args = {g};
    return f;
  }
  // The base expression for a local module `m`: its alias path if `m` is a module
  // alias, else a Var of its binding; null if `m` is not a known local module.
  LamPtr module_base(const std::string& m) {
    if (auto a = module_alias_.find(m); a != module_alias_.end()) return a->second;
    if (auto i = module_ident_.find(m); i != module_ident_.end()) {
      auto v = mk(Lam::K::Var); v->var = i->second; return v;
    }
    return nullptr;
  }
  // Is the HEAD module of a path bound at all -- a local module/alias/functor
  // param, an already-loaded cmi, or a `.cmi` findable on the include path?  When
  // it is NOT, a qualified reference through it (`Lib.x`, `Lib.Make`) cannot be
  // compiled and must be a hard error, not a silent `?`/`0` placeholder (the most
  // common cause is a missing `-I` so the unit's `.cmi` isn't on the load path).
  // Deliberately GENEROUS: any doubt resolves to "resolvable" so a valid compile
  // is never turned into a spurious error -- only a genuinely-absent module fails.
  bool module_head_resolvable(const std::string& head) {
    if (head.empty()) return true;
    // NB: NOT mod_fields.count -- fields_of() inserts the key even when the cmi
    // failed to load (an empty layout), so it is true for an absent module too.
    if (module_base(head) || module_layout_.count(head) ||
        module_ident_.count(head) || module_alias_.count(head) ||
        submod_alias_.count(head) || module_functor_src_.count(head))
      return true;
    for (const std::string& o : opened_)
      if (o == head || o.rfind(head + ".", 0) == 0) return true;
    if (!opened_submodule_path(head).empty()) return true;
    // A SUBMODULE of the default-opened Stdlib (`LargeFile.seek_in` =
    // Stdlib.LargeFile.seek_in) or of any explicitly-opened module is a real
    // reference we may not fully lower yet -- a feature gap, NOT an unbound module.
    if (fields_of("Stdlib").count(head)) return true;
    for (const std::string& o : opened_)
      if (o.find('.') == std::string::npos &&
          (fields_of(o).count(head) || module_layout_[o].count(head)))
        return true;
    return std::filesystem::exists(resolve_cmi(head));
  }
  // A path resolution dead end: the head module is not bound anywhere.  Matches
  // OCaml's `Unbound module` (which its type checker raises in Env BEFORE codegen)
  // plus a hint, since the usual cause is the include path.
  [[noreturn]] void unbound_module(const std::string& head, const std::string& full) {
    std::string low = head.empty() ? head
                    : std::string(1, (char)std::tolower((unsigned char)head[0])) + head.substr(1);
    throw std::runtime_error(
        "Unbound module " + head +
        (full != head && !full.empty() ? " (referenced as " + full + ")" : "") +
        ": cannot find " + low + ".cmi on the include path (is the -I path correct?)");
  }
  // Resolve a (possibly dotted) local module path to its base expression plus
  // the module_layout_ key holding its field layout; base is null when the
  // head isn't a local module or a step's layout is unknown.
  struct ModPath { LamPtr base; std::string key; };
  ModPath resolve_module_path(const std::string& dotted) {
    size_t p = dotted.find('.');
    std::string head = p == std::string::npos ? dotted : dotted.substr(0, p);
    LamPtr base = module_base(head);
    if (!base) return {};
    std::string key = head;
    while (p != std::string::npos) {
      size_t q = dotted.find('.', p + 1);
      std::string comp =
          dotted.substr(p + 1, (q == std::string::npos ? dotted.size() : q) - p - 1);
      auto li = module_layout_.find(key);
      if (li == module_layout_.end()) return {};
      auto fi = li->second.find(comp);
      if (fi == li->second.end()) return {};
      base = fieldimm(fi->second, base);
      key += '.'; key += comp;
      p = q;
    }
    return {base, key};
  }
  // `M.E` / `A.B.E`: a constructor qualified by a bound local module whose
  // layout EXPORTS it (an exception or extension constructor -- regular variant
  // constructors are type-level, not module fields).  Returns the constructor's
  // identity value `(field_imm i M)`, which distinguishes same-named extension
  // constructors that live in different modules (`M1.E` vs `M2.E`).  Null when
  // the prefix isn't a bound module path or doesn't export the name.
  LamPtr module_ctor_identity(const Longident& id) {
    auto* dq = std::get_if<Ldot>(&id.v);
    if (!dq) return nullptr;
    std::string prefix;
    if (!lid_to_dotted(*dq->prefix, prefix)) return nullptr;
    ModPath mp = resolve_module_path(prefix);
    if (!mp.base) return nullptr;
    auto li = module_layout_.find(mp.key);
    if (li == module_layout_.end()) return nullptr;
    auto fi = li->second.find(dq->name);
    if (fi == li->second.end()) return nullptr;
    return fieldimm(fi->second, mp.base);
  }
  // The identity value of a QUALIFIED exn/extension ctor `M.E` (M local OR
  // imported, possibly nested): a runtime field of M's block.  Null when `M.E`
  // is not a module field -- a bare variant ctor never is -- so a non-null
  // result means `M.E` really is an exception (and outranks a same-named bare
  // variant the unqualified resolution would otherwise pick).
  LamPtr qualified_exn_identity(const Longident& id) {
    if (LamPtr v = module_ctor_identity(id)) return v;  // local module
    auto* dq = std::get_if<Ldot>(&id.v);
    if (!dq) return nullptr;
    std::string prefix;
    if (!lid_to_dotted(*dq->prefix, prefix)) return nullptr;
    if (prefix.find('.') != std::string::npos)   // nested submodule path
      return submodule_value(prefix, dq->name);
    if (module_base(prefix)) return nullptr;     // local module: handled above
    auto& fm = fields_of(prefix);
    if (auto f = fm.find(dq->name); f != fm.end())
      return field_of(global_of(prefix), f->second);
    return nullptr;
  }
  // The package module-type name of `(module .. : S)` / `(e : (module S))` /
  // an ident bound to one; empty if `e` isn't a first-class-module package.
  std::string expr_pack_modtype(const Expression& e0) {
    const Expression* e = &e0;
    while (auto* ct = std::get_if<Pexp_constraint>(&e->desc)) {
      if (ct->t)
        if (auto* pk = std::get_if<Ptyp_package>(&ct->t->desc)) {
          std::string out;
          if (lid_to_dotted(pk->path.txt, out)) return out;
        }
      e = ct->e.get();
    }
    if (auto* pp = std::get_if<Pexp_pack>(&e->desc); pp && pp->pkg) {
      std::string out;
      if (lid_to_dotted(pp->pkg->path.txt, out)) return out;
    }
    if (auto* id = std::get_if<Pexp_ident>(&e->desc))
      if (auto* l = std::get_if<Lident>(&id->id.txt.v))
        if (auto it = pack_modtype_.find(l->name); it != pack_modtype_.end())
          return it->second;
    // `(val (match .. with .. -> (module M : S) | ..))` (filename.ml's Sysdeps):
    // every arm packs the same module type, so any arm's gives the layout.
    if (auto* m = std::get_if<Pexp_match>(&e->desc))
      for (auto& c : m->cases)
        if (std::string r = expr_pack_modtype(*c.rhs); !r.empty()) return r;
    if (auto* it = std::get_if<Pexp_ifthenelse>(&e->desc)) {
      if (std::string r = expr_pack_modtype(*it->then_); !r.empty()) return r;
      if (it->else_) return expr_pack_modtype(**it->else_);
    }
    return {};
  }
  // The runtime field layout of a (possibly qualified) package module type
  // name: a local `module type S` via modtype_layout_, or a qualified stdlib
  // one (`Set.S`) from its module's cmi modtype decl.  Empty if unresolved.
  std::vector<std::string> pack_modtype_layout(const std::string& mtname) {
    if (mtname.empty()) return {};
    size_t dot = mtname.rfind('.');
    std::string last = mtname.substr(dot + 1);
    if (auto it = modtype_layout_.find(last); it != modtype_layout_.end())
      return it->second;
    if (dot != std::string::npos) {  // Head.S -> the head module's cmi modtype
      std::string head = mtname.substr(0, dot);
      if (head.find('.') == std::string::npos && !module_base(head)) try {
        auto cmi = cmi::CmiFile::load(resolve_cmi(head));
        for (auto& md : cmi.sig().modtypes)
          if (md.name == last) return mt_fields(cmi, md.type);
      } catch (...) {}
    }
    return {};
  }
  // Register the layouts of a module bound to a first-class-module package of
  // module type `mtname` (dotted): nested via the local sig AST when known,
  // else flat via the named module type's layout (local or qualified stdlib).
  void register_pack_layouts(const std::string& prefix, const std::string& mtname) {
    if (mtname.empty()) return;
    std::string last = mtname.substr(mtname.rfind('.') + 1);
    if (auto a = modtype_ast_.find(last); a != modtype_ast_.end())
      return register_sig_layouts(prefix, *a->second);
    auto lay = pack_modtype_layout(mtname);
    auto& ml = module_layout_[prefix]; ml.clear();
    for (int i = 0; i < (int)lay.size(); ++i) ml[lay[i]] = i;
  }
  // Coerce a packed module value to the package module type's layout when it
  // differs (the typed coercion ocamlc inserts when creating a first-class
  // module): `(let (let/N = mv) (makeblock 0 (field_mut i let/N) ..))`.
  LamPtr pack_coerce(LamPtr mv, const ModuleExpr& me, const std::string& mtname) {
    if (mtname.empty()) return mv;
    std::string last = mtname.substr(mtname.rfind('.') + 1);
    std::vector<std::string> target;
    if (auto it = modtype_layout_.find(last); it != modtype_layout_.end())
      target = it->second;
    if (target.empty()) return mv;
    auto src = module_result_layout(me);
    if (LamPtr c = coerce_block(mv, src, target)) return c;
    // coerce_block bails when a target member is absent from src -- targetint's
    // `(module Int32 : S)` where Int32 = `struct include Int32; .. end` and S
    // lists Int32's EXTERNALS (add/sub/..) as vals: our `include Int32` inlines
    // them (no field).  Build the block field-reading the present members and
    // eta-stubbing the missing ones from the same-named stdlib module's prims.
    std::string stdmod;
    if (auto* pi = std::get_if<Pmod_ident>(&me.desc))
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) stdmod = l->name;
    if (stdmod.empty()) return mv;
    for (auto& nm : target)
      if (std::find(src.begin(), src.end(), nm) == src.end() &&
          value_prim(stdmod, nm).name.empty())
        return mv;  // a missing member isn't a stdlib prim -> can't materialise
    Ident id = fresh("let");
    auto v = mk(Lam::K::Var); v->var = id;
    std::vector<LamPtr> fs;
    for (auto& nm : target) {
      int idx = -1;
      for (int i = 0; i < (int)src.size(); ++i) if (src[i] == nm) { idx = i; break; }
      if (idx >= 0) { fs.push_back(fieldimm(idx, v)); continue; }
      LamPtr s = prim_stub(value_prim(stdmod, nm));
      fs.push_back(s ? s : cint(0));
    }
    auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
    blk->args = std::move(fs);
    auto lt = mk(Lam::K::Let); lt->bindings = {{id, ValueKind::Gen, mv}}; lt->body = blk;
    return lt;
  }
  // The Pmty_signature under a module type, resolving named module types
  // through modtype_ast_; null when unknown.
  const Pmty_signature* sig_items_of(const ModuleType& mt0) {
    const ModuleType* m = &mt0;
    for (int g = 0; g < 8; ++g) {
      if (auto* ps = std::get_if<Pmty_signature>(&m->desc)) return ps;
      auto* pi = std::get_if<Pmty_ident>(&m->desc);
      if (!pi) return nullptr;
      const ModuleType* res = nullptr;
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v))
        if (auto a = modtype_ast_.find(l->name); a != modtype_ast_.end()) res = a->second;
      if (!res) return nullptr;
      m = res;
    }
    return nullptr;
  }
  // A signature's named module member's type; null if not a module member.
  static const ModuleType* sig_member_modtype(const Pmty_signature& sig,
                                              const std::string& nm) {
    for (auto& it : sig.items)
      if (auto* m = std::get_if<Psig_module>(&it.desc))
        if (m->md.name.txt && *m->md.name.txt == nm) return &*m->md.type;
    return nullptr;
  }
  // A source module expression's named module member: its expression when the
  // source is a structure (for nested coercion), else null.
  static const ModuleExpr* src_member_expr(const ModuleExpr& me0, const std::string& nm) {
    const ModuleExpr* m = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&m->desc)) m = pc->me.get();
    if (auto* ps = std::get_if<Pmod_structure>(&m->desc))
      for (auto& it : ps->items)
        if (auto* pm = std::get_if<Pstr_module>(&it.desc))
          if (pm->binding.name.txt && *pm->binding.name.txt == nm)
            return &pm->binding.expr;
    return nullptr;
  }
  // The field layout of a source module expression's named module member.
  std::vector<std::string> src_member_layout(const ModuleExpr& me0, const std::string& nm) {
    if (const ModuleExpr* sub = src_member_expr(me0, nm))
      return module_result_layout(*sub);
    const ModuleExpr* m = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&m->desc)) m = pc->me.get();
    if (auto* pi = std::get_if<Pmod_ident>(&m->desc)) {
      std::string dotted;
      if (lid_to_dotted(pi->id.txt, dotted)) return layout_vec(dotted + "." + nm);
    }
    return {};
  }
  // Project a module value from a `src` field layout to a narrower/reordered
  // `target` one: `(let (let/N = mv) (makeblock 0 (field_mut i let/N) ..))`.
  // Null when no projection is needed (or possible): equal layouts, either
  // side unknown, or a target field missing from the source.  With `src_me`
  // and `tsig` supplied, module members narrowed by the signature project
  // recursively.
  // The nested signature of submodule `nm` within a decoded .cmi signature, when
  // it is a structural (Mty_signature) submodule -- used to recurse impl->intf
  // field coercion into nested submodules (Misc.Stdlib.String).
  static const cmi::Signature* nested_cmi_sig(const cmi::Signature& s,
                                              const std::string& nm) {
    for (auto& md : s.modules)
      if (md.name == nm && md.type && md.type->kind == cmi::ModuleType::Sig &&
          md.type->sig)
        return md.type->sig.get();
    return nullptr;
  }
  // Does coercing a module block (field names `src`, with nested submodule
  // layouts registered under dotted `src_path` keys in module_layout_) to the
  // cmi target signature `tsig` change anything -- at this level OR in any nested
  // submodule?  A nested submodule can be out of order even when this level's
  // field order already matches, so the top-level reorder must still recurse.
  bool needs_coerce_cmi(const std::vector<std::string>& src,
                        const cmi::Signature& tsig, const std::string& src_path) {
    if (tsig.fields.empty()) return false;
    for (auto& nm : tsig.fields)
      if (std::find(src.begin(), src.end(), nm) == src.end()) return false;
    if (tsig.fields != src) return true;
    for (auto& nm : tsig.fields)
      if (const cmi::Signature* nsig = nested_cmi_sig(tsig, nm)) {
        std::string sp = src_path.empty() ? nm : src_path + "." + nm;
        std::vector<std::string> sl = layout_vec(sp);
        if (!sl.empty() && needs_coerce_cmi(sl, *nsig, sp)) return true;
      }
    return false;
  }
  LamPtr coerce_block(const LamPtr& mv, const std::vector<std::string>& src,
                      const std::vector<std::string>& target,
                      const ModuleExpr* src_me = nullptr,
                      const Pmty_signature* tsig = nullptr,
                      const cmi::Signature* tcmi = nullptr,
                      const std::string& src_path = "") {
    if (target.empty() || src.empty()) return nullptr;
    for (auto& nm : target)
      if (std::find(src.begin(), src.end(), nm) == src.end()) return nullptr;
    // When this level's field order already matches, a coercion is still needed
    // if a NESTED submodule (per the .cmi signature) is itself out of order.
    if (target == src && !(tcmi && needs_coerce_cmi(src, *tcmi, src_path)))
      return nullptr;
    // Structure-include optimisation: when the source value is a freshly-built
    // structure block `(let <bindings> (makeblock 0 v0 v1 ..))`, the coercion is
    // fused into that makeblock -- keep the bindings (their effects) but rebuild
    // the body block from the selected/reordered field values directly, instead
    // of materialising the full block and reprojecting it (which allocates a
    // second module block).  ocamlc does this so a coerced `include (struct ..
    // end : sig .. end)` allocates only the narrowed block.  Only when the body
    // block's fields are all pure (Var/const) so dropping/reordering is safe.
    if (mv->k == Lam::K::Let && mv->body && mv->body->k == Lam::K::Prim &&
        mv->body->prim == Prim::Makeblock && mv->body->prim_arg == 0 &&
        mv->body->args.size() == src.size()) {
      bool simple = true;
      for (auto& a : mv->body->args)
        if (!(a->k == Lam::K::Var || a->k == Lam::K::ConstInt ||
              a->k == Lam::K::ConstChar || a->k == Lam::K::ConstString ||
              a->k == Lam::K::ConstFloat)) { simple = false; break; }
      if (simple) {
        std::vector<LamPtr> fs;
        for (auto& nm : target) {
          int idx = 0;
          for (int i = 0; i < (int)src.size(); ++i) if (src[i] == nm) { idx = i; break; }
          LamPtr fr = mv->body->args[idx];
          if (src_me && tsig)  // a narrowed module member projects recursively
            if (const ModuleType* smt = sig_member_modtype(*tsig, nm)) {
              const ModuleExpr* sme = src_member_expr(*src_me, nm);
              if (LamPtr c2 = coerce_block(fr, src_member_layout(*src_me, nm),
                                           sig_layout(*smt), sme, sig_items_of(*smt)))
                fr = c2;
            }
          if (tcmi)  // .cmi-driven nested submodule reorder (Misc.Stdlib.String)
            if (const cmi::Signature* nsig = nested_cmi_sig(*tcmi, nm)) {
              std::string sp = src_path.empty() ? nm : src_path + "." + nm;
              std::vector<std::string> sl = layout_vec(sp);
              if (!sl.empty())
                if (LamPtr c2 = coerce_block(fr, sl, nsig->fields, nullptr, nullptr,
                                             nsig, sp))
                  fr = c2;
            }
          fs.push_back(fr);
        }
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = std::move(fs);
        auto lt = mk(Lam::K::Let); lt->bindings = mv->bindings; lt->body = blk;
        return lt;
      }
    }
    Ident id = fresh("let");
    auto v = mk(Lam::K::Var); v->var = id;
    std::vector<LamPtr> fs;
    for (auto& nm : target) {
      int idx = 0;
      for (int i = 0; i < (int)src.size(); ++i) if (src[i] == nm) { idx = i; break; }
      LamPtr fr = mk(Lam::K::Prim); fr->prim = Prim::FieldMut;
      fr->prim_arg = idx; fr->args = {v};
      if (src_me && tsig)
        if (const ModuleType* smt = sig_member_modtype(*tsig, nm)) {
          const ModuleExpr* sme = src_member_expr(*src_me, nm);
          if (LamPtr c2 = coerce_block(fr, src_member_layout(*src_me, nm),
                                       sig_layout(*smt), sme, sig_items_of(*smt)))
            fr = c2;
        }
      if (tcmi)  // .cmi-driven nested submodule reorder (Misc.Stdlib.String)
        if (const cmi::Signature* nsig = nested_cmi_sig(*tcmi, nm)) {
          std::string sp = src_path.empty() ? nm : src_path + "." + nm;
          std::vector<std::string> sl = layout_vec(sp);
          if (!sl.empty())
            if (LamPtr c2 = coerce_block(fr, sl, nsig->fields, nullptr, nullptr,
                                         nsig, sp))
              fr = c2;
        }
      fs.push_back(fr);
    }
    auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
    blk->args = std::move(fs);
    auto lt = mk(Lam::K::Let); lt->bindings = {{id, ValueKind::Gen, mv}};
    lt->body = blk;
    return lt;
  }
  // An alias `module Y = X.M` adopts the source's layouts: every layout (and
  // functor) key at or under `src` re-registers at or under `dst`.
  void copy_layout_subtree(const std::string& src, const std::string& dst) {
    if (src == dst) return;
    auto under = [&](const std::string& k) {
      return k == src || (k.size() > src.size() && k[src.size()] == '.' &&
                          k.compare(0, src.size(), src) == 0);
    };
    auto copy = [&](auto& map) {
      std::vector<std::pair<std::string, typename std::decay_t<decltype(map)>::mapped_type>> ks;
      for (auto& [k, v] : map) if (under(k)) ks.emplace_back(dst + k.substr(src.size()), v);
      for (auto& [k, v] : ks) map[k] = std::move(v);
    };
    copy(module_layout_);
    copy(functor_result_);
    copy(functor_param_);
  }
  // Register the field layouts a module of signature `mt` exposes -- the module
  // itself under `prefix`, submodules under dotted keys, functor members in
  // functor_result_/functor_param_ -- so member paths through a first-class
  // module resolve.  A named module type resolves locally via modtype_ast_,
  // else flat via sig_layout (named/stdlib module types).
  void register_sig_layouts(const std::string& prefix, const ModuleType& mt, int depth = 0) {
    if (depth > 24) return;  // a recursive module type (strongly_connected_components'
                             // `module Id : .. module Id : ..`) would recurse forever
    const ModuleType* m = &mt;
    for (int guard = 0; ; ++guard) {
      // `S with ..` has S's runtime layout unless a modsubst removes a field.
      if (auto* pw = std::get_if<Pmty_with>(&m->desc); pw && with_keeps_layout(*pw)) {
        m = pw->mt.get(); continue;
      }
      auto* pi = std::get_if<Pmty_ident>(&m->desc);
      if (!pi) break;
      const ModuleType* res = nullptr;
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {
        if (auto a = modtype_ast_.find(l->name); a != modtype_ast_.end()) res = a->second;
      } else if (auto* d = std::get_if<Ldot>(&pi->id.txt.v)) {
        // A QUALIFIED modtype (`TableFormat.TABLES` -- a functor parameter sig in a
        // sibling submodule): resolve by its dotted path (registered alongside the
        // bare name), falling back to the last component.  The dotted key
        // disambiguates a name shared by two submodules (TableFormat.TABLES vs
        // InspectionTableFormat.TABLES).
        std::string dotted;
        if (lid_to_dotted(pi->id.txt, dotted))
          if (auto a = modtype_ast_.find(dotted); a != modtype_ast_.end()) res = a->second;
        if (!res)
          if (auto a = modtype_ast_.find(d->name); a != modtype_ast_.end()) res = a->second;
      }
      if (!res || guard > 8) {  // no local sig AST: flat layout only
        auto lay = sig_layout(*m);
        auto& ml = module_layout_[prefix]; ml.clear();
        for (int i = 0; i < (int)lay.size(); ++i) ml[lay[i]] = i;
        return;
      }
      m = res;
    }
    auto* ps = std::get_if<Pmty_signature>(&m->desc);
    if (!ps) return;
    auto& ml = module_layout_[prefix]; ml.clear();
    // Collect field names IN ORDER (expanding includes), then dedup-keep-last and
    // assign CONTIGUOUS indices -- the same canonical shadowing rule as the .cmi
    // writer.  `ml[name]=i++` would orphan a slot (a GAP) when a name is shadowed
    // across `include A; include B` (main_args' Bytecomp_options) -> the functor
    // body would index its members one slot off the coerced argument.
    std::vector<std::string> names;
    for (auto& it : ps->items) {
      if (auto* v = std::get_if<Psig_value>(&it.desc)) names.push_back(v->vd.name.txt);
      else if (auto* ex = std::get_if<Psig_exception>(&it.desc))
        names.push_back(ex->exn.ctor.name.txt);  // exceptions occupy slots
      else if (auto* tx = std::get_if<Psig_typext>(&it.desc))
        for (auto& c : tx->ext.ctors) names.push_back(c.name.txt);
      else if (auto* cl = std::get_if<Psig_class>(&it.desc))
        for (auto& d : cl->decls) {
          names.push_back(d.name.txt);  // so do classes
          register_class_meta_from_sig(prefix + "." + d.name.txt, d.expr);  // for `inherit M.c`
        }
      else if (auto* md = std::get_if<Psig_module>(&it.desc)) {
        if (!md->md.name.txt) continue;
        const std::string& nm = *md->md.name.txt;
        names.push_back(nm);
        const ModuleType& t = *md->md.type;
        if (auto* pf = std::get_if<Pmty_functor>(&t.desc)) {
          functor_result_[prefix + "." + nm] = sig_layout(*pf->body);
          if (auto* fp = std::get_if<Functor_named>(&pf->param); fp && fp->type)
            functor_param_[prefix + "." + nm] = sig_layout(*fp->type);
        } else {
          register_sig_layouts(prefix + "." + nm, t, depth + 1);
        }
      } else if (auto* pin = std::get_if<Psig_include>(&it.desc)) {
        // `include HashedType` inside a functor parameter's signature (`T : Thing`
        // = `sig include Hashtbl.HashedType; include Map.OrderedType; .. end`):
        // each included member occupies a slot, so `include T` in the functor body
        // splices them (equal/hash/compare) -- otherwise the result block is short
        // and a consumer's `include M` reads out of bounds -> GC segfault.
        const ModuleType* im = &pin->mt;
        if (auto* ii = std::get_if<Pmty_ident>(&im->desc)) {
          // Resolve a QUALIFIED modtype include (`IncrementalEngine.INCREMENTAL_
          // ENGINE`) by its DOTTED path first.  The bare last-name picks a SIBLING
          // submodule's same-named modtype (the top-level INCREMENTAL_ENGINE) whose
          // field ORDER differs -> the .cmo layout drifts from the .cmi (ENGINE
          // off-by-one -> the menhir wild-jump crash).
          std::string dotted;
          if (lid_to_dotted(ii->id.txt, dotted))
            if (auto a = modtype_ast_.find(dotted); a != modtype_ast_.end()) im = a->second;
          if (im == &pin->mt)
            if (auto a = modtype_ast_.find(lid_last(ii->id.txt)); a != modtype_ast_.end())
              im = a->second;
        }
        for (auto& n : sig_layout(*im)) names.push_back(n);
      }
    }
    names = dedup_keep_last(std::move(names));
    for (int i = 0; i < (int)names.size(); ++i) ml[names[i]] = i;
  }
  // Register the field layouts of a decoded cmi signature under dotted keys,
  // recursing into structural submodules: module_layout_[prefix.Sub] etc.  Used
  // for a cross-module functor RESULT (`module M = Identifiable.Make(..)`) so a
  // later `include M; Set.empty` (Set a functor-result submodule) resolves.
  void register_cmi_nested_layouts(const cmi::CmiFile& cmi, const std::string& prefix,
                                   const cmi::Signature& sig) {
    auto& ml = module_layout_[prefix]; ml.clear();
    for (int i = 0; i < (int)sig.fields.size(); ++i) ml[sig.fields[i]] = i;
    for (auto& md : sig.modules)
      if (const cmi::Signature* sub = mt_sig(cmi, md.type))
        register_cmi_nested_layouts(cmi, prefix + "." + md.name, *sub);
  }
  // For `module M = F(Arg)` with F a separately-compiled functor (Identifiable.
  // Make): register F's RESULT nested submodule layouts under M so `M.Set.x` (and
  // `Set.x` after `include M`) field-resolve.
  void register_functor_result_layouts(const std::string& prefix, const ModuleExpr& me) {
    auto* pa = std::get_if<Pmod_apply>(&me.desc);
    if (!pa) return;
    auto* fi = std::get_if<Pmod_ident>(&pa->f->desc);
    if (!fi) return;
    auto* d = std::get_if<Ldot>(&fi->id.txt.v);
    if (!d) return;
    auto* pl = std::get_if<Lident>(&d->prefix->v);
    if (!pl || module_base(pl->name) || fields_of(pl->name).empty()) return;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(pl->name));
      for (auto& md : cmi.sig().modules)
        if (md.name == d->name && md.type && md.type->kind == cmi::ModuleType::Functor)
          if (const cmi::Signature* rs = mt_sig(cmi, md.type->functor_body))
            // only the NESTED submodules -- the top-level prefix layout is already
            // set from module_result_layout and must not be clobbered (it drives
            // the field count of a downstream `include M`).
            for (auto& sm : rs->modules)
              if (const cmi::Signature* sub = mt_sig(cmi, sm.type))
                register_cmi_nested_layouts(cmi, prefix + "." + sm.name, *sub);
    } catch (...) {}
  }
  // Register the variant constructors declared in a functor parameter's signature
  // (`module Z(T:S)` with `type u = X|Y|Z`), so they resolve to their tags inside
  // the body after `open T`.  Returns a save list for restore_sig_ctors().
  struct SigCtorSave { std::string cname; bool had_c; CtorInfo c_info;
                       std::string tname; bool had_t; std::pair<int,int> t_info; };
  std::vector<SigCtorSave> register_sig_ctors(const ModuleType& mt) {
    std::vector<SigCtorSave> saved;
    const ModuleType* m = &mt;
    for (int g = 0; g < 8; ++g) {  // resolve a named module type to its signature
      auto* pi = std::get_if<Pmty_ident>(&m->desc);
      if (!pi) break;
      auto* l = std::get_if<Lident>(&pi->id.txt.v);
      if (!l) break;
      auto a = modtype_ast_.find(l->name);
      if (a == modtype_ast_.end()) break;
      m = a->second;
    }
    auto* ps = std::get_if<Pmty_signature>(&m->desc);
    if (!ps) return saved;
    for (auto& it : ps->items) {
      auto* pt = std::get_if<Psig_type>(&it.desc);
      if (!pt) continue;
      for (auto& d : pt->decls) {
        auto* v = std::get_if<Ptype_variant>(&d.kind);
        if (!v) continue;
        int nc = 0, nb = 0;
        for (auto& c : v->ctors) {
          int arity = 0; bool block = true;
          if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) { arity = (int)t->elems.size(); block = arity > 0; }
          else if (auto* r = std::get_if<Pcstr_record>(&c.args)) arity = (int)r->fields.size();
          SigCtorSave s;
          s.cname = c.name.txt;
          s.had_c = ctor_info_.count(c.name.txt) > 0;
          if (s.had_c) s.c_info = ctor_info_[c.name.txt];
          s.tname = d.name.txt;
          s.had_t = type_ctors_.count(d.name.txt) > 0;
          if (s.had_t) s.t_info = type_ctors_[d.name.txt];
          saved.push_back(std::move(s));
          ctor_info_[c.name.txt] = CtorInfo{d.name.txt, block ? nb : nc, block, arity, false};
          type_ctor_info_[d.name.txt][c.name.txt] = ctor_info_[c.name.txt];
          if (block) ++nb; else ++nc;
        }
        type_ctors_[d.name.txt] = {nc, nb};
      }
    }
    return saved;
  }
  void restore_sig_ctors(const std::vector<SigCtorSave>& saved) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->had_c) ctor_info_[it->cname] = it->c_info; else ctor_info_.erase(it->cname);
      if (it->had_t) type_ctors_[it->tname] = it->t_info; else type_ctors_.erase(it->tname);
    }
  }
  // Register the EXTENSION constructors of a functor parameter's signature
  // (`type t += A|B|C`) as fields of the parameter `pid`, shadowing a same-named
  // sibling's binding -- so `open T; match x with C -> ..` compares against the
  // param's C identity (not a sibling module's).  Needs module_layout_[prefix]
  // (register_sig_layouts) already populated for the field indices.
  struct SigExtSave { std::string name; bool had_i; Ident i; bool had_f; std::pair<Ident, int> f;
                      bool had_ci; CtorInfo ci; };
  std::vector<SigExtSave> register_sig_exts(const std::string& prefix, const Ident& pid,
                                            const ModuleType& mt) {
    std::vector<SigExtSave> saved;
    const ModuleType* m = &mt;
    for (int g = 0; g < 8; ++g) {
      auto* pi = std::get_if<Pmty_ident>(&m->desc);
      if (!pi) break;
      auto* l = std::get_if<Lident>(&pi->id.txt.v);
      if (!l) break;
      auto a = modtype_ast_.find(l->name);
      if (a == modtype_ast_.end()) break;
      m = a->second;
    }
    auto* ps = std::get_if<Pmty_signature>(&m->desc);
    if (!ps) return saved;
    auto& lay = module_layout_[prefix];
    for (auto& it : ps->items) {
      auto* tx = std::get_if<Psig_typext>(&it.desc);
      if (!tx) continue;
      for (auto& c : tx->ext.ctors) {
        auto li = lay.find(c.name.txt);
        if (li == lay.end()) continue;
        SigExtSave s;
        s.name = c.name.txt;
        s.had_i = exn_ident_.count(c.name.txt) > 0; if (s.had_i) s.i = exn_ident_[c.name.txt];
        s.had_f = exn_field_.count(c.name.txt) > 0; if (s.had_f) s.f = exn_field_[c.name.txt];
        s.had_ci = ctor_info_.count(c.name.txt) > 0; if (s.had_ci) s.ci = ctor_info_[c.name.txt];
        saved.push_back(std::move(s));
        exn_ident_.erase(c.name.txt);             // shadow a sibling's binder
        ctor_info_.erase(c.name.txt);             // ... and a same-named VARIANT ctor
        exn_field_[c.name.txt] = {pid, li->second};
      }
    }
    return saved;
  }
  void restore_sig_exts(const std::vector<SigExtSave>& saved) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->had_i) exn_ident_[it->name] = it->i; else exn_ident_.erase(it->name);
      if (it->had_f) exn_field_[it->name] = it->f; else exn_field_.erase(it->name);
      if (it->had_ci) ctor_info_[it->name] = it->ci; else ctor_info_.erase(it->name);
    }
  }
  // A pure module path (a Var, a Global, or a chain of immutable field reads of
  // one) -- safe to inline at every use of a module alias.
  static bool is_pure_path(const LamPtr& l) {
    if (!l) return false;
    if (l->k == Lam::K::Var) return true;
    if (l->k == Lam::K::Prim) {
      if (l->prim == Prim::Global) return true;
      if ((l->prim == Prim::FieldImm || l->prim == Prim::FieldInt ||
           l->prim == Prim::FieldMut) && l->args.size() == 1)
        return is_pure_path(l->args[0]);
    }
    return false;
  }
  // Field map of a stdlib (sub)module; empty if not a loadable stdlib module.
  const std::unordered_map<std::string, int>& fields_of(const std::string& mod) {
    auto it = mod_fields.find(mod);
    if (it != mod_fields.end()) return it->second;
    std::unordered_map<std::string, int> m;
    std::unordered_map<std::string, StdPrim> pr;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      int i = 0;
      for (auto& f : cmi.sig().fields) m[f] = i++;
      for (auto& v : cmi.values()) if (!v.prim.empty()) {
        pr[v.name] = {v.prim, v.prim_arity};
        // A real C primitive (`external f = "caml_.."`) used in the code makes
        // this module a REQUIRED global (linked even though the call is a Ccall,
        // not a getglobal): map the prim name -> its declaring stdlib module.
        if (v.prim[0] != '%') prim_to_mod_.emplace(v.prim, mod);
      }
    } catch (...) {}
    mod_prims[mod] = std::move(pr);
    return mod_fields[mod] = std::move(m);
  }
  // The external prim (name + arity) of a (possibly stdlib) module's value; .name
  // empty if the value is not an external.
  StdPrim value_prim(const std::string& mod, const std::string& name) {
    fields_of(mod);  // ensures mod_prims[mod] is populated
    auto& pr = mod_prims[mod];
    auto it = pr.find(name);
    return it == pr.end() ? StdPrim{"", 0} : it->second;
  }
  // If `mod.sub` is a module alias (`module List = ListLabels` in StdLabels),
  // the bare name of the aliased top-level stdlib module ("ListLabels"); else "".
  // Lets `open StdLabels; List.map` reach Stdlib__ListLabels.map like ocamlc.
  std::string stdlib_alias_target(const std::string& mod, const std::string& sub) {
    auto ck = mod + "." + sub;
    if (auto it = alias_target_cache_.find(ck); it != alias_target_cache_.end()) return it->second;
    std::string tgt;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& md : cmi.sig().modules)
        if (md.name == sub && md.type && md.type->kind == cmi::ModuleType::Alias && md.type->path) {
          std::string nm = md.type->path->kind == cmi::Path::Pident ? md.type->path->id.name
                                                                    : md.type->path->s;
          if (nm.rfind("Stdlib__", 0) == 0) nm = nm.substr(8);  // bare module name
          tgt = nm;
          break;
        }
    } catch (...) {}
    return alias_target_cache_[ck] = tgt;
  }
  std::unordered_map<std::string, std::string> alias_target_cache_;
  std::string global_of(const std::string& mod) const {
    // Stdlib and the CamlinternalXxx units are top-level compilation units; a
    // stdlib *submodule* is the `Stdlib__`-prefixed global -- recognised by its
    // stdlib__<mod>.cmi existing.  Any other module (a separately-compiled local
    // unit like `A`) is itself a top-level unit, so its global is the bare name.
    if (mod == "Stdlib" || mod.rfind("Camlinternal", 0) == 0) return mod;
    if (std::filesystem::exists(stdlib_dir + "/stdlib__" + mod + ".cmi")) return "Stdlib__" + mod;
    return mod;
  }
  // A dotted module path from a Longident ("Effect.Deep"); false on Lapply.
  static bool lid_to_dotted(const Longident& l, std::string& out) {
    if (auto* p = std::get_if<Lident>(&l.v)) { out += p->name; return true; }
    if (auto* d = std::get_if<Ldot>(&l.v)) {
      if (!lid_to_dotted(*d->prefix, out)) return false;
      out += '.'; out += d->name;
      return true;
    }
    return false;
  }
  // A stdlib *submodule* path ("Effect.Deep"): the field-index chain from the
  // head module's global down to the submodule block, plus its own field map --
  // so `Effect.Deep.continue` compiles to nested field reads.
  struct SubMod { std::vector<int> path; std::unordered_map<std::string, int> fields;
                  std::unordered_map<std::string, StdPrim> prims;
                  std::unordered_map<std::string, FnSig> sigs;  // labelled values only
                  std::string base;  // global to start `path` from (empty = the head unit)
                  bool ok = false; };
  // A cmi Path (Pident/Pdot) as a dotted string ("Shape.Uid"), for following a
  // submodule alias (`module Uid = Shape.Uid`) to its target unit + path.
  static std::string cmi_path_dotted(const cmi::Path& p) {
    if (p.kind == cmi::Path::Pident) return p.id.name;
    if (p.kind == cmi::Path::Pdot && p.a)
      return cmi_path_dotted(*p.a) + "." + p.s;
    return "";
  }
  std::unordered_map<std::string, SubMod> submod_cache_;
  // local alias name -> stdlib submodule dotted path (`module MP = Gc.Memprof`)
  std::unordered_map<std::string, std::string> submod_alias_;
  // Expand a leading submodule-alias component to its target path: with
  // `module S = Misc.Stdlib.String`, `S.Map` -> `Misc.Stdlib.String.Map` (so the
  // env.ml chain `module NameMap = S.Map` and `S.Map.empty` resolve).
  std::string expand_alias_head(std::string dotted) {
    for (int g = 0; g < 8; ++g) {
      size_t d = dotted.find('.');
      std::string head = d == std::string::npos ? dotted : dotted.substr(0, d);
      auto it = submod_alias_.find(head);
      if (it == submod_alias_.end()) break;
      dotted = it->second + (d == std::string::npos ? "" : dotted.substr(d));
    }
    return dotted;
  }
  // translating raise's argument: the position is exn-typed, so a registered
  // exception outranks a same-named variant constructor
  bool raise_arg_ = false;
  const SubMod& submodule_of(const std::string& dotted) {
    auto it = submod_cache_.find(dotted);
    if (it != submod_cache_.end()) return it->second;
    SubMod sm;
    size_t dot = dotted.find('.');
    if (dot != std::string::npos) try {
      std::string head = dotted.substr(0, dot);
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      std::vector<std::shared_ptr<cmi::CmiFile>> keep;  // park cross-unit cmis
      size_t pos = dot + 1;
      bool fail = false;
      while (!fail) {
        size_t nd = dotted.find('.', pos);
        std::string comp = dotted.substr(pos, nd == std::string::npos ? std::string::npos
                                                                      : nd - pos);
        int ix = -1;
        for (size_t i = 0; i < sig->fields.size(); ++i)
          if (sig->fields[i] == comp) { ix = (int)i; break; }
        const cmi::ModuleDecl* md = nullptr;
        for (auto& mm : sig->modules) if (mm.name == comp) { md = &mm; break; }
        // A submodule ALIAS (`module Uid = Shape.Uid`) takes no field (Mp_absent,
        // ix < 0) and resolves to ITS target unit.  Expand the path to the alias
        // target plus the remaining components and resolve THAT, recording the
        // target's head as the base global so member reads start from it.
        if (md && md->type && md->type->kind == cmi::ModuleType::Alias &&
            md->type->path) {
          std::string tgt = cmi_path_dotted(*md->type->path);
          if (!tgt.empty() && tgt.find('.') != std::string::npos) {
            std::string rest = (nd == std::string::npos) ? "" : dotted.substr(nd);
            SubMod r = submodule_of(tgt + rest);  // resolve the real path
            if (r.ok && r.base.empty()) r.base = tgt.substr(0, tgt.find('.'));
            return submod_cache_[dotted] = std::move(r);
          }
        }
        // a submodule whose type is a NAMED module type (`Digest.MD5 : S`)
        // resolves through this cmi's modtypes via mt_sig.
        const cmi::Signature* nsig = md ? mt_sig_x(cmi, md->type, keep) : nullptr;
        if (ix < 0 || !nsig) { fail = true; break; }
        sm.path.push_back(ix);
        sig = nsig;
        if (nd == std::string::npos) break;
        pos = nd + 1;
      }
      if (!fail) {
        int i = 0;
        for (auto& f : sig->fields) sm.fields[f] = i++;
        // the submodule's externals (Bigarray.Array1.get = %caml_ba_ref_1 etc.)
        for (auto& v : sig->values) {
          if (!v.prim.empty()) sm.prims[v.name] = {v.prim, v.prim_arity};
          // a labelled/optional arrow's signature, for call-site arg matching
          // (Domain.DLS.new_key ?split_from_parent needs its None filled)
          FnSig fs;
          cmi::TypePtr t = v.type;
          while (t) {
            while (t && (t->kind == cmi::TypeExpr::Tlink ||
                         t->kind == cmi::TypeExpr::Tsubst))
              t = t->link;
            if (!t || t->kind != cmi::TypeExpr::Tarrow) break;
            fs.push_back({t->label_kind, t->label});
            t = t->cod;
          }
          for (auto& [k, nm] : fs)
            if (k != 0) { sm.sigs[v.name] = fs; break; }
        }
        // Register the submodule's record types (Effect.Deep's handler etc.) so
        // record literals/projections with these labels resolve.  Only labels no
        // local type claims -- a fallback, never an ambiguity.
        for (auto& td : sig->types) {
          if (td.kind != cmi::TypeDecl::Record) continue;
          RecType rt; rt.mut = false;
          bool fresh_type = !rec_types_.count(td.name);
          for (int j = 0; j < (int)td.labels.size(); ++j) {
            ValueKind k = cmi_field_kind(td.labels[j].type);
            bool m = td.labels[j].mutable_;
            rt.mut |= m;
            rt.labels.push_back(td.labels[j].name);
            rt.shape.push_back(k);
            if (fresh_type && !field_info_.count(td.labels[j].name) &&
                !ambiguous_fields_.count(td.labels[j].name))
              field_info_[td.labels[j].name] = {td.name, j, m, k};
          }
          // On a short type-NAME collision (the bare `t` of countless submodules,
          // e.g. Persistent_env.Persistent_signature.t {filename;cmi;visibility})
          // keep the first under the bare name but ALSO register the colliding one
          // under a disambiguated key, so a `M.N.{ .. }` record LITERAL whose label
          // set only fits this record still resolves by-field-set (else -> 0).
          if (fresh_type) rec_types_[td.name] = std::move(rt);
          else if (rec_types_[td.name].labels != rt.labels)
            rec_types_[td.name + "#" + std::to_string(rec_types_.size())] =
                std::move(rt);
        }
        // Register the submodule's VARIANT constructors (Gc.Memprof's
        // `allocation_source = Normal | Marshalled | Custom`) so unqualified
        // type-directed uses resolve to their tags.  Fallback only -- never
        // override or make ambiguous a name a local/earlier type already claims.
        for (auto& td : sig->types) {
          if (td.kind != cmi::TypeDecl::Variant) continue;
          int nc = 0, nb = 0;
          for (auto& c : td.ctors) {
            bool block = !c.args.empty() || c.is_inline_record;
            int arity = c.is_inline_record ? 1 : (int)c.args.size();
            if (!ctor_info_.count(c.name) && !ambiguous_ctors_.count(c.name)) {
              CtorInfo ci{td.name, block ? nb : nc, block, arity};
              // An inline-record ctor (Patterns.Head's `Variant of {tag; cstr_row;
              // ..}`): carry its label order so a `M.Sub.Variant {tag; cstr_row}`
              // pattern binds the fields (else they fall to `?tag`/`?cstr_row`).
              if (c.is_inline_record)
                for (auto& l : c.inline_record) {
                  ci.rlabels.push_back(l.name);
                  ci.rshape.push_back(cmi_field_kind(l.type));
                  ci.rfmut.push_back(l.mutable_);
                  ci.rftypes.push_back("");
                }
              ctor_info_[c.name] = ci;
              type_ctor_info_[td.name][c.name] = std::move(ci);
            }
            if (block) ++nb; else ++nc;
          }
          if (!type_ctors_.count(td.name)) type_ctors_[td.name] = {nc, nb};
        }
        sm.ok = true;
      }
    } catch (...) {}
    return submod_cache_[dotted] = std::move(sm);
  }
  // Resolve a bare module name used as a prefix (`Variance`, `Separability`) to
  // its dotted path when it is a submodule of an opened module (`open Types`):
  // returns "Types.Variance".  Empty when it is not an opened submodule.
  std::string opened_submodule_path(const std::string& name) {
    if (name.find('.') != std::string::npos || module_base(name)) return "";
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
      if (it->find('.') == std::string::npos && !module_base(*it))
        if (submodule_of(*it + "." + name).ok) return *it + "." + name;
    return "";
  }
  // `Sub.Ctor` where Sub is a submodule of an opened module (predef.ml's
  // `Separability.Ind` under `open Types`): eagerly resolve the submodule, which
  // registers its variant constructors into ctor_info_ so the qualified ctor
  // resolves to its tag instead of staying an unbound `?Ctor`.
  void register_opened_submodule_ctors(const Longident& id) {
    auto* d = std::get_if<Ldot>(&id.v);
    if (!d) return;
    std::string prefix;
    if (!lid_to_dotted(*d->prefix, prefix)) return;
    if (prefix.find('.') != std::string::npos) { submodule_of(prefix); return; }
    if (std::string p = opened_submodule_path(prefix); !p.empty()) submodule_of(p);
  }
  StdPrim submodule_prim(const std::string& dotted, const std::string& name) {
    auto& sm = submodule_of(dotted);
    if (!sm.ok) return {"", 0};
    auto it = sm.prims.find(name);
    return it == sm.prims.end() ? StdPrim{"", 0} : it->second;
  }
  // `open M` where M is a separately-compiled module (Syntax): register M's
  // top-level record types' field labels into field_info_, so a record pattern
  // / literal using those bare labels (`fun {name; args; ..} -> ..`) resolves to
  // the right offsets.  Without this the labels stay unbound -> garbage reads.
  // (Dotted opens already do this via submodule_of; bare opens did not.)
  std::set<std::string> module_records_done_;
  // A LOCAL (same-unit) submodule's record type KEYS (rec_types_/type_field_info_
  // keys), e.g. "Unscoped" -> ["desc"], populated by register_types' submodule
  // walk.  Lets a qualified record pattern/literal `Unscoped.{name; stamp}`
  // (Ppat_open / Pexp_open of a local module) bring that record's labels into
  // scope even when they are ambiguous overall (Ident.t's inline records also
  // have name/stamp).
  std::unordered_map<std::string, std::vector<std::string>> mod_record_types_;
  // Bring a LOCAL module's record-type labels into the scoped resolution, so the
  // inner bare labels of `M.{ .. }` resolve to that record's offsets.  A later
  // top-level `type` decl re-scopes any shared label (build_module's per-decl
  // scoping), so this only governs until the next such decl -- enough for the
  // `M.{ .. }` pattern itself.
  void register_local_module_records(const std::string& mod) {
    auto it = mod_record_types_.find(mod);
    if (it == mod_record_types_.end()) return;
    for (auto& key : it->second) {
      auto tfi = type_field_info_.find(key);
      if (tfi == type_field_info_.end()) continue;
      for (auto& [fn, fi] : tfi->second) {
        field_info_[fn] = fi;
        scoped_unambig_fields_.insert(fn);
      }
    }
  }
  void register_module_records(const std::string& mod) {
    if (mod.empty() || mod.find('.') != std::string::npos) return;
    if (module_base(mod)) {  // a locally-defined module: AST-derived records
      register_local_module_records(mod);
      return;
    }
    // An imported module: load its .cmi record types.  Do NOT gate on
    // fields_of(mod) being non-empty -- a TYPES-ONLY module (Parsetree: all type
    // declarations, no values) has empty fields_of but its cmi still carries the
    // record decls (signature_item={psig_desc;psig_loc}) that `open Parsetree`
    // must register so `item.psig_desc` resolves to a field read (not 0).
    if (!module_records_done_.insert(mod).second) return;    // once
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.sig().types) {
        if (td.kind == cmi::TypeDecl::Record) {
          // An ALL-FLOAT record (Complex.t = {re;im}) has the flat Double_array_tag
          // representation; registering its plain field offsets here would build/
          // read it with the wrong (boxed) layout.  Skip such records -- this
          // helper only needs ordinary (mixed) records like ocamllex's `entry`.
          bool all_float = !td.labels.empty();
          for (auto& l : td.labels)
            if (cmi_field_kind(l.type) != ValueKind::Float) { all_float = false; break; }
          if (all_float) continue;
          RecType rt; rt.mut = false;
          bool fresh_type = !rec_types_.count(td.name);
          for (int j = 0; j < (int)td.labels.size(); ++j) {
            ValueKind k = cmi_field_kind(td.labels[j].type);
            bool m = td.labels[j].mutable_;
            rt.mut |= m; rt.labels.push_back(td.labels[j].name); rt.shape.push_back(k);
            // A later `open M` SHADOWS an earlier record's label, matching
            // OCaml's resolution of a bare label to the most-recently-opened
            // record.  Two cases, BOTH last-open-wins:
            //  - shared label across distinct types: cmt2annot's `open Typedtree`
            //    then `open Tast_iterator` -- `structure` is in BOTH
            //    Typedtree.implementation@0 and Tast_iterator.iterator@32, and
            //    `iter.structure` must take the last-opened iterator@32.
            //  - a SHORT TYPE-NAME collision: typetexp's `open Parsetree` then
            //    `open Typedtree` both define a record named `core_type`, with
            //    DISJOINT fields (ptyp_* vs ctyp_*).  Parsetree registers
            //    rec_types_["core_type"] first, so `fresh_type` is false for
            //    Typedtree's -- but its fields (ctyp_type@1 ...) must still be
            //    registered or `typ.ctyp_type` drops to 0.  So register the
            //    field regardless of `fresh_type`; only `rec_types_` (used for
            //    record CONSTRUCTION, matched by field set) stays name-keyed.
            // do NOT flag ambiguous (find_field returns null for ambiguous
            // labels, which would drop the field read to 0).
            if (!ambiguous_fields_.count(td.labels[j].name))
              field_info_[td.labels[j].name] = {td.name, j, m, k};
            // Capture the field's value type when it is a constructor of a single-
            // component-qualified module (`vb_rec_kind : Value_rec_types.recursive_
            // binding_kind`), so a bare ctor written for it resolves by type direction.
            if (auto p = field_ctor_type_path(td.labels[j].type); !p.empty())
              field_value_ctor_type_[td.labels[j].name] = std::move(p);
          }
          // Register the layout for by-field-set CONSTRUCTION lookups.  On a
          // short type-name collision (Parsetree.expression vs
          // Typedtree.expression) keep the first under the bare name but ALSO
          // register the colliding one under a disambiguated key, so a `{e
          // with l=v}` whose label set only fits the second record still
          // resolves (as the local nested-records pass already does).
          if (fresh_type) rec_types_[td.name] = std::move(rt);
          else if (rec_types_[td.name].labels != rt.labels)
            rec_types_[td.name + "#" + std::to_string(rec_types_.size())] =
                std::move(rt);
        }
        // NOTE: variant constructors are deliberately NOT registered here -- a
        // bare `open M` of a large stdlib module (Bigarray/Complex) would inject
        // its ctors globally and shadow/skew unrelated resolution.  This helper
        // exists only to resolve record PATTERN/literal labels (ocamllex's entry).
      }
    } catch (...) {}
  }
  LamPtr submodule_value(const std::string& dotted, const std::string& name) {
    auto& sm = submodule_of(dotted);
    if (!sm.ok) return nullptr;
    auto f = sm.fields.find(name);
    if (f == sm.fields.end()) return nullptr;
    auto g = mk(Lam::K::Prim); g->prim = Prim::Global;
    g->prim_id = global_of(sm.base.empty() ? dotted.substr(0, dotted.find('.'))
                                           : sm.base);
    LamPtr cur = g;
    for (int ix : sm.path) cur = fieldimm(ix, cur);
    return fieldimm(f->second, cur);
  }
  // Drop a redundant leading `Stdlib.` from a module path: `Stdlib.Array` and
  // `Stdlib.Float.Array` denote the same units as `Array` / `Float.Array`,
  // because Stdlib re-exports each stdlib unit `Foo` as an alias of the separate
  // top-level unit `Stdlib__Foo` (not as a field of Stdlib's own block).  Strip
  // only when `Foo` is genuinely a stdlib unit and not shadowed by a local one.
  std::string canon_stdlib_path(const std::string& dotted) {
    if (dotted.rfind("Stdlib.", 0) != 0) return dotted;
    std::string rest = dotted.substr(7);
    std::string head = rest.substr(0, rest.find('.'));
    if (head.empty() || module_base(head) || fields_of(head).empty()) return dotted;
    return rest;
  }
  // Resolve a stdlib module path `A[.B[.C..]]` (A a stdlib unit) to its runtime
  // value: bare `A` is the whole unit global; `A.B` is field B of A's block;
  // deeper levels walk submodule_of.
  LamPtr stdlib_module_path(const std::string& dotted) {
    size_t first = dotted.find('.');
    std::string head = first == std::string::npos ? dotted : dotted.substr(0, first);
    if (module_base(head) || fields_of(head).empty()) return nullptr;
    if (first == std::string::npos) {  // a bare stdlib unit: its global block
      auto g = mk(Lam::K::Prim); g->prim = Prim::Global; g->prim_id = global_of(head);
      return g;
    }
    if (dotted.find('.', first + 1) == std::string::npos) {  // A.B
      auto& fm = fields_of(head);
      auto f = fm.find(dotted.substr(first + 1));
      if (f == fm.end()) return nullptr;
      return field_of(global_of(head), f->second);
    }
    size_t ld = dotted.rfind('.');  // A.B...X.Y
    return submodule_value(dotted.substr(0, ld), dotted.substr(ld + 1));
  }
  // A stdlib functor (e.g. Set.Make): its field index in its module, and the
  // runtime field layouts of its parameter signature and its result signature.
  // The runtime field names of a cmi module type, resolving a named module type
  // (`Ident`, e.g. Set's OrderedType) through the same signature's modtype decls.
  static std::vector<std::string> mt_fields(const cmi::CmiFile& cmi,
                                            const cmi::ModuleTypePtr& mt, int depth = 0) {
    if (!mt || depth > 8) return {};
    if (mt->kind == cmi::ModuleType::Sig && mt->sig) return mt->sig->fields;
    if (mt->kind == cmi::ModuleType::Ident && mt->path) {
      const std::string& nm = mt->path->kind == cmi::Path::Pident ? mt->path->id.name : mt->path->s;
      for (auto& md : cmi.sig().modtypes)
        if (md.name == nm) return mt_fields(cmi, md.type, depth + 1);
    }
    return {};
  }
  struct FunctorSig { int idx = -1; std::vector<std::string> param, result; bool ok = false; };
  // Resolve a cmi module type to its concrete Signature (following a named
  // module-type Ident through the same cmi's modtype decls), or null.
  static const cmi::Signature* mt_sig(const cmi::CmiFile& cmi,
                                      const cmi::ModuleTypePtr& mt, int depth = 0) {
    if (!mt || depth > 8) return nullptr;
    if (mt->kind == cmi::ModuleType::Sig) return mt->sig.get();
    if (mt->kind == cmi::ModuleType::Ident && mt->path) {
      const std::string& nm = mt->path->kind == cmi::Path::Pident ? mt->path->id.name
                                                                  : mt->path->s;
      for (auto& md : cmi.sig().modtypes)
        if (md.name == nm) return mt_sig(cmi, md.type, depth + 1);
    }
    return nullptr;
  }
  // As mt_sig, but resolves a modtype named in ANOTHER unit (`Signatures.LIST`)
  // by loading that unit's cmi.  The loaded cmi is parked in `keep` so the
  // returned Signature* stays valid for the caller's scope.  My_std re-exports
  // `module List : Signatures.LIST`, so reading List's field layout needs it.
  const cmi::Signature* mt_sig_x(const cmi::CmiFile& cmi, const cmi::ModuleTypePtr& mt,
                                 std::vector<std::shared_ptr<cmi::CmiFile>>& keep,
                                 int depth = 0) {
    if (!mt || depth > 8) return nullptr;
    if (mt->kind == cmi::ModuleType::Sig) return mt->sig.get();
    if (mt->kind == cmi::ModuleType::Ident && mt->path) {
      if (mt->path->kind == cmi::Path::Pident) {
        for (auto& md : cmi.sig().modtypes)
          if (md.name == mt->path->id.name) return mt_sig_x(cmi, md.type, keep, depth + 1);
        return nullptr;
      }
      if (mt->path->kind == cmi::Path::Pdot && mt->path->a &&
          mt->path->a->kind == cmi::Path::Pident) {
        try {
          auto sub = std::make_shared<cmi::CmiFile>(
              cmi::CmiFile::load(resolve_cmi(mt->path->a->id.name)));
          keep.push_back(sub);
          for (auto& md : sub->sig().modtypes)
            if (md.name == mt->path->s) return mt_sig_x(*sub, md.type, keep, depth + 1);
        } catch (...) {}
      }
    }
    return nullptr;
  }
  // The labelled signature of a value in a stdlib functor's RESULT signature
  // (`Map.Make`'s `fold : f:.. -> .. -> init:.. -> ..`), for call-site reordering.
  FnSig functor_result_value_sig(const std::string& moddotted, const std::string& fname,
                                 const std::string& value) {
    FnSig s;
    try {
      size_t dot = moddotted.find('.');
      std::string head = dot == std::string::npos ? moddotted : moddotted.substr(0, dot);
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      // navigate submodules for `MoreLabels.Map` (the functor's containing module)
      for (size_t pos = dot; pos != std::string::npos;) {
        size_t nd = moddotted.find('.', pos + 1);
        std::string comp = moddotted.substr(pos + 1,
                                            nd == std::string::npos ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return s;
        sig = next; pos = nd;
      }
      for (auto& md : sig->modules) {
        if (md.name != fname || !md.type || md.type->kind != cmi::ModuleType::Functor) continue;
        const cmi::Signature* rsig = mt_sig(cmi, md.type->functor_body);
        if (!rsig) break;
        for (auto& v : rsig->values)
          if (v.name == value) {
            cmi::TypePtr t = v.type;
            while (t) {
              while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst))
                t = t->link;
              if (!t || t->kind != cmi::TypeExpr::Tarrow) break;
              s.push_back({t->label_kind, t->label});
              t = t->cod;
            }
            break;
          }
        break;
      }
    } catch (...) {}
    return s;
  }
  // The fields of a (possibly cross-module) named module type referenced from a
  // cmi -- e.g. Weak.Make's parameter `Hashtbl.HashedType`, whose modtype lives
  // in a DIFFERENT cmi than the functor's.  Empty if not an Ident or unresolved.
  std::vector<std::string> qualified_modtype_fields(const cmi::ModuleTypePtr& mt) {
    if (!mt || mt->kind != cmi::ModuleType::Ident || !mt->path) return {};
    if (mt->path->kind != cmi::Path::Pdot || !mt->path->a) return {};
    std::string mod = mt->path->a->kind == cmi::Path::Pident ? mt->path->a->id.name
                                                             : mt->path->a->s;
    std::string ty = mt->path->s;  // `Stdlib__Hashtbl.HashedType` -> Hashtbl / HashedType
    if (mod.rfind("Stdlib__", 0) == 0) mod = mod.substr(8);
    if (module_base(mod)) return {};
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& md : cmi.sig().modtypes)
        if (md.name == ty) return mt_fields(cmi, md.type);
    } catch (...) {}
    return {};
  }
  // The parameter-signature field layout of a stdlib functor named by a (possibly
  // deep) dotted path -- `Weak.Make`, `Ephemeron.K1.Make` -- navigating submodules
  // and resolving a cross-module parameter modtype.  Empty if not found.
  std::vector<std::string> stdlib_functor_param(const std::string& dotted) {
    size_t lastd = dotted.rfind('.');
    if (lastd == std::string::npos) return {};
    std::string container = dotted.substr(0, lastd), name = dotted.substr(lastd + 1);
    size_t headd = container.find('.');
    std::string head = headd == std::string::npos ? container : container.substr(0, headd);
    if (module_base(head) || fields_of(head).empty()) return {};
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t pos = headd; pos != std::string::npos;) {  // navigate Ephemeron.K1
        size_t nd = container.find('.', pos + 1);
        std::string comp = container.substr(pos + 1, nd == std::string::npos ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules) if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return {};
        sig = next; pos = nd;
      }
      for (auto& md : sig->modules) {
        if (md.name != name || !md.type || md.type->kind != cmi::ModuleType::Functor) continue;
        auto p = mt_fields(cmi, md.type->functor_param_type);
        return p.empty() ? qualified_modtype_fields(md.type->functor_param_type) : p;
      }
    } catch (...) {}
    return {};
  }
  // The runtime field layout of a (possibly deeply nested) cross-module path
  // like `Misc.Stdlib.String`, read by navigating the head module's cmi through
  // its submodules.  Empty if the head isn't a loadable module or a step is
  // missing.  Used to coerce a functor ARGUMENT that is such a nested path to
  // the functor's parameter signature (Consistbl.Make(Misc.Stdlib.String)).
  std::vector<std::string> dotted_module_fields(const std::string& dotted) {
    size_t dot = dotted.find('.');
    if (dot == std::string::npos) return {};
    std::string head = dotted.substr(0, dot);
    if (module_base(head) || fields_of(head).empty()) return {};
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t pos = dot; pos != std::string::npos;) {
        size_t nd = dotted.find('.', pos + 1);
        std::string comp = dotted.substr(pos + 1,
                                         nd == std::string::npos ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules) if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return {};
        sig = next; pos = nd;
      }
      return sig->fields;
    } catch (...) {}
    return {};
  }
  FunctorSig stdlib_functor(const std::string& mod, const std::string& name) {
    FunctorSig fs;
    auto& fm = fields_of(mod);
    auto fi = fm.find(name);
    if (fi == fm.end()) return fs;
    fs.idx = fi->second;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& md : cmi.sig().modules) {
        if (md.name != name || !md.type || md.type->kind != cmi::ModuleType::Functor) continue;
        fs.param = mt_fields(cmi, md.type->functor_param_type);
        fs.result = mt_fields(cmi, md.type->functor_body);
        fs.ok = true;
        break;
      }
    } catch (...) {}
    return fs;
  }
  // Register a cmi signature's variant constructors (fill-absent, like nested
  // local decls) so matches over a stdlib functor result's constructors
  // compile (Sys.Immediate64.Make's Immediate/Non_immediate).
  void register_cmi_sig_ctors(const cmi::Signature& sig) {
    for (auto& td : sig.types) {
      if (td.ctors.empty()) continue;
      int nc = 0, nb = 0;
      for (auto& c : td.ctors) {
        int arity = (int)(c.args.empty() ? c.inline_record.size() : c.args.size());
        bool block = arity > 0;
        if (!ctor_info_.count(c.name)) {
          builtin_ctors_.erase(c.name);
          ctor_info_[c.name] = {td.name, block ? nb : nc, block, arity};
        }
        if (block) ++nb; else ++nc;
      }
      type_ctors_.emplace(td.name, std::make_pair(nc, nb));
    }
  }
  // `open Patterns.Head` brings Head's VARIANT CONSTRUCTORS into scope and they
  // SHADOW any same-named ctor of another type (OCaml's open semantics).  This
  // matters for matching.ml: `Tuple` is ambiguous between the local `pure_arg.Tuple`
  // (tag 1) and `Patterns.Head.desc.Tuple` (tag 2); within `let open Patterns.Head
  // in match ph.pat_desc with Tuple -> ..` the open's Tuple (tag 2) must win, else
  // an int-constant head dispatches to divide_tuple (matching.ml's assert).  We
  // override the opened module's ctors scoped to the open's body, restoring on exit.
  CtorSave open_shadow_ctors(const std::string& dotted) {
    CtorSave saved;
    size_t d0 = dotted.find('.');
    std::string unit = (d0 == std::string::npos) ? dotted : dotted.substr(0, d0);
    if (module_base(unit)) return saved;  // a local module: ctors register via AST
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(unit));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t p = d0; p != std::string::npos;) {
        size_t q = dotted.find('.', p + 1);
        std::string comp =
            dotted.substr(p + 1, (q == std::string::npos ? dotted.size() : q) - p - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return saved;
        sig = next; p = q;
      }
      for (auto& td : sig->types) {
        if (td.ctors.empty()) continue;
        int nc = 0, nb = 0;
        for (auto& c : td.ctors) {
          int arity = (int)(c.args.empty() ? c.inline_record.size() : c.args.size());
          bool block = arity > 0;
          int tag = block ? nb : nc;
          auto it = ctor_info_.find(c.name);
          // override an ambiguous name bound to a DIFFERENT type, or add a fresh
          // one -- both scoped: the open's ctors are only visible in its body.
          if (it == ctor_info_.end() || it->second.type != td.name) {
            saved.emplace_back(c.name, it == ctor_info_.end()
                                           ? std::optional<CtorInfo>()
                                           : std::optional<CtorInfo>(it->second));
            builtin_ctors_.erase(c.name);
            ctor_info_[c.name] = {td.name, tag, block, arity};
          }
          if (block) ++nb; else ++nc;
        }
      }
    } catch (...) {}
    return saved;
  }
  void restore_ctors(const CtorSave& saved) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      if (it->second) ctor_info_[it->first] = *it->second;
      else ctor_info_.erase(it->first);
    }
  }
  // The runtime fields of a (possibly nested) stdlib functor's result after
  // `napps` applications -- "Sys.Immediate64.Make" applied twice yields its
  // innermost result signature ([repr]).  Registers that signature's variant
  // constructors as a side effect.  Empty when the path isn't such a functor.
  std::vector<std::string> stdlib_functor_result(const std::string& dotted, int napps) {
    size_t d0 = dotted.find('.');
    if (d0 == std::string::npos) return {};
    std::string unit = dotted.substr(0, d0);
    if (module_base(unit)) return {};
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(unit));
      const cmi::Signature* sig = &cmi.sig();
      cmi::ModuleTypePtr mt;
      for (size_t p = d0; p != std::string::npos;) {
        size_t q = dotted.find('.', p + 1);
        std::string comp =
            dotted.substr(p + 1, (q == std::string::npos ? dotted.size() : q) - p - 1);
        mt = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comp) { mt = md.type; break; }
        if (!mt) return {};
        if (q != std::string::npos) {  // an intermediate step must be a signature
          if (mt->kind != cmi::ModuleType::Sig || !mt->sig) return {};
          sig = mt->sig.get();
        }
        p = q;
      }
      for (int i = 0; i < napps && mt; ++i) {
        if (mt->kind != cmi::ModuleType::Functor) return {};
        mt = mt->functor_body;
      }
      if (!mt) return {};
      if (mt->kind == cmi::ModuleType::Sig && mt->sig) {
        register_cmi_sig_ctors(*mt->sig);
        return mt->sig->fields;
      }
      return mt_fields(cmi, mt);
    } catch (...) {}
    return {};
  }
  // The argument labels of a stdlib value's type, in order (0 Nolabel /
  // 1 Labelled / 2 Optional), so a call can insert defaults for omitted optionals.
  std::vector<int> stdlib_value_labels(const std::string& mod, const std::string& name) {
    std::vector<int> labels;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& v : cmi.values())
        if (v.name == name) {
          cmi::TypePtr t = v.type;
          while (t) {
            while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst))
              t = t->link;
            if (!t || t->kind != cmi::TypeExpr::Tarrow) break;
            labels.push_back(t->label_kind);
            t = t->cod;
          }
          break;
        }
    } catch (...) {}
    return labels;
  }
  // The (label kind, name) signature of a qualified stdlib value from its cmi
  // arrow type, for matching labelled/optional call arguments.
  FnSig stdlib_value_sig(const std::string& mod, const std::string& name) {
    FnSig s;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& v : cmi.values())
        if (v.name == name) {
          cmi::TypePtr t = v.type;
          while (t) {
            while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst))
              t = t->link;
            if (!t || t->kind != cmi::TypeExpr::Tarrow) break;
            s.push_back({t->label_kind, t->label});
            t = t->cod;
          }
          break;
        }
    } catch (...) {}
    return s;
  }
  // The labelled arrow signature of a cmi value's type (label kind per param).
  static FnSig cmi_arrow_sig(const cmi::TypePtr& type) {
    FnSig s;
    cmi::TypePtr t = type;
    while (t) {
      while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst))
        t = t->link;
      if (!t || t->kind != cmi::TypeExpr::Tarrow) break;
      s.push_back({t->label_kind, t->label});
      t = t->cod;
    }
    return s;
  }
  // Register the labelled value signatures of a functor parameter `prefix`'s
  // module type (`H : Hashtbl.SeededS`), so a call `H.create 51` can fill the
  // omitted optional `?random`.  Resolves a cross-module named modtype via its cmi.
  void register_param_value_sigs(const std::string& prefix, const ModuleType& mt) {
    if (auto* pi = std::get_if<Pmty_ident>(&mt.desc))
      if (auto* d = std::get_if<Ldot>(&pi->id.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {  // Hashtbl.SeededS
          std::string mod = pl->name;
          if (mod.rfind("Stdlib__", 0) == 0) mod = mod.substr(8);
          if (module_base(mod) || fields_of(mod).empty()) return;
          try {
            auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
            for (auto& mtd : cmi.sig().modtypes)
              if (mtd.name == d->name)
                if (const cmi::Signature* sig = mt_sig(cmi, mtd.type))
                  for (auto& v : sig->values) {
                    FnSig fs = cmi_arrow_sig(v.type);
                    for (auto& [k, n] : fs)
                      if (k != 0) { param_value_sigs_[prefix][v.name] = fs; break; }
                  }
          } catch (...) {}
        }
  }
  // Build `(apply fn args..)` inserting `0` (None) for each omitted optional
  // parameter the application passes; null if not applicable (no optionals, or a
  // labeled argument, which we don't match precisely).
  LamPtr apply_optionals(const Pexp_apply& ap, const std::vector<int>& labels) {
    if (std::find(labels.begin(), labels.end(), 2) == labels.end()) return nullptr;
    for (auto& a : ap.args) if (!std::holds_alternative<Nolabel>(a.first)) return nullptr;
    std::vector<LamPtr> args;
    size_t ai = 0, remaining = ap.args.size();
    for (size_t pi = 0; pi < labels.size() && remaining > 0; ++pi) {
      if (labels[pi] == 2) args.push_back(cint(0));  // omitted optional -> None
      else { args.push_back(expr(*ap.args[ai].second)); ++ai; --remaining; }
    }
    for (; ai < ap.args.size(); ++ai) args.push_back(expr(*ap.args[ai].second));  // over-app
    auto a = mk(Lam::K::Apply); a->fn = expr(*ap.fn); a->args = std::move(args);
    return a;
  }
  // (label kind, name) per parameter of a syntactic function -- its call
  // signature.  Follows the curried tail through sequences/lets/constraints so a
  // function whose later parameters are nested (`fun ?a -> e; fun ~b -> ...`)
  // still reports all of them.
  // Record a LOCAL functor's result-struct value label-sigs: navigate the
  // functor body to its result structure and remember each `let f = fun ..`
  // whose params carry labels/optionals, so a `module M = F(arg)` application
  // can give `M.f` the sig needed to fill omitted optionals.
  void record_local_functor_result_sigs(const std::string& fname, const ModuleExpr* body) {
    while (body) {
      if (auto* pf = std::get_if<Pmod_functor>(&body->desc)) { body = pf->body.get(); continue; }
      if (auto* pc = std::get_if<Pmod_constraint>(&body->desc)) { body = pc->me.get(); continue; }
      break;
    }
    if (!body) return;
    auto* ps = std::get_if<Pmod_structure>(&body->desc);
    if (!ps) return;
    for (auto& it : ps->items) {
      auto* pv = std::get_if<Pstr_value>(&it.desc);
      if (!pv) continue;
      for (auto& b : pv->bindings) {
        auto* nm = std::get_if<Ppat_var>(&b.pat.desc);
        if (!nm || !b.expr) continue;
        auto* fn = std::get_if<Pexp_function>(&b.expr->desc);
        if (!fn) continue;
        FnSig fs = fn_param_labels(*fn);
        for (auto& [k, n] : fs)
          if (k != 0) { local_functor_member_sig_[fname][nm->name.txt] = fs; break; }
      }
    }
  }
  static FnSig fn_param_labels(const Pexp_function& f) {
    FnSig v;
    for (auto& fp : f.params) {
      auto* pv = std::get_if<Pparam_val>(&fp.desc);
      // a `(type a)` newtype param is erased at runtime: skip it (don't bail), so a
      // GADT function `let f : type k. .. = fun ..` still reports its value params'
      // labels -- a local wrapper of it needs them to fill omitted optionals.
      if (!pv) continue;
      if (auto* lb = std::get_if<Labelled>(&pv->label)) v.push_back({1, lb->name});
      else if (auto* op = std::get_if<Optional>(&pv->label)) v.push_back({2, op->name});
      else v.push_back({0, ""});
    }
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      const Expression* e = fb->e.get();
      while (e) {  // peel an effect/binding spine down to a tail function
        if (auto* sq = std::get_if<Pexp_sequence>(&e->desc)) { e = sq->e2.get(); continue; }
        if (auto* le = std::get_if<Pexp_let>(&e->desc)) { e = le->body.get(); continue; }
        if (auto* ct = std::get_if<Pexp_constraint>(&e->desc)) { e = ct->e.get(); continue; }
        break;
      }
      if (e)
        if (auto* nf = std::get_if<Pexp_function>(&e->desc)) {
          FnSig more = fn_param_labels(*nf);
          v.insert(v.end(), more.begin(), more.end());
        }
    } else if (std::holds_alternative<Pfunction_cases>(f.body->v)) {
      v.push_back({0, ""});  // `function ...` adds one implicit positional parameter
    }
    return v;
  }
  // The tail expression a syntactic function reduces to once its parameters,
  // any nested `fun` layers, and the binding/sequence spine are peeled off.
  static const Expression* fn_tail_expr(const Pexp_function& f) {
    auto* fb = std::get_if<Pfunction_body>(&f.body->v);
    if (!fb) return nullptr;  // `function ...` cases: no simple tail
    const Expression* e = fb->e.get();
    while (e) {
      if (auto* sq = std::get_if<Pexp_sequence>(&e->desc)) { e = sq->e2.get(); continue; }
      if (auto* le = std::get_if<Pexp_let>(&e->desc)) { e = le->body.get(); continue; }
      if (auto* ct = std::get_if<Pexp_constraint>(&e->desc)) { e = ct->e.get(); continue; }
      if (auto* nf = std::get_if<Pexp_function>(&e->desc)) return fn_tail_expr(*nf);
      break;
    }
    return e;
  }
  // The parameters of `params` left unsupplied by an application's argument list
  // `as` (matching positional args to unlabelled params and labelled/optional
  // args to their named params, in parameter order) -- the residual signature of
  // a partial application.
  static FnSig residual_after_apply(
      const FnSig& params,
      const std::vector<std::pair<ArgLabel, ExprBox>>& as) {
    auto alab = [](const ArgLabel& a, std::string& nm) -> int {
      if (auto* lb = std::get_if<Labelled>(&a)) { nm = lb->name; return 1; }
      if (auto* op = std::get_if<Optional>(&a)) { nm = op->name; return 2; }
      return 0;
    };
    std::vector<bool> used(as.size(), false);
    FnSig resid;
    for (auto& [pk, pn] : params) {
      int found = -1;
      for (size_t i = 0; i < as.size(); ++i) {
        if (used[i]) continue;
        std::string nm; int k = alab(as[i].first, nm);
        if (pk == 0 && k == 0) { found = (int)i; break; }
        if (pk == 1 && k == 1 && nm == pn) { found = (int)i; break; }
        if (pk == 2 && (k == 1 || k == 2) && nm == pn) { found = (int)i; break; }
      }
      if (found < 0) resid.push_back({pk, pn});
      else used[found] = true;
    }
    return resid;
  }
  // Record a binding's parameter labels (only if it is a function with at least
  // one labelled/optional parameter), so its call sites can reorder/wrap args.
  void record_fn_sig(const Ident& id, const Expression* e) {
    if (!e) return;
    // `let f : type k. t = fun ..` wraps the function in newtype/constraint nodes
    // (one Pexp_newtype per locally-abstract type); peel them so the inner
    // function's labelled signature is still recorded.
    while (e) {
      if (auto* nt = std::get_if<Pexp_newtype>(&e->desc)) { e = nt->body.get(); continue; }
      if (auto* ct = std::get_if<Pexp_constraint>(&e->desc)) { e = ct->e.get(); continue; }
      break;
    }
    if (!e) return;
    if (auto* f = std::get_if<Pexp_function>(&e->desc)) {
      // first-class-module parameters `(module P : S)`: record each positional
      // param's package type so un-annotated `(module M)` arguments coerce
      std::vector<std::string> packs; bool any_pack = false;
      for (auto& fp : f->params)
        if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
          const Pattern* p = &pv->pat;
          while (auto* pc = std::get_if<Ppat_constraint>(&p->desc)) p = pc->p.get();
          std::string mt;
          if (auto* up = std::get_if<Ppat_unpack>(&p->desc); up && up->pkg)
            if (lid_to_dotted(up->pkg->path.txt, mt) && !mt.empty()) any_pack = true;
          packs.push_back(mt);
        }
      if (any_pack) fn_pack_params_[id.stamp] = std::move(packs);
      FnSig s = fn_param_labels(*f);
      // If the function's body is a partial application of a labelled/optional
      // function (`let fpm name = base penv f name` where `base ~allow_hidden ..`
      // leaves `~allow_hidden` unsupplied), append the residual parameters so
      // THIS function's call sites reorder/fill them too.
      if (const Expression* body = fn_tail_expr(*f))
        if (auto* ap = std::get_if<Pexp_apply>(&body->desc)) {
          FnSig callee = callee_sig(ap->fn.get());
          if (!callee.empty()) {
            FnSig resid = residual_after_apply(callee, ap->args);
            s.insert(s.end(), resid.begin(), resid.end());
          }
        }
      for (auto& [k, n] : s) if (k != 0) { fn_sig_[id.stamp] = s; return; }
    }
  }
  // `let f, g = M.(a, b)`: each tuple component that aliases a labelled function
  // (qualified, or a bare name under the wrapping local open) gets its source's
  // parameter signature, so call sites still reorder/fill labelled args
  // (Format.(pp_print_custom_break) was applied with verbatim arg order).
  void record_tuple_sigs(const Pattern& pat0, const Expression& ex) {
    const Expression* e = &ex;
    std::string open_mod;
    while (auto* sti = std::get_if<Pexp_struct_item>(&e->desc)) {
      if (auto* po = std::get_if<Pstr_open>(&sti->item->desc))
        if (auto* mi = std::get_if<Pmod_ident>(&po->expr.desc))
          lid_to_dotted(mi->id.txt, open_mod);
      e = sti->body.get();
    }
    const Pattern* pat = effective_pat(&pat0);
    auto* tp = std::get_if<Ppat_tuple>(&pat->desc);
    auto* te = std::get_if<Pexp_tuple>(&e->desc);
    if (!tp || !te || tp->elems.size() != te->elems.size()) return;
    for (size_t i = 0; i < tp->elems.size(); ++i) {
      auto* pv = std::get_if<Ppat_var>(&effective_pat(tp->elems[i].get())->desc);
      if (!pv) continue;
      const Ident* bid = lookup(pv->name.txt);
      if (!bid) continue;
      FnSig s = callee_sig(te->elems[i].get());
      if (s.empty() && !open_mod.empty())
        if (auto* idc = std::get_if<Pexp_ident>(&te->elems[i]->desc))
          if (auto* lc = std::get_if<Lident>(&idc->id.txt.v)) {
            if (open_mod.find('.') == std::string::npos)
              s = stdlib_value_sig(open_mod, lc->name);
            else {
              auto& sm = submodule_of(open_mod);
              if (auto f = sm.sigs.find(lc->name); sm.ok && f != sm.sigs.end())
                s = f->second;
            }
          }
      for (auto& [k, n2] : s)
        if (k != 0) { fn_sig_[bid->stamp] = s; break; }
    }
  }
  // The callee's parameter signature for an application: a local function (by its
  // recorded sig) or a qualified stdlib value (from its cmi arrow type).  Empty if
  // unknown or unlabelled.
  // The label signature of a function type, expanding a leading local type alias
  // (`value_description core_incl` -> core_incl's `loc:.. -> Env.t -> direction:..`
  // arrow), then reading each arrow arg's label kind/name.  Empty if not an arrow
  // or has no labelled/optional argument (a positional-only callee needs no sig).
  FnSig coretype_label_sig(const CoreType* t, int depth = 0) {
    if (!t || depth > 8) return {};
    if (auto* c = std::get_if<Ptyp_constr>(&t->desc)) {
      auto it = local_alias_.find(lid_last(c->id.txt));
      if (it == local_alias_.end()) return {};
      return coretype_label_sig(it->second, depth + 1);
    }
    FnSig s;
    const CoreType* cur = t;
    while (cur) {
      if (auto* c = std::get_if<Ptyp_constr>(&cur->desc)) {  // a mid/tail alias arrow
        auto it = local_alias_.find(lid_last(c->id.txt));
        if (it == local_alias_.end() || ++depth > 8) break;
        cur = it->second; continue;
      }
      auto* ar = std::get_if<Ptyp_arrow>(&cur->desc);
      if (!ar) break;
      int k = 0; std::string nm;
      if (auto* lb = std::get_if<Labelled>(&ar->label)) { k = 1; nm = lb->name; }
      else if (auto* op = std::get_if<Optional>(&ar->label)) { k = 2; nm = op->name; }
      s.push_back({k, nm});
      cur = ar->cod.get();
    }
    for (auto& [k, n] : s) if (k != 0) return s;  // only worth it with a label
    return {};
  }
  FnSig callee_sig(const Expression* fn) {
    // A call through a record-field projection of a labelled function.
    if (auto* fe = std::get_if<Pexp_field>(&fn->desc)) {
      auto it = field_fn_sig_.find(lid_last(fe->field.txt));
      if (it != field_fn_sig_.end()) return it->second;
      return {};
    }
    auto* id = std::get_if<Pexp_ident>(&fn->desc);
    if (!id) return {};
    if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
      if (auto* b = lookup(l->name)) {
        auto it = fn_sig_.find(b->stamp);
        if (it != fn_sig_.end()) return it->second;
        return {};  // a local binding shadows any opened-module value
      }
      // An unqualified name brought into scope by `open M` / `M.(..)`: the value's
      // label sig from M's cmi (so a local-open partial application like
      // `Profile.(record transl)` fills record's omitted optional `?accumulate`).
      if (!opened_has(l->name)) return {};
      for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
        if (module_base(*it)) continue;
        if (it->find('.') != std::string::npos) {  // an opened submodule path
          auto& sm = submodule_of(*it);
          if (!sm.ok) continue;
          if (auto sit = sm.sigs.find(l->name); sit != sm.sigs.end()) return sit->second;
          if (sm.fields.count(l->name)) return {};  // a known plain value here
          continue;
        }
        FnSig s = stdlib_value_sig(*it, l->name);
        for (auto& [k, n] : s) if (k != 0) return s;
        if (fields_of(*it).count(l->name)) return {};  // shadows outer opens
      }
    } else if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
      if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
        // `DLS.new_key` where DLS is a local submodule of THIS unit: its value
        // label sig from this unit's own .cmi (fills the omitted optional).
        if (auto it = local_member_sig_.find(pl->name); it != local_member_sig_.end())
          if (auto vit = it->second.find(d->name); vit != it->second.end())
            return vit->second;
        // `H.create` where H is a functor parameter: its sig's labelled value sig.
        if (auto it = param_value_sigs_.find(pl->name); it != param_value_sigs_.end())
          if (auto vit = it->second.find(d->name); vit != it->second.end())
            return vit->second;
        // `Subst.fold` where Subst = Map.Make(..): the labelled result signature.
        // A local `module Hashtbl = Hashtbl.Make(..)` SHADOWS any stdlib module of
        // the same name, so the functor result's signature is authoritative -- we
        // must NOT fall through to the stdlib value below (whose `create` carries a
        // `?random` the result's `create` lacks, which would insert a bogus None
        // and over-apply).  Return the result sig even when it has no labels.
        if (auto fs = module_functor_src_.find(pl->name); fs != module_functor_src_.end())
          return functor_result_value_sig(fs->second.first, fs->second.second, d->name);
        FnSig s = stdlib_value_sig(pl->name, d->name);
        for (auto& [k, n] : s) if (k != 0) return s;
        // `open StdLabels; List.map` -> the label sig of ListLabels.map.
        for (auto oit = opened_.rbegin(); oit != opened_.rend(); ++oit) {
          if (module_base(*oit)) continue;
          std::string tgt = stdlib_alias_target(*oit, pl->name);
          if (tgt.empty()) continue;
          FnSig s2 = stdlib_value_sig(tgt, d->name);
          for (auto& [k, n] : s2) if (k != 0) return s2;
        }
      }
      // a stdlib submodule's value (Domain.DLS.new_key), incl. an opened head
      // and a local alias (`module MP = Gc.Memprof` -> MP.start)
      std::string dotted;
      if (lid_to_dotted(*d->prefix, dotted)) {
        if (auto sa = submod_alias_.find(dotted); sa != submod_alias_.end())
          dotted = sa->second;
        std::vector<std::string> cands;
        if (dotted.find('.') != std::string::npos) cands.push_back(dotted);
        for (auto it2 = opened_.rbegin(); it2 != opened_.rend(); ++it2)
          if (it2->find('.') == std::string::npos)
            cands.push_back(*it2 + "." + dotted);
        for (auto& cand : cands) {
          auto& sm = submodule_of(cand);
          if (!sm.ok) continue;
          if (auto it3 = sm.sigs.find(d->name); it3 != sm.sigs.end())
            return it3->second;
          if (sm.fields.count(d->name)) break;  // known value, no labels
        }
      }
    }
    return {};
  }
  // Append args to an application (merging into an existing apply, like
  // translcore's lapply).
  LamPtr lapply_(LamPtr fn, std::vector<LamPtr> args) {
    if (args.empty()) return fn;
    if (fn->k == Lam::K::Apply) { for (auto& a : args) fn->args.push_back(a); return fn; }
    auto a = mk(Lam::K::Apply); a->fn = fn; a->args = std::move(args); return a;
  }
  // One element of the parameter-ordered argument list: an Arg (its translated
  // value) or an Omitted slot, with the parameter's optional flag.
  struct AppArg { LamPtr val; bool omitted; bool optional; };
  // Port of translcore's build_apply: build the application, evaluating arguments
  // right-to-left in parameter order, and -- for an out-of-order partial
  // application (an Omitted slot before a later Arg) -- protecting the already
  // computed pieces in `let`s and wrapping the rest in a `stub` closure of the
  // omitted parameters.  `acc` accumulates the consumed Args (reversed).
  LamPtr build_apply(LamPtr lam, std::vector<std::pair<LamPtr, bool>> acc,
                     const std::vector<AppArg>& list, size_t i) {
    if (i >= list.size()) {  // [] -> apply lam to the accumulated args (un-reversed)
      std::vector<LamPtr> a;
      for (auto it = acc.rbegin(); it != acc.rend(); ++it) a.push_back(it->first);
      return lapply_(lam, std::move(a));
    }
    if (!list[i].omitted) {  // Arg -> accumulate (prepend) and continue
      acc.insert(acc.begin(), {list[i].val, list[i].optional});
      return build_apply(lam, std::move(acc), list, i + 1);
    }
    // Omitted: out-of-order partial application -> a closure over this parameter.
    std::vector<std::pair<Ident, LamPtr>> defs;
    auto protect = [&](const std::string& nm, LamPtr l) -> LamPtr {
      if (l->k == Lam::K::Var || is_const(l)) return l;
      Ident id = fresh(nm);
      defs.push_back({id, l});
      auto v = mk(Lam::K::Var); v->var = id; return v;
    };
    bool all_opt = true;
    for (auto& [a, o] : acc) if (!o) all_opt = false;
    std::vector<std::pair<LamPtr, bool>> kept, delayed;
    if (all_opt) delayed = acc; else kept = acc;  // delay all-optional args past here
    LamPtr lam2 = lam;
    if (!kept.empty()) {
      std::vector<LamPtr> a;
      for (auto it = kept.rbegin(); it != kept.rend(); ++it) a.push_back(it->first);
      lam2 = lapply_(lam, std::move(a));
    }
    LamPtr handle = protect("func", lam2);
    std::vector<std::pair<LamPtr, bool>> acc2;
    for (auto& [a, o] : delayed) acc2.push_back({protect("arg", a), o});
    std::vector<AppArg> rest;
    for (size_t j = i + 1; j < list.size(); ++j) {
      AppArg aa = list[j];
      if (!aa.omitted) aa.val = protect("arg", aa.val);
      rest.push_back(aa);
    }
    Ident id_arg = fresh("param");
    auto idv = mk(Lam::K::Var); idv->var = id_arg;
    std::vector<std::pair<LamPtr, bool>> recacc;
    recacc.push_back({idv, list[i].optional});
    for (auto& a : acc2) recacc.push_back(a);
    LamPtr body = build_apply(handle, std::move(recacc), rest, 0);
    LamPtr fn;  // merge into a curried function when the recursion already is one
    if (body->k == Lam::K::Function && (int)body->params.size() < 120) {
      body->params.insert(body->params.begin(), {id_arg, ValueKind::Gen});
      fn = body;
    } else {
      fn = mk(Lam::K::Function); fn->params = {{id_arg, ValueKind::Gen}};
      fn->inline_attr = "stub"; fn->body = body;
    }
    for (auto& [id, l] : defs) {  // wrap the protected defs (last-pushed outermost)
      auto let = mk(Lam::K::Let); let->bindings = {{id, ValueKind::Gen, l}};
      let->body = fn; fn = let;
    }
    return fn;
  }
  // Match a call's arguments to the callee's parameter labels (reorder labelled
  // args, wrap a `~l:e` optional as `Some e`, pass a `?l:e` one directly, mark a
  // missing parameter Omitted), then build the application via build_apply.  Null
  // on a shape we can't place safely (caller then applies the args verbatim).
  LamPtr apply_labeled(const Expression* fnexpr, const FnSig& params, const Pexp_apply& ap) {
    const auto& as = ap.args;
    auto alabel = [&](size_t i, std::string& nm) -> int {
      if (auto* lb = std::get_if<Labelled>(&as[i].first)) { nm = lb->name; return 1; }
      if (auto* op = std::get_if<Optional>(&as[i].first)) { nm = op->name; return 2; }
      return 0;
    };
    LamPtr fn = expr(*fnexpr);  // the function is translated before its arguments
    std::vector<bool> used(as.size(), false);
    std::vector<AppArg> list;
    int last_arg = -1;
    for (auto& [pk, pn] : params) {
      int found = -1, fk = 0;
      for (size_t i = 0; i < as.size(); ++i) {
        if (used[i]) continue;
        std::string nm; int k = alabel(i, nm);
        if (pk == 0 && k == 0) { found = (int)i; break; }
        if (pk == 1 && k == 1 && nm == pn) { found = (int)i; break; }
        if (pk == 2 && (k == 1 || k == 2) && nm == pn) { found = (int)i; fk = k; break; }
      }
      if (found < 0) {
        // An omitted optional is filled with None (0) ONLY when a later POSITIONAL
        // (Nolabel) argument forces the default (`Hashtbl.create 51` defaults
        // ?random because 51 is supplied past it).  When no un-used positional
        // remains -- a PARTIAL application supplying only a LATER labelled arg
        // (`add_value ?shape:None`, where ?check precedes ?shape) -- the earlier
        // optional must stay a parameter of the result (eta), NOT be defaulted;
        // ocamlc emits `fun check -> impl check None`, not `impl None None`.
        // An omitted non-optional (a skipped label) is always an eta (Omitted) slot.
        if (pk == 2) {
          bool later_positional = false;
          for (size_t i = 0; i < as.size(); ++i)
            if (!used[i]) { std::string nm; if (alabel(i, nm) == 0) { later_positional = true; break; } }
          if (later_positional) list.push_back({cint(0), false, true});  // -> None
          else list.push_back({nullptr, true, true});                    // -> eta param
        } else {
          list.push_back({nullptr, true, false});
        }
        continue;
      }
      used[found] = true;
      LamPtr v = expr(*as[found].second);
      if (pk == 2 && fk == 1) {  // ~l:e on an optional param -> Some e
        ValueKind vk = expr_kind(as[found].second.get());
        v = block(0, {v});
        if (v->k == Lam::K::Prim) v->blk_shape = {vk};  // a dynamic Some -> field shape
      }
      list.push_back({v, false, pk == 2});
      last_arg = (int)list.size() - 1;
    }
    if (last_arg < 0) return nullptr;  // nothing matched -> verbatim apply
    list.resize(last_arg + 1);  // drop trailing Omitted (params beyond the call)
    bool has_omitted = false;
    for (auto& a : list) if (a.omitted) has_omitted = true;
    std::vector<LamPtr> leftover;
    for (size_t i = 0; i < as.size(); ++i)
      if (!used[i]) {
        std::string nm;
        if (alabel(i, nm) != 0) return nullptr;  // a stray labelled over-app arg
        leftover.push_back(expr(*as[i].second));
      }
    if (!leftover.empty() && has_omitted) return nullptr;  // over-app + gap: too complex
    LamPtr r = build_apply(fn, {}, list, 0);
    return lapply_(r, std::move(leftover));
  }
  // Whether a pure path bottoms out in a global (a stdlib module) rather than a
  // local Var -- a global functor/argument is wrapped in `(let (let/N = p) ..)`.
  static bool is_global_path(const LamPtr& l) {
    if (!l) return false;
    if (l->k == Lam::K::Prim && l->prim == Prim::Global) return true;
    if (l->k == Lam::K::Prim && !l->args.empty() &&
        (l->prim == Prim::FieldImm || l->prim == Prim::FieldInt || l->prim == Prim::FieldMut))
      return is_global_path(l->args[0]);
    return false;
  }
  // The module-qualified type path ("Module.type") of a cmi field type that is a
  // constructor application headed by a `Module.type` Pdot path whose prefix is a
  // single module component; "" otherwise (a builtin, a bare-local type, or a
  // nested-module path register_ctors_of_type cannot use anyway).
  static std::string field_ctor_type_path(const cmi::TypePtr& t0) {
    cmi::TypePtr t = t0;
    while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst)) t = t->link;
    if (!t || t->kind != cmi::TypeExpr::Tconstr || !t->path) return {};
    const cmi::Path& p = *t->path;
    if (p.kind != cmi::Path::Pdot || !p.a || p.a->kind != cmi::Path::Pident) return {};
    return p.a->id.name + "." + p.s;
  }
  // The value kind of a cmi field type (int/char/bool/unit -> int, float ->
  // float, everything else -> generic/boxed), for spelling its field read.
  static ValueKind cmi_field_kind(const cmi::TypePtr& t0) {
    cmi::TypePtr t = t0;
    while (t && (t->kind == cmi::TypeExpr::Tlink || t->kind == cmi::TypeExpr::Tsubst)) t = t->link;
    if (!t || t->kind != cmi::TypeExpr::Tconstr || !t->path) return ValueKind::Gen;
    const std::string& n = t->path->kind == cmi::Path::Pident ? t->path->id.name : t->path->s;
    if (n == "int" || n == "char" || n == "bool" || n == "unit") return ValueKind::Int;
    if (n == "float") return ValueKind::Float;
    return ValueKind::Gen;
  }
  // A record field of a stdlib (sub)module's record type, e.g. `Gc.minor_heap_size`
  // -> {field index, kind, mutable}; nullopt if not found.
  struct StdField { int index; ValueKind kind; bool mut; bool flat = false; };
  // True when every label of `td` is float-kind (a flat float record, read/written
  // with floatfield rather than the boxed field ops).
  static bool record_all_float(const cmi::TypeDecl& td) {
    for (auto& l : td.labels) if (cmi_field_kind(l.type) != ValueKind::Float) return false;
    return !td.labels.empty();
  }
  std::optional<StdField> stdlib_record_field(const std::string& mod, const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        for (int i = 0; i < (int)td.labels.size(); ++i)
          if (td.labels[i].name == label)
            return StdField{i, cmi_field_kind(td.labels[i].type), td.labels[i].mutable_,
                            record_all_float(td)};
      }
    } catch (...) {}
    return std::nullopt;
  }
  // A qualified label `M.label` where M is a LOCAL (same-unit) submodule: the
  // field of M's record type (via mod_record_types_).  The qualifier is
  // authoritative -- it must win over a bare find_field that could pick a
  // different record sharing the ambiguous label (load_path's `dir.Dir.hidden`
  // [field 2] vs the top-level paths record's `hidden` [field 1]).
  std::optional<StdField> local_module_field(const std::string& mod, const std::string& label) {
    auto it = mod_record_types_.find(mod);
    if (it == mod_record_types_.end()) return std::nullopt;
    for (auto& key : it->second) {
      auto tfi = type_field_info_.find(key);
      if (tfi == type_field_info_.end()) continue;
      auto f = tfi->second.find(label);
      if (f != tfi->second.end()) {
        auto rt = rec_types_.find(key);
        bool flat = rt != rec_types_.end() && rt->second.flat;
        return StdField{f->second.index, f->second.kind, f->second.mut, flat};
      }
    }
    return std::nullopt;
  }
  // Resolve a qualified record label (`M.l` local or stdlib, or deep `A.B.l`).
  std::optional<StdField> qualified_field(const Longident& lid) {
    auto* d = std::get_if<Ldot>(&lid.v);
    if (!d) return std::nullopt;
    if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
      if (auto rf = local_module_field(pl->name, d->name)) return rf;
      return stdlib_record_field(pl->name, d->name);
    }
    std::string dotted;
    if (lid_to_dotted(*d->prefix, dotted)) return nested_record_field(dotted, d->name);
    return std::nullopt;
  }
  // An unqualified label resolved through the base expression's INFERRED type:
  // `heap_stats.major_collections` with heap_stats : Gc.stat reads the labeled
  // field of the record decl `stat` in gc.cmi.
  // Find record type `ty`'s field `label` in a top-level module's cmi.
  std::optional<StdField> toplevel_typed_record_field(const std::string& mod,
                                                      const std::string& ty,
                                                      const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record || td.name != ty) continue;
        for (int i = 0; i < (int)td.labels.size(); ++i)
          if (td.labels[i].name == label)
            return StdField{i, cmi_field_kind(td.labels[i].type), td.labels[i].mutable_,
                            record_all_float(td)};
      }
    } catch (...) {}
    return std::nullopt;
  }
  // Same, but for a NESTED module path ("Ctype.Pattern_env"): navigate the head
  // unit's cmi through the named submodules, then find record `ty`'s `label`.
  std::optional<StdField> nested_typed_record_field(const std::string& dotted,
                                                    const std::string& ty,
                                                    const std::string& label) {
    size_t dot = dotted.find('.');
    if (dot == std::string::npos) return toplevel_typed_record_field(dotted, ty, label);
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(dotted.substr(0, dot)));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t pos = dot; pos != std::string::npos;) {
        size_t nd = dotted.find('.', pos + 1);
        std::string comp = dotted.substr(pos + 1, nd == std::string::npos
                                                      ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules)
          if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return std::nullopt;
        sig = next; pos = nd;
      }
      for (auto& td : sig->types) {
        if (td.kind != cmi::TypeDecl::Record || td.name != ty) continue;
        for (int i = 0; i < (int)td.labels.size(); ++i)
          if (td.labels[i].name == label)
            return StdField{i, cmi_field_kind(td.labels[i].type), td.labels[i].mutable_,
                            record_all_float(td)};
      }
    } catch (...) {}
    return std::nullopt;
  }
  // The inferred module-qualified type path of a record expression: the
  // expression node itself if the checker typed it, else its `with`-base
  // (a `{base with ..}` node often goes untyped while `base` does not).
  decltype(ValueKinds{}.expr_constr)::const_iterator
  expr_constr_of(const Expression* e, const Expression* base) {
    auto it = vk.expr_constr.find(e);
    if (it != vk.expr_constr.end()) return it;
    return vk.expr_constr.find(base);
  }
  std::optional<StdField> inferred_record_field(const Expression* base,
                                                const std::string& label) {
    auto it = vk.expr_constr.find(base);
    if (it == vk.expr_constr.end()) return std::nullopt;
    const std::string& p = it->second;
    auto d = p.rfind('.');
    std::string mod = p.substr(0, d), ty = p.substr(d + 1);
    // `mod` may itself be dotted (an already-qualified nested type path).
    if (mod.find('.') != std::string::npos)
      return nested_typed_record_field(mod, ty, label);
    // A top-level module: resolve from its own cmi.
    if (auto rf = toplevel_typed_record_field(mod, ty, label)) return rf;
    // Else `mod` is a SUBMODULE reached via `open M` (Ctype's `Pattern_env`,
    // written `Pattern_env.t` after `open Ctype`): resolve to its full dotted
    // path and navigate.  Without this, `penv.env` (penv : Pattern_env.t) drops
    // to a 0 field read.
    if (std::string full = opened_submodule_path(mod); !full.empty())
      return nested_typed_record_field(full, ty, label);
    return std::nullopt;
  }
  // The full layout of the stdlib record type (in module `mod`'s cmi) declaring
  // `label`, for unqualified-label updates of non-opened stdlib records
  // (`{ (Gc.get ()) with allocation_policy = 2 }`). All-float (flat) records
  // have a different representation and are not handled.
  struct StdRec { std::vector<std::string> labels; std::vector<ValueKind> shape; std::vector<bool> mut;
                  bool flat = false; };  // all-float -> a flat float block (Complex.t)
  std::optional<StdRec> stdlib_record_layout(const std::string& mod, const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        bool has = false;
        for (auto& l : td.labels) if (l.name == label) { has = true; break; }
        if (!has) continue;
        StdRec r;
        bool all_float = true;
        for (auto& l : td.labels) {
          r.labels.push_back(l.name);
          r.shape.push_back(cmi_field_kind(l.type));
          r.mut.push_back(l.mutable_);
          if (r.shape.back() != ValueKind::Float) all_float = false;
        }
        r.flat = all_float;  // Complex.t etc.: a flat float block
        return r;
      }
    } catch (...) {}
    return std::nullopt;
  }
  // Same, but keyed by the record TYPE name ("Gc.control" -> mod Gc, ty control)
  // for resolution through an inferred type path.
  std::optional<StdRec> stdlib_record_layout_named(const std::string& mod,
                                                   const std::string& ty) {
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record || td.name != ty) continue;
        StdRec r;
        bool all_float = true;
        for (auto& l : td.labels) {
          r.labels.push_back(l.name);
          r.shape.push_back(cmi_field_kind(l.type));
          r.mut.push_back(l.mutable_);
          if (r.shape.back() != ValueKind::Float) all_float = false;
        }
        r.flat = all_float;  // Complex.t etc.: a flat float block
        return r;
      }
    } catch (...) {}
    return std::nullopt;
  }
  // A record layout from a NESTED module path (`CamlinternalMenhirLib.EngineTypes`):
  // navigate the head module's cmi through its submodules, then find the record
  // A lazily-built index of every include-path record type's SORTED label set ->
  // its layout.  Resolves an UNQUALIFIED record literal of an IMPORTED record that
  // is in scope only by type-directed disambiguation (typecore's `{pattern;
  // has_guard; needs_refute}` = Parmatch.parmatch_case, with no `open Parmatch`).
  // Consulted ONLY as a last resort -- a literal that would otherwise collapse to
  // 0 -- and only when the label set maps to exactly ONE record across the path,
  // so it can never change a record that already resolves.
  // A cmi Path -> its dotted name ("lfunction", "M.t").
  static std::string cmi_path_dotted(const cmi::PathPtr& p) {
    if (!p) return "";
    if (p->kind == cmi::Path::Pident) return p->id.name;
    if (p->kind == cmi::Path::Pdot) {
      std::string pre = cmi_path_dotted(p->a);
      return pre.empty() ? p->s : pre + "." + p->s;
    }
    return "";
  }
  // The dotted type-constructor path of a cmi field's type ("lfunction"), else "".
  static std::string cmi_type_ctor_path(const cmi::TypePtr& t) {
    if (!t || t->kind != cmi::TypeExpr::Tconstr) return "";
    return cmi_path_dotted(t->path);
  }
  bool labelset_index_built_ = false;
  std::map<std::vector<std::string>, StdRec> labelset_index_;
  std::set<std::vector<std::string>> labelset_ambiguous_;
  // Per record-type-NAME: its layout, and each field's declared type path -- so a
  // nested record pattern (`{ def = {params;body} }`) resolves through the enclosing
  // field's value type (def : lfunction), disambiguating an ambiguous label set.
  std::map<std::string, StdRec> record_by_name_;
  std::map<std::string, std::map<std::string, std::string>> record_field_types_;
  std::set<std::string> record_by_name_ambig_;
  // A variant constructor's argument type paths (qualified with the defining
  // module), so a record arg pattern (`Sig_module(_,_,{md_type=..},_,_)`) resolves
  // its AMBIGUOUS label through Types.module_declaration, not the last-registered.
  std::map<std::string, std::vector<std::string>> ctor_arg_paths_;
  std::set<std::string> ctor_arg_paths_ambig_;
  void build_labelset_index() {
    if (labelset_index_built_) return;
    labelset_index_built_ = true;
    std::vector<std::string> dirs = module_dirs_;
    dirs.push_back(stdlib_dir);
    for (auto& d : dirs) {
      std::error_code ec;
      if (d.empty() || !std::filesystem::is_directory(d, ec)) continue;
      for (auto& ent : std::filesystem::directory_iterator(d, ec)) {
        if (ent.path().extension() != ".cmi") continue;
        try {
          auto cmi = cmi::CmiFile::load(ent.path().string());
          std::string modname = ent.path().stem().string();
          if (!modname.empty()) modname[0] = (char)std::toupper((unsigned char)modname[0]);
          for (auto& td : cmi.sig().types) {
            if (td.kind == cmi::TypeDecl::Variant && !td.ctors.empty())
              for (auto& c : td.ctors) {
                if (ctor_arg_paths_ambig_.count(c.name)) continue;
                std::vector<std::string> argpaths;
                for (auto& a : c.args) {
                  std::string p = cmi_type_ctor_path(a);
                  // a local (bare) type of this cmi -> qualify with its module
                  if (!p.empty() && p.find('.') == std::string::npos) p = modname + "." + p;
                  argpaths.push_back(p);
                }
                auto ci = ctor_arg_paths_.find(c.name);
                if (ci == ctor_arg_paths_.end()) ctor_arg_paths_[c.name] = std::move(argpaths);
                else if (ci->second != argpaths) {  // same ctor name, different module
                  ctor_arg_paths_.erase(ci); ctor_arg_paths_ambig_.insert(c.name);
                }
              }
            if (td.kind != cmi::TypeDecl::Record || td.labels.empty()) continue;
            StdRec r; bool all_float = true;
            std::vector<std::string> key;
            std::map<std::string, std::string> ftypes;
            for (auto& l : td.labels) {
              r.labels.push_back(l.name); key.push_back(l.name);
              r.shape.push_back(cmi_field_kind(l.type));
              r.mut.push_back(l.mutable_);
              if (r.shape.back() != ValueKind::Float) all_float = false;
              if (std::string fp = cmi_type_ctor_path(l.type); !fp.empty())
                ftypes[l.name] = fp;
            }
            r.flat = all_float;
            // Index by NAME (for resolving through a field's value type).
            if (!record_by_name_ambig_.count(td.name)) {
              auto ni = record_by_name_.find(td.name);
              if (ni == record_by_name_.end()) {
                record_by_name_[td.name] = r;
                record_field_types_[td.name] = std::move(ftypes);
              } else if (ni->second.labels != r.labels) {
                record_by_name_.erase(ni); record_field_types_.erase(td.name);
                record_by_name_ambig_.insert(td.name);
              }
            }
            std::sort(key.begin(), key.end());
            if (labelset_ambiguous_.count(key)) continue;
            auto it = labelset_index_.find(key);
            if (it == labelset_index_.end()) labelset_index_[key] = std::move(r);
            else if (it->second.labels != r.labels) {  // a genuinely different layout
              labelset_index_.erase(it); labelset_ambiguous_.insert(key);
            }
          }
        } catch (...) {}
      }
    }
  }
  // A var bound to a record LITERAL whose label set uniquely identifies one record
  // type: remember that type's layout so a later `v.label` reads the right offset
  // even when the label is AMBIGUOUS across records (`md.md_type` --
  // Types.module_declaration@0 vs Typedtree.module_declaration@4 -- find_field
  // would pick the wrong one).
  std::map<int, StdRec> var_record_lit_;
  void record_record_lit(const Ident& id, const Expression* e) {
    if (!e) return;
    auto* rc = std::get_if<Pexp_record>(&e->desc);
    if (!rc || rc->base || rc->fields.empty()) return;
    build_labelset_index();
    std::vector<std::string> key;
    for (auto& [lid, v] : rc->fields) key.push_back(lid_last(lid.txt));
    std::sort(key.begin(), key.end());
    if (auto it = labelset_index_.find(key); it != labelset_index_.end())
      var_record_lit_[id.stamp] = it->second;
  }
  // The declared type path of `record_type`'s field `label`, from its cmi decl.
  std::string record_field_type_path(const std::string& record_type,
                                     const std::string& label) {
    build_labelset_index();
    auto it = record_field_types_.find(record_type);
    if (it == record_field_types_.end()) return "";
    auto f = it->second.find(label);
    return f == it->second.end() ? "" : f->second;
  }
  // Resolve `label` of the record named (dotted) `path` through record_by_name_,
  // registering its layout into rec_types_ on demand so find paths see it.
  const FieldInfo* named_record_field(const std::string& path, const std::string& label,
                                      FieldInfo& store) {
    build_labelset_index();
    std::string ty = path.substr(path.rfind('.') + 1);
    auto it = record_by_name_.find(ty);
    if (it == record_by_name_.end() || it->second.flat) return nullptr;
    if (!rec_types_.count(ty)) {  // make it visible to from()/find paths
      RecType rt; rt.labels = it->second.labels; rt.shape = it->second.shape;
      rt.flat = it->second.flat; rt.mut = false;
      for (bool m : it->second.mut) rt.mut = rt.mut || m;
      rec_types_[ty] = std::move(rt);
    }
    for (int i = 0; i < (int)it->second.labels.size(); ++i)
      if (it->second.labels[i] == label) {
        store = FieldInfo{ty, i, it->second.mut[i], it->second.shape[i]};
        return &store;
      }
    return nullptr;
  }
  // A nested record pattern -> its record type, set from the enclosing field's
  // declared value type while compiling the outer record pattern.
  std::unordered_map<const void*, std::string> pat_type_hint_;
  // While compiling a record pattern's field `label` (resolved to `fi`) whose
  // sub-pattern is itself a record pattern, tag that sub-pattern with the field's
  // declared value type, so its (ambiguous) labels resolve through it.
  void tag_nested_record_subpat(const FieldInfo* fi, const std::string& label,
                                const Pattern* sub) {
    if (!fi || fi->type.empty() || !sub) return;
    const Pattern* e = effective_pat(sub);
    if (!std::get_if<Ppat_record>(&e->desc)) return;
    if (std::string vt = record_field_type_path(fi->type, label); !vt.empty())
      pat_type_hint_[(const void*)e] = vt;
  }
  // While scanning a constructor pattern `C(a0, a1, ..)`, tag any record-pattern
  // argument with C's declared arg type (qualified), so its labels resolve through
  // that record even when ambiguous (`Sig_module(_,_,{md_type=..},_,_)`).
  void tag_ctor_record_args(const std::string& ctor, const Pattern* arg) {
    if (!arg) return;
    build_labelset_index();
    auto it = ctor_arg_paths_.find(ctor);
    if (it == ctor_arg_paths_.end()) return;
    const std::vector<std::string>& paths = it->second;
    std::vector<const Pattern*> args;
    const Pattern* a0 = effective_pat(arg);
    if (auto* tup = std::get_if<Ppat_tuple>(&a0->desc); tup && tup->elems.size() == paths.size())
      for (auto& e : tup->elems) args.push_back(e.get());
    else if (paths.size() == 1) args.push_back(arg);
    else return;
    for (size_t i = 0; i < args.size() && i < paths.size(); ++i) {
      if (paths[i].empty() || paths[i].find('.') == std::string::npos) continue;
      const Pattern* s = effective_pat(args[i]);
      if (std::get_if<Ppat_record>(&s->desc))
        pat_type_hint_[(const void*)s] = paths[i];
      else if (std::get_if<Ppat_var>(&s->desc))  // a var arg: tag for var_record_path_
        var_node_path_[(const void*)s] = paths[i];
    }
  }
  // type that has `label`.  Used for an explicitly deep-qualified record literal
  // (`{ CamlinternalMenhirLib.EngineTypes.state = .. }` in menhir's actions).
  std::optional<StdRec> nested_record_layout(const std::string& dotted, const std::string& label) {
    size_t dot = dotted.find('.');
    if (dot == std::string::npos) return stdlib_record_layout(dotted, label);
    std::string head = dotted.substr(0, dot);
    if (module_base(head) || fields_of(head).empty()) return std::nullopt;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(head));
      const cmi::Signature* sig = &cmi.sig();
      for (size_t pos = dot; pos != std::string::npos;) {
        size_t nd = dotted.find('.', pos + 1);
        std::string comp = dotted.substr(pos + 1, nd == std::string::npos ? std::string::npos : nd - pos - 1);
        const cmi::Signature* next = nullptr;
        for (auto& md : sig->modules) if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
        if (!next) return std::nullopt;
        sig = next; pos = nd;
      }
      for (auto& td : sig->types) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        bool has = false;
        for (auto& l : td.labels) if (l.name == label) { has = true; break; }
        if (!has) continue;
        StdRec r; bool all_float = true;
        for (auto& l : td.labels) {
          r.labels.push_back(l.name); r.shape.push_back(cmi_field_kind(l.type));
          r.mut.push_back(l.mutable_);
          if (r.shape.back() != ValueKind::Float) all_float = false;
        }
        r.flat = all_float; return r;
      }
    } catch (...) {}
    return std::nullopt;
  }
  // Resolve a record-pattern/expression label to its field info: the in-scope
  // find_field, else a deep-qualified (`A.B.field`) one via the navigated cmi.
  const FieldInfo* resolve_record_label(const Longident& lid, FieldInfo& store) {
    // A qualified label (`M.l` local or stdlib, `A.B.l` deep) resolves through its
    // authoritative qualifier FIRST -- a bare find_field could pick a different
    // record sharing the ambiguous label (load_path's `Dir.hidden` vs paths.hidden).
    if (auto rf = qualified_field(lid)) {
      store = FieldInfo{"", rf->index, rf->mut, rf->kind}; return &store;
    }
    if (const FieldInfo* fi = find_field(lid_last(lid))) return fi;
    // `{contents = ..}` is the ref cell (`'a ref = {mutable contents}`): field 0,
    // mutable.  Without this a `{contents = p}` PATTERN (Tobject's abbrev ref in
    // ctype's `copy`) leaves the label unresolved, so pat_test bails and the
    // whole match collapses to the int_cases best-effort (unbound `?` bindings).
    if (lid_last(lid) == "contents") {
      store = FieldInfo{"", 0, true, ValueKind::Gen}; return &store;
    }
    return nullptr;
  }
  // The record type a record pattern's/expr's field SET identifies: the unique
  // rec_types_ entry whose labels contain ALL the given labels.  Null if none or
  // 2+ match.  Lets an AMBIGUOUS label (`ty`, shared by many records) resolve to
  // the index in the record the OTHER fields pin down (typecore's
  // `{ty; explanation}` = type_expected.ty@0, not another record's `ty`).
  const std::pair<const std::string, RecType>* record_for_fields(
      const std::vector<std::string>& want) {
    const std::pair<const std::string, RecType>* best = nullptr;
    for (auto& kv : rec_types_) {
      bool all = true;
      for (auto& w : want) {
        bool has = false;
        for (auto& l : kv.second.labels) if (l == w) { has = true; break; }
        if (!has) { all = false; break; }
      }
      if (!all) continue;
      if (best) return nullptr;  // ambiguous field set
      best = &kv;
    }
    return best;
  }
  // The module qualifying a record pattern: the prefix of its first `M.field`
  // field.  OCaml's type-directed disambiguation pins the whole record to that
  // module, so an ambiguous bare sibling label resolves there.
  const std::string& pat_record_qual_mod(const Ppat_record& pr) {
    static const std::string empty;
    for (auto& f : pr.fields)
      if (auto* d = std::get_if<Ldot>(&f.first.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          return pl->name;
    return empty;
  }
  // Resolve a record-PATTERN label, using the pattern's full field set to pin the
  // record for an AMBIGUOUS label (so it takes the right index, not last-wins).
  const FieldInfo* resolve_record_pat_field(const std::vector<std::string>& fields,
                                            const Longident& lid, FieldInfo& store,
                                            const void* pat_key = nullptr,
                                            const std::string& qual_mod = "") {
    // A qualified field (`{ Lambda.code = ..; required_globals }`) names the
    // record's module: register its record types so the SIBLING bare labels
    // (required_globals) resolve by-field-set too -- else the whole pattern fails
    // and every bound var is left unresolved (?lambda/?required_globals).
    if (auto* d = std::get_if<Ldot>(&lid.v))
      if (auto* pl = std::get_if<Lident>(&d->prefix->v))
        register_module_records(pl->name);
    if (auto rf = qualified_field(lid)) { store = FieldInfo{"", rf->index, rf->mut, rf->kind}; return &store; }
    std::string n = lid_last(lid);
    // A NESTED record pattern tagged with its record type by the enclosing field's
    // value type (`{ def = {params;body} }` -> {params;body} : lfunction): resolve
    // its (ambiguous) labels through that type, not a same-labelled local record
    // (bytegen's function_to_compile vs Lambda's lfunction).
    if (pat_key)
      if (auto h = pat_type_hint_.find(pat_key); h != pat_type_hint_.end()) {
        // a QUALIFIED hint (`Types.module_declaration`) names the exact record's
        // module -- resolve through it so an ambiguous label (md_type, shared with
        // Typedtree.module_declaration) is not stripped to the bare ambiguous name.
        if (auto rf = resolve_field_in_record_path(h->second, n)) { store = *rf; return &store; }
        if (const FieldInfo* fi = named_record_field(h->second, n, store)) return fi;
      }
    // A bare label whose record is pinned by a QUALIFIED SIBLING field
    // (`{Types.cd_id; cd_args; cd_res}`): OCaml requires all fields of one record,
    // so the sibling's module is authoritative -- resolve `cd_args` as
    // `Types.cd_args` (@1), not the last-registered Typedtree.cd_args (@4).
    if (!qual_mod.empty()) {
      register_module_records(qual_mod);
      if (auto rf = local_module_field(qual_mod, n)) {
        store = FieldInfo{"", rf->index, rf->mut, rf->kind}; return &store;
      }
      if (auto rf = stdlib_record_field(qual_mod, n)) {
        store = FieldInfo{"", rf->index, rf->mut, rf->kind}; return &store;
      }
    }
    // The record the WHOLE field set pins down is authoritative for a multi-field
    // pattern -- an ambiguous label (`ty`) takes ITS index there, not the
    // last-declared / scoped one (`{ty; explanation}` is type_expected.ty@0).
    if (fields.size() > 1)
      if (auto* kv = record_for_fields(fields))
        for (int i = 0; i < (int)kv->second.labels.size(); ++i)
          if (kv->second.labels[i] == n) {
            store = FieldInfo{kv->first, i, kv->second.mut, kv->second.shape[i]};
            return &store;
          }
    // An AMBIGUOUS field (`args` in both `pattern_matching`@1 and `division`@0)
    // resolved by TYPE: (a) the matched value's record type the inferencer pinned
    // for this pattern, or (b) the record uniquely determined by the UNION of the
    // sibling match arms' record-pattern fields (`{cases;args}` pins
    // pattern_matching, so the size-1 `{args}` arm takes args@1 there too).  This
    // runs BEFORE find_field, whose scoped-unambiguous map may hold the wrong one.
    if (ambiguous_fields_.count(n)) {
      auto from = [&](const std::string& ty) -> const FieldInfo* {
        auto rt = rec_types_.find(ty);
        if (rt == rec_types_.end()) return nullptr;
        for (int i = 0; i < (int)rt->second.labels.size(); ++i)
          if (rt->second.labels[i] == n) {
            store = FieldInfo{ty, i, rt->second.mut, rt->second.shape[i]};
            return &store;
          }
        return nullptr;
      };
      if (pat_key)
        if (auto pt = vk.pat_record_type.find(pat_key); pt != vk.pat_record_type.end())
          if (const FieldInfo* fi = from(pt->second.substr(pt->second.rfind('.') + 1)))
            return fi;
      if (!match_rec_fields_.empty()) {
        std::set<std::string> u(match_rec_fields_.begin(), match_rec_fields_.end());
        u.insert(fields.begin(), fields.end());
        std::vector<std::string> uv(u.begin(), u.end());
        if (auto* kv = record_for_fields(uv))
          if (const FieldInfo* fi = from(kv->first)) return fi;
      }
    }
    if (const FieldInfo* fi = find_field(n)) return fi;
    return resolve_record_label(lid, store);
  }
  // The UNION of the record-pattern field labels across the arms of the match
  // currently being compiled -- to disambiguate a size-1 ambiguous-field arm by a
  // multi-field sibling (set/restored around compile_match's row loop).
  std::vector<std::string> match_rec_fields_;
  // A single field of a record in a NESTED module path (`e.CamlinternalMenhirLib.
  // EngineTypes.current`): its index/kind/mut from the navigated cmi record.
  std::optional<StdField> nested_record_field(const std::string& dotted, const std::string& label) {
    if (auto sr = nested_record_layout(dotted, label))
      for (int i = 0; i < (int)sr->labels.size(); ++i)
        if (sr->labels[i] == label)
          return StdField{i, sr->shape[i], sr->mut[i], sr->flat};
    return std::nullopt;
  }
  // The stdlib-module prefix governing a record label: an explicit `M.label`
  // qualification, else the base expression's qualified head.
  static std::string record_module_of(const Longident& lid, const Expression* base) {
    if (auto* d = std::get_if<Ldot>(&lid.v))
      if (auto* pl = std::get_if<Lident>(&d->prefix->v)) return pl->name;
    while (base) {
      if (auto* ap = std::get_if<Pexp_apply>(&base->desc)) { base = ap->fn.get(); continue; }
      if (auto* ct = std::get_if<Pexp_constraint>(&base->desc)) { base = ct->e.get(); continue; }
      break;
    }
    if (base)
      if (auto* id = std::get_if<Pexp_ident>(&base->desc))
        if (auto* d = std::get_if<Ldot>(&id->id.txt.v))
          if (auto* pl = std::get_if<Lident>(&d->prefix->v)) return pl->name;
    return {};
  }

  Ident fresh(const std::string& name, bool temp = false) { return Ident{name, stamp++, temp}; }
  ValueKind pat_kind(const Pattern* p) {
    auto it = vk.pat.find(p);
    return it == vk.pat.end() ? ValueKind::Gen : vkind(it->second);
  }
  ValueKind expr_kind(const Expression* e) {
    auto it = vk.expr.find(e);
    return it == vk.expr.end() ? ValueKind::Gen : vkind(it->second);
  }
  // Approximate Typeopt.maybe_pointer for object val-inits / `n<-e`: is the value
  // an immediate (Immediate -> setfield_imm_computed) rather than a heap pointer?
  // The inferencer doesn't visit object bodies, so fall back to a syntactic check
  // (constants, bool/unit, int arithmetic & comparisons) and default to pointer.
  bool value_is_immediate(const Expression* e) {
    if (expr_kind(e) == ValueKind::Int) return true;
    if (auto* c = std::get_if<Pexp_constant>(&e->desc)) {
      if (auto* i = std::get_if<Pconst_integer>(&c->c.desc)) return !i->suffix;  // l/L/n boxed
      if (std::get_if<Pconst_char>(&c->c.desc)) return true;
      return false;  // string/float
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e->desc)) return value_is_immediate(ct->e.get());
    if (auto* k = std::get_if<Pexp_construct>(&e->desc)) {
      std::string n = lid_last(k->id.txt);
      return n == "true" || n == "false" || n == "()";
    }
    if (auto* ap = std::get_if<Pexp_apply>(&e->desc))
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* l = std::get_if<Lident>(&fid->id.txt.v)) {
          const std::string& n = l->name;
          static const std::set<std::string> imm = {
            "+","-","*","/","mod","land","lor","lxor","lsl","lsr","asr","~-","abs",
            "=","<>","<",">","<=",">=","==","!=","not","&&","||"};
          if (imm.count(n)) return true;
        }
    if (auto* it = std::get_if<Pexp_ifthenelse>(&e->desc))
      return it->else_ && value_is_immediate(it->then_.get()) && value_is_immediate((*it->else_).get());
    return false;
  }

  // Translclass.const_path: a method body that is a self-contained value (a
  // constant, an outer variable, or a closure not capturing self) becomes a
  // GetConst builtin method instead of a closure.
  bool is_const_path(const LamPtr& l, const Ident& self) {
    switch (l->k) {
      case Lam::K::ConstInt: case Lam::K::ConstChar: case Lam::K::ConstFloat:
      case Lam::K::ConstString: case Lam::K::ConstBlock: return true;
      case Lam::K::Var: return l->var.stamp != self.stamp;
      case Lam::K::Function: return count_var(l->body, self) == 0;
      default: return false;
    }
  }
  bool expr_is_string(const Expression* e) {  // string isn't a value kind; drives string compares
    auto it = vk.expr.find(e);
    return it != vk.expr.end() && it->second == "string";
  }
  const Ident* lookup(const std::string& n) {
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
      auto f = it->find(n);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  // Like lookup, but only scope frames at index >= floor (the topmost frames).
  const Ident* lookup_from(const std::string& n, size_t floor) {
    for (size_t i = scope.size(); i-- > floor;) {
      auto f = scope[i].find(n);
      if (f != scope[i].end()) return &f->second;
    }
    return nullptr;
  }

  // Integer arithmetic operators that compile to a primitive.
  static bool int_op(const std::string& n, Prim& p) {
    if (n == "+") { p = Prim::Addint; return true; }
    if (n == "-") { p = Prim::Subint; return true; }
    if (n == "*") { p = Prim::Mulint; return true; }
    return false;
  }
  // A monomorphic pervasive %-primitive -> {printlambda spelling, arity}.  These
  // always inline to the primitive regardless of operand type.
  static std::pair<std::string, int> pervasive_prim(const std::string& n) {
    static const std::unordered_map<std::string, std::pair<std::string, int>> t = {
      {"+.", {"+.", 2}}, {"-.", {"-.", 2}}, {"*.", {"*.", 2}}, {"/.", {"/.", 2}},
      {"/", {"/", 2}}, {"mod", {"mod", 2}},
      {"land", {"and", 2}}, {"lor", {"or", 2}}, {"lxor", {"xor", 2}},
      {"lsl", {"lsl", 2}}, {"lsr", {"lsr", 2}}, {"asr", {"asr", 2}},
      {"not", {"not", 1}}, {"~-", {"~", 1}}, {"~-.", {"~.", 1}},
      {"abs_float", {"abs.", 1}}, {"ignore", {"ignore", 1}},
      {"float_of_int", {"float_of_int", 1}}, {"int_of_float", {"int_of_float", 1}},
      {"float", {"float_of_int", 1}}, {"truncate", {"int_of_float", 1}},
    };
    auto it = t.find(n);
    return it == t.end() ? std::pair<std::string, int>{"", 0} : it->second;
  }
  // Array element-kind annotation (array_kind in printlambda).
  static std::string array_kind(ValueKind k) {
    if (k == ValueKind::Int) return "int";
    if (k == ValueKind::Float) return "float";
    return "gen";
  }
  // Array element kind from an element expression's inferred type: int / float /
  // addr (a known boxed type, e.g. string/record) / gen (a type variable).
  std::string array_elem_kind(const Expression* e) {
    auto it = vk.expr.find(e);
    std::string s = it == vk.expr.end() ? "" : it->second;
    if (s == "int") return "int";
    if (s == "float") return "float";
    if (s == "addr" || s == "string") return "addr";
    return "gen";
  }
  // Element kind of an *array-typed* expression (e.g. the arg of Array.length, or
  // an empty `[||]`), from the inferencer's recorded element kind.
  std::string array_arg_kind(const Expression* a) {
    auto it = vk.array_elem.find(a);
    if (it == vk.array_elem.end()) return "gen";
    if (it->second == "int") return "int";
    if (it->second == "float") return "float";
    if (it->second == "addr" || it->second == "string") return "addr";
    return "gen";
  }
  // A polymorphic comparison operator -> {int-comparison spelling, caml_* C name}.
  static std::pair<std::string, std::string> poly_cmp(const std::string& n) {
    if (n == "<") return {"<", "caml_lessthan"};
    if (n == ">") return {">", "caml_greaterthan"};
    if (n == "<=") return {"<=", "caml_lessequal"};
    if (n == ">=") return {">=", "caml_greaterequal"};
    if (n == "=") return {"==", "caml_equal"};
    if (n == "<>") return {"!=", "caml_notequal"};
    return {"", ""};
  }

  // Lower an applied stdlib `external` (its prim_name read from the cmi) to its
  // Lambda form, matching `ocamlc -dlambda`.  Returns null for prims we don't yet
  // spell (the caller leaves them unresolved) -- never wrong, only incomplete.
  // Conservative on type-directed prims: only the cases whose spelling we can
  // determine from value kinds are emitted.
  // A primitive used as a *value* (not applied): e.g. Sys.argv = %sys_argv lowers
  // to `(caml_sys_argv 0)`.  Null for prims we don't spell in value position.
  LamPtr prim_value(const std::string& prim) {
    if (prim == "%sys_argv") {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = "caml_sys_argv";
      pr->args = {cint(0)}; return pr;
    }
    return nullptr;
  }

  // The GENERIC (type-agnostic) lambda body for `prim` applied to `argv`, used to
  // eta-expand a primitive that appears in value position.  Mirrors translprim's
  // fallback lowering: the stub carries no operand types, so polymorphic compare
  // takes its `caml_*` form and field reads take the pointer (`field_mut`) form.
  // Null for prims we don't lower this way.
  // The printlambda spelling of a string/bytes/bigstring 16/32/64-bit
  // accessor %-builtin; null for other primitives.  Safe and unsafe variants
  // lower to the same checked C entry points in bytecode.
  static const std::string* mem_access_spelling(const std::string& prim) {
    static const std::unordered_map<std::string, std::string> mem = {
      {"%caml_string_get16", "string.get16"}, {"%caml_string_get16u", "string.unsafe_get16"},
      {"%caml_string_get32", "string.get32"}, {"%caml_string_get32u", "string.unsafe_get32"},
      {"%caml_string_get64", "string.get64"}, {"%caml_string_get64u", "string.unsafe_get64"},
      {"%caml_bytes_get16", "bytes.get16"}, {"%caml_bytes_get16u", "bytes.unsafe_get16"},
      {"%caml_bytes_get32", "bytes.get32"}, {"%caml_bytes_get32u", "bytes.unsafe_get32"},
      {"%caml_bytes_get64", "bytes.get64"}, {"%caml_bytes_get64u", "bytes.unsafe_get64"},
      {"%caml_bytes_set16", "bytes.set16"}, {"%caml_bytes_set16u", "bytes.unsafe_set16"},
      {"%caml_bytes_set32", "bytes.set32"}, {"%caml_bytes_set32u", "bytes.unsafe_set32"},
      {"%caml_bytes_set64", "bytes.set64"}, {"%caml_bytes_set64u", "bytes.unsafe_set64"},
      {"%caml_bigstring_get16", "bigarray.array1.get16"},
      {"%caml_bigstring_get16u", "bigarray.array1.unsafe_get16"},
      {"%caml_bigstring_get32", "bigarray.array1.get32"},
      {"%caml_bigstring_get32u", "bigarray.array1.unsafe_get32"},
      {"%caml_bigstring_get64", "bigarray.array1.get64"},
      {"%caml_bigstring_get64u", "bigarray.array1.unsafe_get64"},
      {"%caml_bigstring_set16", "bigarray.array1.set16"},
      {"%caml_bigstring_set16u", "bigarray.array1.unsafe_set16"},
      {"%caml_bigstring_set32", "bigarray.array1.set32"},
      {"%caml_bigstring_set32u", "bigarray.array1.unsafe_set32"},
      {"%caml_bigstring_set64", "bigarray.array1.set64"},
      {"%caml_bigstring_set64u", "bigarray.array1.unsafe_set64"},
    };
    auto m = mem.find(prim);
    return m == mem.end() ? nullptr : &m->second;
  }
  LamPtr prim_stub_body(const std::string& prim, const std::vector<LamPtr>& argv) {
    int n = (int)argv.size();
    if (const std::string* sp = mem_access_spelling(prim)) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp;
      pr->prim_id = *sp; pr->args = argv; return pr;
    }
    auto ic = [&](const std::string& sp) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp; pr->prim_id = sp;
      pr->args = argv; return pr; };
    auto cc = [&](const std::string& nm) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = nm;
      pr->args = argv; return pr; };
    static const std::unordered_map<std::string, std::string> poly = {
      {"%compare", "caml_compare"}, {"%equal", "caml_equal"},
      {"%notequal", "caml_notequal"}, {"%lessthan", "caml_lessthan"},
      {"%lessequal", "caml_lessequal"}, {"%greaterthan", "caml_greaterthan"},
      {"%greaterequal", "caml_greaterequal"},
    };
    if (auto it = poly.find(prim); it != poly.end() && n == 2) return cc(it->second);
    if (prim == "%opaque" && n == 1) return ic("opaque");
    if (prim == "%ignore" && n == 1) return ic("ignore");
    {  // Sys compile-time constants, used as a value (eta-stub) -> ccall.
      static const std::unordered_map<std::string, std::string> sysconst = {
        {"%big_endian", "caml_sys_const_big_endian"},
        {"%word_size", "caml_sys_const_word_size"},
        {"%int_size", "caml_sys_const_int_size"},
        {"%max_wosize", "caml_sys_const_max_wosize"},
        {"%ostype_unix", "caml_sys_const_ostype_unix"},
        {"%ostype_win32", "caml_sys_const_ostype_win32"},
        {"%ostype_cygwin", "caml_sys_const_ostype_cygwin"},
        {"%backend_type", "caml_sys_const_backend_type"},
        {"%standard_library_default", "caml_sys_const_standard_library_default"},
      };
      if (auto it = sysconst.find(prim); it != sysconst.end() && n == 1) return cc(it->second);
    }
    if (prim == "%identity" && n == 1) return argv[0];
    // Array/string/bytes element access as a value (eta-stub): the operand type
    // is unknown here, so the generic spelling (a runtime-tag-checked access).
    if ((prim == "%array_unsafe_get" || prim == "%array_safe_get" ||
         prim == "%array_get") && n == 2)
      return ic(prim == "%array_unsafe_get" ? "array.unsafe_get[gen]" : "array.get[gen]");
    if ((prim == "%array_unsafe_set" || prim == "%array_safe_set" ||
         prim == "%array_set") && n == 3)
      return ic(prim == "%array_unsafe_set" ? "array.unsafe_set[gen]" : "array.set[gen]");
    if (prim == "%array_length" && n == 1) return ic("array.length[gen]");
    // Obj.size/field/set_field as values: generic-array ops (Obj.t is opaque).
    if (prim == "%obj_size" && n == 1) return ic("array.length[gen]");
    if (prim == "%obj_field" && n == 2) return ic("array.unsafe_get[gen]");
    if (prim == "%obj_set_field" && n == 3) return ic("array.unsafe_set[gen]");
    // floatarray element access (Float.Array.unsafe_get etc.): the [float] form.
    if ((prim == "%floatarray_unsafe_get" || prim == "%floatarray_safe_get") && n == 2)
      return ic(prim == "%floatarray_unsafe_get" ? "array.unsafe_get[float]" : "array.get[float]");
    if ((prim == "%floatarray_unsafe_set" || prim == "%floatarray_safe_set") && n == 3)
      return ic(prim == "%floatarray_unsafe_set" ? "array.unsafe_set[float]" : "array.set[float]");
    if (prim == "%floatarray_length" && n == 1) return ic("array.length[float]");
    // Bigarray accessors as values (`String.init n (Array1.get buf)`): the
    // kind/layout-less spelling, like the direct-application path.
    if (prim.rfind("%caml_ba_", 0) == 0) {
      static const std::pair<const char*, const char*> ba[] = {
          {"%caml_ba_ref_", "get"}, {"%caml_ba_unsafe_ref_", "unsafe_get"},
          {"%caml_ba_set_", "set"}, {"%caml_ba_unsafe_set_", "unsafe_set"}};
      for (auto& [pfx, nm] : ba)
        if (prim.rfind(pfx, 0) == 0)
          return ic(std::string("Bigarray.") + nm + "[generic,unknown]");
      if (prim.rfind("%caml_ba_dim_", 0) == 0)
        return ic("Bigarray.dim_" + prim.substr(sizeof("%caml_ba_dim_") - 1));
    }
    if (prim == "%string_unsafe_get" && n == 2) return ic("string.unsafe_get");
    if ((prim == "%string_safe_get" || prim == "%string_get") && n == 2) return ic("string.get");
    if (prim == "%bytes_unsafe_get" && n == 2) return ic("bytes.unsafe_get");
    if ((prim == "%bytes_safe_get" || prim == "%bytes_get") && n == 2) return ic("bytes.get");
    if (prim == "%bytes_unsafe_set" && n == 3) return ic("bytes.unsafe_set");
    if ((prim == "%bytes_safe_set" || prim == "%bytes_set") && n == 3) return ic("bytes.set");
    if (prim == "%string_length" && n == 1) return ic("string.length");
    if (prim == "%bytes_length" && n == 1) return ic("bytes.length");
    if ((prim == "%bytes_to_string" || prim == "%bytes_of_string" ||
         prim == "%string_to_bytes" || prim == "%string_of_bytes") && n == 1) return argv[0];
    {  // atomic ops as first-class values (Atomic.get etc.): the field-0 ccall form
      auto acc = [&](const char* fn, std::vector<LamPtr> v) {
        auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = fn;
        pr->args = std::move(v); return pr; };
      if (prim == "%atomic_load" && n == 1) return acc("caml_atomic_load_field", {argv[0], cint(0)});
      if (prim == "%atomic_exchange" && n == 2) return acc("caml_atomic_exchange_field", {argv[0], cint(0), argv[1]});
      if ((prim == "%atomic_compare_and_set" || prim == "%atomic_cas") && n == 3) return acc("caml_atomic_cas_field", {argv[0], cint(0), argv[1], argv[2]});
      if (prim == "%atomic_fetch_add" && n == 2) return acc("caml_atomic_fetch_add_field", {argv[0], cint(0), argv[1]});
      if (prim == "%dls_get" && n == 1) return acc("caml_domain_dls_get", {argv[0]});
      // %atomic_*_loc (OCaml 5.2+ atomic-location API): arg0 is a 2-field block
      // (obj, field-index) from `[%atomic.loc e.field]`; bind it to a temp and
      // forward (obj, field) to the runtime caml_atomic_*_field primitive.
      auto loc_call = [&](const char* fn, std::vector<LamPtr> extra) {
        Ident t = fresh("loc", true);
        std::vector<LamPtr> v = {fieldimm(0, varof(t)), fieldimm(1, varof(t))};
        for (auto& e : extra) v.push_back(std::move(e));
        auto pr = acc(fn, std::move(v));
        auto l = mk(Lam::K::Let);
        l->bindings = {{t, ValueKind::Gen, argv[0]}};
        l->body = pr;
        return l;
      };
      if (prim == "%atomic_load_loc" && n == 1) return loc_call("caml_atomic_load_field", {});
      if (prim == "%atomic_exchange_loc" && n == 2) return loc_call("caml_atomic_exchange_field", {argv[1]});
      if (prim == "%atomic_cas_loc" && n == 3) return loc_call("caml_atomic_cas_field", {argv[1], argv[2]});
      if (prim == "%atomic_fetch_add_loc" && n == 2) return loc_call("caml_atomic_fetch_add_field", {argv[1]});
    }
    if (prim == "%makemutable" && n == 1) {  // `ref` as a value: (makemutable 0 prim)
      auto m = mk(Lam::K::Prim); m->prim = Prim::Makemutable; m->prim_arg = 0;
      m->blk_shape = {ValueKind::Gen}; m->args = argv; return m;
    }
    if (prim == "%perform" && n == 1) return cc("perform");
    if (prim == "%lazy_force" && n == 1) return force_lazy(argv[0]);
    if (prim == "%obj_is_int" && n == 1) return ic("isint");
    if ((prim == "%raise" || prim == "%reraise" || prim == "%raise_notrace") && n == 1) {
      auto pr = mk(Lam::K::Prim);  // raise / raise_notrace as a value
      pr->prim = prim == "%reraise" ? Prim::Reraise : Prim::Raise;
      pr->args = argv;
      return pr;
    }
    if (prim == "%raise_with_backtrace" && n == 2) {  // translprim's expansion
      Ident ex = fresh("exn");
      auto rst = mk(Lam::K::Prim); rst->prim = Prim::Ccall;
      rst->prim_id = "caml_restore_raw_backtrace"; rst->args = {varof(ex), argv[1]};
      auto rr = mk(Lam::K::Prim); rr->prim = Prim::Reraise; rr->args = {varof(ex)};
      auto sq = mk(Lam::K::Sequence); sq->cond = rst; sq->else_ = rr;
      auto l = mk(Lam::K::Let);
      l->bindings = {{ex, ValueKind::Gen, argv[0]}};
      l->body = sq;
      return l;
    }
    if ((prim == "%sequand" || prim == "%sequor") && n == 2) {  // && / || as values
      auto i = mk(Lam::K::IfThenElse);
      i->cond = argv[0];
      if (prim == "%sequand") { i->then_ = argv[1]; i->else_ = cint(0); }
      else { i->then_ = cint(1); i->else_ = argv[1]; }
      return i;
    }
    if ((prim == "%succint" || prim == "%predint") && n == 1) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Offsetint;
      pr->prim_arg = prim == "%succint" ? 1 : -1; pr->args = argv; return pr;
    }
    if ((prim == "%field0" || prim == "%field1") && n == 1) {  // generic -> field_mut
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::FieldMut;
      pr->prim_arg = prim == "%field1" ? 1 : 0; pr->args = argv; return pr;
    }
    if ((prim == "%setfield0" || prim == "%setfield1") && n == 2) {  // (:=) as a value
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::SetfieldImm;
      pr->prim_arg = prim == "%setfield1" ? 1 : 0; pr->args = argv; return pr;
    }
    {  // pervasive arithmetic / bitwise / unary, via the operator-form maps
      static const std::unordered_map<std::string, std::string> parith = {
        {"%addint", "+"}, {"%subint", "-"}, {"%mulint", "*"}, {"%divint", "/"},
        {"%modint", "mod"}, {"%negint", "~-"}, {"%andint", "land"}, {"%orint", "lor"},
        {"%xorint", "lxor"}, {"%lslint", "lsl"}, {"%lsrint", "lsr"}, {"%asrint", "asr"},
        {"%addfloat", "+."}, {"%subfloat", "-."}, {"%mulfloat", "*."}, {"%divfloat", "/."},
        {"%negfloat", "~-."}, {"%floatofint", "float_of_int"}, {"%intoffloat", "int_of_float"},
        {"%boolnot", "not"},
      };
      if (auto pi = parith.find(prim); pi != parith.end()) {
        Prim p;
        if (int_op(pi->second, p) && n == 2) {
          auto pr = mk(Lam::K::Prim); pr->prim = p; pr->args = argv; return pr;
        }
        if (auto pp = pervasive_prim(pi->second); !pp.first.empty() && n == pp.second)
          return ic(pp.first);
      }
    }
    {  // boxed integers print `Int32.`/`Int64.`/`Nativeint.<op>`
      static const std::pair<std::string, std::string> bint[] = {
        {"%int32_", "Int32."}, {"%int64_", "Int64."}, {"%nativeint_", "Nativeint."}};
      for (auto& [pfx, mod] : bint)
        if (prim.rfind(pfx, 0) == 0) return cc(mod + prim.substr(pfx.size()));
    }
    if (!prim.empty() && prim[0] != '%') return cc(prim);  // a C external
    return nullptr;
  }
  // A primitive used as a first-class value is eta-expanded to a `stub` function
  // applying the primitive to its parameters: `compare` -> `(function prim prim
  // stub (caml_compare prim prim))`, `succ` -> `(function prim stub (1+ prim))`.
  // Returns null for prims we don't lower this way.
  LamPtr prim_stub(const StdPrim& p) {
    bool poly = p.name == "%compare" || p.name == "%equal" || p.name == "%notequal" ||
                p.name == "%lessthan" || p.name == "%lessequal" ||
                p.name == "%greaterthan" || p.name == "%greaterequal";
    int arity = p.arity > 0 ? p.arity : (poly ? 2 : 1);
    auto fn = mk(Lam::K::Function); fn->inline_attr = "stub";
    std::vector<LamPtr> argv;  // params in body-application order
    for (int i = 0; i < arity; ++i) {
      Ident pp = fresh("prim");  // unprinted stamps are invisible to normalization
      fn->params.push_back({pp, ValueKind::Gen});
      auto v = mk(Lam::K::Var); v->var = pp; argv.push_back(v);
    }
    LamPtr body = prim_stub_body(p.name, argv);
    if (!body) return nullptr;
    fn->body = body;
    return fn;
  }
  // Over-application of a primitive (`Lazy.force e ()`, `snd p 42`): saturate
  // it on the first `arity` arguments, then apply the result to the rest.  The
  // argument vector is trimmed in place for the nested call and restored.
  LamPtr prim_apply(const std::string& prim, int arity, const Pexp_apply& ap0,
                    const Expression& e) {
    if (arity <= 0 || (int)ap0.args.size() <= arity)
      return prim_to_lam(prim, arity, ap0, e);
    auto& ap = const_cast<Pexp_apply&>(ap0);
    std::vector<std::pair<ArgLabel, ExprBox>> tail;
    for (size_t i = arity; i < ap.args.size(); ++i) tail.push_back(std::move(ap.args[i]));
    ap.args.resize(arity);
    LamPtr r = prim_to_lam(prim, arity, ap, e, /*over=*/true);
    std::vector<LamPtr> rest;
    if (r) for (auto& t : tail) rest.push_back(expr(*t.second));
    for (auto& t : tail) ap.args.push_back(std::move(t));
    if (!r) return nullptr;
    return lapply_(r, std::move(rest));
  }
  LamPtr prim_to_lam(const std::string& prim, int arity, const Pexp_apply& ap, const Expression& e,
                     bool over = false) {
    auto& as = ap.args;
    auto args = [&] {
      std::vector<LamPtr> v;
      for (auto& a : as) v.push_back(expr(*a.second));
      return v;
    };
    auto op = [&](const std::string& spelling) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp;
      pr->prim_id = spelling; pr->args = args(); return pr;
    };
    // Structural (kind-independent) prims.
    if ((prim == "%raise" || prim == "%raise_notrace") && as.size() == 1) {
      raise_arg_ = true; LamPtr arg = expr(*as[0].second); raise_arg_ = false;
      bool reraise = !caught_exn_.empty() && arg->k == Lam::K::Var &&
                     arg->var.stamp == caught_exn_.back().stamp;
      auto pr = mk(Lam::K::Prim); pr->prim = reraise ? Prim::Reraise : Prim::Raise;
      pr->args = {arg}; return pr;
    }
    if (prim == "%opaque" && as.size() == 1) return op("opaque");
    if (prim == "%ignore" && as.size() == 1) return op("ignore");
    // `Fun.todo ()`: evaluate the argument, then raise Todo with the call-site
    // [0: file line] (translprim's location builtin).
    if (prim == "%todo" && as.size() == 1) {
      auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
      blk->args = {predef_global("Todo"),
                   cblock(0, {cstr(file_name_), cint(e.loc.start.lnum)})};
      auto r = mk(Lam::K::Prim); r->prim = Prim::Raise; r->args = {blk};
      auto s = mk(Lam::K::Sequence); s->cond = expr(*as[0].second); s->else_ = r;
      return s;
    }
    if (prim == "%identity" && as.size() == 1) return expr(*as[0].second);  // no-op
    // qualified short-circuits (`Bool.( && ) a b`): still lazy in the 2nd arg
    if ((prim == "%sequand" || prim == "%sequor") && as.size() == 2) {
      auto i = mk(Lam::K::IfThenElse);
      i->cond = expr(*as[0].second);
      if (prim == "%sequand") { i->then_ = expr(*as[1].second); i->else_ = cint(0); }
      else { i->then_ = cint(1); i->else_ = expr(*as[1].second); }
      return i;
    }
    // `f @@ x` / `x |> f` apply f to x (the function is the 1st / 2nd argument).
    if ((prim == "%apply" || prim == "%revapply") && as.size() == 2) {
      int fi = prim == "%apply" ? 0 : 1, xi = prim == "%apply" ? 1 : 0;
      // `f @@ x` / `x |> f` is an application; when `f` is itself a saturated
      // application (`Array.init n @@ g`) ocamlc's lapply merges the arg into it
      // (`(apply Array.init n g)`, not a nested `(apply (apply ..) g)`).
      LamPtr fn = expr(*as[fi].second);
      // When `f` is a bare primitive identifier (`ignore`, `Sys.opaque_identity`,
      // ...) it translates to an eta `stub` closure; the pipe then saturates that
      // arity-1 stub.  Applying the stub allocates one closure per call (15M
      // words in pr7798's `x |> ignore` loops).  Instead lower the application
      // `f x` directly, so the primitive is applied without the stub -- which is
      // also what translprim emits.
      if (fn->k == Lam::K::Function && fn->inline_attr == "stub" &&
          fn->params.size() == 1 &&
          std::holds_alternative<Pexp_ident>(as[fi].second->desc)) {
        auto& asm_ = const_cast<std::vector<std::pair<ArgLabel, ExprBox>>&>(as);
        Expression synth; synth.loc = e.loc;
        Pexp_apply sa;
        sa.fn = std::move(asm_[fi].second);
        sa.args.emplace_back(Nolabel{}, std::move(asm_[xi].second));
        synth.desc = std::move(sa);
        LamPtr r = expr(synth);
        auto& sap = std::get<Pexp_apply>(synth.desc);  // restore the moved boxes
        asm_[fi].second = std::move(sap.fn);
        asm_[xi].second = std::move(sap.args[0].second);
        return r;
      }
      return lapply_(fn, {expr(*as[xi].second)});
    }
    if (prim == "%lazy_force" && as.size() == 1) return force_lazy(expr(*as[0].second));
    if (prim == "%obj_is_int" && as.size() == 1) return op("isint");
    // Polymorphic compare specialized by operand kind (the spellings we can be
    // sure of; string/other gen needs the operand type, so left unresolved).
    if (prim == "%compare" && as.size() == 2) {
      // A string operand -> caml_string_compare (the typed compare), matching the
      // operator path (caml_string_lessthan, ~line 9646).  Without this `compare`
      // on known strings fell back to polymorphic caml_compare, which walks the
      // value (Obj.tag storm) and crashes on a corrupt/abstract block.
      if (expr_is_string(as[0].second.get()) || expr_is_string(as[1].second.get())) {
        auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = "caml_string_compare";
        pr->args = {expr(*as[0].second), expr(*as[1].second)};
        return pr;
      }
      ValueKind k0 = expr_kind(as[0].second.get()), k1 = expr_kind(as[1].second.get());
      if (k0 == ValueKind::Int || k1 == ValueKind::Int) return op("compare_ints");
      if (k0 == ValueKind::Float || k1 == ValueKind::Float) return op("compare_floats");
    }
    // fst / snd: read field 0 / 1 with the element's read kind (int vs pointer;
    // a float element would be field_float, which we don't spell -> leave it).
    if ((prim == "%field0" || prim == "%field1") && as.size() == 1) {
      // translprim: %field0/%field1 are Pfield(_, Pointer, Mutable) by default
      // (field_mut), specialised to field_int only when the field is provably an
      // immediate.  When over-applied (`snd p x` -- e is the CALL, not the field),
      // e's kind is the call result, not the field, so don't trust it as Int.
      ValueKind k = over ? ValueKind::Gen : expr_kind(&e);
      auto pr = mk(Lam::K::Prim);
      pr->prim = k == ValueKind::Int ? Prim::FieldInt : Prim::FieldMut;
      pr->prim_arg = prim == "%field0" ? 0 : 1;
      pr->args = {expr(*as[0].second)};
      return pr;
    }
    // Atomic primitives (OCaml 5).  An Atomic.t is a flat 1-field mutable block
    // (`atomic_make` = makemutable), so each op is the field-0 form of the
    // runtime caml_atomic_*_field primitive.  do_at_exit's `atomic_get
    // exit_function` is %atomic_load; at_exit uses %atomic_compare_and_set.
    {
      auto cc = [&](const char* fn, std::vector<LamPtr> v) {
        auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = fn;
        pr->args = std::move(v); return pr;
      };
      if (prim == "%atomic_load" && as.size() == 1)
        return cc("caml_atomic_load_field", {expr(*as[0].second), cint(0)});
      if (prim == "%atomic_exchange" && as.size() == 2)
        return cc("caml_atomic_exchange_field",
                  {expr(*as[0].second), cint(0), expr(*as[1].second)});
      if ((prim == "%atomic_compare_and_set" || prim == "%atomic_cas") && as.size() == 3)
        return cc("caml_atomic_cas_field", {expr(*as[0].second), cint(0),
                  expr(*as[1].second), expr(*as[2].second)});
      if (prim == "%atomic_fetch_add" && as.size() == 2)
        return cc("caml_atomic_fetch_add_field",
                  {expr(*as[0].second), cint(0), expr(*as[1].second)});
      // %dls_get (domain-local state root): ccall caml_domain_dls_get (unit).
      // Domain.DLS.new_key runs this at module init (needed by Hashtbl).
      if (prim == "%dls_get" && as.size() == 1)
        return cc("caml_domain_dls_get", {expr(*as[0].second)});
      // The OCaml 5.2+ atomic-LOCATION API (Atomic.fetch_and_add etc. call
      // `Loc.* [%atomic.loc t.contents]`).  `[%atomic.loc e.field]` lowers to a
      // 2-field block (obj, field-index) (see the Pexp_extension handler), so a
      // `%atomic_*_loc` op binds that pair to a temp and forwards (obj, field).
      {
        auto loc_call = [&](const char* fn, std::vector<LamPtr> extra) {
          Ident t = fresh("loc", true);
          std::vector<LamPtr> v = {fieldimm(0, varof(t)), fieldimm(1, varof(t))};
          for (auto& e : extra) v.push_back(std::move(e));
          auto pr = cc(fn, std::move(v));
          auto l = mk(Lam::K::Let);
          l->bindings = {{t, ValueKind::Gen, expr(*as[0].second)}};
          l->body = pr;
          return l;
        };
        if (prim == "%atomic_load_loc" && as.size() == 1)
          return loc_call("caml_atomic_load_field", {});
        if (prim == "%atomic_exchange_loc" && as.size() == 2)
          return loc_call("caml_atomic_exchange_field", {expr(*as[1].second)});
        if (prim == "%atomic_cas_loc" && as.size() == 3)
          return loc_call("caml_atomic_cas_field",
                          {expr(*as[1].second), expr(*as[2].second)});
        if (prim == "%atomic_fetch_add_loc" && as.size() == 2)
          return loc_call("caml_atomic_fetch_add_field", {expr(*as[1].second)});
      }
    }
    // Sys compile-time constants (%word_size, %big_endian, %ostype_unix, ...):
    // the oracle lowers each to `(sys.constant_X 0)`; in bytecode that is a ccall
    // to the runtime caml_sys_const_X applied to the unit argument.  sys.ml runs
    // these at module init (`let word_size = word_size ()`), so they must lower.
    {
      static const std::unordered_map<std::string, const char*> sysconst = {
        {"%big_endian", "caml_sys_const_big_endian"},
        {"%word_size", "caml_sys_const_word_size"},
        {"%int_size", "caml_sys_const_int_size"},
        {"%max_wosize", "caml_sys_const_max_wosize"},
        {"%ostype_unix", "caml_sys_const_ostype_unix"},
        {"%ostype_win32", "caml_sys_const_ostype_win32"},
        {"%ostype_cygwin", "caml_sys_const_ostype_cygwin"},
        {"%backend_type", "caml_sys_const_backend_type"},
        {"%standard_library_default", "caml_sys_const_standard_library_default"},
      };
      if (auto it = sysconst.find(prim); it != sysconst.end() && as.size() == 1) {
        auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = it->second;
        pr->args = {expr(*as[0].second)}; return pr;
      }
    }
    // Pervasive arithmetic primitives reached module-qualified (Int.add = %addint,
    // Float.add = %addfloat, Float.of_int = %floatofint, ...): emit their operator
    // forms, the same way the unqualified pervasives do.
    {
      static const std::unordered_map<std::string, std::string> parith = {
        {"%addint", "+"}, {"%subint", "-"}, {"%mulint", "*"}, {"%divint", "/"},
        {"%modint", "mod"}, {"%negint", "~-"}, {"%andint", "land"}, {"%orint", "lor"},
        {"%xorint", "lxor"}, {"%lslint", "lsl"}, {"%lsrint", "lsr"}, {"%asrint", "asr"},
        {"%addfloat", "+."}, {"%subfloat", "-."}, {"%mulfloat", "*."}, {"%divfloat", "/."},
        {"%negfloat", "~-."}, {"%floatofint", "float_of_int"}, {"%intoffloat", "int_of_float"},
      };
      if (auto pi = parith.find(prim); pi != parith.end() && (int)as.size() == arity) {
        Prim p;
        if (int_op(pi->second, p)) {
          auto pr = mk(Lam::K::Prim); pr->prim = p; pr->args = args(); return pr;
        }
        if (auto pp = pervasive_prim(pi->second); !pp.first.empty()) return op(pp.first);
      }
    }
    // Boxed-integer primitives (`%int32_add`, `%int64_mul`, `%nativeint_sub`, ...)
    // print `Int32.add` etc. -- the module-qualified operation name (the suffix
    // after the prefix, e.g. add / sub / mul / div / mod / of_int / to_int / lsl).
    {
      static const std::pair<std::string, std::string> bint[] = {
        {"%int32_", "Int32."}, {"%int64_", "Int64."}, {"%nativeint_", "Nativeint."}};
      for (auto& [pfx, mod] : bint)
        if (prim.rfind(pfx, 0) == 0 && (int)as.size() == arity) {
          auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall;
          pr->prim_id = mod + prim.substr(pfx.size());
          pr->args = args();
          return pr;
        }
    }
    // Bigarray accessors: the kind/layout-less spelling (what ocamlc prints
    // when inference supplies no type info); the bytecode back end uses the
    // generic C entry points regardless of kind, exactly like upstream bytegen.
    if (prim.rfind("%caml_ba_", 0) == 0 && (int)as.size() == arity) {
      static const std::pair<const char*, const char*> ba[] = {
          {"%caml_ba_ref_", "get"}, {"%caml_ba_unsafe_ref_", "unsafe_get"},
          {"%caml_ba_set_", "set"}, {"%caml_ba_unsafe_set_", "unsafe_set"}};
      for (auto& [pfx, nm] : ba)
        if (prim.rfind(pfx, 0) == 0)
          return op(std::string("Bigarray.") + nm + "[generic,unknown]");
      if (prim.rfind("%caml_ba_dim_", 0) == 0)
        return op("Bigarray.dim_" + prim.substr(sizeof("%caml_ba_dim_") - 1));
    }
    // String/bytes/bigstring 16/32/64-bit accessors (printlambda's spelling);
    // bytecode calls the same checked C entry points for safe and unsafe.
    if ((int)as.size() == arity)
      if (const std::string* sp = mem_access_spelling(prim)) return op(*sp);
    // String/bytes element access + length as locally-declared externals (the
    // printlambda spelling; bytecode calls the checked C entry for safe/unsafe).
    if (prim == "%string_unsafe_get" && as.size() == 2) return op("string.unsafe_get");
    if (prim == "%string_safe_get" && as.size() == 2) return op("string.get");
    if (prim == "%bytes_unsafe_get" && as.size() == 2) return op("bytes.unsafe_get");
    if (prim == "%bytes_safe_get" && as.size() == 2) return op("bytes.get");
    if (prim == "%bytes_unsafe_set" && as.size() == 3) return op("bytes.unsafe_set");
    if (prim == "%bytes_safe_set" && as.size() == 3) return op("bytes.set");
    if (prim == "%string_length" && as.size() == 1) return op("string.length");
    if (prim == "%bytes_length" && as.size() == 1) return op("bytes.length");
    // bytes<->string conversions are representation no-ops (identity).  The real
    // prim names are %bytes_to_string / %bytes_of_string (bytes.ml's
    // unsafe_to_string / unsafe_of_string); also accept the %string_* spellings.
    if ((prim == "%bytes_to_string" || prim == "%bytes_of_string" ||
         prim == "%string_to_bytes" || prim == "%string_of_bytes") && as.size() == 1)
      return expr(*as[0].second);
    // Array access builtins as locally-declared externals: the same spelling
    // and lowering as Array.get/set (element kind from the application).
    if (prim == "%array_safe_get" && as.size() == 2)
      return op("array.get[" + array_elem_kind(&e) + "]");
    if (prim == "%array_unsafe_get" && as.size() == 2)
      return op("array.unsafe_get[" + array_elem_kind(&e) + "]");
    if (prim == "%array_safe_set" && as.size() == 3)
      return op("array.set[" + array_elem_kind(as[2].second.get()) + "]");
    if (prim == "%array_unsafe_set" && as.size() == 3)
      return op("array.unsafe_set[" + array_elem_kind(as[2].second.get()) + "]");
    if (prim == "%array_length" && as.size() == 1)
      return op("array.length[" + array_arg_kind(as[0].second.get()) + "]");
    // Atomic.Array: `unsafe_index t i` is the atomic LOCATION of element i, i.e.
    // the 2-field (obj, field-index) block -- element i IS field i of the array
    // block (translprim's make_atomic_loc).  `check_array_bound t i` is
    // caml_check_bound(length t, i) (translprim's Pcheckbound).
    if (prim == "%atomic_unsafe_index" && as.size() == 2) {
      auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
      blk->args = {expr(*as[0].second), expr(*as[1].second)};
      return blk;
    }
    if (prim == "%check_array_bound" && as.size() == 2) {
      auto len = mk(Lam::K::Prim); len->prim = Prim::IntCmp;
      len->prim_id = "array.length[" + array_arg_kind(as[0].second.get()) + "]";
      len->args = {expr(*as[0].second)};
      auto cb = mk(Lam::K::Prim); cb->prim = Prim::Ccall; cb->prim_id = "caml_check_bound";
      cb->args = {len, expr(*as[1].second)};
      return cb;
    }
    // Effect primitives applied directly (effect.ml's `resume (..) f v` etc.):
    // lower to the special bytecode ops the backend emits (Kperform / Kresume /
    // Kresumeterm / Kreperformterm).  %resume and %runstack are the same op.
    {
      auto eff = [&](const char* fn) {
        auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = fn;
        pr->args = args(); return pr; };
      if (prim == "%perform" && as.size() == 1) return eff("perform");
      if ((prim == "%resume" || prim == "%runstack") && as.size() == 3) return eff("runstack");
      if (prim == "%reperform" && as.size() == 2) return eff("reperform");
    }
    // Method-dispatch primitives applied directly (camlinternalOO's `sendself obj
    // n` / `sendcache ..`): a Send prim the backend lowers to Kgetmethod (self) /
    // Kgetpubmet|Kgetdynmet (public).  In bytecode %sendcache is a public send
    // (cache/pos ignored -- the cached path is native-only).
    {
      auto snd = [&](const char* kind) {
        auto s = mk(Lam::K::Prim); s->prim = Prim::Send; s->prim_id = kind;
        s->args = {expr(*as[0].second), expr(*as[1].second)}; return s; };
      if (prim == "%send" && as.size() == 2) return snd("send");
      if (prim == "%sendself" && as.size() == 2) return snd("sendself");
      if (prim == "%sendcache" && as.size() == 4) return snd("send");
    }
    // Obj.size/field/set_field: translprim lowers these to generic-array
    // operations (Obj.t is opaque, so the kind is always `gen`).
    if (prim == "%obj_size" && as.size() == 1) return op("array.length[gen]");
    if (prim == "%obj_field" && as.size() == 2) return op("array.unsafe_get[gen]");
    if (prim == "%obj_set_field" && as.size() == 3) return op("array.unsafe_set[gen]");
    // Byte-swap builtins: %bswap16 prints bare, the boxed ones module-qualified.
    if ((int)as.size() == arity && arity == 1) {
      static const std::unordered_map<std::string, std::string> bsw = {
          {"%bswap16", "bswap16"}, {"%bswap_int32", "Int32.bswap"},
          {"%bswap_int64", "Int64.bswap"}, {"%bswap_native", "Nativeint.bswap"}};
      if (auto b = bsw.find(prim); b != bsw.end()) {
        auto pr = mk(Lam::K::Prim);
        pr->prim = prim == "%bswap16" ? Prim::IntCmp : Prim::Ccall;
        pr->prim_id = b->second; pr->args = args();
        return pr;
      }
    }
    // A C-external (non-`%`) primitive applied at its full arity -> a C call.
    if (!prim.empty() && prim[0] != '%' && (int)as.size() == arity) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = prim;
      pr->args = args();
      if (prim == "caml_obj_with_tag")
        if (auto r = fold_with_tag(pr->args)) return r;
      return pr;
    }
    // A `%`-primitive we did not specialise above, applied at its full arity:
    // emit the primitive directly (the same lowering prim_stub_body uses for the
    // value-position eta-stub, but with the real arguments).  translprim applies
    // a saturated primitive directly -- letting the caller fall through to the
    // generic apply would instead eta-expand it into a `stub` closure and apply
    // that, allocating one closure per call (breaks Set/Map's #6645
    // no-allocation assertions: `compare (x:t) y = compare x y` over an abstract
    // `t` lowers `compare x y` to a polymorphic caml_compare here).
    if (!prim.empty() && prim[0] == '%' && (int)as.size() == arity)
      if (LamPtr b = prim_stub_body(prim, args())) return b;
    return nullptr;
  }

  // Simplif's Obj.with_tag folding (lambda/simplif.ml): `caml_obj_with_tag` of a
  // compile-time tag and a directly-allocated block is the same allocation with
  // the new tag; of a structured constant, the retagged constant.  Null when the
  // shape doesn't match (the generic C call stands).
  LamPtr fold_with_tag(const std::vector<LamPtr>& args) {
    if (args.size() != 2) return nullptr;
    const LamPtr& t = args[0];
    if (t->k != Lam::K::ConstInt || !t->str_val.empty()) return nullptr;
    const LamPtr& b = args[1];
    bool makeblk = b->k == Lam::K::Prim &&
                   (b->prim == Prim::Makeblock || b->prim == Prim::Makemutable);
    if (!makeblk && b->k != Lam::K::ConstBlock) return nullptr;
    auto r = std::make_shared<Lam>(*b);
    r->prim_arg = (int)t->int_val;
    return r;
  }

  // --- lazy values (Texp_lazy / %lazy_force), mirroring translcore + matching ---
  // The raw inferred runtime-class string for an expression: "int"/"float"/
  // "addr" (known boxed)/"" (unrecorded, treated as the polymorphic Any).
  std::string vk_str(const Expression* e) {
    auto it = vk.expr.find(e);
    return it == vk.expr.end() ? "" : it->second;
  }
  // Typeopt.classify_lazy_argument's "small and commutative" test: an expression
  // with no coeffects and only generative effects, under a size cutoff of 42.
  bool lazy_commutative(const Expression* e, int& size) {
    if (++size > 42) return false;
    if (std::get_if<Pexp_ident>(&e->desc) || std::get_if<Pexp_constant>(&e->desc) ||
        std::get_if<Pexp_function>(&e->desc) || std::get_if<Pexp_lazy>(&e->desc) ||
        std::get_if<Pexp_extension>(&e->desc))  // extension_constructor approx
      return true;
    if (auto* v = std::get_if<Pexp_variant>(&e->desc))
      return !v->arg || lazy_commutative(v->arg->get(), size);
    if (auto* c = std::get_if<Pexp_construct>(&e->desc))
      return !c->arg || lazy_commutative(c->arg->get(), size);
    if (auto* a = std::get_if<Pexp_array>(&e->desc)) {
      for (auto& el : a->elems) if (!lazy_commutative(el.get(), size)) return false;
      return true;
    }
    if (auto* t = std::get_if<Pexp_tuple>(&e->desc)) {
      for (auto& el : t->elems) if (!lazy_commutative(el.get(), size)) return false;
      return true;
    }
    if (auto* r = std::get_if<Pexp_record>(&e->desc)) {
      for (auto& f : r->fields) if (!lazy_commutative(f.second.get(), size)) return false;
      return !r->base || lazy_commutative(r->base->get(), size);
    }
    return false;
  }
  // `lazy e`.  A non-commutative body becomes a thunk in a Lazy_tag block; a
  // small commutative body is evaluated eagerly and either shortcut to the value
  // itself (immediate/known-boxed: forcing returns it) or wrapped in a Forward_tag
  // block (float, or a body of polymorphic/lazy type, where the shortcut is unsafe).
  LamPtr lazy_expr(const Expression& e) {
    int size = 0;
    if (!lazy_commutative(&e, size)) {  // Lazy_thunk: (makelazyblock (function param e))
      auto fn = mk(Lam::K::Function); fn->params = {{fresh("param"), ValueKind::Gen}};
      fn->body = expr(e);
      auto b = mk(Lam::K::Prim); b->prim = Prim::Makelazyblock; b->prim_arg = 246;
      b->args = {fn};
      return b;
    }
    bool forward;  // Eager: float and the polymorphic/lazy `Any` class -> Forward
    if (std::get_if<Pexp_constant>(&e.desc))
      forward = std::holds_alternative<Pconst_float>(
                    std::get_if<Pexp_constant>(&e.desc)->c.desc);
    else if (std::get_if<Pexp_construct>(&e.desc) || std::get_if<Pexp_variant>(&e.desc) ||
             std::get_if<Pexp_array>(&e.desc) || std::get_if<Pexp_tuple>(&e.desc) ||
             std::get_if<Pexp_record>(&e.desc) || std::get_if<Pexp_function>(&e.desc))
      forward = false;  // always a (boxed or immediate) non-lazy value -> Shortcut
    else {  // ident / field: by runtime class (addr & int shortcut, float/'a forward)
      std::string k = vk_str(&e);
      forward = (k == "float" || (k != "int" && k != "addr" && k != "string"));
    }
    if (!forward) return expr(e);  // Shortcut
    auto b = mk(Lam::K::Prim); b->prim = Prim::Makelazyblock; b->prim_arg = 250;  // Forward
    b->args = {expr(e)};
    return b;
  }
  // Matching.inline_lazy_force: the inlined head of Lazy.force.
  //   (let (lzarg = arg)              -- elided when arg is already a variable
  //     (let (tag =a (caml_obj_tag lzarg))
  //       (if (== tag 250) (field_mut 0 lzarg)         -- Forward_tag: already a value
  //         (if (|| (== tag 246) (== tag 244))         -- Lazy_tag / Forcing_tag
  //           (apply (field_imm <force_lazy_block> CamlinternalLazy) (opaque lzarg))
  //           lzarg))))                                -- otherwise the value itself
  LamPtr force_lazy(LamPtr arg) {
    LamPtr lzarg; Ident la; bool bind = arg->k != Lam::K::Var;
    if (bind) { la = fresh("lzarg"); auto v = mk(Lam::K::Var); v->var = la; lzarg = v; }
    else lzarg = arg;
    auto use = [&] { auto v = mk(Lam::K::Var); v->var = lzarg->var; return v; };
    Ident tagid = fresh("tag");
    auto tagvar = [&] { auto v = mk(Lam::K::Var); v->var = tagid; return v; };
    auto tag_call = mk(Lam::K::Prim); tag_call->prim = Prim::Ccall;
    tag_call->prim_id = "caml_obj_tag"; tag_call->args = {use()};
    auto eqtag = [&](int t) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::EqInt; pr->args = {tagvar(), cint(t)};
      return pr;
    };
    // force_lazy_block: CamlinternalLazy.force_lazy_block (resolve its field index).
    auto& fm = fields_of("CamlinternalLazy");
    int fidx = 1; if (auto f = fm.find("force_lazy_block"); f != fm.end()) fidx = f->second;
    auto opq = mk(Lam::K::Prim); opq->prim = Prim::IntCmp; opq->prim_id = "opaque"; opq->args = {use()};
    auto call = mk(Lam::K::Apply); call->fn = field_of(global_of("CamlinternalLazy"), fidx);
    call->args = {opq};
    auto orcond = mk(Lam::K::Prim); orcond->prim = Prim::IntCmp; orcond->prim_id = "||";
    orcond->args = {eqtag(246), eqtag(244)};
    auto inner = mk(Lam::K::IfThenElse);
    inner->cond = orcond; inner->then_ = call; inner->else_ = use();
    auto fwd = mk(Lam::K::Prim); fwd->prim = Prim::FieldMut; fwd->prim_arg = 0; fwd->args = {use()};
    auto outer = mk(Lam::K::IfThenElse);
    outer->cond = eqtag(250); outer->then_ = fwd; outer->else_ = inner;
    auto tlet = mk(Lam::K::Let);  // (let (tag =a (caml_obj_tag ..)) ...)
    tlet->bindings = {{tagid, ValueKind::Gen, tag_call, /*alias=*/true}};
    tlet->body = outer;
    if (!bind) return tlet;
    auto alet = mk(Lam::K::Let);  // (let (lzarg = arg) ...)
    alet->bindings = {{la, ValueKind::Gen, arg}};
    alet->body = tlet;
    return alet;
  }

  static bool is_const(const LamPtr& l) {
    return l->k == Lam::K::ConstInt || l->k == Lam::K::ConstChar ||
           l->k == Lam::K::ConstFloat || l->k == Lam::K::ConstString ||
           l->k == Lam::K::ConstBlock;
  }
  // Build a block: a struct-const [tag: ...] if all fields are constant, else a
  // dynamic (makeblock tag ...).
  LamPtr block(int tag, std::vector<LamPtr> fields) {
    bool allc = true;
    for (auto& f : fields) if (!is_const(f)) allc = false;
    auto b = mk(allc ? Lam::K::ConstBlock : Lam::K::Prim);
    if (!allc) b->prim = Prim::Makeblock;
    b->prim_arg = tag;
    b->args = std::move(fields);
    return b;
  }
  // block() from field expressions, attaching the per-field value-kind shape (a
  // dynamic makeblock prints `(makeblock T (k1,k2,...) ...)`; all-gen -> no shape).
  LamPtr block_of(int tag, const std::vector<const Expression*>& exprs) {
    std::vector<LamPtr> fields; std::vector<ValueKind> shape;
    for (auto* ex : exprs) { fields.push_back(expr(*ex)); shape.push_back(expr_kind(ex)); }
    auto b = block(tag, std::move(fields));
    if (b->k == Lam::K::Prim) b->blk_shape = std::move(shape);
    return b;
  }

  LamPtr cint(long long n) { auto z = mk(Lam::K::ConstInt); z->int_val = n; return z; }
  LamPtr varof(const Ident& id) { auto v = mk(Lam::K::Var); v->var = id; return v; }

  // Wrap the module body in the `=a` bindings for the shared method/variable-name
  // const blocks (Translobj.transl_label_init_general); single-use ones inline
  // into their use site (Simplif).  Multiply-used blocks print in reverse creation
  // order (Ident.Map.fold places the largest stamp outermost).
  LamPtr wrap_shared(LamPtr body) {
    if (shared_consts_.empty()) return body;
    std::vector<Lam::Binding> keep;
    for (auto it = shared_consts_.rbegin(); it != shared_consts_.rend(); ++it) {
      auto& b = it->second;
      if (count_var(body, b.id) <= 1) subst_var(body, b.id, b.val);
      else keep.push_back(b);
    }
    if (keep.empty()) return body;
    auto let = mk(Lam::K::Let); let->bindings = std::move(keep); let->body = body;
    return let;
  }

  // A polymorphic-variant tag `` `Foo `` is represented at runtime by the hash of
  // its name (caml_hash_variant / typing/btype.ml hash_variant): an accumulator
  // mod 2^31, normalized to the signed range of an OCaml int.
  static long long hash_variant(const std::string& s) {
    unsigned long long accu = 0;
    for (unsigned char c : s) accu = 223 * accu + c;
    long long r = (long long)(accu & ((1ULL << 31) - 1));
    if (r > 0x3FFFFFFF) r -= (1LL << 31);
    return r;
  }
  LamPtr alloc_dummy(int size, const char* prim = "caml_alloc_dummy") {
    auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = prim;
    pr->args = {cint(size)}; return pr;
  }
  LamPtr update_dummy(const Ident& id, const LamPtr& val,
                      const char* prim = "caml_update_dummy") {
    auto v = mk(Lam::K::Var); v->var = id;
    auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = prim;
    pr->args = {v, val}; return pr;
  }
  struct RecParts {
    std::vector<Lam::Binding> dummies, funcs;
    std::vector<LamPtr> updates;
  };
  // The size of a rec RHS, mirroring value_rec_compiler's compute_static_size:
  // the tail spine (lets, sequences, single-reachable branches) determines
  // whether the value is a sized heap block, a function, or a constant; `env`
  // carries the sizes of spine-bound locals so `let x = 1 :: t in x` sizes as
  // the block.  Dyn means no class fits (caller falls back).
  struct RSize {
    enum K { Dyn, Unreach, Const, Func, Block } k = Dyn;
    int n = 0;
    enum B { Reg, Flt, Lzy } b = Reg;  // block kind: regular / float record / lazy
  };
  static RSize rjoin(const RSize& a, const RSize& b) {
    if (a.k == RSize::Unreach) return b;
    if (b.k == RSize::Unreach) return a;
    return {};  // two reachable branches: upstream join_sizes fails
  }
  static RSize static_size(const LamPtr& l, std::map<int, RSize>& env) {
    if (!l) return {};
    switch (l->k) {
      case Lam::K::ConstInt: case Lam::K::ConstChar: case Lam::K::ConstFloat:
      case Lam::K::ConstString: case Lam::K::ConstBlock: return {RSize::Const, 0};
      case Lam::K::Function: return {RSize::Func, 0};
      case Lam::K::Var: {
        auto it = env.find(l->var.stamp);
        return it != env.end() ? it->second : RSize{};
      }
      case Lam::K::Let:
        for (auto& b : l->bindings) {
          RSize s = static_size(b.val, env);
          env[b.id.stamp] = s;
        }
        return static_size(l->body, env);
      case Lam::K::Letrec:
        for (auto& b : l->bindings) env[b.id.stamp] = {RSize::Func, 0};
        return static_size(l->body, env);
      case Lam::K::Sequence: return static_size(l->else_, env);
      case Lam::K::IfThenElse:
        return rjoin(static_size(l->then_, env), static_size(l->else_, env));
      case Lam::K::Try: return rjoin(static_size(l->body, env), static_size(l->then_, env));
      case Lam::K::Catch: return rjoin(static_size(l->cond, env), static_size(l->then_, env));
      case Lam::K::Switch: {
        RSize r{RSize::Unreach, 0};
        for (auto& sc : l->sw_consts) r = rjoin(r, static_size(sc.body, env));
        for (auto& sc : l->sw_blocks) r = rjoin(r, static_size(sc.body, env));
        if (l->sw_default) r = rjoin(r, static_size(l->sw_default, env));
        return r;
      }
      case Lam::K::Staticraise: return {RSize::Unreach, 0};
      case Lam::K::Prim:
        if (l->prim == Prim::Raise || l->prim == Prim::Reraise) return {RSize::Unreach, 0};
        if (l->prim == Prim::Makeblock || l->prim == Prim::Makemutable)
          return {RSize::Block, (int)l->args.size()};
        if (l->prim == Prim::Makelazyblock)  // lazy/forward blocks share the lazy dummy
          return {RSize::Block, 0, RSize::Lzy};
        if (l->prim == Prim::IntCmp && l->prim_id.rfind("makearray", 0) == 0)
          return {RSize::Block, (int)l->args.size(),
                  l->prim_id.find("[float]") != std::string::npos ? RSize::Flt : RSize::Reg};
        return {};
      default: return {};
    }
  }
  // Free variables of l (stamp -> ident), respecting binders -- which context
  // locals does a lifted letrec function capture?
  static void free_vars(const LamPtr& l, std::set<int>& bound, std::map<int, Ident>& out) {
    if (!l) return;
    switch (l->k) {
      case Lam::K::Var: case Lam::K::Mutvar:
        if (!bound.count(l->var.stamp)) out.emplace(l->var.stamp, l->var);
        return;
      case Lam::K::Assign:
        if (!bound.count(l->var.stamp)) out.emplace(l->var.stamp, l->var);
        free_vars(l->cond, bound, out);
        return;
      case Lam::K::Function: {
        std::set<int> b2 = bound;
        for (auto& [id, k] : l->params) b2.insert(id.stamp);
        free_vars(l->body, b2, out);
        return;
      }
      case Lam::K::Let: {
        std::set<int> b2 = bound;
        for (auto& b : l->bindings) { free_vars(b.val, b2, out); b2.insert(b.id.stamp); }
        free_vars(l->body, b2, out);
        return;
      }
      case Lam::K::Letrec: {
        std::set<int> b2 = bound;
        for (auto& b : l->bindings) b2.insert(b.id.stamp);
        for (auto& b : l->bindings) free_vars(b.val, b2, out);
        free_vars(l->body, b2, out);
        return;
      }
      case Lam::K::Try: {
        free_vars(l->body, bound, out);
        std::set<int> b2 = bound; b2.insert(l->var.stamp);
        free_vars(l->then_, b2, out);
        return;
      }
      case Lam::K::Catch: {
        free_vars(l->cond, bound, out);
        std::set<int> b2 = bound;
        for (auto& v : l->catch_vars) b2.insert(v.stamp);
        free_vars(l->then_, b2, out);
        return;
      }
      case Lam::K::For: {
        free_vars(l->then_, bound, out); free_vars(l->else_, bound, out);
        std::set<int> b2 = bound; b2.insert(l->var.stamp);
        free_vars(l->body, b2, out);
        return;
      }
      default:
        free_vars(l->fn, bound, out); free_vars(l->body, bound, out);
        free_vars(l->cond, bound, out); free_vars(l->then_, bound, out);
        free_vars(l->else_, bound, out); free_vars(l->sw_default, bound, out);
        for (auto& a : l->args) free_vars(a, bound, out);
        for (auto& b : l->bindings) free_vars(b.val, bound, out);
        for (auto& sc : l->sw_consts) free_vars(sc.body, bound, out);
        for (auto& sc : l->sw_blocks) free_vars(sc.body, bound, out);
        return;
    }
  }
  // Replace each captured local (slots: stamp -> field index) with
  // `(field_imm i ctx)`: a lifted function reads its context block.  Stamps
  // are unique, so no binder handling is needed.
  static void subst_ctx(LamPtr& l, const std::map<int, int>& slots, const Ident& ctx) {
    if (!l) return;
    if (l->k == Lam::K::Var) {
      auto it = slots.find(l->var.stamp);
      if (it == slots.end()) return;
      auto cv = mk(Lam::K::Var); cv->var = ctx;
      auto f = mk(Lam::K::Prim); f->prim = Prim::FieldImm; f->prim_arg = it->second;
      f->args = {cv};
      l = f;
      return;
    }
    subst_ctx(l->fn, slots, ctx); subst_ctx(l->body, slots, ctx);
    subst_ctx(l->cond, slots, ctx); subst_ctx(l->then_, slots, ctx);
    subst_ctx(l->else_, slots, ctx); subst_ctx(l->sw_default, slots, ctx);
    for (auto& a : l->args) subst_ctx(a, slots, ctx);
    for (auto& b : l->bindings) subst_ctx(b.val, slots, ctx);
    for (auto& sc : l->sw_consts) subst_ctx(sc.body, slots, ctx);
    for (auto& sc : l->sw_blocks) subst_ctx(sc.body, slots, ctx);
  }
  // value_rec_compiler's split_static_function: descend the tail spine of a
  // non-syntactic function RHS.  On commit, the tail function is lifted into
  // `lifted` (its captured context locals rebound through ctx, see subst_ctx)
  // and the tail slot becomes the context block (makeblock of the captured
  // locals, in ascending-stamp order like upstream's Ident.Set); a variable
  // tail eta-expands to a `stub` wrapper applying the single block field.
  // Branch points admit at most one reachable arm.  The check pass
  // (commit=false) never mutates, so a Fail leaves the term intact.
  enum class SplitR { Fail, Unreach, Ok };
  SplitR split_fn(LamPtr& slot, const Ident& ctx, std::map<int, Ident>& locals,
                  bool commit, LamPtr& lifted, int& blk_size) {
    LamPtr l = slot;
    if (!l) return SplitR::Fail;
    switch (l->k) {
      case Lam::K::Var: {  // eta-expansion
        if (!commit) return SplitR::Ok;
        Ident p = fresh("let_rec_param");
        auto cv = mk(Lam::K::Var); cv->var = ctx;
        auto fld = mk(Lam::K::Prim); fld->prim = Prim::FieldImm; fld->prim_arg = 0;
        fld->args = {cv};
        auto pv = mk(Lam::K::Var); pv->var = p;
        auto ap = mk(Lam::K::Apply); ap->fn = fld; ap->args = {pv};
        auto fn = mk(Lam::K::Function); fn->params = {{p, ValueKind::Gen}};
        fn->inline_attr = "stub"; fn->body = ap;
        lifted = fn; blk_size = 1;
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = {l};
        slot = blk;
        return SplitR::Ok;
      }
      case Lam::K::Function: {
        if (!commit) return SplitR::Ok;
        std::set<int> bound; std::map<int, Ident> fv;
        free_vars(l, bound, fv);
        std::map<int, int> slots; std::vector<LamPtr> fields;
        for (auto& [s, id] : fv)
          if (locals.count(s)) {
            slots[s] = (int)fields.size();
            auto v = mk(Lam::K::Var); v->var = id; fields.push_back(v);
          }
        subst_ctx(l->body, slots, ctx);
        lifted = l; blk_size = (int)fields.size();
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = std::move(fields);
        slot = blk;
        return SplitR::Ok;
      }
      case Lam::K::Let: case Lam::K::Letrec:
        for (auto& b : l->bindings) locals.emplace(b.id.stamp, b.id);
        return split_fn(l->body, ctx, locals, commit, lifted, blk_size);
      case Lam::K::Sequence:
        return split_fn(l->else_, ctx, locals, commit, lifted, blk_size);
      case Lam::K::IfThenElse:
        return split_arms({&l->then_, &l->else_}, ctx, locals, commit, lifted, blk_size);
      case Lam::K::Try:
        locals.emplace(l->var.stamp, l->var);
        return split_arms({&l->body, &l->then_}, ctx, locals, commit, lifted, blk_size);
      case Lam::K::Catch:
        for (auto& v : l->catch_vars) locals.emplace(v.stamp, v);
        return split_arms({&l->cond, &l->then_}, ctx, locals, commit, lifted, blk_size);
      case Lam::K::Switch: {
        std::vector<LamPtr*> arms;
        for (auto& sc : l->sw_consts) arms.push_back(&sc.body);
        for (auto& sc : l->sw_blocks) arms.push_back(&sc.body);
        if (l->sw_default) arms.push_back(&l->sw_default);
        return split_arms(arms, ctx, locals, commit, lifted, blk_size);
      }
      case Lam::K::Staticraise: return SplitR::Unreach;
      case Lam::K::Prim:
        return (l->prim == Prim::Raise || l->prim == Prim::Reraise)
                   ? SplitR::Unreach : SplitR::Fail;
      default: return SplitR::Fail;
    }
  }
  SplitR split_arms(const std::vector<LamPtr*>& arms, const Ident& ctx,
                    std::map<int, Ident>& locals, bool commit,
                    LamPtr& lifted, int& blk_size) {
    LamPtr* ok = nullptr;
    for (auto* a : arms) {
      SplitR r = split_fn(*a, ctx, locals, false, lifted, blk_size);
      if (r == SplitR::Fail) return SplitR::Fail;
      if (r == SplitR::Ok) {
        if (ok) return SplitR::Fail;  // multiple reachable functions
        ok = a;
      }
    }
    if (!ok) return SplitR::Unreach;
    return commit ? split_fn(*ok, ctx, locals, true, lifted, blk_size) : SplitR::Ok;
  }
  // Simplif's `(let (x = e) x)` -> e on the binding spine.  ocamlc runs this
  // simplification *after* value_rec compilation, so rec RHSs reach
  // partition_rec un-collapsed (rec_spine_ suppresses the translate-time
  // rule) and are collapsed here afterwards.
  static LamPtr collapse_let_id(LamPtr l) {
    if (!l) return l;
    if (l->k == Lam::K::Let && l->body) {
      l->body = collapse_let_id(l->body);
      while (!l->bindings.empty() && !l->bindings.back().mut &&
             l->body->k == Lam::K::Var &&
             l->body->var.stamp == l->bindings.back().id.stamp) {
        LamPtr v = l->bindings.back().val;
        l->bindings.pop_back();
        l->body = collapse_let_id(v);
      }
      if (l->bindings.empty()) return l->body;
    } else if (l->k == Lam::K::Sequence && l->else_) {
      l->else_ = collapse_let_id(l->else_);
    }
    return l;
  }
  // Partition a `let rec` group for value recursion, mirroring ocamlc's
  // value_rec_compiler: sized heap blocks become caml_alloc_dummy pre-bindings
  // backpatched in place; a binding that references no rec id at all is
  // upstream's Dynamic class -- an ordinary binding (constants, but also e.g.
  // `let rec foof = f` aliasing an outer name), placed after the allocs;
  // syntactic functions stay a letrec; a non-syntactic function splits into a
  // lifted function plus a backpatched context block (split_fn).  The result
  // shape is `(let <allocs;dynamics> (letrec <funcs> (seq <updates> body)))`.
  // False when every binding is already a syntactic function (plain letrec)
  // or a binding fits no class (unsupported; caller keeps its existing path).
  bool partition_rec(const std::vector<Ident>& ids, const std::vector<ValueKind>& kinds,
                     const std::vector<LamPtr>& vals, RecParts& out) {
    enum Cls { Func, Lift, Block, Dyn };
    std::vector<Cls> cls(vals.size());
    std::vector<RSize> bsize(vals.size());
    std::set<int> rec_ids;
    for (auto& id : ids) rec_ids.insert(id.stamp);
    bool transform = false;  // any non-(syntactic-function) binding?
    for (size_t i = 0; i < vals.size(); ++i) {
      auto& v = vals[i];
      if (v->k == Lam::K::Function) { cls[i] = Func; continue; }
      transform = true;
      std::set<int> bound; std::map<int, Ident> fv;
      free_vars(v, bound, fv);
      bool refs_rec = false;
      for (int s : rec_ids) if (fv.count(s)) { refs_rec = true; break; }
      if (!refs_rec) { cls[i] = Dyn; continue; }
      std::map<int, RSize> env;
      RSize sz = static_size(v, env);
      if (sz.k == RSize::Block) { cls[i] = Block; bsize[i] = sz; continue; }
      if (sz.k == RSize::Func) {
        std::map<int, Ident> locals; LamPtr lf; int bs = 0;
        LamPtr probe = v;
        if (split_fn(probe, Ident{}, locals, false, lf, bs) == SplitR::Ok) {
          cls[i] = Lift;
          continue;
        }
      }
      return false;
    }
    if (!transform) return false;
    std::vector<Lam::Binding> dyns;
    for (size_t i = 0; i < vals.size(); ++i) {
      LamPtr v = vals[i];
      switch (cls[i]) {
        case Func: out.funcs.push_back({ids[i], kinds[i], v}); break;
        case Dyn:  // upstream binds dynamics with Pgenval, not the inferred kind
          dyns.push_back({ids[i], ValueKind::Gen, collapse_let_id(v)});
          break;
        case Block:
          if (bsize[i].b == RSize::Lzy) {
            // value_rec_compiler's Lazy_block: alloc takes unit, and a value
            // that isn't syntactically a lazy block is wrapped in
            // CamlinternalLazy.indirect so backpatching can't race a force.
            bool direct = v->k == Lam::K::Prim && v->prim == Prim::Makelazyblock;
            LamPtr nv = collapse_let_id(v);
            if (!direct) {
              auto& cil = fields_of("CamlinternalLazy");
              auto ind = cil.find("indirect");
              if (ind == cil.end()) return false;
              auto ap = mk(Lam::K::Apply);
              ap->fn = field_of("CamlinternalLazy", ind->second);
              ap->args = {nv};
              nv = ap;
            }
            out.dummies.push_back({ids[i], ValueKind::Gen,
                                   alloc_dummy(0, "caml_alloc_dummy_lazy")});
            out.updates.push_back(update_dummy(ids[i], nv, "caml_update_dummy_lazy"));
          } else if (bsize[i].b == RSize::Flt) {
            out.dummies.push_back({ids[i], ValueKind::Gen,
                                   alloc_dummy(bsize[i].n, "caml_alloc_dummy_float")});
            out.updates.push_back(update_dummy(ids[i], collapse_let_id(v)));
          } else {
            out.dummies.push_back({ids[i], ValueKind::Gen, alloc_dummy(bsize[i].n)});
            out.updates.push_back(update_dummy(ids[i], collapse_let_id(v)));
          }
          break;
        case Lift: {
          Ident ctx = fresh("letrec_function_context");
          std::map<int, Ident> locals; LamPtr lf; int bs = 0;
          split_fn(v, ctx, locals, true, lf, bs);
          out.dummies.push_back({ctx, ValueKind::Gen, alloc_dummy(bs)});
          out.funcs.push_back({ids[i], kinds[i], lf});
          out.updates.push_back(update_dummy(ctx, v));
          break;
        }
      }
    }
    out.dummies.insert(out.dummies.end(), dyns.begin(), dyns.end());
    return true;
  }
  LamPtr cchar(int c) { auto z = mk(Lam::K::ConstChar); z->int_val = c; return z; }
  LamPtr cstr(const std::string& s) { auto z = mk(Lam::K::ConstString); z->str_val = s; return z; }
  LamPtr cblock(int tag, std::vector<LamPtr> fs) {
    auto b = mk(Lam::K::ConstBlock); b->prim_arg = tag; b->args = std::move(fs); return b;
  }

  // Lower a format string into its CamlinternalFormatBasics value, matching the
  // -dlambda structured constant: `Format (fmt, original)` = [0: <fmt> <string>],
  // where <fmt> is a cons-list of format elements ending in End_of_format (int 0).
  // Returns null on any directive we don't yet encode, so the caller can fall back
  // to a plain string (partial support only improves parity, never regresses).
  // Tags follow camlinternalFormatBasics.ml's `fmt` GADT (block ctors 0..24).
  LamPtr format_value(const std::string& s) {
    LamPtr fmt = fmt_parse(s, 0, s.size());
    if (!fmt) return nullptr;
    return cblock(0, {fmt, cstr(s)});  // Format (fmt, original)
  }
  // Recursive mirror of CamlinternalFormat.fmt_ebb_of_string over s[i,end):
  // literal runs up to the next '%'/'@' become Char_literal(12)/String_literal(11),
  // then the directive parsers below.  Null on a directive outside the supported
  // subset, so the caller can fall back to a plain string.
  LamPtr fmt_parse(const std::string& s, size_t i, size_t end) {
    size_t j = i;
    while (j < end && s[j] != '%' && s[j] != '@') ++j;
    if (j > i) {
      LamPtr rest = fmt_parse(s, j, end);
      if (!rest) return nullptr;
      if (j - i == 1) return cblock(12, {cchar((unsigned char)s[i]), rest});
      return cblock(11, {cstr(s.substr(i, j - i)), rest});
    }
    if (i >= end) return cint(0);  // End_of_format
    if (s[i] == '@') return fmt_parse_at(s, i + 1, end);
    return fmt_parse_pct(s, i + 1, end);
  }
  // Formatting_lit(17) of a constant formatting_lit, then the rest.
  LamPtr fmt_flit(int tag, const std::string& s, size_t k, size_t end) {
    LamPtr r = fmt_parse(s, k, end);
    return r ? cblock(17, {cint(tag), r}) : nullptr;
  }
  // Formatting_lit(Break(src, width, offset)), then the rest.
  LamPtr fmt_break(const std::string& src, int w, int o,
                   const std::string& s, size_t k, size_t end) {
    LamPtr r = fmt_parse(s, k, end);
    return r ? cblock(17, {cblock(0, {cstr(src), cint(w), cint(o)}), r}) : nullptr;
  }
  // `@<spaces><integer><spaces>` helper for @;<w o> and @<n>: parses an optional
  // '-' sign and digits with surrounding blanks; false if no integer is present.
  static bool fmt_int(const std::string& s, size_t& i, size_t end, int& out) {
    while (i < end && s[i] == ' ') ++i;
    size_t d = i; bool neg = false;
    if (d < end && s[d] == '-') { neg = true; ++d; }
    if (d >= end || s[d] < '0' || s[d] > '9') return false;
    long v = 0;
    while (d < end && s[d] >= '0' && s[d] <= '9') v = v * 10 + (s[d] - '0'), ++d;
    while (d < end && s[d] == ' ') ++d;
    i = d; out = (int)(neg ? -v : v);
    return true;
  }
  // After '@' (parse_after_at): i points at the directive character.
  LamPtr fmt_parse_at(const std::string& s, size_t i, size_t end) {
    if (i >= end) return cblock(12, {cchar('@'), cint(0)});
    char d = s[i];
    switch (d) {
      case '[': case '{': {  // Formatting_gen(18): Open_box(1) / Open_tag(0)
        size_t k = i + 1;
        LamPtr sub = cint(0);  // Format(End_of_format, "") when no <...> follows
        std::string substr;
        if (k < end && s[k] == '<') {
          size_t gt = s.find('>', k + 1);
          if (gt != std::string::npos && gt < end) {
            substr = s.substr(k, gt - k + 1);
            sub = fmt_parse(s, k, gt + 1);  // the bracket text, parsed as a format
            if (!sub) return nullptr;
            k = gt + 1;
          }
        }
        LamPtr r = fmt_parse(s, k, end);
        if (!r) return nullptr;
        return cblock(18, {cblock(d == '{' ? 0 : 1, {cblock(0, {sub, cstr(substr)})}), r});
      }
      case ']': return fmt_flit(0, s, i + 1, end);    // Close_box
      case '}': return fmt_flit(1, s, i + 1, end);    // Close_tag
      case '?': return fmt_flit(2, s, i + 1, end);    // FFlush
      case '\n': return fmt_flit(3, s, i + 1, end);   // Force_newline
      case '.': return fmt_flit(4, s, i + 1, end);    // Flush_newline
      case '@': return fmt_flit(5, s, i + 1, end);    // Escaped_at
      case ',': return fmt_break("@,", 0, 0, s, i + 1, end);
      case ' ': return fmt_break("@ ", 1, 0, s, i + 1, end);
      case ';': {  // @; or @;<w> or @;<w o>; on malformed <...>, plain "@;"
        size_t k = i + 1;
        if (k < end && s[k] == '<') {
          size_t p = k + 1; int w = 0, o = 0;
          if (fmt_int(s, p, end, w)) {
            if (p < end && s[p] == '>')
              return fmt_break(s.substr(i - 1, p - i + 2), w, 0, s, p + 1, end);
            if (fmt_int(s, p, end, o) && p < end && s[p] == '>')
              return fmt_break(s.substr(i - 1, p - i + 2), w, o, s, p + 1, end);
          }
        }
        return fmt_break("@;", 1, 0, s, k, end);
      }
      case '<': {  // @<n> Magic_size(block 1) or, malformed, Scan_indic '<'
        size_t p = i + 1; int sz = 0;
        if (fmt_int(s, p, end, sz) && p < end && s[p] == '>') {
          LamPtr r = fmt_parse(s, p + 1, end);
          if (!r) return nullptr;
          return cblock(17, {cblock(1, {cstr(s.substr(i - 1, p - i + 2)), cint(sz)}), r});
        }
        LamPtr r = fmt_parse(s, i + 1, end);
        return r ? cblock(17, {cblock(2, {cchar('<')}), r}) : nullptr;
      }
      case '%':
        if (i + 1 < end && s[i + 1] == '%') return fmt_flit(6, s, i + 2, end);  // @%% Escaped_percent
        {  // lone @%: a literal '@', then re-parse at the '%'
          LamPtr r = fmt_parse(s, i, end);
          return r ? cblock(12, {cchar('@'), r}) : nullptr;
        }
      default: {  // any other char: Formatting_lit(Scan_indic(block 2))
        LamPtr r = fmt_parse(s, i + 1, end);
        return r ? cblock(17, {cblock(2, {cchar((unsigned char)d)}), r}) : nullptr;
      }
    }
  }
  // After '%': flags, padding, precision, length modifier, conversion.
  LamPtr fmt_parse_pct(const std::string& s, size_t i, size_t end) {
    if (i >= end) return nullptr;
    if (s[i] == '%' || s[i] == '@') {  // literal '%'/'@': its own Char_literal node
      LamPtr r = fmt_parse(s, i + 1, end);
      return r ? cblock(12, {cchar((unsigned char)s[i]), r}) : nullptr;
    }
    // `%_[nlNL]` (read-and-discard counter): Ignored_param(Ignored_scan_get_counter)
    // -- the only `%_` forms lowered; others keep the plain-string fallback.
    if (s[i] == '_' && i + 1 < end &&
        (s[i + 1] == 'n' || s[i + 1] == 'l' || s[i + 1] == 'L' || s[i + 1] == 'N') &&
        (s[i + 1] == 'N' || i + 2 >= end ||
         std::string_view("dixXou").find(s[i + 2]) == std::string_view::npos)) {
      char cc = s[i + 1];
      LamPtr r = fmt_parse(s, i + 2, end);
      if (!r) return nullptr;
      return cblock(23, {cblock(11, {cint(cc == 'l' ? 0 : cc == 'n' ? 1 : 2)}), r});
    }
    bool plus = false, space = false, hash = false, minus = false, zero = false;
    for (; i < end; ++i) {
      if (s[i] == '+') plus = true;
      else if (s[i] == ' ') space = true;
      else if (s[i] == '#') hash = true;
      else if (s[i] == '-') minus = true;
      else if (s[i] == '0') zero = true;
      else break;
    }
    if (i >= end) return nullptr;
    int padty = minus ? 0 : (zero ? 2 : 1);  // Left=0, Right=1, Zeros=2
    // padding: No_padding(0) | Lit_padding[0: padty w] | Arg_padding[1: padty] (%*)
    LamPtr pad = cint(0);
    if (s[i] == '*') { pad = cblock(1, {cint(padty)}); ++i; }
    else if (s[i] >= '0' && s[i] <= '9') {
      int width = 0;
      while (i < end && s[i] >= '0' && s[i] <= '9') width = width * 10 + (s[i] - '0'), ++i;
      pad = cblock(0, {cint(padty), cint(width)});
    }
    if (i >= end) return nullptr;
    // precision: No_precision(0) | Lit_precision[0: n] | Arg_precision(1) (.*)
    LamPtr prec = cint(0);
    if (s[i] == '.') {
      ++i;
      if (i < end && s[i] == '*') { prec = cint(1); ++i; }
      else {
        int p = 0;
        while (i < end && s[i] >= '0' && s[i] <= '9') p = p * 10 + (s[i] - '0'), ++i;
        prec = cblock(0, {cint(p)});
      }
    }
    if (i >= end) return nullptr;
    char len = 0;  // boxed-int length modifier, only before an int conversion
    if ((s[i] == 'l' || s[i] == 'n' || s[i] == 'L') && i + 1 < end &&
        std::string_view("dixXou").find(s[i + 1]) != std::string_view::npos) { len = s[i]; ++i; }
    char conv = s[i]; ++i;
    // `%(...%)` format substitution -> Format_subst(14) of (pad_option, fmtty,
    // continuation).  Only the empty inner form `%(%)` is lowered: its fmtty is
    // End_of_fmtty (the sole constant fmtty_rel ctor = int 0), and a width-less
    // `%(` has pad_option None (= int 0).  A non-empty inner / width falls back
    // to a plain string (which is unsafe in format position, but no worse than
    // before).  This is the common `%(%)` splice-a-format-argument idiom.
    if (conv == '(') {
      bool no_pad = pad->k == Lam::K::ConstInt && pad->int_val == 0;
      if (no_pad && i + 1 < end && s[i] == '%' && s[i + 1] == ')') {
        LamPtr rest = fmt_parse(s, i + 2, end);
        if (!rest) return nullptr;
        return cblock(14, {cint(0), cint(0), rest});  // None, End_of_fmtty, rest
      }
      return nullptr;
    }
    LamPtr r = fmt_parse(s, i, end);
    if (!r) return nullptr;
    switch (conv) {
      case 'c': return cblock(0, {r});                 // Char
      case 'C': return cblock(1, {r});                 // Caml_char
      case 's': return cblock(2, {pad, r});            // String
      case 'S': return cblock(3, {pad, r});            // Caml_string
      case 'b': case 'B': return cblock(9, {pad, r});  // Bool
      case 'a': return cblock(15, {r});                // Alpha
      case 't': return cblock(16, {r});                // Theta
      case '!': return cblock(10, {r});                // Flush
      case 'd': case 'i': case 'x': case 'X': case 'o': case 'u': {
        int ic = int_conv(conv, plus, space, hash);
        if (ic < 0) return nullptr;
        int tag = len == 'l' ? 5 : len == 'n' ? 6 : len == 'L' ? 7 : 4;  // Int32/Nativeint/Int64/Int
        return cblock(tag, {cint(ic), pad, prec, r});
      }
      case 'f': case 'e': case 'E': case 'g': case 'G': case 'F': case 'h': case 'H': {
        int flag = plus ? 1 : space ? 2 : 0;
        int kind = conv == 'f' ? 0 : conv == 'e' ? 1 : conv == 'E' ? 2 : conv == 'g' ? 3
                 : conv == 'G' ? 4 : conv == 'F' ? 5 : conv == 'h' ? 6 : 7;
        return cblock(8, {cblock(0, {cint(flag), cint(kind)}), pad, prec, r});
      }
      // a bare l/n/L (no int conversion follows -- the modifier branch above
      // didn't consume it) or N is the deprecated %u-style counter directive:
      // Scan_get_counter(Line=0 / Char=1 / Token=2)
      case 'l': case 'n': case 'L': case 'N':
        return cblock(21, {cint(conv == 'l' ? 0 : conv == 'n' ? 1 : 2), r});
      default: return nullptr;  // %r %{ %( %[ etc: fall back to plain string
    }
  }
  // int_conv tag for a %[dixXou] with +/space/# flags, or -1 if unrepresentable.
  static int int_conv(char conv, bool plus, bool space, bool hash) {
    switch (conv) {
      case 'd': return hash ? 13 : plus ? 1 : space ? 2 : 0;          // Int_d/pd/sd/Cd
      case 'i': return hash ? 14 : plus ? 4 : space ? 5 : 3;          // Int_i/pi/si/Ci
      case 'x': return hash ? 7 : 6;                                  // Int_x/Cx
      case 'X': return hash ? 9 : 8;                                  // Int_X/CX
      case 'o': return hash ? 11 : 10;                                // Int_o/Co
      case 'u': return hash ? 15 : 12;                                // Int_u/Cu
    }
    return -1;
  }
  // The identity value of an exception constructor: a local exception's binder, a
  // predefined exception's Stdlib field, else null (unresolved).
  LamPtr exn_value(const std::string& name) {
    if (auto ei = exn_ident_.find(name); ei != exn_ident_.end()) {
      auto v = mk(Lam::K::Var); v->var = ei->second; return v;
    }
    // a submodule's exception/extension ctor: its binder is out of scope
    // outside the module, so read the module's export field
    if (auto ef = exn_field_.find(name); ef != exn_field_.end())
      return fieldimm(ef->second.second, varof(ef->second.first));
    // an opened stdlib module's exception (`open Effect; ... with Unhandled e`):
    // its identity is the module's export field, shadowing the pervasives
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
      if (it->find('.') != std::string::npos) continue;  // dotted: submodule opens
      if (module_base(*it)) continue;  // local module exns register in exn_field_
      auto& fm = fields_of(*it);
      if (auto f = fm.find(name); f != fm.end())
        return field_of(global_of(*it), f->second);
    }
    // A predefined exception's identity is the predef global -- checked BEFORE the
    // stdlib_fields path so `exception Match_failure = Match_failure` while
    // compiling stdlib.ml ITSELF doesn't resolve to `(field i Stdlib)` (a
    // self-reference); the predef global is the same value Stdlib re-exports.
    if (is_predef_exn_name(name)) return predef_global(name);
    if (auto sf = stdlib_fields.find(name); sf != stdlib_fields.end())
      return field_of("Stdlib", sf->second);
    return nullptr;
  }
  // A bare exception/extension ctor brought into scope by `open M` (M a global
  // module whose export field carries the identity): `open A; raise E0` ->
  // `field_of(global A, off)`.  Mirrors exn_value's opened-module branch but
  // WITHOUT the predef/stdlib-field fallbacks, which have dedicated branches at
  // the construct site.  Null if no opened module exports `name`.
  LamPtr opened_module_exn_value(const std::string& name) {
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
      if (it->find('.') != std::string::npos) continue;  // dotted submodule opens
      if (module_base(*it)) continue;                    // local module -> exn_field_
      auto& fm = fields_of(*it);
      if (auto f = fm.find(name); f != fm.end())
        return field_of(global_of(*it), f->second);
    }
    return nullptr;
  }
  // The arity of an exception/extension ctor exported by an opened, separately-
  // compiled module (Unix's `Unix_error of error * string * string` -> 3), read
  // from that module's cmi typexts.  Without it exn_case_body defaults to arity 1
  // and treats the whole payload tuple as one refutable field, dropping the arm.
  // Returns -1 when no opened module exports `name` as a tuple exception.
  int opened_exn_arity(const std::string& name) {
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
      if (it->find('.') != std::string::npos) continue;
      try {
        auto cmi = cmi::CmiFile::load(resolve_cmi(*it));
        for (auto& tx : cmi.sig().typexts)
          if (tx.name == name && !tx.is_inline_record) return (int)tx.args.size();
      } catch (...) {}
    }
    return -1;
  }
  // Register a bare constructor (`EINTR`) brought into scope by `open M` of a
  // separately-compiled module, by finding which opened module's cmi declares it
  // and registering its whole variant type's ctors.  Lets an exn payload pattern
  // `Unix_error (EINTR, _, _)` test EINTR's tag instead of bailing.
  void register_opened_ctor(const std::string& name) {
    if (ctor_info_.count(name)) return;
    for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
      if (it->find('.') != std::string::npos) continue;
      register_module_ctor(*it, name);
      if (ctor_info_.count(name)) return;
    }
  }
  // A match row as a *borrowed* view into the AST (the Structure outlives the
  // translation), so sub-matches can be built from inner sub-patterns without
  // copying the move-only Case.  guard==nullptr means no `when`.
  struct Row { const Pattern* lhs; const Expression* rhs; const Expression* guard; };
  // Compile a `try ... with` handler body: an if-chain testing the caught
  // exception `exn` against each case, falling through to (reraise exn).
  LamPtr exn_dispatch(const Ident& exn, const std::vector<Row>& rows, size_t i) {
    if (i >= rows.size()) {
      auto rr = mk(Lam::K::Prim); rr->prim = Prim::Reraise;
      auto v = mk(Lam::K::Var); v->var = exn; rr->args = {v}; return rr;
    }
    const Row& c = rows[i];
    const Pattern* lhsp = effective_pat(c.lhs);
    // `with A | B -> body`: expand the or-pattern into consecutive rows sharing
    // the body (each alternative binds its own vars) -- previously the row was
    // silently dropped to a reraise.
    if (std::holds_alternative<Ppat_or>(lhsp->desc)) {
      std::vector<const Pattern*> alts;
      flatten_or(lhsp, alts);
      std::vector<Row> expanded;
      for (auto* a : alts) expanded.push_back({a, c.rhs, c.guard});
      for (size_t j = i + 1; j < rows.size(); ++j) expanded.push_back(rows[j]);
      return exn_dispatch(exn, expanded, 0);
    }
    {
      // `E p as x`: x is the exception value itself; peel the alias and bind
      // it over the row's body (was silently dropped to a reraise).
      std::vector<std::string> row_aliases;
      while (auto* pa = std::get_if<Ppat_alias>(&lhsp->desc)) {
        row_aliases.push_back(pa->name.txt);
        lhsp = effective_pat(pa->p.get());
      }
      if (is_catchall(*lhsp)) {  // `_`/var: handle (guard permitting)
        if (auto* pv = std::get_if<Ppat_var>(&lhsp->desc)) scope.back()[pv->name.txt] = exn;
        for (auto& nm : row_aliases) scope.back()[nm] = exn;
        if (!c.guard) return expr(*c.rhs);
        // `with x when g -> body`: test the guard, else fall to the rest.
        auto iff = mk(Lam::K::IfThenElse);
        iff->cond = expr(*c.guard); iff->then_ = expr(*c.rhs);
        iff->else_ = exn_dispatch(exn, rows, i + 1);
        return iff;
      }
      if (auto* k = std::get_if<Ppat_construct>(&lhsp->desc)) {
        // An exn ctor from `open Unix` (`Unix_error of error*string*string`):
        // register its true arity so exn_case_body destructures the payload
        // rather than treating it as one tuple field and dropping the arm.
        if (k->arg) {
          std::string cn = lid_last(k->id.txt);
          if (!exn_arity_.count(cn))
            if (int a = opened_exn_arity(cn); a > 0) exn_arity_[cn] = a;
        }
        // a module-qualified exception/extension ctor resolves to that module's
        // own field first (distinguishes `M1.E` from `M2.E`); else the binder.
        LamPtr id0 = module_ctor_identity(k->id.txt);
        if (!id0) id0 = exn_value(lid_last(k->id.txt));
        // A stdlib module's exception (`Lazy.Undefined`): its identity is the
        // module's runtime export field.  A LOCAL module's (incl. one spliced
        // in by `include Stack` -- `with S.Empty ->`): its layout field.
        if (!id0)
          if (auto* d = std::get_if<Ldot>(&k->id.txt.v))
            if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
              if (LamPtr base = module_base(pl->name)) {
                auto& lay = module_layout_[pl->name];
                if (auto f = lay.find(d->name); f != lay.end())
                  id0 = fieldimm(f->second, base);
              }
              if (!id0) {
                auto& fm = fields_of(pl->name);
                if (auto f = fm.find(d->name); f != fm.end()) {
                  // The module's runtime global: a stdlib submodule is
                  // `Stdlib__M`, but a separately-compiled unit (e.g. the
                  // compiler's own `Syntaxerr`) is just `M`.
                  id0 = field_of(global_of(pl->name), f->second);
                }
              }
            }
        if (LamPtr id = id0) {
          auto exv = [&] { auto v = mk(Lam::K::Var); v->var = exn; return v; };
          LamPtr lhs;
          if (k->arg) {  // exn carries data: compare its identity field
            lhs = mk(Lam::K::Prim); lhs->prim = Prim::FieldImm; lhs->prim_arg = 0;
            lhs->args = {exv()};
          } else {
            lhs = exv();
          }
          std::vector<PayloadTest> ptests;
          // A `when` guard (and any payload identity test) needs the dispatch's
          // remainder at several failure points, so share it behind a catch/exit
          // and pass the exit as the guard's `else`.
          int eid = c.guard ? ++next_exit_ : 0;
          auto exitL = [&] {
            auto x = mk(Lam::K::Staticraise); x->prim_arg = eid; return x;
          };
          LamPtr then = exn_case_body(exn, k, lid_last(k->id.txt), *c.rhs, &ptests,
                                      &row_aliases, c.guard,
                                      c.guard ? exitL() : nullptr);
          if (!then) return exn_dispatch(exn, rows, i + 1);  // unsupported binder shape
          auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
          test->args = {lhs, id};
          if (!ptests.empty() || c.guard) {
            // payload identity tests / a guard: the rest of the dispatch is
            // needed at each failure point, so share it behind a catch/exit
            if (!eid) eid = ++next_exit_;
            for (auto it = ptests.rbegin(); it != ptests.rend(); ++it) {
              auto t = mk(Lam::K::Prim);
              if (it->string_eq) { t->prim = Prim::Ccall; t->prim_id = "caml_string_equal"; }
              else { t->prim = Prim::IntCmp; t->prim_id = "=="; }
              t->args = {fieldimm(it->idx, exv()), it->rhs};
              auto pf = mk(Lam::K::IfThenElse);
              pf->cond = t; pf->then_ = then; pf->else_ = exitL();
              then = pf;
            }
            auto iff = mk(Lam::K::IfThenElse);
            iff->cond = test; iff->then_ = then; iff->else_ = exitL();
            auto cat = mk(Lam::K::Catch);
            cat->cond = iff; cat->prim_arg = eid;
            cat->then_ = exn_dispatch(exn, rows, i + 1);
            return cat;
          }
          auto iff = mk(Lam::K::IfThenElse);
          iff->cond = test; iff->then_ = then;
          iff->else_ = exn_dispatch(exn, rows, i + 1);
          return iff;
        }
      }
    }
    return exn_dispatch(exn, rows, i + 1);  // unsupported case: skip
  }
  // The body of a matched exception case: the constructor's data lives at fields
  // 1..arity of the exception block (field 0 is its identity).  A structured
  // sub-pattern (the string*int*int tuple of Assert_failure etc.) reads through a
  // `*match*` temp like ocamlc's matcher; simple binders inline as field reads.
  // A constant extension-ctor sub-pattern (`with Unhandled E ->`) adds a
  // field-identity test to `tests` (field index, identity value) when the caller
  // provides it.  Null when a sub-pattern is otherwise refutable (a constant
  // pattern etc.) -- the case is then skipped, preserving the previous behavior.
  // Inline-record labels of an exception/extension ctor (decl order), for a
  // `{l1; l2}` pattern: from the local map, else navigated from the qualifier's
  // cmi typexts (`Includemod.Apply_error`).  Empty when not an inline-record exn.
  std::vector<std::string> exn_inline_labels(const std::string& name, const Longident* lid) {
    if (auto it = exn_rlabels_.find(name); it != exn_rlabels_.end()) return it->second;
    if (lid) if (auto* d = std::get_if<Ldot>(&lid->v)) {
      std::string dotted;
      if (lid_to_dotted(*d->prefix, dotted)) {
        try {
          size_t dot = dotted.find('.');
          auto cmi = cmi::CmiFile::load(resolve_cmi(dot == std::string::npos
                                                    ? dotted : dotted.substr(0, dot)));
          const cmi::Signature* sig = &cmi.sig();
          for (size_t pos = dot; pos != std::string::npos;) {
            size_t nd = dotted.find('.', pos + 1);
            std::string comp = dotted.substr(pos + 1, nd == std::string::npos
                                                      ? std::string::npos : nd - pos - 1);
            const cmi::Signature* next = nullptr;
            for (auto& md : sig->modules)
              if (md.name == comp) { next = mt_sig(cmi, md.type); break; }
            if (!next) return {};
            sig = next; pos = nd;
          }
          for (auto& tx : sig->typexts)
            if (tx.name == name && tx.is_inline_record) {
              std::vector<std::string> r;
              for (auto& l : tx.inline_record) r.push_back(l.name);
              return r;
            }
        } catch (...) {}
      }
    }
    return {};
  }
  struct PayloadTest { int idx; LamPtr rhs; bool string_eq; };
  LamPtr exn_case_body(const Ident& exn, const Ppat_construct* k, const std::string& name,
                       const Expression& rhs,
                       std::vector<PayloadTest>* tests = nullptr,
                       const std::vector<std::string>* aliases = nullptr,
                       const Expression* guard = nullptr, LamPtr guard_else = nullptr) {
    // Compile `rhs`, or -- when the case carries a `when` guard -- the
    // `if guard then rhs else guard_else` test, with both evaluated in the
    // scope that holds the payload binders.
    auto with_guard = [&]() -> LamPtr {
      if (!guard) return expr(rhs);
      LamPtr g = expr(*guard);
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = g; iff->then_ = expr(rhs); iff->else_ = guard_else;
      return iff;
    };
    if (!k->arg) {
      if ((!aliases || aliases->empty()) && !guard) return expr(rhs);
      scope.emplace_back();
      if (aliases) for (auto& nm : *aliases) scope.back()[nm] = exn;
      LamPtr b = with_guard();
      scope.pop_back();
      return b;
    }
    // Inline-record exn payload `Apply_error {loc; env; ..}`: the fields are FLAT
    // in the exn block at offsets 1.. , bound per the LABEL's decl index (the
    // pattern may omit/reorder labels), not positionally.
    if (auto* pr = std::get_if<Ppat_record>(&effective_pat(k->arg->get())->desc)) {
      auto labels = exn_inline_labels(name, &k->id.txt);
      if (!labels.empty()) {
        auto exv = [&] { auto v = mk(Lam::K::Var); v->var = exn; return v; };
        scope.emplace_back();
        if (aliases) for (auto& nm : *aliases) scope.back()[nm] = exn;
        std::vector<std::pair<Ident, LamPtr>> binders;
        std::vector<std::pair<Ident, LamPtr>> temps;
        std::vector<std::vector<std::pair<Ident, LamPtr>>> sub_binders;
        bool ok = true;
        for (auto& [lbl, sub] : pr->fields) {
          int idx = -1;
          for (int i = 0; i < (int)labels.size(); ++i)
            if (labels[i] == lid_last(lbl.txt)) { idx = i; break; }
          if (idx < 0) { ok = false; break; }
          const Pattern* fp = effective_pat(sub.get());
          LamPtr acc = fieldimm(idx + 1, exv());
          if (std::holds_alternative<Ppat_any>(fp->desc)) continue;
          if (std::holds_alternative<Ppat_var>(fp->desc) ||
              std::holds_alternative<Ppat_alias>(fp->desc))
            ok = collect_binders(*fp, acc, binders);
          else if (is_irrefutable(*fp)) {
            Ident tv = fresh("", true); auto tvv = mk(Lam::K::Var); tvv->var = tv;
            std::vector<std::pair<Ident, LamPtr>> sb;
            ok = collect_binders(*fp, tvv, sb);
            temps.push_back({tv, acc}); sub_binders.push_back(std::move(sb));
          } else ok = false;
          if (!ok) break;
        }
        if (!ok) { scope.pop_back(); return nullptr; }
        LamPtr body = with_guard();
        scope.pop_back();
        for (auto& sb : sub_binders) body = wrap_binders(body, sb);
        body = wrap_binders(body, binders);
        body = wrap_binders(body, temps);
        return body;
      }
    }
    int arity = 1;
    if (auto a = exn_arity_.find(name); a != exn_arity_.end()) arity = a->second;
    auto fps = ctor_field_pats(k, arity);
    if ((int)fps.size() != arity) return nullptr;
    auto exv = [&] { auto v = mk(Lam::K::Var); v->var = exn; return v; };
    scope.emplace_back();
    if (aliases) for (auto& nm : *aliases) scope.back()[nm] = exn;
    std::vector<std::pair<Ident, LamPtr>> binders;  // simple field binders
    std::vector<std::pair<Ident, LamPtr>> temps;    // *match* temps for structured sub-pats
    std::vector<std::vector<std::pair<Ident, LamPtr>>> sub_binders;
    bool ok = true;
    for (int j = 0; ok && j < arity; ++j) {
      const Pattern* fp = effective_pat(fps[j]);
      LamPtr acc = fieldimm(j + 1, exv());
      if (std::holds_alternative<Ppat_any>(fp->desc)) continue;
      if (auto* kc = std::get_if<Ppat_construct>(&fp->desc);
          kc && !kc->arg && tests &&
          (exn_ident_.count(lid_last(kc->id.txt)) ||
           exn_field_.count(lid_last(kc->id.txt)))) {
        // constant extension/exception ctor payload: physical-identity test
        if (LamPtr idv = exn_value(lid_last(kc->id.txt))) {
          tests->push_back({j + 1, idv, false});
          continue;
        }
        ok = false;
      } else if (auto* kc = std::get_if<Ppat_construct>(&fp->desc);
                 kc && !kc->arg && tests) {
        // a nullary VARIANT constructor payload (`Error (Unterminated_string, x)`,
        // lexer.ml): an integer test on the field against its constant-ctor index.
        // Without this the whole `with`-arm was dropped to a bare reraise.
        // A ctor from `open Unix` (EINTR in `Unix_error (EINTR,_,_)`) is not yet
        // in ctor_info_ -- register its type from the opened module first.
        if (std::holds_alternative<Lident>(kc->id.txt.v))
          register_opened_ctor(ctor_of(*fp));
        auto ci = ctor_info_.find(ctor_of(*fp));
        if (ci != ctor_info_.end() && !ci->second.is_block) {
          tests->push_back({j + 1, cint(ci->second.tag), false});
          continue;
        }
        ok = false;
      } else if (auto* pc = std::get_if<Ppat_constant>(&fp->desc); pc && tests) {
        // constant payload (`Ex "!!!!!"`, `Code 42`): value test on the field
        if (auto* ps = std::get_if<Pconst_string>(&pc->c.desc)) {
          auto sv = mk(Lam::K::ConstString); sv->str_val = ps->s;
          tests->push_back({j + 1, sv, true});
          continue;
        }
        if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc); pi && !pi->suffix) {
          tests->push_back({j + 1, cint(parse_ocaml_int(pi->value)), false});
          continue;
        }
        if (auto* pch = std::get_if<Pconst_char>(&pc->c.desc)) {
          tests->push_back({j + 1, cchar((unsigned char)pch->code), false});
          continue;
        }
        ok = false;
      } else if (std::holds_alternative<Ppat_var>(fp->desc) ||
          std::holds_alternative<Ppat_alias>(fp->desc)) {
        ok = collect_binders(*fp, acc, binders);
      } else if (is_irrefutable(*fp)) {
        Ident tv = fresh("", true);
        auto tvv = mk(Lam::K::Var); tvv->var = tv;
        std::vector<std::pair<Ident, LamPtr>> sub;
        ok = collect_binders(*fp, tvv, sub);
        temps.push_back({tv, acc});
        sub_binders.push_back(std::move(sub));
      } else ok = false;
    }
    if (!ok) { scope.pop_back(); return nullptr; }
    LamPtr body = with_guard();
    scope.pop_back();
    for (auto& sb : sub_binders) body = wrap_binders(body, sb);
    body = wrap_binders(body, binders);
    body = wrap_binders(body, temps);
    return body;
  }
  // The effect-handler match syntax: lower the whole match to
  //   (runstack (caml_alloc_stack <value fn> <exn fn> <effect fn>)
  //             (function param <scrut>) 0)
  // The effect fn `(function eff k ...)` dispatches on the extension-ctor
  // identity (field 0 of an applied effect, the value for a constant one),
  // binding the data fields like an exn handler and the continuation pattern
  // to k; the fall-through is `(reperform eff k)` (always tail position).
  LamPtr effect_match(const Expression& scrut, const std::vector<Row>& vrows,
                      const std::vector<Row>& erows,
                      const std::vector<std::pair<const Ppat_effect*, const Expression*>>& frows,
                      const Location& mloc) {
    Ident eff = fresh("eff"), kid = fresh("k");
    auto effv = [&] { return varof(eff); };
    std::function<LamPtr(size_t)> dispatch = [&](size_t i) -> LamPtr {
      if (i == frows.size()) {
        auto rp = mk(Lam::K::Prim); rp->prim = Prim::Ccall; rp->prim_id = "reperform";
        rp->args = {effv(), varof(kid)};
        return rp;
      }
      auto [pf, rhs] = frows[i];
      const Pattern* p = effective_pat(pf->eff.get());
      auto* k = std::get_if<Ppat_construct>(&p->desc);
      if (!k) return nullptr;
      LamPtr id = exn_value(lid_last(k->id.txt));
      if (!id) return nullptr;
      scope.emplace_back();
      if (auto* kv = std::get_if<Ppat_var>(&pf->cont->desc))
        scope.back()[kv->name.txt] = kid;
      LamPtr then = exn_case_body(eff, k, lid_last(k->id.txt), *rhs);
      scope.pop_back();
      if (!then) return nullptr;
      LamPtr rest = dispatch(i + 1);
      if (!rest) return nullptr;
      auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
      test->args = {k->arg ? fieldimm(0, effv()) : effv(), id};
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = test; iff->then_ = then; iff->else_ = rest;
      return iff;
    };
    LamPtr dis = dispatch(0);
    if (!dis) return nullptr;
    auto ffn = mk(Lam::K::Function);
    ffn->params = {{eff, ValueKind::Gen}, {kid, ValueKind::Gen}};
    ffn->body = dis;
    // value continuation: named after the first var/alias value row
    std::string vn = "param";
    for (auto& r : vrows) {
      const Pattern* ep = effective_pat(r.lhs);
      if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) { vn = pv->name.txt; break; }
      if (auto* pa = std::get_if<Ppat_alias>(&ep->desc)) { vn = pa->name.txt; break; }
    }
    Ident v = fresh(vn);
    auto vfn = mk(Lam::K::Function);
    vfn->params = {{v, ValueKind::Gen}};
    scope.emplace_back();
    vfn->body = compile_match(varof(v), vrows, mloc);
    scope.pop_back();
    // exception continuation: reraise, or the exception arms' dispatch
    Ident exn = fresh("exn");
    auto efn = mk(Lam::K::Function);
    efn->params = {{exn, ValueKind::Gen}};
    scope.emplace_back();
    caught_exn_.push_back(exn);
    if (erows.empty()) {
      auto rr = mk(Lam::K::Prim); rr->prim = Prim::Reraise; rr->args = {varof(exn)};
      efn->body = rr;
    } else {
      efn->body = exn_dispatch(exn, erows, 0);
    }
    caught_exn_.pop_back();
    scope.pop_back();
    auto al = mk(Lam::K::Prim); al->prim = Prim::Ccall; al->prim_id = "caml_alloc_stack";
    al->args = {vfn, efn, ffn};
    auto rs = mk(Lam::K::Prim); rs->prim = Prim::Ccall; rs->prim_id = "runstack";
    // runstack runs `f x` on the new stack.  When the matched computation is a
    // single-argument application of a plain function (`match f x with effect..`),
    // ocamlc passes f and x to runstack directly rather than the thunk
    // `(fun () -> f x) 0`.
    if (auto* ap = std::get_if<Pexp_apply>(&scrut.desc))
      if (ap->args.size() == 1 && std::holds_alternative<Nolabel>(ap->args[0].first) &&
          std::holds_alternative<Pexp_ident>(ap->fn->desc)) {
        rs->args = {al, expr(*ap->fn), expr(*ap->args[0].second)};
        return rs;
      }
    Ident pp = fresh("param");
    auto th = mk(Lam::K::Function);
    th->params = {{pp, ValueKind::Gen}};
    th->body = expr(scrut);
    rs->args = {al, th, cint(0)};
    return rs;
  }
  static bool is_catchall(const Pattern& p) {
    return std::holds_alternative<Ppat_any>(p.desc) || std::holds_alternative<Ppat_var>(p.desc);
  }
  static std::string ctor_of(const Pattern& p) {
    if (auto* k = std::get_if<Ppat_construct>(&p.desc)) return lid_last(k->id.txt);
    return "";
  }
  // Compile a match into an if-chain (int constants + catch-all) or a bool if;
  // other forms (variant tags, nested patterns, guards) fall back best-effort.
  // Count / substitute occurrences of a binder in a (freshly-built) Lambda tree.
  // A use under a lambda weighs 2 (simplif's use_var on a var not locally bound:
  // enough that single-use inlining does not apply -- the closure may run often).
  static int count_var(const LamPtr& l, const Ident& id, int w = 1) {
    if (!l) return 0;
    if (l->k == Lam::K::Var)
      return (l->var.stamp == id.stamp && l->var.name == id.name) ? w : 0;
    int wb = l->k == Lam::K::Function ? 2 : w;
    int c = count_var(l->fn, id, w) + count_var(l->body, id, wb) +
            count_var(l->cond, id, w) + count_var(l->then_, id, w) +
            count_var(l->else_, id, w) + count_var(l->sw_default, id, w);
    for (auto& a : l->args) c += count_var(a, id, w);
    for (auto& b : l->bindings) c += count_var(b.val, id, w);
    for (auto& sc : l->sw_consts) c += count_var(sc.body, id, w);
    for (auto& sc : l->sw_blocks) c += count_var(sc.body, id, w);
    return c;
  }
  static void subst_var(LamPtr& l, const Ident& id, const LamPtr& repl) {
    if (!l) return;
    if (l->k == Lam::K::Var && l->var.stamp == id.stamp && l->var.name == id.name) { l = repl; return; }
    subst_var(l->fn, id, repl); subst_var(l->body, id, repl); subst_var(l->cond, id, repl);
    subst_var(l->then_, id, repl); subst_var(l->else_, id, repl); subst_var(l->sw_default, id, repl);
    for (auto& a : l->args) subst_var(a, id, repl);
    for (auto& b : l->bindings) subst_var(b.val, id, repl);
    for (auto& sc : l->sw_consts) subst_var(sc.body, id, repl);
    for (auto& sc : l->sw_blocks) subst_var(sc.body, id, repl);
  }
  LamPtr fieldimm(int i, const LamPtr& s) {
    auto f = mk(Lam::K::Prim); f->prim = Prim::FieldImm; f->prim_arg = i; f->args = {s}; return f;
  }

  // ===== Simplif.simplify_exits (lambda/simplif.ml) port =====
  // Count Lstaticraise of exit `id`; `bad` accumulates those nested under an
  // inner try..with (which can't be safely inlined).  Static exits never cross a
  // function boundary, so a nested Lfunction contributes nothing.
  static int count_exit(const LamPtr& l, int id, bool under_try, int& bad) {
    if (!l) return 0;
    if (l->k == Lam::K::Staticraise && l->prim_arg == id) {
      if (under_try) ++bad;
      return 1;
    }
    if (l->k == Lam::K::Function) return 0;
    if (l->k == Lam::K::Try)  // protected body is under_try; handler is not
      return count_exit(l->body, id, true, bad) +
             count_exit(l->then_, id, under_try, bad);
    int c = count_exit(l->fn, id, under_try, bad) +
            count_exit(l->body, id, under_try, bad) +
            count_exit(l->cond, id, under_try, bad) +
            count_exit(l->then_, id, under_try, bad) +
            count_exit(l->else_, id, under_try, bad) +
            count_exit(l->sw_default, id, under_try, bad);
    for (auto& a : l->args) c += count_exit(a, id, under_try, bad);
    for (auto& b : l->bindings) c += count_exit(b.val, id, under_try, bad);
    for (auto& sc : l->sw_consts) c += count_exit(sc.body, id, under_try, bad);
    for (auto& sc : l->sw_blocks) c += count_exit(sc.body, id, under_try, bad);
    return c;
  }
  // Replace the single `(exit id args)` with `let vars = args in handler` (Strict
  // lets, last var outermost -- matching simplif's fold_left2).
  void inline_exit(LamPtr& l, int id, const std::vector<Ident>& vars,
                   const std::vector<ValueKind>& kinds, const LamPtr& handler) {
    if (!l) return;
    if (l->k == Lam::K::Staticraise && l->prim_arg == id) {
      LamPtr res = handler;
      if (l->args.size() == vars.size())
        for (size_t i = 0; i < vars.size(); ++i) {
          auto let = mk(Lam::K::Let);
          ValueKind k = i < kinds.size() ? kinds[i] : ValueKind::Gen;
          let->bindings = {{vars[i], k, l->args[i]}};  // Strict
          let->body = res; res = let;
        }
      l = res; return;
    }
    if (l->k == Lam::K::Function) return;  // exits don't cross functions
    inline_exit(l->fn, id, vars, kinds, handler);
    inline_exit(l->body, id, vars, kinds, handler);
    inline_exit(l->cond, id, vars, kinds, handler);
    inline_exit(l->then_, id, vars, kinds, handler);
    inline_exit(l->else_, id, vars, kinds, handler);
    inline_exit(l->sw_default, id, vars, kinds, handler);
    for (auto& a : l->args) inline_exit(a, id, vars, kinds, handler);
    for (auto& b : l->bindings) inline_exit(b.val, id, vars, kinds, handler);
    for (auto& sc : l->sw_consts) inline_exit(sc.body, id, vars, kinds, handler);
    for (auto& sc : l->sw_blocks) inline_exit(sc.body, id, vars, kinds, handler);
  }
  // Simplif drops `let v = (Lvar w)` for ANY let-kind, substituting v by w.  The
  // construction-time pass only catches source lets; compiler-generated temps
  // (an apply's function bound to a temp, a renamed parameter) need a tree-wide
  // pass.  Runs after rec compilation, so it can't perturb letrec strategy.
  // Mutable-local bindings (`=mut`) are Lmutlet, never inlined -- skip them.
  void inline_var_aliases(LamPtr& l) {
    if (!l) return;
    inline_var_aliases(l->fn);
    inline_var_aliases(l->body);
    inline_var_aliases(l->cond);
    inline_var_aliases(l->then_);
    inline_var_aliases(l->else_);
    inline_var_aliases(l->sw_default);
    for (auto& a : l->args) inline_var_aliases(a);
    for (auto& b : l->bindings) inline_var_aliases(b.val);
    for (auto& sc : l->sw_consts) inline_var_aliases(sc.body);
    for (auto& sc : l->sw_blocks) inline_var_aliases(sc.body);
    if (l->k != Lam::K::Let) return;
    std::vector<Lam::Binding> keep;
    for (size_t i = 0; i < l->bindings.size(); ++i) {
      auto& b = l->bindings[i];
      if (b.val && b.val->k == Lam::K::Var && !b.mut) {
        // substitute v -> w in the later (sequential) bindings and the body
        for (size_t j = i + 1; j < l->bindings.size(); ++j)
          subst_var(l->bindings[j].val, b.id, b.val);
        subst_var(l->body, b.id, b.val);
      } else {
        keep.push_back(std::move(b));
      }
    }
    if (keep.empty()) { l = l->body; return; }
    l->bindings = std::move(keep);
  }
  // Collapse a static-catch whose exit is raised 0 times (drop the handler) or
  // exactly once and not under an inner try (inline the handler at that site).
  void simplify_static_catches(LamPtr& l) {
    if (!l) return;
    simplify_static_catches(l->fn);
    simplify_static_catches(l->body);
    simplify_static_catches(l->cond);
    simplify_static_catches(l->then_);
    simplify_static_catches(l->else_);
    simplify_static_catches(l->sw_default);
    for (auto& a : l->args) simplify_static_catches(a);
    for (auto& b : l->bindings) simplify_static_catches(b.val);
    for (auto& sc : l->sw_consts) simplify_static_catches(sc.body);
    for (auto& sc : l->sw_blocks) simplify_static_catches(sc.body);
    if (l->k != Lam::K::Catch) return;
    int bad = 0;
    int n = count_exit(l->cond, l->prim_arg, false, bad);
    if (n == 0) { l = l->cond; return; }  // exit never raised -> drop handler
    if (n == 1 && bad == 0) {
      LamPtr body = l->cond;
      inline_exit(body, l->prim_arg, l->catch_vars, l->catch_var_kinds, l->then_);
      l = body;
    }
  }
  // ----- mutable-local `ref` optimization ------------------------------------
  // Whether `rid` (a `ref`'s binder) is used anywhere but as `!r` / `r := e` /
  // `incr r` / `decr r` in an already-translated body -- i.e. it ESCAPES (is taken
  // as a first-class value), so it must stay a heap ref.  A bare Var(rid) reached
  // without a consuming ref-operation parent counts as an escape.
  static bool ref_op0(const LamPtr& l, const Ident& rid) {
    return !l->args.empty() && l->args[0]->k == Lam::K::Var &&
           l->args[0]->var.stamp == rid.stamp;
  }
  static bool ref_escapes(const LamPtr& l, const Ident& rid) {
    if (!l) return false;
    if (l->k == Lam::K::Var) return l->var.stamp == rid.stamp;
    // Any use inside a nested closure forces a heap ref (the closure may outlive
    // the binding's stack frame), even a mere `!r`.
    if (l->k == Lam::K::Function) return count_var(l->body, rid) > 0;
    if (l->k == Lam::K::Prim && ref_op0(l, rid)) {
      if (l->prim_arg == 0 && (l->prim == Prim::FieldInt || l->prim == Prim::FieldMut))
        return false;                                              // !r
      if (l->prim_arg == 0 && (l->prim == Prim::SetfieldImm || l->prim == Prim::SetfieldPtr))
        return ref_escapes(l->args[1], rid);                       // r := e -> check e
      if (l->prim == Prim::Offsetref) return false;                // incr/decr
    }
    if (ref_escapes(l->fn, rid) || ref_escapes(l->body, rid) ||
        ref_escapes(l->cond, rid) || ref_escapes(l->then_, rid) ||
        ref_escapes(l->else_, rid) || ref_escapes(l->sw_default, rid)) return true;
    for (auto& a : l->args) if (ref_escapes(a, rid)) return true;
    for (auto& b : l->bindings) if (ref_escapes(b.val, rid)) return true;
    for (auto& sc : l->sw_consts) if (ref_escapes(sc.body, rid)) return true;
    for (auto& sc : l->sw_blocks) if (ref_escapes(sc.body, rid)) return true;
    return false;
  }
  // Rewrite the (non-escaping) ref operations on `rid` in place to mutable-local
  // form: `!r` -> `*r` (Mutvar), `r := e` -> `(assign r e)`, `incr/decr r` ->
  // `(assign r (n+ *r))`.
  void ref_rewrite(LamPtr& l, const Ident& rid) {
    if (!l) return;
    if (l->k == Lam::K::Prim && ref_op0(l, rid)) {
      if (l->prim_arg == 0 && (l->prim == Prim::FieldInt || l->prim == Prim::FieldMut)) {
        l->k = Lam::K::Mutvar; l->var = rid; l->args.clear(); return;
      }
      if (l->prim_arg == 0 && (l->prim == Prim::SetfieldImm || l->prim == Prim::SetfieldPtr)) {
        ref_rewrite(l->args[1], rid);
        l->k = Lam::K::Assign; l->var = rid; l->cond = l->args[1]; l->args.clear(); return;
      }
      if (l->prim == Prim::Offsetref) {
        auto mv = mk(Lam::K::Mutvar); mv->var = rid;
        auto oi = mk(Lam::K::Prim); oi->prim = Prim::Offsetint;
        oi->prim_arg = l->prim_arg; oi->args = {mv};
        l->k = Lam::K::Assign; l->var = rid; l->cond = oi; l->args.clear(); return;
      }
    }
    ref_rewrite(l->fn, rid); ref_rewrite(l->body, rid); ref_rewrite(l->cond, rid);
    ref_rewrite(l->then_, rid); ref_rewrite(l->else_, rid); ref_rewrite(l->sw_default, rid);
    for (auto& a : l->args) ref_rewrite(a, rid);
    for (auto& b : l->bindings) ref_rewrite(b.val, rid);
    for (auto& sc : l->sw_consts) ref_rewrite(sc.body, rid);
    for (auto& sc : l->sw_blocks) ref_rewrite(sc.body, rid);
  }
  // `ref E` as a call -> the content expression E (else null).
  static const Expression* ref_call(const Expression& e) {
    auto* ap = std::get_if<Pexp_apply>(&e.desc);
    if (!ap || ap->args.size() != 1) return nullptr;
    auto* id = std::get_if<Pexp_ident>(&ap->fn->desc);
    if (id && lid_last(id->id.txt) == "ref") return ap->args[0].second.get();
    return nullptr;
  }
  // Read record field `fi` of `s` with the spelling its kind implies (an int field
  // is field_int, a mutable boxed field field_mut, otherwise field_imm).
  // Rewrite every TAIL value of `l` that is a k-tuple construction into
  // (exit N <components>); bottoms (raise / existing exits) stay.  Returns
  // false when any tail produces the tuple un-decomposed (a variable, a call)
  // -- the caller then keeps the allocating form.  This is how
  // `let (a, b) = match .. with .. -> e1, e2` avoids building the pair
  // (Matching's exit-with-args form).
  bool tail_tuple_exit(LamPtr& l, int n, size_t k) {
    if (!l) return false;
    switch (l->k) {
      case Lam::K::Prim:
        if (l->prim == Prim::Makeblock && l->prim_arg == 0 && l->args.size() == k) {
          l->k = Lam::K::Staticraise;
          l->prim_arg = n;
          l->blk_shape.clear();
          return true;
        }
        if (l->prim == Prim::Raise || l->prim == Prim::Reraise) return true;
        return false;
      case Lam::K::ConstBlock:
        if (l->prim_arg == 0 && l->args.size() == k) {
          l->k = Lam::K::Staticraise;
          l->prim_arg = n;
          return true;
        }
        return false;
      case Lam::K::Staticraise: return true;  // bottom w.r.t. the value
      case Lam::K::Let:
      case Lam::K::Letrec: return tail_tuple_exit(l->body, n, k);
      case Lam::K::Sequence: return tail_tuple_exit(l->else_, n, k);
      case Lam::K::IfThenElse:
        return tail_tuple_exit(l->then_, n, k) && tail_tuple_exit(l->else_, n, k);
      case Lam::K::Switch: {
        for (auto& c : l->sw_consts) if (!tail_tuple_exit(c.body, n, k)) return false;
        for (auto& c : l->sw_blocks) if (!tail_tuple_exit(c.body, n, k)) return false;
        if (l->sw_default) return tail_tuple_exit(l->sw_default, n, k);
        return true;
      }
      case Lam::K::Catch:
        return tail_tuple_exit(l->cond, n, k) && tail_tuple_exit(l->then_, n, k);
      case Lam::K::Try:
        return tail_tuple_exit(l->body, n, k) && tail_tuple_exit(l->then_, n, k);
      default: return false;
    }
  }

  // ===== multi-value match: `match e1, e2 with p1, p2 -> ..` ==============
  // The components match column-by-column without ever building the tuple
  // (Matching.for_multiple_match; match-exception/allocation's no-alloc
  // requirement).  Restricted: no guards, every (or-expanded) row a k-tuple
  // of var/_/single-field ctors of a 1-const/1-block type, and no column
  // mixing vars with ctors (default rows need the real matrix).  Or-rows
  // duplicate bodies where ocamlc shares via exits (exec-equivalent).
  struct MRow {
    std::vector<const Pattern*> cols;
    const Expression* rhs;
    std::vector<std::pair<std::string, Ident>> binds;
  };
  LamPtr multi_match(const Pexp_tuple* tu, const std::vector<Row>& vrows,
                     const std::vector<Row>& erows, const Location& mloc) {
    size_t k = tu->elems.size();
    std::vector<MRow> rows;
    for (auto& r : vrows) {
      if (r.guard) return nullptr;
      std::vector<const Pattern*> alts;
      flatten_or(r.lhs, alts);
      for (auto* a : alts) {
        auto* tp = std::get_if<Ppat_tuple>(&a->desc);
        if (!tp || tp->elems.size() != k) return nullptr;
        MRow mr;
        mr.rhs = r.rhs;
        for (auto& el : tp->elems) mr.cols.push_back(effective_pat(el.get()));
        rows.push_back(std::move(mr));
      }
    }
    if (rows.empty()) return nullptr;
    for (size_t c = 0; c < k; ++c) {
      bool anyvar = false, anyctor = false, all_const_type = false, ext_col = false;
      std::string type;
      for (auto& r : rows) {
        std::vector<const Pattern*> alts;  // examine each or-pattern alternative
        flatten_or(r.cols[c], alts);
        for (const Pattern* p : alts) {
          if (std::get_if<Ppat_var>(&p->desc) ||
              std::holds_alternative<Ppat_any>(p->desc)) {
            anyvar = true;
            continue;
          }
          auto* kc = std::get_if<Ppat_construct>(&p->desc);
          if (!kc) return nullptr;
          auto ci = ctor_info_.find(ctor_of(*p));
          if (ci == ctor_info_.end()) {
            // An extensible/exception constructor (`type t += A`): identity-matched,
            // nullary only -- mm_cols builds an `(if (== col id) ..)` chain.
            std::string cn = ctor_of(*p);
            if ((exn_ident_.count(cn) || exn_field_.count(cn)) && !kc->arg) {
              ext_col = true; anyctor = true; continue;
            }
            return nullptr;
          }
          auto tc = type_ctors_.find(ci->second.type);
          if (tc == type_ctors_.end()) return nullptr;
          bool one_one = tc->second.first == 1 && tc->second.second == 1;
          bool allc = tc->second.second == 0;
          if (!one_one && !allc) return nullptr;
          if (type.empty()) { type = ci->second.type; all_const_type = allc; }
          else if (type != ci->second.type) return nullptr;
          if (kc->arg) {
            if (ci->second.arity != 1) return nullptr;
            const Pattern* ap = effective_pat(kc->arg->get());
            if (!std::get_if<Ppat_var>(&ap->desc) &&
                !std::holds_alternative<Ppat_any>(ap->desc))
              return nullptr;
          }
          anyctor = true;
        }
      }
      // A mixed var/constructor column is only handled for an all-constant type
      // or an extensible type (mm_cols routes var rows to every case + default).
      if (anyvar && anyctor && !all_const_type && !ext_col) return nullptr;
    }
    // translate components; non-var ones bind to *match* temps
    std::vector<LamPtr> comps;
    std::vector<Lam::Binding> temps;
    for (auto& el : tu->elems) {
      LamPtr v = expr(*el);
      if (v->k != Lam::K::Var) {
        Ident t = fresh("", true);
        temps.push_back({t, expr_kind(el.get()), v});
        v = varof(t);
      }
      comps.push_back(v);
    }
    LamPtr body;
    if (!erows.empty()) {
      // (catch (try (exit N c1..ck) with exn <dispatch>) with (N v1..vk) <mm>)
      int eid = ++next_exit_;
      auto ex = mk(Lam::K::Staticraise);
      ex->prim_arg = eid;
      ex->args = comps;
      auto tr = mk(Lam::K::Try);
      tr->body = ex;
      tr->var = fresh("exn");
      scope.emplace_back();
      caught_exn_.push_back(tr->var);
      tr->then_ = exn_dispatch(tr->var, erows, 0);
      caught_exn_.pop_back();
      scope.pop_back();
      auto cat = mk(Lam::K::Catch);
      cat->cond = tr;
      cat->prim_arg = eid;
      std::vector<LamPtr> hv;
      for (size_t c = 0; c < k; ++c) {
        Ident v = fresh("val");
        cat->catch_vars.push_back(v);
        hv.push_back(varof(v));
      }
      LamPtr mbody = mm_cols(hv, rows, 0, mloc);
      if (!mbody) return nullptr;
      cat->then_ = mbody;
      body = cat;
    } else {
      body = mm_cols(comps, rows, 0, mloc);
      if (!body) return nullptr;
    }
    if (!temps.empty()) {
      auto l = mk(Lam::K::Let);
      l->bindings = std::move(temps);
      l->body = body;
      body = l;
    }
    return body;
  }
  LamPtr mm_cols(const std::vector<LamPtr>& comps, std::vector<MRow> rows,
                 size_t i, const Location& mloc) {
    if (rows.empty()) return raise_predef("Match_failure", mloc);
    if (i == comps.size()) {
      auto& r = rows[0];
      scope.emplace_back();
      for (auto& [nm, id] : r.binds) scope.back()[nm] = id;
      LamPtr b = expr(*r.rhs);
      scope.pop_back();
      return b;
    }
    // Expand or-patterns in this column into separate rows (preserving order),
    // so `(A | B), C` matches like two rows `A, C` and `B, C`.
    for (auto& r : rows)
      if (std::get_if<Ppat_or>(&r.cols[i]->desc)) {
        std::vector<MRow> ex;
        for (auto& rr : rows) {
          std::vector<const Pattern*> alts;
          flatten_or(rr.cols[i], alts);
          for (auto* a : alts) { MRow nr = rr; nr.cols[i] = a; ex.push_back(std::move(nr)); }
        }
        return mm_cols(comps, std::move(ex), i, mloc);
      }
    bool varcol = true;
    for (auto& r : rows)
      if (std::get_if<Ppat_construct>(&r.cols[i]->desc)) { varcol = false; break; }
    if (varcol) {
      for (auto& r : rows)
        if (auto* pv = std::get_if<Ppat_var>(&r.cols[i]->desc))
          r.binds.push_back({pv->name.txt, comps[i]->var});
      return mm_cols(comps, std::move(rows), i + 1, mloc);
    }
    auto is_ctor = [](const MRow& r, size_t c) {
      return std::get_if<Ppat_construct>(&r.cols[c]->desc) != nullptr; };
    auto bindv = [&](MRow r) -> MRow {
      if (auto* pv = std::get_if<Ppat_var>(&r.cols[i]->desc))
        r.binds.push_back({pv->name.txt, comps[i]->var});
      return r; };
    // An extensible/exception-constructor column (`type t += A | B`): the values
    // are identity-matched, so build an `(if (== col <id>) <rows for A> <rest>)`
    // chain, recursing on the remaining columns; var rows match every arm + rest.
    {
      bool extensible = false;
      for (auto& r : rows)
        if (is_ctor(r, i)) {
          std::string n = ctor_of(*r.cols[i]);
          if (ctor_info_.count(n)) { extensible = false; break; }
          if ((exn_ident_.count(n) || exn_field_.count(n)) &&
              !std::get_if<Ppat_construct>(&r.cols[i]->desc)->arg) extensible = true;
          else { extensible = false; break; }
        }
      if (extensible) {
        // Group by IDENTITY, not name: `exception Bar = Foo` makes Bar and Foo the
        // same value, so a row matching one matches the other (PR#5788).  Key each
        // name by its binder stamp / module-field slot; same key => same branch.
        auto ident_key = [&](const std::string& n) -> std::string {
          if (auto ei = exn_ident_.find(n); ei != exn_ident_.end())
            return "v" + std::to_string(ei->second.stamp);
          if (auto ef = exn_field_.find(n); ef != exn_field_.end())
            return "f" + std::to_string(ef->second.first.stamp) + ":" +
                   std::to_string(ef->second.second);
          return "";
        };
        std::vector<std::string> keys;
        std::unordered_map<std::string, std::string> rep;  // identity key -> a name
        for (auto& r : rows)
          if (is_ctor(r, i)) {
            std::string n = ctor_of(*r.cols[i]), key = ident_key(n);
            if (key.empty()) return nullptr;
            if (!rep.count(key)) { rep[key] = n; keys.push_back(key); }
          }
        // Extension constructors can ALIAS at runtime (`type t += C=A` rebinds C to
        // A's identity), so the grouped identity-key dispatch below is UNSOUND when
        // a later column's sub-match can fail and a different key would then match
        // (x=A failing A's rest must still reach C's row).  ocamlc handles this with
        // catch/exit fallthrough; we fall back to the row-by-row matcher (always
        // correct for aliasing) when 2+ keys coincide with remaining columns.
        if (keys.size() >= 2 && i + 1 < comps.size()) return nullptr;
        std::vector<MRow> dft;
        for (auto& r : rows) if (!is_ctor(r, i)) dft.push_back(bindv(r));
        LamPtr acc = dft.empty() ? raise_predef("Match_failure", mloc)
                                 : mm_cols(comps, dft, i + 1, mloc);
        if (!acc) return nullptr;
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
          LamPtr idv = exn_value(rep[*it]);
          if (!idv) return nullptr;
          std::vector<MRow> sub;
          for (auto& r : rows) {
            if (is_ctor(r, i)) { if (ident_key(ctor_of(*r.cols[i])) == *it) sub.push_back(r); }
            else sub.push_back(bindv(r));
          }
          LamPtr body = mm_cols(comps, std::move(sub), i + 1, mloc);
          if (!body) return nullptr;
          auto eq = mk(Lam::K::Prim); eq->prim = Prim::IntCmp; eq->prim_id = "==";
          eq->args = {comps[i], idv};
          auto iff = mk(Lam::K::IfThenElse);
          iff->cond = eq; iff->then_ = body; iff->else_ = acc;
          acc = iff;
        }
        return acc;
      }
    }
    // Determine this column's type shape (n_const, n_block).
    std::string coltype;
    for (auto& r : rows)
      if (std::get_if<Ppat_construct>(&r.cols[i]->desc)) {
        auto ci = ctor_info_.find(ctor_of(*r.cols[i]));
        if (ci == ctor_info_.end()) return nullptr;
        coltype = ci->second.type; break;
      }
    auto tcit = type_ctors_.find(coltype);
    if (tcit == type_ctors_.end()) return nullptr;
    int n_const = tcit->second.first, n_block = tcit->second.second;
    // An all-constant enum column (`type t = A | B | C | ..`): switch on the tag,
    // recursing on the remaining columns per case.  A var/any row matches every
    // tag (and the default); a constructor row only its tag.
    if (n_block == 0 && n_const >= 1) {
      // Emit a DENSE exhaustive switch (a case for every tag 0..n_const-1): our
      // bytecode Kswitch indexes labels by tag and has no failaction slot, so a
      // sparse switch + sw_default would read out of bounds.  Gaps reuse the
      // shared default body.  Cap the width so a huge enum falls back instead.
      if (n_const > 64) return nullptr;
      auto is_ctor = [](const MRow& r, size_t c) {
        return std::get_if<Ppat_construct>(&r.cols[c]->desc) != nullptr; };
      auto bind = [&](MRow r) -> MRow {
        if (auto* pv = std::get_if<Ppat_var>(&r.cols[i]->desc))
          r.binds.push_back({pv->name.txt, comps[i]->var});
        return r; };
      std::vector<MRow> dft_rows;
      for (auto& r : rows) if (!is_ctor(r, i)) dft_rows.push_back(bind(r));
      LamPtr dft_body = dft_rows.empty() ? raise_predef("Match_failure", mloc)
                                         : mm_cols(comps, dft_rows, i + 1, mloc);
      if (!dft_body) return nullptr;
      auto sw = mk(Lam::K::Switch); sw->cond = comps[i];
      for (int t = 0; t < n_const; ++t) {
        std::vector<MRow> sub;
        bool explicit_t = false;
        for (auto& r : rows) {
          if (is_ctor(r, i)) {
            if (ctor_info_.at(ctor_of(*r.cols[i])).tag == t) { sub.push_back(r); explicit_t = true; }
          } else sub.push_back(bind(r));
        }
        if (!explicit_t) { sw->sw_consts.push_back({t, dft_body}); continue; }
        LamPtr cb = mm_cols(comps, std::move(sub), i + 1, mloc);
        if (!cb) return nullptr;
        sw->sw_consts.push_back({t, cb});
      }
      return sw;  // exhaustive: switch*, no failaction
    }
    if (n_const != 1 || n_block != 1) return nullptr;  // only 1-const/1-block below
    // ctor column over a 1-const/1-block type: (if comp <block> <const>)
    std::vector<MRow> crows, brows;
    bool field_used = false;
    std::string fname;
    for (auto& r : rows) {
      auto* kc = std::get_if<Ppat_construct>(&r.cols[i]->desc);
      auto& ci = ctor_info_.at(ctor_of(*r.cols[i]));
      if (ci.is_block) {
        if (kc->arg)
          if (auto* pv = std::get_if<Ppat_var>(&effective_pat(kc->arg->get())->desc)) {
            field_used = true;
            if (fname.empty()) fname = pv->name.txt;
          }
        brows.push_back(r);
      } else {
        crows.push_back(r);
      }
    }
    LamPtr cbranch = mm_cols(comps, std::move(crows), i + 1, mloc);
    LamPtr bbranch;
    if (field_used) {
      Ident fid = fname.empty() ? fresh("", true) : fresh(fname);
      for (auto& r : brows) {
        auto* kc = std::get_if<Ppat_construct>(&r.cols[i]->desc);
        if (kc->arg)
          if (auto* pv = std::get_if<Ppat_var>(&effective_pat(kc->arg->get())->desc))
            r.binds.push_back({pv->name.txt, fid});
      }
      LamPtr inner = mm_cols(comps, std::move(brows), i + 1, mloc);
      auto l = mk(Lam::K::Let);
      l->bindings = {{fid, ValueKind::Gen, fieldimm(0, comps[i]), true}};
      l->body = inner;
      bbranch = l;
    } else {
      bbranch = mm_cols(comps, std::move(brows), i + 1, mloc);
    }
    auto iff = mk(Lam::K::IfThenElse);
    iff->cond = comps[i];
    iff->then_ = bbranch;
    iff->else_ = cbranch;
    return iff;
  }

  // ===== Simplif.simplify_local_functions (lambda/simplif.ml) port =====
  // A let-bound function whose every use is a FULL application in one shared
  // "tail scope" (and the same function) becomes a static-catch handler on
  // that scope, its calls becoming exits -- zero allocation.  ocamlc applies
  // this automatically; [@local] forces it, [@inline]/[@local never] disable.
  // func held by LamPtr: the rewrite erases the binding that owns the function
  // node before re-attaching its body as a handler.
  struct LFSlot { LamPtr func; Lam* fn_scope; Lam* scope = nullptr; };
  std::unordered_map<int, LFSlot> lf_slots_;        // binder stamp -> slot
  std::unordered_map<int, int> lf_static_id_;       // binder stamp -> exit id
  std::unordered_map<Lam*, std::vector<std::pair<int, LamPtr>>> lf_static_;
  Lam* lf_scope_ = nullptr;
  Lam* lf_fnscope_ = nullptr;
  static bool lf_enabled(const Lam* f) {
    const std::string& a = f->inline_attr;
    if (a.find("never_local") != std::string::npos) return false;
    if (a.find("always_local") != std::string::npos) return true;
    return a.find("always_inline") == std::string::npos;
  }
  void lf_nontail(Lam* l) {
    if (!l) return;
    Lam* sv = lf_scope_;
    lf_scope_ = l;
    lf_tail(l);
    lf_scope_ = sv;
  }
  void lf_fndef(Lam* f) {  // PR11383: never move code across function boundaries
    Lam* sv = lf_fnscope_;
    lf_fnscope_ = f->body.get();
    lf_nontail(f->body.get());
    lf_fnscope_ = sv;
  }
  void lf_tail(Lam* l) {
    if (!l) return;
    switch (l->k) {
      case Lam::K::Let: {
        std::vector<size_t> fnb;  // indices of candidate function bindings
        for (size_t i = 0; i < l->bindings.size(); ++i) {
          auto& b = l->bindings[i];
          if (b.val && b.val->k == Lam::K::Function && lf_enabled(b.val.get()) &&
              !b.alias && !b.mut) {
            lf_slots_[b.id.stamp] = {b.val, lf_fnscope_, nullptr};
            fnb.push_back(i);
          } else if (b.val) {
            lf_nontail(b.val.get());
          }
        }
        lf_tail(l->body.get());
        // later (inner) bindings finalize first, like ocamlc's nested Llets
        for (auto it = fnb.rbegin(); it != fnb.rend(); ++it) {
          auto& b = l->bindings[*it];
          auto si = lf_slots_.find(b.id.stamp);
          if (si != lf_slots_.end() && si->second.scope) {
            int st = ++next_exit_;
            Lam* sc = si->second.scope == lf_scope_ ? l->body.get()
                                                    : si->second.scope;
            lf_static_id_[b.id.stamp] = st;
            lf_static_[sc].push_back({st, si->second.func});
            // the body becomes a handler in that scope
            Lam* sv = lf_scope_;
            lf_scope_ = si->second.scope;
            lf_tail(si->second.func->body.get());
            lf_scope_ = sv;
            lf_slots_.erase(b.id.stamp);
          } else {  // unused, or disqualified mid-analysis: an ordinary function
            lf_slots_.erase(b.id.stamp);
            lf_fndef(b.val.get());
          }
        }
        return;
      }
      case Lam::K::Apply: {
        if (l->fn && l->fn->k == Lam::K::Var) {
          auto it = lf_slots_.find(l->fn->var.stamp);
          if (it != lf_slots_.end()) {
            auto& s = it->second;
            if (l->args.size() != s.func->params.size())
              lf_slots_.erase(it);  // partial / over-application
            else if (s.scope && s.scope != lf_scope_)
              lf_slots_.erase(it);  // a second, different tail scope
            else if (s.fn_scope != lf_fnscope_)
              lf_slots_.erase(it);  // crosses a function boundary
            else if (!s.scope)
              s.scope = lf_scope_;  // first use pins the tail scope
          }
        } else {
          lf_nontail(l->fn.get());
        }
        for (auto& a : l->args) lf_nontail(a.get());
        return;
      }
      case Lam::K::Var:
        lf_slots_.erase(l->var.stamp);  // used as a value: disqualify
        return;
      case Lam::K::Function: lf_fndef(l); return;
      case Lam::K::Sequence: lf_nontail(l->cond.get()); lf_tail(l->else_.get()); return;
      case Lam::K::IfThenElse:
        lf_nontail(l->cond.get());
        lf_tail(l->then_.get());
        lf_tail(l->else_.get());
        return;
      case Lam::K::Switch:
        lf_nontail(l->cond.get());
        for (auto& c : l->sw_consts) lf_tail(c.body.get());
        for (auto& c : l->sw_blocks) lf_tail(c.body.get());
        if (l->sw_default) lf_tail(l->sw_default.get());
        return;
      case Lam::K::Catch: lf_tail(l->cond.get()); lf_tail(l->then_.get()); return;
      case Lam::K::Staticraise:
        for (auto& a : l->args) lf_nontail(a.get());
        return;
      case Lam::K::Try: lf_nontail(l->body.get()); lf_tail(l->then_.get()); return;
      case Lam::K::While: lf_nontail(l->cond.get()); lf_nontail(l->body.get()); return;
      case Lam::K::For:
        lf_nontail(l->then_.get());
        lf_nontail(l->else_.get());
        lf_nontail(l->body.get());
        return;
      case Lam::K::Letrec:
        for (auto& b : l->bindings) if (b.val) lf_nontail(b.val.get());
        lf_tail(l->body.get());
        return;
      case Lam::K::Assign: lf_nontail(l->cond.get()); return;
      case Lam::K::Prim:
        for (auto& a : l->args) lf_nontail(a.get());
        return;
      default: return;  // constants / Mutvar
    }
  }
  void lf_rewrite(LamPtr& l) {
    if (!l) return;
    Lam* orig = l.get();
    switch (l->k) {
      case Lam::K::Let: {
        for (auto it = l->bindings.begin(); it != l->bindings.end();) {
          if (lf_static_id_.count(it->id.stamp)) it = l->bindings.erase(it);
          else { lf_rewrite(it->val); ++it; }
        }
        lf_rewrite(l->body);
        if (l->bindings.empty()) { LamPtr b = l->body; *l = *b; }
        break;
      }
      case Lam::K::Apply: {
        if (l->fn && l->fn->k == Lam::K::Var) {
          auto it = lf_static_id_.find(l->fn->var.stamp);
          if (it != lf_static_id_.end()) {
            for (auto& a : l->args) lf_rewrite(a);
            l->k = Lam::K::Staticraise;
            l->prim_arg = it->second;
            l->fn = nullptr;
            break;
          }
        }
        lf_rewrite(l->fn);
        for (auto& a : l->args) lf_rewrite(a);
        break;
      }
      default: {
        lf_rewrite(l->fn);
        lf_rewrite(l->body);
        lf_rewrite(l->cond);
        lf_rewrite(l->then_);
        lf_rewrite(l->else_);
        lf_rewrite(l->sw_default);
        for (auto& a : l->args) lf_rewrite(a);
        for (auto& b : l->bindings) lf_rewrite(b.val);
        for (auto& c : l->sw_consts) lf_rewrite(c.body);
        for (auto& c : l->sw_blocks) lf_rewrite(c.body);
        break;
      }
    }
    // wrap the catches attached to this (original) node: oldest innermost
    if (auto it = lf_static_.find(orig); it != lf_static_.end()) {
      auto handlers = std::move(it->second);
      lf_static_.erase(it);
      for (auto& [st, fnp] : handlers) {
        auto inner = std::make_shared<Lam>(*l);
        LamPtr hb = fnp->body;
        lf_rewrite(hb);
        auto cat = mk(Lam::K::Catch);
        cat->cond = inner;
        cat->prim_arg = st;
        cat->then_ = hb;
        for (auto& p : fnp->params) {
          cat->catch_vars.push_back(p.first);
          cat->catch_var_kinds.push_back(p.second);
        }
        *l = *cat;
      }
    }
  }
  // After local functions became static catches, a heap ref whose only closure
  // uses were the converted functions no longer escapes: demote it to a mutable
  // local (`=mut` + assign/*r) -- ocamlc's ref elimination also runs after
  // simplify_local_functions, which is exactly why its tail-scope refs are flat.
  void demote_refs(LamPtr& l) {
    if (!l) return;
    if (l->k == Lam::K::Let) {
      for (size_t i = 0; i < l->bindings.size(); ++i) {
        auto& b = l->bindings[i];
        if (b.val && b.val->k == Lam::K::Prim && b.val->prim == Prim::Makemutable &&
            b.val->prim_arg == 0 && b.val->args.size() == 1 && !b.mut && !b.alias) {
          bool esc = ref_escapes(l->body, b.id);
          for (size_t j = i + 1; j < l->bindings.size() && !esc; ++j)
            esc = ref_escapes(l->bindings[j].val, b.id);
          if (!esc) {
            ValueKind k = b.val->blk_shape.empty() ? ValueKind::Gen : b.val->blk_shape[0];
            b.kind = k;
            b.mut = true;
            b.val = b.val->args[0];
            for (size_t j = i + 1; j < l->bindings.size(); ++j)
              ref_rewrite(l->bindings[j].val, b.id);
            ref_rewrite(l->body, b.id);
          }
        }
      }
    }
    demote_refs(l->fn);
    demote_refs(l->body);
    demote_refs(l->cond);
    demote_refs(l->then_);
    demote_refs(l->else_);
    demote_refs(l->sw_default);
    for (auto& a : l->args) demote_refs(a);
    for (auto& b : l->bindings) demote_refs(b.val);
    for (auto& c : l->sw_consts) demote_refs(c.body);
    for (auto& c : l->sw_blocks) demote_refs(c.body);
  }
  void simplify_local_functions(LamPtr& root) {
    lf_slots_.clear();
    lf_static_id_.clear();
    lf_static_.clear();
    lf_scope_ = lf_fnscope_ = root.get();
    lf_tail(root.get());
    lf_slots_.clear();
    if (lf_static_.empty()) return;
    lf_rewrite(root);
    demote_refs(root);
  }

  LamPtr field_read(const FieldInfo* fi, const LamPtr& s) {
    auto l = mk(Lam::K::Prim);
    auto rt = rec_types_.find(fi->type);
    if (rt != rec_types_.end() && rt->second.flat)
      l->prim = Prim::Floatfield;  // flat float record: unboxed field read
    else
      l->prim = fi->kind == ValueKind::Int ? Prim::FieldInt
                : fi->mut                  ? Prim::FieldMut
                                           : Prim::FieldImm;
    l->prim_arg = fi->index; l->args = {s};
    // A read of a MUTABLE field is bound StrictOpt, not Alias (matching.ml maps
    // Immutable->Alias, Mutable->StrictOpt).  An immediate mutable field reads as
    // `field_int` (spelling ignores mutability), so the spelling alone can't tell
    // -- record the node so wrap_binders picks the right let-kind.
    if (fi->mut) mutfield_reads_.insert(l.get());
    return l;
  }
  std::set<const void*> mutfield_reads_;  // mutable-field read nodes (StrictOpt)
  // Bind the variables of an irrefutable pattern (var / alias / tuple / record /
  // single-constructor) to field reads of `scrut`, recording (ident, access) in
  // `out` and binding the names in the current scope.  Returns false on any shape
  // that isn't irrefutably destructurable (the caller falls back to a plain temp).
  bool collect_binders(const Pattern& p0, const LamPtr& scrut,
                       std::vector<std::pair<Ident, LamPtr>>& out) {
    const Pattern* p = effective_pat(&p0);
    if (std::holds_alternative<Ppat_any>(p->desc)) return true;
    if (auto* pv = std::get_if<Ppat_var>(&p->desc)) {
      Ident id = fresh(pv->name.txt);
      scope.back()[pv->name.txt] = id; apply_var_node_path(p, id);
      out.push_back({id, scrut}); return true;
    }
    if (auto* up = std::get_if<Ppat_unpack>(&p->desc)) {  // `(module M [: S])`: bind M
      if (up->name.txt) {
        Ident id = fresh(*up->name.txt);
        scope.back()[*up->name.txt] = id; out.push_back({id, scrut});
        module_ident_[*up->name.txt] = id;
        std::string mt;
        if (up->pkg && lid_to_dotted(up->pkg->path.txt, mt))
          register_pack_layouts(*up->name.txt, mt);  // so `M.x` in a guard/body resolves
      }
      return true;
    }
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {  // `pat as x`: bind x and recurse
      Ident id = fresh(pa->name.txt);
      scope.back()[pa->name.txt] = id; out.push_back({id, scrut});
      return collect_binders(*pa->p, scrut, out);
    }
    if (auto* pt = std::get_if<Ppat_tuple>(&p->desc)) {
      for (size_t i = 0; i < pt->elems.size(); ++i)
        if (!collect_binders(*pt->elems[i], fieldimm((int)i, scrut), out)) return false;
      return true;
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      std::vector<std::string> flds;
      for (auto& [lbl, sub] : pr->fields) flds.push_back(lid_last(lbl.txt));
      for (auto& [lbl, sub] : pr->fields) {
        FieldInfo nfi;
        const FieldInfo* fi = resolve_record_pat_field(flds, lbl.txt, nfi, p, pat_record_qual_mod(*pr));
        tag_nested_record_subpat(fi, lid_last(lbl.txt), sub.get());
        if (!fi) {
          // The predefined `'a ref = { mutable contents }` cell: a `{contents=p}`
          // pattern reads the mutable field 0 (deferred to here, the function
          // body, so it isn't read until full saturation -- syntactic_arity).
          if (lid_last(lbl.txt) == "contents" && pr->fields.size() == 1) {
            auto fm = mk(Lam::K::Prim); fm->prim = Prim::FieldMut;
            fm->prim_arg = 0; fm->args = {scrut};
            mutfield_reads_.insert(fm.get());
            if (!collect_binders(*sub, fm, out)) return false;
            continue;
          }
          return false;
        }
        if (!collect_binders(*sub, field_read(fi, scrut), out)) return false;
      }
      return true;
    }
    if (auto* pk = std::get_if<Ppat_construct>(&p->desc)) {
      auto ci = ctor_info_.find(ctor_of(*p));
      if (ci == ctor_info_.end()) return false;
      auto tc = type_ctors_.find(ci->second.type);
      // Irrefutable only for a single-constructor type, OR a GADT one: there, type
      // refinement can make a one-constructor parameter pattern (`let f (Float x)`)
      // exhaustive even though the type has several constructors, and ocamlc emits a
      // bare field read with no tag test.
      if (tc == type_ctors_.end()) return false;
      if (tc->second.first + tc->second.second != 1 && !gadt_types_.count(ci->second.type))
        return false;
      // inline record (`T {pos}`): each named label reads its block field
      if (!ci->second.rlabels.empty()) {
        auto* pr = pk->arg ? std::get_if<Ppat_record>(&effective_pat(pk->arg->get())->desc)
                           : nullptr;
        if (!pr) return false;
        auto& L = ci->second.rlabels;
        for (auto& [lbl, sub] : pr->fields) {
          int ix = -1;
          for (size_t i2 = 0; i2 < L.size(); ++i2)
            if (L[i2] == lid_last(lbl.txt)) { ix = (int)i2; break; }
          if (ix < 0) return false;
          FieldInfo fi{ci->second.type, ix, ci->second.rfmut[ix], ci->second.rshape[ix]};
          if (!collect_binders(*sub, field_read(&fi, scrut), out)) return false;
          tag_inline_field_var(sub.get(), ci->second, ix);
        }
        return true;
      }
      auto fps = ctor_field_pats(pk, ci->second.arity);
      if ((int)fps.size() != ci->second.arity) return false;
      for (size_t i = 0; i < fps.size(); ++i) {
        if (!collect_binders(*fps[i], fieldimm((int)i, scrut), out)) return false;
        if (auto* pv = std::get_if<Ppat_var>(&effective_pat(fps[i])->desc))
          if (auto* b = lookup(pv->name.txt)) apply_var_node_path(effective_pat(fps[i]), *b);
      }
      return true;
    }
    // A polymorphic-variant pattern: the block is [hash; arg], the payload at
    // field 1 (exhaustive by typing when ocamlc emits no tag test either).
    if (auto* pvr = std::get_if<Ppat_variant>(&p->desc)) {
      if (!pvr->arg) return true;
      return collect_binders(**pvr->arg, fieldimm(1, scrut), out);
    }
    // `lazy p`: force the scrutinee (an effect that must happen even when p
    // binds nothing -- ocamlc binds a strict *match*), then destructure p
    // against the forced value.
    if (auto* pz = std::get_if<Ppat_lazy>(&p->desc)) {
      const Pattern* sub = effective_pat(pz->p.get());
      Ident id;
      if (auto* pv = std::get_if<Ppat_var>(&sub->desc)) {
        id = fresh(pv->name.txt);
        scope.back()[pv->name.txt] = id;
      } else {
        id = fresh("", true);
      }
      lazy_force_binders_.insert(id.stamp);
      out.push_back({id, force_lazy(scrut)});
      if (std::holds_alternative<Ppat_var>(sub->desc) ||
          std::holds_alternative<Ppat_any>(sub->desc))
        return true;
      return collect_binders(*sub, varof(id), out);
    }
    return false;
  }
  // Strip type constraints and peel `[@@unboxed]` constructor wrappers (whose
  // value is transparently their argument), giving the pattern that actually
  // tests/binds the scrutinee.
  const Pattern* effective_pat(const Pattern* p) {
    while (true) {
      while (auto* c = std::get_if<Ppat_constraint>(&p->desc)) p = c->p.get();
      // `M.(P)` / `M.{ field = .. }` (compile.ml's `to_bytecode Typedtree.{structure;
      // coercion; _}`): register M's record labels so P's unqualified labels resolve,
      // then descend into P.
      while (auto* po = std::get_if<Ppat_open>(&p->desc)) {
        if (auto* l = std::get_if<Lident>(&po->mod_.txt.v)) register_module_records(l->name);
        p = po->p.get();
      }
      auto* k = std::get_if<Ppat_construct>(&p->desc);
      if (!k) return p;
      auto ci = ctor_info_.find(ctor_of(*p));
      if (ci == ctor_info_.end() || !ci->second.unboxed) return p;
      auto fps = ctor_field_pats(k, ci->second.arity);
      if (fps.size() != 1) return p;
      p = fps[0];
    }
  }
  // Flatten a (possibly nested) or-pattern into its leaf alternatives.
  void flatten_or(const Pattern* p, std::vector<const Pattern*>& alts) {
    p = effective_pat(p);
    if (auto* po = std::get_if<Ppat_or>(&p->desc)) {
      flatten_or(po->l.get(), alts); flatten_or(po->r.get(), alts);
    } else alts.push_back(p);
  }
  // Variable-name -> field-access for one already-selected constructor arm (no
  // tag test, no scope binding).  False on a refutable sub-pattern (a constant,
  // a nested or, ...) so the caller falls back.
  bool or_accesses(const Pattern& p0, const LamPtr& scrut,
                   std::vector<std::pair<std::string, LamPtr>>& out) {
    const Pattern* p = effective_pat(&p0);
    if (std::holds_alternative<Ppat_any>(p->desc)) return true;
    if (auto* pv = std::get_if<Ppat_var>(&p->desc)) { out.push_back({pv->name.txt, scrut}); return true; }
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {
      out.push_back({pa->name.txt, scrut}); return or_accesses(*pa->p, scrut, out);
    }
    if (auto* pt = std::get_if<Ppat_tuple>(&p->desc)) {
      for (size_t i = 0; i < pt->elems.size(); ++i)
        if (!or_accesses(*pt->elems[i], fieldimm((int)i, scrut), out)) return false;
      return true;
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      std::vector<std::string> flds;
      for (auto& [lbl, sub] : pr->fields) flds.push_back(lid_last(lbl.txt));
      for (auto& [lbl, sub] : pr->fields) {
        FieldInfo nfi;
        const FieldInfo* fi = resolve_record_pat_field(flds, lbl.txt, nfi, p, pat_record_qual_mod(*pr));
        tag_nested_record_subpat(fi, lid_last(lbl.txt), sub.get());
        if (!fi || !or_accesses(*sub, field_read(fi, scrut), out)) return false;
      }
      return true;
    }
    if (auto* pk = std::get_if<Ppat_construct>(&p->desc)) {
      auto ci = ctor_info_.find(ctor_of(*p));
      if (ci == ctor_info_.end()) return false;
      auto fps = ctor_field_pats(pk, ci->second.arity);
      if ((int)fps.size() != ci->second.arity) return false;
      for (size_t i = 0; i < fps.size(); ++i)
        if (!or_accesses(*fps[i], fieldimm((int)i, scrut), out)) return false;
      return true;
    }
    return false;  // a refutable / unsupported sub-pattern
  }
  // `let (orpat) = e`: an or-pattern of same-type constructor alternatives, each
  // binding the same variables (possibly at different field positions).  Build a
  // switch over the scrutinee tag whose arms produce the bound values -- for >1
  // variable, each arm makes a tuple in the canonical (first-alternative) order,
  // which the caller projects.  ocamlc instead passes the values out of the
  // switch as static-exception arguments; the tuple form runs identically.
  // Returns the switch and var order, or false (the caller keeps its path).
  // `scrut` must be a variable/temp.
  bool or_pattern_values(const Pattern& orpat, const LamPtr& scrut, const Location& loc,
                         std::vector<std::string>& order, LamPtr& result) {
    std::vector<const Pattern*> alts; flatten_or(&orpat, alts);
    if (alts.size() < 2) return false;
    struct Arm { int tag; bool is_block; std::map<std::string, LamPtr> acc; };
    std::vector<Arm> arms; std::string type;
    for (auto* a : alts) {
      const Pattern* ep = effective_pat(a);
      auto* pk = std::get_if<Ppat_construct>(&ep->desc);
      if (!pk) return false;
      auto ci = ctor_info_.find(ctor_of(*ep));
      if (ci == ctor_info_.end()) return false;
      if (type.empty()) type = ci->second.type;
      else if (type != ci->second.type) return false;
      std::vector<std::pair<std::string, LamPtr>> a2;
      auto fps = ctor_field_pats(pk, ci->second.arity);
      if ((int)fps.size() != ci->second.arity) return false;
      for (size_t i = 0; i < fps.size(); ++i)
        if (!or_accesses(*fps[i], fieldimm((int)i, scrut), a2)) return false;
      Arm arm; arm.tag = ci->second.tag; arm.is_block = ci->second.is_block;
      if (order.empty()) for (auto& [nm, _] : a2) order.push_back(nm);
      for (auto& [nm, ac] : a2) arm.acc[nm] = ac;
      if (arm.acc.size() != order.size()) return false;  // alternatives bind different sets
      arms.push_back(std::move(arm));
    }
    if (order.empty()) return false;
    auto arm_body = [&](Arm& arm) -> LamPtr {
      if (order.size() == 1) return arm.acc[order[0]];
      auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
      for (auto& nm : order) blk->args.push_back(arm.acc[nm]);
      return blk;
    };
    auto sw = mk(Lam::K::Switch); sw->cond = scrut;
    for (auto& arm : arms) {
      Lam::SwitchCase c{arm.tag, arm_body(arm)};
      (arm.is_block ? sw->sw_blocks : sw->sw_consts).push_back(c);
    }
    auto tc = type_ctors_.find(type);  // non-exhaustive -> Match_failure default
    if (tc == type_ctors_.end() || (int)arms.size() != tc->second.first + tc->second.second)
      sw->sw_default = raise_predef("Match_failure", loc);
    result = sw;
    return true;
  }
  // Whether a pattern is irrefutable (always matches): exactly the shapes
  // collect_binders destructures.  Used to decide between field extraction and a
  // partial match (which raises Match_failure on the missing cases).
  bool is_irrefutable(const Pattern& p0) {
    const Pattern* p = effective_pat(&p0);
    if (std::holds_alternative<Ppat_any>(p->desc) ||
        std::holds_alternative<Ppat_var>(p->desc) ||
        std::holds_alternative<Ppat_unpack>(p->desc)) return true;  // `(module M)` always matches
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) return is_irrefutable(*pa->p);
    if (auto* pt = std::get_if<Ppat_tuple>(&p->desc)) {
      for (auto& e : pt->elems) if (!is_irrefutable(*e)) return false;
      return true;
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      for (auto& [lbl, sub] : pr->fields) {
        // The predefined `'a ref` cell `{contents=p}` is always destructurable
        // even though `contents` isn't in the user field registry.
        if (!find_field(lid_last(lbl.txt)) &&
            !(lid_last(lbl.txt) == "contents" && pr->fields.size() == 1))
          return false;
        if (!is_irrefutable(*sub)) return false;
      }
      return true;
    }
    if (auto* pk = std::get_if<Ppat_construct>(&p->desc)) {
      auto ci = ctor_info_.find(ctor_of(*p));
      if (ci == ctor_info_.end()) return false;
      auto tc = type_ctors_.find(ci->second.type);
      if (tc == type_ctors_.end()) return false;
      if (tc->second.first + tc->second.second != 1 && !gadt_types_.count(ci->second.type))
        return false;
      if (!ci->second.rlabels.empty()) {  // inline record: check the label pats
        auto* pr = pk->arg ? std::get_if<Ppat_record>(&effective_pat(pk->arg->get())->desc)
                           : nullptr;
        if (!pr) return false;
        for (auto& [lbl, sub] : pr->fields) if (!is_irrefutable(*sub)) return false;
        return true;
      }
      for (auto* fp : ctor_field_pats(pk, ci->second.arity))
        if (!is_irrefutable(*fp)) return false;
      return true;
    }
    if (auto* pvr = std::get_if<Ppat_variant>(&p->desc))
      return !pvr->arg || is_irrefutable(**pvr->arg);
    if (auto* pz = std::get_if<Ppat_lazy>(&p->desc))  // always matches; forces
      return is_irrefutable(*pz->p);
    return false;
  }
  // Wrap `body` (already compiled with `binders` in scope) so each binder's
  // variable reads its field access -- inlined when used at most once, `=a`-aliased
  // (a field read is an alias) otherwise; exactly ocamlc's matcher + simplif.
  LamPtr wrap_binders(LamPtr body, std::vector<std::pair<Ident, LamPtr>>& binders) {
    std::vector<Lam::Binding> aliases;
    for (auto& [id, acc] : binders) {
      // A lazy-force binder is an effectful computation: always kept, strict,
      // never inlined into its use (Simplif only inlines alias lets).
      if (lazy_force_binders_.count(id.stamp))
        aliases.push_back({id, ValueKind::Gen, acc, false});
      else if (count_var(body, id) <= 1) subst_var(body, id, acc);
      else if (is_mut_field_access(acc)) {  // mutable field -> StrictOpt (`=o`)
        Lam::Binding b; b.id = id; b.kind = ValueKind::Gen; b.val = acc;
        b.strict_opt = true; aliases.push_back(b);
      } else aliases.push_back({id, ValueKind::Gen, acc, is_field_access(acc)});
    }
    if (aliases.empty()) return body;
    auto l = mk(Lam::K::Let); l->bindings = std::move(aliases); l->body = body; return l;
  }
  std::set<int> lazy_force_binders_;  // binder stamps holding a lazy force
  // An IMMUTABLE field read -- the alias (`=a`) class.  A mutable read
  // (field_mut) is a strict computation: re-evaluation could differ.
  static bool is_field_access(const LamPtr& l) {
    return l->k == Lam::K::Prim &&
           (l->prim == Prim::FieldImm || l->prim == Prim::FieldInt);
  }
  // A read of a mutable field (field_mut, or a field_int off a mutable immediate
  // field): bound StrictOpt rather than Alias.
  bool is_mut_field_access(const LamPtr& l) const {
    return l->k == Lam::K::Prim &&
           (l->prim == Prim::FieldMut || mutfield_reads_.count(l.get()));
  }

  static std::vector<Row> rows_of(const std::vector<Case>& cs) {
    std::vector<Row> rs;
    for (auto& c : cs) rs.push_back({&c.lhs, c.rhs.get(), c.guard ? c.guard->get() : nullptr});
    return rs;
  }

  // Compile one block-constructor arm: bind its argument fields (field_imm i scrut)
  // in the body, inlining single-use bindings and `=a`-aliasing multi-use ones (as
  // ocamlc's matcher + simplif do).  Returns null for sub-patterns we don't bind.
  LamPtr build_block_arm(const LamPtr& scrut, const CtorInfo& ci,
                         const Ppat_construct* k, const Expression& rhs,
                         const Expression* guard = nullptr, const LamPtr& dflt = nullptr) {
    scope.emplace_back();
    std::vector<std::pair<Ident, LamPtr>> binders;  // bound var -> field-access path
    bool ok = true;
    // Destructure a constructor field, binding its variables to the field-read
    // chain.  Handles a nested *tuple/record* sub-pattern (`Some (x, y)`,
    // `Ok {a; b}`) but bails on a nested constructor (it would add a tag test we
    // don't model here) or any refutable pattern -- the caller then falls back.
    std::function<void(const Pattern&, const LamPtr&)> destruct =
        [&](const Pattern& p0, const LamPtr& acc) {
      const Pattern* p = effective_pat(&p0);
      if (std::holds_alternative<Ppat_any>(p->desc)) return;
      if (auto* pv = std::get_if<Ppat_var>(&p->desc)) {
        Ident id = fresh(pv->name.txt); scope.back()[pv->name.txt] = id;
        apply_var_node_path(p, id);  // a ctor-arg var: tag its record type
        binders.push_back({id, acc}); return;
      }
      if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {
        Ident id = fresh(pa->name.txt); scope.back()[pa->name.txt] = id;
        binders.push_back({id, acc}); destruct(*pa->p, acc); return;
      }
      if (auto* pt = std::get_if<Ppat_tuple>(&p->desc)) {
        for (size_t i = 0; i < pt->elems.size(); ++i) destruct(*pt->elems[i], fieldimm((int)i, acc));
        return;
      }
      if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
        std::vector<std::string> flds;
        for (auto& [lbl, sub] : pr->fields) flds.push_back(lid_last(lbl.txt));
        for (auto& [lbl, sub] : pr->fields) {
          FieldInfo nfi;
          const FieldInfo* fi = resolve_record_pat_field(flds, lbl.txt, nfi, p, pat_record_qual_mod(*pr));
          tag_nested_record_subpat(fi, lid_last(lbl.txt), sub.get());
          if (!fi) { ok = false; return; }
          destruct(*sub, field_read(fi, acc));
        }
        return;
      }
      ok = false;  // a nested constructor / constant / refutable sub-pattern
    };
    auto bind_field = [&](int idx, const Pattern& p) { destruct(p, fieldimm(idx, scrut)); };
    if (k->arg) {
      const Pattern& arg = **k->arg;
      if (!ci.rlabels.empty()) {
        // inline record: the argument pattern matches the block itself
        // (`T r` binds r to the scrutinee; `T {cnt}` reads the labels' fields).
        // A `T {l=..}` field pattern MUST resolve `l` via the ctor's rlabels, not
        // generic destruct/find_field -- when the label name is shared by another
        // record (env.ml's TycompTbl.Open.components vs IdTbl.opened.components),
        // find_field picks an arbitrary index and reads the wrong block field.
        const Pattern* ap = effective_pat(&arg);
        if (auto* pr = std::get_if<Ppat_record>(&ap->desc)) {
          auto& L = ci.rlabels;
          for (auto& [lbl, sub] : pr->fields) {
            int ix = -1;
            for (size_t i2 = 0; i2 < L.size(); ++i2)
              if (L[i2] == lid_last(lbl.txt)) { ix = (int)i2; break; }
            if (ix < 0) { ok = false; break; }
            FieldInfo fi{ci.type, ix, ci.rfmut[ix], ci.rshape[ix]};
            destruct(*sub, field_read(&fi, scrut));
            tag_inline_field_var(sub.get(), ci, ix);
          }
        } else {
          destruct(arg, scrut);
          // `Local x` binds x to the WHOLE record -- remember its ctor so an
          // ambiguous `x.stamp` resolves via ci.rlabels (find_field would bail).
          if (auto* pv = std::get_if<Ppat_var>(&ap->desc))
            if (auto sit = scope.back().find(pv->name.txt); sit != scope.back().end())
              inline_rec_var_[sit->second.stamp] = &ci;
        }
      } else if (ci.arity > 1) {
        auto* tup = std::get_if<Ppat_tuple>(&arg.desc);
        if (!tup || (int)tup->elems.size() != ci.arity) ok = false;
        else for (int idx = 0; idx < ci.arity && ok; ++idx) bind_field(idx, *tup->elems[idx]);
      } else {
        bind_field(0, arg);
      }
    }
    if (!ok) { scope.pop_back(); return nullptr; }
    LamPtr body = expr(rhs);
    if (guard) {  // a `when` guard: failure falls to the shared default (exit)
      auto i = mk(Lam::K::IfThenElse);
      i->cond = expr(*guard); i->then_ = body; i->else_ = dflt;
      body = i;
    }
    scope.pop_back();
    std::vector<Lam::Binding> aliases;
    for (auto& [id, fa] : binders) {
      if (count_var(body, id) <= 1) subst_var(body, id, fa);
      else aliases.push_back({id, ValueKind::Gen, fa, true});
    }
    if (aliases.empty()) return body;
    auto l = mk(Lam::K::Let); l->bindings = std::move(aliases); l->body = body; return l;
  }

  // Match over constructor patterns of one variant type.  Two exact shapes:
  //   1 constant + 1 block  -> (if scrut <block-arm> <const-arm>)   (option/list)
  //   blocks only (>=2)     -> (switch* scrut case tag T: ...)      (result)
  // Returns null otherwise (mixed isint forms / non-exhaustive / complex
  // sub-patterns), so the caller falls back without harm.  Eligibility is decided
  // before any arm is compiled, so a null return allocates no stamps.
  // One block constructor's arm, given all rows that select it (in source order).
  // A single simple-argument row uses the inlining fast path; multiple rows (or a
  // complex argument) sub-match the constructor's single field (arity 1 only).
  LamPtr build_ctor_group_arm(const LamPtr& scrut, const CtorInfo& ci,
                              const std::vector<const Row*>& rs, const Location& mloc,
                              const LamPtr& dflt) {
    if (rs.size() == 1) {
      auto* k = std::get_if<Ppat_construct>(&rs[0]->lhs->desc);
      const Expression* g = rs[0]->guard;  // a `when` on this single row -> if/dflt
      if (LamPtr a = build_block_arm(scrut, ci, k, *rs[0]->rhs, g, dflt)) return a;
    }
    for (auto* r : rs) if (r->guard) return nullptr;  // multi-row guards: bail
    // An INLINE-RECORD ctor with multiple rows (`Tfunction_cases {cases=..}` in two
    // arms): the block IS the inline record.  Map each row's record pattern to its
    // positional field sub-patterns; if they all constrain a SINGLE common field,
    // sub-match on that field (read at its inline index).  Multi-field divergence is
    // a multi-column match we don't replicate -> bail.
    if (!ci.rlabels.empty()) {
      int col = -1;
      std::vector<std::vector<const Pattern*>> rowfps;
      for (auto* r : rs) {
        auto* k = std::get_if<Ppat_construct>(&r->lhs->desc);
        if (!k) return nullptr;
        auto fps = ctor_field_pats(k, ci.arity);
        if (fps.size() != ci.rlabels.size()) return nullptr;
        for (int i = 0; i < (int)fps.size(); ++i)
          if (!std::holds_alternative<Ppat_any>(effective_pat(fps[i])->desc)) {
            if (col < 0) col = i;
            else if (col != i) return nullptr;  // multi-column: bail
          }
        rowfps.push_back(std::move(fps));
      }
      if (col < 0) return nullptr;
      std::vector<Row> sub;
      for (size_t ri = 0; ri < rs.size(); ++ri)
        sub.push_back({rowfps[ri][col], rs[ri]->rhs, rs[ri]->guard});
      FieldInfo fi{ci.type, col, ci.rfmut[col], ci.rshape[col]};
      return compile_match(field_read(&fi, scrut), sub, mloc);
    }
    if (ci.arity != 1) return nullptr;  // multi-field multi-row: multi-column, bail
    LamPtr field0 = fieldimm(0, scrut);
    std::vector<Row> sub;
    bool has_var = false;  // a row whose inner pattern is a plain variable
    for (auto* r : rs) {
      auto* k = std::get_if<Ppat_construct>(&r->lhs->desc);
      if (!k || !k->arg) return nullptr;
      const Pattern* ip = k->arg->get();
      if (!r->guard && std::holds_alternative<Ppat_var>(ip->desc)) has_var = true;
      sub.push_back({ip, r->rhs, r->guard});
    }
    // With a variable sub-pattern, compile_match binds the field to that user name.
    if (has_var) return compile_match(field0, sub, mloc);
    // Otherwise bind the (possibly reused) field to a *match* temp -- aliased if
    // used more than once, inlined if not (as ocamlc's matcher does).
    Ident tv = fresh("", true);
    auto tvar = mk(Lam::K::Var); tvar->var = tv;
    LamPtr body = compile_match(tvar, sub, mloc);
    if (count_var(body, tv) <= 1) { subst_var(body, tv, field0); return body; }
    auto l = mk(Lam::K::Let); l->bindings = {{tv, ValueKind::Gen, field0, true}}; l->body = body;
    return l;
  }

  LamPtr ctor_match(const LamPtr& scrut, const std::vector<Row>& rows, const Location& mloc,
                    const LamPtr& dflt = nullptr) {
    if (rows.empty()) return nullptr;
    // Peel top-level `pat as x` aliases: x binds to the scrutinee value over its
    // own row (the scrutinee must be a named var; simplif inlines the alias).
    std::vector<Row> prows;
    prows.reserve(rows.size());
    std::vector<std::vector<std::string>> ralias(rows.size());
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      const Pattern* l = rows[ri].lhs;
      while (auto* pa = std::get_if<Ppat_alias>(&l->desc)) {
        if (scrut->k != Lam::K::Var) return nullptr;
        ralias[ri].push_back(pa->name.txt);
        l = pa->p.get();
      }
      prows.push_back({l, rows[ri].rhs, rows[ri].guard});
    }
    auto with_alias = [&](const Row* r, auto&& fn) -> LamPtr {
      size_t idx = (size_t)(r - prows.data());
      if (ralias[idx].empty()) return fn();
      scope.emplace_back();
      for (auto& nm : ralias[idx]) scope.back()[nm] = scrut->var;
      LamPtr b = fn();
      scope.pop_back();
      return b;
    };
    std::string type;
    std::set<int> cseen;                            // covered constant values
    std::map<int, const Row*> crow;                 // const value -> its (sole) row
    std::map<int, std::vector<const Row*>> brows;   // block tag -> its rows, in order
    for (auto& r : prows) {
      if (r.guard && !dflt) return nullptr;  // guards only with a shared default (catch)
      if (r.guard && !std::holds_alternative<Ppat_construct>(r.lhs->desc)) return nullptr;
      auto* k = std::get_if<Ppat_construct>(&r.lhs->desc);
      if (!k) return nullptr;
      auto it = ctor_info_.find(ctor_of(*r.lhs));
      if (it == ctor_info_.end()) return nullptr;
      auto& ci = it->second;
      if (type.empty()) type = ci.type;
      else if (type != ci.type) return nullptr;
      if (r.guard && !ci.is_block) return nullptr;  // guarded constant: bail (rare)
      if (ci.is_block) brows[ci.tag].push_back(&r);
      else { if (k->arg || !cseen.insert(ci.tag).second) return nullptr; crow[ci.tag] = &r; }
    }
    auto tc = type_ctors_.find(type);
    if (tc == type_ctors_.end()) return nullptr;
    int NC = tc->second.first, NB = tc->second.second;
    if (NB < 1) return nullptr;  // const-only -> const_switch handles it
    bool exhaustive = (int)cseen.size() == NC && (int)brows.size() == NB;
    // A partial multi-block match over a type that ALSO has constant ctors needs
    // the matcher's isint-split heuristic we don't replicate; bail.  (Pure-block
    // types -- NC==0, e.g. result -- just fill missing tags with Match_failure.)
    if (!exhaustive && NB >= 2 && NC >= 1) return nullptr;
    // Eligible: compile covered arms, then fill missing ctors with Match_failure
    // (a missing constructor's slot raises, exactly as ocamlc fills partial matches).
    std::map<int, LamPtr> cmap, bmap;
    for (auto& [v, r] : crow)
      cmap[v] = with_alias(r, [&] { return expr(*r->rhs); });
    for (auto& [tag, rs] : brows) {
      auto& ci = ctor_info_.at(ctor_of(*rs[0]->lhs));
      // an aliased row in a multi-row group would need per-sub-row scoping: bail
      if (rs.size() > 1)
        for (auto* r : rs)
          if (!ralias[(size_t)(r - prows.data())].empty()) return nullptr;
      LamPtr body = with_alias(rs[0], [&] {
        return build_ctor_group_arm(scrut, ci, rs, mloc, dflt);
      });
      if (!body) return nullptr;  // (rare: complex sub-pattern; arms already compiled)
      bmap[tag] = body;
    }
    // A missing constructor's slot raises Match_failure, or jumps to the shared
    // default (exit) when one was supplied (a catch context).
    auto miss = [&] { return dflt ? dflt : raise_predef("Match_failure", mloc); };
    std::vector<Lam::SwitchCase> consts, blocks;
    for (int v = 0; v < NC; ++v)
      consts.push_back({v, cmap.count(v) ? cmap[v] : miss()});
    for (int t = 0; t < NB; ++t)
      blocks.push_back({t, bmap.count(t) ? bmap[t] : miss()});
    // nb==1, nc==1 -> truthy `(if scrut <block> <const>)` (option/list).
    if (consts.size() == 1 && blocks.size() == 1) {
      auto i = mk(Lam::K::IfThenElse);
      i->cond = scrut; i->then_ = blocks[0].body; i->else_ = consts[0].body;
      return i;
    }
    // nb>=2 -> one (switch* scrut case int V: .. case tag T: ..) over both.
    if (blocks.size() >= 2) {
      auto sw = mk(Lam::K::Switch); sw->cond = scrut;
      sw->sw_consts = std::move(consts); sw->sw_blocks = std::move(blocks);
      return sw;
    }
    // nb==1, nc==0 -> the single block arm directly (e.g. `type t = T of int`).
    if (consts.empty()) return blocks[0].body;
    // nb==1, nc>=2 -> `(if (isint scrut) <const-dispatch> <block-arm>)`.
    auto isint = mk(Lam::K::Prim); isint->prim = Prim::IntCmp;
    isint->prim_id = "isint"; isint->args = {scrut};
    auto i = mk(Lam::K::IfThenElse);
    i->cond = isint; i->then_ = const_dispatch(scrut, consts); i->else_ = blocks[0].body;
    return i;
  }
  // Dispatch over a variant's constant constructors (values 0..nc-1, sorted):
  // 1 -> the arm; 2 -> `(if scrut <v1> <v0>)`; >=3 -> `(switch* scrut case int V:)`.
  LamPtr const_dispatch(const LamPtr& scrut, std::vector<Lam::SwitchCase>& consts) {
    if (consts.size() == 1) return consts[0].body;
    if (consts.size() == 2) {
      auto i = mk(Lam::K::IfThenElse);
      i->cond = scrut; i->then_ = consts[1].body; i->else_ = consts[0].body;
      return i;
    }
    auto sw = mk(Lam::K::Switch); sw->cond = scrut; sw->sw_consts = consts;
    return sw;
  }

  // ----- nested-pattern matcher (one ctor row + a trailing catch-all) ----------
  // The sub-patterns of a block constructor `K(p0,..,pn)` (its argument fields):
  // for arity>1 the argument is a tuple, for arity 1 it is the single pattern.
  std::vector<const Pattern*> ctor_field_pats(const Ppat_construct* k, int arity) {
    std::vector<const Pattern*> v;
    if (!k->arg) return v;
    const Pattern& arg = **k->arg;
    // An INLINE-RECORD ctor (`Tfunction_cases { cases = .. }`): the arg is a record
    // pattern; map its field patterns to positional sub-patterns in the ctor's
    // label order (absent labels -> wildcard), so the match compiler reads/binds
    // each inline field by index.  Without this the record arg matched no shape and
    // ctor_field_pats returned empty -> a bound var (`c_rhs`) was left unresolved.
    if (auto* pr = std::get_if<Ppat_record>(&effective_pat(&arg)->desc)) {
      auto ci = ctor_info_.find(lid_last(k->id.txt));
      if (ci != ctor_info_.end() && !ci->second.rlabels.empty()) {
        static const Pattern any_pat = [] { Pattern p; p.desc = Ppat_any{}; return p; }();
        for (auto& lbl : ci->second.rlabels) {
          const Pattern* fp = &any_pat;
          for (auto& [l, sub] : pr->fields)
            if (lid_last(l.txt) == lbl) { fp = sub.get(); break; }
          v.push_back(fp);
        }
        return v;
      }
    }
    if (arity > 1) {
      if (auto* tup = std::get_if<Ppat_tuple>(&arg.desc))
        for (auto& e : tup->elems) v.push_back(e.get());
    } else {
      v.push_back(&arg);
    }
    return v;
  }
  // The number of constructor/constant tests a pattern performs against the
  // scrutinee (its "failure points" toward the fallback): 0 for a var/wildcard,
  // 1 per integer-constant or 1-const-1-block constructor (option/list/`A|B of t`),
  // summed over fields.  Returns 999 for any shape this matcher can't compile, so
  // a value in [1,998] guarantees match_pat() below will not return null.
  int count_tests(const Pattern& p0) {
    const Pattern* p = effective_pat(&p0);
    if (std::holds_alternative<Ppat_any>(p->desc) ||
        std::holds_alternative<Ppat_var>(p->desc)) return 0;
    if (auto* pc = std::get_if<Ppat_constant>(&p->desc))
      return std::holds_alternative<Pconst_integer>(pc->c.desc) ? 1 : 999;
    auto* k = std::get_if<Ppat_construct>(&p->desc);
    if (!k) return 999;
    auto cit = ctor_info_.find(ctor_of(*p));
    if (cit == ctor_info_.end()) return 999;
    auto& ci = cit->second;
    auto tc = type_ctors_.find(ci.type);
    if (tc == type_ctors_.end() || tc->second.first != 1 || tc->second.second != 1)
      return 999;  // only 1-const-1-block types (truthy test)
    auto fps = ctor_field_pats(k, ci.arity);
    if ((int)fps.size() != ci.arity) return 999;
    int n = 1;
    for (auto* fp : fps) { int t = count_tests(*fp); if (t >= 999) return 999; n += t; }
    return n;
  }
  // Match the fields of a (1-const-1-block) block constructor, left to right,
  // then run the continuation k; any field mismatch jumps to dflt.
  LamPtr match_fields(const LamPtr& s, const std::vector<const Pattern*>& fps,
                      size_t idx, const std::function<LamPtr()>& k, const LamPtr& dflt) {
    if (idx >= fps.size()) return k();
    return match_pat(fieldimm((int)idx, s), *fps[idx],
                     [&, idx] { return match_fields(s, fps, idx + 1, k, dflt); }, dflt);
  }
  // Match `pat` against `scrut`, binding its variables in the current scope; on a
  // full match run k(), on any mismatch evaluate dflt.  Mirrors ocamlc's decision
  // tree: a reused field is aliased to a `*match*` temp (`=a`), a single-use one is
  // inlined.  Pre-validated by count_tests(), so it does not return null in use.
  LamPtr match_pat(const LamPtr& scrut, const Pattern& pat0,
                   const std::function<LamPtr()>& k, const LamPtr& dflt) {
    const Pattern* p = effective_pat(&pat0);
    if (std::holds_alternative<Ppat_any>(p->desc)) return k();
    if (auto* pv = std::get_if<Ppat_var>(&p->desc)) {
      Ident id = fresh(pv->name.txt);
      scope.back()[pv->name.txt] = id;
      apply_var_node_path(p, id);  // a ctor-arg var: tag its record type before k()
      LamPtr body = k();
      if (count_var(body, id) <= 1) { subst_var(body, id, scrut); return body; }
      auto l = mk(Lam::K::Let);
      l->bindings = {{id, ValueKind::Gen, scrut, true}}; l->body = body; return l;
    }
    if (auto* pc = std::get_if<Ppat_constant>(&p->desc)) {
      auto* pi = std::get_if<Pconst_integer>(&pc->c.desc);
      if (!pi) return nullptr;
      auto ne = mk(Lam::K::Prim); ne->prim = Prim::NotEqInt;
      ne->args = {scrut, cint(parse_ocaml_int(pi->value))};
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = ne; iff->then_ = dflt; iff->else_ = k();
      return iff;
    }
    auto* k_ = std::get_if<Ppat_construct>(&p->desc);
    if (!k_) return nullptr;
    auto cit = ctor_info_.find(ctor_of(*p));
    if (cit == ctor_info_.end()) return nullptr;
    auto& ci = cit->second;
    // Reused scrutinee (a field read, not a bare Var) -> bind it to a temp, aliased
    // if the resulting tree reads it more than once, inlined otherwise.
    LamPtr s = scrut;
    bool need_temp = scrut->k != Lam::K::Var;
    Ident tv;
    if (need_temp) { tv = fresh("", true); auto v = mk(Lam::K::Var); v->var = tv; s = v; }
    LamPtr inner;
    if (ci.is_block) {  // block ctor matched -> test truthy, then match fields
      std::vector<const Pattern*> fps;
      // `Local x` binds x to the WHOLE inline record (the block itself), not a
      // field-0 read.  Bind it to `s`, remember its ctor so `x.stamp` resolves
      // via ci.rlabels (an ambiguous label find_field would bail on), then test.
      if (!ci.rlabels.empty())
        if (const Pattern* ap = k_->arg ? effective_pat(k_->arg->get()) : nullptr)
          if (auto* pv = std::get_if<Ppat_var>(&ap->desc)) {
            Ident id = fresh(pv->name.txt); scope.back()[pv->name.txt] = id;
            inline_rec_var_[id.stamp] = &ci;
            LamPtr body = k();
            if (!body) return nullptr;
            LamPtr fields;
            if (count_var(body, id) <= 1) { subst_var(body, id, s); fields = body; }
            else { auto l = mk(Lam::K::Let);
                   l->bindings = {{id, ValueKind::Gen, s, true}}; l->body = body; fields = l; }
            auto iff = mk(Lam::K::IfThenElse);
            iff->cond = s; iff->then_ = fields; iff->else_ = dflt; inner = iff;
            if (!need_temp) return inner;
            if (count_var(inner, tv) <= 1) { subst_var(inner, tv, scrut); return inner; }
            auto l = mk(Lam::K::Let);
            l->bindings = {{tv, ValueKind::Gen, scrut, true}}; l->body = inner; return l;
          }
      if (!ci.rlabels.empty()) {
        // Inline record (`Cons of {content; next}`): labels are the block's FLAT
        // fields, so match them at their own indices (label order) -- NOT via a
        // field-0 wrapper, which would read `field i (field 0 s)` (Queue.take's
        // `Cons {next = Nil}` -> a wrong double field read).
        auto* pr = k_->arg ? std::get_if<Ppat_record>(&effective_pat(k_->arg->get())->desc)
                           : nullptr;
        if (!pr) return nullptr;
        static const Pattern any_pat = [] { Pattern p; p.desc = Ppat_any{}; return p; }();
        for (auto& lbl : ci.rlabels) {
          const Pattern* fp = &any_pat;
          for (auto& [l, sub] : pr->fields) if (lid_last(l.txt) == lbl) { fp = sub.get(); break; }
          fps.push_back(fp);
        }
      } else {
        fps = ctor_field_pats(k_, ci.arity);
      }
      LamPtr fields = match_fields(s, fps, 0, k, dflt);
      if (!fields) return nullptr;
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = s; iff->then_ = fields; iff->else_ = dflt; inner = iff;
    } else {            // constant ctor matched (None/[]): test truthy inverted
      LamPtr kk = k(); if (!kk) return nullptr;
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = s; iff->then_ = dflt; iff->else_ = kk; inner = iff;
    }
    if (!need_temp) return inner;
    if (count_var(inner, tv) <= 1) { subst_var(inner, tv, scrut); return inner; }
    auto l = mk(Lam::K::Let);
    l->bindings = {{tv, ValueKind::Gen, scrut, true}}; l->body = inner; return l;
  }
  // Drive a `<ctor pattern> -> body | _ -> fallback` match.  One failure point (a
  // single top constructor with simple fields) inlines the fallback as the else
  // branch; two or more share it once behind `(catch .. with (N) fallback)`, with
  // `(exit N)` at each mismatch -- exactly ocamlc's action-sharing for this shape.
  LamPtr nested_match(const LamPtr& scrut, const std::vector<Row>& rows,
                      const Location& mloc) {
    (void)mloc;
    if (rows.size() != 2 || rows[0].guard || rows[1].guard) return nullptr;
    if (!is_catchall(*rows[1].lhs)) return nullptr;
    const Pattern& pat = *rows[0].lhs;
    if (!std::holds_alternative<Ppat_construct>(pat.desc)) return nullptr;
    int nt = count_tests(pat);
    if (nt <= 0 || nt >= 999) return nullptr;  // pure var / unsupported -> existing path
    if (nt == 1) {  // single test: inline the fallback as the else branch
      bind_catchall(*rows[1].lhs, scrut);
      LamPtr fb = expr(*rows[1].rhs);
      scope.emplace_back();
      LamPtr m = match_pat(scrut, pat, [&] { return expr(*rows[0].rhs); }, fb);
      scope.pop_back();
      return m;
    }
    int eid = ++next_exit_;  // multiple tests: share the fallback behind catch/exit
    auto exitL = mk(Lam::K::Staticraise); exitL->prim_arg = eid;
    scope.emplace_back();
    LamPtr m = match_pat(scrut, pat, [&] { return expr(*rows[0].rhs); }, exitL);
    scope.pop_back();
    if (!m) { --next_exit_; return nullptr; }
    bind_catchall(*rows[1].lhs, scrut);
    LamPtr fb = expr(*rows[1].rhs);
    auto c = mk(Lam::K::Catch); c->cond = m; c->prim_arg = eid; c->then_ = fb;
    return c;
  }

  LamPtr compile_match(const LamPtr& scrut, const std::vector<Case>& cases,
                       const Location& mloc) {
    return compile_match(scrut, rows_of(cases), mloc);
  }
  LamPtr compile_match(const LamPtr& scrut, const std::vector<Row>& rows,
                       const Location& mloc) {
    // If the scrutinee is a var of a known imported type, force-register that type's
    // constructors (correct arity/tag) for THIS match, so an ambiguous ctor name
    // (`Named`, shared by 6 types at arities 1/2/3) resolves through the scrutinee's
    // type, not the globally last-registered one.  Restored on return.
    CtorSave ctor_save;
    if (scrut->k == Lam::K::Var)
      if (auto vp = var_record_path_.find(scrut->var.stamp); vp != var_record_path_.end())
        ctor_save = force_register_type_ctors(vp->second);
    struct CtorGuard { Translator* self; CtorSave sv;
                       ~CtorGuard() { self->restore_ctors(sv); } } ctor_guard{this, std::move(ctor_save)};
    // Resolve any qualified stdlib constructors in the rows (Seq.Cons, ...) so
    // the matcher below has their tag/arity like local/predef constructors.
    for (auto& r : rows) scan_pat_ctors(*r.lhs);
    // The union of the arms' top-level record-pattern fields, so a size-1
    // ambiguous-field arm (`{ args = .. }`) is disambiguated by a multi-field
    // sibling (`{ cases = []; args = [] }` pins pattern_matching).  Save/restore.
    std::vector<std::string> saved_mrf = match_rec_fields_;
    {
      std::set<std::string> u;  // this match's arms only (not the enclosing match's)
      for (auto& r : rows)
        if (auto* pr = std::get_if<Ppat_record>(&effective_pat(r.lhs)->desc))
          for (auto& [lbl, sub] : pr->fields) u.insert(lid_last(lbl.txt));
      match_rec_fields_.assign(u.begin(), u.end());
    }
    struct MrfGuard { std::vector<std::string>& cur; std::vector<std::string> sv;
                      ~MrfGuard() { cur = std::move(sv); } } mrf_guard{match_rec_fields_, std::move(saved_mrf)};
    // Peel `[@@unboxed]` constructor wrappers off the patterns (transparent) and
    // re-dispatch, so the matcher below never sees an unboxed constructor.
    bool unbox = false;
    for (auto& r : rows) if (effective_pat(r.lhs) != r.lhs) { unbox = true; break; }
    if (unbox) {
      std::vector<Row> ur;
      for (auto& r : rows) ur.push_back({effective_pat(r.lhs), r.rhs, r.guard});
      return compile_match(scrut, ur, mloc);
    }
    // A non-variable scrutinee with a `| n -> ...` catch-all is bound to n first
    // (`let n = scrut in ...`), so n refers to it inside the arms (matches ocamlc).
    // A binding to a sub-term of the scrutinee (a field read) is an Alias (`=a`).
    if (scrut->k != Lam::K::Var)
      for (auto& r : rows)
        if (!r.guard)
          if (auto* pv = std::get_if<Ppat_var>(&r.lhs->desc)) {
            Ident nid = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = nid;
            auto v = mk(Lam::K::Var); v->var = nid;
            auto body = compile_match(v, rows, mloc);
            auto l = mk(Lam::K::Let);
            l->bindings = {{nid, ValueKind::Gen, scrut, is_field_access(scrut)}};
            l->body = body;
            return l;
          }
    // A non-variable scrutinee with no `| n ->` catch-all is evaluated once into a
    // `*match*` temp, so constructor arms reading its fields don't re-evaluate it
    // (critical when the scrutinee reads mutable state an arm then mutates, e.g. an
    // instance variable).  Kept only if used more than once (else simplif inlines).
    if (scrut->k != Lam::K::Var) {
      Ident mv = fresh("", true);
      LamPtr body = compile_match(varof(mv), rows, mloc);
      // single-use inlining is simplif's ALIAS rule: only a field access (or
      // var) inlines; a strict computation (apply/send/...) stays bound.
      if (count_var(body, mv) <= 1 && is_field_access(scrut)) {
        subst_var(body, mv, scrut);
        return body;
      }
      // The matcher's top argument is bound Strict (`=`), never Alias: the scrutinee
      // is evaluated once into the temp (matching.ml `root_arg arg Strict`), and
      // simplif keeps a multi-use Strict let verbatim.
      auto l = mk(Lam::K::Let); l->bindings = {{mv, ValueKind::Gen, scrut, false}};
      l->body = body; return l;
    }
    // Shared catch-all fallback via catch/exit: a guard on a non-variable pattern
    // makes guard-failure AND pattern-mismatch both reach the trailing catch-all,
    // so ocamlc emits it once behind `(catch <body> with (N) fallback)` with
    // `(exit N)` at each failure path.
    {
      bool guarded_nonvar = false;
      for (auto& r : rows)
        if (r.guard && !is_catchall(*r.lhs)) guarded_nonvar = true;
      if (guarded_nonvar && rows.size() >= 2 && !rows.back().guard &&
          is_catchall(*rows.back().lhs)) {
        int eid = ++next_exit_;
        auto exitL = mk(Lam::K::Staticraise); exitL->prim_arg = eid;
        std::vector<Row> inner(rows.begin(), rows.end() - 1);
        if (LamPtr body = ctor_match(scrut, inner, mloc, exitL)) {
          bind_catchall(*rows.back().lhs, scrut);
          LamPtr fb = expr(*rows.back().rhs);
          auto c = mk(Lam::K::Catch); c->cond = body; c->prim_arg = eid; c->then_ = fb;
          return c;
        }
        --next_exit_;  // inner not buildable: undo id, fall through to best-effort
      } else if (guarded_nonvar) {
        // no explicit catch-all: a guard failure falls to Match_failure (inlined
        // when single-use; ocamlc shares multi-use defaults behind a catch)
        if (LamPtr body = ctor_match(scrut, rows, mloc,
                                     raise_predef("Match_failure", mloc)))
          return body;
      }
    }
    if (rows.size() == 2 && !rows[0].guard && !rows[1].guard) {
      std::string a = ctor_of(*rows[0].lhs), b = ctor_of(*rows[1].lhs);
      if ((a == "true" && b == "false") || (a == "false" && b == "true")) {
        auto i = mk(Lam::K::IfThenElse);
        i->cond = scrut;
        i->then_ = expr(*(a == "true" ? rows[0] : rows[1]).rhs);
        i->else_ = expr(*(a == "false" ? rows[0] : rows[1]).rhs);
        return i;
      }
    }
    if (auto pm = pv_const_match(scrut, rows)) return pm;
    if (auto pt = pvtype_match(scrut, rows)) return pt;
    // A single `#poly`-type row is exhaustive by typing, hence irrefutable: just
    // bind the `as` alias and run the body, no tag test (matches ocamlc, which
    // emits only the scrutinee evaluation).
    if (rows.size() == 1 && !rows[0].guard) {
      const Pattern* p = effective_pat(rows[0].lhs);
      const std::string* bind = nullptr;
      if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {
        bind = &pa->name.txt; p = effective_pat(pa->p.get());
      }
      if (std::holds_alternative<Ppat_type>(p->desc)) {
        scope.emplace_back();
        if (bind) scope.back()[*bind] = scrut->var;
        LamPtr b = expr(*rows[0].rhs);
        scope.pop_back();
        return b;
      }
    }
    // A single irrefutable non-catchall row (e.g. a polyvariant payload
    // `` `A g -> .. ``): destructure directly, no test.
    if (rows.size() == 1 && !rows[0].guard && !is_catchall(*rows[0].lhs) &&
        is_irrefutable(*rows[0].lhs)) {
      std::vector<std::pair<Ident, LamPtr>> binders;
      scope.emplace_back();
      bool ok = collect_binders(*rows[0].lhs, scrut, binders);
      LamPtr body = ok ? expr(*rows[0].rhs) : nullptr;
      scope.pop_back();
      if (body) return wrap_binders(body, binders);
    }
    if (auto sw = const_switch(scrut, rows)) return sw;
    if (auto ds = switcher_match(scrut, rows)) return ds;
    if (auto cm = ctor_match(scrut, rows, mloc)) return cm;
    if (auto nm = nested_match(scrut, rows, mloc)) return nm;
    if (auto em = ext_match(scrut, rows)) return em;
    if (LamPtr r = int_cases(scrut, rows, 0, /*strict=*/true)) return r;
    // shapes the structured paths can't express compile through the correct
    // per-row chain instead of silently collapsing
    if (LamPtr r = naive_match(scrut, rows, mloc)) return r;
    return int_cases(scrut, rows, 0);
  }

  // Constant polymorphic-variant match: dispatch on the tag hashes with the
  // Switcher's test shapes (n<=4 exact; larger fall back to a correct equality
  // chain).  An optional trailing catch-all gives the isint-split catch form
  // (n<=2 exact, matching ocamlc) or an equality chain ending in the fallback.
  LamPtr pv_const_match(const LamPtr& scrut, const std::vector<Row>& rows) {
    struct KV { long long h; const Expression* rhs; };
    std::vector<KV> kvs;
    const Row* dflt = nullptr;
    for (auto& r : rows) {
      if (r.guard) return nullptr;
      const Pattern* p = effective_pat(r.lhs);
      if (auto* pv = std::get_if<Ppat_variant>(&p->desc)) {
        if (pv->arg || dflt) return nullptr;
        kvs.push_back({hash_variant(pv->label), r.rhs});
      } else if (is_catchall(*p) && !dflt && &r == &rows.back()) {
        dflt = &r;
      } else {
        return nullptr;
      }
    }
    if (kvs.size() < (dflt ? 1u : 2u)) return nullptr;
    if (scrut->k != Lam::K::Var) return nullptr;  // bound by compile_match first
    std::sort(kvs.begin(), kvs.end(), [](const KV& a, const KV& b) { return a.h < b.h; });
    for (size_t i = 1; i < kvs.size(); ++i)
      if (kvs[i].h == kvs[i - 1].h) return nullptr;  // duplicate tags
    auto cmp = [&](const char* op, long long h) {
      auto t = mk(Lam::K::Prim); t->prim = Prim::IntCmp; t->prim_id = op;
      t->args = {scrut, cint(h)};
      return t;
    };
    auto iff = [&](LamPtr c, LamPtr a, LamPtr b) {
      auto i = mk(Lam::K::IfThenElse); i->cond = c; i->then_ = a; i->else_ = b;
      return i;
    };
    if (!dflt) {
      std::function<LamPtr(int, int)> tree = [&](int lo, int hi) -> LamPtr {
        int n = hi - lo + 1;
        if (n == 1) return expr(*kvs[lo].rhs);
        if (n == 2)
          return iff(cmp(">=", kvs[hi].h), expr(*kvs[hi].rhs), expr(*kvs[lo].rhs));
        if (n == 3)
          return iff(cmp("!=", kvs[lo + 1].h),
                     iff(cmp(">=", kvs[lo + 2].h), expr(*kvs[lo + 2].rhs),
                         expr(*kvs[lo].rhs)),
                     expr(*kvs[lo + 1].rhs));
        if (n == 4) return iff(cmp(">=", kvs[lo + 2].h), tree(lo + 2, hi), tree(lo, lo + 1));
        // larger: a correct (not byte-exact) equality chain
        LamPtr c = expr(*kvs[hi].rhs);
        for (int i = hi - 1; i >= lo; --i)
          c = iff(cmp("==", kvs[i].h), expr(*kvs[i].rhs), c);
        return c;
      };
      return tree(0, (int)kvs.size() - 1);
    }
    // trailing catch-all: bind its var to the scrutinee for the fallback body
    auto bind_dflt = [&]() -> LamPtr {
      scope.emplace_back();
      if (auto* pv2 = std::get_if<Ppat_var>(&effective_pat(dflt->lhs)->desc))
        scope.back()[pv2->name.txt] = scrut->var;
      LamPtr b = expr(*dflt->rhs);
      scope.pop_back();
      return b;
    };
    if (kvs.size() <= 2) {
      // (catch (if (isint s) <!=-chain, miss -> exit> (exit N)) with (N) fb)
      int eid = ++next_exit_;
      auto exitL = [&] { auto x = mk(Lam::K::Staticraise); x->prim_arg = eid; return x; };
      LamPtr chain = exitL();
      for (int i = (int)kvs.size() - 1; i >= 0; --i)
        chain = iff(cmp("!=", kvs[i].h), chain, expr(*kvs[i].rhs));
      auto isi = mk(Lam::K::Prim); isi->prim = Prim::IntCmp;
      isi->prim_id = "isint"; isi->args = {scrut};
      auto c = mk(Lam::K::Catch);
      c->cond = iff(isi, chain, exitL());
      c->prim_arg = eid; c->then_ = bind_dflt();
      return c;
    }
    // larger with default: equality chain ending in the fallback (correct)
    LamPtr c = bind_dflt();
    for (int i = (int)kvs.size() - 1; i >= 0; --i)
      c = iff(cmp("==", kvs[i].h), expr(*kvs[i].rhs), c);
    return c;
  }

  // Match over polymorphic-variant *type* patterns (`#lambda as x -> ..`): each
  // arm matches the set of tags belonging to a named polyvariant type.  The
  // scrutinee's tag is the value itself for a constant tag, else field 0 of its
  // block; `as x` binds x to the whole scrutinee.  Not byte-exact with ocamlc's
  // interval Switcher, but correct -- enough for execution parity.
  LamPtr pvtype_match(const LamPtr& scrut, const std::vector<Row>& rows) {
    if (scrut->k != Lam::K::Var) return nullptr;
    struct Arm { std::set<long long> tags; const std::string* bind; const Expression* rhs; };
    std::vector<Arm> arms;
    for (auto& r : rows) {
      if (r.guard) return nullptr;
      const Pattern* p = effective_pat(r.lhs);
      const std::string* bind = nullptr;
      if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {
        bind = &pa->name.txt;
        p = effective_pat(pa->p.get());
      }
      auto* pt = std::get_if<Ppat_type>(&p->desc);
      if (!pt) return nullptr;
      std::set<long long> tags;
      std::set<std::string> seen;
      collect_pv_tags_lid(pt->id.txt, tags, seen);
      if (tags.empty()) return nullptr;  // unknown polyvariant type
      arms.push_back({std::move(tags), bind, r.rhs});
    }
    if (arms.size() < 2) return nullptr;
    Ident tagid = fresh("", true);
    auto isi = mk(Lam::K::Prim); isi->prim = Prim::IntCmp; isi->prim_id = "isint";
    isi->args = {scrut};
    auto tagexpr = mk(Lam::K::IfThenElse);
    tagexpr->cond = isi; tagexpr->then_ = scrut; tagexpr->else_ = fieldimm(0, scrut);
    auto tagvar = [&] { auto v = mk(Lam::K::Var); v->var = tagid; return v; };
    auto cmpeq = [&](long long h) {
      auto t = mk(Lam::K::Prim); t->prim = Prim::IntCmp; t->prim_id = "==";
      t->args = {tagvar(), cint(h)};
      return t;
    };
    auto iff = [&](LamPtr c, LamPtr a, LamPtr b) {
      auto i = mk(Lam::K::IfThenElse); i->cond = c; i->then_ = a; i->else_ = b;
      return i;
    };
    auto arm_body = [&](const Arm& a) -> LamPtr {
      scope.emplace_back();
      if (a.bind) scope.back()[*a.bind] = scrut->var;
      LamPtr b = expr(*a.rhs);
      scope.pop_back();
      return b;
    };
    auto member = [&](const std::set<long long>& tags) -> LamPtr {
      std::vector<long long> hs(tags.begin(), tags.end());
      LamPtr c = cmpeq(hs.back());
      for (int i = (int)hs.size() - 2; i >= 0; --i)
        c = iff(cmpeq(hs[i]), cint(1), c);  // (|| (== tag h) rest)
      return c;
    };
    LamPtr body = arm_body(arms.back());  // last arm = else (match is exhaustive)
    for (int i = (int)arms.size() - 2; i >= 0; --i)
      body = iff(member(arms[i].tags), arm_body(arms[i]), body);
    auto l = mk(Lam::K::Let);
    l->bindings = {{tagid, ValueKind::Int, tagexpr, false}};
    l->body = body;
    return l;
  }

  // Match over extension constructors (`type t += A ...`, exceptions, effects):
  // an if-chain comparing constructor identities (field 0 of an applied
  // constructor's block, the value itself for a constant one), each arm binding
  // its fields like a try-handler case.  Requires a variable scrutinee and a
  // trailing catch-all; anything else keeps the existing paths.
  LamPtr ext_match(const LamPtr& scrut, const std::vector<Row>& rows) {
    if (scrut->k != Lam::K::Var || rows.size() < 2) return nullptr;
    if (rows.back().guard || !is_catchall(*rows.back().lhs)) return nullptr;
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
      if (rows[i].guard) return nullptr;
      const Pattern* lp = effective_pat(rows[i].lhs);  // `E .. as x`: x = scrutinee
      while (auto* pa = std::get_if<Ppat_alias>(&lp->desc)) lp = effective_pat(pa->p.get());
      auto* k = std::get_if<Ppat_construct>(&lp->desc);
      if (!k) return nullptr;
      std::string n = lid_last(k->id.txt);
      // A QUALIFIED exn/extension ctor (`Includemod.Error`, `Includemod.
      // Apply_error`) is identity-matched via module_ctor_identity; it resolves
      // ONLY for a module runtime field (exns take one, variant ctors do not),
      // so a non-null result means it really is an imported exception -- and it
      // outranks a same-named bare VARIANT (`result.Error`) the unqualified
      // resolution would otherwise pick (mis-reading the exn as a tagged variant).
      bool qualified_exn = std::holds_alternative<Ldot>(k->id.txt.v) &&
                           qualified_exn_identity(k->id.txt) != nullptr;
      // a variant-ctor entry blocks the exception reading -- unless it is a
      // shadowed builtin (`exception Error` vs result's Error).  A predefined
      // exception (Failure/Invalid_argument/..) counts as an exception ctor too:
      // without this, `function Failure s -> ..` collapses and drops `s`.
      if (!qualified_exn &&
          ((!exn_ident_.count(n) && !exn_field_.count(n) && !is_predef_exn_name(n)) ||
           (ctor_info_.count(n) && !builtin_ctors_.count(n))))
        return nullptr;
      if (k->arg) {  // binder shapes exn_case_body supports only
        // Inline-record exn payload `{l; ..}`: exn_case_body binds via label
        // index when the labels are known; require simple field sub-patterns.
        if (auto* pr = std::get_if<Ppat_record>(&effective_pat(k->arg->get())->desc)) {
          if (exn_inline_labels(n, &k->id.txt).empty()) return nullptr;
          for (auto& [lbl, sub] : pr->fields) {
            const Pattern* e = effective_pat(sub.get());
            if (!std::holds_alternative<Ppat_any>(e->desc) &&
                !std::holds_alternative<Ppat_var>(e->desc) &&
                !std::holds_alternative<Ppat_alias>(e->desc) && !is_irrefutable(*e))
              return nullptr;
          }
          continue;
        }
        int arity = exn_arity_.count(n) ? exn_arity_[n] : 1;
        for (auto* fp : ctor_field_pats(k, arity)) {
          const Pattern* e = effective_pat(fp);
          if (auto* pc = std::get_if<Ppat_constant>(&e->desc)) {
            // constant payloads exn_case_body can test (string/int/char)
            if (std::holds_alternative<Pconst_string>(pc->c.desc)) continue;
            if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc); pi && !pi->suffix)
              continue;
            if (std::holds_alternative<Pconst_char>(pc->c.desc)) continue;
            return nullptr;
          }
          if (!std::holds_alternative<Ppat_any>(e->desc) &&
              !std::holds_alternative<Ppat_var>(e->desc) &&
              !std::holds_alternative<Ppat_alias>(e->desc) && !is_irrefutable(*e))
            return nullptr;
        }
      }
    }
    return ext_match_arm(scrut->var, rows, 0);
  }
  LamPtr ext_match_arm(const Ident& sid, const std::vector<Row>& rows, size_t i) {
    auto sv = [&] { auto v = mk(Lam::K::Var); v->var = sid; return v; };
    if (i + 1 == rows.size()) {
      bind_catchall(*rows[i].lhs, sv());
      return expr(*rows[i].rhs);
    }
    const Pattern* lp = effective_pat(rows[i].lhs);
    std::vector<std::string> aliases;
    while (auto* pa = std::get_if<Ppat_alias>(&lp->desc)) {
      aliases.push_back(pa->name.txt);
      lp = effective_pat(pa->p.get());
    }
    auto* k = std::get_if<Ppat_construct>(&lp->desc);
    std::string n = lid_last(k->id.txt);
    // a module-qualified extension ctor (`M1.E`) tests that module's own field,
    // so distinct same-named ctors in sibling modules stay distinct (local AND
    // imported, e.g. `Includemod.Error` vs the bare `result.Error` variant).
    LamPtr idv = qualified_exn_identity(k->id.txt);
    if (!idv) idv = exn_value(n);
    if (!idv) return nullptr;
    LamPtr lhs = k->arg ? fieldimm(0, sv()) : sv();
    std::vector<PayloadTest> ptests;
    LamPtr body = exn_case_body(sid, k, n, *rows[i].rhs, &ptests, &aliases);
    if (!body) return nullptr;
    LamPtr rest = ext_match_arm(sid, rows, i + 1);
    if (!rest) return nullptr;
    auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
    test->args = {lhs, idv};
    if (!ptests.empty()) {
      // constant payloads: identity-fail and each payload-fail share the
      // rest of the chain behind a catch/exit (like exn_dispatch)
      int eid = ++next_exit_;
      auto exitL = [&] {
        auto x = mk(Lam::K::Staticraise); x->prim_arg = eid; return x;
      };
      for (auto it = ptests.rbegin(); it != ptests.rend(); ++it) {
        auto t = mk(Lam::K::Prim);
        if (it->string_eq) { t->prim = Prim::Ccall; t->prim_id = "caml_string_equal"; }
        else { t->prim = Prim::IntCmp; t->prim_id = "=="; }
        t->args = {fieldimm(it->idx, sv()), it->rhs};
        auto pf = mk(Lam::K::IfThenElse);
        pf->cond = t; pf->then_ = body; pf->else_ = exitL();
        body = pf;
      }
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = test; iff->then_ = body; iff->else_ = exitL();
      auto cat = mk(Lam::K::Catch); cat->cond = iff; cat->prim_arg = eid;
      cat->then_ = rest;
      return cat;
    }
    auto iff = mk(Lam::K::IfThenElse);
    iff->cond = test; iff->then_ = body; iff->else_ = rest;
    return iff;
  }

  // Exhaustive match over a purely-constant variant type -> (switch* ...).
  LamPtr const_switch(const LamPtr& scrut, const std::vector<Row>& rows) {
    std::string type;
    std::vector<Lam::SwitchCase> arms;
    for (auto& r : rows) {
      if (r.guard) return nullptr;
      auto it = ctor_info_.find(ctor_of(*r.lhs));
      if (it == ctor_info_.end() || it->second.is_block) return nullptr;
      auto* k = std::get_if<Ppat_construct>(&r.lhs->desc);
      if (k && k->arg) return nullptr;  // constant ctor must take no argument
      if (type.empty()) type = it->second.type;
      else if (type != it->second.type) return nullptr;
      arms.push_back({it->second.tag, expr(*r.rhs)});
    }
    auto t = type_ctors_.find(type);
    if (t == type_ctors_.end() || t->second.second != 0 ||
        (int)arms.size() != t->second.first)
      return nullptr;  // not exhaustive over a constant-only type
    std::sort(arms.begin(), arms.end(),
              [](auto& x, auto& y) { return x.tag < y.tag; });
    // Exactly two constant constructors (tags 0 and 1) -> a truthy `if`, like bool;
    // three or more -> a switch.
    if (arms.size() == 2 && arms[0].tag == 0 && arms[1].tag == 1) {
      auto i = mk(Lam::K::IfThenElse);
      i->cond = scrut; i->then_ = arms[1].body; i->else_ = arms[0].body;
      return i;
    }
    auto sw = mk(Lam::K::Switch);
    sw->cond = scrut;
    sw->sw_consts = std::move(arms);
    return sw;
  }
  // ===== The Switcher (a faithful port of lambda/switch.ml) ==================
  // switcher_match (below) is the entry point: for an integer- or char-constant
  // match with a trailing catch-all, it reproduces ocamlc's switch compilation
  // -- clustering dense runs into jump tables (`switch*`) joined by an optimal
  // tree of comparison / interval (`isout`) tests, with the default action
  // shared behind a `(catch .. with (N) default)` when it is reached from more
  // than one leaf.  It only takes over when ocamlc would emit at least one jump
  // table; pure if-chains are left to int_cases (which already matches them).

  // -- cost algebra (switch.ml:261-306) --
  static constexpr long long kTooMuch = (long long)1 << 60;  // sentinel; never add_test'd
  static bool less_tests(const Ctests& a, const Ctests& b) {
    return a.n < b.n || (a.n == b.n && a.ni < b.ni);
  }
  static bool eq_tests(const Ctests& a, const Ctests& b) { return a.n == b.n && a.ni == b.ni; }
  static bool less2tests(const std::pair<Ctests, Ctests>& a, const std::pair<Ctests, Ctests>& b) {
    return eq_tests(a.first, b.first) ? less_tests(a.second, b.second) : less_tests(a.first, b.first);
  }
  static void add_test(Ctests& t, const Ctests& o) { t.n += o.n; t.ni += o.ni; }

  // coupe i: split at i -> (lo of cases[i], cases[0..i), cases[i..end))
  static void coupe(const std::vector<SwCase>& c, int i, long long& lim,
                    std::vector<SwCase>& left, std::vector<SwCase>& right) {
    lim = c[i].lo;
    left.assign(c.begin(), c.begin() + i);
    right.assign(c.begin() + i, c.end());
  }
  // case_append (switch.ml:328): concatenate two interval arrays, merging the
  // junction (shared action, or absorbing a degenerate boundary interval).
  static std::vector<SwCase> case_append(const std::vector<SwCase>& c1, const std::vector<SwCase>& c2) {
    size_t l1 = c1.size(), l2 = c2.size();
    if (l1 == 0) return c2;
    if (l2 == 0) return c1;
    SwCase a = c1.back(), b = c2.front();
    std::vector<SwCase> r;
    if (a.act == b.act) {
      r = c1; r.pop_back();
      long long lo = (l1 < 2) ? a.lo : std::min(r[l1 - 2].hi + 1, a.lo);
      long long hi = (l2 < 2) ? b.hi : std::max(b.hi, c2[1].lo - 1);
      r.push_back({lo, hi, a.act});
      for (size_t i = 1; i < l2; ++i) r.push_back(c2[i]);
      return r;
    } else if (a.hi > a.lo) {
      r = c1; r.back() = {a.lo, b.lo - 1, a.act};
      for (auto& x : c2) r.push_back(x);
      return r;
    } else if (b.hi > b.lo) {
      r = c1; r.push_back({a.hi + 1, b.hi, b.act});
      for (size_t i = 1; i < l2; ++i) r.push_back(c2[i]);
      return r;
    } else {
      r = c1; for (auto& x : c2) r.push_back(x); return r;
    }
  }
  // coupe_inter i j: carve out the closed interval [i,j] as `inside`, the rest as `outside`.
  static void coupe_inter(int i, int j, const std::vector<SwCase>& c, long long& low, long long& high,
                          std::vector<SwCase>& inside, std::vector<SwCase>& outside) {
    low = c[i].lo; high = c[j].hi;
    inside.assign(c.begin() + i, c.begin() + j + 1);
    std::vector<SwCase> a(c.begin(), c.begin() + i), b(c.begin() + j + 1, c.end());
    outside = case_append(a, b);
  }
  static bool same_act(const std::vector<SwCase>& c) {
    for (size_t i = 0; i + 1 < c.size(); ++i) if (c[i].act != c.back().act) return false;
    return true;
  }
  // make_key (switch.ml:409): a canonical string keyed only by the case-array
  // *shape* (value vs interval, action-equalities normalized by first-appearance,
  // gaps marked) -- structurally-identical sub-problems share a memo entry.
  static std::string make_key_str(const std::vector<SwCase>& c) {
    std::vector<int> seen; int count = 0;
    auto got_it = [&](int act) {
      for (size_t i = 0; i < seen.size(); ++i) if (seen[i] == act) return (int)i;  // (act,index)
      seen.push_back(act); return count++;
    };
    auto make_one = [&](const SwCase& s) {
      return std::string(s.lo == s.hi ? "V" : "I") + std::to_string(got_it(s.act));
    };
    std::string r;
    int n = (int)c.size();
    r = make_one(c[n - 1]);
    long long pl = c[n - 1].lo;
    for (int i = n - 2; i >= 0; --i) {
      if (pl == c[i].hi + 1) r += "|" + make_one(c[i]);
      else r += "|E|" + make_one(c[i]);
      pl = c[i].lo;
    }
    return r;
  }

  // opt_count (switch.ml:474): the memoized recursive cost optimizer; returns the
  // chosen test tactic and its (worst-case, total) cost.
  OptRes opt_count(const std::vector<SwCase>& c) {
    std::string key = make_key_str(c);
    auto it = sw_memo_.find(key);
    if (it != sw_memo_.end()) return it->second;
    OptRes r;
    int lc = (int)c.size();
    if (same_act(c)) r = {{TR::No}, {{0, 0}, {0, 0}}};
    else if (lc < 8) r = sw_enum(c);
    else if (lc < 16) r = sw_heuristic(c);
    else r = sw_divide(c);
    sw_memo_[key] = r;
    return r;
  }
  OptRes sw_divide(const std::vector<SwCase>& c) {
    int lc = (int)c.size(), m = lc / 2;
    long long lim; std::vector<SwCase> left, right; coupe(c, m, lim, left, right);
    Ctests ci{1, 0}, cm{1, 0};
    auto pl = opt_count(left).second, pr = opt_count(right).second;
    add_test(ci, pl.second); add_test(ci, pr.second);
    add_test(cm, less_tests(pl.first, pr.first) ? pr.first : pl.first);
    return {{TR::Sep, m}, {cm, ci}};
  }
  OptRes sw_heuristic(const std::vector<SwCase>& c) {
    int lc = (int)c.size();
    auto sep = sw_divide(c);  // (Sep m, csep)
    TRet inter{TR::Inter, -1, -1}; std::pair<Ctests, Ctests> cinter{{kTooMuch, kTooMuch}, {kTooMuch, kTooMuch}};
    if (sw_ok_inter_ && c[0].act == c[lc - 1].act) {
      long long low, high; std::vector<SwCase> inside, outside;
      coupe_inter(1, lc - 2, c, low, high, inside, outside);
      auto pi = opt_count(inside).second, po = opt_count(outside).second;
      Ctests cmij{1, low == high ? 0 : 1}, cij{1, low == high ? 0 : 1};
      add_test(cij, pi.second); add_test(cij, po.second);
      add_test(cmij, less_tests(pi.first, po.first) ? po.first : pi.first);
      inter = {TR::Inter, 1, lc - 2}; cinter = {cmij, cij};
    }
    if (less2tests(sep.second, cinter)) return sep;
    return {inter, cinter};
  }
  OptRes sw_enum(const std::vector<SwCase>& c) {
    int lc = (int)c.size();
    int lim = -1; std::pair<Ctests, Ctests> best{{kTooMuch, kTooMuch}, {kTooMuch, kTooMuch}};
    for (int i = 1; i <= lc - 1; ++i) {
      long long l; std::vector<SwCase> left, right; coupe(c, i, l, left, right);
      Ctests ci{1, 0}, cm{1, 0};
      auto pl = opt_count(left).second, pr = opt_count(right).second;
      add_test(ci, pl.second); add_test(ci, pr.second);
      add_test(cm, less_tests(pl.first, pr.first) ? pr.first : pl.first);
      if (less2tests({cm, ci}, best)) { lim = i; best = {cm, ci}; }
    }
    auto with_sep = best;
    int ilow = -1, ihigh = -1; std::pair<Ctests, Ctests> winter{{kTooMuch, kTooMuch}, {kTooMuch, kTooMuch}};
    auto try_inter = [&](int i, int j) {
      long long low, high; std::vector<SwCase> inside, outside;
      coupe_inter(i, j, c, low, high, inside, outside);
      auto pi = opt_count(inside).second, po = opt_count(outside).second;
      Ctests cmij{1, low == high ? 0 : 1}, cij{1, low == high ? 0 : 1};
      add_test(cij, pi.second); add_test(cij, po.second);
      add_test(cmij, less_tests(pi.first, po.first) ? po.first : pi.first);
      if (less2tests({cmij, cij}, winter)) { ilow = i; ihigh = j; winter = {cmij, cij}; }
    };
    if (!sw_ok_inter_) {
      for (int i = 1; i <= lc - 2; ++i) if (c[i].lo == c[i].hi) try_inter(i, i);
    } else {
      for (int i = 1; i <= lc - 2; ++i) for (int j = i; j <= lc - 2; ++j) try_inter(i, j);
    }
    TRet r{TR::Inter, ilow, ihigh}; auto rc = winter;
    if (less2tests(with_sep, rc)) { r = {TR::Sep, lim}; rc = with_sep; }
    return {r, rc};
  }

  // -- dense / clustering (switch.ml:811-874) --
  static bool particular_case(const std::vector<SwCase>& c, int i, int j) {
    return j - i == 2 && c[i].lo + 1 == c[i + 1].lo && c[i + 1].lo + 1 == c[i + 2].lo &&
           c[i + 2].lo == c[i + 2].hi && c[i].act != c[i + 2].act;
  }
  long long approx_count(const std::vector<SwCase>& c, int i, int j) {
    int l = j - i + 1;
    if (l < 8) {
      std::vector<SwCase> sub(c.begin() + i, c.begin() + j + 1);
      return opt_count(sub).second.second.n;  // ci.n
    }
    return l - 1;
  }
  bool dense(const std::vector<SwCase>& c, int i, int j) {
    if (i == j) return true;
    long long l = c[i].lo, h = c[j].hi;
    long long ntests = approx_count(c, i, j);
    return particular_case(c, i, j) ||
           (ntests >= 3 && (double)ntests + 1.0 >= 0.33333 * ((double)h - (double)l + 1.0));
  }
  // comp_clusters (switch.ml:856): DP for the minimum number of dense clusters.
  void comp_clusters(const std::vector<SwCase>& c, std::vector<int>& k) {
    int len = (int)c.size();
    std::vector<long long> mc(len, kTooMuch);
    k.assign(len, 0);
    auto get_min = [&](int i) -> long long { return i < 0 ? 0 : mc[i]; };
    for (int i = 0; i < len; ++i)
      for (int j = 0; j <= i; ++j)
        if (dense(c, j, i) && get_min(j - 1) + 1 < mc[i]) { k[i] = j; mc[i] = get_min(j - 1) + 1; }
  }

  // make_switch (switch.ml:879): a jump table over [ll,hh].  Gap/default slots
  // (act 0) emit a fresh default sentinel; value slots emit the action term.
  ActFn make_switch_act(const std::vector<SwCase>& c, int i, int j,
                        const std::vector<LamPtr>& actions) {
    long long ll = c[i].lo, hh = c[j].hi;
    int len = (int)(hh - ll + 1);
    std::vector<int> tbl(len, 0);
    for (int kk = i; kk <= j; ++kk)
      for (long long v = c[kk].lo; v <= c[kk].hi; ++v) tbl[v - ll] = c[kk].act;
    return [this, ll, len, tbl, &actions](const SwCtx& ctx) -> LamPtr {
      long long off2 = -ll - ctx.off;
      LamPtr arg; Ident sv;
      if (off2 == 0) arg = ctx.arg;
      else { sv = fresh("switcher"); arg = varof(sv); }
      auto sw = mk(Lam::K::Switch); sw->cond = arg;
      for (int kk = 0; kk < len; ++kk)
        sw->sw_consts.push_back({kk, clone_or_leaf(tbl[kk], actions)});
      if (off2 == 0) return sw;
      auto offn = mk(Lam::K::Prim); offn->prim = Prim::Offsetint;
      offn->prim_arg = (int)off2; offn->args = {ctx.arg};
      auto l = mk(Lam::K::Let);
      l->bindings = {{sv, ValueKind::Gen, offn, /*alias=*/true}};
      l->body = sw; return l;
    };
  }
  LamPtr clone_or_leaf(int act, const std::vector<LamPtr>& actions) {
    if (act == 0) { auto e = mk(Lam::K::Staticraise); e->prim_arg = kDefaultLeaf; return e; }
    return actions[act];
  }
  // make_clusters (switch.ml:916): turn the cluster choice `k` into a new case
  // array whose actions are either a singleton's term or a make_switch closure.
  void make_clusters(const std::vector<SwCase>& c, const std::vector<int>& k,
                     const std::vector<LamPtr>& actions, std::vector<SwCase>& out_cases,
                     std::vector<ActFn>& out_acts, bool& made_switch) {
    std::unordered_map<int, int> single_idx;  // dedup singleton actions by orig act
    auto get_index = [&](int act) {
      auto it = single_idx.find(act);
      if (it != single_idx.end()) return it->second;
      int idx = (int)out_acts.size();
      out_acts.push_back([this, act, &actions](const SwCtx&) { return clone_or_leaf(act, actions); });
      single_idx[act] = idx; return idx;
    };
    std::vector<SwCase> rev;
    int j = (int)c.size() - 1;
    while (true) {
      int i = k[j];
      if (i == j) rev.push_back({c[i].lo, c[i].hi, get_index(c[i].act)});
      else {
        made_switch = true;
        int idx = (int)out_acts.size();
        out_acts.push_back(make_switch_act(c, i, j, actions));
        rev.push_back({c[i].lo, c[j].hi, idx});
      }
      if (i <= 0) break;
      j = i - 1;
    }
    out_cases.assign(rev.rbegin(), rev.rend());
  }

  // -- code generation: the test tree (switch.ml:672-808) --
  LamPtr sw_if(const LamPtr& cond, const LamPtr& ifso, const LamPtr& ifnot) {
    auto i = mk(Lam::K::IfThenElse); i->cond = cond; i->then_ = ifso; i->else_ = ifnot; return i;
  }
  LamPtr sw_cmp(const std::string& op, const LamPtr& arg, long long i) {
    auto p = mk(Lam::K::Prim); p->prim = Prim::IntCmp; p->prim_id = op;
    p->args = {arg, cint(i)}; return p;
  }
  LamPtr make_if_lt(const LamPtr& a, long long i, const LamPtr& s, const LamPtr& n) {
    return i == 1 ? sw_if(sw_cmp("<=", a, 0), s, n) : sw_if(sw_cmp("<", a, i), s, n);
  }
  LamPtr make_if_ge(const LamPtr& a, long long i, const LamPtr& s, const LamPtr& n) {
    return i == 1 ? sw_if(sw_cmp(">", a, 0), s, n) : sw_if(sw_cmp(">=", a, i), s, n);
  }
  LamPtr make_if_eq(const LamPtr& a, long long i, const LamPtr& s, const LamPtr& n) { return sw_if(sw_cmp("==", a, i), s, n); }
  LamPtr make_if_ne(const LamPtr& a, long long i, const LamPtr& s, const LamPtr& n) { return sw_if(sw_cmp("!=", a, i), s, n); }
  // `(if arg ifso ifnot)` -- bytecode's make_is_nonzero / arg_as_test are the identity.
  LamPtr make_if_bool(const LamPtr& a, const LamPtr& s, const LamPtr& n) { return sw_if(a, s, n); }
  LamPtr isout_node(long long d, const LamPtr& arg) {
    auto p = mk(Lam::K::Prim); p->prim = Prim::IntCmp; p->prim_id = "isout";
    p->args = {cint(d), arg}; return p;  // prints `(isout d arg)`
  }
  // make_if_out / make_if_in (switch.ml:701-729): interval test, binding a shifted
  // `switcher` alias when the lower bound l != 0 so nested tests reuse it.
  LamPtr make_if_io(bool is_in, const SwCtx& ctx, long long l, long long d,
                    const std::function<LamPtr(const SwCtx&)>& mk_so,
                    const std::function<LamPtr(const SwCtx&)>& mk_no) {
    auto mk_test = [&](long long dd, const LamPtr& arg, const LamPtr& so, const LamPtr& no) {
      LamPtr t = isout_node(dd, arg);
      if (is_in) { auto nt = mk(Lam::K::Prim); nt->prim = Prim::IntCmp; nt->prim_id = "not"; nt->args = {t}; t = nt; }
      return sw_if(t, so, no);
    };
    if (l == 0) return mk_test(d, ctx.arg, mk_so(ctx), mk_no(ctx));
    Ident sv = fresh("switcher");
    SwCtx c2{-l + ctx.off, varof(sv)};
    auto offn = mk(Lam::K::Prim); offn->prim = Prim::Offsetint; offn->prim_arg = (int)(-l); offn->args = {ctx.arg};
    auto inner = mk_test(d, c2.arg, mk_so(c2), mk_no(c2));
    auto let = mk(Lam::K::Let);
    let->bindings = {{sv, ValueKind::Gen, offn, /*alias=*/true}};
    let->body = inner; return let;
  }
  LamPtr c_test(const SwCtx& ctx, const std::vector<SwCase>& cases, const std::vector<ActFn>& actions) {
    int lc = (int)cases.size();
    if (lc == 1) return actions[cases[0].act](ctx);
    auto w = opt_count(cases).first;
    if (w.k == TR::No) return actions[cases[0].act](ctx);
    if (w.k == TR::Inter) {
      long long low, high; std::vector<SwCase> inside, outside;
      coupe_inter((int)w.i, (int)w.j, cases, low, high, inside, outside);
      Ctests cinside = opt_count(inside).second.first, coutside = opt_count(outside).second.first;
      if (low == high) {
        if (less_tests(coutside, cinside))
          return make_if_eq(ctx.arg, low + ctx.off, c_test(ctx, inside, actions), c_test(ctx, outside, actions));
        return make_if_ne(ctx.arg, low + ctx.off, c_test(ctx, outside, actions), c_test(ctx, inside, actions));
      }
      auto in_fn = [&](const SwCtx& c) { return c_test(c, inside, actions); };
      auto out_fn = [&](const SwCtx& c) { return c_test(c, outside, actions); };
      if (less_tests(coutside, cinside)) return make_if_io(true, ctx, low + ctx.off, high - low, in_fn, out_fn);
      return make_if_io(false, ctx, low + ctx.off, high - low, out_fn, in_fn);
    }
    // Sep i
    long long lim; std::vector<SwCase> left, right; coupe(cases, (int)w.i, lim, left, right);
    Ctests cleft = opt_count(left).second.first, cright = opt_count(right).second.first;
    if (w.i == 1 && (lim + ctx.off) == 1 && cases[0].lo + ctx.off == 0) {
      // both make_if_bool and make_if_nonzero are `(if arg ..)` in bytecode
      return make_if_bool(ctx.arg, c_test(ctx, right, actions), c_test(ctx, left, actions));
    } else if (less_tests(cright, cleft))
      return make_if_lt(ctx.arg, lim + ctx.off, c_test(ctx, left, actions), c_test(ctx, right, actions));
    return make_if_ge(ctx.arg, lim + ctx.off, c_test(ctx, right, actions), c_test(ctx, left, actions));
  }

  // -- default sharing (abstract_shared / PR#11893): if the default is reached
  // from >=2 leaves, share it behind one `(catch .. with (N) default)`. --
  static int count_default_leaves(const LamPtr& l) {
    if (!l) return 0;
    if (l->k == Lam::K::Staticraise && l->prim_arg == kDefaultLeaf && l->args.empty()) return 1;
    int c = count_default_leaves(l->fn) + count_default_leaves(l->body) +
            count_default_leaves(l->cond) + count_default_leaves(l->then_) +
            count_default_leaves(l->else_) + count_default_leaves(l->sw_default);
    for (auto& a : l->args) c += count_default_leaves(a);
    for (auto& b : l->bindings) c += count_default_leaves(b.val);
    for (auto& sc : l->sw_consts) c += count_default_leaves(sc.body);
    for (auto& sc : l->sw_blocks) c += count_default_leaves(sc.body);
    return c;
  }
  // Inline the single default leaf (replace it in place), or set every default
  // leaf to `(exit eid)`.  Returns whether any leaf was a default sentinel.
  static void rewrite_default_leaves(const LamPtr& l, int eid, const LamPtr& inline_with) {
    if (!l) return;
    if (l->k == Lam::K::Staticraise && l->prim_arg == kDefaultLeaf && l->args.empty()) {
      if (inline_with) *l = *inline_with; else l->prim_arg = eid;
      return;
    }
    rewrite_default_leaves(l->fn, eid, inline_with); rewrite_default_leaves(l->body, eid, inline_with);
    rewrite_default_leaves(l->cond, eid, inline_with); rewrite_default_leaves(l->then_, eid, inline_with);
    rewrite_default_leaves(l->else_, eid, inline_with); rewrite_default_leaves(l->sw_default, eid, inline_with);
    for (auto& a : l->args) rewrite_default_leaves(a, eid, inline_with);
    for (auto& b : l->bindings) rewrite_default_leaves(b.val, eid, inline_with);
    for (auto& sc : l->sw_consts) rewrite_default_leaves(sc.body, eid, inline_with);
    for (auto& sc : l->sw_blocks) rewrite_default_leaves(sc.body, eid, inline_with);
  }

  // A conservative structural key for an action body, used only to detect when
  // ocamlc would *share* two equal bodies in a switch (which we don't model) so
  // we can bail to int_cases.  Returns "" for terms ocamlc's make_key rejects
  // (functions/letrec/loops) -- those are never shared, so never trigger a bail.
  static std::string make_lam_key(const LamPtr& l) {
    if (!l) return "_";
    using K = Lam::K;
    switch (l->k) {
      case K::Function: case K::Letrec: case K::For: case K::While: return "";
      case K::Var: return "v" + l->var.name + "#" + std::to_string(l->var.stamp);
      case K::Mutvar: return "m" + l->var.name + "#" + std::to_string(l->var.stamp);
      case K::ConstInt: return "i" + std::to_string(l->int_val);
      case K::ConstChar: return "c" + std::to_string(l->int_val);
      case K::ConstFloat: return "f" + l->str_val;
      case K::ConstString: return "s" + l->str_val;
      default: break;
    }
    std::string r = "(" + std::to_string((int)l->k);
    if (l->k == K::Prim) r += ":" + std::to_string((int)l->prim) + ":" + l->prim_id + ":" + std::to_string(l->prim_arg);
    auto add = [&](const LamPtr& c) { if (c) { std::string k = make_lam_key(c); if (k.empty()) { r = ""; } else if (!r.empty()) r += " " + k; } };
    add(l->fn); add(l->cond); add(l->then_); add(l->else_); add(l->body); add(l->sw_default);
    for (auto& a : l->args) add(a);
    for (auto& b : l->bindings) { if (!r.empty()) r += " b" + std::to_string(b.id.stamp); add(b.val); }
    for (auto& sc : l->sw_consts) { if (!r.empty()) r += " C" + std::to_string(sc.tag); add(sc.body); }
    for (auto& sc : l->sw_blocks) { if (!r.empty()) r += " B" + std::to_string(sc.tag); add(sc.body); }
    if (r.empty()) return "";
    return r + ")";
  }
  // Entry point: an int/char-constant match with a trailing catch-all.  Returns
  // null (deferring to int_cases) unless at least one jump table is generated --
  // i.e. unless ocamlc would emit a switch rather than a plain if-chain.
  LamPtr switcher_match(const LamPtr& scrut, const std::vector<Row>& rows) {
    if (scrut->k != Lam::K::Var) return nullptr;
    struct KV { long long v; const Expression* rhs; };
    std::vector<KV> kvs;
    const Row* dflt = nullptr;
    bool is_int = false, is_char = false;
    for (auto& r : rows) {
      if (r.guard) return nullptr;
      const Pattern* p = effective_pat(r.lhs);
      if (auto* pc = std::get_if<Ppat_constant>(&p->desc)) {
        if (dflt) return nullptr;  // a case after the catch-all
        if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc)) {
          if (pi->suffix) return nullptr;  // boxed-int literal
          is_int = true; kvs.push_back({parse_ocaml_int(pi->value), r.rhs});
        } else if (auto* ch = std::get_if<Pconst_char>(&pc->c.desc)) {
          is_char = true; kvs.push_back({(unsigned char)ch->code, r.rhs});
        } else return nullptr;  // string/float
      } else if (is_catchall(*p) && !dflt && &r == &rows.back()) {
        dflt = &r;
      } else return nullptr;
    }
    if ((is_int && is_char) || !dflt || kvs.size() < 2) return nullptr;
    std::sort(kvs.begin(), kvs.end(), [](auto& a, auto& b) { return a.v < b.v; });
    for (size_t i = 1; i < kvs.size(); ++i)
      if (kvs[i].v == kvs[i - 1].v) return nullptr;  // duplicate value: bail

    // Translate the case bodies (source order) and the default body once.  If
    // any two bodies are structurally identical, ocamlc would *share* them in
    // the switch (which we don't model); bail so int_cases stays exact.
    std::vector<LamPtr> actions(1);  // index 0 = default (emitted via sentinel)
    std::vector<std::string> body_keys;
    actions.resize(kvs.size() + 1);
    std::vector<int> act_of(kvs.size());
    // bodies must be translated in source order for stable stamp normalization
    std::vector<const Expression*> by_src;
    for (auto& r : rows) if (!r.guard) { const Pattern* p = effective_pat(r.lhs);
      if (std::get_if<Ppat_constant>(&p->desc)) by_src.push_back(r.rhs); }
    std::unordered_map<const Expression*, int> src_idx;
    for (size_t i = 0; i < kvs.size(); ++i) {
      // find this kv's source position -> action index (1-based, source order)
      int pos = 0; for (size_t s = 0; s < by_src.size(); ++s) if (by_src[s] == kvs[i].rhs) { pos = (int)s; break; }
      act_of[i] = pos + 1;
    }
    for (size_t s = 0; s < by_src.size(); ++s) {
      LamPtr b = expr(*by_src[s]);
      std::string key = make_lam_key(b);
      for (auto& bk : body_keys) if (bk == key && !key.empty()) return nullptr;  // shared body
      body_keys.push_back(key);
      actions[s + 1] = b;
    }
    scope.emplace_back();
    bind_catchall(*dflt->lhs, scrut);
    LamPtr default_body = expr(*dflt->rhs);
    scope.pop_back();
    {  // a case body equal to the default is folded into the default by ocamlc
      std::string dk = make_lam_key(default_body);
      if (!dk.empty()) for (auto& bk : body_keys) if (bk == dk) return nullptr;
    }

    // Build the interval cover (matching.ml as_interval_canfail, distinct values).
    const long long LOW = is_char ? 0 : (LLONG_MIN / 4);
    const long long HIGH = is_char ? 255 : (LLONG_MAX / 4);
    std::vector<SwCase> cases;
    long long firstv = kvs.front().v, lastv = kvs.back().v;
    if (LOW < firstv) cases.push_back({LOW, firstv - 1, 0});
    for (size_t i = 0; i < kvs.size(); ++i) {
      cases.push_back({kvs[i].v, kvs[i].v, act_of[i]});
      if (i + 1 < kvs.size() && kvs[i + 1].v > kvs[i].v + 1)
        cases.push_back({kvs[i].v + 1, kvs[i + 1].v - 1, 0});
    }
    if (lastv < HIGH) cases.push_back({lastv + 1, HIGH, 0});

    sw_ok_inter_ = std::llabs(firstv) <= (1 << 16) && std::llabs(lastv) <= (1 << 16);
    sw_memo_.clear();
    std::vector<int> k; comp_clusters(cases, k);
    std::vector<SwCase> cl_cases; std::vector<ActFn> cl_acts; bool made_switch = false;
    make_clusters(cases, k, actions, cl_cases, cl_acts, made_switch);
    if (!made_switch) return nullptr;  // ocamlc emits an if-chain -> let int_cases match

    LamPtr tree = c_test({0, scrut}, cl_cases, cl_acts);
    int nd = count_default_leaves(tree);
    if (nd == 0) return tree;
    if (nd == 1) { rewrite_default_leaves(tree, 0, default_body); return tree; }
    int eid = ++next_exit_;
    rewrite_default_leaves(tree, eid, nullptr);
    auto cat = mk(Lam::K::Catch); cat->cond = tree; cat->prim_arg = eid; cat->then_ = default_body;
    return cat;
  }
  // A catch-all `n -> ...` binds n to the scrutinee (which, for a var scrutinee,
  // is just an alias to its binder).
  void bind_catchall(const Pattern& p, const LamPtr& scrut) {
    if (auto* pv = std::get_if<Ppat_var>(&p.desc))
      if (scrut->k == Lam::K::Var) scope.back()[pv->name.txt] = scrut->var;
  }
  LamPtr int_cases(const LamPtr& scrut, const std::vector<Row>& rows, size_t i,
                   bool strict = false) {
    if (i >= rows.size()) return cint(0);
    const Row& r = rows[i];
    // Peel `<inner> as x` aliases: x binds to the scrutinee value over this row's
    // guard and body.  Bound by scope to the scrutinee variable (as simplif also
    // does); a non-variable scrutinee skips the alias (best-effort, the test still
    // emits).  A row-local scope frame keeps x out of the later rows.
    const Pattern* lhs = r.lhs;
    std::vector<std::string> aliases;
    while (auto* pa = std::get_if<Ppat_alias>(&lhs->desc)) { aliases.push_back(pa->name.txt); lhs = pa->p.get(); }
    bool framed = !aliases.empty() && scrut->k == Lam::K::Var;
    auto enter = [&] { if (framed) { scope.emplace_back(); for (auto& nm : aliases) scope.back()[nm] = scrut->var; } };
    auto leave = [&] { if (framed) scope.pop_back(); };
    // A guard on a catch-all (var/`_`) pattern inlines: the pattern always matches,
    // so guard-failure just falls through to the rest -> `(if guard body <rest>)`.
    // (A guard on a constant/ctor pattern would share <rest> across pattern- and
    // guard-failure, needing the matcher's catch/exit; left to the best-effort tail.)
    if (r.guard && is_catchall(*lhs)) {
      LamPtr rest = int_cases(scrut, rows, i + 1, strict);
      if (!rest) return nullptr;
      enter(); bind_catchall(*lhs, scrut);
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = expr(*r.guard); iff->then_ = expr(*r.rhs);
      leave();
      iff->else_ = rest;
      return iff;
    }
    if (!r.guard && (is_catchall(*lhs) || (!strict && i + 1 == rows.size()))) {
      // strict mode: a non-catchall final row binds nothing here -- null, so
      // the caller's correct fallback (naive_match) takes over
      enter(); bind_catchall(*lhs, scrut);
      LamPtr b = expr(*r.rhs); leave();
      return b;
    }
    // An integer literal, or a constant constructor (matched by its integer tag),
    // tested against the scrutinee with the rest of the rows as the fall-through.
    if (!r.guard) {
      bool isint = false, ctor = false; long long val = 0;
      if (auto* pc = std::get_if<Ppat_constant>(&lhs->desc)) {
        if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc)) { isint = true; val = parse_ocaml_int(pi->value); }
      } else if (auto* k = std::get_if<Ppat_construct>(&lhs->desc); k && !k->arg) {
        auto it = ctor_info_.find(ctor_of(*lhs));
        if (it != ctor_info_.end() && !it->second.is_block) { isint = true; val = it->second.tag; ctor = true; }
      }
      if (isint) {
        LamPtr rest = int_cases(scrut, rows, i + 1, strict);
        if (!rest) return nullptr;
        auto iff = mk(Lam::K::IfThenElse);
        if (ctor && val == 0) {
          iff->cond = scrut;  // constant ctor of tag 0: a truthy test (`!= 0` is identity)
        } else {
          auto ne = mk(Lam::K::Prim); ne->prim = Prim::NotEqInt; ne->args = {scrut, cint(val)};
          iff->cond = ne;
        }
        iff->then_ = rest;
        enter(); iff->else_ = expr(*r.rhs); leave();
        return iff;
      }
    }
    if (strict) return nullptr;  // unsupported: let naive_match handle it correctly
    return expr(*r.rhs);  // unsupported pattern: best-effort
  }

  // ---- last-resort, always-correct match: per-row test chains -------------
  // Each row compiles to its own test + binders + (guarded) body; failures fall
  // to the next row (guard/test failures share the rest behind a catch/exit).
  // Tests duplicate work the matrix compiler would share -- correctness first,
  // so unsupported shapes never silently collapse to a wrong arm again.
  LamPtr if_and(const LamPtr& a, const LamPtr& b) {
    if (!a) return b;
    if (!b) return a;
    auto i = mk(Lam::K::IfThenElse);
    i->cond = a; i->then_ = b; i->else_ = cint(0);
    return i;
  }
  // Does the pattern bind any variable (a Ppat_var / Ppat_alias anywhere)?
  bool pattern_binds(const Pattern* p0) {
    const Pattern* p = effective_pat(p0);
    if (std::holds_alternative<Ppat_var>(p->desc)) return true;
    if (std::holds_alternative<Ppat_alias>(p->desc)) return true;
    if (auto* tu = std::get_if<Ppat_tuple>(&p->desc)) {
      for (auto& e : tu->elems) if (pattern_binds(e.get())) return true; return false;
    }
    if (auto* k = std::get_if<Ppat_construct>(&p->desc)) return k->arg && pattern_binds(k->arg->get());
    if (auto* o = std::get_if<Ppat_or>(&p->desc)) return pattern_binds(o->l.get()) || pattern_binds(o->r.get());
    if (auto* pv = std::get_if<Ppat_variant>(&p->desc)) return pv->arg && pattern_binds(pv->arg->get());
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      for (auto& [l, s] : pr->fields) if (pattern_binds(s.get())) return true; return false;
    }
    if (auto* arr = std::get_if<Ppat_array>(&p->desc)) {
      for (auto& e : arr->elems) if (pattern_binds(e.get())) return true; return false;
    }
    if (auto* pz = std::get_if<Ppat_lazy>(&p->desc)) return pattern_binds(pz->p.get());
    return false;
  }
  // A choice of one child at each BINDING or-node, so a row with binding
  // or-alternatives (`(C|D) as x | E x | F(_,x)`) can be matched as several
  // or-free rows (each consults its choice in pat_test).  Non-binding or-nodes
  // are left to pat_test's OR, preserving their existing codegen.
  using Choices = std::unordered_map<const Pattern*, const Pattern*>;
  const Choices* cur_choices_ = nullptr;
  static constexpr size_t kOrExpandCap = 64;
  // Enumerate the or-free instantiations of a pattern's BINDING or-nodes.
  bool expand_pattern(const Pattern* p0, std::vector<Choices>& out) {
    const Pattern* p = effective_pat(p0);
    auto cartesian = [&](auto&& subs) -> bool {
      out = {Choices{}};
      for (auto* e : subs) {
        std::vector<Choices> ee;
        if (!expand_pattern(e, ee)) return false;
        std::vector<Choices> next;
        for (auto& a : out) for (auto& b : ee) {
          Choices m = a; for (auto& kv : b) m.insert(kv); next.push_back(std::move(m));
          if (next.size() > kOrExpandCap) return false;
        }
        out = std::move(next);
      }
      return true;
    };
    if (auto* o = std::get_if<Ppat_or>(&p->desc)) {
      if (!pattern_binds(p)) { out = {Choices{}}; return true; }  // non-binding: leaf
      out.clear();
      for (const Pattern* child : {o->l.get(), o->r.get()}) {
        std::vector<Choices> sub;
        if (!expand_pattern(child, sub)) return false;
        for (auto& s : sub) { s[p] = child; out.push_back(std::move(s)); }
        if (out.size() > kOrExpandCap) return false;
      }
      return true;
    }
    if (auto* tu = std::get_if<Ppat_tuple>(&p->desc)) {
      std::vector<const Pattern*> es; for (auto& e : tu->elems) es.push_back(e.get());
      return cartesian(es);
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      std::vector<const Pattern*> es; for (auto& [l, s] : pr->fields) es.push_back(s.get());
      return cartesian(es);
    }
    if (auto* arr = std::get_if<Ppat_array>(&p->desc)) {
      std::vector<const Pattern*> es; for (auto& e : arr->elems) es.push_back(e.get());
      return cartesian(es);
    }
    if (auto* k = std::get_if<Ppat_construct>(&p->desc))
      return k->arg ? expand_pattern(k->arg->get(), out) : (out = {Choices{}}, true);
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) return expand_pattern(pa->p.get(), out);
    if (auto* pv = std::get_if<Ppat_variant>(&p->desc))
      return pv->arg ? expand_pattern(pv->arg->get(), out) : (out = {Choices{}}, true);
    if (auto* pz = std::get_if<Ppat_lazy>(&p->desc)) return expand_pattern(pz->p.get(), out);
    out = {Choices{}};
    return true;
  }
  bool pat_test(const Pattern* p0, const LamPtr& acc, LamPtr& test,
                std::vector<std::pair<Ident, LamPtr>>& binds) {
    const Pattern* p = effective_pat(p0);
    if (std::holds_alternative<Ppat_any>(p->desc)) return true;
    if (auto* pv = std::get_if<Ppat_var>(&p->desc)) {
      Ident id = fresh(pv->name.txt);
      apply_var_node_path(p, id);  // a ctor-arg var: tag its record type
      binds.push_back({id, acc});
      return true;
    }
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) {
      binds.push_back({fresh(pa->name.txt), acc});
      return pat_test(pa->p.get(), acc, test, binds);
    }
    if (auto* up = std::get_if<Ppat_unpack>(&p->desc)) {  // `(module M [: S])`: bind M
      if (up->name.txt) {
        Ident id = fresh(*up->name.txt);
        binds.push_back({id, acc});
        module_ident_[*up->name.txt] = id;  // so `M.x` in a `when` guard / body resolves
        std::string mt;
        if (up->pkg && lid_to_dotted(up->pkg->path.txt, mt))
          register_pack_layouts(*up->name.txt, mt);
      }
      return true;  // always matches
    }
    if (auto* pc = std::get_if<Ppat_constant>(&p->desc)) {
      if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc)) {
        if (pi->suffix) return false;  // boxed-int literals: skip
        auto t = mk(Lam::K::Prim); t->prim = Prim::EqInt;
        t->args = {acc, cint(parse_ocaml_int(pi->value))};
        test = if_and(test, t);
        return true;
      }
      if (auto* pch = std::get_if<Pconst_char>(&pc->c.desc)) {
        auto t = mk(Lam::K::Prim); t->prim = Prim::EqInt;
        t->args = {acc, cchar((unsigned char)pch->code)};
        test = if_and(test, t);
        return true;
      }
      if (auto* ps = std::get_if<Pconst_string>(&pc->c.desc)) {
        auto sv = mk(Lam::K::ConstString); sv->str_val = ps->s;
        auto t = mk(Lam::K::Prim); t->prim = Prim::Ccall;
        t->prim_id = "caml_string_equal"; t->args = {acc, sv};
        test = if_and(test, t);
        return true;
      }
      return false;  // float literals: NaN semantics, skip
    }
    if (auto* tu = std::get_if<Ppat_tuple>(&p->desc)) {
      for (size_t i = 0; i < tu->elems.size(); ++i)
        if (!pat_test(tu->elems[i].get(), fieldimm((int)i, acc), test, binds))
          return false;
      return true;
    }
    if (auto* k = std::get_if<Ppat_construct>(&p->desc)) {
      std::string cn = ctor_of(*p);
      auto ci = ctor_info_.find(cn);
      if (ci == ctor_info_.end()) {
        // an EXTENSION constructor (exn_ident_/exn_field_): test the identity
        // (the value itself when nullary, else field 0), like ext_match -- so a
        // pattern containing one (`Some B`, `(Some A|Some B), A`) matches in the
        // multi-column naive matcher instead of bailing.
        if (exn_ident_.count(cn) || exn_field_.count(cn) ||
            is_predef_exn_name(cn) || module_ctor_identity(k->id.txt)) {
          int arity = exn_arity_.count(cn) ? exn_arity_[cn] : (k->arg ? 1 : 0);
          // a module-qualified extension constructor (`M1.E`) compares against
          // that module's own field, not the last same-named binder
          LamPtr ident = module_ctor_identity(k->id.txt);
          if (!ident) ident = exn_value(cn);
          auto t = mk(Lam::K::Prim); t->prim = Prim::IntCmp; t->prim_id = "==";
          t->args = {k->arg ? fieldimm(0, acc) : acc, ident};
          test = if_and(test, t);
          if (k->arg) {  // args at field 1.. (field 0 is the identity)
            auto* at = std::get_if<Ppat_tuple>(&effective_pat(k->arg->get())->desc);
            if (arity > 1 && at && (int)at->elems.size() == arity) {
              for (int i = 0; i < arity; ++i)
                if (!pat_test(at->elems[i].get(), fieldimm(i + 1, acc), test, binds)) return false;
            } else if (!pat_test(k->arg->get(), fieldimm(1, acc), test, binds)) return false;
          }
          return true;
        }
        return false;
      }
      auto tc = type_ctors_.find(ci->second.type);
      int nc = tc != type_ctors_.end() ? tc->second.first : -1;
      int nb = tc != type_ctors_.end() ? tc->second.second : -1;
      if (!ci->second.is_block) {
        LamPtr t;
        if (nb == 0) {  // constants only: a plain integer compare
          auto e2 = mk(Lam::K::Prim); e2->prim = Prim::EqInt;
          e2->args = {acc, cint(ci->second.tag)};
          t = e2;
        } else {
          auto ii = mk(Lam::K::Prim); ii->prim = Prim::IntCmp;
          ii->prim_id = "isint"; ii->args = {acc};
          auto e2 = mk(Lam::K::Prim); e2->prim = Prim::EqInt;
          e2->args = {acc, cint(ci->second.tag)};
          auto g = mk(Lam::K::IfThenElse);
          g->cond = ii; g->then_ = e2; g->else_ = cint(0);
          t = g;
        }
        test = if_and(test, t);
        return true;
      }
      // block ctor: tag test (caml_obj_tag of an immediate is out of range)
      if (!(nb == 1 && nc >= 0 && ci->second.tag == 0)) {
        auto tg = mk(Lam::K::Prim); tg->prim = Prim::Ccall;
        tg->prim_id = "caml_obj_tag"; tg->args = {acc};
        auto e2 = mk(Lam::K::Prim); e2->prim = Prim::EqInt;
        e2->args = {tg, cint(ci->second.tag)};
        test = if_and(test, e2);
      } else if (nc > 0) {  // sole block ctor of a mixed type: non-immediate test
        auto ii = mk(Lam::K::Prim); ii->prim = Prim::IntCmp;
        ii->prim_id = "isint"; ii->args = {acc};
        auto g = mk(Lam::K::IfThenElse);
        g->cond = ii; g->then_ = cint(0); g->else_ = cint(1);
        test = if_and(test, g);
      }
      if (k->arg) {
        // While matching this ctor's arguments, force-register each arg type's
        // constructors so a nested ambiguous ctor (`Mty_functor(Named(id,arg),res)`:
        // Named is functor_parameter@arity-2 here) resolves through the enclosing
        // ctor's declared arg type rather than the global last-registered one.
        CtorSave argsv;
        if (auto cap = ctor_arg_paths_.find(lid_last(k->id.txt)); cap != ctor_arg_paths_.end())
          for (auto& ap : cap->second) {
            auto s = force_register_type_ctors(ap);
            argsv.insert(argsv.end(), std::make_move_iterator(s.begin()),
                         std::make_move_iterator(s.end()));
          }
        struct G { Translator* self; CtorSave sv; ~G() { self->restore_ctors(sv); } }
          arg_guard{this, std::move(argsv)};
        if (ci->second.unboxed) return pat_test(k->arg->get(), acc, test, binds);
        auto* at = std::get_if<Ppat_tuple>(&(*k->arg)->desc);
        if (ci->second.arity > 1 && at && (int)at->elems.size() == ci->second.arity) {
          for (int i = 0; i < ci->second.arity; ++i)
            if (!pat_test(at->elems[i].get(), fieldimm(i, acc), test, binds))
              return false;
        } else if (!ci->second.rlabels.empty()) {
          // inline record (`Cons {next = Nil}`): the record's labels are the
          // constructor block's FLAT fields, so match the record pattern against
          // the block itself (acc), not `field 0 acc` -- else next reads
          // `field 1 (field 0 acc)` (Queue.take's wrong double field read).
          // A bare var arg (`Local x`) binds x to the WHOLE record; remember its
          // ctor so `x.stamp` resolves via rlabels (ambiguous labels).
          const Pattern* ap = effective_pat(k->arg->get());
          if (auto* pv = std::get_if<Ppat_var>(&ap->desc)) {
            Ident id = fresh(pv->name.txt);
            inline_rec_var_[id.stamp] = &ci->second;
            binds.push_back({id, acc});
          } else if (auto* pr = std::get_if<Ppat_record>(&ap->desc)) {
            // `Predef { stamp = s1; _ }`: match each label at its rlabels index of
            // the block directly -- find_field would bail on an AMBIGUOUS label
            // (Ident's `stamp`, shared by Local/Predef/Scoped).
            auto& L = ci->second.rlabels;
            for (auto& [lbl, sub] : pr->fields) {
              int ix = -1;
              for (size_t i2 = 0; i2 < L.size(); ++i2)
                if (L[i2] == lid_last(lbl.txt)) { ix = (int)i2; break; }
              if (ix < 0) return false;
              FieldInfo fi{ci->second.type, ix, ci->second.rfmut[ix], ci->second.rshape[ix]};
              if (!pat_test(sub.get(), field_read(&fi, acc), test, binds)) return false;
            }
          } else if (!pat_test(k->arg->get(), acc, test, binds)) return false;
        } else if (!pat_test(k->arg->get(), fieldimm(0, acc), test, binds)) {
          return false;
        }
      }
      return true;
    }
    if (auto* o = std::get_if<Ppat_or>(&p->desc)) {
      // a binding or-node was expanded into separate rows: follow this row's
      // chosen alternative.
      if (cur_choices_)
        if (auto it = cur_choices_->find(p); it != cur_choices_->end())
          return pat_test(it->second, acc, test, binds);
      // alternatives must bind nothing (per-alternative binds would need
      // per-branch bodies); tests OR together
      std::vector<std::pair<Ident, LamPtr>> b1, b2;
      LamPtr t1, t2;
      if (!pat_test(o->l.get(), acc, t1, b1) || !b1.empty()) return false;
      if (!pat_test(o->r.get(), acc, t2, b2) || !b2.empty()) return false;
      if (!t1 || !t2) return true;  // one side always matches
      auto g = mk(Lam::K::IfThenElse);
      g->cond = t1; g->then_ = cint(1); g->else_ = t2;
      test = if_and(test, g);
      return true;
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      std::vector<std::string> flds;
      for (auto& [lbl, sub] : pr->fields) flds.push_back(lid_last(lbl.txt));
      for (auto& [lbl, sub] : pr->fields) {
        FieldInfo nfi;
        const FieldInfo* fi = resolve_record_pat_field(flds, lbl.txt, nfi, p, pat_record_qual_mod(*pr));
        tag_nested_record_subpat(fi, lid_last(lbl.txt), sub.get());
        if (!fi) return false;
        if (!pat_test(sub.get(), field_read(fi, acc), test, binds)) return false;
      }
      return true;
    }
    if (auto* pv2 = std::get_if<Ppat_variant>(&p->desc)) {
      if (!pv2->arg) {
        auto t = mk(Lam::K::Prim); t->prim = Prim::EqInt;
        t->args = {acc, cint(hash_variant(pv2->label))};
        test = if_and(test, t);
        return true;
      }
      auto ii = mk(Lam::K::Prim); ii->prim = Prim::IntCmp;
      ii->prim_id = "isint"; ii->args = {acc};
      auto e2 = mk(Lam::K::Prim); e2->prim = Prim::EqInt;
      e2->args = {fieldimm(0, acc), cint(hash_variant(pv2->label))};
      auto g = mk(Lam::K::IfThenElse);
      g->cond = ii; g->then_ = cint(0); g->else_ = e2;
      test = if_and(test, g);
      return pat_test(pv2->arg->get(), fieldimm(1, acc), test, binds);
    }
    // A polymorphic-variant *type* pattern (`#lambda`): the scrutinee's tag (the
    // value itself when immediate, else field 0 of the block) must belong to the
    // named type's tag set.  Lets naive_match handle matches mixing `#type` rows
    // with explicit-tag rows (`#var as x | `Abs .. | `App ..`).
    if (auto* pt = std::get_if<Ppat_type>(&p->desc)) {
      std::set<long long> tags; std::set<std::string> seen;
      collect_pv_tags_lid(pt->id.txt, tags, seen);
      if (tags.empty()) return false;  // unknown polyvariant type
      auto tagof = [&]() -> LamPtr {
        auto ii = mk(Lam::K::Prim); ii->prim = Prim::IntCmp;
        ii->prim_id = "isint"; ii->args = {acc};
        auto tx = mk(Lam::K::IfThenElse);
        tx->cond = ii; tx->then_ = acc; tx->else_ = fieldimm(0, acc);
        return tx;
      };
      auto eq = [&](long long h) -> LamPtr {
        auto e = mk(Lam::K::Prim); e->prim = Prim::EqInt; e->args = {tagof(), cint(h)};
        return e;
      };
      std::vector<long long> hs(tags.begin(), tags.end());
      LamPtr mem = eq(hs.back());
      for (int i = (int)hs.size() - 2; i >= 0; --i) {  // (|| (== tag h) rest)
        auto o = mk(Lam::K::IfThenElse); o->cond = eq(hs[i]);
        o->then_ = cint(1); o->else_ = mem; mem = o;
      }
      test = if_and(test, mem);
      return true;
    }
    if (auto* pi = std::get_if<Ppat_interval>(&p->desc)) {
      // `lo..hi` (char or int range): acc >= lo && acc <= hi.  A single-point
      // range collapses to one equality.  This is correct (not byte-exact with
      // the Switcher's `isout`); the naive matcher's safety net handles it.
      auto bound = [](const Constant& c, long long& out) -> bool {
        if (auto* ch = std::get_if<Pconst_char>(&c.desc)) { out = (unsigned char)ch->code; return true; }
        if (auto* in = std::get_if<Pconst_integer>(&c.desc)) {
          if (in->suffix) return false; out = parse_ocaml_int(in->value); return true;
        }
        return false;
      };
      long long lo, hi;
      if (!bound(pi->c1, lo) || !bound(pi->c2, hi)) return false;
      if (lo > hi) std::swap(lo, hi);
      auto rel = [&](const char* op, long long v) {
        auto t = mk(Lam::K::Prim); t->prim = Prim::IntCmp; t->prim_id = op;
        t->args = {acc, cint(v)}; return t;
      };
      if (lo == hi) { test = if_and(test, rel("==", lo)); return true; }
      LamPtr rng = if_and(rel(">=", lo), rel("<=", hi));
      test = if_and(test, rng);
      return true;
    }
    if (auto* arr = std::get_if<Ppat_array>(&p->desc)) {
      // `[|p0;..;pn-1|]`: a length test (== n) gates per-element matches.
      // Generic array access (correct for every element kind, incl. flat
      // float arrays); feeds the naive matcher, so correctness not byte-parity.
      int n = (int)arr->elems.size();
      auto len = mk(Lam::K::Prim); len->prim = Prim::IntCmp;
      len->prim_id = "array.length[gen]"; len->args = {acc};
      auto lt = mk(Lam::K::Prim); lt->prim = Prim::EqInt; lt->args = {len, cint(n)};
      test = if_and(test, lt);  // length first: short-circuits the element gets
      for (int i = 0; i < n; ++i) {
        auto get = mk(Lam::K::Prim); get->prim = Prim::IntCmp;
        get->prim_id = "array.get[gen]"; get->args = {acc, cint(i)};
        if (!pat_test(arr->elems[i].get(), get, test, binds)) return false;
      }
      return true;
    }
    return false;  // lazy / unpack: unmodeled
  }
  LamPtr naive_match(const LamPtr& scrut0, const std::vector<Row>& rows0,
                     const Location& mloc) {
    LamPtr scrut = scrut0;
    Ident stmp;
    bool tempd = false;
    if (scrut->k != Lam::K::Var) {
      stmp = fresh("", true);
      tempd = true;
      scrut = varof(stmp);
    }
    struct NRow { const Pattern* lhs; const Expression* rhs; const Expression* guard;
                  std::vector<std::string> aliases; Choices choices; bool tcatch = false; };
    std::vector<NRow> rows;
    for (size_t ri = 0; ri < rows0.size(); ++ri) {
      auto& r = rows0[ri];
      const Pattern* l = effective_pat(r.lhs);
      std::vector<std::string> als;
      while (auto* pa = std::get_if<Ppat_alias>(&l->desc)) {
        als.push_back(pa->name.txt);
        l = effective_pat(pa->p.get());
      }
      // A `#type [as id]` pattern matches a subset of polyvariant tags.  When the
      // tag set is RESOLVABLE (a local abbreviation or, cross-module, via the
      // qualified `#Mod.type` cmi lookup) it compiles like any other testable row
      // -- pat_test dispatches on the tags and binds the alias -- so a non-last /
      // GUARDED `#type` arm works (matching.ml's `#Simple.view as view when ..`).
      // When the set is UNKNOWN it is only sound as the LAST (residual) arm, where
      // it binds its alias and runs without a test (earlier arms filtered first).
      if (auto* pt0 = std::get_if<Ppat_type>(&l->desc)) {
        std::set<long long> tags; std::set<std::string> seen;
        collect_pv_tags_lid(pt0->id.txt, tags, seen);
        if (tags.empty()) {
          if (ri + 1 != rows0.size()) return nullptr;
          rows.push_back({l, r.rhs, r.guard, als, {}, true});
          continue;
        }
        // known tags: fall through to the normal testable-row path below.
      }
      std::vector<const Pattern*> alts;
      flatten_or(l, alts);  // top-level or split (preserves non-binding-or codegen)
      for (auto* a : alts) {
        // expand any NESTED binding or-patterns (`(C|D) as x | E x`) into one row
        // per alternative, each carrying its choice for pat_test to follow.
        std::vector<Choices> choices;
        if (!expand_pattern(a, choices)) return nullptr;  // blowup -> bail
        for (auto& ch : choices) rows.push_back({a, r.rhs, r.guard, als, std::move(ch)});
      }
    }
    LamPtr chain = raise_predef("Match_failure", mloc);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      LamPtr test;
      std::vector<std::pair<Ident, LamPtr>> binds;
      if (!it->tcatch) {
        cur_choices_ = it->choices.empty() ? nullptr : &it->choices;
        bool ok = pat_test(it->lhs, scrut, test, binds);
        cur_choices_ = nullptr;
        if (!ok) return nullptr;
      }  // tcatch: test stays null (irrefutable residual), only aliases bind
      scope.emplace_back();
      for (auto& nm : it->aliases) scope.back()[nm] = scrut->var;
      for (auto& [id, acc] : binds) scope.back()[id.name] = id;
      if (it->guard) {
        int n = ++next_exit_;
        auto exitL = [&] {
          auto x = mk(Lam::K::Staticraise); x->prim_arg = n; return x;
        };
        auto gi = mk(Lam::K::IfThenElse);
        gi->cond = expr(*it->guard);
        gi->then_ = expr(*it->rhs);
        gi->else_ = exitL();
        LamPtr body = wrap_binders(gi, binds);
        if (test) {
          auto o = mk(Lam::K::IfThenElse);
          o->cond = test; o->then_ = body; o->else_ = exitL();
          body = o;
        }
        auto cat = mk(Lam::K::Catch);
        cat->cond = body; cat->prim_arg = n; cat->then_ = chain;
        chain = cat;
      } else {
        LamPtr body = wrap_binders(expr(*it->rhs), binds);
        if (!test) {
          chain = body;  // irrefutable row: the rest is unreachable
        } else {
          auto o = mk(Lam::K::IfThenElse);
          o->cond = test; o->then_ = body; o->else_ = chain;
          chain = o;
        }
      }
      scope.pop_back();
    }
    if (tempd) {
      auto l = mk(Lam::K::Let);
      l->bindings = {{stmp, ValueKind::Gen, scrut0}};
      l->body = chain;
      chain = l;
    }
    return chain;
  }

  // Eta-expand an optional-argument-erased value: `(let (arg = inner) (function
  // eta.. (apply arg <slots>)))`, where each `true` slot is a None (0) for an
  // erased optional and each `false` slot is one of the eta parameters.
  std::set<const Expression*> erasure_active_;
  LamPtr wrap_optional_erasure(LamPtr inner, const std::vector<bool>& slots) {
    Ident argid = fresh("arg");
    auto fn = mk(Lam::K::Function);
    std::vector<Ident> etas;
    for (bool none_slot : slots)
      if (!none_slot) { Ident e = fresh("eta"); etas.push_back(e); fn->params.push_back({e, ValueKind::Gen}); }
    auto ap = mk(Lam::K::Apply);
    auto argv = mk(Lam::K::Var); argv->var = argid; ap->fn = argv;
    size_t ei = 0;
    for (bool none_slot : slots) {
      if (none_slot) ap->args.push_back(mk(Lam::K::ConstInt));  // None == 0
      else { auto ev = mk(Lam::K::Var); ev->var = etas[ei++]; ap->args.push_back(ev); }
    }
    fn->body = ap;
    auto let = mk(Lam::K::Let);
    let->bindings = {{argid, ValueKind::Gen, inner}};
    let->body = fn;
    return let;
  }

  LamPtr expr(const Expression& e) {
    // Optional-argument erasure: a `?l:.. -> ..`-typed value used where a
    // non-optional arrow is expected is eta-expanded (None for the omitted
    // optional).  Re-enter once (guarded) to translate the inner value, wrap it.
    if (auto it = vk.optional_erasures.find(&e);
        it != vk.optional_erasures.end() && !erasure_active_.count(&e)) {
      erasure_active_.insert(&e);
      LamPtr inner = expr(e);
      erasure_active_.erase(&e);
      return wrap_optional_erasure(inner, it->second);
    }
    // The rec-RHS spine flag holds only along tail spines: take it, clear it,
    // and re-set it just before each spine-continuing body recursion below.
    bool rec_spine = rec_spine_;
    rec_spine_ = false;
    // `M.(body)` / `let open M in body`: resolve `body`'s unqualified names in M.
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) {
      if (auto* op = std::get_if<Pstr_open>(&si->item->desc)) {
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
          std::string dotted;  // a dotted submodule path opens under its full path
          if (!lid_to_dotted(mi->id.txt, dotted)) dotted = lid_last(mi->id.txt);
          // a bare local open of a submodule of an already-opened module
          // (`open Types` then `Variance.(contravariant, covariant)`): resolve it
          // to the full path so its members resolve.
          if (std::string p = opened_submodule_path(dotted); !p.empty()) dotted = p;
          if (dotted.find('.') != std::string::npos)
            submodule_of(dotted);  // eager: registers its record-type labels
          CtorSave ctor_save = open_shadow_ctors(dotted);  // ctors shadow in body
          opened_.push_back(dotted);
          rec_spine_ = rec_spine;
          LamPtr b = expr(*si->body);
          opened_.pop_back();
          restore_ctors(ctor_save);
          return b;
        }
        // `let open F(X) / struct..end / (M:S) in body`: bind open/N over body
        LamPtr mv = compile_module_expr(op->expr);
        auto rl = module_result_layout(op->expr);
        if (rl.empty()) rl = arg_layout(op->expr);
        std::string nm = "open#" + std::to_string(++open_gen_count_);
        Ident oid = fresh("open");
        module_ident_[nm] = oid;
        auto& lay = module_layout_[nm]; lay.clear();
        for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
        opened_.push_back(nm);
        rec_spine_ = rec_spine;
        LamPtr b = expr(*si->body);
        opened_.pop_back();
        module_ident_.erase(nm);
        module_layout_.erase(nm);
        auto l = mk(Lam::K::Let);
        l->bindings = {{oid, ValueKind::Gen, mv}};
        l->body = b;
        return l;
      }
      // `let module M = me in body`: bind M (value + layout), then the body.
      if (auto* pm = std::get_if<Pstr_module>(&si->item->desc))
        if (pm->binding.name.txt) {
          auto& mb = pm->binding;
          const std::string& nm = *mb.name.txt;
          // Entering module N: __FUNCTION__ inside is `<enclosing path>.N.fn`, and
          // the enclosing-function scope resets (N is a new module scope).
          std::string saved_mp = mod_path_;
          std::vector<std::string> saved_fp = func_path_;
          std::string cur_path = mod_path_;
          for (auto& fn : func_path_) { if (!cur_path.empty()) cur_path += "."; cur_path += fn; }
          mod_path_ = cur_path + (cur_path.empty() ? "" : ".") + nm;
          func_path_.clear();
          LamPtr modval = compile_module_expr(mb.expr);
          mod_path_ = saved_mp; func_path_ = saved_fp;
          auto rl = module_result_layout(mb.expr);
          if (rl.empty()) rl = arg_layout(mb.expr);  // a module path -> its own fields
          // save the names this binding shadows (M is local to the body)
          bool had_i = module_ident_.count(nm), had_a = module_alias_.count(nm);
          Ident sav_i = had_i ? module_ident_[nm] : Ident{};
          LamPtr sav_a = had_a ? module_alias_[nm] : nullptr;
          // every layout/functor key at/under M is scoped to the body too
          // (copy_layout_subtree below adds dotted keys like "M.Make")
          auto under = [&](const std::string& k) {
            return k == nm || (k.size() > nm.size() && k[nm.size()] == '.' &&
                               k.compare(0, nm.size(), nm) == 0);
          };
          auto snap = [&](auto& map) {
            std::vector<std::pair<std::string,
                typename std::decay_t<decltype(map)>::mapped_type>> s;
            for (auto& [k, v] : map) if (under(k)) s.emplace_back(k, v);
            return s;
          };
          auto unsnap = [&](auto& map, auto& s) {
            for (auto it2 = map.begin(); it2 != map.end();)
              if (under(it2->first)) it2 = map.erase(it2); else ++it2;
            for (auto& [k, v] : s) map[k] = std::move(v);
          };
          auto sav_l = snap(module_layout_);
          auto sav_fr = snap(functor_result_);
          auto sav_fp = snap(functor_param_);
          auto& lay = module_layout_[nm]; lay.clear();
          for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
          // a module-path RHS adopts the source's layout subtree (functor
          // members included): `let module S2 = S in .. S2.Make(..)`
          if (auto* pi2 = std::get_if<Pmod_ident>(&mb.expr.desc)) {
            std::string dotted;
            if (lid_to_dotted(pi2->id.txt, dotted)) copy_layout_subtree(dotted, nm);
          }
          LamPtr result;
          if (is_pure_path(modval)) {  // `let module M = <path>`: an alias, elided
            module_alias_[nm] = modval; module_ident_.erase(nm);
            result = expr(*si->body);
          } else {
            Ident mid = fresh(nm);
            module_ident_[nm] = mid; module_alias_.erase(nm);
            auto l = mk(Lam::K::Let);
            l->bindings = {{mid, ValueKind::Gen, modval}}; l->body = expr(*si->body);
            result = l;
          }
          if (had_i) module_ident_[nm] = sav_i; else module_ident_.erase(nm);
          if (had_a) module_alias_[nm] = sav_a; else module_alias_.erase(nm);
          unsnap(module_layout_, sav_l);
          unsnap(functor_result_, sav_fr);
          unsnap(functor_param_, sav_fp);
          return result;
        }
      // `let exception E [of t] in body`: a fresh exception identity per
      // evaluation (caml_fresh_oo_id runs each time this expression does), in
      // scope only over body.  Local exceptions use the bare name string.
      if (auto* pe = std::get_if<Pstr_exception>(&si->item->desc)) {
        const std::string& nm = pe->exn.ctor.name.txt;
        auto str = mk(Lam::K::ConstString); str->str_val = nm;
        auto oid = mk(Lam::K::Prim); oid->prim = Prim::Ccall;
        oid->prim_id = "caml_fresh_oo_id"; oid->args = {cint(0)};
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 248;
        blk->args = {str, oid};
        Ident id = fresh(nm);
        // save what this local exception shadows, restore after the body
        bool had_i = exn_ident_.count(nm), had_a = exn_arity_.count(nm);
        Ident sav_i = had_i ? exn_ident_[nm] : Ident{};
        int sav_a = had_a ? exn_arity_[nm] : 0;
        bool had_r = exn_rlabels_.count(nm);
        std::vector<std::string> sav_r = had_r ? exn_rlabels_[nm] : std::vector<std::string>{};
        exn_ident_[nm] = id;
        if (auto* d = std::get_if<Pext_decl>(&pe->exn.ctor.kind)) {
          if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
            exn_arity_[nm] = (int)t->elems.size();
          else if (auto* r = std::get_if<Pcstr_record>(&d->args)) {
            exn_arity_[nm] = (int)r->fields.size();
            std::vector<std::string> ls;
            for (auto& f : r->fields) ls.push_back(f.name.txt);
            exn_rlabels_[nm] = std::move(ls);
          }
        }
        rec_spine_ = rec_spine;
        LamPtr body = expr(*si->body);
        if (had_i) exn_ident_[nm] = sav_i; else exn_ident_.erase(nm);
        if (had_a) exn_arity_[nm] = sav_a; else exn_arity_.erase(nm);
        if (had_r) exn_rlabels_[nm] = std::move(sav_r); else exn_rlabels_.erase(nm);
        auto l = mk(Lam::K::Let);
        l->bindings = {{id, ValueKind::Gen, blk}}; l->body = body;
        return l;
      }
      return expr(*si->body);  // other struct-item bodies: best-effort
    }
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      // A string literal the inferencer typed at a format type lowers to a
      // CamlinternalFormatBasics format value, not a plain string.
      if (auto* s = std::get_if<Pconst_string>(&c->c.desc); s && vk.format_lits.count(&e))
        if (auto fv = format_value(s->s)) return fv;
      return translate_const(c->c);
    }
    if (auto* as = std::get_if<Pexp_assert>(&e.desc)) {
      // `assert false` -> raise directly; `assert e` -> (if e 0 (raise Assert_failure)).
      // The Assert_failure location is the `assert …` span (kw_loc), which -- unlike
      // e.loc -- is not widened to enclosing parens (ocaml's innermost loc_stack).
      auto* ic = std::get_if<Pexp_construct>(&as->e->desc);
      if (ic && lid_last(ic->id.txt) == "false") return raise_predef("Assert_failure", as->kw_loc);
      auto i = mk(Lam::K::IfThenElse);
      i->cond = expr(*as->e); i->then_ = cint(0);
      i->else_ = raise_predef("Assert_failure", as->kw_loc);
      return i;
    }
    if (auto* lz = std::get_if<Pexp_lazy>(&e.desc)) return lazy_expr(*lz->e);
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) {
      // Mixed value/exception arms: the scrutinee is evaluated under a try whose
      // body exits with the value -- value arms run OUTSIDE the try, exception
      // arms dispatch in its handler:
      //   (catch (try (exit N scrut) with exn <dispatch|reraise>)
      //    with (N v) <value-arm match over v>)
      std::vector<Row> vrows, erows;
      std::vector<std::pair<const Ppat_effect*, const Expression*>> frows;
      bool eff_guard = false;
      for (auto& c : m->cases) {
        const Expression* g = c.guard ? c.guard->get() : nullptr;
        // A value-or-exception or-pattern (`| P as x | exception (Q as x) ->`)
        // splits into one row per side sharing the body (duplicated; ocamlc
        // shares via exits -- exec-equivalent).  Guarded rows keep the old path
        // (exn_dispatch would silently skip a guarded exception row).
        if (!g) {
          std::vector<const Pattern*> leaves;
          bool mixed = false;
          std::function<void(const Pattern*)> walk = [&](const Pattern* p) {
            if (auto* o2 = std::get_if<Ppat_or>(&p->desc)) {
              walk(o2->l.get()); walk(o2->r.get()); return;
            }
            if (std::holds_alternative<Ppat_exception>(p->desc)) mixed = true;
            leaves.push_back(p);
          };
          walk(&c.lhs);
          if (mixed && leaves.size() > 1) {
            for (auto* l : leaves) {
              if (auto* pe2 = std::get_if<Ppat_exception>(&l->desc))
                erows.push_back({pe2->p.get(), c.rhs.get(), nullptr});
              else
                vrows.push_back({l, c.rhs.get(), nullptr});
            }
            continue;
          }
        }
        if (auto* pe = std::get_if<Ppat_exception>(&c.lhs.desc))
          erows.push_back({pe->p.get(), c.rhs.get(), g});
        else if (auto* pf = std::get_if<Ppat_effect>(&c.lhs.desc)) {
          if (g) eff_guard = true;
          frows.push_back({pf, c.rhs.get()});
        } else
          vrows.push_back({&c.lhs, c.rhs.get(), g});
      }
      // Effect handlers (`| effect (Foo i), k -> ..`): the whole match becomes
      //   (runstack (caml_alloc_stack <value fn> <exn fn> <effect fn>)
      //             (function param <scrut>) 0)
      if (!frows.empty() && !eff_guard && !vrows.empty())
        if (LamPtr r = effect_match(*m->e, vrows, erows, frows, e.loc))
          return r;
      // Multiple values (`match e1, e2 with ..`): match the components
      // column-by-column without building the tuple.
      if (frows.empty() && !vrows.empty())
        if (auto* tu = std::get_if<Pexp_tuple>(&m->e->desc))
          if (LamPtr r = multi_match(tu, vrows, erows, e.loc))
            return r;
      if (!erows.empty() && !vrows.empty() && frows.empty()) {
        int eid = ++next_exit_;
        auto ex = mk(Lam::K::Staticraise);
        ex->prim_arg = eid; ex->args = {expr(*m->e)};
        auto tr = mk(Lam::K::Try);
        tr->body = ex;
        // the exn binder takes the first var/alias exception row's name (`|
        // exception e ->` -> `with e`), else ocamlc's default `exn`.
        std::string en;
        for (auto& r : erows) {
          const Pattern* ep = effective_pat(r.lhs);
          if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) { en = pv->name.txt; break; }
          if (auto* pa = std::get_if<Ppat_alias>(&ep->desc)) { en = pa->name.txt; break; }
        }
        tr->var = fresh(en.empty() ? "exn" : en);
        scope.emplace_back();
        caught_exn_.push_back(tr->var);
        tr->then_ = exn_dispatch(tr->var, erows, 0);
        caught_exn_.pop_back();
        scope.pop_back();
        // the catch var takes the first var/alias value row's name, else ocamlc's
        // default "val" (Matching.name_pattern); it carries the scrutinee's kind.
        std::string vn;
        for (auto& r : vrows) {
          const Pattern* ep = effective_pat(r.lhs);
          if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) { vn = pv->name.txt; break; }
          if (auto* pa = std::get_if<Ppat_alias>(&ep->desc)) { vn = pa->name.txt; break; }
        }
        Ident v = fresh(vn.empty() ? "val" : vn);
        auto cat = mk(Lam::K::Catch);
        cat->cond = tr; cat->prim_arg = eid; cat->catch_vars = {v};
        cat->catch_var_kinds = {expr_kind(m->e.get())};
        scope.emplace_back();
        cat->then_ = compile_match(varof(v), vrows, e.loc);
        scope.pop_back();
        return cat;
      }
      return compile_match(expr(*m->e), m->cases, e.loc);
    }
    if (auto* tu = std::get_if<Pexp_tuple>(&e.desc)) {
      std::vector<LamPtr> es;
      std::vector<ValueKind> shape;
      for (auto& el : tu->elems) { es.push_back(expr(*el)); shape.push_back(expr_kind(el.get())); }
      auto b = block(0, std::move(es));
      if (b->k == Lam::K::Prim) b->blk_shape = std::move(shape);  // dynamic block -> field shape
      return b;
    }
    if (auto* rc = std::get_if<Pexp_record>(&e.desc)) {
      // Functional update `{e with l=v}`: unchanged fields read off the base --
      // bound to an `init` temp unless it is already a variable (simplif's
      // alias elimination), each read by its field kind/mutability.
      if (rc->base && !rc->fields.empty()) {
        const RecType* rt = nullptr;
        RecType std_rt;            // backing store for a stdlib-cmi layout
        std::vector<bool> fmut;    // per-field mutability, parallel to labels
        // The update is valid only against a record whose layout HAS every
        // updated label; a `find_field`-by-name hit can land on the WRONG record
        // when the short type name collides (Parsetree.expression vs
        // Typedtree.expression -- `{texp with exp_type=..}` must not resolve to
        // Parsetree's `expression`, which has no `exp_type`).
        auto has_all_labels = [&](const RecType* r) {
          for (auto& [lid, ve] : rc->fields) {
            bool found = false;
            for (auto& l : r->labels) if (l == lid_last(lid.txt)) { found = true; break; }
            if (!found) return false;
          }
          return true;
        };
        if (auto* f0 = find_field(lid_last(rc->fields[0].first.txt)))
          if (auto it = rec_types_.find(f0->type);
              it != rec_types_.end() && has_all_labels(&it->second)) {
            rt = &it->second;
            for (auto& l : it->second.labels) {
              auto* fi = find_field(l);
              fmut.push_back(fi && fi->type == f0->type && fi->mut);
            }
          }
        if (!rt) {  // a stdlib record of a non-opened module (e.g. Gc.control)
          std::string mod = record_module_of(rc->fields[0].first.txt, rc->base->get());
          if (!mod.empty())
            if (auto sr = stdlib_record_layout(mod, lid_last(rc->fields[0].first.txt))) {
              std_rt.labels = std::move(sr->labels);
              std_rt.shape = std::move(sr->shape);
              std_rt.mut = false;
              for (bool m : sr->mut) if (m) std_rt.mut = true;
              std_rt.flat = sr->flat;  // Complex.t etc.: a flat float block
              fmut = std::move(sr->mut);
              rt = &std_rt;
            }
        }
        if (!rt)  // `{ e with A.B.label = v }` -- a DEEP-qualified record label
          if (auto* d0 = std::get_if<Ldot>(&rc->fields[0].first.txt.v))
            if (!std::get_if<Lident>(&d0->prefix->v)) {
              std::string dotted;
              if (lid_to_dotted(*d0->prefix, dotted))
                if (auto sr = nested_record_layout(dotted, d0->name)) {
                  std_rt.labels = std::move(sr->labels);
                  std_rt.shape = std::move(sr->shape);
                  std_rt.mut = false;
                  for (bool m : sr->mut) if (m) std_rt.mut = true;
                  std_rt.flat = sr->flat;
                  fmut = std::move(sr->mut);
                  rt = &std_rt;
                }
            }
        if (!rt)  // the base's INFERRED type path (`Typedtree.expression`): load
                  // its layout from the cmi by name -- the only reliable source
                  // when a short type-name collision kept it out of rec_types_.
          if (auto itc = expr_constr_of(&e, rc->base->get());
              itc != vk.expr_constr.end()) {
            const std::string& p = itc->second;
            auto dpos = p.rfind('.');
            if (dpos != std::string::npos && p.find('.') == dpos)
              if (auto sr = stdlib_record_layout_named(p.substr(0, dpos),
                                                       p.substr(dpos + 1))) {
                std_rt.labels = std::move(sr->labels);
                std_rt.shape = std::move(sr->shape);
                std_rt.mut = false;
                for (bool m : sr->mut) if (m) std_rt.mut = true;
                std_rt.flat = sr->flat;
                fmut = std::move(sr->mut);
                rt = &std_rt;
              }
          }
        if (!rt) {  // the UNIQUE registered record whose layout has every updated
                    // label -- finds a collision-disambiguated key
                    // (`expression#N` = Typedtree.expression) that the bare-name
                    // `find_field` lookup above could not.
          const RecType* uniq = nullptr; bool many = false;
          for (auto& [name, cand] : rec_types_)
            if (has_all_labels(&cand)) { if (uniq) many = true; uniq = &cand; }
          if (uniq && !many) {
            rt = uniq;
            for (auto& l : rt->labels) {
              auto* fi = find_field(l);
              fmut.push_back(fi && fi->mut);
            }
          }
        }
        if (rt) {
          auto index_of = [&](const std::string& l) {
            for (size_t i = 0; i < rt->labels.size(); ++i)
              if (rt->labels[i] == l) return (int)i;
            return -1;
          };
          LamPtr basev = expr(**rc->base);
          std::vector<LamPtr> vals(rt->labels.size());
          bool ok = true;
          for (auto& [lid, ve] : rc->fields) {
            int ix = index_of(lid_last(lid.txt));
            if (ix < 0 || vals[ix]) { ok = false; break; }
            vals[ix] = expr(*ve);
          }
          if (ok) {
            Ident tv; bool temp = basev->k != Lam::K::Var;
            LamPtr bv = basev;
            if (temp) { tv = fresh("init"); auto v = mk(Lam::K::Var); v->var = tv; bv = v; }
            for (size_t i = 0; i < vals.size(); ++i) {
              if (vals[i]) continue;
              auto fr = mk(Lam::K::Prim);
              if (rt->flat) fr->prim = Prim::Floatfield;
              else
                fr->prim = rt->shape[i] == ValueKind::Int ? Prim::FieldInt
                           : fmut[i]                      ? Prim::FieldMut
                                                          : Prim::FieldImm;
              fr->prim_arg = (int)i; fr->args = {bv};
              vals[i] = fr;
            }
            LamPtr blk;
            if (rt->flat) {  // flat float record: a float block, not a record
              auto m = mk(Lam::K::Prim); m->prim = Prim::IntCmp;
              m->prim_id = rt->mut ? "makearray[float]" : "makearray_imm[float]";
              m->args = std::move(vals);
              blk = m;
            } else if (!rt->mut) {
              blk = block(0, std::move(vals));
              if (blk->k == Lam::K::Prim) blk->blk_shape = rt->shape;
            } else {
              auto m = mk(Lam::K::Prim);
              m->prim = Prim::Makemutable; m->prim_arg = 0;
              m->blk_shape = rt->shape; m->args = std::move(vals);
              blk = m;
            }
            if (!temp) return blk;
            auto l = mk(Lam::K::Let);
            l->bindings = {{tv, ValueKind::Gen, basev}};
            l->body = blk;
            return l;
          }
        }
      }
      if (!rc->base && !rc->fields.empty()) {  // not a functional update `{e with ..}`
        // Pick the record type: the first label's registered type when the
        // literal's label set fills it exactly; otherwise any known record type
        // the label set fills exactly (shared label names -- Effect.Deep's
        // handler.effc vs effect_handler.effc -- disambiguate by shape).
        std::set<std::string> labs;
        for (auto& [lid, ve] : rc->fields) labs.insert(lid_last(lid.txt));
        const RecType* rt = nullptr;
        // The first label's registered type -- but only when it actually HAS the
        // whole literal label set.  A same-SHORT-NAMED record from another opened
        // module (`open Parsetree; open Typedtree` -> two `value_binding`s, both 5
        // fields: pvb_* vs vb_*) can match by COUNT yet have the wrong labels; if
        // we locked onto it here, index_of below would fail (-1) and the literal
        // collapse to 0.  Requiring the labels to match lets the exact-set loop
        // below find the right (collision-disambiguated) record instead.
        if (auto* f0 = find_field(lid_last(rc->fields[0].first.txt)))
          if (auto it = rec_types_.find(f0->type);
              it != rec_types_.end() && it->second.labels.size() == labs.size()) {
            bool all = true;
            for (auto& l : labs)
              if (std::find(it->second.labels.begin(), it->second.labels.end(), l)
                  == it->second.labels.end()) { all = false; break; }
            if (all) rt = &it->second;
          }
        if (!rt)
          for (auto& [name, cand] : rec_types_) {
            if (cand.labels.size() != labs.size()) continue;
            bool all = true;
            for (auto& l : cand.labels) if (!labs.count(l)) { all = false; break; }
            if (all) { rt = &cand; break; }
          }
        RecType std_rt;  // a stdlib record: by the literal's inferred type path,
                         // else an opened module's label set (`open Gc; {..}`)
        if (!rt) {
          auto try_std = [&](std::optional<StdRec> sr) {
            if (!sr || sr->labels.size() != labs.size()) return false;
            for (auto& l : sr->labels) if (!labs.count(l)) return false;
            std_rt.labels = std::move(sr->labels);
            std_rt.shape = std::move(sr->shape);
            std_rt.mut = false;
            for (bool m : sr->mut) if (m) std_rt.mut = true;
            std_rt.flat = sr->flat;  // Complex.t etc.: build a flat float block
            rt = &std_rt;
            return true;
          };
          // an explicit module qualification on the first label (`{Complex.im=..}`)
          if (auto* d0 = std::get_if<Ldot>(&rc->fields[0].first.txt.v)) {
            if (auto* pl0 = std::get_if<Lident>(&d0->prefix->v))
              try_std(stdlib_record_layout(pl0->name, d0->name));
            else {  // a NESTED module prefix (`CamlinternalMenhirLib.EngineTypes.state`)
              std::string dotted;
              if (lid_to_dotted(*d0->prefix, dotted))
                try_std(nested_record_layout(dotted, d0->name));
            }
          }
          if (!rt)
            if (auto itc = vk.expr_constr.find(&e); itc != vk.expr_constr.end()) {
              const std::string& p = itc->second;
              auto dpos = p.rfind('.');
              std::string mod = p.substr(0, dpos);
              if (mod.find('.') == std::string::npos)
                try_std(stdlib_record_layout_named(mod, p.substr(dpos + 1)));
            }
          if (!rt)
            for (auto it2 = opened_.rbegin(); it2 != opened_.rend(); ++it2) {
              if (it2->find('.') != std::string::npos || module_base(*it2)) continue;
              if (try_std(stdlib_record_layout(*it2,
                                               lid_last(rc->fields[0].first.txt))))
                break;
            }
          // LAST RESORT: an unqualified literal of an imported record in scope only
          // by type-directed disambiguation (Parmatch.parmatch_case).  The label
          // set must map to exactly one include-path record, else leave it (= 0).
          if (!rt) {
            build_labelset_index();
            std::vector<std::string> key(labs.begin(), labs.end());  // labs: sorted set
            auto li = labelset_index_.find(key);
            if (li != labelset_index_.end() && !li->second.flat) try_std(li->second);
          }
        }
        if (rt) {
          auto index_of = [&](const std::string& l) {
            for (size_t i = 0; i < rt->labels.size(); ++i)
              if (rt->labels[i] == l) return (int)i;
            return -1;
          };
          std::vector<LamPtr> vals(rt->labels.size());
          bool ok = vals.size() == rc->fields.size();
          for (auto& [lid, ve] : rc->fields) {
            int ix = index_of(lid_last(lid.txt));
            if (ix < 0 || vals[ix]) { ok = false; break; }
            // A bare unresolved ctor field value whose field type is a qualified
            // variant (`{ vb_rec_kind = Dynamic }`, Value_rec_types un-opened):
            // register that type's ctors first so it lowers to a tag, not `?Dynamic`.
            if (auto* kc = std::get_if<Pexp_construct>(&ve->desc))
              if (std::holds_alternative<Lident>(kc->id.txt.v) &&
                  !ctor_info_.count(lid_last(kc->id.txt)))
                if (auto fc = field_value_ctor_type_.find(lid_last(lid.txt));
                    fc != field_value_ctor_type_.end())
                  register_ctors_of_type(fc->second);
            vals[ix] = expr(*ve);
          }
          if (ok) {
            if (rt->flat) {  // flat float record: a float block, not a record
              auto m = mk(Lam::K::Prim); m->prim = Prim::IntCmp;
              m->prim_id = rt->mut ? "makearray[float]" : "makearray_imm[float]";
              m->args = std::move(vals);
              return m;
            }
            if (!rt->mut) {
              auto b = block(0, std::move(vals));
              if (b->k == Lam::K::Prim) b->blk_shape = rt->shape;  // field kinds
              return b;
            }
            auto m = mk(Lam::K::Prim);  // any mutable field -> makemutable
            m->prim = Prim::Makemutable; m->prim_arg = 0;
            m->blk_shape = rt->shape; m->args = std::move(vals);
            return m;
          }
        }
        // The predefined `'a ref` record: `{contents = e}` == `ref e`
        // (no user/stdlib type matched the label set above).
        if (!rt && rc->fields.size() == 1 &&
            lid_last(rc->fields[0].first.txt) == "contents") {
          auto* ve = rc->fields[0].second.get();
          auto m = mk(Lam::K::Prim);
          m->prim = Prim::Makemutable; m->prim_arg = 0;
          m->blk_shape = {expr_kind(ve)};
          m->args = {expr(*ve)};
          return m;
        }
      }
    }
    if (auto* ar = std::get_if<Pexp_array>(&e.desc)) {  // [| ... |] -> makearray[k]
      std::string k = ar->elems.empty() ? array_arg_kind(&e) : array_elem_kind(ar->elems[0].get());
      auto m = mk(Lam::K::Prim); m->prim = Prim::IntCmp;
      m->prim_id = "makearray[" + k + "]";
      for (auto& el : ar->elems) m->args.push_back(expr(*el));
      return m;
    }
    if (auto* fe = std::get_if<Pexp_field>(&e.desc)) {
      // `x.label` on a whole-inline-record-bound var (ident.ml's `Local x` then
      // `x.stamp`): resolve via the ctor's rlabels (ambiguous labels first).
      if (auto irf = inline_rec_field(fe->e.get(), lid_last(fe->field.txt))) {
        auto l = mk(Lam::K::Prim);
        l->prim = irf->kind == ValueKind::Int ? Prim::FieldInt
                  : irf->mut                  ? Prim::FieldMut
                                              : Prim::FieldImm;
        l->prim_arg = irf->index; l->args = {expr(*fe->e)};
        return l;
      }
      // A qualified field `e.M.label` (M local or stdlib) / deep `e.A.B.label`:
      // the qualifier is authoritative, so resolve it BEFORE the bare find_field
      // (which could pick a different record sharing an ambiguous label).
      if (auto rf = qualified_field(fe->field.txt)) {
        auto l = mk(Lam::K::Prim);
        l->prim = rf->flat                     ? Prim::Floatfield
                  : rf->kind == ValueKind::Int ? Prim::FieldInt
                  : rf->mut                    ? Prim::FieldMut
                                               : Prim::FieldImm;
        l->prim_arg = rf->index; l->args = {expr(*fe->e)};
        return l;
      }
      // A var bound to a constructor argument or inline-record field of a KNOWN
      // record type: that declared type is AUTHORITATIVE, beating the checker's
      // ambiguous guess below for a var whose type it could not pin down
      // (`Sig_module(_,_,md,_,_)` then `md.md_type` -> Types.module_declaration@0,
      // not the last-registered Typedtree.module_declaration@4).
      if (auto* id = std::get_if<Pexp_ident>(&fe->e->desc))
        if (auto* l = std::get_if<Lident>(&id->id.txt.v))
          if (auto* b = lookup(l->name))
            if (auto vp = var_record_path_.find(b->stamp); vp != var_record_path_.end()) {
              if (auto rf = resolve_field_in_record_path(vp->second, lid_last(fe->field.txt))) {
                auto lp = mk(Lam::K::Prim);
                auto rt = rec_types_.find(rf->type);
                lp->prim = (rt != rec_types_.end() && rt->second.flat) ? Prim::Floatfield
                           : rf->kind == ValueKind::Int ? Prim::FieldInt
                           : rf->mut                    ? Prim::FieldMut
                                                        : Prim::FieldImm;
                lp->prim_arg = rf->index; lp->args = {expr(*fe->e)};
                return lp;
              }
            }
      // An AMBIGUOUS label resolved by the checker through the base's type
      // identity (the bare find_field below cannot disambiguate same-named
      // records -- Sign_diff.t.untypables@4 vs signature_symptom.untypables@8).
      if (auto it = vk.field_resolved.find(&e); it != vk.field_resolved.end()) {
        auto l = mk(Lam::K::Prim);
        l->prim = it->second.kind == "int" ? Prim::FieldInt
                  : it->second.mut         ? Prim::FieldMut
                                           : Prim::FieldImm;
        l->prim_arg = it->second.index; l->args = {expr(*fe->e)};
        return l;
      }
      // The base's INFERRED module-qualified record type disambiguates a label
      // shared across opened records: `vd1 : Types.value_description` then
      // `vd1.val_loc`, where `open Types; open Typedtree` both define a
      // value_description with `val_loc`/`val_attributes` at DIFFERENT offsets --
      // the bare find_field below takes the last-opened (Typedtree) offset and
      // reads the wrong (here out-of-bounds) field of a Types.value_description.
      if (auto it = vk.expr_constr.find(fe->e.get()); it != vk.expr_constr.end()) {
        const std::string& p = it->second;
        auto dpos = p.rfind('.');
        std::string lbl = lid_last(fe->field.txt);
        if (dpos != std::string::npos && p.find('.') == dpos) {
          std::string mod = p.substr(0, dpos), ty = p.substr(dpos + 1);
          // A LOCAL nested-module record (`module Measure = struct type t = {..}`;
          // `(m : Measure.t).allocated_words`): the same collision but the type has
          // no cmi, so resolve via the nested record registered under this module.
          if (auto mr = mod_record_types_.find(mod); mr != mod_record_types_.end())
            for (auto& key : mr->second)
              if (key == ty ||
                  (key.size() > ty.size() + 1 && key.compare(0, ty.size(), ty) == 0 &&
                   key[ty.size()] == '#'))
                if (auto tf = type_field_info_.find(key); tf != type_field_info_.end())
                  if (auto fi = tf->second.find(lbl); fi != tf->second.end()) {
                    auto l = mk(Lam::K::Prim);
                    auto rt = rec_types_.find(key);
                    l->prim = (rt != rec_types_.end() && rt->second.flat) ? Prim::Floatfield
                              : fi->second.kind == ValueKind::Int ? Prim::FieldInt
                              : fi->second.mut                    ? Prim::FieldMut
                                                                  : Prim::FieldImm;
                    l->prim_arg = fi->second.index; l->args = {expr(*fe->e)};
                    return l;
                  }
          if (auto sr = stdlib_record_layout_named(mod, ty))
            for (size_t i = 0; i < sr->labels.size(); ++i)
              if (sr->labels[i] == lbl) {
                auto l = mk(Lam::K::Prim);
                l->prim = sr->flat                         ? Prim::Floatfield
                          : sr->shape[i] == ValueKind::Int ? Prim::FieldInt
                          : sr->mut[i]                     ? Prim::FieldMut
                                                           : Prim::FieldImm;
                l->prim_arg = (int)i; l->args = {expr(*fe->e)};
                return l;
              }
        }
      }
      // A var bound to a record LITERAL of a label-set-determined type: resolve the
      // (possibly ambiguous) label through that exact layout (`md.md_type` ->
      // Types.module_declaration@0, not Typedtree.module_declaration@4).
      if (auto* id = std::get_if<Pexp_ident>(&fe->e->desc))
        if (auto* l = std::get_if<Lident>(&id->id.txt.v))
          if (auto* b = lookup(l->name))
            if (auto vit = var_record_lit_.find(b->stamp); vit != var_record_lit_.end()) {
              const StdRec& sr = vit->second;
              std::string lbl = lid_last(fe->field.txt);
              for (int i = 0; i < (int)sr.labels.size(); ++i)
                if (sr.labels[i] == lbl) {
                  auto lp = mk(Lam::K::Prim);
                  lp->prim = sr.flat ? Prim::Floatfield
                             : sr.shape[i] == ValueKind::Int ? Prim::FieldInt
                             : sr.mut[i]                     ? Prim::FieldMut
                                                             : Prim::FieldImm;
                  lp->prim_arg = i; lp->args = {expr(*fe->e)};
                  return lp;
                }
            }
      // A var bound to an inline-record field of NAMED RECORD type (`Pattern
      // {penv}`, penv : Pattern_env.t): resolve its AMBIGUOUS label through that
      // record type, which find_field below cannot disambiguate (Ctype.get_env's
      // `penv.env` -- env shared with the Expression inline record -> else 0).
      if (auto* id = std::get_if<Pexp_ident>(&fe->e->desc))
        if (auto* l = std::get_if<Lident>(&id->id.txt.v))
          if (auto* b = lookup(l->name))
            if (auto vp = var_record_path_.find(b->stamp); vp != var_record_path_.end())
              if (auto rf = resolve_field_in_record_path(vp->second,
                                                         lid_last(fe->field.txt))) {
                auto lp = mk(Lam::K::Prim);
                auto rt = rec_types_.find(rf->type);
                lp->prim = (rt != rec_types_.end() && rt->second.flat) ? Prim::Floatfield
                           : rf->kind == ValueKind::Int ? Prim::FieldInt
                           : rf->mut                    ? Prim::FieldMut
                                                        : Prim::FieldImm;
                lp->prim_arg = rf->index; lp->args = {expr(*fe->e)};
                return lp;
              }
      if (auto* fi = find_field(lid_last(fe->field.txt))) {
        auto l = mk(Lam::K::Prim);
        auto rt = rec_types_.find(fi->type);
        if (rt != rec_types_.end() && rt->second.flat)
          l->prim = Prim::Floatfield;  // flat float record: unboxed field read
        else
          l->prim = fi->kind == ValueKind::Int ? Prim::FieldInt
                    : fi->mut                  ? Prim::FieldMut
                                               : Prim::FieldImm;
        l->prim_arg = fi->index; l->args = {expr(*fe->e)};
        return l;
      }
      // `r.contents` is the ref's mutable cell (the `!r` spelling's field).
      if (lid_last(fe->field.txt) == "contents") {
        auto l = mk(Lam::K::Prim);
        l->prim = expr_kind(&e) == ValueKind::Int ? Prim::FieldInt : Prim::FieldMut;
        l->prim_arg = 0; l->args = {expr(*fe->e)};
        return l;
      }
      // A module-qualified field `e.M.label` of a stdlib record (e.g. Gc.control),
      // or a DEEP-qualified one `e.A.B.label` (menhir's `env.CamlinternalMenhirLib.
      // EngineTypes.current`) navigated through nested submodules.
      if (auto* d = std::get_if<Ldot>(&fe->field.txt.v)) {
        std::optional<StdField> rf;
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) rf = stdlib_record_field(pl->name, d->name);
        else {
          std::string dotted;
          if (lid_to_dotted(*d->prefix, dotted)) rf = nested_record_field(dotted, d->name);
        }
        if (rf) {
          auto l = mk(Lam::K::Prim);
          l->prim = rf->flat                 ? Prim::Floatfield  // Complex.t etc.
                    : rf->kind == ValueKind::Int ? Prim::FieldInt
                    : rf->mut                  ? Prim::FieldMut
                                               : Prim::FieldImm;
          l->prim_arg = rf->index; l->args = {expr(*fe->e)};
          return l;
        }
      }
      // An unqualified stdlib-record label via the base's inferred type.
      if (auto rf = inferred_record_field(fe->e.get(), lid_last(fe->field.txt))) {
        auto l = mk(Lam::K::Prim);
        l->prim = rf->flat                 ? Prim::Floatfield  // Complex.t etc.
                  : rf->kind == ValueKind::Int ? Prim::FieldInt
                  : rf->mut                  ? Prim::FieldMut
                                             : Prim::FieldImm;
        l->prim_arg = rf->index; l->args = {expr(*fe->e)};
        return l;
      }
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      // A qualified field `e.M.label <- v` (M local or stdlib) / deep one:
      // resolve via the authoritative qualifier BEFORE the bare find_field.
      if (auto rf = qualified_field(sf->field.txt)) {
        auto l = mk(Lam::K::Prim);
        l->prim = rf->flat                     ? Prim::SetFloatfield
                  : rf->kind == ValueKind::Int ? Prim::SetfieldImm
                                               : Prim::SetfieldPtr;
        l->prim_arg = rf->index; l->args = {expr(*sf->obj), expr(*sf->value)};
        return l;
      }
      if (auto* fi = find_field(lid_last(sf->field.txt))) {
        auto l = mk(Lam::K::Prim);
        auto rt = rec_types_.find(fi->type);
        if (rt != rec_types_.end() && rt->second.flat)
          l->prim = Prim::SetFloatfield;  // flat float record: unboxed field write
        else
          l->prim = fi->kind == ValueKind::Int ? Prim::SetfieldImm : Prim::SetfieldPtr;
        l->prim_arg = fi->index; l->args = {expr(*sf->obj), expr(*sf->value)};
        return l;
      }
      if (lid_last(sf->field.txt) == "contents") {  // r.contents <- v == r := v
        auto l = mk(Lam::K::Prim);
        l->prim = expr_kind(sf->value.get()) == ValueKind::Int ? Prim::SetfieldImm
                                                               : Prim::SetfieldPtr;
        l->prim_arg = 0; l->args = {expr(*sf->obj), expr(*sf->value)};
        return l;
      }
      // A module-qualified field `e.M.label <- v` of a cross-module record
      // (mirrors the read path -- without this the set silently became a no-op).
      if (auto* d = std::get_if<Ldot>(&sf->field.txt.v)) {
        std::optional<StdField> rf;
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) rf = stdlib_record_field(pl->name, d->name);
        else {
          std::string dotted;
          if (lid_to_dotted(*d->prefix, dotted)) rf = nested_record_field(dotted, d->name);
        }
          if (rf) {
            auto l = mk(Lam::K::Prim);
            l->prim = rf->flat                 ? Prim::SetFloatfield
                      : rf->kind == ValueKind::Int ? Prim::SetfieldImm
                                                   : Prim::SetfieldPtr;
            l->prim_arg = rf->index; l->args = {expr(*sf->obj), expr(*sf->value)};
            return l;
          }
      }
      // An unqualified stdlib-record label via the base's inferred type.
      if (auto rf = inferred_record_field(sf->obj.get(), lid_last(sf->field.txt))) {
        auto l = mk(Lam::K::Prim);
        l->prim = rf->flat                 ? Prim::SetFloatfield  // Complex.t etc.
                  : rf->kind == ValueKind::Int ? Prim::SetfieldImm
                                               : Prim::SetfieldPtr;
        l->prim_arg = rf->index; l->args = {expr(*sf->obj), expr(*sf->value)};
        return l;
      }
    }
    // Polymorphic variant: `` `Foo `` is its name's hash; `` `Foo e `` is a block
    // tag 0 of (hash, arg) -- block() folds it to a constant when the arg is one.
    if (auto* pv = std::get_if<Pexp_variant>(&e.desc)) {
      LamPtr h = cint(hash_variant(pv->label));
      if (!pv->arg) return h;
      return block(0, {h, expr(**pv->arg)});
    }
    if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      std::string n = lid_last(k->id.txt);
      // A qualified ctor of a separately-compiled module in EXPRESSION position
      // registers its whole type's ctors (like the pattern path does), so a later
      // UNQUALIFIED sibling resolves: predef.ml writes `Type_immediacy.Unknown`
      // (the decl0 default) then `~immediate:Always` -- type-directed, where the
      // expected type is fixed by that same qualified default.
      if (std::holds_alternative<Ldot>(k->id.txt.v) && !ctor_info_.count(n)) {
        register_single_qualified_ctor(k->id.txt);     // top-level `M.Ctor` (this
                                                        // ctor only -- a sibling's
                                                        // short name may belong to
                                                        // another type; a later
                                                        // BARE sibling resolves via
                                                        // the inferred-type path
                                                        // below)
        register_opened_submodule_ctors(k->id.txt);    // opened-submodule `Sub.Ctor`
      }
      // A BARE ctor unresolved here but whose inferred type is a module-qualified
      // variant (type-directed disambiguation, NO `open`): register that type's
      // ctors.  btype.ml's `Misc.protect_refs [R(pool_stack, pool)] f` -- `R` is
      // Misc.ref_and_value's constructor, resolved only by protect_refs's expected
      // arg type; without this it compiled to a bare `?R` with its args DROPPED.
      // Mirrors scan_pat_ctors' pattern-side type-directed registration.
      if (std::holds_alternative<Lident>(k->id.txt.v) && !ctor_info_.count(n)) {
        auto itc = vk.expr_constr.find(&e);
        if (itc != vk.expr_constr.end()) register_ctors_of_type(itc->second);
      }
      // a constructor qualified by a bound module that exports it (an exception /
      // extension constructor): read its identity from that module's block, so
      // `M1.E` and `M2.E` (distinct `type t += E`) stay distinct.
      if (LamPtr cid = module_ctor_identity(k->id.txt)) {
        if (!k->arg) return cid;  // nullary: the identity value itself
        int arity = exn_arity_.count(n) ? exn_arity_[n] : 1;  // applied: block tag 0
        std::vector<const Expression*> fs;
        if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
            at && arity > 1 && (int)at->elems.size() == arity)
          for (auto& el : at->elems) fs.push_back(el.get());
        else fs.push_back(k->arg->get());
        std::vector<LamPtr> fields = {cid};
        std::vector<ValueKind> shape = {ValueKind::Gen};
        for (auto* ex : fs) { fields.push_back(expr(*ex)); shape.push_back(expr_kind(ex)); }
        auto b = mk(Lam::K::Prim); b->prim = Prim::Makeblock; b->prim_arg = 0;
        b->args = std::move(fields); b->blk_shape = std::move(shape);
        return b;
      }
      // an `[@@unboxed]` constructor is a no-op wrapper: its value is its argument
      if (auto ci = ctor_info_.find(n); ci != ctor_info_.end() && ci->second.unboxed && k->arg)
        return expr(**k->arg);
      if (n == "[]" || n == "None" || n == "false" || n == "()") { auto z = mk(Lam::K::ConstInt); z->int_val = 0; return z; }
      if (n == "true") { auto z = mk(Lam::K::ConstInt); z->int_val = 1; return z; }
      if (n == "::" && k->arg) {  // a :: b : block tag 0 of (head, tail)
        if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc); at && at->elems.size() == 2)
          return block_of(0, {at->elems[0].get(), at->elems[1].get()});
      }
      if (n == "Some" && k->arg) return block_of(0, {k->arg->get()});
      // a LOCAL exception/extension ctor shadows a same-named builtin ctor
      // (`exception Ok` vs result's Ok); in raise position the argument is
      // exn-typed, so a registered exception wins over a same-named variant
      bool raise_pos = raise_arg_;
      raise_arg_ = false;  // consumed by the head constructor only
      bool exn_shadows = (exn_ident_.count(n) || exn_field_.count(n)) &&
                         (raise_pos || !ctor_info_.count(n) || builtin_ctors_.count(n));
      // A QUALIFIED `M.C` resolves via M's own type, OUTRANKING a same-named
      // local/bare ctor (typecore's `Env.Pattern` vs its local `Pattern`).
      const CtorInfo* qci = std::holds_alternative<Ldot>(k->id.txt.v)
                            ? qualified_ctor_info(k->id.txt) : nullptr;
      const CtorInfo* cip = qci;
      if (!cip) { auto ci = ctor_info_.find(n); if (ci != ctor_info_.end()) cip = &ci->second; }
      if (cip && !exn_shadows) {
        if (!cip->is_block) return cint(cip->tag);  // constant -> its tag
        // inline record (`T {pos}`): the labels are the block's fields, in
        // declaration order, with the declared kinds as the shape
        if (!cip->rlabels.empty() && k->arg) {
          auto* rc = std::get_if<Pexp_record>(&(*k->arg)->desc);
          if (rc && !rc->base) {
            auto& L = cip->rlabels;
            std::vector<const Expression*> vexps(L.size(), nullptr);
            bool ok = vexps.size() == rc->fields.size();
            for (auto& [lid, ve] : rc->fields) {
              int ix = -1;
              for (size_t i2 = 0; i2 < L.size(); ++i2)
                if (L[i2] == lid_last(lid.txt)) { ix = (int)i2; break; }
              if (ix < 0 || vexps[ix]) { ok = false; break; }
              vexps[ix] = ve.get();
            }
            if (ok) {
              bool anymut = false;
              for (bool m : cip->rfmut) anymut = anymut || m;
              std::vector<LamPtr> vals;
              for (auto* vexp : vexps) vals.push_back(expr(*vexp));
              if (anymut) {
                auto m = mk(Lam::K::Prim);
                m->prim = Prim::Makemutable; m->prim_arg = cip->tag;
                m->blk_shape = cip->rshape; m->args = std::move(vals);
                return m;
              }
              auto b = block(cip->tag, std::move(vals));
              if (b->k == Lam::K::Prim) b->blk_shape = cip->rshape;
              return b;
            }
          }
          // A functional update of an inline record, re-wrapped in its ctor
          // (`Pattern { r with assume_injective = false }`): copy the base's fields,
          // override the updated labels, rebuild the ctor block.  Without this the
          // update collapses to a 1-field block and later field reads go OOB.
          if (rc && rc->base) {
            auto& L = cip->rlabels;
            std::unordered_map<std::string, const Expression*> upd;
            bool ok = true;
            for (auto& [lid, ve] : rc->fields) {
              std::string ln = lid_last(lid.txt);
              if (std::find(L.begin(), L.end(), ln) == L.end()) { ok = false; break; }
              upd[ln] = ve.get();
            }
            if (ok) {
              LamPtr basev = expr(**rc->base);
              bool temp = basev->k != Lam::K::Var;
              Ident tv;
              LamPtr bref = basev;
              if (temp) { tv = fresh("with"); bref = mk(Lam::K::Var); bref->var = tv; }
              std::vector<LamPtr> vals;
              for (int i = 0; i < (int)L.size(); ++i) {
                auto u = upd.find(L[i]);
                if (u != upd.end()) vals.push_back(expr(*u->second));
                else {
                  FieldInfo fi{cip->type, i, cip->rfmut[i], cip->rshape[i]};
                  vals.push_back(field_read(&fi, bref));
                }
              }
              bool anymut = false;
              for (bool m : cip->rfmut) anymut = anymut || m;
              LamPtr blk;
              if (anymut) {
                auto m = mk(Lam::K::Prim);
                m->prim = Prim::Makemutable; m->prim_arg = cip->tag;
                m->blk_shape = cip->rshape; m->args = std::move(vals);
                blk = m;
              } else {
                blk = block(cip->tag, std::move(vals));
                if (blk->k == Lam::K::Prim) blk->blk_shape = cip->rshape;
              }
              if (!temp) return blk;
              auto lt = mk(Lam::K::Let);
              lt->bindings = {{tv, ValueKind::Gen, basev}};
              lt->body = blk;
              return lt;
            }
          }
        }
        std::vector<const Expression*> fs;
        if (k->arg) {  // `B of t1 * t2` flattens the tuple argument into fields
          if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
              at && cip->arity > 1 && (int)at->elems.size() == cip->arity)
            for (auto& el : at->elems) fs.push_back(el.get());
          else
            fs.push_back(k->arg->get());
        }
        return block_of(cip->tag, fs);
      }
      // A bare exception ctor that is local (exn_ident_/exn_field_) OR brought in
      // by `open M` (a global module).  The opened-module case must be resolved
      // here -- otherwise `open My_std; raise Exit_OK` left Exit_OK an unbound
      // var, raising garbage that no `with Exit_OK` arm could match.
      LamPtr exnv = (exn_ident_.count(n) || exn_field_.count(n))
                        ? exn_value(n)
                        : (std::holds_alternative<Lident>(k->id.txt.v) && !ctor_info_.count(n)
                               ? opened_module_exn_value(n)
                               : nullptr);
      if (exnv) {
        LamPtr v = exnv;
        if (!k->arg) return v;  // local exception/extension-ctor value
        // applied: a block whose field 0 is the constructor's identity --
        // `(makeblock 0 (*,k1..) E/1 a1..)` -- with a tuple argument flattened
        // when the constructor was declared with several fields.
        int arity = 1;
        if (auto a = exn_arity_.find(n); a != exn_arity_.end()) arity = a->second;
        std::vector<const Expression*> fs;
        if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
            at && arity > 1 && (int)at->elems.size() == arity)
          for (auto& el : at->elems) fs.push_back(el.get());
        else
          fs.push_back(k->arg->get());
        std::vector<LamPtr> fields = {v};
        std::vector<ValueKind> shape = {ValueKind::Gen};
        for (auto* ex : fs) { fields.push_back(expr(*ex)); shape.push_back(expr_kind(ex)); }
        auto b = mk(Lam::K::Prim); b->prim = Prim::Makeblock; b->prim_arg = 0;
        b->args = std::move(fields); b->blk_shape = std::move(shape);
        return b;
      }
      // a stdlib module's variant constructor (`Arg.Unit f`, or bare under
      // `open Arg`): tag/arity from the module cmi's variant decls.  Checked
      // before the Stdlib-field fallback so an opened constructor (`Bigarray`'s
      // `Int` kind) shadows a same-named Stdlib module/exception field.
      if (const CtorInfo* sci = stdlib_module_ctor(k->id.txt, n)) {
        if (!sci->is_block) return cint(sci->tag);
        std::vector<const Expression*> fs;
        if (k->arg) {
          if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
              at && sci->arity > 1 && (int)at->elems.size() == sci->arity)
            for (auto& el : at->elems) fs.push_back(el.get());
          else
            fs.push_back(k->arg->get());
        }
        return block_of(sci->tag, fs);
      }
      // A predefined exception (Not_found, Match_failure, ...): ALWAYS use the
      // predef global as its identity -- the same value Stdlib re-exports (so
      // it's correct from any module) AND the same one exn_value/raise_predef use
      // (so `raise`/`with` agree).  Crucially this is checked BEFORE the
      // stdlib_fields path: now that stdlib.cmi carries these as fields, resolving
      // them to `(field i Stdlib)` while COMPILING stdlib.ml itself
      // (`exception Match_failure = Match_failure`) would be a self-reference ->
      // "undefined global Stdlib referenced by Stdlib" at link.
      if (is_predef_exn_name(n)) {
        if (!k->arg) return predef_global(n);
        auto b = mk(Lam::K::Prim); b->prim = Prim::Makeblock; b->prim_arg = 0;
        b->args = {predef_global(n), expr(**k->arg)};
        b->blk_shape = {ValueKind::Gen, expr_kind(k->arg->get())};
        return b;
      }
      // any other Stdlib-field value (a pervasive exception/value re-export)
      if (auto sf = stdlib_fields.find(n); sf != stdlib_fields.end())
        if (!k->arg) return field_of("Stdlib", sf->second);
      // a stdlib module's exception, constructed (`Arg.Bad msg`): identity is the
      // module's export field (uppercase non-ctor exports are exceptions)
      if (auto* dq = std::get_if<Ldot>(&k->id.txt.v))
        if (auto* pl = std::get_if<Lident>(&dq->prefix->v))
          if (!module_base(pl->name)) {
            auto& fm = fields_of(pl->name);
            if (auto f = fm.find(n); f != fm.end()) {
              LamPtr idv = field_of(global_of(pl->name), f->second);
              if (!k->arg) return idv;
              if (!std::holds_alternative<Pexp_tuple>((*k->arg)->desc)) {
                auto b = mk(Lam::K::Prim); b->prim = Prim::Makeblock; b->prim_arg = 0;
                b->args = {idv, expr(**k->arg)};
                b->blk_shape = {ValueKind::Gen, expr_kind(k->arg->get())};
                return b;
              }
            }
          }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + n);  // user ctor: needs its tag (defer)
      return v;
    }
    // Binding operators: `let* p0 = e0 and* p1 = e1 ... and* pk = ek in body`
    // desugars to `(let*) ((and*) (... ((and*) e0 e1) ...) ek) (fun pat -> body)`
    // where the and-combined operand is left-nested and `pat` is the matching
    // left-nested tuple `((p0,p1),...,pk)`.  The let*/and* operators resolve as
    // ordinary values in scope (e.g. brought in by `let open Result.Syntax`).
    if (auto* lo = std::get_if<Pexp_letop>(&e.desc)) {
      auto resolve_op = [&](const StringLoc& op) -> LamPtr {
        Expression ide; ide.loc = op.loc;
        ide.desc = Pexp_ident{LongidentLoc{Longident{Lident{op.txt}}, op.loc}};
        return expr(ide);
      };
      // and-combined operand value (all operands evaluated in the outer scope).
      LamPtr acc = expr(*lo->let_.exp);
      for (auto& an : lo->ands) {
        auto ap = mk(Lam::K::Apply);
        ap->fn = resolve_op(an.op);
        ap->args = {acc, expr(*an.exp)};
        acc = ap;
      }
      // fun <left-nested tuple pattern> -> body
      auto fn = mk(Lam::K::Function);
      Ident pid = fresh("param");
      fn->params.push_back({pid, ValueKind::Gen});
      scope.emplace_back();
      std::vector<std::pair<Ident, LamPtr>> binders;
      LamPtr cur = mk(Lam::K::Var); cur->var = pid;
      for (int i = (int)lo->ands.size() - 1; i >= 0; --i) {
        collect_binders(lo->ands[i].pat, fieldimm(1, cur), binders);
        cur = fieldimm(0, cur);
      }
      collect_binders(lo->let_.pat, cur, binders);
      fn->body = wrap_binders(expr(*lo->body), binders);
      scope.pop_back();
      auto call = mk(Lam::K::Apply);
      call->fn = resolve_op(lo->let_.op);
      call->args = {acc, fn};
      return call;
    }
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
        // Inside a method body, a method-local binding (param / let, in a frame at
        // or above the method floor) shadows an instance variable, which in turn
        // shadows the class-creation / module scope.  Outside a method, plain
        // lexical lookup applies.
        if (cur_self_) {
          if (auto* b = lookup_from(l->name, method_scope_floor_)) {
            auto v = mk(Lam::K::Var); v->var = *b; return v;
          }
          if (auto iv = inst_vars_.find(l->name); iv != inst_vars_.end()) {
            auto self = mk(Lam::K::Var); self->var = *cur_self_;
            auto idv = mk(Lam::K::Var); idv->var = iv->second;
            auto fc = mk(Lam::K::Prim); fc->prim = Prim::FieldComputed; fc->args = {self, idv};
            return fc;
          }
        }
        if (auto* b = lookup(l->name)) { auto v = mk(Lam::K::Var); v->var = *b; return v; }
        if (l->name.size() > 4 && l->name[0] == '_' && l->name[1] == '_')
          if (LamPtr lp = loc_primitive(l->name, e.loc)) return lp;
        // a locally-declared external / %-builtin in value position eta-stubs
        // (`assert_bound_check2 caml_bytes_get_16 s ..` passes the prim itself)
        if (auto ex = externals_.find(l->name); ex != externals_.end())
          if (LamPtr s = prim_stub(ex->second)) return s;
        if (auto lp = local_prims_.find(l->name); lp != local_prims_.end())
          if (LamPtr s = prim_stub({lp->second.first, lp->second.second})) return s;
        // an `open M` brings M's exported values into scope (innermost first);
        // they SHADOW the pervasives (open Random; float = Random.float).
        for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
          if (LamPtr base = module_base(*it)) {  // local module (binding or alias)
            auto& lay = module_layout_[*it];
            if (auto f = lay.find(l->name); f != lay.end()) {
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {base};
              return fi;
            }
          }
          if (it->find('.') != std::string::npos) {  // opened submodule
            // a LOCAL nested submodule path (`open M.Ops`, M a functor param):
            // resolve the path and read the member from its registered layout
            if (auto mp = resolve_module_path(*it); mp.base)
              if (auto li = module_layout_.find(mp.key); li != module_layout_.end())
                if (auto f = li->second.find(l->name); f != li->second.end()) {
                  auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
                  fi->prim_arg = f->second; fi->args = {mp.base};
                  return fi;
                }
            if (LamPtr v = submodule_value(*it, l->name)) return v;  // stdlib submodule
            if (StdPrim sp = submodule_prim(*it, l->name); !sp.name.empty())
              if (LamPtr s = prim_stub(sp)) return s;
            continue;
          }
          auto& fm = fields_of(*it);  // stdlib module
          if (auto f = fm.find(l->name); f != fm.end())
            return field_of(global_of(*it), f->second);
          // an opened module's EXTERNAL member in value position -> eta-stub
          // (externals have no runtime field; e.g. `open Effect; ... perform`)
          if (StdPrim sp = value_prim(*it, l->name); !sp.name.empty())
            if (LamPtr s = prim_stub(sp)) return s;
        }
        auto sf = stdlib_fields.find(l->name);  // unqualified pervasive
        if (sf != stdlib_fields.end()) return field_of("Stdlib", sf->second);
        if (auto pi = stdlib_prims.find(l->name); pi != stdlib_prims.end())  // prim as value
          if (LamPtr s = prim_stub(pi->second)) return s;
      }
      if (auto* d = std::get_if<Ldot>(&id->id.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
          // Qualified M.x where M is a local submodule: field of its block.
          if (LamPtr base = module_base(pl->name)) {
            auto& lay = module_layout_[pl->name];
            if (auto f = lay.find(d->name); f != lay.end()) {
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {base};
              return fi;
            }
            // A submodule's EXTERNAL member (no runtime field) used as a value:
            // eta-stub the primitive (domain.ml's `Raw.get_domain_count`).  The
            // submodule's externals were registered by bare name when it was built.
            if (auto ex = externals_.find(d->name); ex != externals_.end())
              if (LamPtr s = prim_stub(ex->second)) return s;
            if (auto lp = local_prims_.find(d->name); lp != local_prims_.end())
              if (LamPtr s = prim_stub({lp->second.first, lp->second.second})) return s;
          }
          // `open StdLabels` brings `List` into scope as an alias to ListLabels;
          // a `List.x` then resolves through the alias target (shadows the plain
          // List), matching ocamlc.  Checked before the plain stdlib module.
          for (auto oit = opened_.rbegin(); oit != opened_.rend(); ++oit) {
            if (module_base(*oit)) continue;  // only stdlib opens carry aliases
            std::string tgt = stdlib_alias_target(*oit, pl->name);
            if (tgt.empty()) continue;
            auto& afm = fields_of(tgt);
            if (auto f = afm.find(d->name); f != afm.end())
              return field_of(global_of(tgt), f->second);
            StdPrim asp = value_prim(tgt, d->name);
            if (!asp.name.empty()) if (LamPtr s = prim_stub(asp)) return s;
          }
          // `open My_std` (a separately-compiled unit) brings its SUBMODULES
          // into scope, so a re-exported `module List`/`String` shadows the
          // stdlib module.  A bare `List.x` must then resolve through My_std's
          // List submodule (a DIFFERENT runtime field layout from Stdlib__List
          // -- `getglobal My_std; getfield <List>; getfield <x>`), matching
          // ocamlc.  Checked before the plain stdlib module below; harmless
          // when the open carries no such submodule (submodule_value -> null).
          for (auto oit = opened_.rbegin(); oit != opened_.rend(); ++oit) {
            if (oit->find('.') != std::string::npos) continue;
            if (LamPtr v = submodule_value(*oit + "." + pl->name, d->name)) return v;
          }
          // A local alias to a top-level stdlib module (`module B = Bytes`):
          // resolve B.x as Bytes.x.  (Dotted submodule aliases go through the
          // submodule_prim path elsewhere; here we only remap single-component
          // stdlib targets so fields_of/value_prim below take the real module.)
          std::string head = pl->name;
          if (auto sa = submod_alias_.find(head);
              sa != submod_alias_.end() && sa->second.find('.') == std::string::npos)
            head = sa->second;
          // Qualified M.x where M is a stdlib (sub)module: field of Stdlib[__M].
          auto& fm = fields_of(head);
          auto sf = fm.find(d->name);
          if (sf != fm.end()) return field_of(global_of(head), sf->second);
          // A prim used as a value (e.g. Sys.argv = %sys_argv -> (caml_sys_argv 0)).
          StdPrim sp = value_prim(head, d->name);
          if (auto pv = prim_value(sp.name)) return pv;
          // Otherwise a primitive in value position eta-expands to a stub
          // (`Int64.add` -> `(function p p stub (Int64.add p p))`).
          if (!sp.name.empty()) if (LamPtr s = prim_stub(sp)) return s;
        }
      // Qualified M.S.x through a *local* deep module path (alias chains,
      // first-class-module members): resolve the prefix, field-read the member.
      if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
        std::string pdotted;
        if (lid_to_dotted(*d->prefix, pdotted) && pdotted.find('.') != std::string::npos)
          if (auto mp = resolve_module_path(pdotted); mp.base)
            if (auto li = module_layout_.find(mp.key); li != module_layout_.end())
              if (auto f = li->second.find(d->name); f != li->second.end())
                return fieldimm(f->second, mp.base);
      }
      // Qualified M.S.x through a stdlib submodule path (Effect.Deep.continue),
      // including an opened head (`Array1.x` under `open Bigarray`) and the
      // submodule's externals (value position -> eta-stub).
      if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
        std::string dotted;
        if (lid_to_dotted(*d->prefix, dotted)) {
          dotted = expand_alias_head(dotted);  // `S.Map`/`NameMap` -> the full path
          std::vector<std::string> cands;
          if (dotted.find('.') != std::string::npos) cands.push_back(dotted);
          for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
            if (it->find('.') == std::string::npos) cands.push_back(*it + "." + dotted);
          for (auto& cand : cands) {
            // a LOCAL module's submodule (`include Int_base; Set.empty`, with
            // Int_base a functor-result module whose nested layouts we
            // registered): field-read through the registered module_layout_.
            if (auto mp = resolve_module_path(cand); mp.base)
              if (auto li = module_layout_.find(mp.key); li != module_layout_.end())
                if (auto f = li->second.find(d->name); f != li->second.end())
                  return fieldimm(f->second, mp.base);
            if (LamPtr v = submodule_value(cand, d->name)) return v;
            if (StdPrim sp = submodule_prim(cand, d->name); !sp.name.empty())
              if (LamPtr s = prim_stub(sp)) return s;
          }
        }
      }
      // A QUALIFIED value whose head module isn't bound anywhere is unbound, not a
      // best-effort `?` (which would silently miscompile to a wrong slot at link).
      if (std::holds_alternative<Ldot>(id->id.txt.v)) {
        std::string head = lid_head(id->id.txt);
        if (!module_head_resolvable(head)) {
          std::string full; lid_to_dotted(id->id.txt, full);
          unbound_module(head, full);
        }
      }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + lid_last(id->id.txt));  // unresolved (will DIFF)
      return v;
    }
    if (auto* ap = std::get_if<Pexp_apply>(&e.desc)) {
      Prim p;
      // A record literal passed to `Mod.f` is disambiguated by f's parameter type
      // (the record's labels need not be in scope -- no `open Mod`).  Eagerly load
      // Mod's record-type labels so the argument literal resolves to its layout
      // instead of collapsing to 0 (maindriver's `process_deferred_actions {..}`).
      {
        bool has_record_arg = false;
        for (auto& a : ap->args)
          if (std::get_if<Pexp_record>(&a.second->desc)) { has_record_arg = true; break; }
        if (has_record_arg)
          if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
            if (auto* d = std::get_if<Ldot>(&fid->id.txt.v))
              if (auto* pl = std::get_if<Lident>(&d->prefix->v))
                register_module_records(pl->name);
      }
      // Likewise, a BARE constructor inside an argument to `Mod.f` is disambiguated
      // by f's parameter type (no `open Mod`): register Mod's ctor for it
      // (btype.ml's `Misc.protect_refs [R(pool_stack, pool)] f`).
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* d = std::get_if<Ldot>(&fid->id.txt.v))
          if (auto* pl = std::get_if<Lident>(&d->prefix->v); pl && !module_base(pl->name))
            for (auto& a : ap->args) scan_expr_ctors_from(*a.second, pl->name);
      // `__LOC_OF__ e` / `__LINE_OF__ e` / `__POS_OF__ e`: a pair of the argument's
      // location info and the argument itself.
      if (ap->args.size() == 1)
        if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
          if (auto* fl = std::get_if<Lident>(&fid->id.txt.v)) {
            const Expression& a = *ap->args[0].second;
            const Location& al = e.loc;  // the WHOLE `__X_OF__ e` application's loc
            if (fl->name == "__LOC_OF__")
              return cblock(0, {cstr(loc_string(al)), expr(a)});
            if (fl->name == "__LINE_OF__")
              return cblock(0, {cint(al.start.lnum), expr(a)});
            if (fl->name == "__POS_OF__")
              return cblock(0, {cblock(0, {cstr(file_name_), cint(al.start.lnum),
                                          cint(al.start.cnum - al.start.bol),
                                          cint(al.end.cnum - al.end.bol)}), expr(a)});
          }
      // A method call `o#m a b` is a single send carrying its arguments.
      if (auto* sd = std::get_if<Pexp_send>(&ap->fn->desc)) {
        bool simple = true;
        for (auto& a : ap->args) if (!std::holds_alternative<Nolabel>(a.first)) simple = false;
        if (simple) {
          std::vector<LamPtr> args;
          for (auto& a : ap->args) args.push_back(expr(*a.second));
          return send_expr(*sd, std::move(args));
        }
      }
      // `new c arg..` is one application: (apply (field_mut 0 c) 0 arg..).
      // A class with labelled/optional params goes through apply_labeled
      // (reorder, Some-wrap, None-fill) against its recorded signature.
      if (auto* nw = std::get_if<Pexp_new>(&ap->fn->desc)) {
        if (auto* l = std::get_if<Lident>(&nw->id.txt.v))
          if (auto* b = lookup(l->name))
            if (auto it = fn_sig_.find(b->stamp); it != fn_sig_.end())
              if (auto r = apply_labeled(ap->fn.get(), it->second, *ap)) return r;
        bool simple = true;
        for (auto& a : ap->args) if (!std::holds_alternative<Nolabel>(a.first)) simple = false;
        if (simple) {
          LamPtr nw2 = expr(*ap->fn);
          if (nw2->k == Lam::K::Apply) {
            for (auto& a : ap->args) nw2->args.push_back(expr(*a.second));
            return nw2;
          }
        }
      }
      // Qualified module primitives: Array.get/set (kind-annotated), String/Bytes
      // length/get/set.  `x.(i)` / `s.[i]` desugar to these.
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* d = std::get_if<Ldot>(&fid->id.txt.v))
          if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
            auto& as = ap->args;
            const std::string& m = pl->name;
            const std::string& f = d->name;
            std::string op;
            if (m == "Array" && f == "length" && as.size() == 1)
              op = "array.length[" + array_arg_kind(as[0].second.get()) + "]";
            else if (m == "Array" && f == "get" && as.size() == 2)
              op = "array.get[" + array_elem_kind(&e) + "]";
            else if (m == "Array" && f == "unsafe_get" && as.size() == 2)
              op = "array.unsafe_get[" + array_elem_kind(&e) + "]";
            else if (m == "Array" && f == "set" && as.size() == 3)
              op = "array.set[" + array_elem_kind(as[2].second.get()) + "]";
            else if (m == "Array" && f == "unsafe_set" && as.size() == 3)
              op = "array.unsafe_set[" + array_elem_kind(as[2].second.get()) + "]";
            else if (m == "String" && f == "length" && as.size() == 1) op = "string.length";
            else if (m == "String" && f == "get" && as.size() == 2) op = "string.get";
            else if (m == "String" && f == "unsafe_get" && as.size() == 2) op = "string.unsafe_get";
            else if (m == "Bytes" && f == "length" && as.size() == 1) op = "bytes.length";
            else if (m == "Bytes" && f == "get" && as.size() == 2) op = "bytes.get";
            else if (m == "Bytes" && f == "set" && as.size() == 3) op = "bytes.set";
            else if (m == "Bytes" && f == "unsafe_get" && as.size() == 2) op = "bytes.unsafe_get";
            else if (m == "Bytes" && f == "unsafe_set" && as.size() == 3) op = "bytes.unsafe_set";
            if (!op.empty()) {
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp; pr->prim_id = op;
              for (auto& a : as) pr->args.push_back(expr(*a.second));
              return pr;
            }
            // General: an `external` value resolved via its cmi prim_name.
            if (auto prim = value_prim(m, f); !prim.name.empty())
              if (auto r = prim_apply(prim.name, prim.arity, *ap, e)) return r;
          }
      // Nested-prefix qualified externals (`Bigarray.Array1.get a i`, or
      // `Array1.get` under `open Bigarray`): the submodule's prim via the cmi
      // signature chain.
      if (auto* fid2 = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (std::get_if<Ldot>(&fid2->id.txt.v)) {
          std::string full;
          if (lid_to_dotted(fid2->id.txt, full)) {
            size_t lastd = full.rfind('.');
            std::string pre = full.substr(0, lastd), nm = full.substr(lastd + 1);
            std::string head = pre.substr(0, pre.find('.'));
            if (!lookup(head) && !module_base(head)) {
              std::vector<std::string> cands;
              if (pre.find('.') != std::string::npos) cands.push_back(pre);
              for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
                if (it->find('.') == std::string::npos) cands.push_back(*it + "." + pre);
              for (auto& cand : cands)
                if (StdPrim sp = submodule_prim(cand, nm); !sp.name.empty())
                  if (auto r = prim_apply(sp.name, sp.arity, *ap, e)) return r;
            }
          }
        }
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* l = std::get_if<Lident>(&fid->id.txt.v))
          if (!lookup(l->name) && !opened_has(l->name)) {  // an unshadowed pervasive
            const auto& n = l->name;
            auto& as = ap->args;
            if (auto ex = externals_.find(n); ex != externals_.end()) {
              // A %-builtin external (e.g. `external f = "%atomic_compare_and_set"`)
              // is NOT a C function -- route it through prim_to_lam (a Ccall to
              // "%foo" would be a bogus undefined primitive).
              if (!ex->second.name.empty() && ex->second.name[0] == '%') {
                if (auto r = prim_apply(ex->second.name, ex->second.arity, *ap, e)) return r;
              } else {  // a real C external
                auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall;
                pr->prim_id = ex->second.name;
                for (auto& a : as) pr->args.push_back(expr(*a.second));
                if (pr->prim_id == "caml_obj_with_tag")
                  if (auto r = fold_with_tag(pr->args)) return r;
                return pr;
              }
            }
            if (auto lp = local_prims_.find(n); lp != local_prims_.end())
              if (auto r = prim_apply(lp->second.first, lp->second.second, *ap, e))
                return r;
            // an opened module's external member applied directly (its fields
            // are handled by the generic path; opened_has excludes prims)
            for (auto it2 = opened_.rbegin(); it2 != opened_.rend(); ++it2) {
              bool dotted = it2->find('.') != std::string::npos;
              StdPrim sp = dotted ? submodule_prim(*it2, n) : value_prim(*it2, n);
              if (!sp.name.empty()) {
                if (auto r = prim_apply(sp.name, sp.arity, *ap, e)) return r;
                break;
              }
              if (dotted ? submodule_of(*it2).fields.count(n) > 0
                         : fields_of(*it2).count(n) > 0)
                break;  // a field here shadows outer opens
            }
            if (n == "raise" && as.size() == 1) {
              // raise's argument is exn-typed: a registered exception wins over
              // a same-named variant ctor (`type t = E` after `exception E`).
              raise_arg_ = true;
              LamPtr arg = expr(*as[0].second);
              raise_arg_ = false;
              // raising the innermost caught exception re-raises (keeps backtrace).
              bool reraise = !caught_exn_.empty() && arg->k == Lam::K::Var &&
                             arg->var.stamp == caught_exn_.back().stamp;
              auto pr = mk(Lam::K::Prim);
              pr->prim = reraise ? Prim::Reraise : Prim::Raise;
              pr->args = {arg};
              return pr;
            }
            if (int_op(n, p) && as.size() == 2) {
              auto pr = mk(Lam::K::Prim); pr->prim = p;
              pr->args = {expr(*as[0].second), expr(*as[1].second)};
              return pr;
            }
            if (auto pp = pervasive_prim(n); !pp.first.empty() && (int)as.size() == pp.second) {
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp; pr->prim_id = pp.first;
              for (auto& a : as) pr->args.push_back(expr(*a.second));
              return pr;
            }
            // Polymorphic comparison ops: specialize to an integer comparison
            // when an operand is an immediate, else a caml_* C compare.
            if (auto c = poly_cmp(n); !c.first.empty() && as.size() == 2) {
              // A string operand selects the string compare (caml_string_lessthan,
              // ...): the polymorphic caml_* name with the `string` infix.
              if (expr_is_string(as[0].second.get()) || expr_is_string(as[1].second.get())) {
                auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall;
                pr->prim_id = "caml_string_" + c.second.substr(5);  // drop "caml_"
                pr->args = {expr(*as[0].second), expr(*as[1].second)};
                return pr;
              }
              LamPtr e0 = expr(*as[0].second), e1 = expr(*as[1].second);
              // The operand kind drives the spelling: int -> `==`, float -> `==.`,
              // int64/int32/nativeint -> `Int64.==` etc.; two generics fall back to
              // the polymorphic caml_* compare.
              ValueKind k = ValueKind::Gen;
              for (ValueKind kk : {expr_kind(as[0].second.get()), expr_kind(as[1].second.get())})
                if (kk != ValueKind::Gen) k = kk;
              // Equality with an immediate constant of a generic type (`x = None`,
              // `x = []`) is physical -- comparing any value with an immediate is the
              // `==`/`!=` int test.  Only when the kind is generic: a boxed-int
              // literal (`0n`/`0l`/`0L`) is ConstInt too but keeps its `Int64.==`
              // spelling, which the kind path below provides.
              auto is_imm = [](const LamPtr& l) {
                return l->k == Lam::K::ConstInt || l->k == Lam::K::ConstChar;
              };
              if (k == ValueKind::Gen && (n == "=" || n == "<>") && (is_imm(e0) || is_imm(e1))) {
                auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp; pr->prim_id = c.first;
                pr->args = {e0, e1}; return pr;
              }
              auto pr = mk(Lam::K::Prim);
              if (k == ValueKind::Gen) { pr->prim = Prim::Ccall; pr->prim_id = c.second; }
              else {
                pr->prim = Prim::IntCmp;
                pr->prim_id = k == ValueKind::Int ? c.first
                            : k == ValueKind::Float ? c.first + "."
                            : k == ValueKind::Boxedint64 ? "Int64." + c.first
                            : k == ValueKind::Boxedint32 ? "Int32." + c.first
                            : "Nativeint." + c.first;
              }
              pr->args = {e0, e1};
              return pr;
            }
            // Physical equality and short-circuit boolean ops are always inlined.
            if ((n == "==" || n == "!=" || n == "&&" || n == "||") && as.size() == 2) {
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::IntCmp; pr->prim_id = n;
              pr->args = {expr(*as[0].second), expr(*as[1].second)};
              return pr;
            }
            if (n == "ref" && as.size() == 1) {  // (makemutable 0 (shape) e)
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Makemutable; pr->prim_arg = 0;
              pr->blk_shape = {expr_kind(as[0].second.get())};
              pr->args = {expr(*as[0].second)};
              return pr;
            }
            if (n == "!" && as.size() == 1) {  // deref: field_int / field_mut 0
              auto pr = mk(Lam::K::Prim);
              pr->prim = expr_kind(&e) == ValueKind::Int ? Prim::FieldInt : Prim::FieldMut;
              pr->prim_arg = 0; pr->args = {expr(*as[0].second)};
              return pr;
            }
            if (n == ":=" && as.size() == 2) {  // setfield_imm / setfield_ptr 0
              auto pr = mk(Lam::K::Prim);
              pr->prim = expr_kind(as[1].second.get()) == ValueKind::Int
                             ? Prim::SetfieldImm : Prim::SetfieldPtr;
              pr->prim_arg = 0;
              pr->args = {expr(*as[0].second), expr(*as[1].second)};
              return pr;
            }
            if ((n == "incr" || n == "decr") && as.size() == 1) {  // +:=1 / +:=-1
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Offsetref;
              pr->prim_arg = n == "incr" ? 1 : -1;
              pr->args = {expr(*as[0].second)};
              return pr;
            }
            if (n == "perform" && as.size() == 1) {  // Effect.perform e -> (perform e)
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = "perform";
              pr->args = {expr(*as[0].second)};
              return pr;
            }
            // General: an unqualified pervasive `external` (e.g. compare, ignore)
            // resolved via its cmi prim_name.
            if (auto pi = stdlib_prims.find(n); pi != stdlib_prims.end())
              if (auto r = prim_apply(pi->second.name, pi->second.arity, *ap, e)) return r;
            // an `open M`'d external (e.g. Marshal.(to_string ...)): M's prim.
            for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
              if (auto p = value_prim(*it, n); !p.name.empty())
                if (auto r = prim_apply(p.name, p.arity, *ap, e)) return r;
          }
      // A call to a labelled/optional function (local or a qualified stdlib value):
      // reorder the arguments to parameter order, wrap/insert optionals.
      if (FnSig sig = callee_sig(ap->fn.get()); !sig.empty())
        if (auto r = apply_labeled(ap->fn.get(), sig, *ap)) return r;
      auto a = mk(Lam::K::Apply);
      a->fn = expr(*ap->fn);
      const std::vector<std::string>* packs = nullptr;
      if (auto* fid3 = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* l3 = std::get_if<Lident>(&fid3->id.txt.v))
          if (auto* b3 = lookup(l3->name))
            if (auto it3 = fn_pack_params_.find(b3->stamp); it3 != fn_pack_params_.end())
              packs = &it3->second;
      size_t ai = 0;
      for (auto& [lbl, arg] : ap->args) {
        LamPtr av = expr(*arg);
        // an un-annotated `(module M)` argument coerces to the callee
        // parameter's package type (recorded off `(module P : S)` params)
        if (packs && ai < packs->size() && !(*packs)[ai].empty())
          if (auto* pk = std::get_if<Pexp_pack>(&arg->desc); pk && !pk->pkg)
            av = pack_coerce(std::move(av), *pk->me, (*packs)[ai]);
        a->args.push_back(std::move(av));
        ++ai;
      }
      a->inline_attr = inline_of_named(ap->fn->attrs, "inlined");  // (f [@inlined never]) x
      if (has_attr(ap->fn->attrs, "tailcall")) {  // (f [@tailcall]) x -> ... tailcall
        if (!a->inline_attr.empty()) a->inline_attr += " ";
        a->inline_attr += "tailcall";
      }
      return a;
    }
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) {
      // an ANONYMOUS function (not named by a let binding) is `<enclosing>.(fun)`
      // for __FUNCTION__; a named one already has its name on func_path_.  Nested
      // anonymous functions don't stack `.(fun).(fun)` (ocamlc appends one).
      bool anon = !named_funcs_.count(f) &&
                  (func_path_.empty() || func_path_.back() != "(fun)");
      if (anon) func_path_.push_back("(fun)");
      LamPtr v = function(*f, e.loc);
      if (anon) func_path_.pop_back();
      return v;
    }
    if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      // `let _ = e in body` discards e -> seq, not a binding.
      if (le->bindings.size() == 1 &&
          std::holds_alternative<Ppat_any>(le->bindings[0].pat.desc)) {
        auto sq = mk(Lam::K::Sequence);
        sq->cond = expr(*le->bindings[0].expr);
        rec_spine_ = rec_spine;
        sq->else_ = expr(*le->body);
        return sq;
      }
      // `let x = E in x` -> E: a linear alias binding ocamlc's simplif drops.
      if (!rec_spine && le->rf != RecFlag::Recursive && le->bindings.size() == 1)
        if (auto* pv = std::get_if<Ppat_var>(&le->bindings[0].pat.desc))
          if (auto* bid = std::get_if<Pexp_ident>(&le->body->desc))
            if (auto* bl = std::get_if<Lident>(&bid->id.txt.v); bl && bl->name == pv->name.txt)
              // keep the binding's name (for __FUNCTION__) and attributes (a
              // `let[@inline never] f = fun.. in f` must keep never_inline).
              return fn_binding_rhs(pv->name.txt, *le->bindings[0].expr,
                                    le->bindings[0].attrs);
      scope.emplace_back();
      if (le->rf == RecFlag::Recursive) {  // names in scope within their RHSs
        std::vector<std::pair<const ValueBinding*, Ident>> recs;
        for (auto& b : le->bindings)
          if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
            Ident id = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = id;
            record_fn_sig(id, b.expr.get());
            recs.push_back({&b, id});
          }
        std::vector<Ident> ids; std::vector<ValueKind> kinds; std::vector<LamPtr> vals;
        for (auto& [b, id] : recs) {
          ids.push_back(id);
          kinds.push_back(pat_kind(&b->pat));
          rec_spine_ = true;
          vals.push_back(expr(*b->expr));
        }
        RecParts rp;
        if (!recs.empty() && partition_rec(ids, kinds, vals, rp)) {
          LamPtr body = expr(*le->body);
          for (auto u = rp.updates.rbegin(); u != rp.updates.rend(); ++u) {
            auto sq = mk(Lam::K::Sequence); sq->cond = *u; sq->else_ = body; body = sq;
          }
          if (!rp.funcs.empty()) {
            auto lr = mk(Lam::K::Letrec);
            lr->bindings = std::move(rp.funcs); lr->body = body; body = lr;
          }
          auto l = mk(Lam::K::Let);
          l->bindings = std::move(rp.dummies); l->body = body;
          scope.pop_back(); return l;
        }
        auto l = mk(Lam::K::Letrec);
        for (size_t i = 0; i < recs.size(); ++i)
          l->bindings.push_back({ids[i], kinds[i], collapse_let_id(vals[i])});
        l->body = expr(*le->body);
        scope.pop_back();
        return l;
      }
      // `let r = ref E in body`: if r never escapes (used only as `!r` / `r := e`
      // / `incr`/`decr`), it becomes a mutable local variable -- `=mut[k]` with
      // `*r` reads and `(assign r e)` writes -- instead of a heap ref.
      if (le->bindings.size() == 1)
        if (auto* pv = std::get_if<Ppat_var>(&le->bindings[0].pat.desc))
          if (const Expression* init = ref_call(*le->bindings[0].expr)) {
            Ident rid = fresh(pv->name.txt);
            ValueKind k = expr_kind(init);
            // the init sees the OUTER scope: `let b = ref b` reads the shadowed
            // binding, not itself (this once self-referenced -> garbage init)
            LamPtr iv = expr(*init);
            scope.back()[pv->name.txt] = rid;
            rec_spine_ = rec_spine;
            LamPtr body = expr(*le->body);
            auto l = mk(Lam::K::Let);
            if (ref_escapes(body, rid)) {  // escapes -> stays a heap ref
              auto mm = mk(Lam::K::Prim); mm->prim = Prim::Makemutable;
              mm->prim_arg = 0; mm->blk_shape = {k}; mm->args = {iv};
              l->bindings = {{rid, ValueKind::Gen, mm}};
            } else {  // mutable local
              ref_rewrite(body, rid);
              l->bindings = {{rid, k, iv, false, true}};
            }
            l->body = body;
            scope.pop_back();
            return l;
          }
      // `let (a, b) = match .. in body`: when every match arm RESULT is a
      // syntactic tuple, pass the components through a static catch instead of
      // building the pair (Matching's exit-with-args form; basic/tuple_match).
      if (le->bindings.size() == 1)
        if (auto* tp = std::get_if<Ppat_tuple>(
                &effective_pat(&le->bindings[0].pat)->desc)) {
          bool allvars = !tp->elems.empty();
          for (auto& el : tp->elems) {
            const Pattern* ep = effective_pat(el.get());
            if (!std::get_if<Ppat_var>(&ep->desc) &&
                !std::holds_alternative<Ppat_any>(ep->desc)) {
              allvars = false;
              break;
            }
          }
          const Expression* rhs = le->bindings[0].expr.get();
          while (auto* ct = std::get_if<Pexp_constraint>(&rhs->desc)) rhs = ct->e.get();
          if (allvars && (std::get_if<Pexp_match>(&rhs->desc) ||
                          std::get_if<Pexp_try>(&rhs->desc) ||
                          std::get_if<Pexp_ifthenelse>(&rhs->desc))) {
            LamPtr mm = expr(*rhs);
            int n = ++next_exit_;
            if (tail_tuple_exit(mm, n, tp->elems.size())) {
              auto cat = mk(Lam::K::Catch);
              cat->cond = mm;
              cat->prim_arg = n;
              for (auto& el : tp->elems) {
                const Pattern* ep = effective_pat(el.get());
                Ident id;
                if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) {
                  id = fresh(pv->name.txt);
                  scope.back()[pv->name.txt] = id;
                } else {
                  id = fresh("", true);
                }
                cat->catch_vars.push_back(id);
                cat->catch_var_kinds.push_back(pat_kind(ep));
              }
              rec_spine_ = rec_spine;
              cat->then_ = expr(*le->body);
              scope.pop_back();
              return cat;
            }
            // partially-rewritten translation discarded; fall through to the
            // allocating path, which re-translates from the AST
          }
        }
      // `let <refutable> = e in body`: a partial pattern match over e raising
      // Match_failure on the missing cases (located at the let expression).  e is
      // bound to a *match* temp unless it is already a variable.
      if (le->bindings.size() == 1) {
        auto& b = le->bindings[0];
        if (!std::holds_alternative<Ppat_var>(b.pat.desc) &&
            !std::holds_alternative<Ppat_any>(b.pat.desc) && !is_irrefutable(b.pat)) {
          // `let (P1 | P2) = e in body`: an or-pattern binding -- switch into a
          // tuple of the bound values, project each, then the body.
          if (const Pattern* ep = effective_pat(&b.pat);
              std::holds_alternative<Ppat_or>(ep->desc)) {
            LamPtr v0 = expr(*b.expr); LamPtr scrut = v0; Ident mtmp;
            if (v0->k != Lam::K::Var) { mtmp = fresh("", true); auto tv = mk(Lam::K::Var); tv->var = mtmp; scrut = tv; }
            std::vector<std::string> order; LamPtr orval;
            if (or_pattern_values(*ep, scrut, b.pat.loc, order, orval)) {
              auto outer = mk(Lam::K::Let);
              if (v0->k != Lam::K::Var) outer->bindings.push_back({mtmp, ValueKind::Gen, v0});
              if (order.size() == 1) {
                Ident vid = fresh(order[0]); scope.back()[order[0]] = vid;
                outer->bindings.push_back({vid, ValueKind::Gen, orval});
              } else {
                Ident tupid = fresh("", true);
                outer->bindings.push_back({tupid, ValueKind::Gen, orval});
                auto tv = mk(Lam::K::Var); tv->var = tupid;
                for (size_t i = 0; i < order.size(); ++i) {
                  Ident vid = fresh(order[i]); scope.back()[order[i]] = vid;
                  outer->bindings.push_back({vid, ValueKind::Gen, fieldimm((int)i, tv), true});
                }
              }
              outer->body = expr(*le->body);
              scope.pop_back();
              return outer;
            }
          }
          LamPtr val = expr(*b.expr);
          auto wrap = mk(Lam::K::Let); LamPtr scrut = val;
          if (val->k != Lam::K::Var) {
            Ident tmp = fresh("", true);
            wrap->bindings.push_back({tmp, ValueKind::Gen, val});
            auto tv = mk(Lam::K::Var); tv->var = tmp; scrut = tv;
          }
          std::vector<Row> rows = {{&b.pat, le->body.get(), nullptr}};
          LamPtr m = compile_match(scrut, rows, e.loc);
          scope.pop_back();
          if (wrap->bindings.empty()) return m;
          wrap->body = m; return wrap;
        }
      }
      auto l = mk(Lam::K::Let);
      std::vector<std::pair<Ident, LamPtr>> binders;  // sub-vars of destructured pats
      for (auto& b : le->bindings) {
        if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
          // pass b.attrs so a local `let[@inline never] f = ..` carries its
          // never_inline attribute onto the function (as the top-level path does).
          LamPtr rhs = fn_binding_rhs(pv->name.txt, *b.expr, b.attrs);
          // simplif drops `let x = (var w)` unconditionally (any let-kind),
          // substituting x by w -- so alias the name to w rather than binding it.
          // Not on a recursive binding's spine, though: the rec-value compiler
          // chooses its strategy (dummy-context vs direct letrec) from the RHS's
          // SYNTACTIC shape, before simplif inlines -- inlining here would flip it.
          if (rhs->k == Lam::K::Var && !rec_spine) {
            scope.back()[pv->name.txt] = rhs->var;
            record_fn_sig(rhs->var, b.expr.get());
            continue;
          }
          Ident id = fresh(pv->name.txt);
          Lam::Binding bd{id, pat_kind(&b.pat), std::move(rhs)};
          l->bindings.push_back(std::move(bd));
          // record BEFORE binding the name: in this non-recursive let, the RHS's
          // own references resolve to the OUTER binding (`let f .. = f ..` wrapping
          // an outer f), which is what the residual-signature analysis must see.
          record_fn_sig(id, b.expr.get());
          record_record_lit(id, b.expr.get());
          scope.back()[pv->name.txt] = id;
        } else {  // `let (a,b) = e` / `let {a;b} = e`: the irrefutable sub-vars read
          LamPtr val = expr(*b.expr);  // fields of e -- directly when e is a var, else
          if (val->k == Lam::K::Var) {  // via a *match* temp bound to e
            collect_binders(b.pat, val, binders);
          } else {
            Ident tmp = fresh("", true);
            l->bindings.push_back({tmp, ValueKind::Gen, val});
            auto tv = mk(Lam::K::Var); tv->var = tmp;
            collect_binders(b.pat, tv, binders);
          }
          record_tuple_sigs(b.pat, *b.expr);
        }
      }
      rec_spine_ = rec_spine;
      LamPtr body = wrap_binders(expr(*le->body), binders);
      scope.pop_back();
      if (l->bindings.empty()) return body;  // all bindings were field reads of a var
      l->body = body;
      return l;
    }
    if (auto* it = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      auto l = mk(Lam::K::IfThenElse);
      l->cond = expr(*it->cond);
      l->then_ = expr(*it->then_);
      l->else_ = it->else_ ? expr(**it->else_) : mk(Lam::K::ConstInt);  // () = 0
      return l;
    }
    if (auto* sq = std::get_if<Pexp_sequence>(&e.desc)) {
      auto l = mk(Lam::K::Sequence);
      l->cond = expr(*sq->e1);
      rec_spine_ = rec_spine;
      l->else_ = expr(*sq->e2);
      return l;
    }
    if (auto* wh = std::get_if<Pexp_while>(&e.desc)) {
      auto l = mk(Lam::K::While);
      l->cond = expr(*wh->cond); l->body = expr(*wh->body);
      return l;
    }
    if (auto* fo = std::get_if<Pexp_for>(&e.desc)) {
      auto l = mk(Lam::K::For);
      l->then_ = expr(*fo->lo); l->else_ = expr(*fo->hi);
      l->downto_ = fo->dir == DirectionFlag::Downto;
      scope.emplace_back();
      if (auto* pv = std::get_if<Ppat_var>(&fo->var.desc)) {
        l->var = fresh(pv->name.txt); scope.back()[pv->name.txt] = l->var;
      } else {
        l->var = fresh("_for");  // `for _ = ...`: ocaml names the index `_for`
      }
      l->body = expr(*fo->body);
      scope.pop_back();
      return l;
    }
    if (auto* tr = std::get_if<Pexp_try>(&e.desc)) {
      auto l = mk(Lam::K::Try);
      l->body = expr(*tr->e);
      scope.emplace_back();
      // Register constructors named in the handler patterns (the match path does
      // this at scan_pat_ctors; try/with skipped it).  A payload ctor like EINTR
      // in `with Unix_error (EINTR, _, _) ->` (from `open Unix`) is otherwise
      // unresolved, so exn_case_body bails and the WHOLE arm is silently dropped
      // to a bare reraise -- e.g. ocamlbuild's select() EINTR-retry never retried.
      for (auto& c : tr->cases) scan_pat_ctors(c.lhs);
      // `with e -> body` (a single catch-all var) binds `e` directly; otherwise a
      // synthetic `exn` is matched against the cases.
      const Ppat_var* pv = nullptr;
      if (tr->cases.size() == 1 && !tr->cases[0].guard)
        pv = std::get_if<Ppat_var>(&tr->cases[0].lhs.desc);
      if (pv) {
        l->var = fresh(pv->name.txt);
        scope.back()[pv->name.txt] = l->var;
        caught_exn_.push_back(l->var);
        l->then_ = expr(*tr->cases[0].rhs);
      } else {
        l->var = fresh("exn");
        caught_exn_.push_back(l->var);
        l->then_ = exn_dispatch(l->var, rows_of(tr->cases), 0);
      }
      caught_exn_.pop_back();
      scope.pop_back();
      return l;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) return expr(*ct->e);
    if (auto* nt = std::get_if<Pexp_newtype>(&e.desc)) return expr(*nt->body);  // (type a) -> e erased
    if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) return expr(*co->e);  // (e :> t) erased
    if (auto* pp = std::get_if<Pexp_pack>(&e.desc)) {  // (module ME): the module value
      LamPtr mv = compile_module_expr(*pp->me);
      if (pp->pkg) {  // coerce to the package type's layout when it differs
        std::string mt;
        if (lid_to_dotted(pp->pkg->path.txt, mt)) return pack_coerce(mv, *pp->me, mt);
      } else if (!unpack_pkg_.empty()) {  // package type from an enclosing (val..:S)
        std::string mt = unpack_pkg_, saved = unpack_pkg_;
        unpack_pkg_.clear();  // don't leak into ME's own contents
        LamPtr c = pack_coerce(mv, *pp->me, mt);
        unpack_pkg_ = saved;
        return c;
      }
      return mv;
    }
    if (auto* sd = std::get_if<Pexp_send>(&e.desc)) return send_expr(*sd, {});
    if (auto* si = std::get_if<Pexp_setinstvar>(&e.desc)) {  // n <- e (in a method)
      if (cur_self_) if (auto iv = inst_vars_.find(si->name.txt); iv != inst_vars_.end()) {
        auto self = mk(Lam::K::Var); self->var = *cur_self_;
        auto idv = mk(Lam::K::Var); idv->var = iv->second;
        auto sf = mk(Lam::K::Prim); sf->prim = Prim::SetfieldComputed;
        sf->prim_id = value_is_immediate(si->value.get())
                        ? "setfield_imm_computed" : "setfield_ptr_computed";
        sf->args = {self, idv, expr(*si->value)};
        return sf;
      }
    }
    if (auto* ob = std::get_if<Pexp_object>(&e.desc))
      if (LamPtr o = object_expr(*ob->cs)) return o;
    // [%extension_constructor M.A]: the constructor's runtime identity -- the
    // payload constructor expression's own (unapplied) value.
    if (auto* xe = std::get_if<Pexp_extension>(&e.desc)) {
      if ((xe->name == "extension_constructor" ||
           xe->name == "ocaml.extension_constructor") &&
          xe->payload.str.size() == 1)
        if (auto* ev = std::get_if<Pstr_eval>(&xe->payload.str[0].desc))
          return expr(*ev->e);
      // `[%atomic.loc e.field]`: an atomic LOCATION (OCaml 5.2+ atomic API), which
      // we represent as the pair (obj, field-index); the %atomic_*_loc primitives
      // consume that pair (-> caml_atomic_*_field obj field ..).  Used by atomic.ml
      // (Atomic.fetch_and_add etc. via Loc.* [%atomic.loc t.contents]).
      if ((xe->name == "atomic.loc" || xe->name == "ocaml.atomic.loc") &&
          xe->payload.str.size() == 1)
        if (auto* ev = std::get_if<Pstr_eval>(&xe->payload.str[0].desc))
          if (auto* fa = std::get_if<Pexp_field>(&ev->e->desc)) {
            int idx = 0;
            if (auto* fi = find_field(lid_last(fa->field.txt))) idx = fi->index;
            auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
            blk->args = {expr(*fa->e), cint(idx)};
            return blk;
          }
    }
    if (auto* nw = std::get_if<Pexp_new>(&e.desc)) {  // new c -> (apply (field_mut 0 c) 0)
      LamPtr clsval;
      if (auto* l = std::get_if<Lident>(&nw->id.txt.v)) {
        if (auto* b = lookup(l->name)) clsval = varof(*b);
        if (!clsval)  // an include/open'd module's class (include E; new c)
          for (auto it = opened_.rbegin(); !clsval && it != opened_.rend(); ++it)
            if (LamPtr base = module_base(*it)) {
              auto& lay = module_layout_[*it];
              if (auto f = lay.find(l->name); f != lay.end())
                clsval = fieldimm(f->second, base);
            }
      } else if (std::get_if<Ldot>(&nw->id.txt.v)) {
        std::string dotted;  // a class at a (possibly deep) local module path
        if (lid_to_dotted(nw->id.txt, dotted))
          if (auto mp = resolve_module_path(dotted); mp.base) clsval = mp.base;
      }
      if (clsval) {
        auto fm = mk(Lam::K::Prim); fm->prim = Prim::FieldMut; fm->prim_arg = 0;
        fm->args = {clsval};
        auto ap = mk(Lam::K::Apply); ap->fn = fm; ap->args = {cint(0)};
        return ap;
      }
    }
    return mk(Lam::K::ConstInt);  // unsupported: placeholder (will DIFF)
  }

  // Translobj.share: a hoisted module-global `shared` const-block of label
  // strings, reused across the module; single-use ones inline away later.  An
  // empty list is the integer 0.
  LamPtr transl_meth_list(const std::vector<std::string>& labels) {
    if (labels.empty()) return cint(0);
    std::string key;
    for (auto& s : labels) { key += std::to_string(s.size()); key += ':'; key += s; }
    Ident id;
    if (auto it = shared_index_.find(key); it != shared_index_.end()) id = it->second;
    else {
      id = fresh("shared");
      auto blk = mk(Lam::K::ConstBlock); blk->prim_arg = 0;
      for (auto& s : labels) { auto c = mk(Lam::K::ConstString); c->str_val = s; blk->args.push_back(c); }
      Lam::Binding b; b.id = id; b.kind = ValueKind::Gen; b.val = blk; b.alias = true;
      shared_consts_.push_back({key, b});
      shared_index_[key] = id;
    }
    auto v = mk(Lam::K::Var); v->var = id; return v;
  }

  // A CamlinternalOO value as `(field_imm idx (global CamlinternalOO!))`.
  LamPtr oo_prim(const std::string& name) {
    auto& fm = fields_of("CamlinternalOO");
    auto it = fm.find(name);
    return field_of("CamlinternalOO", it == fm.end() ? 0 : it->second);
  }
  LamPtr oo_call(const std::string& name, std::vector<LamPtr> args) {
    auto a = mk(Lam::K::Apply); a->fn = oo_prim(name); a->args = std::move(args); return a;
  }

  // e#m, possibly applied to `args`: a public `(send obj tag args..)`, or
  // `(sendself self m args..)` when the receiver is the enclosing method's self
  // and `m` is one of the current object's methods.
  LamPtr send_expr(const ast::Pexp_send& sd, std::vector<LamPtr> args) {
    // `super#m`: apply the parent's method closure to self -- not a send.
    if (cur_self_ && !cur_super_name_.empty())
      if (auto* id = std::get_if<Pexp_ident>(&sd.obj->desc))
        if (auto* l = std::get_if<Lident>(&id->id.txt.v))
          if (l->name == cur_super_name_ && cur_super_mid_.count(sd.meth.txt)) {
            auto ap = mk(Lam::K::Apply);
            ap->fn = varof(cur_super_mid_[sd.meth.txt]);
            ap->args = {varof(*cur_self_)};
            for (auto& a : args) ap->args.push_back(a);
            return ap;
          }
    LamPtr obj = expr(*sd.obj);
    auto s = mk(Lam::K::Prim); s->prim = Prim::Send;
    if (cur_self_ && obj->k == Lam::K::Var && obj->var.stamp == cur_self_->stamp &&
        cur_meth_id_.count(sd.meth.txt)) {
      s->prim_id = "sendself";
      s->args = {obj, varof(cur_meth_id_[sd.meth.txt])};
    } else {
      s->prim_id = "send";
      s->args = {obj, cint(hash_variant(sd.meth.txt))};
    }
    for (auto& a : args) s->args.push_back(a);
    return s;
  }

  LamPtr object_expr(const ast::ClassStructure& cs) { return build_object(cs, false); }

  // Record a class's shape (vals / methods / virtual-vs-concrete split) so a
  // later `inherit` can build its CamlinternalOO.inherits arguments.  Skipped
  // (children then bail) when the shape isn't analyzable: unknown parent,
  // multiple inherits, duplicate vals.
  void register_class_meta(const std::string& name, const ast::ClassStructure& cs) {
    ClassMeta m;
    std::set<std::string> vset, virt, concr;
    bool inherited = false;
    for (auto& f : cs.fields) {
      if (auto* inh = std::get_if<ast::Pcf_inherit>(&f.desc)) {
        const ast::ClassExpr* pe = inh->ce.get();
        if (auto* ap = std::get_if<ast::Pcl_apply>(&pe->desc)) pe = ap->ce.get();
        auto* pc = std::get_if<ast::Pcl_constr>(&pe->desc);
        if (!pc) return;
        auto* pl = std::get_if<Lident>(&pc->id.txt.v);
        if (!pl) return;
        auto it = class_meta_.find(pl->name);
        if (it == class_meta_.end() || inherited || !vset.empty()) return;
        inherited = true;
        m = it->second;  // start from the parent's shape
        for (auto& v : m.vals) vset.insert(v);
        virt.insert(it->second.virt.begin(), it->second.virt.end());
        concr.insert(it->second.concr.begin(), it->second.concr.end());
      } else if (auto* v = std::get_if<ast::Pcf_val>(&f.desc)) {
        if (!std::get_if<ast::Cfk_concrete>(&v->kind)) return;  // virtual val
        if (!vset.insert(v->name.txt).second) return;  // duplicate val
        m.vals.push_back(v->name.txt);
      } else if (auto* me = std::get_if<ast::Pcf_method>(&f.desc)) {
        if (std::get_if<ast::Cfk_concrete>(&me->kind)) {
          concr.insert(me->name.txt); virt.erase(me->name.txt);
        } else if (!concr.count(me->name.txt)) {
          virt.insert(me->name.txt);
        }
      }
    }
    m.meths.assign(concr.begin(), concr.end());
    m.meths.insert(m.meths.end(), virt.begin(), virt.end());
    std::sort(m.meths.begin(), m.meths.end());
    m.virt.assign(virt.begin(), virt.end());
    m.concr.assign(concr.begin(), concr.end());
    class_meta_[name] = m;
  }
  // The inheritance metadata of a class from its SIGNATURE class type (`class c :
  // object val v.. method m.. end`), keyed by a (possibly module-qualified) name
  // -- so `inherit M.c` over a recursive-module class can build its inherits args.
  void register_class_meta_from_sig(const std::string& name, const ast::ClassType& ct) {
    auto* ps = std::get_if<ast::Pcty_signature>(&ct.desc);
    if (!ps) return;
    ClassMeta m;
    std::set<std::string> virt, concr, vset;
    for (auto& f : ps->cs.fields) {
      if (auto* mm = std::get_if<ast::Pctf_method>(&f.desc)) {
        if (mm->virt == ast::VirtualFlag::Virtual) virt.insert(mm->name.txt);
        else concr.insert(mm->name.txt);
      } else if (auto* v = std::get_if<ast::Pctf_val>(&f.desc)) {
        if (v->virt != ast::VirtualFlag::Virtual && vset.insert(v->name.txt).second)
          m.vals.push_back(v->name.txt);
      }
    }
    m.meths.assign(concr.begin(), concr.end());
    m.meths.insert(m.meths.end(), virt.begin(), virt.end());
    std::sort(m.meths.begin(), m.meths.end());
    m.virt.assign(virt.begin(), virt.end());
    m.concr.assign(concr.begin(), concr.end());
    class_meta_[name] = m;
  }

  // An object structure `object (self) val.. method.. end` (Tcl_structure, concrete
  // fields).  `as_class` selects the class-declaration form (a class_init function +
  // make_class, obj_init taking a self argument) vs the immediate-object form
  // (create_table + a direct obj_init applied to 0).  `cl_params` are class
  // parameters (`class c x = ...`): extra obj_init parameters after self, in scope
  // for the val initialisers.  A method referencing a class parameter needs the
  // env-capture machinery (not yet built) -- detected post-translation and bailed.
  // Returns null (-> placeholder) for shapes not yet handled (inherit/virtual/
  // initializers/env capture).
  // A side-effect-free expression: an identifier / constant, or a coercion /
  // type-constraint wrapping a pure expression (`(x :> t)`, `(x : t)`).  Used to
  // tell whether a `let _ = e` binding can simply be dropped.
  static bool is_pure_expr(const ast::Expression& e) {
    if (std::holds_alternative<ast::Pexp_ident>(e.desc) ||
        std::holds_alternative<ast::Pexp_constant>(e.desc)) return true;
    if (auto* c = std::get_if<ast::Pexp_coerce>(&e.desc)) return is_pure_expr(*c->e);
    if (auto* c = std::get_if<ast::Pexp_constraint>(&e.desc)) return is_pure_expr(*c->e);
    return false;
  }
  LamPtr build_object(const ast::ClassStructure& cs, bool as_class,
                      const std::string& class_name = "",
                      const std::vector<const ast::Pcl_fun*>* cl_params = nullptr,
                      const std::vector<const ast::Pcl_let*>* cl_lets = nullptr,
                      bool virt_class = false,
                      const std::vector<const ast::Pcl_let*>* cl_per_obj_lets = nullptr) {
    struct Meth { std::string name; const ast::Expression* body; };
    struct Val  { std::string name; const ast::Expression* init; };
    std::vector<Meth> meths;
    std::vector<Val> vals;
    std::vector<const ast::Expression*> initializers;
    // Single `inherit parent args..` (no `as super`), before any val or
    // initializer (its parent-init call precedes their stores in env_init).
    const ClassMeta* parent_meta = nullptr;
    LamPtr parent_val;  // the parent class value (a local Var, or `field_imm i M`)
    std::vector<const ast::Expression*> inh_args;
    std::string super_name;  // `inherit parent as super`
    std::vector<std::string> virt_own;  // own virtual methods (virtual class only)
    std::vector<std::string> virt_vals; // own virtual vals (reserve a slot, no init)
    for (auto& f : cs.fields) {
      if (auto* m = std::get_if<ast::Pcf_method>(&f.desc)) {
        auto* cc = std::get_if<ast::Cfk_concrete>(&m->kind);
        if (!cc) {
          // a virtual method contributes its label to the method universe but
          // has no code; only legal inside a `class virtual`.
          if (!virt_class) return nullptr;
          virt_own.push_back(m->name.txt);
          continue;
        }
        const ast::Expression* body = cc->e.get();
        if (auto* poly = std::get_if<ast::Pexp_poly>(&body->desc)) body = poly->e.get();
        meths.push_back({m->name.txt, body});
      } else if (auto* v = std::get_if<ast::Pcf_val>(&f.desc)) {
        auto* cc = std::get_if<ast::Cfk_concrete>(&v->kind);
        if (!cc) {
          // a virtual val reserves a variable slot (new_variable) but has no
          // initialiser; only legal in a `class virtual`.
          if (!virt_class) return nullptr;
          virt_vals.push_back(v->name.txt);
          continue;
        }
        vals.push_back({v->name.txt, cc->e.get()});
      } else if (auto* ini = std::get_if<ast::Pcf_initializer>(&f.desc)) {
        initializers.push_back(ini->e.get());
      } else if (auto* inh = std::get_if<ast::Pcf_inherit>(&f.desc)) {
        if (parent_meta || !vals.empty() || !initializers.empty())
          return nullptr;  // multiple/late inherit: unsupported
        if (inh->as_) super_name = inh->as_->txt;
        const ast::ClassExpr* pe = inh->ce.get();
        if (auto* ap = std::get_if<ast::Pcl_apply>(&pe->desc)) {
          for (auto& [l, e] : ap->args) {
            if (!std::holds_alternative<ast::Nolabel>(l)) return nullptr;
            inh_args.push_back(e.get());
          }
          pe = ap->ce.get();
        }
        auto* pc = std::get_if<ast::Pcl_constr>(&pe->desc);
        if (!pc) return nullptr;
        if (auto* pl = std::get_if<Lident>(&pc->id.txt.v)) {  // inherit LocalClass
          auto mit = class_meta_.find(pl->name);
          const Ident* pid = lookup(pl->name);
          if (mit == class_meta_.end() || !pid) return nullptr;
          parent_meta = &mit->second;
          parent_val = varof(*pid);
        } else if (auto* d = std::get_if<Ldot>(&pc->id.txt.v)) {  // inherit M.c
          auto* ml2 = std::get_if<Lident>(&d->prefix->v);
          if (!ml2) return nullptr;
          std::string key = ml2->name + "." + d->name;
          auto mit = class_meta_.find(key);
          // A class brought in through a functor application + `include` (e.g.
          // `module Basic = include Create(P)` -> `Basic.agent`) has no
          // module-qualified meta registered; fall back to the bare class name,
          // under which it was registered when the (functor) body was compiled.
          if (mit == class_meta_.end()) mit = class_meta_.find(d->name);
          LamPtr base = module_base(ml2->name);
          auto& lay = module_layout_[ml2->name];
          auto fi = lay.find(d->name);
          if (mit == class_meta_.end() || !base || fi == lay.end()) return nullptr;
          parent_meta = &mit->second;
          auto f0 = mk(Lam::K::Prim); f0->prim = Prim::FieldImm;
          f0->prim_arg = fi->second; f0->args = {base};
          parent_val = f0;
        } else return nullptr;
      } else if (std::get_if<ast::Pcf_constraint>(&f.desc) ||
                 std::get_if<ast::Pcf_attribute>(&f.desc)) {
        // no runtime effect
      } else {
        return nullptr;  // unsupported field
      }
    }

    // Class parameters: var patterns only (a constraint wrapper is peeled).
    // A parameter referenced by a method body is COPIED into an anonymous
    // instance variable (`=o (new_variable class "")`), stored in env_init
    // before the val inits; the method reads it via field_computed.  Val
    // initialisers use the raw parameter directly.
    struct CParam { std::string name; Ident pid, var_id; const ast::Pattern* pat;
                    bool is_param; bool captured = false;
                    Ident opt_id; const ast::Expression* dflt = nullptr; };
    std::vector<CParam> cparams;
    if (cl_params)
      for (auto* pf : *cl_params) {
        const ast::Pattern* p = &pf->pat;
        while (auto* pc = std::get_if<ast::Ppat_constraint>(&p->desc)) p = pc->p.get();
        bool optdef = std::holds_alternative<Optional>(pf->label) && pf->default_;
        if (auto* pv = std::get_if<ast::Ppat_var>(&p->desc)) {
          CParam cp{pv->name.txt, fresh(pv->name.txt), fresh(pv->name.txt), p, true};
          if (optdef) {  // `?(h=d)`: a *opt* param + an unwrap let in the body
            cp.opt_id = fresh("opt", true);
            cp.dflt = pf->default_->get();
          }
          cparams.push_back(std::move(cp));
        } else if (!optdef &&
                   (std::holds_alternative<ast::Ppat_any>(p->desc) ||
                    (std::get_if<ast::Ppat_construct>(&p->desc) &&
                     lid_last(std::get_if<ast::Ppat_construct>(&p->desc)->id.txt) == "()"))) {
          // a binderless `()`/`_` parameter, named "param", never referenced
          cparams.push_back({"", fresh("param"), fresh("param"), p, true});
        } else {
          return nullptr;  // destructuring class params: unsupported
        }
      }

    // In a PARAMETERIZED class the class-creation lets become PER-OBJECT instance
    // variables: ocamlc runs their inits in obj_init (where the params are
    // bound), unlike a parameterless class where a let is computed once and the
    // shared value is stored into every object.  Route simple var-binding lets
    // into `vals` (the instance-var machinery); their inits then run per object
    // with the params in scope.  Side-effect / destructuring / rec lets keep the
    // class-creation path (which bails on a param ref, as before).
    bool lets_as_vals = false;
    if (cl_params && !cl_params->empty() && cl_lets && !cl_lets->empty()) {
      std::vector<Val> let_vals;
      lets_as_vals = true;
      for (auto* lg : *cl_lets) {
        if (lg->rf == RecFlag::Recursive) { lets_as_vals = false; break; }
        for (auto& b : lg->bindings) {
          auto* pv = std::get_if<ast::Ppat_var>(&effective_pat(&b.pat)->desc);
          if (!pv) { lets_as_vals = false; break; }
          let_vals.push_back({pv->name.txt, b.expr.get()});
        }
        if (!lets_as_vals) break;
      }
      if (lets_as_vals) for (auto& v : let_vals) vals.push_back(v);
    }

    // The method universe is the union of inherited and own method names
    // (including own virtual ones -- they have labels but no code).
    std::set<std::string> own_defined;
    std::set<std::string> all_meths;
    for (auto& m : meths) { own_defined.insert(m.name); all_meths.insert(m.name); }
    for (auto& n : virt_own) all_meths.insert(n);
    if (parent_meta) for (auto& n : parent_meta->meths) all_meths.insert(n);
    // pub_meths: sorted by hash_variant ascending (create_table/make_class arg).
    std::vector<std::string> pub_meths(all_meths.begin(), all_meths.end());
    std::sort(pub_meths.begin(), pub_meths.end(),
              [&](const std::string& a, const std::string& b) {
                return hash_variant(a) < hash_variant(b); });
    // methl: descending name order (Meths.fold prepend order).
    std::vector<std::string> methl_names(all_meths.begin(), all_meths.end());
    std::sort(methl_names.begin(), methl_names.end(), std::greater<>());
    std::vector<std::string> val_names;   // own (new) vals only
    for (auto& v : vals) val_names.push_back(v.name);
    for (auto& n : virt_vals) val_names.push_back(n);  // virtual vals: slot only
    int len = (int)methl_names.size(), nvals = (int)val_names.size();

    Ident cla = fresh("class");
    Ident obj_init = fresh("obj_init");
    Ident envp = fresh("env");

    // Method-id and val-id binders (used both in the index bindings and as the
    // method-label entries of the set_methods block).  Parent instance vars get
    // ids too -- bound from the inherits result rather than the ids array.
    std::unordered_map<std::string, Ident> meth_id, val_id;
    for (auto& n : methl_names) meth_id[n] = fresh(n);
    for (auto& n : val_names)   val_id[n]  = fresh(n);
    if (parent_meta)
      for (auto& n : parent_meta->vals)
        if (!val_id.count(n)) val_id[n] = fresh(n);
    // `as super`: ids for the parent's concrete-method closures (the trailing
    // fields of the inherits result), bound sparsely by super#m usage.
    std::unordered_map<std::string, Ident> super_mid;
    if (parent_meta && !super_name.empty())
      for (auto& n : parent_meta->concr) super_mid[n] = fresh(n);

    // Translate method bodies and val initialisers with the instance variables in
    // scope (a method's `n` -> (field_computed self n)) and the method labels
    // known (so a self-send resolves to (sendself self m)).
    auto save_iv = inst_vars_; auto save_mid = cur_meth_id_;
    auto save_sn = cur_super_name_; auto save_smid = cur_super_mid_;
    inst_vars_.clear();
    if (parent_meta)
      for (auto& n : parent_meta->vals) inst_vars_[n] = val_id[n];
    for (auto& v : vals) inst_vars_[v.name] = val_id[v.name];
    cur_meth_id_ = meth_id;
    cur_super_name_ = super_name; cur_super_mid_ = super_mid;
    std::string self_name = "self-" + std::to_string(++obj_counter_);
    scope.emplace_back();  // class parameters, visible to val inits and methods
    for (auto& cp : cparams)
      if (!cp.name.empty()) scope.back()[cp.name] = cp.pid;
    auto restore = [&] { scope.pop_back(); inst_vars_ = save_iv; cur_meth_id_ = save_mid;
                         cur_super_name_ = save_sn; cur_super_mid_ = save_smid; };

    // `class c = let .. in object`: the bindings wrap the class_init function
    // (bodies attached at the end).  Simple var lets and letrecs of functions
    // only.  Their vars behave like class parameters: val inits use them
    // directly, a method reference is copied into an anonymous instance var.
    // A let RHS cannot reference a class parameter (bound only inside obj_init).
    std::vector<LamPtr> let_layers;
    size_t nparams = cparams.size();
    auto rhs_leaks_param = [&](const LamPtr& v) {
      for (size_t i = 0; i < nparams; ++i)
        if (cparams[i].is_param && count_var(v, cparams[i].pid)) return true;
      return false;
    };
    if (cl_lets && !lets_as_vals)
      for (auto* lg : *cl_lets) {
        if (lg->rf == RecFlag::Recursive) {
          std::vector<std::pair<const ast::ValueBinding*, Ident>> recs;
          for (auto& b : lg->bindings) {
            auto* pv = std::get_if<ast::Ppat_var>(&b.pat.desc);
            if (!pv) { restore(); return nullptr; }
            Ident id = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = id;
            record_fn_sig(id, b.expr.get());
            recs.push_back({&b, id});
          }
          std::vector<Ident> ids; std::vector<ValueKind> kinds; std::vector<LamPtr> rvals;
          for (auto& [b, id] : recs) {
            ids.push_back(id);
            kinds.push_back(pat_kind(&b->pat));
            rec_spine_ = true;
            LamPtr v = expr(*b->expr);
            if (rhs_leaks_param(v)) { restore(); return nullptr; }
            rvals.push_back(v);
          }
          RecParts rp;
          if (!recs.empty() && partition_rec(ids, kinds, rvals, rp)) {
            // value recursion: dummies-let outermost, lifted funcs, then the
            // backpatch updates as seq layers before the class_init fn.
            if (!rp.dummies.empty()) {
              auto l = mk(Lam::K::Let); l->bindings = std::move(rp.dummies);
              let_layers.push_back(l);
            }
            if (!rp.funcs.empty()) {
              auto lr = mk(Lam::K::Letrec); lr->bindings = std::move(rp.funcs);
              let_layers.push_back(lr);
            }
            for (auto& u : rp.updates) {
              auto s = mk(Lam::K::Sequence); s->cond = u;
              let_layers.push_back(s);
            }
          } else {
            auto lr = mk(Lam::K::Letrec);
            for (size_t i = 0; i < recs.size(); ++i)
              lr->bindings.push_back({ids[i], kinds[i], collapse_let_id(rvals[i])});
            let_layers.push_back(lr);
          }
          for (auto& [b, id] : recs) {
            auto& nm = std::get<ast::Ppat_var>(b->pat.desc).name.txt;
            cparams.push_back({nm, id, fresh(nm), &b->pat, false});
          }
        } else {
          auto l = mk(Lam::K::Let);
          std::vector<std::pair<const ast::ValueBinding*, Ident>> binds;
          for (auto& b : lg->bindings) {  // RHSs see the outer scope only
            const ast::Pattern* bp = effective_pat(&b.pat);
            auto* pv = std::get_if<ast::Ppat_var>(&bp->desc);
            // `let () = e` / `let _ = e` (run for effect, bind nothing): the
            // oracle binds a throwaway `*match*` and keeps the side effect.
            bool nobind = !pv &&
                (std::holds_alternative<ast::Ppat_any>(bp->desc) ||
                 (std::get_if<ast::Ppat_construct>(&bp->desc) &&
                  lid_last(std::get<ast::Ppat_construct>(bp->desc).id.txt) == "()"));
            if (!pv && !nobind) { restore(); return nullptr; }
            LamPtr v = expr(*b.expr);
            if (rhs_leaks_param(v)) {
              // `let _ = (param :> t)` -- a pure wildcard coercion of a class
              // parameter has no effect, so eliding it is sound.  (Class-creation
              // lets can't see params; the oracle runs this pure no-op per object
              // as `(seq param ..)`.)  Only a pure RHS is safe to drop.
              if (nobind && is_pure_expr(*b.expr)) continue;
              restore(); return nullptr;
            }
            if (nobind) { l->bindings.push_back({fresh("", true), ValueKind::Gen, v}); continue; }
            Ident id = fresh(pv->name.txt);
            record_fn_sig(id, b.expr.get());
            l->bindings.push_back({id, pat_kind(&b.pat), v});
            binds.push_back({&b, id});
          }
          for (auto& [b, id] : binds) {
            auto& nm = std::get<ast::Ppat_var>(b->pat.desc).name.txt;
            scope.back()[nm] = id;
            cparams.push_back({nm, id, fresh(nm), &b->pat, false});
          }
          let_layers.push_back(l);
        }
      }

    // The set_methods block: in declaration order, each method's id var then its
    // method code -- a closure `(function self params.. <body>)`, or the builtin
    // `GetConst <value>` (tag 0) when the body is a self-contained value.
    std::vector<LamPtr> methods_block;
    for (auto& m : meths) {
      methods_block.push_back(varof(meth_id[m.name]));
      Ident self = fresh(self_name);
      scope.emplace_back();
      auto save_floor = method_scope_floor_; method_scope_floor_ = scope.size() - 1;
      if (auto* pv = std::get_if<ast::Ppat_var>(&effective_pat(&cs.self)->desc)) scope.back()[pv->name.txt] = self;
      auto save_self = cur_self_; cur_self_ = self;
      // __FUNCTION__ inside a method is `<path>.<class>#<method>` (an anonymous
      // object is named `object`).
      func_path_.push_back((class_name.empty() ? "object" : class_name) + "#" + m.name);
      // A method `method f a b = e` is one curried function over self plus its own
      // params: prepend self to the (flattened) function translated from the body.
      LamPtr fn;
      // A polymorphic method `method m : type a. T = fun ..` desugars to a
      // Pexp_newtype (and maybe a constraint) wrapping the function; peel those
      // (no runtime effect) so the function flattens like a plain method body.
      const ast::Expression* mb = m.body;
      while (true) {
        if (auto* nt = std::get_if<ast::Pexp_newtype>(&mb->desc)) { mb = nt->body.get(); continue; }
        if (auto* ct = std::get_if<ast::Pexp_constraint>(&mb->desc)) { mb = ct->e.get(); continue; }
        break;
      }
      if (auto* pf = std::get_if<ast::Pexp_function>(&mb->desc)) {
        fn = function(*pf, mb->loc);
        fn->params.insert(fn->params.begin(), {self, ValueKind::Gen});
        fn->ret_kind = ValueKind::Gen;  // method closures use lfunction ~return:Pgenval
      } else {
        fn = mk(Lam::K::Function);
        fn->params = {{self, ValueKind::Gen}};
        fn->body = expr(*m.body);
      }
      func_path_.pop_back();
      cur_self_ = save_self;
      method_scope_floor_ = save_floor;
      scope.pop_back();
      // A referenced class parameter is rewritten to its instance-var copy.
      for (auto& cp : cparams)
        if (count_var(fn->body, cp.pid)) {
          auto fc = mk(Lam::K::Prim); fc->prim = Prim::FieldComputed;
          fc->args = {varof(self), varof(cp.var_id)};
          subst_var(fn->body, cp.pid, fc);
          cp.captured = true;
        }
      if (fn->params.size() == 1 && is_const_path(fn->body, self)) {
        methods_block.push_back(cint(0));      // GetConst
        methods_block.push_back(fn->body);
      } else {
        methods_block.push_back(fn);
      }
    }

    // Initializer bodies: closures over self, same instance-var/capture rules
    // as methods; each becomes `(apply add_initializer class fn)` after
    // set_methods.
    std::vector<LamPtr> init_fns;
    for (auto* ie : initializers) {
      Ident self = fresh(self_name);
      scope.emplace_back();
      auto save_floor = method_scope_floor_; method_scope_floor_ = scope.size() - 1;
      if (auto* pv = std::get_if<ast::Ppat_var>(&effective_pat(&cs.self)->desc)) scope.back()[pv->name.txt] = self;
      auto save_self = cur_self_; cur_self_ = self;
      auto fn = mk(Lam::K::Function);
      fn->params = {{self, ValueKind::Gen}};
      fn->body = expr(*ie);
      cur_self_ = save_self;
      method_scope_floor_ = save_floor;
      scope.pop_back();
      for (auto& cp : cparams)
        if (count_var(fn->body, cp.pid)) {
          auto fc = mk(Lam::K::Prim); fc->prim = Prim::FieldComputed;
          fc->args = {varof(self), varof(cp.var_id)};
          subst_var(fn->body, cp.pid, fc);
          cp.captured = true;
        }
      init_fns.push_back(fn);
    }

    // The env_init function: `(function env [self] (let (self2 = create_object_opt
    // <obj> class) (seq <val-inits> self2)))`.  For a class the object argument is
    // the env_init's own self parameter (so `new`/inheritance can pass an allocated
    // object); for an immediate object it is 0.  No vals -> just the call.
    Ident self_param = fresh("self");  // the obj_init parameter (class mode only)
    LamPtr obj_arg = as_class ? varof(self_param) : cint(0);
    Ident selfo = fresh("self");
    // The object-field stores: captured-parameter copies (parameter order), the
    // parent's obj_init call (when inheriting), then the val inits (declaration
    // order).
    Ident pobj_init = fresh("obj_init");  // parent obj_init (inherit only)
    std::vector<LamPtr> stores;
    for (auto& cp : cparams) {
      if (!cp.captured) continue;
      auto sf = mk(Lam::K::Prim); sf->prim = Prim::SetfieldComputed;
      sf->prim_id = pat_kind(cp.pat) == ValueKind::Int
                      ? "setfield_imm_computed" : "setfield_ptr_computed";
      sf->args = {varof(selfo), varof(cp.var_id), varof(cp.pid)};
      stores.push_back(sf);
    }
    if (parent_meta) {
      auto call = mk(Lam::K::Apply);
      call->fn = varof(pobj_init);
      call->args = {varof(selfo)};
      for (auto* a : inh_args) call->args.push_back(expr(*a));
      stores.push_back(call);
    }
    for (auto& vl : vals) {
      auto save_self = cur_self_; cur_self_ = selfo;
      // A val initialiser is not a method body: resolve names lexically (floor 0
      // makes lookup_from behave like plain lookup), so a class parameter or
      // outer value isn't mistaken for an instance-var field read.
      auto save_floor = method_scope_floor_; method_scope_floor_ = 0;
      LamPtr v = expr(*vl.init);
      cur_self_ = save_self;
      method_scope_floor_ = save_floor;
      auto sf = mk(Lam::K::Prim); sf->prim = Prim::SetfieldComputed;
      sf->prim_id = value_is_immediate(vl.init)
                      ? "setfield_imm_computed" : "setfield_ptr_computed";
      sf->args = {varof(selfo), varof(val_id[vl.name]), v};
      stores.push_back(sf);
    }
    LamPtr env_body;
    if (stores.empty()) {
      env_body = init_fns.empty()
          ? oo_call("create_object_opt", {obj_arg, varof(cla)})
          : oo_call("create_object_and_run_initializers", {obj_arg, varof(cla)});
    } else {
      // stores right-associated, then `(seq <stores> <tail>)` where the tail is
      // self (create_object wraps the init sequence with it as its value), or
      // the run-initializers call when the class has (possibly inherited)
      // initializers.
      LamPtr tail = init_fns.empty() && !parent_meta
          ? varof(selfo)
          : oo_call("run_initializers_opt", {obj_arg, varof(selfo), varof(cla)});
      LamPtr inits;
      for (auto it = stores.rbegin(); it != stores.rend(); ++it) {
        if (!inits) inits = *it;
        else { auto s = mk(Lam::K::Sequence); s->cond = *it; s->else_ = inits; inits = s; }
      }
      auto outer = mk(Lam::K::Sequence); outer->cond = inits; outer->else_ = tail;
      auto let = mk(Lam::K::Let);
      let->bindings = {{selfo, ValueKind::Gen,
                        oo_call("create_object_opt", {obj_arg, varof(cla)}), false, false, false}};
      let->body = outer;
      env_body = let;
    }
    // Per-object lets (a `let` under a constraint, `(let () = e in object : ct)`):
    // ocamlc keeps them inside the env_init body so they run on each `new`, not
    // lifted to class creation.  Only the var / `()` / `_` binding shapes.
    if (cl_per_obj_lets)
      for (auto it = cl_per_obj_lets->rbegin(); it != cl_per_obj_lets->rend(); ++it) {
        auto l = mk(Lam::K::Let);
        for (auto& b : (*it)->bindings) {
          const ast::Pattern* bp = effective_pat(&b.pat);
          auto* pv = std::get_if<ast::Ppat_var>(&bp->desc);
          Ident bid = pv ? fresh(pv->name.txt) : fresh("", true);
          if (pv) scope.back()[pv->name.txt] = bid;
          l->bindings.push_back({bid, ValueKind::Gen, expr(*b.expr)});
        }
        l->body = env_body; env_body = l;
      }
    // obj_init parameters, split into curried functions at each
    // optional-default unwrap (`?(h=d)` ends its group with the *opt* param;
    // the body lets `h = (if *opt* (field_imm 0 *opt*) d)` and the remaining
    // parameters form a nested function).
    struct PGroup { std::vector<std::pair<Ident, ValueKind>> ps; const CParam* unwrap = nullptr; };
    std::vector<PGroup> groups(1);
    groups[0].ps.push_back({envp, ValueKind::Gen});
    if (as_class) groups[0].ps.push_back({self_param, ValueKind::Gen});
    for (auto& cp : cparams) {
      if (!cp.is_param) continue;
      if (cp.dflt) {
        groups.back().ps.push_back({cp.opt_id, ValueKind::Gen});
        groups.back().unwrap = &cp;
        groups.emplace_back();
      } else {
        groups.back().ps.push_back({cp.pid, ValueKind::Gen});
      }
    }
    LamPtr env_fn = env_body;
    for (int gi = (int)groups.size() - 1; gi >= 0; --gi) {
      if (groups[gi].unwrap) {
        const CParam* cp = groups[gi].unwrap;
        auto iff = mk(Lam::K::IfThenElse);
        iff->cond = varof(cp->opt_id);
        iff->then_ = fieldimm(0, varof(cp->opt_id));
        iff->else_ = expr(*cp->dflt);
        auto let = mk(Lam::K::Let);
        let->bindings = {{cp->pid, pat_kind(cp->pat), iff}};
        let->body = env_fn; env_fn = let;
      }
      if (!groups[gi].ps.empty()) {
        auto f = mk(Lam::K::Function);
        f->params = groups[gi].ps; f->body = env_fn; env_fn = f;
      }
    }

    restore();

    // output_methods: exactly one (label, code) pair -> set_method, else a
    // set_methods over a makeblock of all the entries.  Initializer
    // registrations follow set_methods, in declaration order.
    LamPtr cl_init = env_fn;
    for (auto it = init_fns.rbegin(); it != init_fns.rend(); ++it) {
      auto s = mk(Lam::K::Sequence);
      s->cond = oo_call("add_initializer", {varof(cla), *it});
      s->else_ = cl_init; cl_init = s;
    }
    if (!methods_block.empty()) {
      LamPtr setm;
      if (methods_block.size() == 2) {
        setm = oo_call("set_method", {varof(cla), methods_block[0], methods_block[1]});
      } else {
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = methods_block;
        setm = oo_call("set_methods", {varof(cla), blk});
      }
      auto s = mk(Lam::K::Sequence); s->cond = setm; s->else_ = cl_init; cl_init = s;
    }

    // Whether a binder is referenced by any translated method/initializer body
    // or by env_init -- unreferenced inherited labels/vars are not bound.
    auto used_id = [&](const Ident& id) {
      if (count_var(env_fn, id)) return true;
      for (auto& mb : methods_block) if (count_var(mb, id)) return true;
      for (auto& fi : init_fns) if (count_var(fi, id)) return true;
      return false;
    };

    // inherits: `(inherits class parent_vals virt_meths concr_meths parent 1)`,
    // binding the parent's obj_init (field 0) and the used inherited instance
    // vars (fields 1..) -- the innermost binder group, after the ids binds.
    if (parent_meta) {
      Ident inh = fresh("inh");
      // argument arrays are created before the method/val-name arrays, the
      // concrete-methods array before the virtual one (oracle creation order)
      LamPtr vals_arr =
          parent_meta->vals.empty() ? cint(0) : transl_meth_list(parent_meta->vals);
      LamPtr concr_arr =
          parent_meta->concr.empty() ? cint(0) : transl_meth_list(parent_meta->concr);
      LamPtr virt_arr =
          parent_meta->virt.empty() ? cint(0) : transl_meth_list(parent_meta->virt);
      std::vector<LamPtr> ia = {varof(cla), vals_arr, virt_arr, concr_arr,
                                parent_val, cint(1)};
      auto bindlet = mk(Lam::K::Let);
      auto f0 = mk(Lam::K::Prim); f0->prim = Prim::FieldMut; f0->prim_arg = 0;
      f0->args = {varof(inh)};
      bindlet->bindings.push_back({pobj_init, ValueKind::Gen, f0, false, false, true});
      for (size_t i = 0; i < parent_meta->vals.size(); ++i) {
        Ident& vid = val_id[parent_meta->vals[i]];
        if (!used_id(vid)) continue;
        auto fm = mk(Lam::K::Prim); fm->prim = Prim::FieldMut;
        fm->prim_arg = (int)(1 + i); fm->args = {varof(inh)};
        bindlet->bindings.push_back({vid, ValueKind::Gen, fm, false, false, true});
      }
      // super-called parent method closures follow the vals in the result array
      if (!super_mid.empty())
        for (size_t j = 0; j < parent_meta->concr.size(); ++j) {
          Ident& mid = super_mid[parent_meta->concr[j]];
          if (!used_id(mid)) continue;
          auto fm = mk(Lam::K::Prim); fm->prim = Prim::FieldMut;
          fm->prim_arg = (int)(1 + parent_meta->vals.size() + j);
          fm->args = {varof(inh)};
          bindlet->bindings.push_back({mid, ValueKind::Gen, fm, false, false, true});
        }
      bindlet->body = cl_init;
      auto inhlet = mk(Lam::K::Let);
      inhlet->bindings = {{inh, ValueKind::Gen, oo_call("inherits", ia), false, false, false}};
      inhlet->body = bindlet;
      cl_init = inhlet;
    }

    // bind_methods: bind the method-label / variable-index ids from the table.
    if (len < 2 && nvals == 0) {
      // bind_method: a single method via get_method_label (Strict, `=`).
      for (auto it = methl_names.rbegin(); it != methl_names.rend(); ++it) {
        if (!own_defined.count(*it) && !used_id(meth_id[*it])) continue;
        auto let = mk(Lam::K::Let);
        let->bindings = {{meth_id[*it], ValueKind::Gen,
                          oo_call("get_method_label", {varof(cla), cstr(*it)}),
                          false, false, false}};
        let->body = cl_init; cl_init = let;
      }
    } else if (len == 0 && nvals < 2) {
      // transl_vals: a lone variable via new_variable (Strict, `=`).
      for (auto it = val_names.rbegin(); it != val_names.rend(); ++it) {
        auto let = mk(Lam::K::Let);
        let->bindings = {{val_id[*it], ValueKind::Gen,
                          oo_call("new_variable", {varof(cla), cstr(*it)}),
                          false, false, false}};
        let->body = cl_init; cl_init = let;
      }
    } else {
      // new_methods_variables: one `ids` array, fields bound StrictOpt (`=o`).
      // ocaml binds the var-name array (`names`) before the method-name array, so
      // create the shared const for vals first to match the stamp/print order.
      Ident ids = fresh("ids");
      LamPtr val_arr = nvals ? transl_meth_list(val_names) : nullptr;
      std::vector<LamPtr> nmv_args = {varof(cla), transl_meth_list(methl_names)};
      if (val_arr) nmv_args.push_back(val_arr);
      std::string getter = nvals ? "new_methods_variables" : "get_method_labels";
      // layout order: methl method labels, then val indices.
      std::vector<std::string> layout = methl_names;
      for (auto& n : val_names) layout.push_back(n);
      auto let = mk(Lam::K::Let);
      for (size_t i = 0; i < layout.size(); ++i) {
        bool is_meth = i < methl_names.size();
        // inherited methods are bound only when referenced (self-sends)
        if (is_meth && !own_defined.count(layout[i]) && !used_id(meth_id[layout[i]]))
          continue;
        Ident& bid = is_meth ? meth_id[layout[i]] : val_id[layout[i]];
        auto fm = mk(Lam::K::Prim); fm->prim = Prim::FieldMut; fm->prim_arg = (int)i;
        fm->args = {varof(ids)};
        let->bindings.push_back({bid, ValueKind::Gen, fm, false, false, /*strict_opt=*/true});
      }
      let->body = cl_init;
      auto outer = mk(Lam::K::Let);
      outer->bindings = {{ids, ValueKind::Gen, oo_call(getter, nmv_args), false, false, false}};
      outer->body = let;
      cl_init = outer;
    }

    // Captured-parameter instance variables: anonymous (`new_variable class ""`),
    // bound `=o` outermost (printed before the method-label binders).
    for (auto it = cparams.rbegin(); it != cparams.rend(); ++it) {
      if (!it->captured) continue;
      auto let = mk(Lam::K::Let);
      let->bindings = {{it->var_id, ValueKind::Gen,
                        oo_call("new_variable", {varof(cla), cstr("")}),
                        false, false, /*strict_opt=*/true}};
      let->body = cl_init; cl_init = let;
    }

    if (as_class) {
      // A class declaration: wrap cl_init in the class_init function over the table,
      // then `(apply make_class shared class_init)` returns the class 3-tuple.
      Ident class_init = fresh(class_name + "_init");
      auto ci_fn = mk(Lam::K::Function);
      ci_fn->params = {{cla, ValueKind::Gen}};
      ci_fn->body = cl_init;
      // `class c = let .. in object`: the let layers wrap the class_init fn.
      LamPtr ci_val = ci_fn;
      for (auto it = let_layers.rbegin(); it != let_layers.rend(); ++it) {
        if ((*it)->k == Lam::K::Sequence) (*it)->else_ = ci_val;
        else (*it)->body = ci_val;
        ci_val = *it;
      }
      // A virtual class cannot be instantiated: no make_class -- its value is
      // the plain 3-tuple [0; class_init; 0], dummy-allocated and updated at
      // the binding site (value-rec compilation of class declarations).
      if (virt_class) {
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = {cint(0), ci_val, cint(0)};
        return blk;
      }
      auto mc = oo_call("make_class", {transl_meth_list(pub_meths), varof(class_init)});
      auto let = mk(Lam::K::Let);
      let->bindings = {{class_init, ValueKind::Gen, ci_val, false, false, false}};
      let->body = mc;
      return let;
    }

    // ltable cla (ldirect obj_init):
    //   (let (class = create_table(pub_meths)  obj_init = cl_init)
    //     (seq (init_class class) (apply obj_init 0)))
    // When cl_init is directly a function (a method-less, val-less object), simplif
    // inlines the single-use obj_init and beta-reduces the application, leaving
    // `(seq (init_class class) (let (env = 0) <body>))`.
    LamPtr top = mk(Lam::K::Let);
    top->bindings = {{cla, ValueKind::Gen,
                      oo_call("create_table", {transl_meth_list(pub_meths)}), false, false, false}};
    auto seq = mk(Lam::K::Sequence);
    seq->cond = oo_call("init_class", {varof(cla)});
    if (cl_init->k == Lam::K::Function && cl_init->params.size() == 1) {
      auto let = mk(Lam::K::Let);
      let->bindings = {{cl_init->params[0].first, ValueKind::Gen, cint(0), false, false, false}};
      let->body = cl_init->body;
      seq->else_ = let;
    } else {
      top->bindings.push_back({obj_init, ValueKind::Gen, cl_init, false, false, false});
      auto apply_init = mk(Lam::K::Apply); apply_init->fn = varof(obj_init);
      apply_init->args = {cint(0)};
      seq->else_ = apply_init;
    }
    top->body = seq;
    return top;
  }

  LamPtr function(const Pexp_function& f, const Location& floc) {
    scope.emplace_back();
    auto l = mk(Lam::K::Function);
    std::vector<std::pair<Ident, LamPtr>> binders;  // sub-vars of destructured params
    // An `?(x=default)` parameter becomes a `*opt*` param plus a body let binding
    // `x = (if *opt* (field_imm 0 *opt*) default)` -- unwrap the option or use the
    // default.  (A `?x` without a default keeps the option itself as the param.)
    struct OptDef { Ident xid, optid; LamPtr def; ValueKind k; bool discard = false; };
    std::vector<OptDef> optdefs;
    const Pattern* refut = nullptr; Ident refut_pid; Location refut_loc; int nrefut = 0;
    // A `(module M : S)` parameter shadows S's variant / extension constructors to
    // the parameter's own fields (so `M.E` reads `(field_imm i M)`, not a sibling
    // module's same-named ctor); restored when the function scope closes.
    std::vector<SigExtSave> fcm_ext_saves;
    std::vector<SigCtorSave> fcm_ctor_saves;
    auto restore_fcm = [&] { restore_sig_exts(fcm_ext_saves); restore_sig_ctors(fcm_ctor_saves); };
    for (auto& fp : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        const Pattern* pat = &pv->pat;
        while (auto* pc = std::get_if<Ppat_constraint>(&pat->desc)) pat = pc->p.get();
        if (std::holds_alternative<Optional>(pv->label) && pv->default_) {
          const Ppat_var* var = std::get_if<Ppat_var>(&pat->desc);
          bool any = std::holds_alternative<Ppat_any>(pat->desc);
          if (var || any || is_irrefutable(*pat)) {
            Ident optid = fresh("opt", true);  // the `*opt*` parameter
            l->params.push_back({optid, ValueKind::Gen});
            // var -> bind the name; `_` -> discard via seq (runs the default for
            // effect); `()`/tuple/record -> bind a *match* temp then destructure.
            Ident xid = var ? fresh(var->name.txt) : fresh("", !any);
            // Compile the default expression BEFORE binding this parameter's name,
            // so `?(current=current)` resolves the default's `current` to the OUTER
            // binding (the global ref) -- not to this very parameter (a self-ref
            // that read garbage and broke Arg.parse's loop counter).  Earlier
            // params are already in scope, which is correct.
            LamPtr dlam = expr(*pv->default_->get());
            if (var) scope.back()[var->name.txt] = xid;
            else if (!any) collect_binders(*pat, varof(xid), binders);
            ValueKind ok = var ? pat_kind(pat) : ValueKind::Gen;  // a *match* temp is unannotated
            optdefs.push_back({xid, optid, std::move(dlam), ok, /*discard=*/any});
            continue;
          }
        }
        // Every parameter gets a binder; a non-variable pattern (a constructor,
        // record or tuple) is named "param" like ocamlc.  An irrefutable one has its
        // variables bound to field reads in the body; a refutable one (a partial
        // pattern, e.g. `(Some x)`) is matched in the body, raising Match_failure on
        // the missing cases -- supported for at most one such parameter.
        if (auto* var = std::get_if<Ppat_var>(&pat->desc)) {
          Ident id = fresh(var->name.txt);
          l->params.push_back({id, pat_kind(pat)});
          scope.back()[var->name.txt] = id;
        } else if (auto* up = std::get_if<Ppat_unpack>(&pat->desc); up && up->name.txt) {
          Ident id = fresh(*up->name.txt);  // `(module X)`: a first-class-module param
          l->params.push_back({id, ValueKind::Gen});
          scope.back()[*up->name.txt] = id;
          module_ident_[*up->name.txt] = id;
          if (up->pkg) {  // `(module X : S)`: members resolve via S's layout
            std::string mt;
            if (lid_to_dotted(up->pkg->path.txt, mt)) {
              register_pack_layouts(*up->name.txt, mt);
              // S's own variant / extension constructors resolve to this param's
              // fields (`M.E` -> `(field_imm i M)`), shadowing siblings'.
              std::string last = mt.substr(mt.rfind('.') + 1);
              if (auto a = modtype_ast_.find(last); a != modtype_ast_.end()) {
                auto cs = register_sig_ctors(*a->second);
                fcm_ctor_saves.insert(fcm_ctor_saves.end(), cs.begin(), cs.end());
                auto es = register_sig_exts(*up->name.txt, id, *a->second);
                fcm_ext_saves.insert(fcm_ext_saves.end(), es.begin(), es.end());
              }
            }
          }
        } else if (is_irrefutable(*pat)) {
          Ident pid = fresh("param");
          l->params.push_back({pid, pat_kind(pat)});
          auto pvar = mk(Lam::K::Var); pvar->var = pid;
          collect_binders(*pat, pvar, binders);
        } else {
          Ident pid = fresh("param");
          l->params.push_back({pid, pat_kind(pat)});
          refut = pat; refut_pid = pid; refut_loc = pv->pat.loc; ++nrefut;
        }
      }
    auto rk = vk.fn_ret.find(&f);
    l->ret_kind = rk == vk.fn_ret.end() ? ValueKind::Gen : vkind(rk->second);
    // Wrap the body with each `?(x=default)` binding (outermost first).
    auto wrap_optdefs = [&](LamPtr body) -> LamPtr {
      for (auto it = optdefs.rbegin(); it != optdefs.rend(); ++it) {
        auto cond = mk(Lam::K::Var); cond->var = it->optid;
        auto optv = mk(Lam::K::Var); optv->var = it->optid;
        auto iff = mk(Lam::K::IfThenElse);
        iff->cond = cond; iff->then_ = fieldimm(0, optv); iff->else_ = it->def;
        if (it->discard) {  // `_` pattern: evaluate for effect, discard the value
          auto sq = mk(Lam::K::Sequence); sq->cond = iff; sq->else_ = body; body = sq;
        } else {
          auto let = mk(Lam::K::Let);
          let->bindings = {{it->xid, it->k, iff}};
          let->body = body; body = let;
        }
      }
      return body;
    };
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      LamPtr body;
      if (refut && nrefut == 1) {  // a partial parameter pattern -> match in the body
        auto sv = mk(Lam::K::Var); sv->var = refut_pid;
        std::vector<Row> rows = {{refut, fb->e.get(), nullptr}};
        body = compile_match(sv, rows, refut_loc);
      } else {
        body = expr(*fb->e);
      }
      l->body = wrap_optdefs(wrap_binders(body, binders));
    } else if (auto* fc = std::get_if<Pfunction_cases>(&f.body->v)) {
      // `function P -> ...` adds an implicit final parameter matched on.  A
      // single unguarded irrefutable case binds directly like an ordinary
      // parameter -- no match: `function () -> e` is just the body, and
      // `function x -> e` names the parameter x.
      if (fc->cases.size() == 1 && !fc->cases[0].guard) {
        const Pattern* pat = effective_pat(&fc->cases[0].lhs);
        if (auto* var = std::get_if<Ppat_var>(&pat->desc)) {
          Ident id = fresh(var->name.txt);
          l->params.push_back({id, pat_kind(pat)});
          scope.back()[var->name.txt] = id;
          l->body = wrap_optdefs(wrap_binders(expr(*fc->cases[0].rhs), binders));
          restore_fcm();
          scope.pop_back();
          return l;
        }
        if (is_irrefutable(*pat)) {
          Ident pid = fresh("param");
          l->params.push_back({pid, pat_kind(pat)});
          auto pvar = mk(Lam::K::Var); pvar->var = pid;
          collect_binders(*pat, pvar, binders);
          l->body = wrap_optdefs(wrap_binders(expr(*fc->cases[0].rhs), binders));
          restore_fcm();
          scope.pop_back();
          return l;
        }
      }
      // Matching.name_pattern: the parameter takes the first var/alias row's name.
      std::string pname = "param";
      for (auto& c : fc->cases) {
        const Pattern* ep = effective_pat(&c.lhs);
        if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) { pname = pv->name.txt; break; }
        if (auto* pa = std::get_if<Ppat_alias>(&ep->desc)) { pname = pa->name.txt; break; }
      }
      Ident pid = fresh(pname);
      // the param's kind is the scrutinee type = any case pattern's (unified)
      l->params.push_back({pid, pat_kind(&fc->cases[0].lhs)});
      auto scrut = mk(Lam::K::Var); scrut->var = pid;
      l->body = wrap_optdefs(wrap_binders(compile_match(scrut, fc->cases, floc), binders));
    } else {
      l->body = mk(Lam::K::ConstInt);
    }
    restore_fcm();
    scope.pop_back();
    return l;
  }

  // Build a module body: the `(let <segments> (makeblock 0 exports))` term for a
  // structure.  Consecutive value/module bindings group into one `let`; a
  // discarded computation (`let _ = e` or bare `e;;`) becomes a `seq` that splits
  // the groups.  `names` (if given) receives the exported field names in order.
  // The leading identifier of an attribute payload (`[@inline never]` -> "never").
  static std::string attr_ident(const Structure& s) {
    if (s.empty()) return "";
    if (auto* ev = std::get_if<Pstr_eval>(&s[0].desc))
      if (auto* id = std::get_if<Pexp_ident>(&ev->e->desc))
        if (auto* l = std::get_if<Lident>(&id->id.txt.v)) return l->name;
    return "";
  }
  // The -dlambda inline annotation from an `[@<which> ...]` attribute (which is
  // "inline" on a function binding, "inlined" on a call site).
  static std::string inline_of_named(const Attributes& attrs, const std::string& which) {
    for (auto& a : attrs) {
      if (a.name != which && a.name != "ocaml." + which) continue;
      return attr_ident(a.payload) == "never" ? "never_inline" : "always_inline";
    }
    return "";
  }
  static bool has_attr(const Attributes& attrs, const std::string& n) {
    for (auto& a : attrs) if (a.name == n || a.name == "ocaml." + n) return true;
    return false;
  }
  static std::string attr_of(const Attributes& attrs, const std::string& n) {
    for (auto& a : attrs) if (a.name == n || a.name == "ocaml." + n) return attr_ident(a.payload);
    return "<none>";
  }
  // The -dlambda function annotations from a binding's attributes, in printlambda
  // order: inline, local, tail_mod_cons, poll (specialise is flambda-only, not
  // printed by ocamlc -dlambda).
  static std::string fn_attrs(const Attributes& attrs) {
    bool poll_err = has_attr(attrs, "poll") && attr_of(attrs, "poll") == "error";
    std::vector<std::string> a;
    // A [@poll error] function cannot be inlined or stack-allocated, so the
    // compiler forces never_inline + never_local on it.
    std::string inl = inline_of_named(attrs, "inline");
    if (poll_err) inl = "never_inline";
    if (!inl.empty()) a.push_back(inl);
    std::string loc;
    if (has_attr(attrs, "local"))
      loc = attr_of(attrs, "local") == "never" ? "never_local" : "always_local";
    if (poll_err) loc = "never_local";
    if (!loc.empty()) a.push_back(loc);
    if (has_attr(attrs, "tail_mod_cons")) a.push_back("tail_mod_cons");
    if (poll_err) a.push_back("error_poll");
    std::string r;
    for (size_t i = 0; i < a.size(); ++i) { if (i) r += " "; r += a[i]; }
    return r;
  }
  // Does `l` reach a function application through tail positions?  A cons whose
  // TAIL argument does is the recursive call that `[@tail_mod_cons]` delays.
  static bool tail_reaches_apply(const LamPtr& l) {
    if (!l) return false;
    switch (l->k) {
      case Lam::K::Apply: return true;
      case Lam::K::Sequence: return tail_reaches_apply(l->else_);
      case Lam::K::Let: case Lam::K::Letrec: return tail_reaches_apply(l->body);
      case Lam::K::IfThenElse:
        return tail_reaches_apply(l->then_) || tail_reaches_apply(l->else_);
      default: return false;
    }
  }
  // Partial `[@tail_mod_cons]`: a cons in TAIL position whose tail argument leads
  // to a (recursive) call is what TMC transforms; ocamlc's transform evaluates
  // the constructor's OTHER arguments before the tail recursion (head-first),
  // whereas a plain cons evaluates right-to-left.  We don't do the full
  // destination-passing transform (no extra stack safety), but we DO reproduce
  // the observable evaluation order by binding the head to a temp first.
  LamPtr tmc_head_first(LamPtr l) {
    if (!l) return l;
    switch (l->k) {
      case Lam::K::IfThenElse:
        l->then_ = tmc_head_first(l->then_); l->else_ = tmc_head_first(l->else_); return l;
      case Lam::K::Sequence: l->else_ = tmc_head_first(l->else_); return l;
      case Lam::K::Let: case Lam::K::Letrec: l->body = tmc_head_first(l->body); return l;
      case Lam::K::Catch:
        l->cond = tmc_head_first(l->cond); l->then_ = tmc_head_first(l->then_); return l;
      case Lam::K::Switch:
        for (auto& c : l->sw_consts) c.body = tmc_head_first(c.body);
        for (auto& c : l->sw_blocks) c.body = tmc_head_first(c.body);
        if (l->sw_default) l->sw_default = tmc_head_first(l->sw_default);
        return l;
      case Lam::K::Prim:
        if (l->prim == Prim::Makeblock && l->prim_arg == 0 && l->args.size() == 2 &&
            tail_reaches_apply(l->args[1]) && !is_const(l->args[0]) &&
            l->args[0]->k != Lam::K::Var) {
          Ident h = fresh("");  // an unnamed temp forcing head-before-tail eval
          auto let = mk(Lam::K::Let);
          let->bindings = {{h, ValueKind::Gen, l->args[0]}};
          l->args[0] = varof(h);
          let->body = l;
          return let;
        }
        return l;
      default: return l;
    }
  }
  // Stamp a function value with its binding's annotations, if any.
  LamPtr with_inline(LamPtr v, const Attributes& attrs) {
    if (v && v->k == Lam::K::Function)
      if (auto ia = fn_attrs(attrs); !ia.empty()) {
        v->inline_attr = ia;
        if (has_attr(attrs, "tail_mod_cons")) v->body = tmc_head_first(v->body);
      }
    return v;
  }

  // The runtime field layout (value/module names, in order) of a module-type
  // signature -- a functor parameter's value layout.  (externals/types take no
  // slot; only regular values, submodules, and exceptions do.)
  // Does this type constructor name Lazy.t (the predef lazy_t)?
  static bool is_lazy_lid(const Longident& lid) {
    if (auto* l = std::get_if<Lident>(&lid.v)) return l->name == "lazy_t";
    if (auto* d = std::get_if<Ldot>(&lid.v))
      if (auto* p = std::get_if<Lident>(&d->prefix->v))
        return d->name == "t" && (p->name == "Lazy" || p->name == "Stdlib__Lazy");
    return false;
  }
  // The CamlinternalMod SHAPE of a recursive module's signature, mirroring
  // translmod's init_shape: values map to Function (0) or Lazy (1), classes
  // to Class (2), submodules to `Module [|..|]` ([0: [0: ..]]); types and
  // module types take no slot.  Non-function/lazy values are rejected
  // upstream ("cannot be safely evaluated"); we return null and the binding
  // keeps the old behavior.
  LamPtr recmod_shape(const ModuleType& mt) {
    auto* ps = std::get_if<Pmty_signature>(&mt.desc);
    if (!ps) return nullptr;
    std::vector<LamPtr> elems;
    for (auto& it : ps->items) {
      if (auto* v = std::get_if<Psig_value>(&it.desc)) {
        if (std::get_if<Ptyp_arrow>(&v->vd.type->desc)) {
          elems.push_back(cint(0));  // Function
        } else if (auto* tc = std::get_if<Ptyp_constr>(&v->vd.type->desc);
                   tc && is_lazy_lid(tc->id.txt)) {
          elems.push_back(cint(1));  // Lazy
        } else {
          return nullptr;  // non-function value: unsafe upstream
        }
      } else if (auto* m = std::get_if<Psig_module>(&it.desc)) {
        if (!m->md.name.txt) continue;
        LamPtr sub = recmod_shape(*m->md.type);  // Module of recursive shapes
        if (!sub) return nullptr;
        elems.push_back(sub);
      } else if (auto* cl = std::get_if<Psig_class>(&it.desc)) {
        for (size_t i = 0; i < cl->decls.size(); ++i)
          elems.push_back(cint(2));  // Class
      } else if (std::holds_alternative<Psig_type>(it.desc) ||
                 std::holds_alternative<Psig_modtype>(it.desc) ||
                 std::holds_alternative<Psig_class_type>(it.desc) ||
                 std::holds_alternative<Psig_primitive>(it.desc)) {
        continue;  // no runtime slot
      } else {
        return nullptr;  // typext/exception/...: unsafe upstream
      }
    }
    auto arr = mk(Lam::K::ConstBlock);
    arr->prim_arg = 0;
    arr->args = std::move(elems);
    auto outer = mk(Lam::K::ConstBlock);
    outer->prim_arg = 0;
    outer->args = {arr};
    return outer;
  }

  // `S with ..` keeps S's value/module layout UNLESS a constraint destructively
  // removes a module field (`with module M := N`, Pwith_modsubst); type/modtype
  // (sub)stitutions and `with module M = N` leave the runtime field layout intact.
  static bool with_keeps_layout(const Pmty_with& pw) {
    for (auto& c : pw.constraints)
      if (std::holds_alternative<Pwith_modsubst>(c)) return false;
    return true;
  }
  std::vector<std::string> sig_layout(const ModuleType& mt0) {
    std::vector<std::string> out;
    const ModuleType* mtp = &mt0;
    while (auto* pw = std::get_if<Pmty_with>(&mtp->desc)) {
      if (!with_keeps_layout(*pw)) break;  // a modsubst removes a field -> don't peel
      mtp = pw->mt.get();
    }
    const ModuleType& mt = *mtp;
    if (auto* pi = std::get_if<Pmty_ident>(&mt.desc)) {  // a named module type S
      // The FULL dotted path first: `IncrementalEngine.INCREMENTAL_ENGINE` and a
      // sibling/top-level `INCREMENTAL_ENGINE` share a last name but differ in field
      // ORDER -- the bare last-name lookup picked the wrong one, so ENGINE's layout
      // came out one slot off and the .cmo drifted from the .cmi (menhir wild jump).
      std::string dotted;
      if (lid_to_dotted(pi->id.txt, dotted) && dotted.find('.') != std::string::npos)
        if (auto it = modtype_layout_.find(dotted); it != modtype_layout_.end()) return it->second;
      auto it = modtype_layout_.find(lid_last(pi->id.txt));
      if (it != modtype_layout_.end()) return it->second;
      // a stdlib module's named module type (`Digest.S`): its cmi modtype decl
      if (auto* d = std::get_if<Ldot>(&pi->id.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (!module_base(pl->name)) try {
            auto cmi = cmi::CmiFile::load(
                resolve_cmi(pl->name));
            for (auto& md : cmi.sig().modtypes)
              if (md.name == d->name) return mt_fields(cmi, md.type);
          } catch (...) {}
    }
    // `module type of M` (used by `include module type of String`): the runtime
    // field layout is M's own -- its non-prim values / submodules, in order.
    if (auto* pto = std::get_if<Pmty_typeof>(&mt.desc))
      return arg_layout(*pto->me);
    if (auto* ps = std::get_if<Pmty_signature>(&mt.desc)) {
      // `module type S = ..` siblings have no slot but a later `include S`
      // (which splices S's fields here) resolves through them
      std::unordered_map<std::string, const ModuleType*> local_mts;
      std::function<void(const Pmty_signature&)> walk = [&](const Pmty_signature& sg) {
        for (auto& it : sg.items) {
          if (auto* v = std::get_if<Psig_value>(&it.desc)) out.push_back(v->vd.name.txt);
          else if (auto* m = std::get_if<Psig_module>(&it.desc)) {
            if (m->md.name.txt) out.push_back(*m->md.name.txt);
          } else if (auto* ex = std::get_if<Psig_exception>(&it.desc))
            out.push_back(ex->exn.ctor.name.txt);  // exceptions occupy slots
          else if (auto* tx = std::get_if<Psig_typext>(&it.desc))
            for (auto& c : tx->ext.ctors) out.push_back(c.name.txt);
          else if (auto* cl = std::get_if<Psig_class>(&it.desc))
            for (auto& d : cl->decls) out.push_back(d.name.txt);
          else if (auto* pmt = std::get_if<Psig_modtype>(&it.desc)) {
            if (pmt->type) local_mts[pmt->name.txt] = &*pmt->type;
          } else if (auto* pin = std::get_if<Psig_include>(&it.desc)) {
            const ModuleType* im = &pin->mt;
            if (auto* ii = std::get_if<Pmty_ident>(&im->desc))
              if (auto lm = local_mts.find(lid_last(ii->id.txt)); lm != local_mts.end())
                im = lm->second;
            if (auto* isig = std::get_if<Pmty_signature>(&im->desc)) walk(*isig);
            else for (auto& n : sig_layout(*im)) out.push_back(n);
          }
        }
      };
      walk(*ps);
    }
    return dedup_keep_last(std::move(out));
  }
  // Canonical OCaml shadowing dedup for an AST-derived field-name layout: a name
  // redeclared later (include + a later decl, or two includes) keeps only its LAST
  // occurrence, at its position.  Mirrors cmi::cmiw::dedup_shadowed_fields (the .cmi
  // writer) so the .cmi field index, the .cmo block index and the functor-param
  // index all agree.  Name-keyed (a value and a module sharing a name -- vanishingly
  // rare -- would merge; the shadowed cases here are same-namespace).
  static std::vector<std::string> dedup_keep_last(std::vector<std::string> in) {
    std::unordered_map<std::string, int> last;
    for (int i = 0; i < (int)in.size(); ++i) last[in[i]] = i;
    bool dup = false;
    for (int i = 0; i < (int)in.size(); ++i) if (last[in[i]] != i) { dup = true; break; }
    if (!dup) return in;
    std::vector<std::string> out; out.reserve(in.size());
    for (int i = 0; i < (int)in.size(); ++i) if (last[in[i]] == i) out.push_back(in[i]);
    return out;
  }

  // Compile a module expression to its Lambda value: a structure is a record of
  // its exports; a functor is `(function X is_a_functor <body>)`.
  // The runtime field names a module argument exposes (for projecting it to a
  // functor's parameter signature): a local module's layout or a stdlib module's.
  std::vector<std::string> arg_layout(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
    if (auto* pi = std::get_if<Pmod_ident>(&me->desc)) {
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {
        if (auto it = module_layout_.find(l->name); it != module_layout_.end()) {
          std::vector<std::string> v(it->second.size());
          for (auto& [n, i] : it->second) if (i >= 0 && i < (int)v.size()) v[i] = n;
          return v;
        }
        if (!fields_of(l->name).empty()) {
          auto& fm = fields_of(l->name);
          std::vector<std::string> v(fm.size());
          for (auto& [n, i] : fm) if (i >= 0 && i < (int)v.size()) v[i] = n;
          return v;
        }
        // A submodule of an opened module (`open Types; Set.Make(TransientTypeOps)`):
        // its field layout drives the argument->parameter coercion (project the
        // arg's `compare` to OrderedType field 0).  Without this the WHOLE arg is
        // passed uncoerced and Set/Hashtbl read the wrong member as Ord.compare.
        if (std::string osp = opened_submodule_path(l->name); !osp.empty()) {
          auto& sm = submodule_of(osp);
          if (sm.ok && !sm.fields.empty()) {
            int mx = 0; for (auto& [n, i] : sm.fields) mx = std::max(mx, i + 1);
            std::vector<std::string> v(mx);
            for (auto& [n, i] : sm.fields) if (i >= 0 && i < mx) v[i] = n;
            return v;
          }
        }
      }
      // `Stdlib.Array` etc.: its own fields are the canonical unit's fields.
      std::string dotted;
      if (lid_to_dotted(pi->id.txt, dotted)) {
        std::string c = canon_stdlib_path(dotted);
        if (c.find('.') == std::string::npos && !module_base(c)) {
          auto& fm = fields_of(c);
          std::vector<std::string> v(fm.size());
          for (auto& [n, i] : fm) if (i >= 0 && i < (int)v.size()) v[i] = n;
          return v;
        }
        // A nested cross-module path (`Misc.Stdlib.String`): navigate its cmi so
        // a functor argument that is such a path can be projected to the param.
        if (auto nf = dotted_module_fields(c); !nf.empty()) return nf;
        if (auto nf = dotted_module_fields(dotted); !nf.empty()) return nf;
      }
    }
    return {};
  }
  // The first parameter signature of a (local) functor definition.
  std::vector<std::string> functor_param_layout(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
    if (auto* pf = std::get_if<Pmod_functor>(&me->desc))
      if (auto* fp = std::get_if<Functor_named>(&pf->param); fp && fp->type)
        return sig_layout(*fp->type);
    return {};
  }
  // The parameter's declared module TYPE (for nested argument coercion), null if
  // none.  Keyed alongside functor_param_ by the functor's binding name.
  std::unordered_map<std::string, const ModuleType*> functor_param_sig_;
  const ModuleType* functor_param_type(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
    if (auto* pf = std::get_if<Pmod_functor>(&me->desc))
      if (auto* fp = std::get_if<Functor_named>(&pf->param); fp && fp->type)
        return fp->type.get();
    return nullptr;
  }
  // All declared parameter types of a (possibly multi-parameter) functor
  // definition, in order; an entry is null for an unannotated / `()` param.
  std::vector<const ModuleType*> functor_param_types(const ModuleExpr& me0) {
    std::vector<const ModuleType*> out;
    const ModuleExpr* me = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
    while (auto* pf = std::get_if<Pmod_functor>(&me->desc)) {
      const Functor_named* fp = std::get_if<Functor_named>(&pf->param);
      out.push_back(fp && fp->type ? fp->type.get() : nullptr);
      me = pf->body.get();
    }
    return out;
  }
  // A local functor's per-parameter declared types (for coercing a functor-
  // typed argument's result; functor_param_sig_ holds only the FIRST).
  std::unordered_map<std::string, std::vector<const ModuleType*>> functor_param_types_;
  // From this unit's own .mli (.cmi): a top-level functor's name -> its RESULT
  // signature field order (Map.Make -> S's fields).  Used to coerce the functor
  // BODY struct in map.ml (which has no `: S` on the .ml side) to S's layout, so
  // Map.Make(..).cardinal etc. land at the right field index.
  std::unordered_map<std::string, std::vector<std::string>> mli_functor_results_;
  // From this unit's own .mli (.cmi): a SUBMODULE's value label signatures
  // (`Domain.DLS.new_key`'s `?split_from_parent`), keyed by the immediate
  // submodule name -> value name -> FnSig.  Lets a same-unit call to a local
  // submodule member (`DLS.new_key (fun..)`) fill the omitted optional, just as
  // submodule_of does for a cross-module call.
  std::unordered_map<std::string, std::unordered_map<std::string, FnSig>> local_member_sig_;
  // A LOCAL functor's result-struct value label-sigs (functor name -> value ->
  // FnSig), recorded at the functor's definition.  On `module M = F(arg)` for a
  // local F, copied to local_member_sig_[M] so `M.f` fills f's omitted optionals
  // (typecore's `Constructor = NameChoice(..)` -> `Constructor.disambiguate`'s
  // leading ?warn/?filter).
  std::unordered_map<std::string, std::unordered_map<std::string, FnSig>> local_functor_member_sig_;
  // From this unit's own .mli (.cmi): a SUBMODULE's exported field order, keyed by
  // the immediate submodule name.  The top-level impl->intf coercion uses it to
  // reorder a submodule's OWN block to its .mli layout (domain.ml's DLS struct has
  // new_key at internal field 5, but Domain.DLS's signature exposes it at field 0).
  std::unordered_map<std::string, std::vector<std::string>> mli_submodule_fields_;
  // This unit's own decoded .mli (.cmi) signature, kept whole so the top-level
  // impl->intf coercion can RECURSE into nested submodules (Misc.Stdlib.String)
  // -- mli_submodule_fields_ flattens only one level.  Empty when no .mli.
  cmi::Signature mli_cmi_sig_;
  bool has_mli_cmi_ = false;
  // Set just before compiling a functor whose body must be coerced to its .mli
  // result signature; consumed (and cleared) by compile_module_expr's body build.
  std::vector<std::string> pending_functor_coerce_;
  // Coerce a stdlib/local module VALUE to a target signature layout, eta-stubbing
  // members that are EXTERNALS of the source module (`Int32.neg` = %int32_neg,
  // not a field) and field-reading the rest.  This is how a functor argument's
  // `module Ops = Int32` is narrowed to a sub-signature `sig val neg .. end`.
  LamPtr coerce_module_value(const LamPtr& src, const std::string& src_mod,
                             const std::vector<std::string>& target) {
    if (target.empty()) return src;
    auto& flayout = fields_of(src_mod);  // name -> field index (ensures prims too)
    Ident id = fresh("let");
    bool bind = !is_global_path(src) && !is_pure_path(src);
    LamPtr base = bind ? varof(id) : src;
    std::vector<LamPtr> fs;
    for (auto& nm : target) {
      StdPrim sp = value_prim(src_mod, nm);
      if (!sp.name.empty()) if (LamPtr s = prim_stub(sp)) { fs.push_back(s); continue; }
      auto fit = flayout.find(nm);
      auto fr = mk(Lam::K::Prim); fr->prim = Prim::FieldImm;
      fr->prim_arg = fit != flayout.end() ? fit->second : 0; fr->args = {base};
      fs.push_back(fr);
    }
    auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
    blk->args = std::move(fs);
    if (!bind) return blk;
    auto lt = mk(Lam::K::Let); lt->bindings = {{id, ValueKind::Gen, src}}; lt->body = blk;
    return lt;
  }
  // The source stdlib/local module name of a struct's `module nm = <Ident>` member.
  static std::string struct_member_module_src(const Pmod_structure& ps, const std::string& nm) {
    for (auto& it : ps.items)
      if (auto* pm = std::get_if<Pstr_module>(&it.desc))
        if (pm->binding.name.txt && *pm->binding.name.txt == nm) {
          const ModuleExpr* me = &pm->binding.expr;
          while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
          if (auto* pi = std::get_if<Pmod_ident>(&me->desc))
            if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) return l->name;
        }
    return "";
  }
  // Coerce a struct-LITERAL functor argument to the parameter signature `pmt`,
  // reordering to the sig's field order and recursively coercing a module member
  // (`module Ops = Int32` -> the sig's `Ops` sub-signature, eta-stubbing externals).
  // Null when the shape isn't handled (the caller falls back to the flat path).
  LamPtr coerce_struct_arg(const Pmod_structure& ps, const ModuleType& pmt) {
    const Pmty_signature* psig = sig_items_of(pmt);
    if (!psig) return nullptr;
    std::vector<std::string> force = sig_layout(pmt);
    if (force.empty()) return nullptr;
    std::vector<std::string> sub;
    LamPtr block = build_module(ps.items, &sub, nullptr, &force);
    LamPtr body = block;
    while (body && (body->k == Lam::K::Let || body->k == Lam::K::Letrec)) body = body->body;
    if (!body || body->k != Lam::K::Prim || body->prim != Prim::Makeblock ||
        body->args.size() != sub.size())
      return nullptr;
    std::unordered_map<std::string, LamPtr> val;
    for (size_t i = 0; i < sub.size(); ++i) val[sub[i]] = body->args[i];
    std::vector<LamPtr> fs;
    for (auto& nm : force) {
      auto it = val.find(nm);
      if (it == val.end()) return nullptr;  // a sig field absent from the struct
      LamPtr v = it->second;
      if (const ModuleType* smt = sig_member_modtype(*psig, nm)) {
        std::string srcmod = struct_member_module_src(ps, nm);
        if (!srcmod.empty()) v = coerce_module_value(v, srcmod, sig_layout(*smt));
        else {
          // A struct-LITERAL member (`module Key = struct include Numbers.Int;
          // let of_string end`): reorder its block to the parameter's member
          // signature, recursing into ITS submodules (Key.Map), so a functor-body
          // field read (`S.Key.Map.empty`) lands on the right field.  Without this
          // Arg_helper.Make's argument Key keeps its include-order layout (Map at a
          // high offset) and the parsed record gets a garbage Map -> GC segfault.
          const ModuleExpr* sme = nullptr;
          for (auto& mi : ps.items)
            if (auto* pm = std::get_if<Pstr_module>(&mi.desc))
              if (pm->binding.name.txt && *pm->binding.name.txt == nm) {
                sme = &pm->binding.expr; break;
              }
          if (LamPtr c = coerce_block(v, layout_vec(nm), sig_layout(*smt), sme,
                                      sig_items_of(*smt)))
            v = c;
        }
      }
      fs.push_back(v);
    }
    body->args = std::move(fs);  // reuse the block (its bindings), param-ordered
    return block;
  }
  // The parameter signature of a cross-module functor (`Arg_helper.Make`), copied
  // from the head module's cmi so it outlives the loaded CmiFile.  False when the
  // path isn't such a functor.
  bool load_functor_param_sig(const std::string& mod, const std::string& name,
                              cmi::Signature& out) {
    if (module_base(mod) || fields_of(mod).empty()) return false;
    try {
      auto cmi = cmi::CmiFile::load(resolve_cmi(mod));
      for (auto& md : cmi.sig().modules)
        if (md.name == name && md.type &&
            md.type->kind == cmi::ModuleType::Functor && md.type->functor_param_type)
          if (const cmi::Signature* ps = mt_sig(cmi, md.type->functor_param_type)) {
            out = *ps; return true;  // copy (shares shared_ptr nodes, kept alive)
          }
    } catch (...) {}
    return false;
  }
  // Coerce a struct-literal argument of a CROSS-MODULE functor to its cmi
  // parameter signature, reordering the top level AND recursing into nested
  // submodules (Arg_helper.Make's argument `module Key = struct include
  // Numbers.Int; let of_string end` must be laid out as the param's Key
  // [of_string; Map], else the functor body reads `field 1 Key` as a non-module
  // and stores garbage in the parsed record -> GC segfault).  The AST-based
  // coerce_struct_arg only fires for local functors (it needs an AST param sig);
  // this is its cmi twin for separately-compiled functors.
  LamPtr coerce_struct_arg_cmi(const Pmod_structure& ps, const cmi::Signature& psig) {
    if (psig.fields.empty()) return nullptr;
    std::vector<std::string> sub;
    LamPtr block = build_module(ps.items, &sub, nullptr, &psig.fields);
    LamPtr body = block;
    while (body && (body->k == Lam::K::Let || body->k == Lam::K::Letrec)) body = body->body;
    if (!body || body->k != Lam::K::Prim || body->prim != Prim::Makeblock ||
        body->args.size() != sub.size())
      return nullptr;
    std::unordered_map<std::string, LamPtr> val;
    for (size_t i = 0; i < sub.size(); ++i) val[sub[i]] = body->args[i];
    std::vector<LamPtr> fs;
    for (auto& nm : psig.fields) {
      auto it = val.find(nm);
      if (it == val.end()) return nullptr;
      LamPtr v = it->second;
      if (const cmi::Signature* nsig = nested_cmi_sig(psig, nm))
        if (LamPtr c = coerce_block(v, layout_vec(nm), nsig->fields, nullptr, nullptr,
                                    nsig, nm))
          v = c;
      fs.push_back(v);
    }
    body->args = std::move(fs);
    return block;
  }
  // A functor used as an ARGUMENT whose declared parameter type is a functor type
  // `functor(P) -> R` (a higher-order functor parameter, e.g. Set's PowerSet
  // SetOrd): compile `functor(P) -> body` but coerce its RESULT to R's layout, so
  // the body's module value is narrowed (`functor(S) -> S` over result
  // Set.OrderedType -> `(makeblock 0 (field_mut 34 S))`).  Null when the shape
  // isn't the simple single-parameter / pure-path-body case (flat path applies).
  LamPtr compile_coerced_functor_arg(const ModuleExpr& arg, const ModuleType& expected) {
    const ModuleExpr* a = &arg;
    while (auto* pc = std::get_if<Pmod_constraint>(&a->desc)) a = pc->me.get();
    auto* pf_a = std::get_if<Pmod_functor>(&a->desc);
    auto* pf_e = std::get_if<Pmty_functor>(&expected.desc);
    if (!pf_a || !pf_e) return nullptr;
    if (std::get_if<Pmod_functor>(&pf_a->body->desc)) return nullptr;  // multi-param: bail
    std::vector<std::string> target = sig_layout(*pf_e->body);
    if (target.empty()) return nullptr;
    auto fn = mk(Lam::K::Function); fn->inline_attr = "is_a_functor";
    const Functor_named* fp = std::get_if<Functor_named>(&pf_a->param);
    std::string nm = fp ? (fp->name.txt ? *fp->name.txt : "_") : "*";
    Ident pid = fresh(nm);
    bool had = module_ident_.count(nm); Ident oldid = had ? module_ident_[nm] : Ident{};
    auto oldlay = module_layout_[nm];
    if (fp && fp->type) { module_ident_[nm] = pid; register_sig_layouts(nm, *fp->type); }
    fn->params.push_back({pid, ValueKind::Gen});
    LamPtr body = compile_module_expr(*pf_a->body);
    std::vector<std::string> src = module_result_layout(*pf_a->body);
    fn->body = body;
    if (is_pure_path(body) && !src.empty() && src != target) {  // direct projection
      bool ok = true;
      for (auto& t : target) if (std::find(src.begin(), src.end(), t) == src.end()) ok = false;
      if (ok) {
        std::vector<LamPtr> fs;
        for (auto& t : target) {
          int idx = 0;
          for (int i = 0; i < (int)src.size(); ++i) if (src[i] == t) { idx = i; break; }
          auto fr = mk(Lam::K::Prim); fr->prim = Prim::FieldMut; fr->prim_arg = idx;
          fr->args = {body};
          fs.push_back(fr);
        }
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
        blk->args = std::move(fs); fn->body = blk;
      }
    }
    if (had) module_ident_[nm] = oldid; else module_ident_.erase(nm);
    module_layout_[nm] = oldlay;
    return fn;
  }
  // The head functor's binding name and the 0-based index of THIS application's
  // argument (counting nested applies in `f`), for looking up the formal param.
  std::pair<std::string, int> functor_head_arg_index(const ModuleExpr& f) {
    int depth = 0;
    const ModuleExpr* m = &f;
    for (;;) {
      if (auto* pa = std::get_if<Pmod_apply>(&m->desc)) { ++depth; m = pa->f.get(); continue; }
      if (auto* pu = std::get_if<Pmod_apply_unit>(&m->desc)) { ++depth; m = pu->f.get(); continue; }
      break;
    }
    std::string nm;
    if (auto* pi = std::get_if<Pmod_ident>(&m->desc))
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) nm = l->name;
    return {nm, depth};
  }
  // Compile a functor application `F(Arg)` with ocamlc's coercion: the argument is
  // projected (by field name, via field_mut) to F's parameter signature when it
  // has extra/reordered fields, and a global-path operand is bound to an (unused)
  // `let/N` then re-read.
  LamPtr compile_functor_apply(const Pmod_apply& pa) {
    LamPtr fval; std::vector<std::string> param;
    const ModuleType* param_mt = nullptr;
    if (auto* pi = std::get_if<Pmod_ident>(&pa.f->desc)) {
      if (auto* d = std::get_if<Ldot>(&pi->id.txt.v)) {
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (!module_base(pl->name) && !fields_of(pl->name).empty()) {  // stdlib functor
            auto fs = stdlib_functor(pl->name, d->name);
            if (fs.ok) { fval = field_of(global_of(pl->name), fs.idx); param = fs.param; }
          }
      } else if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {  // local functor
        if (auto it = functor_param_.find(l->name); it != functor_param_.end()) param = it->second;
        if (auto it = functor_param_sig_.find(l->name); it != functor_param_sig_.end()) param_mt = it->second;
      }
      if (param.empty()) {  // a functor member of a local module (X.F): sig info
        std::string dotted;
        if (lid_to_dotted(pi->id.txt, dotted))
          if (auto it = functor_param_.find(dotted); it != functor_param_.end())
            param = it->second;
      }
      if (param.empty()) {  // a deep/cross-module stdlib functor (Weak.Make,
        std::string dotted;  // Ephemeron.K1.Make) whose param sig is in another cmi
        if (lid_to_dotted(pi->id.txt, dotted)) param = stdlib_functor_param(dotted);
      }
    }
    if (!fval) fval = compile_module_expr(*pa.f);
    // A FUNCTOR-typed formal parameter (a higher-order functor like Set's PowerSet
    // SetOrd): coerce the functor argument's RESULT to the declared result sig.
    {
      auto [hnm, aidx] = functor_head_arg_index(*pa.f);
      if (!hnm.empty())
        if (auto it = functor_param_types_.find(hnm); it != functor_param_types_.end())
          if (aidx < (int)it->second.size() && it->second[aidx])
            if (std::get_if<Pmty_functor>(&it->second[aidx]->desc))
              if (LamPtr c = compile_coerced_functor_arg(*pa.arg, *it->second[aidx])) {
                auto ap = mk(Lam::K::Apply); ap->fn = fval; ap->args = {c}; return ap;
              }
    }
    // A struct-LITERAL argument with a known parameter signature is coerced with
    // nested module-member coercion (`module Ops = Int32` -> the param's Ops sub-
    // sig), which the flat projection below can't do.
    if (param_mt)
      if (auto* aps = std::get_if<Pmod_structure>(&pa.arg->desc))
        if (LamPtr c = coerce_struct_arg(*aps, *param_mt)) {
          auto a = mk(Lam::K::Apply); a->fn = fval; a->args = {c}; return a;
        }
    // The same nested coercion for a CROSS-MODULE functor (Arg_helper.Make),
    // whose parameter signature lives in another cmi (no AST param_mt).
    if (!param_mt)
      if (auto* aps = std::get_if<Pmod_structure>(&pa.arg->desc))
        if (auto* pi = std::get_if<Pmod_ident>(&pa.f->desc))
          if (auto* d = std::get_if<Ldot>(&pi->id.txt.v))
            if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
              cmi::Signature psig;
              if (load_functor_param_sig(pl->name, d->name, psig))
                if (LamPtr c = coerce_struct_arg_cmi(*aps, psig)) {
                  auto a = mk(Lam::K::Apply); a->fn = fval; a->args = {c}; return a;
                }
            }
    LamPtr aval = compile_module_expr(*pa.arg);
    LamPtr acoerced = aval;
    auto alay = arg_layout(*pa.arg);
    // The stdlib module name of a bare `Pmod_ident` argument (`F(Int32)`), so a
    // param value that is one of its EXTERNALS (absent from the field layout) can
    // be eta-stubbed from the primitive rather than mis-read as field 0.
    std::string arg_mod;
    {
      const ModuleExpr* am = pa.arg.get();
      while (auto* pc = std::get_if<Pmod_constraint>(&am->desc)) am = pc->me.get();
      if (auto* pi = std::get_if<Pmod_ident>(&am->desc))
        if (auto* l = std::get_if<Lident>(&pi->id.txt.v))
          if (!module_base(l->name) && !fields_of(l->name).empty()) arg_mod = l->name;
    }
    // Project only when the argument's layout is known and differs from the
    // parameter signature; an unknown layout (e.g. a struct literal) is passed as is.
    if (!param.empty() && !alay.empty() && param != alay) {
      std::vector<LamPtr> fs;
      for (auto& nm : param) {
        int idx = -1;
        for (int i = 0; i < (int)alay.size(); ++i) if (alay[i] == nm) { idx = i; break; }
        if (idx < 0 && !arg_mod.empty()) {
          // not a runtime field: an external -> eta-stub the primitive (ocamlc's
          // `(function prim stub (Mod.prim prim))`)
          StdPrim sp = value_prim(arg_mod, nm);
          if (!sp.name.empty()) if (LamPtr s = prim_stub(sp)) { fs.push_back(s); continue; }
        }
        if (idx < 0) idx = 0;  // last-resort (unknown value): field 0 as before
        auto fr = mk(Lam::K::Prim); fr->prim = Prim::FieldMut; fr->prim_arg = idx; fr->args = {aval};
        fs.push_back(fr);
      }
      auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
      blk->args = std::move(fs); acoerced = blk;
    }
    auto wrap = [&](const LamPtr& src, const LamPtr& body) -> LamPtr {
      if (!is_global_path(src)) return body;
      Ident id = fresh("let");
      auto l = mk(Lam::K::Let); l->bindings = {{id, ValueKind::Gen, src}}; l->body = body; return l;
    };
    auto a = mk(Lam::K::Apply);
    a->fn = wrap(fval, fval);
    a->args = {wrap(aval, acoerced)};
    return a;
  }
  LamPtr compile_module_expr(const ModuleExpr& me) {
    if (auto* ps = std::get_if<Pmod_structure>(&me.desc)) {
      std::vector<std::string> sub;
      return build_module(ps->items, &sub);
    }
    if (std::holds_alternative<Pmod_functor>(me.desc)) {
      // A multi-parameter functor `F (A) (B) = ...` parses as nested Pmod_functor
      // but lowers to ONE multi-argument function `(function A B is_a_functor body)`
      // (ocamlc flattens consecutive functor params), not a curried chain.
      auto fn = mk(Lam::K::Function);
      fn->inline_attr = "is_a_functor";  // printed in the header after the params
      struct Saved { std::string nm; bool had; Ident id;
                     decltype(module_layout_[std::string{}]) lay; };
      std::vector<Saved> saves;
      std::vector<SigCtorSave> ctor_saves;  // param-sig variant ctors (`type u=X|Y|Z`)
      std::vector<SigExtSave> ext_saves;    // param-sig extension ctors (`type t+=A|B`)
      std::vector<std::pair<std::string, std::unordered_map<std::string, FnSig>>> pvs_saves;
      const ModuleExpr* cur = &me;
      while (auto* pf = std::get_if<Pmod_functor>(&cur->desc)) {
        std::string nm = "*";
        const Functor_named* fp = std::get_if<Functor_named>(&pf->param);
        if (fp) nm = fp->name.txt ? *fp->name.txt : "_";  // anonymous param prints `_`
        Ident pid = fresh(nm);
        // Bind the parameter X (with its signature's value layout) so `X.foo`
        // inside the body resolves to `(field_imm i X)`.  Save/restore for nesting.
        saves.push_back({nm, module_ident_.count(nm) != 0,
                         module_ident_.count(nm) ? module_ident_[nm] : Ident{},
                         module_layout_[nm]});
        if (fp && fp->type) {
          module_ident_[nm] = pid;
          // Register the param's flat layout AND its nested submodule layouts
          // (so `X.Sub.foo` / `open X; open Sub; foo` resolve -- boxedints).
          register_sig_layouts(nm, *fp->type);
          // and its variant- / extension-type constructors (resolved after `open X`).
          auto cs = register_sig_ctors(*fp->type);
          ctor_saves.insert(ctor_saves.end(), cs.begin(), cs.end());
          auto es = register_sig_exts(nm, pid, *fp->type);
          ext_saves.insert(ext_saves.end(), es.begin(), es.end());
          // and its labelled value sigs (`H.create 51` fills the optional ?random).
          pvs_saves.push_back({nm, param_value_sigs_.count(nm) ? param_value_sigs_[nm]
                                                              : std::unordered_map<std::string, FnSig>{}});
          param_value_sigs_.erase(nm);
          register_param_value_sigs(nm, *fp->type);
        }
        fn->params.push_back({pid, ValueKind::Gen});
        cur = pf->body.get();
      }
      // If the enclosing binding requested a result coercion (.mli `module Make
      // (..) : S`, the .ml has no `: S`), lay the body struct out per S's field
      // order AND RESTRICT it to S's members.  `build_module(coerce)` reorders but
      // keeps the struct's extra members (private helpers like Map.Make's
      // `bal`/`create`/`*_aux`/`rev_seq_of_enum_`), so the result block stays in
      // raw order with helpers and `find` is NOT at S's index -- a CROSS-MODULE /
      // nested consumer reading the .cmi's S layout (env.ml's
      // Misc.Stdlib.String.Map.find @ field 16) then hits the wrong member.  Run
      // coerce_block (target = S) to drop the helpers and reorder, exactly like the
      // `(struct : S)` ascription path below.
      auto pending = std::move(pending_functor_coerce_); pending_functor_coerce_.clear();
      if (auto* ps = std::get_if<Pmod_structure>(&cur->desc); ps && !pending.empty()) {
        std::vector<std::string> sub;
        LamPtr inner = build_module(ps->items, &sub, nullptr, &pending);
        if (LamPtr c = coerce_block(inner, sub, pending)) fn->body = c;
        else fn->body = inner;
      } else {
        fn->body = compile_module_expr(*cur);
      }
      restore_sig_exts(ext_saves);
      restore_sig_ctors(ctor_saves);
      for (auto it = pvs_saves.rbegin(); it != pvs_saves.rend(); ++it) {
        if (it->second.empty()) param_value_sigs_.erase(it->first);
        else param_value_sigs_[it->first] = it->second;
      }
      for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
        if (it->had) module_ident_[it->nm] = it->id; else module_ident_.erase(it->nm);
        module_layout_[it->nm] = it->lay;
      }
      return fn;
    }
    if (auto* pc = std::get_if<Pmod_constraint>(&me.desc)) {
      // (struct .. : S): build the struct forcing S's fields to be exported even
      // if elided as aliases (`module Elem = E`), then coerce_block reorders +
      // applies any nested module coercion.  `sub` is the actual export layout.
      if (auto* ps = std::get_if<Pmod_structure>(&pc->me->desc)) {
        std::vector<std::string> force = sig_layout(*pc->mt);
        std::vector<std::string> sub;
        LamPtr inner = build_module(ps->items, &sub, nullptr, force.empty() ? nullptr : &force);
        if (LamPtr c = coerce_block(inner, sub, sig_layout(*pc->mt),
                                    pc->me.get(), sig_items_of(*pc->mt)))
          return c;
        return inner;
      }
      // `(val e : S)`: a first-class-module unpack constrained to a module type.
      // module_result_layout of the unpack already reports S's layout, so the
      // coerce_block below is a no-op -- but the runtime value (a match of raw
      // `(module Int32)`/`(module Int64)` packs) is NOT in S's layout.  Push S to
      // the leaf packs so each is coerced (targetint's `include (val (match.. )
      // : S)` -- else it reads S's field order off Int32's layout -> garbage).
      if (auto* un = std::get_if<Pmod_unpack>(&pc->me->desc))
        if (auto* mi = std::get_if<Pmty_ident>(&pc->mt->desc)) {
          std::string saved = unpack_pkg_; unpack_pkg_ = lid_last(mi->id.txt);
          LamPtr v = expr(*un->e); unpack_pkg_ = saved;
          return v;
        }
      // (M : S) over a narrower/reordered signature projects to S's layout
      // (include (A : sig val f .. val x .. end) must not read raw slots)
      LamPtr inner = compile_module_expr(*pc->me);
      if (LamPtr c = coerce_block(inner, module_result_layout(*pc->me), sig_layout(*pc->mt),
                                  pc->me.get(), sig_items_of(*pc->mt)))
        return c;
      return inner;
    }
    if (auto* pi = std::get_if<Pmod_ident>(&me.desc)) {  // a module in value position
      {  // a (possibly deep) path through local modules: chain of field reads
        std::string dotted;
        if (lid_to_dotted(pi->id.txt, dotted)) {
          if (auto mp = resolve_module_path(dotted); mp.base) return mp.base;
          // `Stdlib.Array` / `Stdlib.Float.Array` are the units `Array` /
          // `Float.Array` (Stdlib re-exports them as aliases); strip the explicit
          // prefix so the path resolves against the real unit, not Stdlib's block.
          if (std::string c = canon_stdlib_path(dotted); c != dotted)
            if (LamPtr v = stdlib_module_path(c)) return v;
        }
      }
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {
        if (!fields_of(l->name).empty()) {  // a stdlib module: its global
          auto g = mk(Lam::K::Prim); g->prim = Prim::Global; g->prim_id = global_of(l->name);
          return g;
        }
        // A bare module name that is a SUBMODULE of an opened module, used in
        // module position (`open Types; Set.Make(TransientTypeOps)` where
        // TransientTypeOps = Types.TransientTypeOps): resolve it to the field path
        // into the opened module's block.  Without this it fell through to null ->
        // the functor argument compiled to the constant 0 -> a broken Set/Map.
        std::string osp = opened_submodule_path(l->name);
        if (!osp.empty()) {
          auto& sm = submodule_of(osp);
          if (sm.ok) {
            auto g = mk(Lam::K::Prim); g->prim = Prim::Global;
            g->prim_id = global_of(sm.base.empty() ? osp.substr(0, osp.find('.'))
                                                   : sm.base);
            LamPtr cur = g;
            for (int ix : sm.path) cur = fieldimm(ix, cur);
            return cur;
          }
        }
      }
      if (auto* d = std::get_if<Ldot>(&pi->id.txt.v))  // M.Sub -> field of M's block
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
          if (LamPtr base = module_base(pl->name)) {
            auto& lay = module_layout_[pl->name];
            if (auto f = lay.find(d->name); f != lay.end()) {
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {base};
              return fi;
            }
          }
          if (auto& fm = fields_of(pl->name); !fm.empty())  // stdlib M.Sub (e.g. Set.Make)
            if (auto f = fm.find(d->name); f != fm.end())
              return field_of(global_of(pl->name), f->second);
        }
      {  // a deep stdlib submodule member (Sys.Immediate64.Make)
        std::string dotted;
        if (lid_to_dotted(pi->id.txt, dotted)) {
          size_t lastd = dotted.rfind('.');
          if (lastd != std::string::npos && lastd != dotted.find('.') &&
              !module_base(dotted.substr(0, dotted.find('.'))))
            if (LamPtr v = submodule_value(dotted.substr(0, lastd), dotted.substr(lastd + 1)))
              return v;
        }
      }
    }
    if (auto* pa = std::get_if<Pmod_apply>(&me.desc)) return compile_functor_apply(*pa);
    if (auto* pu = std::get_if<Pmod_apply_unit>(&me.desc)) {  // F() -> (apply F 0)
      auto a = mk(Lam::K::Apply);
      a->fn = compile_module_expr(*pu->f);
      a->args = {cint(0)};
      return a;
    }
    if (auto* un = std::get_if<Pmod_unpack>(&me.desc)) {  // (val e): unpack is transparent
      // `(val e : (module S))`: the package type rides on e as a Pexp_constraint
      // (Ptyp_package).  A first-class module built by a match/if packs each arm
      // RAW, so push S down to the leaf `(module M)` packs and coerce them to S's
      // layout -- else `include (val.. : S)` reads S's field order off the arm's
      // own layout (targetint's Int32/Int64) -> a malformed re-export block.
      const Expression* e = un->e.get();
      std::string pkg;
      if (auto* pc = std::get_if<Pexp_constraint>(&e->desc))
        if (pc->t)
          if (auto* tp = std::get_if<Ptyp_package>(&pc->t->desc)) {
            lid_to_dotted(tp->path.txt, pkg);
            e = pc->e.get();
          }
      if (pkg.empty()) return expr(*un->e);
      std::string saved = unpack_pkg_; unpack_pkg_ = pkg;
      LamPtr v = expr(*e); unpack_pkg_ = saved;
      return v;
    }
    // A QUALIFIED module path (`Lib.Make`) that reached here projects into its head
    // (`.Make`), so the head must resolve NOW; if it is bound nowhere this is
    // unbound, not a `0` placeholder (which compiles `Lib.Make(arg)` to `(apply 0
    // arg)` -> a wild call).  A BARE module ident (`module M = Stdlib__ArrayLabels`)
    // is NOT flagged: it may be a forward-declared alias whose unit is linked later
    // (the stdlib's `*Labels` aliases) -- it lowers to a global the linker resolves.
    if (auto* pi = std::get_if<Pmod_ident>(&me.desc))
      if (std::holds_alternative<Ldot>(pi->id.txt.v)) {
        std::string head = lid_head(pi->id.txt);
        if (!module_head_resolvable(head)) {
          std::string full; lid_to_dotted(pi->id.txt, full);
          unbound_module(head, full);
        }
      }
    return mk(Lam::K::ConstInt);  // other module exprs: best-effort
  }
  // The runtime field layout a module expression *produces*: a structure's
  // exports, a functor application's result = the functor's result layout, a
  // constraint's = the ascribed signature.
  // The exported names of a structure (value/module/exception, in declaration
  // order; a redefined name moves to its last position) -- the runtime layout,
  // computed statically (no compilation/side effects).
  std::vector<std::string> struct_export_names(const Structure& s) {
    std::vector<std::string> out;
    auto add = [&](const std::string& n) {
      for (size_t i = 0; i < out.size(); ++i) if (out[i] == n) { out.erase(out.begin() + i); break; }
      out.push_back(n);
    };
    for (auto& it : s) {
      if (auto* sv = std::get_if<Pstr_value>(&it.desc)) {
        for (auto& b : sv->bindings)
          if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) add(pv->name.txt);
      } else if (auto* pm = std::get_if<Pstr_module>(&it.desc)) {
        if (pm->binding.name.txt) add(*pm->binding.name.txt);
      } else if (auto* ex = std::get_if<Pstr_exception>(&it.desc)) {
        add(ex->exn.ctor.name.txt);  // exceptions occupy slots
      } else if (auto* tx = std::get_if<Pstr_typext>(&it.desc)) {
        for (auto& c : tx->ext.ctors) add(c.name.txt);
      } else if (auto* cl = std::get_if<Pstr_class>(&it.desc)) {
        for (auto& d : cl->decls) add(d.name.txt);
      } else if (auto* pin = std::get_if<Pstr_include>(&it.desc)) {
        // `include ME` splices ME's exported fields here
        auto rl = module_result_layout(pin->expr);
        if (rl.empty()) rl = arg_layout(pin->expr);  // a module path: its own fields
        for (auto& n : rl) add(n);
      }
    }
    return out;
  }
  // The field-name vector of a registered (possibly dotted) layout key.
  std::vector<std::string> layout_vec(const std::string& key) {
    auto it = module_layout_.find(key);
    if (it == module_layout_.end()) return {};
    std::vector<std::string> v(it->second.size());
    for (auto& [n, i] : it->second) if (i >= 0 && i < (int)v.size()) v[i] = n;
    return v;
  }
  std::vector<std::string> module_result_layout(const ModuleExpr& me) {
    if (auto* ps = std::get_if<Pmod_structure>(&me.desc)) return struct_export_names(ps->items);
    if (auto* pc = std::get_if<Pmod_constraint>(&me.desc)) {
      auto s = sig_layout(*pc->mt);
      return s.empty() ? module_result_layout(*pc->me) : s;
    }
    if (auto* pf = std::get_if<Pmod_functor>(&me.desc)) return module_result_layout(*pf->body);
    if (auto* pi = std::get_if<Pmod_ident>(&me.desc)) {  // a module path
      std::string dotted;
      if (lid_to_dotted(pi->id.txt, dotted)) {
        auto v = layout_vec(dotted);          // a local module / alias chain
        if (!v.empty()) return v;
        dotted = canon_stdlib_path(dotted);   // `Stdlib.Float.Array` -> `Float.Array`
        if (dotted.find('.') == std::string::npos) {
          // a local module with no value exports shadows any stdlib namesake
          if (module_base(dotted)) return v;
          auto& fm = fields_of(dotted);       // a stdlib module's cmi fields
          if (!fm.empty()) {
            v.resize(fm.size());
            for (auto& [n, i] : fm) if (i >= 0 && i < (int)v.size()) v[i] = n;
            return v;
          }
          // A bare IMPORTED submodule of an opened module (`include TransientTypeHash`
          // with `open Types`, Types.TransientTypeHash = Hashtbl.Make(..)): expand to
          // the full path and read its fields, so the include brings its members
          // (mem/add/..) into scope instead of leaving them unbound `?mem`.
          if (std::string full = opened_submodule_path(dotted); !full.empty()) {
            auto& sm = submodule_of(full);
            if (sm.ok) {
              v.resize(sm.fields.size());
              for (auto& [n, i] : sm.fields) if (i >= 0 && i < (int)v.size()) v[i] = n;
              return v;
            }
          }
          return v;
        }
        if (!module_base(dotted.substr(0, dotted.find('.')))) {
          auto& sm = submodule_of(dotted);    // a stdlib submodule's fields
          if (sm.ok) {
            v.resize(sm.fields.size());
            for (auto& [n, i] : sm.fields) if (i >= 0 && i < (int)v.size()) v[i] = n;
            return v;
          }
        }
      }
      return {};
    }
    if (auto* un = std::get_if<Pmod_unpack>(&me.desc)) {  // (val x): the package sig
      auto lay = pack_modtype_layout(expr_pack_modtype(*un->e));
      if (!lay.empty()) return lay;
    }
    const ModuleExpr* head = nullptr;
    if (auto* pa = std::get_if<Pmod_apply>(&me.desc)) head = pa->f.get();
    else if (auto* pu = std::get_if<Pmod_apply_unit>(&me.desc)) head = pu->f.get();
    // Unwrap a curried application `F(A)(B)` down to the base functor `F`, whose
    // recorded result layout is the layout after all parameters are applied.
    while (head) {
      if (auto* pa2 = std::get_if<Pmod_apply>(&head->desc)) { head = pa2->f.get(); continue; }
      if (auto* pu2 = std::get_if<Pmod_apply_unit>(&head->desc)) { head = pu2->f.get(); continue; }
      break;
    }
    if (head)
      if (auto* fi = std::get_if<Pmod_ident>(&head->desc)) {
        if (auto* l = std::get_if<Lident>(&fi->id.txt.v)) {
          auto it = functor_result_.find(l->name);
          if (it != functor_result_.end()) return it->second;
        }
        {  // a functor member of a local module (X.F): its sig's result layout
          std::string dotted;
          if (lid_to_dotted(fi->id.txt, dotted))
            if (auto it = functor_result_.find(dotted); it != functor_result_.end())
              return it->second;
        }
        if (auto* d = std::get_if<Ldot>(&fi->id.txt.v))  // a stdlib functor M.Make
          if (auto* pl = std::get_if<Lident>(&d->prefix->v))
            if (!module_base(pl->name) && !fields_of(pl->name).empty()) {
              auto fs = stdlib_functor(pl->name, d->name);
              if (fs.ok) return fs.result;
            }
      }
    {  // curried / deep-path stdlib functors (Sys.Immediate64.Make(Int)(Int64)):
       // descend one functor result per application
      const ModuleExpr* base = &me;
      int napps = 0;
      for (;;) {
        if (auto* pa2 = std::get_if<Pmod_apply>(&base->desc)) { base = pa2->f.get(); ++napps; continue; }
        if (auto* pu2 = std::get_if<Pmod_apply_unit>(&base->desc)) { base = pu2->f.get(); ++napps; continue; }
        break;
      }
      if (napps > 0)
        if (auto* fi = std::get_if<Pmod_ident>(&base->desc)) {
          std::string dotted;
          if (lid_to_dotted(fi->id.txt, dotted) && dotted.find('.') != std::string::npos) {
            auto v = stdlib_functor_result(dotted, napps);
            if (!v.empty()) return v;
          }
        }
    }
    return {};
  }

  LamPtr build_module(const Structure& s, std::vector<std::string>* names,
                      const std::vector<std::string>* coerce = nullptr,
                      const std::vector<std::string>* force_export = nullptr,
                      const cmi::Signature* coerce_sig = nullptr) {
    scope.emplace_back();
    // Re-register ambiguous constructors in source order within this module:
    // save their current entries and restore on exit, so a submodule's in-order
    // re-registration does not leak to the enclosing structure.
    std::vector<std::pair<std::string, CtorInfo>> saved_ambig;
    for (auto& n : ambiguous_ctors_)
      if (auto it = ctor_info_.find(n); it != ctor_info_.end())
        saved_ambig.emplace_back(n, it->second);
    std::vector<std::pair<std::string, FieldInfo>> saved_fields;
    for (auto& n : ambiguous_fields_)
      if (auto it = field_info_.find(n); it != field_info_.end())
        saved_fields.emplace_back(n, it->second);
    std::set<std::string> saved_scoped = scoped_unambig_fields_;
    // The variant CONSTRUCTOR-COUNT (type_ctors_) is scoped too: two sibling types
    // sharing a name (A's `t = Leaf|Node`, Expr's `t = Var|Const|Add|Binding`) must
    // each see THEIR own count at a match site, else the matcher truncates the
    // switch to the wrong arity and drops arms.  Snapshot the counts of types this
    // structure (re)declares, re-register in source order below, restore on exit.
    std::vector<std::pair<std::string, std::optional<std::pair<int,int>>>> saved_tctors;
    {
      std::set<std::string> seen;
      std::function<void(const Structure&)> snap = [&](const Structure& items) {
        for (auto& item : items)
          if (auto* td = std::get_if<Pstr_type>(&item.desc))
            for (auto& d : td->decls)
              if (std::get_if<Ptype_variant>(&d.kind) && seen.insert(d.name.txt).second) {
                auto it = type_ctors_.find(d.name.txt);
                saved_tctors.emplace_back(d.name.txt,
                  it == type_ctors_.end() ? std::nullopt
                                          : std::optional<std::pair<int,int>>(it->second));
              }
      };
      snap(s);
    }
    struct AmbigRestore {
      Translator* t; std::vector<std::pair<std::string, CtorInfo>>* s;
      std::vector<std::pair<std::string, FieldInfo>>* sf; std::set<std::string>* ss;
      std::vector<std::pair<std::string, std::optional<std::pair<int,int>>>>* st;
      ~AmbigRestore() {
        for (auto& [n, ci] : *s) t->ctor_info_[n] = ci;
        for (auto& [n, fi] : *sf) t->field_info_[n] = fi;
        t->scoped_unambig_fields_ = std::move(*ss);
        for (auto& [n, v] : *st) { if (v) t->type_ctors_[n] = *v; else t->type_ctors_.erase(n); }
      }
    } ambig_restore{this, &saved_ambig, &saved_fields, &saved_scoped, &saved_tctors};
    // updates non-empty => a recursive-data group: `(let <binds=dummies>
    // (seq <updates> body))` (caml_alloc_dummy + caml_update_dummy).
    struct Seg { bool seq; bool rec_; std::vector<Lam::Binding> binds; LamPtr e;
                 std::vector<LamPtr> updates; };
    std::vector<Seg> segs;
    std::vector<Lam::Binding> cur;
    std::vector<LamPtr> exports;
    std::vector<std::string> export_names;
    // stdlib (sub)modules `include`d here (canonical dotted/bare names): a `: S`
    // coercion eta-stubs S members that are PRIMITIVES of these (no runtime field).
    std::vector<std::string> inc_stdlib_mods;
    auto flush = [&] { if (!cur.empty()) segs.push_back({false, false, std::move(cur), nullptr}), cur.clear(); };
    auto add_export_val = [&](const std::string& nm, LamPtr v) {
      // a redefinition (shadow) moves the name to its last definition's position
      for (size_t i = 0; i < export_names.size(); ++i)
        if (export_names[i] == nm) {
          export_names.erase(export_names.begin() + i);
          exports.erase(exports.begin() + i);
          break;
        }
      exports.push_back(std::move(v));
      export_names.push_back(nm);
    };
    auto add_export = [&](const std::string& nm, const Ident& id) {
      auto v = mk(Lam::K::Var); v->var = id; add_export_val(nm, v);
    };
    int n_opens = 0;  // top-level `open M` opened for the rest of the structure
    for (auto& it : s) {
      if (auto* td = std::get_if<Pstr_type>(&it.desc)) {
        // bring this type's ambiguous constructors / record fields into scope
        // (overwriting an earlier same-named one), so subsequent code resolves
        // them to THIS type
        for (auto& d : td->decls) {
          if (auto tci = type_ctor_info_.find(d.name.txt); tci != type_ctor_info_.end())
            for (auto& [cn, ci] : tci->second)
              if (ambiguous_ctors_.count(cn)) ctor_info_[cn] = ci;
          if (auto tfi = type_field_info_.find(d.name.txt); tfi != type_field_info_.end())
            for (auto& [fn, fi] : tfi->second)
              if (ambiguous_fields_.count(fn)) { field_info_[fn] = fi; scoped_unambig_fields_.insert(fn); }
          // The lookup above is keyed by the BARE type name; when the short name
          // is taken by another module's type the record was registered under a
          // disambiguated key ("t#N"), so scope this decl's record directly from
          // its own fields (env.ml's IdTbl.t / TycompTbl.t both named `t`, sharing
          // the ambiguous `current`/`layer` labels -- without this their `{tbl
          // with current = ..}` updates can't resolve the record and emit `0`).
          scope_record_decl(d);
          // re-register THIS variant's ctor count, so a same-named sibling type's
          // count (registered first by the flat fill-absent pass) doesn't make the
          // matcher truncate this type's switch (restored on module exit).
          if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
            int nc = 0, nb = 0; bool gadt = false;
            for (auto& c : v->ctors) {
              bool block = true;
              if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) block = !t->elems.empty();
              if (c.res) gadt = true;
              if (block) ++nb; else ++nc;
            }
            if (!gadt) type_ctors_[d.name.txt] = {nc, nb};
          }
        }
        continue;
      }
      if (auto* pe = std::get_if<Pstr_eval>(&it.desc)) {  // bare `e;;` -> seq
        flush(); segs.push_back({true, false, {}, expr(*pe->e)}); continue;
      }
      if (auto* op = std::get_if<Pstr_open>(&it.desc)) {  // open M (brings members in)
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
          std::string dotted;  // a dotted submodule path opens under its full path
          if (!lid_to_dotted(mi->id.txt, dotted)) dotted = lid_last(mi->id.txt);
          // a bare `open Ops` where Ops is a submodule of an already-opened module
          // (e.g. `open M; open Ops`, M a functor param): open it as `M.Ops`.
          if (dotted.find('.') == std::string::npos && !module_base(dotted) &&
              fields_of(dotted).empty()) {
            for (auto o = opened_.rbegin(); o != opened_.rend(); ++o) {
              auto li = module_layout_.find(*o);
              if (li != module_layout_.end() && li->second.count(dotted) &&
                  module_layout_.count(*o + "." + dotted)) { dotted = *o + "." + dotted; break; }
            }
            // An IMPORTED opened module's submodule (`open Transient_expr` under
            // `open Types`, Types from its cmi -- module_layout_ has no entry for
            // it): expand via the cmi so its bare values/labels resolve through the
            // dotted-submodule path (else `repr` -> unbound `?repr` -> garbage).
            if (dotted.find('.') == std::string::npos)
              if (std::string full = opened_submodule_path(dotted); !full.empty())
                dotted = full;
          }
          if (dotted.find('.') != std::string::npos)
            submodule_of(dotted);  // eager: registers its record-type labels
          else
            register_module_records(dotted);  // bare `open M`: M's record labels
          opened_.push_back(dotted); ++n_opens;
        } else {
          // a generalized open (`open F(X)` / `open struct..end` / `open (M:S)`)
          // binds the module value like ocamlc's open/N and opens it under a
          // synthetic (un-spellable) module name
          LamPtr mv = compile_module_expr(op->expr);
          auto rl = module_result_layout(op->expr);
          if (rl.empty()) rl = arg_layout(op->expr);
          std::string nm = "open#" + std::to_string(++open_gen_count_);
          Ident oid = fresh("open");
          cur.push_back({oid, ValueKind::Gen, mv});
          module_ident_[nm] = oid;
          auto& lay = module_layout_[nm]; lay.clear();
          for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
          opened_.push_back(nm); ++n_opens;
        }
        continue;
      }
      if (auto* pmt = std::get_if<Pstr_modtype>(&it.desc)) {  // module type S = mty (no slot)
        if (pmt->type) {
          auto lay = sig_layout(*pmt->type);
          modtype_layout_[pmt->name.txt] = lay;
          modtype_ast_[pmt->name.txt] = &*pmt->type;
          // Also register under the unit-relative dotted path so a cross-submodule
          // qualified reference (`TableFormat.TABLES`, `IncrementalEngine.
          // INCREMENTAL_ENGINE`) disambiguates a name SHARED with a sibling/top-level
          // modtype -- the bare last-name picks the wrong one (different field order).
          std::string rel = mod_path_;
          size_t fd = rel.find('.');
          rel = (fd == std::string::npos) ? "" : rel.substr(fd + 1);
          if (!rel.empty()) {
            modtype_ast_[rel + "." + pmt->name.txt] = &*pmt->type;
            modtype_layout_[rel + "." + pmt->name.txt] = std::move(lay);
          }
        }
        continue;
      }
      if (auto* pp = std::get_if<Pstr_primitive>(&it.desc)) {  // external f = "cname"
        auto& pd = pp->prim;
        int ar = 0;  // arity from the declared arrow type
        if (pd.type) {
          const CoreType* t = pd.type.get();
          while (auto* a = std::get_if<Ptyp_arrow>(&t->desc)) { ++ar; t = a->cod.get(); }
        }
        if (!pd.prims.empty() && pd.prims[0][0] != '%') {  // C call
          externals_[pd.name.txt] = {pd.prims[0], ar};
        } else if (!pd.prims.empty() && pd.type) {
          local_prims_[pd.name.txt] = {pd.prims[0], ar};  // a %-builtin
        }
        continue;
      }
      if (auto* pe = std::get_if<Pstr_exception>(&it.desc)) {  // exception E [of ...]
        const std::string& nm = pe->exn.ctor.name.txt;
        // `exception F = E` rebinds: F's identity IS E's value (no fresh block)
        if (auto* rb = std::get_if<Pext_rebind>(&pe->exn.ctor.kind)) {
          if (auto* l = std::get_if<Lident>(&rb->id.txt.v))
            if (auto e2 = exn_ident_.find(l->name); e2 != exn_ident_.end()) {
              exn_ident_[nm] = e2->second;
              add_export(nm, e2->second);
              if (auto a = exn_arity_.find(l->name); a != exn_arity_.end())
                exn_arity_[nm] = a->second;
              continue;
            }
          // A QUALIFIED target (`exception Error = Parsing.Parse_error`): its value
          // is a field of the named module's global.  Resolving it is essential --
          // the .mli still exposes `exception Error` (it TAKES a field), so dropping
          // the binding shifts every following export by one (parser.mli: Error is
          // field 0 before use_file.., so the whole Parser block was misread).
          if (auto* d = std::get_if<Ldot>(&rb->id.txt.v)) {
            std::string dotted;
            if (lid_to_dotted(rb->id.txt, dotted)) {
              size_t ld = dotted.rfind('.');
              std::string modp = dotted.substr(0, ld), en = dotted.substr(ld + 1);
              LamPtr v;
              // A LOCAL module / functor parameter (`exception Error = T.Error`
              // where T : TableFormat.TABLES exposes `exception Error`): read its
              // field from the in-scope module value.
              if (auto mi = module_ident_.find(modp); mi != module_ident_.end())
                if (auto ml = module_layout_.find(modp); ml != module_layout_.end())
                  if (auto f = ml->second.find(en); f != ml->second.end())
                    v = fieldimm(f->second, varof(mi->second));
              if (!v && modp.find('.') != std::string::npos) v = submodule_value(modp, en);
              else if (!v && !module_base(modp) && !fields_of(modp).empty()) {
                auto& fm = fields_of(modp);
                if (auto f = fm.find(en); f != fm.end()) v = field_of(global_of(modp), f->second);
              }
              if (v) {
                Ident id = fresh(nm);
                cur.push_back({id, ValueKind::Gen, v});
                exn_ident_[nm] = id; add_export(nm, id);
                continue;
              }
            }
            (void)d;
          }
          // non-local target (stdlib/qualified): bind a let to its value
          if (LamPtr v = exn_value(lid_last(rb->id.txt))) {
            Ident id = fresh(nm);
            cur.push_back({id, ValueKind::Gen, v});
            exn_ident_[nm] = id;
            add_export(nm, id);
          }
          continue;
        }
        auto str = mk(Lam::K::ConstString); str->str_val = mod_path_.empty() ? nm : mod_path_ + "." + nm;
        auto oid = mk(Lam::K::Prim); oid->prim = Prim::Ccall;
        oid->prim_id = "caml_fresh_oo_id"; oid->args = {cint(0)};
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 248;
        blk->args = {str, oid};
        Ident id = fresh(nm);
        cur.push_back({id, ValueKind::Gen, blk});
        exn_ident_[nm] = id;
        if (auto* d = std::get_if<Pext_decl>(&pe->exn.ctor.kind)) {
          if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
            exn_arity_[nm] = (int)t->elems.size();
          else if (auto* r = std::get_if<Pcstr_record>(&d->args)) {
            exn_arity_[nm] = (int)r->fields.size();
            std::vector<std::string> ls;
            for (auto& f : r->fields) ls.push_back(f.name.txt);
            exn_rlabels_[nm] = std::move(ls);
          }
        }
        add_export(nm, id);
        continue;
      }
      if (auto* px = std::get_if<Pstr_typext>(&it.desc)) {  // type t += E ... (incl. effects)
        for (auto& c : px->ext.ctors) {
          const std::string& nm = c.name.txt;
          // an extension constructor is identity-matched; it shadows a same-named
          // VARIANT constructor (`type other = ..C..` then `type t += C=A`) so a
          // match in its scope compares identity, not the variant's tag.
          ctor_info_.erase(nm); builtin_ctors_.erase(nm);
          if (auto* rb = std::get_if<Pext_rebind>(&c.kind)) {  // `E = D`: alias to D
            if (auto* l = std::get_if<Lident>(&rb->id.txt.v))
              if (auto e = exn_ident_.find(l->name); e != exn_ident_.end()) {
                exn_ident_[nm] = e->second; add_export(nm, e->second);
                if (auto a = exn_arity_.find(l->name); a != exn_arity_.end())
                  exn_arity_[nm] = a->second;
              }
            continue;
          }
          auto str = mk(Lam::K::ConstString); str->str_val = mod_path_.empty() ? nm : mod_path_ + "." + nm;
          auto oid = mk(Lam::K::Prim); oid->prim = Prim::Ccall;
          oid->prim_id = "caml_fresh_oo_id"; oid->args = {cint(0)};
          auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 248;
          blk->args = {str, oid};
          Ident id = fresh(nm);
          cur.push_back({id, ValueKind::Gen, blk});
          exn_ident_[nm] = id;
          if (auto* d = std::get_if<Pext_decl>(&c.kind)) {
            if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
              exn_arity_[nm] = (int)t->elems.size();
            else if (auto* r = std::get_if<Pcstr_record>(&d->args)) {
              exn_arity_[nm] = (int)r->fields.size();
              std::vector<std::string> ls;
              for (auto& f : r->fields) ls.push_back(f.name.txt);
              exn_rlabels_[nm] = std::move(ls);
            }
          }
          add_export(nm, id);
        }
        continue;
      }
      if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {  // class c = object ... end
        flush();
        // Class declarations are recursive (even a lone one may `new` itself,
        // and `class a = .. and b = ..` bodies see every group name), so
        // pre-bind all names before translating any body; a binding whose
        // value then references a group id is backpatched through the
        // caml_alloc_dummy(3) scheme like translclass.
        std::vector<Ident> gids;
        for (auto& d : pc->decls) {
          gids.push_back(fresh(d.name.txt));
          scope.back()[d.name.txt] = gids.back();
          class_ids_.insert(gids.back().stamp);
        }
        struct ClsOut { Ident id; LamPtr v; bool dummy; };
        std::vector<ClsOut> outs;
        for (size_t di = 0; di < pc->decls.size(); ++di) {
          auto& d = pc->decls[di];
          Ident id = gids[di];
          LamPtr v;
          // Peel `class c x y = ...` parameter wrappers (incl. labelled and
          // optional) and `class c = let .. in object` local-binding wrappers.
          const ClassExpr* ce = &d.expr;
          std::vector<const Pcl_fun*> params;
          std::vector<const Pcl_let*> lets;       // class-creation lets (lifted)
          std::vector<const Pcl_let*> per_obj_lets;  // lets under a constraint (per object)
          bool saw_constraint = false;
          int opens_pushed = 0;  // `let open M in <class-expr>` wrappers
          for (;;) {
            if (auto* pf = std::get_if<Pcl_fun>(&ce->desc)) {
              params.push_back(pf);
              ce = pf->body.get();
            } else if (auto* pl = std::get_if<Pcl_let>(&ce->desc)) {
              // a let UNDER a constraint is not lifted to class-creation (the
              // "Constraints prevent lifting" rule) -- it runs per object.
              (saw_constraint ? per_obj_lets : lets).push_back(pl);
              ce = pl->body.get();
            } else if (auto* po = std::get_if<Pcl_open>(&ce->desc)) {
              std::string dotted;
              if (lid_to_dotted(po->id.txt, dotted)) { opened_.push_back(dotted); ++opens_pushed; }
              ce = po->body.get();
            } else if (auto* pcc = std::get_if<Pcl_constraint>(&ce->desc)) {
              saw_constraint = true;  // type ascription is runtime-irrelevant but blocks let-lifting
              ce = pcc->ce.get();
            } else break;
          }
          // labelled/optional class params: record the signature for `new` sites
          FnSig csig;
          bool any_lab = false;
          for (auto* pf : params) {
            int k = 0; std::string nm;
            if (auto* lb = std::get_if<Labelled>(&pf->label)) { k = 1; nm = lb->name; }
            else if (auto* op = std::get_if<Optional>(&pf->label)) { k = 2; nm = op->name; }
            if (k) any_lab = true;
            csig.push_back({k, nm});
          }
          if (any_lab) fn_sig_[id.stamp] = csig;
          bool is_virt = d.virt == VirtualFlag::Virtual;
          // Class-creation-time lets in front of a class application/alias
          // (`class c = let () = e in parent args`): emit the bindings (run when
          // the class value is built) and wrap the class value below.  Only the
          // var / `()` / `_` binding shapes (build_object's set) are handled.
          std::vector<LamPtr> let_wraps;
          bool let_wrap_ok = true;
          if (params.empty() && !lets.empty() &&
              !std::holds_alternative<Pcl_structure>(ce->desc)) {
            for (auto* lg : lets) {
              auto l = mk(Lam::K::Let);
              for (auto& b : lg->bindings) {
                const Pattern* bp = effective_pat(&b.pat);
                auto* pv = std::get_if<Ppat_var>(&bp->desc);
                bool nobind = !pv &&
                    (std::holds_alternative<Ppat_any>(bp->desc) ||
                     (std::get_if<Ppat_construct>(&bp->desc) &&
                      lid_last(std::get<Ppat_construct>(bp->desc).id.txt) == "()"));
                if (!pv && !nobind) { let_wrap_ok = false; break; }
                LamPtr rv = expr(*b.expr);
                Ident bid = pv ? fresh(pv->name.txt) : fresh("", true);
                if (pv) scope.back()[pv->name.txt] = bid;
                l->bindings.push_back({bid, pv ? pat_kind(&b.pat) : ValueKind::Gen, rv});
              }
              if (!let_wrap_ok) break;
              let_wraps.push_back(l);
            }
          }
          if (params.empty() && (lets.empty() || (!let_wraps.empty() && let_wrap_ok))) {
            // `class a = b`: a pure alias -- no binding, the export IS b.
            if (lets.empty()) if (auto* pcn = std::get_if<Pcl_constr>(&ce->desc)) {
              if (auto* pl = std::get_if<Lident>(&pcn->id.txt.v))
                if (const Ident* pid = lookup(pl->name); pid && class_ids_.count(pid->stamp)) {
                  scope.back()[d.name.txt] = *pid;
                  if (auto mit = class_meta_.find(pl->name); mit != class_meta_.end())
                    class_meta_[d.name.txt] = mit->second;
                  if (auto sit = fn_sig_.find(pid->stamp); sit != fn_sig_.end())
                    fn_sig_[id.stamp] = sit->second;
                  add_export(d.name.txt, *pid);
                  continue;
                }
            }
            // `class c = parent args`: rebind the parent's obj_init/env_init
            // through `new_init = (function obj_init self (apply obj_init self
            // args))` in a fresh 3-tuple (translclass's class application).
            if (auto* apc = std::get_if<Pcl_apply>(&ce->desc)) {
              // `(let () = e in parent) args`: a let INSIDE the application is
              // object-creation-time (the test "Nested bindings are not toplevel")
              // -- emit it in the per-object new_init wrap body, not at class build.
              const ClassExpr* ace = apc->ce.get();
              std::vector<const Pcl_let*> inner_lets;
              while (auto* il = std::get_if<Pcl_let>(&ace->desc)) {
                inner_lets.push_back(il); ace = il->body.get();
              }
              if (auto* pcn = std::get_if<Pcl_constr>(&ace->desc))
                if (auto* pl = std::get_if<Lident>(&pcn->id.txt.v))
                  if (const Ident* pid = lookup(pl->name); pid && class_ids_.count(pid->stamp)) {
                    bool simple = true;
                    for (auto& [l, e] : apc->args)
                      if (!std::holds_alternative<Nolabel>(l)) simple = false;
                    // build the inner lets' bindings (binding vars in scope so the
                    // args can see them), to wrap the obj_init application below
                    std::vector<LamPtr> inner_wraps;
                    for (auto* lg : inner_lets) {
                      auto l = mk(Lam::K::Let);
                      for (auto& b : lg->bindings) {
                        const Pattern* bp = effective_pat(&b.pat);
                        auto* pv = std::get_if<Ppat_var>(&bp->desc);
                        bool nobind = !pv &&
                            (std::holds_alternative<Ppat_any>(bp->desc) ||
                             (std::get_if<Ppat_construct>(&bp->desc) &&
                              lid_last(std::get<Ppat_construct>(bp->desc).id.txt) == "()"));
                        if (!pv && !nobind) { simple = false; break; }
                        LamPtr rv = expr(*b.expr);
                        Ident bid = pv ? fresh(pv->name.txt) : fresh("", true);
                        if (pv) scope.back()[pv->name.txt] = bid;
                        l->bindings.push_back({bid, pv ? pat_kind(&b.pat) : ValueKind::Gen, rv});
                      }
                      inner_wraps.push_back(l);
                    }
                    if (simple) {
                      Ident ninit = fresh("new_init");
                      Ident oi = fresh("obj_init"), slf = fresh("self");
                      auto wrap = mk(Lam::K::Function);
                      wrap->params = {{oi, ValueKind::Gen}, {slf, ValueKind::Gen}};
                      auto apl = mk(Lam::K::Apply);
                      apl->fn = varof(oi); apl->args = {varof(slf)};
                      for (auto& [l, e] : apc->args) apl->args.push_back(expr(*e));
                      LamPtr wbody = apl;
                      for (auto wit = inner_wraps.rbegin(); wit != inner_wraps.rend(); ++wit) {
                        (*wit)->body = wbody; wbody = *wit;
                      }
                      wrap->body = wbody;
                      auto fm0 = mk(Lam::K::Prim); fm0->prim = Prim::FieldMut;
                      fm0->prim_arg = 0; fm0->args = {varof(*pid)};
                      auto f0 = mk(Lam::K::Apply); f0->fn = varof(ninit); f0->args = {fm0};
                      Ident tbl = fresh("table"), einit = fresh("env_init"), envs = fresh("envs");
                      auto fm1 = mk(Lam::K::Prim); fm1->prim = Prim::FieldMut;
                      fm1->prim_arg = 1; fm1->args = {varof(*pid)};
                      auto e_ap = mk(Lam::K::Apply); e_ap->fn = fm1; e_ap->args = {varof(tbl)};
                      auto inner_ap = mk(Lam::K::Apply);
                      inner_ap->fn = varof(einit); inner_ap->args = {varof(envs)};
                      auto nf = mk(Lam::K::Apply); nf->fn = varof(ninit); nf->args = {inner_ap};
                      auto envs_fn = mk(Lam::K::Function);
                      envs_fn->params = {{envs, ValueKind::Gen}}; envs_fn->body = nf;
                      auto elet = mk(Lam::K::Let);
                      elet->bindings = {{einit, ValueKind::Gen, e_ap}}; elet->body = envs_fn;
                      auto tbl_fn = mk(Lam::K::Function);
                      tbl_fn->params = {{tbl, ValueKind::Gen}}; tbl_fn->body = elet;
                      auto fm2 = mk(Lam::K::Prim); fm2->prim = Prim::FieldMut;
                      fm2->prim_arg = 2; fm2->args = {varof(*pid)};
                      auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 0;
                      blk->args = {f0, tbl_fn, fm2};
                      auto outer = mk(Lam::K::Let);
                      outer->bindings = {{ninit, ValueKind::Gen, wrap}}; outer->body = blk;
                      v = outer;
                      if (auto mit = class_meta_.find(pl->name); mit != class_meta_.end())
                        class_meta_[d.name.txt] = mit->second;
                    }
                  }
            }  // end `if (auto* apc = ...)`
            // wrap the class value with the class-creation lets (run once, when
            // the class is built), innermost let last
            if (v && !let_wraps.empty())
              for (auto wit = let_wraps.rbegin(); wit != let_wraps.rend(); ++wit) {
                (*wit)->body = v; v = *wit;
              }
          }
          if (auto* ps = std::get_if<Pcl_structure>(&ce->desc)) {
            v = build_object(ps->cs, /*as_class=*/true, d.name.txt,
                             params.empty() ? nullptr : &params,
                             lets.empty() ? nullptr : &lets, is_virt,
                             per_obj_lets.empty() ? nullptr : &per_obj_lets);
            register_class_meta(d.name.txt, ps->cs);
          }
          for (; opens_pushed > 0; --opens_pushed) opened_.pop_back();
          add_export(d.name.txt, id);
          if (!v) v = mk(Lam::K::ConstInt);  // unsupported class shape: placeholder
          outs.push_back({id, v, is_virt});  // virtual classes are always dummies
        }
        std::set<int> gset;
        for (auto& g : gids) gset.insert(g.stamp);
        bool any_dummy = false;
        for (auto& o : outs) {
          if (!o.dummy) {
            std::set<int> bound; std::map<int, Ident> fv;
            free_vars(o.v, bound, fv);
            for (int s : gset) if (fv.count(s)) { o.dummy = true; break; }
          }
          any_dummy = any_dummy || o.dummy;
        }
        if (!any_dummy) {
          for (auto& o : outs) cur.push_back({o.id, ValueKind::Gen, o.v});
        } else {  // `(let (<dummies> <non-recursive binds>) (seq <updates> ..))`
          flush();
          Seg s; s.seq = false; s.rec_ = false;
          for (auto& o : outs)
            if (o.dummy) s.binds.push_back({o.id, ValueKind::Gen, alloc_dummy(3)});
          for (auto& o : outs)
            if (!o.dummy) s.binds.push_back({o.id, ValueKind::Gen, o.v});
          for (auto& o : outs)
            if (o.dummy) s.updates.push_back(update_dummy(o.id, o.v));
          segs.push_back(std::move(s));
        }
        continue;
      }
      if (auto* pm = std::get_if<Pstr_module>(&it.desc)) {  // module M = struct ... end
        auto& mb = pm->binding;
        // `module M : S = struct .. end` coerces the structure's exports to S.
        const ModuleExpr* me = &mb.expr;
        const std::vector<std::string>* coerce = nullptr;
        std::vector<std::string> coerce_store;
        if (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) {
          coerce_store = sig_layout(*pc->mt); coerce = &coerce_store; me = pc->me.get();
        }
        // No explicit `: S`, but THIS unit's own .mli declares the submodule's
        // field order: coerce the submodule body to it so members the impl gives
        // as EXTERNALS (Stdlib.LargeFile's seek_in etc. -- all externals, hence a
        // 0-field raw block) are materialised as eta-stubs.  Without this the
        // submodule block is empty and `M.Sub.f` reads out of bounds -> a garbage
        // pointer that crashes the GC under load.
        if (!coerce && mb.name.txt)
          if (auto sf = mli_submodule_fields_.find(*mb.name.txt);
              sf != mli_submodule_fields_.end())
            coerce = &sf->second;
        // The implicit .mli-driven coercion above narrows the submodule's INTERNAL
        // block to its exposed signature.  When the .mli HIDES a member that is used
        // intra-unit (parser.mli exposes MenhirInterpreter as the incremental API
        // only, but parser.ml calls MenhirInterpreter.entry internally), narrowing
        // here makes that internal `M.entry` an unresolved stub.  Build the submodule
        // FULL instead and let the unit-level export coercion re-narrow it to the
        // .mli for the .cmo's exported block.  EXCEPTION: a struct whose exposed
        // members are `external` decls (Stdlib.LargeFile) must keep the binding
        // coercion -- it materialises those externals as eta-stubs, which the export
        // coercion's coerce_block cannot do (it bails on a member absent from src).
        // An EXPLICIT `(struct : S)` ascription is left untouched (it narrows for real).
        if (coerce && !std::get_if<Pmod_constraint>(&mb.expr.desc))
          if (auto* ps0 = std::get_if<Pmod_structure>(&me->desc)) {
            bool has_external = false;
            for (auto& sit : ps0->items)
              if (auto* pp = std::get_if<Pstr_primitive>(&sit.desc))
                if (std::find(coerce->begin(), coerce->end(), pp->prim.name.txt) != coerce->end()) {
                  has_external = true; break;
                }
            if (!has_external) coerce = nullptr;
          }
        if (mb.name.txt)
          if (auto* ps = std::get_if<Pmod_structure>(&me->desc)) {
            std::vector<std::string> sub;
            std::string saved = mod_path_;
            mod_path_ += "." + *mb.name.txt;  // nested exceptions are "Outer.M.E"
            auto exn_before = exn_ident_;
            auto mod_before = module_ident_;
            auto alias_before = module_alias_;
            // A submodule's external must not SHADOW a same-named parent one
            // unqualified: array.ml's `module Floatarray = struct external
            // unsafe_get : floatarray.. = "%floatarray_unsafe_get" end` would
            // overwrite the top-level `unsafe_get = "%array_unsafe_get"`, so later
            // top-level `unsafe_get` lowered to a FLOAT array get (Array.iter/map
            // garbage).  Restore only the SHADOWED parent entries; KEEP the
            // submodule's NEW ones -- `M.member` resolution falls back to them by
            // name (domain.ml's `Raw.get_domain_count`).
            auto lprims_before = local_prims_;
            auto exts_before = externals_;
            LamPtr body = build_module(ps->items, &sub, coerce);
            for (auto& [k, v] : lprims_before) local_prims_[k] = v;
            for (auto& [k, v] : exts_before) externals_[k] = v;
            mod_path_ = saved;
            Ident mid = fresh(*mb.name.txt);
            cur.push_back({mid, ValueKind::Gen, body});
            auto& lay = module_layout_[*mb.name.txt]; lay.clear();
            for (int i = 0; i < (int)sub.size(); ++i) lay[sub[i]] = i;
            // Binders declared INSIDE the submodule are out of scope out here:
            // exported exception/extension ctors re-register as field reads,
            // exported inner modules as field-path aliases; the rest restore
            // their shadowed outer entries (or vanish).
            for (auto& [nm, eid] : exn_ident_) {
              auto bi = exn_before.find(nm);
              if (bi != exn_before.end() && bi->second.stamp == eid.stamp) continue;
              if (auto f = lay.find(nm); f != lay.end())
                exn_field_[nm] = {mid, f->second};
            }
            for (auto it2 = exn_ident_.begin(); it2 != exn_ident_.end();) {
              auto bi = exn_before.find(it2->first);
              bool inner = bi == exn_before.end() || bi->second.stamp != it2->second.stamp;
              if (!inner) { ++it2; continue; }
              if (bi != exn_before.end()) { it2->second = bi->second; ++it2; }
              else it2 = exn_ident_.erase(it2);
            }
            std::vector<std::string> inner_mods;
            for (auto& [nm, iid] : module_ident_) {
              auto bi = mod_before.find(nm);
              if (bi != mod_before.end() && bi->second.stamp == iid.stamp) continue;
              if (auto f = lay.find(nm); f != lay.end()) {
                module_alias_[nm] = fieldimm(f->second, varof(mid));
                inner_mods.push_back(nm);
              }
            }
            for (auto it2 = module_ident_.begin(); it2 != module_ident_.end();) {
              auto bi = mod_before.find(it2->first);
              bool inner = bi == mod_before.end() || bi->second.stamp != it2->second.stamp;
              if (!inner) { ++it2; continue; }
              if (bi != mod_before.end()) { it2->second = bi->second; ++it2; }
              else it2 = module_ident_.erase(it2);
            }
            for (auto it2 = module_alias_.begin(); it2 != module_alias_.end();) {
              auto bi = alias_before.find(it2->first);
              bool inner = bi == alias_before.end() || bi->second != it2->second;
              if (!inner) { ++it2; continue; }
              if (module_alias_[it2->first] &&
                  lay.count(it2->first) &&
                  bi == alias_before.end()) { ++it2; continue; }  // just re-pointed above
              if (bi != alias_before.end()) { it2->second = bi->second; ++it2; }
              else it2 = module_alias_.erase(it2);
            }
            // exported inner modules' layouts also register under the dotted
            // path ("X.M"), so deep member access / alias chains through X
            // resolve (NOTE: after the loops above -- inserting invalidates lay)
            for (auto& nm : inner_mods)
              copy_layout_subtree(nm, *mb.name.txt + "." + nm);
            module_ident_[*mb.name.txt] = mid;
            add_export(*mb.name.txt, mid);
          } else if (std::holds_alternative<Pmod_functor>(mb.expr.desc)) {
            Ident mid = fresh(*mb.name.txt);            // a functor binds as a function
            std::string saved_mp = mod_path_;  // __FUNCTION__ inside is `..Functor.fn`
            mod_path_ += (mod_path_.empty() ? "" : ".") + *mb.name.txt;
            // .mli-driven functor-result coercion: lay this functor's body struct
            // out per its declared result signature (the .ml has no `: S`).  Keys in
            // mli_functor_results_ are unit-relative ("Engine.Make"); saved_mp here is
            // the unit-qualified enclosing path ("CamlinternalMenhirLib.Engine"), so
            // try its progressively-stripped suffixes (dropping the unit name) down to
            // the bare functor name.
            std::vector<std::string> mli_result_layout;
            {
              std::string full = saved_mp.empty() ? *mb.name.txt
                                                  : saved_mp + "." + *mb.name.txt;
              for (std::string cand = full; ; ) {
                if (auto fr = mli_functor_results_.find(cand); fr != mli_functor_results_.end()) {
                  pending_functor_coerce_ = fr->second; mli_result_layout = fr->second; break;
                }
                size_t d = cand.find('.');
                if (d == std::string::npos) break;
                cand = cand.substr(d + 1);
              }
            }
            LamPtr fv = compile_module_expr(mb.expr);
            pending_functor_coerce_.clear();
            mod_path_ = saved_mp;
            cur.push_back({mid, ValueKind::Gen, fv});
            module_ident_[*mb.name.txt] = mid;
            // When the .mli RESTRICTS this functor's result (the body has more
            // members than the declared result sig, e.g. parmatch.ml's `Compat`
            // exposes only [compat;compats] of its 4-member body), the result
            // block is coerced to the .mli layout -- so an APPLICATION's member
            // (`SyntacticCompat.compats`) must index into THAT layout, not the
            // raw body's.  Use the .mli result layout for functor_result_.
            functor_result_[*mb.name.txt] =
                mli_result_layout.empty() ? module_result_layout(mb.expr)  // for Make(..)
                                          : mli_result_layout;
            functor_param_[*mb.name.txt] = functor_param_layout(mb.expr);   // for arg coercion
            if (auto* pt = functor_param_type(mb.expr)) functor_param_sig_[*mb.name.txt] = pt;
            functor_param_types_[*mb.name.txt] = functor_param_types(mb.expr);  // all params
            record_local_functor_result_sigs(*mb.name.txt, &mb.expr);  // result value optionals
            add_export(*mb.name.txt, mid);
          } else {  // module M = F(X) / M2 / (M : S) / (val x): bind + layout
            LamPtr mv = compile_module_expr(mb.expr);
            const std::string& nm = *mb.name.txt;
            {
              // Compute the layout BEFORE clearing -- a self-alias `module T = T`
              // (Identifiable.Make rebinds its parameter) would otherwise wipe the
              // param's layout and re-read it empty, so a following `include T`
              // splices nothing and the functor result is short.
              auto rl = module_result_layout(mb.expr);
              auto& lay = module_layout_[nm]; lay.clear();
              for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
            }
            // A cross-module functor result's NESTED submodule layouts (M.Set/
            // M.Map for `module M = Identifiable.Make(..)`), so `include M;
            // Set.empty` resolves Set as the functor-result submodule.
            register_functor_result_layouts(nm, mb.expr);
            // `module Subst = Map.Make(..)`: record the stdlib functor source so
            // Subst.fold's labelled result signature is recoverable for reordering.
            // `Map` under `open MoreLabels` is the submodule MoreLabels.Map (its
            // result is the LABELLED Map), so resolve the prefix through opens.
            // `module Constructor = NameChoice(..)` for a LOCAL functor: copy the
            // functor's recorded result value label-sigs so Constructor.f fills
            // f's omitted optionals.
            if (auto* pa = std::get_if<Pmod_apply>(&mb.expr.desc))
              if (auto* fi = std::get_if<Pmod_ident>(&pa->f->desc))
                if (auto* l = std::get_if<Lident>(&fi->id.txt.v))
                  if (auto it = local_functor_member_sig_.find(l->name);
                      it != local_functor_member_sig_.end())
                    local_member_sig_[nm] = it->second;
            if (auto* pa = std::get_if<Pmod_apply>(&mb.expr.desc))
              if (auto* fi = std::get_if<Pmod_ident>(&pa->f->desc))
                if (auto* d = std::get_if<Ldot>(&fi->id.txt.v))
                  if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
                    std::string src = pl->name;
                    for (auto oit = opened_.rbegin(); oit != opened_.rend(); ++oit)
                      if (!module_base(*oit) && submodule_of(*oit + "." + pl->name).ok) {
                        src = *oit + "." + pl->name; break;
                      }
                    if (src.find('.') != std::string::npos ||
                        (!module_base(pl->name) && !fields_of(pl->name).empty()))
                      module_functor_src_[nm] = {src, d->name};
                  }
            // an alias to a stdlib submodule (`module MP = Gc.Memprof`): register
            // the dotted path so MP.x gets the submodule's fields, externals,
            // labelled signatures, and record types
            if (auto* pi2 = std::get_if<Pmod_ident>(&mb.expr.desc)) {
              std::string dotted;
              if (lid_to_dotted(pi2->id.txt, dotted)) {
                dotted = expand_alias_head(dotted);  // `module NameMap = S.Map`
                if (dotted.find('.') != std::string::npos &&
                    !module_base(dotted.substr(0, dotted.find('.')))) {
                  auto& sm = submodule_of(dotted);
                  if (sm.ok) {
                    submod_alias_[nm] = dotted;
                    auto& lay = module_layout_[nm];
                    for (auto& [n2, i2] : sm.fields) lay[n2] = i2;
                  }
                } else if (dotted.find('.') == std::string::npos &&
                           !module_base(dotted) && !fields_of(dotted).empty()) {
                  // `module B = Bytes`: an alias to a top-level stdlib module.
                  // Record it so B.x resolves like Bytes.x -- including externals
                  // (B.create = "caml_create_bytes") that take no runtime field and
                  // so aren't covered by the layout copy below (string.ml/bytes.ml).
                  submod_alias_[nm] = dotted;
                  copy_layout_subtree(dotted, nm);
                } else {
                  // a local-path alias adopts the source's layout subtree
                  // (module D = B / module Y = X.M), nested keys included
                  copy_layout_subtree(dotted, nm);
                }
              }
            }
            const ModuleExpr* mex = &mb.expr;  // (val x): layouts from x's package type
            while (auto* pc2 = std::get_if<Pmod_constraint>(&mex->desc)) mex = pc2->me.get();
            bool unpack = false;
            if (auto* un = std::get_if<Pmod_unpack>(&mex->desc)) {
              unpack = true;
              register_pack_layouts(nm, expr_pack_modtype(*un->e));
            }
            if (is_pure_path(mv)) {  // a module alias `M = N.Sub`: inline the path
              module_alias_[nm] = mv;
              module_ident_.erase(nm);
              // A plain `module M = path` is a type-level alias with no runtime
              // slot (elided from the export); a constrained `module M : S = path`
              // and an unpack `module M = (val x)` materialize a field.  An enclosing
              // result-sig coercion that EXPOSES this alias (force_export) also
              // materialises it (functor result `: H` with `module Elem = E`).
              bool forced = force_export &&
                  std::find(force_export->begin(), force_export->end(), nm) != force_export->end();
              if (std::holds_alternative<Pmod_constraint>(mb.expr.desc) || unpack || forced)
                add_export_val(nm, mv);
            } else {
              Ident mid = fresh(nm);
              cur.push_back({mid, ValueKind::Gen, mv});
              module_ident_[nm] = mid;
              add_export(nm, mid);
            }
          }
        continue;
      }
      if (auto* prm = std::get_if<Pstr_recmodule>(&it.desc)) {
        // module rec M : S = struct .. end -- CamlinternalMod's init/update
        // scheme: each module binds a dummy built from its signature SHAPE,
        // the structs evaluate with the dummies in scope (recursive refs read
        // the dummies' pre-allocated closures, patched in place), then
        // update_mod copies each real structure over its dummy.
        // Per Translmod.eval_rec_bindings: init_shape classifies each binding.
        // A binding whose signature yields a shape (all functions/lazy/modules)
        // is "dummy-able" -> init_mod a dummy, then update_mod the real struct.
        // A binding whose init_shape FAILS (a functor application, a non-function
        // value, an abstract sig) is "unsafe" -> bound directly to its compiled
        // RHS, with no dummy/update (it reads the others' dummies in place).
        struct RM { const ModuleBinding* mb; const ModuleType* sig;
                    const Pmod_structure* body; const ModuleExpr* bodyme;
                    Ident id; LamPtr shape; bool dummyable; };
        std::vector<RM> rms;
        bool ok = !prm->bindings.empty();
        for (auto& mb : prm->bindings) {
          if (!mb.name.txt) { ok = false; break; }
          const ModuleExpr* me = &mb.expr;
          const ModuleType* sig = nullptr;
          while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) {
            sig = pc->mt.get();
            me = pc->me.get();
          }
          auto* ps = sig ? std::get_if<Pmod_structure>(&me->desc) : nullptr;
          LamPtr shape = sig ? recmod_shape(*sig) : nullptr;
          rms.push_back({&mb, sig, ps, me, {}, shape, shape != nullptr});
        }
        auto& cim = fields_of("CamlinternalMod");
        auto initI = cim.find("init_mod");
        auto updI = cim.find("update_mod");
        bool need_mod = false;
        for (auto& rm : rms) if (rm.dummyable) need_mod = true;
        if (ok && (!need_mod || (initI != cim.end() && updI != cim.end()))) {
          // (phase 0) register every name first, so mutual refs resolve in every
          // body; a dummy-able member also registers its signature layout.
          for (auto& rm : rms) {
            rm.id = fresh(*rm.mb->name.txt);
            module_ident_[*rm.mb->name.txt] = rm.id;
            // register the signature layout (for ALL members, not just dummy-able)
            // so a sibling body's `M.member` resolves from M's sig -- e.g. an
            // unsafe value module `Before` whose `Before.x` another body reads.  A
            // functor-application member's real result layout overwrites this in
            // phase 2.
            if (rm.sig) register_sig_layouts(*rm.mb->name.txt, *rm.sig);
          }
          // (phase 1) init_mod dummies for the dummy-able members
          for (auto& rm : rms) {
            if (!rm.dummyable) continue;
            auto ap = mk(Lam::K::Apply);
            ap->fn = field_of("CamlinternalMod", initI->second);
            ap->args = {loc_block(rm.bodyme->loc), rm.shape};
            cur.push_back({rm.id, ValueKind::Gen, ap});
          }
          // (phase 2) bind the unsafe members directly to their RHS (a functor
          // application etc.), reading the dummies that are already in scope.
          // Register the result layout (like `module M = F(X)`) so a sibling
          // body's `M.member` resolves instead of dumping `?member`.  Compile all
          // first, then emit in DEPENDENCY order (reorder_rec_bindings): an unsafe
          // member that reads another unsafe member's REAL value must follow it
          // (a non-function value module like `After = struct let x = Before.x+1`).
          std::vector<RM*> unsafe;
          std::unordered_map<int, LamPtr> uval;  // rm.id.stamp -> compiled body
          for (auto& rm : rms) {
            if (rm.dummyable) continue;
            const std::string& nm = *rm.mb->name.txt;
            std::string saved_mp = mod_path_;  // __FUNCTION__ inside is `..Rec2.fn`
            mod_path_ += (mod_path_.empty() ? "" : ".") + nm;
            LamPtr mv = compile_module_expr(*rm.bodyme);
            mod_path_ = saved_mp;
            if (std::holds_alternative<Pmod_functor>(rm.bodyme->desc)) {
              // a FUNCTOR member (`Rec2 : functor(X)->.. = functor(X)->..`): a
              // sibling's `Rec2(arg).m` resolves via functor_result_, not a layout.
              functor_result_[nm] = module_result_layout(*rm.bodyme);
              functor_param_[nm] = functor_param_layout(*rm.bodyme);
              if (auto* pt = functor_param_type(*rm.bodyme)) functor_param_sig_[nm] = pt;
            } else {
              // Use the functor-RESULT layout only when it's known (Set.Make(..)).
              // For an unknown functor (a functor PARAMETER applied, `MakeH(BE)`)
              // it's empty -- keep the SIGNATURE layout registered in phase 0
              // (the member has a `: S` ascription), so M.member still resolves.
              auto rl = module_result_layout(*rm.bodyme);
              if (!rl.empty()) {
                auto& lay = module_layout_[nm]; lay.clear();
                for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
              }
            }
            uval[rm.id.stamp] = mv; unsafe.push_back(&rm);
          }
          std::vector<RM*> order;        // topological (deps first)
          std::set<int> emitted;
          std::function<void(RM*)> emit = [&](RM* rm) {
            if (emitted.count(rm->id.stamp)) return;
            emitted.insert(rm->id.stamp);  // mark before recursing (cycles -> dummy reads)
            for (RM* o : unsafe)
              if (o != rm && count_var(uval[rm->id.stamp], o->id) > 0) emit(o);
            order.push_back(rm);
          };
          for (RM* rm : unsafe) emit(rm);
          for (RM* rm : order) {
            cur.push_back({rm->id, ValueKind::Gen, uval[rm->id.stamp]});
            add_export(*rm->mb->name.txt, rm->id);
          }
          // (phase 3) update_mod each dummy with the real structure
          for (auto& rm : rms) {
            if (!rm.dummyable) continue;
            LamPtr body;
            if (rm.body) {
              std::vector<std::string> co = sig_layout(*rm.sig);
              std::vector<std::string> sub;
              std::string saved = mod_path_;
              mod_path_ += "." + *rm.mb->name.txt;
              body = build_module(rm.body->items, &sub, &co);
              mod_path_ = saved;
            } else {  // `module rec Id : S = Id` / `= F(X)` and other non-struct bodies
              body = compile_module_expr(*rm.bodyme);
              // coerce a functor-application body's result to the binding's sig
              // (`Coerce6 : sig val at end = Coerce4(Coerce5)` drops Coerce4's `x`)
              if (LamPtr c = coerce_block(body, module_result_layout(*rm.bodyme),
                                          sig_layout(*rm.sig), rm.bodyme,
                                          sig_items_of(*rm.sig)))
                body = c;
            }
            auto up = mk(Lam::K::Apply);
            up->fn = field_of("CamlinternalMod", updI->second);
            up->args = {rm.shape, varof(rm.id), body};
            cur.push_back({fresh("", true), ValueKind::Gen, up});
            add_export(*rm.mb->name.txt, rm.id);
          }
        }
        continue;
      }
      if (auto* pin = std::get_if<Pstr_include>(&it.desc)) {  // include ME
        // Splice ME's exported value fields into this structure.  A pure path
        // (an already-evaluated module) needs no binding; a computation (e.g. a
        // functor application) is bound to `include/N` first for its effect.
        // an anonymous included struct's exceptions have a bare path
        // (Printexc prints "XXX", not "Unit.XXX")
        std::string sv_path = mod_path_;
        if (std::get_if<Pmod_ident>(&pin->expr.desc) == nullptr) mod_path_.clear();
        LamPtr mv = compile_module_expr(pin->expr);
        mod_path_ = sv_path;
        const Pmty_signature* tsig = nullptr;  // a constrained include's sig items
        if (auto* pcst = std::get_if<Pmod_constraint>(&pin->expr.desc))
          tsig = sig_items_of(*pcst->mt);
        LamPtr base = mv;
        bool bound = !is_pure_path(mv);
        if (bound) {
          Ident iid = fresh("include");
          cur.push_back({iid, ValueKind::Gen, mv});
          auto v = mk(Lam::K::Var); v->var = iid; base = v;
        }
        // `include M` where M is a LOCAL module: its fields shadow enclosing
        // bindings (e.g. a function parameter), so they must enter scope as
        // binders -- an open alone resolves them only AFTER local scope.
        bool local_include = false;
        if (auto* mip = std::get_if<Pmod_ident>(&pin->expr.desc))
          if (auto* l = std::get_if<Lident>(&mip->id.txt.v)) local_include = module_base(l->name) != nullptr;
        // remember an `include <stdlib (sub)module>` so a `: S` coercion can
        // eta-stub S members that are PRIMITIVES of it (they have no field here).
        if (auto* mip = std::get_if<Pmod_ident>(&pin->expr.desc)) {
          std::string idot;
          if (lid_to_dotted(mip->id.txt, idot)) {
            std::string c = canon_stdlib_path(idot);
            std::string h = c.substr(0, c.find('.'));
            if (!module_base(h) && !fields_of(h).empty() &&
                (c.find('.') == std::string::npos || submodule_of(c).ok))
              inc_stdlib_mods.push_back(c);
          }
        }
        auto rl = module_result_layout(pin->expr);
        if (rl.empty()) rl = arg_layout(pin->expr);  // a module path: its own fields
        for (int i = 0; i < (int)rl.size(); ++i) {
          if (bound) {
            // a computed include (e.g. a coerced one) has no module name for
            // bare resolution: rebind each field like ocamlc (`f =a field_mut`)
            Ident id = fresh(rl[i]);
            auto fr = mk(Lam::K::Prim); fr->prim = Prim::FieldMut;
            fr->prim_arg = i; fr->args = {base};
            cur.push_back({id, ValueKind::Gen, fr, true});
            scope.back()[rl[i]] = id;
            add_export(rl[i], id);
            if (tsig) {  // module / exception members register as such
              if (const ModuleType* smt = sig_member_modtype(*tsig, rl[i])) {
                module_ident_[rl[i]] = id;
                module_alias_.erase(rl[i]);
                register_sig_layouts(rl[i], *smt);
              } else {
                for (auto& sit : tsig->items)
                  if (auto* ex = std::get_if<Psig_exception>(&sit.desc);
                      ex && ex->exn.ctor.name.txt == rl[i])
                    exn_ident_[rl[i]] = id;
              }
            }
          } else {
            // a coercion/include projection reads module fields with Mutable
            // semantics (translmod's get_field: `Pfield(pos, Pointer, Mutable)`)
            auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldMut;
            fi->prim_arg = i; fi->args = {base};
            if (local_include && lookup(rl[i])) {  // shadow an enclosing binding only
              Ident id = fresh(rl[i]);
              cur.push_back({id, ValueKind::Gen, fi, true});  // =a alias
              scope.back()[rl[i]] = id;
              add_export(rl[i], id);
            } else {
              add_export_val(rl[i], fi);
            }
          }
        }
        // an included module path also brings its names into BARE scope for the
        // rest of the structure (like open): `include Stack ... iter f s`
        if (auto* mi = std::get_if<Pmod_ident>(&pin->expr.desc)) {
          std::string dotted;
          if (!lid_to_dotted(mi->id.txt, dotted)) dotted = lid_last(mi->id.txt);
          // `Stdlib.Array` is the bare stdlib submodule `Array` (Stdlib__Array);
          // open it under its bare name so its members resolve unqualified.
          if (dotted.rfind("Stdlib.", 0) == 0 &&
              dotted.find('.', 7) == std::string::npos)
            dotted = dotted.substr(7);
          // A bare IMPORTED submodule (`include TransientTypeHash` under `open
          // Types`): expand to its full path so its members (mem/add/..) resolve
          // unqualified through the dotted-submodule path, not as unbound `?mem`.
          if (dotted.find('.') == std::string::npos && !module_base(dotted) &&
              fields_of(dotted).empty())
            if (std::string full = opened_submodule_path(dotted); !full.empty())
              dotted = full;
          opened_.push_back(dotted);
          ++n_opens;
        }
        continue;
      }
      auto* sv = std::get_if<Pstr_value>(&it.desc);
      if (!sv) continue;
      if (sv->rf == RecFlag::Recursive) {  // let rec: names in scope within RHSs
        flush();
        std::vector<std::pair<const ValueBinding*, Ident>> recs;
        for (auto& b : sv->bindings)
          if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
            Ident id = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = id;
            record_fn_sig(id, b.expr.get());
            recs.push_back({&b, id});
          }
        std::vector<Ident> ids; std::vector<ValueKind> kinds; std::vector<LamPtr> vals;
        for (auto& [b, id] : recs) {
          ids.push_back(id);
          kinds.push_back(pat_kind(&b->pat));
          rec_spine_ = true;
          vals.push_back(fn_binding_rhs(std::get_if<Ppat_var>(&b->pat.desc)->name.txt,
                                        *b->expr, b->attrs));
          add_export(std::get_if<Ppat_var>(&b->pat.desc)->name.txt, id);
        }
        RecParts rp;
        if (!recs.empty() && partition_rec(ids, kinds, vals, rp)) {
          // value recursion: dummies first, functions next, updates innermost
          // (attached to the innermost of the two segments).
          if (rp.funcs.empty()) {
            Seg s; s.seq = false; s.rec_ = false;
            s.binds = std::move(rp.dummies); s.updates = std::move(rp.updates);
            segs.push_back(std::move(s));
          } else {
            Seg d; d.seq = false; d.rec_ = false; d.binds = std::move(rp.dummies);
            segs.push_back(std::move(d));
            Seg f; f.seq = false; f.rec_ = true;
            f.binds = std::move(rp.funcs); f.updates = std::move(rp.updates);
            segs.push_back(std::move(f));
          }
        } else {
          std::vector<Lam::Binding> binds;
          for (size_t i = 0; i < recs.size(); ++i)
            binds.push_back({ids[i], kinds[i], collapse_let_id(vals[i])});
          segs.push_back({false, true, std::move(binds), nullptr, {}});
        }
        continue;
      }
      for (auto& b : sv->bindings) {
        {  // `let x = (module .. : S)` / `let x : (module S) = ..`: record x's
           // package type so a later `(val x)` knows its layout
          const Pattern* p0 = &b.pat;
          const CoreType* ct0 = nullptr;
          while (auto* pc0 = std::get_if<Ppat_constraint>(&p0->desc)) {
            ct0 = pc0->t.get(); p0 = pc0->p.get();
          }
          if (auto* pv0 = std::get_if<Ppat_var>(&p0->desc)) {
            std::string mt;
            if (ct0)
              if (auto* pk0 = std::get_if<Ptyp_package>(&ct0->desc))
                lid_to_dotted(pk0->path.txt, mt);
            if (mt.empty()) mt = expr_pack_modtype(*b.expr);
            if (!mt.empty()) pack_modtype_[pv0->name.txt] = mt;
          }
        }
        if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
          // alias elimination: `let x = <var v>` binds nothing; x exports as v.
          if (auto* rid = std::get_if<Pexp_ident>(&b.expr->desc))
            if (auto* rl = std::get_if<Lident>(&rid->id.txt.v))
              if (auto* tgt = lookup(rl->name)) {
                add_export(pv->name.txt, *tgt);
                scope.back()[pv->name.txt] = *tgt;
                continue;
              }
          Ident id = fresh(pv->name.txt);
          cur.push_back({id, pat_kind(&b.pat),
                         fn_binding_rhs(pv->name.txt, *b.expr, b.attrs, /*module_level=*/true)});
          scope.back()[pv->name.txt] = id;
          record_fn_sig(id, b.expr.get());
          add_export(pv->name.txt, id);
        } else if (std::holds_alternative<Ppat_any>(b.pat.desc)) {
          flush(); segs.push_back({true, false, {}, expr(*b.expr)});  // `let _ = e` -> seq
        } else {
          // `let (P1 | P2) = e`: an or-pattern of constructor alternatives binding
          // the same variables.  Switch over e's tag into a tuple of the bound
          // values, then export each component off the tuple.
          if (const Pattern* ep = effective_pat(&b.pat);
              std::holds_alternative<Ppat_or>(ep->desc)) {
            std::vector<std::string> order; LamPtr orval;
            Ident mtmp = fresh("", true); auto mv = mk(Lam::K::Var); mv->var = mtmp;
            if (or_pattern_values(*ep, mv, b.pat.loc, order, orval)) {
              cur.push_back({mtmp, ValueKind::Gen, expr(*b.expr)});
              if (order.size() == 1) {
                Ident vid = fresh(order[0]);
                cur.push_back({vid, ValueKind::Gen, orval});
                scope.back()[order[0]] = vid; add_export(order[0], vid);
              } else {
                Ident tupid = fresh("", true);
                cur.push_back({tupid, ValueKind::Gen, orval});
                auto tv = mk(Lam::K::Var); tv->var = tupid;
                for (size_t i = 0; i < order.size(); ++i) {
                  Ident vid = fresh(order[i]);
                  cur.push_back({vid, ValueKind::Gen, fieldimm((int)i, tv), true});
                  scope.back()[order[i]] = vid; add_export(order[i], vid);
                }
              }
              continue;
            }
          }
          // `let (a,b) = e` / `let {x;y} = e`: each component becomes its own export,
          // bound (`=a`) to a field read of e -- directly when e is a variable or a
          // constant block (whose element reads fold to the elements), else via a
          // *match* temp.  ocamlc emits the component bindings in reverse field order
          // but exports them in source order; a refutable pattern stays a bare temp.
          LamPtr val = expr(*b.expr);
          bool direct = val->k == Lam::K::Var || val->k == Lam::K::ConstBlock;
          LamPtr scrut; Ident tmp;
          if (direct) scrut = val;
          else { tmp = fresh("", true); auto tv = mk(Lam::K::Var); tv->var = tmp; scrut = tv; }
          std::vector<std::pair<Ident, LamPtr>> binders;
          if (collect_binders(b.pat, scrut, binders) && !binders.empty()) {
            record_tuple_sigs(b.pat, *b.expr);
            if (!direct) cur.push_back({tmp, ValueKind::Gen, val});
            auto fold = [&](const LamPtr& acc) -> std::pair<LamPtr, bool> {
              if (acc->k == Lam::K::Prim && !acc->args.empty() &&
                  acc->args[0]->k == Lam::K::ConstBlock &&
                  (acc->prim == Prim::FieldImm || acc->prim == Prim::FieldInt ||
                   acc->prim == Prim::FieldMut)) {
                auto& cb = acc->args[0];
                if (acc->prim_arg >= 0 && acc->prim_arg < (int)cb->args.size())
                  return {cb->args[acc->prim_arg], false};  // const element -> strict
              }
              return {acc, is_field_access(acc)};  // a field read -> `=a` alias
            };
            for (auto it2 = binders.rbegin(); it2 != binders.rend(); ++it2) {
              auto [v, alias] = fold(it2->second);
              cur.push_back({it2->first, ValueKind::Gen, v, alias});
            }
            for (auto& [id, acc] : binders) add_export(id.name, id);
          } else {  // `let () = e` and other refutable patterns: a bare *match* temp
            cur.push_back({fresh("", true), ValueKind::Gen, val});
          }
        }
      }
    }
    flush();
    // A signature ascription `(struct .. : S)` coerces the export block to S's
    // fields (selected and reordered by name); the structure's bindings stay.
    if (coerce) {
      std::vector<LamPtr> ce; std::vector<std::string> cn;
      for (auto& nm : *coerce) {
        auto it = std::find(export_names.begin(), export_names.end(), nm);
        if (it != export_names.end()) {
          LamPtr val = exports[it - export_names.begin()];
          // a submodule whose own block layout differs from its .mli signature
          // (Domain.DLS: internal field order != exposed [new_key; get; set])
          // is reprojected to the .mli order so external field reads land right.
          // When this unit's full .cmi signature is available (coerce_sig), drive
          // the reorder from it so the coercion recurses into NESTED submodules
          // (Misc.Stdlib.String: misc.ml and misc.mli order String's members
          // differently -- the top-level Misc coercion must reach inside).
          if (const cmi::Signature* nsig =
                  coerce_sig ? nested_cmi_sig(*coerce_sig, nm) : nullptr) {
            std::vector<std::string> srclay = layout_vec(nm);
            if (!srclay.empty())
              if (LamPtr c = coerce_block(val, srclay, nsig->fields, nullptr,
                                          nullptr, nsig, nm))
                val = c;
          } else if (auto sf = mli_submodule_fields_.find(nm);
                     sf != mli_submodule_fields_.end()) {
            std::vector<std::string> srclay = layout_vec(nm);
            if (!srclay.empty())
              if (LamPtr c = coerce_block(val, srclay, sf->second)) val = c;
          }
          ce.push_back(val); cn.push_back(nm);
        } else if (auto a = module_alias_.find(nm); a != module_alias_.end()) {
          // a module the struct ELIDED as an alias (`module Elem = E`) but the
          // ascribed sig exposes -> materialise the alias value at this slot.
          ce.push_back(a->second); cn.push_back(nm);
        } else if (auto ex = externals_.find(nm);
                   ex != externals_.end() && prim_stub(ex->second)) {
          // The impl declares this as an `external` (inlined, no field) but the
          // interface exposes it as a `val` (takes a field): materialise the
          // primitive as an eta-stub closure, like ocamlc's coercion does --
          // e.g. stdlib.ml's `external flush = "caml_ml_flush"` vs stdlib.mli's
          // `val flush`.
          ce.push_back(prim_stub(ex->second)); cn.push_back(nm);
        } else if (auto lp = local_prims_.find(nm);
                   lp != local_prims_.end() && prim_stub({lp->second.first, lp->second.second})) {
          ce.push_back(prim_stub({lp->second.first, lp->second.second})); cn.push_back(nm);
        } else if (LamPtr v = exn_value(nm)) {
          // An exception the .mli re-exposes but the impl only brings into scope via
          // `open M` (parser.ml: `module MenhirBasics = struct exception Error = ..
          // end; open MenhirBasics`, and parser.mli exposes `exception Error`):
          // materialise its value (a submodule field) at this slot.  Without it the
          // whole exported block is short by one and consumers read shifted fields.
          ce.push_back(v); cn.push_back(nm);
        } else {
          // an S member that is a PRIMITIVE of an `include`d stdlib module
          // (`include Float.Array` then `: S` with `val unsafe_get`): it has no
          // runtime field there, so the coercion eta-stubs it.
          for (auto& im : inc_stdlib_mods) {
            StdPrim sp = im.find('.') != std::string::npos ? submodule_prim(im, nm)
                                                           : value_prim(im, nm);
            if (sp.name.empty()) continue;
            if (LamPtr s = prim_stub(sp)) { ce.push_back(s); cn.push_back(nm); break; }
          }
        }
      }
      exports = std::move(ce); export_names = std::move(cn);
    }
    if (names) *names = export_names;  // the (deduplicated) export layout, in order
    auto block = mk(Lam::K::Prim);
    block->prim = Prim::Makeblock; block->prim_arg = 0; block->args = std::move(exports);
    LamPtr acc = block;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
      if (it->seq) { auto sq = mk(Lam::K::Sequence); sq->cond = it->e; sq->else_ = acc; acc = sq; }
      else {  // updates (caml_update_dummy of value recursion) run before the rest
        LamPtr body = acc;
        for (auto u = it->updates.rbegin(); u != it->updates.rend(); ++u) {
          auto sq = mk(Lam::K::Sequence); sq->cond = *u; sq->else_ = body; body = sq;
        }
        auto l = mk(it->rec_ ? Lam::K::Letrec : Lam::K::Let);
        l->bindings = std::move(it->binds); l->body = body; acc = l;
      }
    }
    for (int i = 0; i < n_opens; ++i) opened_.pop_back();
    scope.pop_back();
    return acc;
  }
};

}  // namespace

static std::vector<std::string> g_module_dirs;
void set_module_dirs(std::vector<std::string> dirs) { g_module_dirs = std::move(dirs); }

LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name,
                                const std::string& stdlib_dir, const std::string& file_name,
                                std::vector<std::string>* required_globals) {
  bool prof = std::getenv("CPPCAML_PROFILE") != nullptr;
  using clk = std::chrono::steady_clock;
  auto tp = clk::now();
  auto lap = [&](const char* what) {
    auto now = clk::now();
    if (prof)
      std::cerr << "    [" << what << "] "
                << std::chrono::duration<double, std::milli>(now - tp).count() << " ms\n";
    tp = now;
  };
  Translator t;
  t.stdlib_dir = stdlib_dir;
  t.module_dirs_ = g_module_dirs;
  t.file_name_ = file_name;
  set_infer_stdlib_dir(stdlib_dir);  // the inferencer reads .cmi files too
  t.vk = infer_value_kinds(s);
  lap("infer_value_kinds");
  t.register_predef_ctor_info();
  t.register_stdlib_ctors();
  t.register_types(s);
  try {  // Stdlib value -> module field index, for pervasive resolution
    auto cmi = cmi::CmiFile::load(stdlib_dir + "/stdlib.cmi");
    int i = 0;
    for (auto& f : cmi.sig().fields) t.stdlib_fields[f] = i++;
    for (auto& v : cmi.values())
      if (!v.prim.empty()) t.stdlib_prims[v.name] = {v.prim, v.prim_arity};
  } catch (...) {}
  lap("register types/ctors + stdlib.cmi");
  t.mod_path_ = module_name;
  t.unit_name_ = module_name;
  // If this unit has a hand-written interface (.mli, already compiled to .cmi),
  // coerce the implementation's module block to the interface's field order --
  // exactly the impl->intf coercion ocamlc performs.  Without it, the .cmo block
  // is laid out in .ml definition order while consumers read fields at the .cmi's
  // (.mli) order, so a separate compilation against the .cmi reads wrong fields
  // (e.g. std_exit's `do_at_exit` vs stdlib.ml's layout -> segfault).
  std::vector<std::string> mli_fields;
  {
    namespace fs = std::filesystem;
    fs::path mli = fs::path(file_name); mli.replace_extension(".mli");
    fs::path cmi = fs::path(file_name); cmi.replace_extension(".cmi");
    if (fs::exists(mli) && fs::exists(cmi)) {
      try {
        auto c = cmi::CmiFile::load(cmi.string());
        if (c.module_name() == module_name) {
          mli_fields = c.sig().fields;
          t.mli_cmi_sig_ = c.sig(); t.has_mli_cmi_ = true;  // for nested coercion
          // record each top-level functor's RESULT field order, to coerce its
          // body struct (Map.Make : S -> S's fields).  Also recurse into Sig
          // submodules so a NESTED functor (CamlinternalMenhirLib's Engine.Make :
          // ENGINE) gets its result recorded under the dotted key "Engine.Make" --
          // without it the .ml body (`include T; .. let entry ..`) stays in raw
          // order and `MenhirInterpreter.entry` reads the wrong field at runtime.
          std::function<void(const cmi::Signature&, const std::string&)> rec_funct =
              [&](const cmi::Signature& sig, const std::string& prefix) {
            for (auto& sm : sig.modules) {
              std::string key = prefix.empty() ? sm.name : prefix + "." + sm.name;
              if (sm.type && sm.type->kind == cmi::ModuleType::Functor &&
                  sm.type->functor_body && sm.type->functor_body->sig)
                t.mli_functor_results_[key] = sm.type->functor_body->sig->fields;
              if (sm.type && sm.type->kind == cmi::ModuleType::Sig && sm.type->sig)
                rec_funct(*sm.type->sig, key);
            }
          };
          for (auto& md : c.modules())
            if (md.type && md.type->kind == cmi::ModuleType::Sig && md.type->sig)
              rec_funct(*md.type->sig, md.name);
          for (auto& md : c.modules()) {
            if (md.type && md.type->kind == cmi::ModuleType::Functor &&
                md.type->functor_body && md.type->functor_body->sig)
              t.mli_functor_results_[md.name] = md.type->functor_body->sig->fields;
            // a plain submodule (Domain.DLS): record its values' label sigs so a
            // same-unit `DLS.new_key (fun..)` fills the omitted optional, and its
            // exported field order so the impl->intf coercion reorders its block.
            if (md.type && md.type->kind == cmi::ModuleType::Sig && md.type->sig) {
              t.mli_submodule_fields_[md.name] = md.type->sig->fields;
              for (auto& v : md.type->sig->values) {
                Translator::FnSig fs = Translator::cmi_arrow_sig(v.type);
                for (auto& [k, n] : fs)
                  if (k != 0) { t.local_member_sig_[md.name][v.name] = fs; break; }
              }
            }
          }
        }
      } catch (...) {}
    }
  }
  auto sg = mk(Lam::K::Prim);
  sg->prim = Prim::Setglobal; sg->prim_id = module_name;
  sg->args.push_back(t.wrap_shared(
      t.build_module(s, nullptr, mli_fields.empty() ? nullptr : &mli_fields,
                     nullptr, t.has_mli_cmi_ ? &t.mli_cmi_sig_ : nullptr)));
  lap("build_module");
  LamPtr root = sg;
  t.simplify_local_functions(root);
  t.simplify_static_catches(root);
  t.inline_var_aliases(root);
  lap("simplify");
  if (required_globals) {
    std::set<std::string> rg;
    t.collect_required_globals(root, rg);
    required_globals->assign(rg.begin(), rg.end());
  }
  return root;
}

void print_dlambda(const LamPtr& code, std::ostream& out) {
  Pr pr;
  std::ostringstream ss;
  DocP d = to_doc(code, pr);
  set_sizes(d, 0);
  Render r{ss};
  r.go(d);
  out << ss.str() << "\n";
}

std::string structured_constant(const LamPtr& c) {
  Pr pr;
  std::ostringstream ss;
  DocP d = to_doc(c, pr);
  set_sizes(d, 0);
  Render r{ss};
  r.go(d);
  return ss.str();
}

std::string const_instruction(const LamPtr& c) {
  Pr pr;
  std::ostringstream ss;
  // printinstr.ml: @[<10>\tconst@ %a@]
  DocP d = box(BoxT::Box, 10, {text("\tconst"), brk(" "), to_doc(c, pr)});
  set_sizes(d, 0);
  Render r{ss};
  r.go(d);
  return ss.str();
}

}  // namespace cppcaml::lambda
