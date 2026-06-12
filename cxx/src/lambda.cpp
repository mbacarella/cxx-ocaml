#include "cppcaml/lambda.hpp"
#include <cstdint>
#include <cstdio>

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
    else if (b->ch[i]->t == Doc::Box) set_sizes(b->ch[i], run(i + 1));
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
      for (auto& v : l->catch_vars) w += " " + pr.ident(v);
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
  std::vector<std::unordered_map<std::string, Ident>> scope{{}};
  std::unordered_map<std::string, int> stdlib_fields;  // Stdlib value -> field index
  struct StdPrim { std::string name; int arity; };  // an external's prim_name + arity
  std::unordered_map<std::string, StdPrim> stdlib_prims;  // Stdlib value -> prim
  // A labeled/optional function's parameter signature: (label kind 0/1/2, name)
  // per parameter, keyed by the binder stamp -- used to match a call's arguments.
  using FnSig = std::vector<std::pair<int, std::string>>;
  std::map<int, FnSig> fn_sig_;
  // module name ("List", "Printf", ...) -> its value -> field index, cached.
  std::unordered_map<std::string, std::unordered_map<std::string, int>> mod_fields;
  // module name -> its value -> external prim (%builtin or C name) + arity, cached.
  std::unordered_map<std::string, std::unordered_map<std::string, StdPrim>> mod_prims;
  // Locally-defined submodules: name -> its binder, and name -> field layout
  // (export value/submodule name -> field index), for resolving `M.x`.
  std::unordered_map<std::string, Ident> module_ident_;
  // A module alias `module F = M.Sub` resolves F to a pure path expression
  // (`(field_imm i M)`) inlined at use sites, instead of a fresh binding.
  std::unordered_map<std::string, LamPtr> module_alias_;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> module_layout_;
  // A local functor's result field layout, so `Make(Arg).foo` resolves.
  std::unordered_map<std::string, std::vector<std::string>> functor_result_;
  // A local functor's parameter signature layout, to coerce its argument.
  std::unordered_map<std::string, std::vector<std::string>> functor_param_;
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
  // string).  Applying one emits (cname args); %-builtins are left for later.
  std::unordered_map<std::string, std::string> externals_;
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
  std::string mod_path_;  // dotted module path prefix for exception names
  std::string file_name_;  // source path, for Match_failure/Assert_failure locations
  // Predefined exception globals (Match_failure/Assert_failure): a stable stamp per
  // name so the dump's first-appearance normalization is consistent within a file.
  std::unordered_map<std::string, int> predef_global_stamp_;
  int next_exit_ = 0;  // static-exception ids (normalized in the dump, so value is free)

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
  struct CtorInfo { std::string type; int tag; bool is_block; int arity; bool unboxed = false; };
  std::unordered_map<std::string, CtorInfo> ctor_info_;
  std::unordered_map<std::string, std::pair<int, int>> type_ctors_;  // type -> (n_const, n_block)
  std::set<std::string> immediate_local_;  // local all-constant variant type names
  std::set<std::string> gadt_types_;        // variant types with a GADT constructor

  // Locally-declared record fields: label -> {owning type, index, mutable, kind}.
  // Only UNAMBIGUOUS labels are usable (a label reused across records can't be
  // resolved without type direction, so it falls back to a generic translation).
  struct FieldInfo { std::string type; int index; bool mut; ValueKind kind; };
  std::unordered_map<std::string, FieldInfo> field_info_;
  std::set<std::string> ambiguous_fields_;
  struct RecType { std::vector<std::string> labels; bool mut; std::vector<ValueKind> shape; };
  std::unordered_map<std::string, RecType> rec_types_;  // type name -> record layout

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
          if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) { arity = (int)t->elems.size(); block = arity > 0; }
          if (c.res) gadt = true;
          if (block) all_const = false;
          builtin_ctors_.erase(c.name.txt);  // a local decl un-marks a builtin
          ctor_info_[c.name.txt] = {d.name.txt, block ? nb : nc, block, arity, unboxed && arity == 1};
          if (block) ++nb; else ++nc;
        }
        type_ctors_[d.name.txt] = {nc, nb};
        if (all_const && !gadt) immediate_local_.insert(d.name.txt);
        if (gadt) gadt_types_.insert(d.name.txt);
      }
    });
    each_decl([&](const TypeDeclaration& d) {  // then records
      if (auto* rec = std::get_if<Ptype_record>(&d.kind)) {
        RecType rt;
        rt.mut = false;
        int idx = 0;
        for (auto& f : rec->fields) {
          ValueKind k = coretype_kind(*f.type);
          bool m = f.mut == MutableFlag::Mutable;
          rt.mut |= m;
          rt.labels.push_back(f.name.txt);
          rt.shape.push_back(k);
          if (field_info_.count(f.name.txt)) ambiguous_fields_.insert(f.name.txt);
          field_info_[f.name.txt] = {d.name.txt, idx++, m, k};
        }
        rec_types_[d.name.txt] = std::move(rt);
      }
    });
    // Constructors declared inside nested module structures are referenced by a
    // bare name within that module's body (e.g. `module B = struct type t = B ..`
    // then `B`).  Register them too, but only filling names not already taken so a
    // nested type never shadows a top-level one (and clashes stay unresolved).
    std::function<void(const Structure&)> nested = [&](const Structure& items) {
      for (auto& item : items) {
        if (auto* td = std::get_if<Pstr_type>(&item.desc))
          for (auto& d : td->decls)
            if (auto* v = std::get_if<Ptype_variant>(&d.kind)) {
              int nc = 0, nb = 0; bool all_const = !v->ctors.empty(), gadt = false;
              for (auto& c : v->ctors) {
                int arity = 0; bool block = true;
                if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) {
                  arity = (int)t->elems.size(); block = arity > 0;
                }
                if (c.res) gadt = true;
                if (block) all_const = false;
                if (!ctor_info_.count(c.name.txt)) {
                  builtin_ctors_.erase(c.name.txt);
                  ctor_info_[c.name.txt] = {d.name.txt, block ? nb : nc, block, arity};
                }
                if (block) ++nb; else ++nc;
              }
              type_ctors_.emplace(d.name.txt, std::make_pair(nc, nb));
              if (all_const && !gadt) immediate_local_.insert(d.name.txt);
            }
        if (auto* pm = std::get_if<Pstr_module>(&item.desc))
          if (auto* ps = std::get_if<Pmod_structure>(&pm->binding.expr.desc))
            nested(ps->items);
      }
    };
    for (auto& item : s)
      if (auto* pm = std::get_if<Pstr_module>(&item.desc))
        if (auto* ps = std::get_if<Pmod_structure>(&pm->binding.expr.desc))
          nested(ps->items);
  }
  const FieldInfo* find_field(const std::string& label) {
    if (ambiguous_fields_.count(label)) return nullptr;
    auto it = field_info_.find(label);
    return it == field_info_.end() ? nullptr : &it->second;
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
      // Stdlib and the CamlinternalXxx units are top-level compilation units whose
      // cmi is `<lowercase-first-char><rest>.cmi`; other stdlib modules are
      // `stdlib__<Mod>.cmi` submodules.
      std::string path;
      if (mod == "Stdlib") path = stdlib_dir + "/stdlib.cmi";
      else if (mod.rfind("Camlinternal", 0) == 0)
        path = stdlib_dir + "/" + (char)std::tolower((unsigned char)mod[0]) + mod.substr(1) + ".cmi";
      else path = stdlib_dir + "/stdlib__" + mod + ".cmi";
      auto cmi = cmi::CmiFile::load(path);
      int i = 0;
      for (auto& f : cmi.sig().fields) m[f] = i++;
      for (auto& v : cmi.values()) if (!v.prim.empty()) pr[v.name] = {v.prim, v.prim_arity};
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
  static std::string global_of(const std::string& mod) {
    // Stdlib and the CamlinternalXxx units are top-level compilation units; every
    // other stdlib module is a `Stdlib__`-prefixed submodule.
    if (mod == "Stdlib" || mod.rfind("Camlinternal", 0) == 0) return mod;
    return "Stdlib__" + mod;
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
                  bool ok = false; };
  std::unordered_map<std::string, SubMod> submod_cache_;
  const SubMod& submodule_of(const std::string& dotted) {
    auto it = submod_cache_.find(dotted);
    if (it != submod_cache_.end()) return it->second;
    SubMod sm;
    size_t dot = dotted.find('.');
    if (dot != std::string::npos) try {
      std::string head = dotted.substr(0, dot);
      auto cmi = cmi::CmiFile::load(head == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                     : stdlib_dir + "/stdlib__" + head + ".cmi");
      const cmi::Signature* sig = &cmi.sig();
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
        if (ix < 0 || !md || !md->type || md->type->kind != cmi::ModuleType::Sig ||
            !md->type->sig) { fail = true; break; }
        sm.path.push_back(ix);
        sig = md->type->sig.get();
        if (nd == std::string::npos) break;
        pos = nd + 1;
      }
      if (!fail) {
        int i = 0;
        for (auto& f : sig->fields) sm.fields[f] = i++;
        // the submodule's externals (Bigarray.Array1.get = %caml_ba_ref_1 etc.)
        for (auto& v : sig->values)
          if (!v.prim.empty()) sm.prims[v.name] = {v.prim, v.prim_arity};
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
          if (fresh_type) rec_types_[td.name] = std::move(rt);
        }
        sm.ok = true;
      }
    } catch (...) {}
    return submod_cache_[dotted] = std::move(sm);
  }
  StdPrim submodule_prim(const std::string& dotted, const std::string& name) {
    auto& sm = submodule_of(dotted);
    if (!sm.ok) return {"", 0};
    auto it = sm.prims.find(name);
    return it == sm.prims.end() ? StdPrim{"", 0} : it->second;
  }
  LamPtr submodule_value(const std::string& dotted, const std::string& name) {
    auto& sm = submodule_of(dotted);
    if (!sm.ok) return nullptr;
    auto f = sm.fields.find(name);
    if (f == sm.fields.end()) return nullptr;
    auto g = mk(Lam::K::Prim); g->prim = Prim::Global;
    g->prim_id = global_of(dotted.substr(0, dotted.find('.')));
    LamPtr cur = g;
    for (int ix : sm.path) cur = fieldimm(ix, cur);
    return fieldimm(f->second, cur);
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
  FunctorSig stdlib_functor(const std::string& mod, const std::string& name) {
    FunctorSig fs;
    auto& fm = fields_of(mod);
    auto fi = fm.find(name);
    if (fi == fm.end()) return fs;
    fs.idx = fi->second;
    try {
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                    : stdlib_dir + "/stdlib__" + mod + ".cmi");
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
  // The argument labels of a stdlib value's type, in order (0 Nolabel /
  // 1 Labelled / 2 Optional), so a call can insert defaults for omitted optionals.
  std::vector<int> stdlib_value_labels(const std::string& mod, const std::string& name) {
    std::vector<int> labels;
    try {
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                    : stdlib_dir + "/stdlib__" + mod + ".cmi");
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
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                    : stdlib_dir + "/stdlib__" + mod + ".cmi");
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
  static FnSig fn_param_labels(const Pexp_function& f) {
    FnSig v;
    for (auto& fp : f.params) {
      auto* pv = std::get_if<Pparam_val>(&fp.desc);
      if (!pv) return {};  // a `(type a)` param mixed in -> don't model
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
  // Record a binding's parameter labels (only if it is a function with at least
  // one labelled/optional parameter), so its call sites can reorder/wrap args.
  void record_fn_sig(const Ident& id, const Expression* e) {
    if (!e) return;
    if (auto* f = std::get_if<Pexp_function>(&e->desc)) {
      FnSig s = fn_param_labels(*f);
      for (auto& [k, n] : s) if (k != 0) { fn_sig_[id.stamp] = s; return; }
    }
  }
  // The callee's parameter signature for an application: a local function (by its
  // recorded sig) or a qualified stdlib value (from its cmi arrow type).  Empty if
  // unknown or unlabelled.
  FnSig callee_sig(const Expression* fn) {
    auto* id = std::get_if<Pexp_ident>(&fn->desc);
    if (!id) return {};
    if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
      if (auto* b = lookup(l->name)) {
        auto it = fn_sig_.find(b->stamp);
        if (it != fn_sig_.end()) return it->second;
      }
    } else if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
      if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
        FnSig s = stdlib_value_sig(pl->name, d->name);
        for (auto& [k, n] : s) if (k != 0) return s;
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
        // An omitted optional is filled with None (0); an omitted non-optional
        // parameter (a skipped label) becomes an Omitted slot -> a stub closure.
        if (pk == 2) list.push_back({cint(0), false, true});
        else list.push_back({nullptr, true, false});
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
  struct StdField { int index; ValueKind kind; bool mut; };
  std::optional<StdField> stdlib_record_field(const std::string& mod, const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                    : stdlib_dir + "/stdlib__" + mod + ".cmi");
      for (auto& td : cmi.types()) {
        if (td.kind != cmi::TypeDecl::Record) continue;
        for (int i = 0; i < (int)td.labels.size(); ++i)
          if (td.labels[i].name == label)
            return StdField{i, cmi_field_kind(td.labels[i].type), td.labels[i].mutable_};
      }
    } catch (...) {}
    return std::nullopt;
  }
  // The full layout of the stdlib record type (in module `mod`'s cmi) declaring
  // `label`, for unqualified-label updates of non-opened stdlib records
  // (`{ (Gc.get ()) with allocation_policy = 2 }`). All-float (flat) records
  // have a different representation and are not handled.
  struct StdRec { std::vector<std::string> labels; std::vector<ValueKind> shape; std::vector<bool> mut; };
  std::optional<StdRec> stdlib_record_layout(const std::string& mod, const std::string& label) {
    try {
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                    : stdlib_dir + "/stdlib__" + mod + ".cmi");
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
        if (all_float) return std::nullopt;
        return r;
      }
    } catch (...) {}
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
  LamPtr prim_stub_body(const std::string& prim, const std::vector<LamPtr>& argv) {
    int n = (int)argv.size();
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
    if (prim == "%identity" && n == 1) return argv[0];
    if (prim == "%perform" && n == 1) return cc("perform");
    if ((prim == "%succint" || prim == "%predint") && n == 1) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Offsetint;
      pr->prim_arg = prim == "%succint" ? 1 : -1; pr->args = argv; return pr;
    }
    if ((prim == "%field0" || prim == "%field1") && n == 1) {  // generic -> field_mut
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::FieldMut;
      pr->prim_arg = prim == "%field1" ? 1 : 0; pr->args = argv; return pr;
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
    LamPtr r = prim_to_lam(prim, arity, ap, e);
    std::vector<LamPtr> rest;
    if (r) for (auto& t : tail) rest.push_back(expr(*t.second));
    for (auto& t : tail) ap.args.push_back(std::move(t));
    if (!r) return nullptr;
    return lapply_(r, std::move(rest));
  }
  LamPtr prim_to_lam(const std::string& prim, int arity, const Pexp_apply& ap, const Expression& e) {
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
    // `f @@ x` / `x |> f` apply f to x (the function is the 1st / 2nd argument).
    if ((prim == "%apply" || prim == "%revapply") && as.size() == 2) {
      auto a = mk(Lam::K::Apply);
      int fi = prim == "%apply" ? 0 : 1, xi = prim == "%apply" ? 1 : 0;
      a->fn = expr(*as[fi].second); a->args = {expr(*as[xi].second)};
      return a;
    }
    if (prim == "%lazy_force" && as.size() == 1) return force_lazy(expr(*as[0].second));
    // Polymorphic compare specialized by operand kind (the spellings we can be
    // sure of; string/other gen needs the operand type, so left unresolved).
    if (prim == "%compare" && as.size() == 2) {
      ValueKind k0 = expr_kind(as[0].second.get()), k1 = expr_kind(as[1].second.get());
      if (k0 == ValueKind::Int || k1 == ValueKind::Int) return op("compare_ints");
      if (k0 == ValueKind::Float || k1 == ValueKind::Float) return op("compare_floats");
    }
    // fst / snd: read field 0 / 1 with the element's read kind (int vs pointer;
    // a float element would be field_float, which we don't spell -> leave it).
    if ((prim == "%field0" || prim == "%field1") && as.size() == 1) {
      ValueKind k = expr_kind(&e);
      if (k == ValueKind::Int || k == ValueKind::Gen) {
        auto pr = mk(Lam::K::Prim);
        pr->prim = k == ValueKind::Int ? Prim::FieldInt : Prim::FieldMut;
        pr->prim_arg = prim == "%field0" ? 0 : 1;
        pr->args = {expr(*as[0].second)};
        return pr;
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
      return pr;
    }
    return nullptr;
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
  LamPtr alloc_dummy(int size) {
    auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = "caml_alloc_dummy";
    pr->args = {cint(size)}; return pr;
  }
  LamPtr update_dummy(const Ident& id, const LamPtr& val) {
    auto v = mk(Lam::K::Var); v->var = id;
    auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = "caml_update_dummy";
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
  struct RSize { enum K { Dyn, Unreach, Const, Func, Block } k = Dyn; int n = 0; };
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
    std::vector<int> bsize(vals.size(), 0);
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
      if (sz.k == RSize::Block) { cls[i] = Block; bsize[i] = sz.n; continue; }
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
        case Dyn: dyns.push_back({ids[i], kinds[i], collapse_let_id(v)}); break;
        case Block:
          out.dummies.push_back({ids[i], ValueKind::Gen, alloc_dummy(bsize[i])});
          out.updates.push_back(update_dummy(ids[i], collapse_let_id(v)));
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
    if (auto sf = stdlib_fields.find(name); sf != stdlib_fields.end())
      return field_of("Stdlib", sf->second);
    return nullptr;
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
    if (!c.guard) {
      if (is_catchall(*lhsp)) {  // `_`/var: handle unconditionally
        if (auto* pv = std::get_if<Ppat_var>(&lhsp->desc)) scope.back()[pv->name.txt] = exn;
        return expr(*c.rhs);
      }
      if (auto* k = std::get_if<Ppat_construct>(&lhsp->desc)) {
        LamPtr id0 = exn_value(lid_last(k->id.txt));
        // A stdlib module's exception (`Lazy.Undefined`): its identity is the
        // module's runtime export field.
        if (!id0)
          if (auto* d = std::get_if<Ldot>(&k->id.txt.v))
            if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
              auto& fm = fields_of(pl->name);
              if (auto f = fm.find(d->name); f != fm.end()) {
                std::string g = pl->name == "Stdlib" ? "Stdlib"
                                : pl->name.rfind("Camlinternal", 0) == 0
                                    ? pl->name
                                    : "Stdlib__" + pl->name;
                id0 = field_of(g, f->second);
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
          LamPtr then = exn_case_body(exn, k, lid_last(k->id.txt), *c.rhs);
          if (!then) return exn_dispatch(exn, rows, i + 1);  // unsupported binder shape
          auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
          test->args = {lhs, id};
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
  // Null when a sub-pattern is refutable (a constant pattern etc.) -- the case is
  // then skipped, preserving the previous behavior.
  LamPtr exn_case_body(const Ident& exn, const Ppat_construct* k, const std::string& name,
                       const Expression& rhs) {
    if (!k->arg) return expr(rhs);
    int arity = 1;
    if (auto a = exn_arity_.find(name); a != exn_arity_.end()) arity = a->second;
    auto fps = ctor_field_pats(k, arity);
    if ((int)fps.size() != arity) return nullptr;
    auto exv = [&] { auto v = mk(Lam::K::Var); v->var = exn; return v; };
    scope.emplace_back();
    std::vector<std::pair<Ident, LamPtr>> binders;  // simple field binders
    std::vector<std::pair<Ident, LamPtr>> temps;    // *match* temps for structured sub-pats
    std::vector<std::vector<std::pair<Ident, LamPtr>>> sub_binders;
    bool ok = true;
    for (int j = 0; ok && j < arity; ++j) {
      const Pattern* fp = effective_pat(fps[j]);
      LamPtr acc = fieldimm(j + 1, exv());
      if (std::holds_alternative<Ppat_any>(fp->desc)) continue;
      if (std::holds_alternative<Ppat_var>(fp->desc) ||
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
    LamPtr body = expr(rhs);
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
    Ident pp = fresh("param");
    auto th = mk(Lam::K::Function);
    th->params = {{pp, ValueKind::Gen}};
    th->body = expr(scrut);
    auto rs = mk(Lam::K::Prim); rs->prim = Prim::Ccall; rs->prim_id = "runstack";
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
  LamPtr field_read(const FieldInfo* fi, const LamPtr& s) {
    auto l = mk(Lam::K::Prim);
    l->prim = fi->kind == ValueKind::Int ? Prim::FieldInt
              : fi->mut                  ? Prim::FieldMut
                                         : Prim::FieldImm;
    l->prim_arg = fi->index; l->args = {s}; return l;
  }
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
      scope.back()[pv->name.txt] = id; out.push_back({id, scrut}); return true;
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
      for (auto& [lbl, sub] : pr->fields) {
        const FieldInfo* fi = find_field(lid_last(lbl.txt));
        if (!fi) return false;
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
      auto fps = ctor_field_pats(pk, ci->second.arity);
      if ((int)fps.size() != ci->second.arity) return false;
      for (size_t i = 0; i < fps.size(); ++i)
        if (!collect_binders(*fps[i], fieldimm((int)i, scrut), out)) return false;
      return true;
    }
    // A polymorphic-variant pattern: the block is [hash; arg], the payload at
    // field 1 (exhaustive by typing when ocamlc emits no tag test either).
    if (auto* pvr = std::get_if<Ppat_variant>(&p->desc)) {
      if (!pvr->arg) return true;
      return collect_binders(**pvr->arg, fieldimm(1, scrut), out);
    }
    return false;
  }
  // Strip type constraints and peel `[@@unboxed]` constructor wrappers (whose
  // value is transparently their argument), giving the pattern that actually
  // tests/binds the scrutinee.
  const Pattern* effective_pat(const Pattern* p) {
    while (true) {
      while (auto* c = std::get_if<Ppat_constraint>(&p->desc)) p = c->p.get();
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
      for (auto& [lbl, sub] : pr->fields) {
        const FieldInfo* fi = find_field(lid_last(lbl.txt));
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
        std::holds_alternative<Ppat_var>(p->desc)) return true;
    if (auto* pa = std::get_if<Ppat_alias>(&p->desc)) return is_irrefutable(*pa->p);
    if (auto* pt = std::get_if<Ppat_tuple>(&p->desc)) {
      for (auto& e : pt->elems) if (!is_irrefutable(*e)) return false;
      return true;
    }
    if (auto* pr = std::get_if<Ppat_record>(&p->desc)) {
      for (auto& [lbl, sub] : pr->fields) {
        if (!find_field(lid_last(lbl.txt))) return false;
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
      for (auto* fp : ctor_field_pats(pk, ci->second.arity))
        if (!is_irrefutable(*fp)) return false;
      return true;
    }
    if (auto* pvr = std::get_if<Ppat_variant>(&p->desc))
      return !pvr->arg || is_irrefutable(**pvr->arg);
    return false;
  }
  // Wrap `body` (already compiled with `binders` in scope) so each binder's
  // variable reads its field access -- inlined when used at most once, `=a`-aliased
  // (a field read is an alias) otherwise; exactly ocamlc's matcher + simplif.
  LamPtr wrap_binders(LamPtr body, std::vector<std::pair<Ident, LamPtr>>& binders) {
    std::vector<Lam::Binding> aliases;
    for (auto& [id, acc] : binders) {
      if (count_var(body, id) <= 1) subst_var(body, id, acc);
      else aliases.push_back({id, ValueKind::Gen, acc, is_field_access(acc)});
    }
    if (aliases.empty()) return body;
    auto l = mk(Lam::K::Let); l->bindings = std::move(aliases); l->body = body; return l;
  }
  // An IMMUTABLE field read -- the alias (`=a`) class.  A mutable read
  // (field_mut) is a strict computation: re-evaluation could differ.
  static bool is_field_access(const LamPtr& l) {
    return l->k == Lam::K::Prim &&
           (l->prim == Prim::FieldImm || l->prim == Prim::FieldInt);
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
        for (auto& [lbl, sub] : pr->fields) {
          const FieldInfo* fi = find_field(lid_last(lbl.txt));
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
      if (ci.arity > 1) {
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
    std::string type;
    std::set<int> cseen;                            // covered constant values
    std::map<int, const Row*> crow;                 // const value -> its (sole) row
    std::map<int, std::vector<const Row*>> brows;   // block tag -> its rows, in order
    for (auto& r : rows) {
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
    for (auto& [v, r] : crow) cmap[v] = expr(*r->rhs);
    for (auto& [tag, rs] : brows) {
      auto& ci = ctor_info_.at(ctor_of(*rs[0]->lhs));
      LamPtr body = build_ctor_group_arm(scrut, ci, rs, mloc, dflt);
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
      auto fps = ctor_field_pats(k_, ci.arity);
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
      auto l = mk(Lam::K::Let); l->bindings = {{mv, ValueKind::Gen, scrut, is_field_access(scrut)}};
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
    if (auto cm = ctor_match(scrut, rows, mloc)) return cm;
    if (auto nm = nested_match(scrut, rows, mloc)) return nm;
    if (auto em = ext_match(scrut, rows)) return em;
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
      auto* k = std::get_if<Ppat_construct>(&rows[i].lhs->desc);
      if (!k) return nullptr;
      std::string n = lid_last(k->id.txt);
      if ((!exn_ident_.count(n) && !exn_field_.count(n)) || ctor_info_.count(n))
        return nullptr;
      if (k->arg) {  // binder shapes exn_case_body supports only
        int arity = exn_arity_.count(n) ? exn_arity_[n] : 1;
        for (auto* fp : ctor_field_pats(k, arity)) {
          const Pattern* e = effective_pat(fp);
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
    auto* k = std::get_if<Ppat_construct>(&rows[i].lhs->desc);
    std::string n = lid_last(k->id.txt);
    LamPtr idv = exn_value(n);
    if (!idv) return nullptr;
    LamPtr lhs = k->arg ? fieldimm(0, sv()) : sv();
    LamPtr body = exn_case_body(sid, k, n, *rows[i].rhs);
    if (!body) return nullptr;
    LamPtr rest = ext_match_arm(sid, rows, i + 1);
    if (!rest) return nullptr;
    auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
    test->args = {lhs, idv};
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
  // A catch-all `n -> ...` binds n to the scrutinee (which, for a var scrutinee,
  // is just an alias to its binder).
  void bind_catchall(const Pattern& p, const LamPtr& scrut) {
    if (auto* pv = std::get_if<Ppat_var>(&p.desc))
      if (scrut->k == Lam::K::Var) scope.back()[pv->name.txt] = scrut->var;
  }
  LamPtr int_cases(const LamPtr& scrut, const std::vector<Row>& rows, size_t i) {
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
      enter(); bind_catchall(*lhs, scrut);
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = expr(*r.guard); iff->then_ = expr(*r.rhs);
      leave();
      iff->else_ = int_cases(scrut, rows, i + 1);
      return iff;
    }
    if (!r.guard && (is_catchall(*lhs) || i + 1 == rows.size())) {
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
        auto iff = mk(Lam::K::IfThenElse);
        if (ctor && val == 0) {
          iff->cond = scrut;  // constant ctor of tag 0: a truthy test (`!= 0` is identity)
        } else {
          auto ne = mk(Lam::K::Prim); ne->prim = Prim::NotEqInt; ne->args = {scrut, cint(val)};
          iff->cond = ne;
        }
        iff->then_ = int_cases(scrut, rows, i + 1);
        enter(); iff->else_ = expr(*r.rhs); leave();
        return iff;
      }
    }
    return expr(*r.rhs);  // unsupported pattern: best-effort
  }

  LamPtr expr(const Expression& e) {
    // The rec-RHS spine flag holds only along tail spines: take it, clear it,
    // and re-set it just before each spine-continuing body recursion below.
    bool rec_spine = rec_spine_;
    rec_spine_ = false;
    // `M.(body)` / `let open M in body`: resolve `body`'s unqualified names in M.
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) {
      if (auto* op = std::get_if<Pstr_open>(&si->item->desc))
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
          std::string dotted;  // a dotted submodule path opens under its full path
          if (!lid_to_dotted(mi->id.txt, dotted)) dotted = lid_last(mi->id.txt);
          if (dotted.find('.') != std::string::npos)
            submodule_of(dotted);  // eager: registers its record-type labels
          opened_.push_back(dotted);
          rec_spine_ = rec_spine;
          LamPtr b = expr(*si->body);
          opened_.pop_back();
          return b;
        }
      // `let module M = me in body`: bind M (value + layout), then the body.
      if (auto* pm = std::get_if<Pstr_module>(&si->item->desc))
        if (pm->binding.name.txt) {
          auto& mb = pm->binding;
          const std::string& nm = *mb.name.txt;
          LamPtr modval = compile_module_expr(mb.expr);
          auto rl = module_result_layout(mb.expr);
          if (rl.empty()) rl = arg_layout(mb.expr);  // a module path -> its own fields
          // save the names this binding shadows (M is local to the body)
          bool had_i = module_ident_.count(nm), had_a = module_alias_.count(nm);
          Ident sav_i = had_i ? module_ident_[nm] : Ident{};
          LamPtr sav_a = had_a ? module_alias_[nm] : nullptr;
          auto sav_l = module_layout_[nm];
          auto& lay = module_layout_[nm]; lay.clear();
          for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
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
          module_layout_[nm] = sav_l;
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
        exn_ident_[nm] = id;
        if (auto* d = std::get_if<Pext_decl>(&pe->exn.ctor.kind))
          if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
            exn_arity_[nm] = (int)t->elems.size();
        rec_spine_ = rec_spine;
        LamPtr body = expr(*si->body);
        if (had_i) exn_ident_[nm] = sav_i; else exn_ident_.erase(nm);
        if (had_a) exn_arity_[nm] = sav_a; else exn_arity_.erase(nm);
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
      if (!erows.empty() && !vrows.empty() && frows.empty()) {
        int eid = ++next_exit_;
        auto ex = mk(Lam::K::Staticraise);
        ex->prim_arg = eid; ex->args = {expr(*m->e)};
        auto tr = mk(Lam::K::Try);
        tr->body = ex;
        tr->var = fresh("exn");
        scope.emplace_back();
        caught_exn_.push_back(tr->var);
        tr->then_ = exn_dispatch(tr->var, erows, 0);
        caught_exn_.pop_back();
        scope.pop_back();
        // the catch var takes the first var/alias value row's name
        std::string vn;
        for (auto& r : vrows) {
          const Pattern* ep = effective_pat(r.lhs);
          if (auto* pv = std::get_if<Ppat_var>(&ep->desc)) { vn = pv->name.txt; break; }
          if (auto* pa = std::get_if<Ppat_alias>(&ep->desc)) { vn = pa->name.txt; break; }
        }
        Ident v = vn.empty() ? fresh("", true) : fresh(vn);
        auto cat = mk(Lam::K::Catch);
        cat->cond = tr; cat->prim_arg = eid; cat->catch_vars = {v};
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
        if (auto* f0 = find_field(lid_last(rc->fields[0].first.txt)))
          if (auto it = rec_types_.find(f0->type); it != rec_types_.end()) {
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
              fmut = std::move(sr->mut);
              rt = &std_rt;
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
              fr->prim = rt->shape[i] == ValueKind::Int ? Prim::FieldInt
                         : fmut[i]                      ? Prim::FieldMut
                                                        : Prim::FieldImm;
              fr->prim_arg = (int)i; fr->args = {bv};
              vals[i] = fr;
            }
            LamPtr blk;
            if (!rt->mut) {
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
        if (auto* f0 = find_field(lid_last(rc->fields[0].first.txt)))
          if (auto it = rec_types_.find(f0->type);
              it != rec_types_.end() && it->second.labels.size() == labs.size())
            rt = &it->second;
        if (!rt)
          for (auto& [name, cand] : rec_types_) {
            if (cand.labels.size() != labs.size()) continue;
            bool all = true;
            for (auto& l : cand.labels) if (!labs.count(l)) { all = false; break; }
            if (all) { rt = &cand; break; }
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
            vals[ix] = expr(*ve);
          }
          if (ok) {
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
      if (auto* fi = find_field(lid_last(fe->field.txt))) {
        auto l = mk(Lam::K::Prim);
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
      // A module-qualified field `e.M.label` of a stdlib record (e.g. Gc.control).
      if (auto* d = std::get_if<Ldot>(&fe->field.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (auto rf = stdlib_record_field(pl->name, d->name)) {
            auto l = mk(Lam::K::Prim);
            l->prim = rf->kind == ValueKind::Int ? Prim::FieldInt
                      : rf->mut                  ? Prim::FieldMut
                                                 : Prim::FieldImm;
            l->prim_arg = rf->index; l->args = {expr(*fe->e)};
            return l;
          }
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      if (auto* fi = find_field(lid_last(sf->field.txt))) {
        auto l = mk(Lam::K::Prim);
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
      // (`exception Ok` vs result's Ok)
      bool exn_shadows = (exn_ident_.count(n) || exn_field_.count(n)) &&
                         (!ctor_info_.count(n) || builtin_ctors_.count(n));
      if (auto ci = ctor_info_.find(n); ci != ctor_info_.end() && !exn_shadows) {
        if (!ci->second.is_block) return cint(ci->second.tag);  // constant -> its tag
        std::vector<const Expression*> fs;
        if (k->arg) {  // `B of t1 * t2` flattens the tuple argument into fields
          if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
              at && ci->second.arity > 1 && (int)at->elems.size() == ci->second.arity)
            for (auto& el : at->elems) fs.push_back(el.get());
          else
            fs.push_back(k->arg->get());
        }
        return block_of(ci->second.tag, fs);
      }
      if (exn_ident_.count(n) || exn_field_.count(n)) {
        LamPtr v = exn_value(n);
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
      // a predefined exception (Not_found, ...) is a Stdlib field; applied
      // (Invalid_argument "X") it builds the block with the identity at field 0
      if (auto sf = stdlib_fields.find(n); sf != stdlib_fields.end()) {
        if (!k->arg) return field_of("Stdlib", sf->second);
        if (is_predef_exn_name(n)) {
          auto b = mk(Lam::K::Prim); b->prim = Prim::Makeblock; b->prim_arg = 0;
          b->args = {field_of("Stdlib", sf->second), expr(**k->arg)};
          b->blk_shape = {ValueKind::Gen, expr_kind(k->arg->get())};
          return b;
        }
      }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + n);  // user ctor: needs its tag (defer)
      return v;
    }
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
        if (auto* b = lookup(l->name)) { auto v = mk(Lam::K::Var); v->var = *b; return v; }
        // An instance variable referenced in a method body: (field_computed self n).
        if (cur_self_) if (auto iv = inst_vars_.find(l->name); iv != inst_vars_.end()) {
          auto self = mk(Lam::K::Var); self->var = *cur_self_;
          auto idv = mk(Lam::K::Var); idv->var = iv->second;
          auto fc = mk(Lam::K::Prim); fc->prim = Prim::FieldComputed; fc->args = {self, idv};
          return fc;
        }
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
          if (it->find('.') != std::string::npos) {  // opened stdlib submodule
            if (LamPtr v = submodule_value(*it, l->name)) return v;
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
          }
          // Qualified M.x where M is a stdlib (sub)module: field of Stdlib[__M].
          auto& fm = fields_of(pl->name);
          auto sf = fm.find(d->name);
          if (sf != fm.end()) return field_of(global_of(pl->name), sf->second);
          // A prim used as a value (e.g. Sys.argv = %sys_argv -> (caml_sys_argv 0)).
          StdPrim sp = value_prim(pl->name, d->name);
          if (auto pv = prim_value(sp.name)) return pv;
          // Otherwise a primitive in value position eta-expands to a stub
          // (`Int64.add` -> `(function p p stub (Int64.add p p))`).
          if (!sp.name.empty()) if (LamPtr s = prim_stub(sp)) return s;
        }
      // Qualified M.S.x through a stdlib submodule path (Effect.Deep.continue),
      // including an opened head (`Array1.x` under `open Bigarray`) and the
      // submodule's externals (value position -> eta-stub).
      if (auto* d = std::get_if<Ldot>(&id->id.txt.v)) {
        std::string dotted;
        if (lid_to_dotted(*d->prefix, dotted)) {
          std::vector<std::string> cands;
          if (dotted.find('.') != std::string::npos) cands.push_back(dotted);
          for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
            if (it->find('.') == std::string::npos) cands.push_back(*it + "." + dotted);
          for (auto& cand : cands) {
            if (LamPtr v = submodule_value(cand, d->name)) return v;
            if (StdPrim sp = submodule_prim(cand, d->name); !sp.name.empty())
              if (LamPtr s = prim_stub(sp)) return s;
          }
        }
      }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + lid_last(id->id.txt));  // unresolved (will DIFF)
      return v;
    }
    if (auto* ap = std::get_if<Pexp_apply>(&e.desc)) {
      Prim p;
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
            if (auto ex = externals_.find(n); ex != externals_.end()) {  // C external
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = ex->second;
              for (auto& a : as) pr->args.push_back(expr(*a.second));
              return pr;
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
              LamPtr arg = expr(*as[0].second);
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
      for (auto& [lbl, arg] : ap->args) a->args.push_back(expr(*arg));
      a->inline_attr = inline_of_named(ap->fn->attrs, "inlined");  // (f [@inlined never]) x
      if (has_attr(ap->fn->attrs, "tailcall")) {  // (f [@tailcall]) x -> ... tailcall
        if (!a->inline_attr.empty()) a->inline_attr += " ";
        a->inline_attr += "tailcall";
      }
      return a;
    }
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) return function(*f, e.loc);
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
              return expr(*le->bindings[0].expr);
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
            scope.back()[pv->name.txt] = rid;
            ValueKind k = expr_kind(init);
            LamPtr iv = expr(*init);
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
          Ident id = fresh(pv->name.txt);
          Lam::Binding bd{id, pat_kind(&b.pat), expr(*b.expr)};
          l->bindings.push_back(std::move(bd));
          scope.back()[pv->name.txt] = id;
          record_fn_sig(id, b.expr.get());
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
    if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) return expr(*co->e);  // (e :> t) erased
    if (auto* pp = std::get_if<Pexp_pack>(&e.desc))   // (module ME): the module value
      return compile_module_expr(*pp->me);
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
    if (auto* xe = std::get_if<Pexp_extension>(&e.desc))
      if ((xe->name == "extension_constructor" ||
           xe->name == "ocaml.extension_constructor") &&
          xe->payload.str.size() == 1)
        if (auto* ev = std::get_if<Pstr_eval>(&xe->payload.str[0].desc))
          return expr(*ev->e);
    if (auto* nw = std::get_if<Pexp_new>(&e.desc)) {  // new c -> (apply (field_mut 0 c) 0)
      LamPtr clsval;
      if (auto* l = std::get_if<Lident>(&nw->id.txt.v)) {
        if (auto* b = lookup(l->name)) clsval = varof(*b);
      } else if (auto* d = std::get_if<Ldot>(&nw->id.txt.v)) {
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (LamPtr base = module_base(pl->name)) {
            auto& lay = module_layout_[pl->name];
            if (auto f = lay.find(d->name); f != lay.end()) {
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {base}; clsval = fi;
            }
          }
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

  // An object structure `object (self) val.. method.. end` (Tcl_structure, concrete
  // fields).  `as_class` selects the class-declaration form (a class_init function +
  // make_class, obj_init taking a self argument) vs the immediate-object form
  // (create_table + a direct obj_init applied to 0).  `cl_params` are class
  // parameters (`class c x = ...`): extra obj_init parameters after self, in scope
  // for the val initialisers.  A method referencing a class parameter needs the
  // env-capture machinery (not yet built) -- detected post-translation and bailed.
  // Returns null (-> placeholder) for shapes not yet handled (inherit/virtual/
  // initializers/env capture).
  LamPtr build_object(const ast::ClassStructure& cs, bool as_class,
                      const std::string& class_name = "",
                      const std::vector<const ast::Pcl_fun*>* cl_params = nullptr,
                      const std::vector<const ast::Pcl_let*>* cl_lets = nullptr,
                      bool virt_class = false) {
    struct Meth { std::string name; const ast::Expression* body; };
    struct Val  { std::string name; const ast::Expression* init; };
    std::vector<Meth> meths;
    std::vector<Val> vals;
    std::vector<const ast::Expression*> initializers;
    // Single `inherit parent args..` (no `as super`), before any val or
    // initializer (its parent-init call precedes their stores in env_init).
    const ClassMeta* parent_meta = nullptr;
    Ident parent_var;
    std::vector<const ast::Expression*> inh_args;
    std::string super_name;  // `inherit parent as super`
    std::vector<std::string> virt_own;  // own virtual methods (virtual class only)
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
        if (!cc) return nullptr;  // virtual val
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
        auto* pl = std::get_if<Lident>(&pc->id.txt.v);
        if (!pl) return nullptr;
        auto mit = class_meta_.find(pl->name);
        const Ident* pid = lookup(pl->name);
        if (mit == class_meta_.end() || !pid) return nullptr;
        parent_meta = &mit->second;
        parent_var = *pid;
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
    if (cl_lets)
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
            auto* pv = std::get_if<ast::Ppat_var>(&b.pat.desc);
            if (!pv) { restore(); return nullptr; }
            LamPtr v = expr(*b.expr);
            if (rhs_leaks_param(v)) { restore(); return nullptr; }
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
      if (auto* pv = std::get_if<ast::Ppat_var>(&cs.self.desc)) scope.back()[pv->name.txt] = self;
      auto save_self = cur_self_; cur_self_ = self;
      // A method `method f a b = e` is one curried function over self plus its own
      // params: prepend self to the (flattened) function translated from the body.
      LamPtr fn;
      if (auto* pf = std::get_if<ast::Pexp_function>(&m.body->desc)) {
        fn = function(*pf, m.body->loc);
        fn->params.insert(fn->params.begin(), {self, ValueKind::Gen});
        fn->ret_kind = ValueKind::Gen;  // method closures use lfunction ~return:Pgenval
      } else {
        fn = mk(Lam::K::Function);
        fn->params = {{self, ValueKind::Gen}};
        fn->body = expr(*m.body);
      }
      cur_self_ = save_self;
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
      if (auto* pv = std::get_if<ast::Ppat_var>(&cs.self.desc)) scope.back()[pv->name.txt] = self;
      auto save_self = cur_self_; cur_self_ = self;
      auto fn = mk(Lam::K::Function);
      fn->params = {{self, ValueKind::Gen}};
      fn->body = expr(*ie);
      cur_self_ = save_self;
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
      LamPtr v = expr(*vl.init);
      cur_self_ = save_self;
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
                                varof(parent_var), cint(1)};
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
    struct OptDef { Ident xid, optid; const Expression* def; ValueKind k; };
    std::vector<OptDef> optdefs;
    const Pattern* refut = nullptr; Ident refut_pid; Location refut_loc; int nrefut = 0;
    for (auto& fp : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        const Pattern* pat = &pv->pat;
        while (auto* pc = std::get_if<Ppat_constraint>(&pat->desc)) pat = pc->p.get();
        if (std::holds_alternative<Optional>(pv->label) && pv->default_)
          if (auto* var = std::get_if<Ppat_var>(&pat->desc)) {
            Ident optid = fresh("opt", true);  // the `*opt*` parameter
            l->params.push_back({optid, ValueKind::Gen});
            Ident xid = fresh(var->name.txt);
            scope.back()[var->name.txt] = xid;
            optdefs.push_back({xid, optid, pv->default_->get(), pat_kind(pat)});
            continue;
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
        iff->cond = cond; iff->then_ = fieldimm(0, optv); iff->else_ = expr(*it->def);
        auto let = mk(Lam::K::Let);
        let->bindings = {{it->xid, it->k, iff}};
        let->body = body; body = let;
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
          scope.pop_back();
          return l;
        }
        if (is_irrefutable(*pat)) {
          Ident pid = fresh("param");
          l->params.push_back({pid, pat_kind(pat)});
          auto pvar = mk(Lam::K::Var); pvar->var = pid;
          collect_binders(*pat, pvar, binders);
          l->body = wrap_optdefs(wrap_binders(expr(*fc->cases[0].rhs), binders));
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
  // Stamp a function value with its binding's annotations, if any.
  LamPtr with_inline(LamPtr v, const Attributes& attrs) {
    if (v && v->k == Lam::K::Function)
      if (auto ia = fn_attrs(attrs); !ia.empty()) v->inline_attr = ia;
    return v;
  }

  // The runtime field layout (value/module names, in order) of a module-type
  // signature -- a functor parameter's value layout.  (externals/types take no
  // slot; only regular values, submodules, and exceptions do.)
  std::vector<std::string> sig_layout(const ModuleType& mt) {
    std::vector<std::string> out;
    if (auto* pi = std::get_if<Pmty_ident>(&mt.desc)) {  // a named module type S
      auto it = modtype_layout_.find(lid_last(pi->id.txt));
      if (it != modtype_layout_.end()) return it->second;
    }
    if (auto* ps = std::get_if<Pmty_signature>(&mt.desc))
      for (auto& it : ps->items) {
        if (auto* v = std::get_if<Psig_value>(&it.desc)) out.push_back(v->vd.name.txt);
        else if (auto* m = std::get_if<Psig_module>(&it.desc)) {
          if (m->md.name.txt) out.push_back(*m->md.name.txt);
        }
      }
    return out;
  }

  // Compile a module expression to its Lambda value: a structure is a record of
  // its exports; a functor is `(function X is_a_functor <body>)`.
  // The runtime field names a module argument exposes (for projecting it to a
  // functor's parameter signature): a local module's layout or a stdlib module's.
  std::vector<std::string> arg_layout(const ModuleExpr& me0) {
    const ModuleExpr* me = &me0;
    while (auto* pc = std::get_if<Pmod_constraint>(&me->desc)) me = pc->me.get();
    if (auto* pi = std::get_if<Pmod_ident>(&me->desc))
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {
        if (auto it = module_layout_.find(l->name); it != module_layout_.end()) {
          std::vector<std::string> v(it->second.size());
          for (auto& [n, i] : it->second) if (i >= 0 && i < (int)v.size()) v[i] = n;
          return v;
        }
        auto& fm = fields_of(l->name);
        std::vector<std::string> v(fm.size());
        for (auto& [n, i] : fm) if (i >= 0 && i < (int)v.size()) v[i] = n;
        return v;
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
  // Compile a functor application `F(Arg)` with ocamlc's coercion: the argument is
  // projected (by field name, via field_mut) to F's parameter signature when it
  // has extra/reordered fields, and a global-path operand is bound to an (unused)
  // `let/N` then re-read.
  LamPtr compile_functor_apply(const Pmod_apply& pa) {
    LamPtr fval; std::vector<std::string> param;
    if (auto* pi = std::get_if<Pmod_ident>(&pa.f->desc)) {
      if (auto* d = std::get_if<Ldot>(&pi->id.txt.v)) {
        if (auto* pl = std::get_if<Lident>(&d->prefix->v))
          if (!module_base(pl->name) && !fields_of(pl->name).empty()) {  // stdlib functor
            auto fs = stdlib_functor(pl->name, d->name);
            if (fs.ok) { fval = field_of(global_of(pl->name), fs.idx); param = fs.param; }
          }
      } else if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {  // local functor
        if (auto it = functor_param_.find(l->name); it != functor_param_.end()) param = it->second;
      }
    }
    if (!fval) fval = compile_module_expr(*pa.f);
    LamPtr aval = compile_module_expr(*pa.arg);
    LamPtr acoerced = aval;
    auto alay = arg_layout(*pa.arg);
    // Project only when the argument's layout is known and differs from the
    // parameter signature; an unknown layout (e.g. a struct literal) is passed as is.
    if (!param.empty() && !alay.empty() && param != alay) {
      std::vector<LamPtr> fs;
      for (auto& nm : param) {
        int idx = 0;
        for (int i = 0; i < (int)alay.size(); ++i) if (alay[i] == nm) { idx = i; break; }
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
          auto& lay = module_layout_[nm]; lay.clear();
          auto fields = sig_layout(*fp->type);
          for (int i = 0; i < (int)fields.size(); ++i) lay[fields[i]] = i;
        }
        fn->params.push_back({pid, ValueKind::Gen});
        cur = pf->body.get();
      }
      fn->body = compile_module_expr(*cur);
      for (auto it = saves.rbegin(); it != saves.rend(); ++it) {
        if (it->had) module_ident_[it->nm] = it->id; else module_ident_.erase(it->nm);
        module_layout_[it->nm] = it->lay;
      }
      return fn;
    }
    if (auto* pc = std::get_if<Pmod_constraint>(&me.desc)) return compile_module_expr(*pc->me);
    if (auto* pi = std::get_if<Pmod_ident>(&me.desc)) {  // a module in value position
      if (auto* l = std::get_if<Lident>(&pi->id.txt.v)) {
        if (LamPtr base = module_base(l->name)) return base;
        if (!fields_of(l->name).empty()) {  // a stdlib module: its global
          auto g = mk(Lam::K::Prim); g->prim = Prim::Global; g->prim_id = global_of(l->name);
          return g;
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
    }
    if (auto* pa = std::get_if<Pmod_apply>(&me.desc)) return compile_functor_apply(*pa);
    if (auto* pu = std::get_if<Pmod_apply_unit>(&me.desc)) {  // F() -> (apply F 0)
      auto a = mk(Lam::K::Apply);
      a->fn = compile_module_expr(*pu->f);
      a->args = {cint(0)};
      return a;
    }
    if (auto* un = std::get_if<Pmod_unpack>(&me.desc))  // (val e): unpack is transparent
      return expr(*un->e);
    return mk(Lam::K::ConstInt);  // other module exprs: best-effort
  }
  // The runtime field layout a module expression *produces*: a structure's
  // exports, a functor application's result = the functor's result layout, a
  // constraint's = the ascribed signature.
  // The exported names of a structure (value/module/exception, in declaration
  // order; a redefined name moves to its last position) -- the runtime layout,
  // computed statically (no compilation/side effects).
  static std::vector<std::string> struct_export_names(const Structure& s) {
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
      }
    }
    return out;
  }
  std::vector<std::string> module_result_layout(const ModuleExpr& me) {
    if (auto* ps = std::get_if<Pmod_structure>(&me.desc)) return struct_export_names(ps->items);
    if (auto* pc = std::get_if<Pmod_constraint>(&me.desc)) {
      auto s = sig_layout(*pc->mt);
      return s.empty() ? module_result_layout(*pc->me) : s;
    }
    if (auto* pf = std::get_if<Pmod_functor>(&me.desc)) return module_result_layout(*pf->body);
    const ModuleExpr* head = nullptr;
    if (auto* pa = std::get_if<Pmod_apply>(&me.desc)) head = pa->f.get();
    else if (auto* pu = std::get_if<Pmod_apply_unit>(&me.desc)) head = pu->f.get();
    if (head)
      if (auto* fi = std::get_if<Pmod_ident>(&head->desc)) {
        if (auto* l = std::get_if<Lident>(&fi->id.txt.v)) {
          auto it = functor_result_.find(l->name);
          if (it != functor_result_.end()) return it->second;
        }
        if (auto* d = std::get_if<Ldot>(&fi->id.txt.v))  // a stdlib functor M.Make
          if (auto* pl = std::get_if<Lident>(&d->prefix->v))
            if (!module_base(pl->name) && !fields_of(pl->name).empty()) {
              auto fs = stdlib_functor(pl->name, d->name);
              if (fs.ok) return fs.result;
            }
      }
    return {};
  }

  LamPtr build_module(const Structure& s, std::vector<std::string>* names,
                      const std::vector<std::string>* coerce = nullptr) {
    scope.emplace_back();
    // updates non-empty => a recursive-data group: `(let <binds=dummies>
    // (seq <updates> body))` (caml_alloc_dummy + caml_update_dummy).
    struct Seg { bool seq; bool rec_; std::vector<Lam::Binding> binds; LamPtr e;
                 std::vector<LamPtr> updates; };
    std::vector<Seg> segs;
    std::vector<Lam::Binding> cur;
    std::vector<LamPtr> exports;
    std::vector<std::string> export_names;
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
      if (auto* pe = std::get_if<Pstr_eval>(&it.desc)) {  // bare `e;;` -> seq
        flush(); segs.push_back({true, false, {}, expr(*pe->e)}); continue;
      }
      if (auto* op = std::get_if<Pstr_open>(&it.desc)) {  // open M (brings members in)
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
          std::string dotted;  // a dotted submodule path opens under its full path
          if (!lid_to_dotted(mi->id.txt, dotted)) dotted = lid_last(mi->id.txt);
          if (dotted.find('.') != std::string::npos)
            submodule_of(dotted);  // eager: registers its record-type labels
          opened_.push_back(dotted); ++n_opens;
        }
        continue;
      }
      if (auto* pmt = std::get_if<Pstr_modtype>(&it.desc)) {  // module type S = mty (no slot)
        if (pmt->type) modtype_layout_[pmt->name.txt] = sig_layout(*pmt->type);
        continue;
      }
      if (auto* pp = std::get_if<Pstr_primitive>(&it.desc)) {  // external f = "cname"
        auto& pd = pp->prim;
        if (!pd.prims.empty() && pd.prims[0][0] != '%') {  // C call
          externals_[pd.name.txt] = pd.prims[0];
        } else if (!pd.prims.empty() && pd.type) {
          // a locally-declared %-builtin: arity from the declared arrow type
          int ar = 0;
          const CoreType* t = pd.type.get();
          while (auto* a = std::get_if<Ptyp_arrow>(&t->desc)) { ++ar; t = a->cod.get(); }
          local_prims_[pd.name.txt] = {pd.prims[0], ar};
        }
        continue;
      }
      if (auto* pe = std::get_if<Pstr_exception>(&it.desc)) {  // exception E [of ...]
        const std::string& nm = pe->exn.ctor.name.txt;
        auto str = mk(Lam::K::ConstString); str->str_val = mod_path_ + "." + nm;
        auto oid = mk(Lam::K::Prim); oid->prim = Prim::Ccall;
        oid->prim_id = "caml_fresh_oo_id"; oid->args = {cint(0)};
        auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 248;
        blk->args = {str, oid};
        Ident id = fresh(nm);
        cur.push_back({id, ValueKind::Gen, blk});
        exn_ident_[nm] = id;
        if (auto* d = std::get_if<Pext_decl>(&pe->exn.ctor.kind))
          if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
            exn_arity_[nm] = (int)t->elems.size();
        add_export(nm, id);
        continue;
      }
      if (auto* px = std::get_if<Pstr_typext>(&it.desc)) {  // type t += E ... (incl. effects)
        for (auto& c : px->ext.ctors) {
          const std::string& nm = c.name.txt;
          if (auto* rb = std::get_if<Pext_rebind>(&c.kind)) {  // `E = D`: alias to D
            if (auto* l = std::get_if<Lident>(&rb->id.txt.v))
              if (auto e = exn_ident_.find(l->name); e != exn_ident_.end()) {
                exn_ident_[nm] = e->second; add_export(nm, e->second);
                if (auto a = exn_arity_.find(l->name); a != exn_arity_.end())
                  exn_arity_[nm] = a->second;
              }
            continue;
          }
          auto str = mk(Lam::K::ConstString); str->str_val = mod_path_ + "." + nm;
          auto oid = mk(Lam::K::Prim); oid->prim = Prim::Ccall;
          oid->prim_id = "caml_fresh_oo_id"; oid->args = {cint(0)};
          auto blk = mk(Lam::K::Prim); blk->prim = Prim::Makeblock; blk->prim_arg = 248;
          blk->args = {str, oid};
          Ident id = fresh(nm);
          cur.push_back({id, ValueKind::Gen, blk});
          exn_ident_[nm] = id;
          if (auto* d = std::get_if<Pext_decl>(&c.kind))
            if (auto* t = std::get_if<Pcstr_tuple>(&d->args))
              exn_arity_[nm] = (int)t->elems.size();
          add_export(nm, id);
        }
        continue;
      }
      if (auto* pc = std::get_if<Pstr_class>(&it.desc)) {  // class c = object ... end
        flush();
        for (auto& d : pc->decls) {
          Ident id = fresh(d.name.txt);
          LamPtr v;
          // Peel `class c x y = ...` parameter wrappers (incl. labelled and
          // optional) and `class c = let .. in object` local-binding wrappers.
          const ClassExpr* ce = &d.expr;
          std::vector<const Pcl_fun*> params;
          std::vector<const Pcl_let*> lets;
          for (;;) {
            if (auto* pf = std::get_if<Pcl_fun>(&ce->desc)) {
              params.push_back(pf);
              ce = pf->body.get();
            } else if (auto* pl = std::get_if<Pcl_let>(&ce->desc)) {
              lets.push_back(pl);
              ce = pl->body.get();
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
          if (params.empty() && lets.empty()) {
            // `class a = b`: a pure alias -- no binding, the export IS b.
            if (auto* pcn = std::get_if<Pcl_constr>(&ce->desc)) {
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
            if (auto* apc = std::get_if<Pcl_apply>(&ce->desc))
              if (auto* pcn = std::get_if<Pcl_constr>(&apc->ce->desc))
                if (auto* pl = std::get_if<Lident>(&pcn->id.txt.v))
                  if (const Ident* pid = lookup(pl->name); pid && class_ids_.count(pid->stamp)) {
                    bool simple = true;
                    for (auto& [l, e] : apc->args)
                      if (!std::holds_alternative<Nolabel>(l)) simple = false;
                    if (simple) {
                      Ident ninit = fresh("new_init");
                      Ident oi = fresh("obj_init"), slf = fresh("self");
                      auto wrap = mk(Lam::K::Function);
                      wrap->params = {{oi, ValueKind::Gen}, {slf, ValueKind::Gen}};
                      auto apl = mk(Lam::K::Apply);
                      apl->fn = varof(oi); apl->args = {varof(slf)};
                      for (auto& [l, e] : apc->args) apl->args.push_back(expr(*e));
                      wrap->body = apl;
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
          }
          if (auto* ps = std::get_if<Pcl_structure>(&ce->desc)) {
            v = build_object(ps->cs, /*as_class=*/true, d.name.txt,
                             params.empty() ? nullptr : &params,
                             lets.empty() ? nullptr : &lets, is_virt);
            register_class_meta(d.name.txt, ps->cs);
          }
          scope.back()[d.name.txt] = id;
          class_ids_.insert(id.stamp);
          add_export(d.name.txt, id);
          if (v && is_virt) {
            // virtual class: `c = (caml_alloc_dummy 3)` updated with the 3-tuple
            flush();
            Seg s; s.seq = false; s.rec_ = false;
            s.binds = {{id, ValueKind::Gen, alloc_dummy(3)}};
            s.updates = {update_dummy(id, v)};
            segs.push_back(std::move(s));
            continue;
          }
          if (!v) v = mk(Lam::K::ConstInt);  // unsupported class shape: placeholder
          cur.push_back({id, ValueKind::Gen, v});
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
        if (mb.name.txt)
          if (auto* ps = std::get_if<Pmod_structure>(&me->desc)) {
            std::vector<std::string> sub;
            std::string saved = mod_path_;
            mod_path_ += "." + *mb.name.txt;  // nested exceptions are "Outer.M.E"
            auto exn_before = exn_ident_;
            auto mod_before = module_ident_;
            auto alias_before = module_alias_;
            LamPtr body = build_module(ps->items, &sub, coerce);
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
            for (auto& [nm, iid] : module_ident_) {
              auto bi = mod_before.find(nm);
              if (bi != mod_before.end() && bi->second.stamp == iid.stamp) continue;
              if (auto f = lay.find(nm); f != lay.end())
                module_alias_[nm] = fieldimm(f->second, varof(mid));
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
            module_ident_[*mb.name.txt] = mid;
            add_export(*mb.name.txt, mid);
          } else if (std::holds_alternative<Pmod_functor>(mb.expr.desc)) {
            Ident mid = fresh(*mb.name.txt);            // a functor binds as a function
            cur.push_back({mid, ValueKind::Gen, compile_module_expr(mb.expr)});
            module_ident_[*mb.name.txt] = mid;
            functor_result_[*mb.name.txt] = module_result_layout(mb.expr);  // for Make(..)
            functor_param_[*mb.name.txt] = functor_param_layout(mb.expr);   // for arg coercion
            add_export(*mb.name.txt, mid);
          } else {  // module M = F(X) / M2 / (M : S): bind + layout from the result
            LamPtr mv = compile_module_expr(mb.expr);
            auto& lay = module_layout_[*mb.name.txt]; lay.clear();
            auto rl = module_result_layout(mb.expr);
            for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
            if (is_pure_path(mv)) {  // a module alias `M = N.Sub`: inline the path
              module_alias_[*mb.name.txt] = mv;
              module_ident_.erase(*mb.name.txt);
              // A plain `module M = path` is a type-level alias with no runtime
              // slot (elided from the export); a constrained `module M : S = path`
              // materializes a coerced field.
              if (std::holds_alternative<Pmod_constraint>(mb.expr.desc))
                add_export_val(*mb.name.txt, mv);
            } else {
              Ident mid = fresh(*mb.name.txt);
              cur.push_back({mid, ValueKind::Gen, mv});
              module_ident_[*mb.name.txt] = mid;
              add_export(*mb.name.txt, mid);
            }
          }
        continue;
      }
      if (auto* pin = std::get_if<Pstr_include>(&it.desc)) {  // include ME
        // Splice ME's exported value fields into this structure.  A pure path
        // (an already-evaluated module) needs no binding; a computation (e.g. a
        // functor application) is bound to `include/N` first for its effect.
        LamPtr mv = compile_module_expr(pin->expr);
        LamPtr base = mv;
        if (!is_pure_path(mv)) {
          Ident iid = fresh("include");
          cur.push_back({iid, ValueKind::Gen, mv});
          auto v = mk(Lam::K::Var); v->var = iid; base = v;
        }
        auto rl = module_result_layout(pin->expr);
        for (int i = 0; i < (int)rl.size(); ++i) {
          auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm; fi->prim_arg = i; fi->args = {base};
          add_export_val(rl[i], fi);
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
          vals.push_back(with_inline(expr(*b->expr), b->attrs));
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
          cur.push_back({id, pat_kind(&b.pat), with_inline(expr(*b.expr), b.attrs)});
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
          ce.push_back(exports[it - export_names.begin()]); cn.push_back(nm);
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

LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name,
                                const std::string& stdlib_dir, const std::string& file_name) {
  Translator t;
  t.stdlib_dir = stdlib_dir;
  t.file_name_ = file_name;
  set_infer_stdlib_dir(stdlib_dir);  // the inferencer reads .cmi files too
  t.vk = infer_value_kinds(s);
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
  t.mod_path_ = module_name;
  auto sg = mk(Lam::K::Prim);
  sg->prim = Prim::Setglobal; sg->prim_id = module_name;
  sg->args.push_back(t.wrap_shared(t.build_module(s, nullptr)));
  return sg;
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
