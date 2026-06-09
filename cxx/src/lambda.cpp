#include "cppcaml/lambda.hpp"
#include <cstdio>

#include <algorithm>
#include <functional>
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
    std::string base = i.temp ? "*match*" : i.name;
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
DocP let_doc(const LamPtr& l, Pr& pr) {
  std::vector<DocP> binds{text("(")};
  for (size_t i = 0; i < l->bindings.size(); ++i) {
    auto& b = l->bindings[i];
    if (i) binds.push_back(brk());
    binds.push_back(box(BoxT::Box, 2,
        {text(pr.ident(b.id) + " =" +
              std::string(b.mut ? "mut" : b.alias ? "a" : "") + kind_suffix(b.kind)),
         brk(), to_doc(b.val, pr)}));
  }
  binds.push_back(text(")"));
  DocP bindings = box(BoxT::Hv, 1, std::move(binds));
  return box(BoxT::Box, 2, {text("(let"), brk(), bindings, brk(), to_doc(l->body, pr), text(")")});
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
    if (i->suffix) l->str_val = std::string(1, *i->suffix);  // 42L / 42l / 42n
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
  std::string stdlib_dir = "stdlib";  // where to find stdlib*.cmi (CWD-relative by default)
  std::vector<std::unordered_map<std::string, Ident>> scope{{}};
  std::unordered_map<std::string, int> stdlib_fields;  // Stdlib value -> field index
  struct StdPrim { std::string name; int arity; };  // an external's prim_name + arity
  std::unordered_map<std::string, StdPrim> stdlib_prims;  // Stdlib value -> prim
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
  // The exception binders of the enclosing try/with handlers; `raise` of the
  // innermost caught exception is a `reraise`.
  std::vector<Ident> caught_exn_;
  // User C externals: value name -> C primitive name (the `external f = "cname"`
  // string).  Applying one emits (cname args); %-builtins are left for later.
  std::unordered_map<std::string, std::string> externals_;
  // Locally-declared exceptions: name -> its binder (the makeblock-248 value).
  std::unordered_map<std::string, Ident> exn_ident_;
  std::string mod_path_;  // dotted module path prefix for exception names
  std::string file_name_;  // source path, for Match_failure/Assert_failure locations
  // Predefined exception globals (Match_failure/Assert_failure): a stable stamp per
  // name so the dump's first-appearance normalization is consistent within a file.
  std::unordered_map<std::string, int> predef_global_stamp_;
  int next_exit_ = 0;  // static-exception ids (normalized in the dump, so value is free)

  // `(global Name/stamp!)` for a predefined exception used by the compiler.
  LamPtr predef_global(const std::string& name) {
    auto it = predef_global_stamp_.find(name);
    int st = it != predef_global_stamp_.end() ? it->second
                                              : (predef_global_stamp_[name] = stamp++);
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
  void register_predef_ctor_info() {
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
        if (!ambiguous.count(name) && !ctor_info_.count(name)) ctor_info_[name] = ci;
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
                if (!ctor_info_.count(c.name.txt))
                  ctor_info_[c.name.txt] = {d.name.txt, block ? nb : nc, block, arity};
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
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                     : stdlib_dir + "/stdlib__" + mod + ".cmi");
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
    return mod == "Stdlib" ? "Stdlib" : "Stdlib__" + mod;
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

  Ident fresh(const std::string& name, bool temp = false) { return Ident{name, stamp++, temp}; }
  ValueKind pat_kind(const Pattern* p) {
    auto it = vk.pat.find(p);
    return it == vk.pat.end() ? ValueKind::Gen : vkind(it->second);
  }
  ValueKind expr_kind(const Expression* e) {
    auto it = vk.expr.find(e);
    return it == vk.expr.end() ? ValueKind::Gen : vkind(it->second);
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

  // A primitive used as a first-class value is eta-expanded to a `stub` function
  // applying the underlying C call: `compare` -> `(function prim prim stub
  // (caml_compare prim prim))`.  Returns null for prims we don't C-call this way.
  LamPtr prim_stub(const StdPrim& p) {
    static const std::unordered_map<std::string, std::string> poly = {
      {"%compare", "caml_compare"}, {"%equal", "caml_equal"},
      {"%notequal", "caml_notequal"}, {"%lessthan", "caml_lessthan"},
      {"%lessequal", "caml_lessequal"}, {"%greaterthan", "caml_greaterthan"},
      {"%greaterequal", "caml_greaterequal"},
    };
    std::string cname;
    if (auto it = poly.find(p.name); it != poly.end()) cname = it->second;
    else if (!p.name.empty() && p.name[0] != '%') cname = p.name;  // a C external
    else return nullptr;
    int arity = p.arity > 0 ? p.arity : 2;
    auto fn = mk(Lam::K::Function); fn->inline_attr = "stub";
    auto call = mk(Lam::K::Prim); call->prim = Prim::Ccall; call->prim_id = cname;
    for (int i = 0; i < arity; ++i) {
      Ident pp = fresh("prim");
      fn->params.push_back({pp, ValueKind::Gen});
      auto v = mk(Lam::K::Var); v->var = pp; call->args.push_back(v);
    }
    fn->body = call;
    return fn;
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
    if (prim == "%identity" && as.size() == 1) return expr(*as[0].second);  // no-op
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
    // A C-external (non-`%`) primitive applied at its full arity -> a C call.
    if (!prim.empty() && prim[0] != '%' && (int)as.size() == arity) {
      auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = prim;
      pr->args = args();
      return pr;
    }
    return nullptr;
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
  // A recursive binding whose value is a heap block (tuple/ctor/record) -- compiled
  // with caml_alloc_dummy + caml_update_dummy, unlike a recursive function.
  static bool is_rec_data(const LamPtr& v) {
    return v->k == Lam::K::Prim && (v->prim == Prim::Makeblock || v->prim == Prim::Makemutable);
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
    // Each parsed element is a builder taking the already-built `rest`.
    std::vector<std::function<LamPtr(LamPtr)>> elems;
    auto lit = [&](const std::string& run) {  // 1 char -> Char_literal(12), else String_literal(11)
      if (run.size() == 1)
        elems.push_back([this, c = run[0]](LamPtr r) { return cblock(12, {cchar((unsigned char)c), r}); });
      else
        elems.push_back([this, run](LamPtr r) { return cblock(11, {cstr(run), r}); });
    };
    auto flit = [&](int tag) {  // Formatting_lit(17) of a constant formatting_lit
      elems.push_back([this, tag](LamPtr r) { return cblock(17, {cint(tag), r}); });
    };
    auto fbreak = [&](const std::string& src, int w, int o) {  // Formatting_lit(Break)
      elems.push_back([this, src, w, o](LamPtr r) {
        return cblock(17, {cblock(0, {cstr(src), cint(w), cint(o)}), r});
      });
    };
    std::string run;
    size_t i = 0, n = s.size();
    while (i < n) {
      char ch = s[i];
      if (ch == '@') {  // a Format `@` directive (shared format parsing, even Printf)
        if (i + 1 >= n) return nullptr;
        if (!run.empty()) { lit(run); run.clear(); }
        char d = s[i + 1];
        if (d == ']') { flit(0); i += 2; continue; }                 // @] Close_box
        if (d == '}') { flit(1); i += 2; continue; }                 // @} Close_tag
        if (d == '?') { flit(2); i += 2; continue; }                 // @? FFlush
        if (d == '\n') { flit(3); i += 2; continue; }                // @\n Force_newline
        if (d == '.') { flit(4); i += 2; continue; }                 // @. Flush_newline
        if (d == '@') { flit(5); i += 2; continue; }                 // @@ Escaped_at
        if (d == ',') { fbreak("@,", 0, 0); i += 2; continue; }      // @, break
        if (d == ' ') { fbreak("@ ", 1, 0); i += 2; continue; }      // @  break
        if (d == ';') {  // @; or @;<w o>
          size_t j = i + 2; int w = 1, o = 0;
          if (j < n && s[j] == '<') {
            size_t k = s.find('>', j);
            if (k == std::string::npos) return nullptr;
            int a = 0, b = 0; bool sa = false, sb = false; bool sp = false;
            for (size_t p = j + 1; p < k; ++p) {
              char cc = s[p];
              if (cc == ' ') { sp = true; continue; }
              if (cc < '0' || cc > '9') return nullptr;
              if (!sp) { a = a * 10 + (cc - '0'); sa = true; }
              else { b = b * 10 + (cc - '0'); sb = true; }
            }
            if (!sa) return nullptr;
            w = a; o = sb ? b : 0;
            fbreak(s.substr(i, k + 1 - i), w, o); i = k + 1; continue;
          }
          fbreak("@;", 1, 0); i += 2; continue;
        }
        return nullptr;  // @[ @{ @< (box/tag/magic, needs Formatting_gen): fall back
      }
      if (ch != '%') { run += ch; ++i; continue; }
      // a '%' directive: flush any pending literal run first
      ++i;  // past '%'
      if (i >= n) return nullptr;
      if (s[i] == '%') { run += '%'; ++i; continue; }  // %% -> literal '%'
      if (s[i] == '@') { run += '@'; ++i; continue; }  // %@ -> literal '@'
      if (!run.empty()) { lit(run); run.clear(); }
      // flags
      bool plus = false, space = false, hash = false, minus = false, zero = false;
      for (; i < n; ++i) {
        if (s[i] == '+') plus = true;
        else if (s[i] == ' ') space = true;
        else if (s[i] == '#') hash = true;
        else if (s[i] == '-') minus = true;
        else if (s[i] == '0') zero = true;
        else break;
      }
      if (i >= n) return nullptr;
      // width (literal padding) -- '*' (arg-padding) not yet supported
      bool has_w = false; int width = 0;
      while (i < n && s[i] >= '0' && s[i] <= '9') { has_w = true; width = width * 10 + (s[i] - '0'); ++i; }
      // precision .N -- '.*' not yet supported
      bool has_p = false; int prec = 0;
      if (i < n && s[i] == '.') {
        ++i; has_p = true;
        if (i < n && s[i] == '*') return nullptr;
        while (i < n && s[i] >= '0' && s[i] <= '9') { prec = prec * 10 + (s[i] - '0'); ++i; }
      }
      if (i >= n) return nullptr;
      // length modifier for ints: l/n/L
      char len = 0;
      if (s[i] == 'l' || s[i] == 'n' || s[i] == 'L') { len = s[i]; ++i; }
      if (i >= n) return nullptr;
      char conv = s[i]; ++i;
      // padding value: No_padding(int 0) | Lit_padding(block0: padty,width)
      auto padding = [&]() -> LamPtr {
        if (!has_w) return cint(0);
        int padty = minus ? 0 : (zero ? 2 : 1);  // Left=0, Right=1, Zeros=2
        return cblock(0, {cint(padty), cint(width)});
      };
      // precision value: No_precision(int 0) | Lit_precision(block0: n)
      auto precision = [&]() -> LamPtr {
        if (!has_p) return cint(0);
        return cblock(0, {cint(prec)});
      };
      switch (conv) {
        case 'c': elems.push_back([this](LamPtr r) { return cblock(0, {r}); }); break;       // Char
        case 'C': elems.push_back([this](LamPtr r) { return cblock(1, {r}); }); break;       // Caml_char
        case 's': { auto p = padding(); elems.push_back([this, p](LamPtr r) { return cblock(2, {p, r}); }); break; }
        case 'S': { auto p = padding(); elems.push_back([this, p](LamPtr r) { return cblock(3, {p, r}); }); break; }
        case 'b': case 'B': { auto p = padding(); elems.push_back([this, p](LamPtr r) { return cblock(9, {p, r}); }); break; }
        case 'a': elems.push_back([this](LamPtr r) { return cblock(15, {r}); }); break;       // Alpha
        case 't': elems.push_back([this](LamPtr r) { return cblock(16, {r}); }); break;       // Theta
        case '!': elems.push_back([this](LamPtr r) { return cblock(10, {r}); }); break;       // Flush
        case 'd': case 'i': case 'x': case 'X': case 'o': case 'u': {
          int ic = int_conv(conv, plus, space, hash);
          if (ic < 0) return nullptr;
          int tag = len == 'l' ? 5 : len == 'n' ? 6 : len == 'L' ? 7 : 4;  // Int32/Nativeint/Int64/Int
          auto p = padding(), q = precision();
          elems.push_back([this, ic, tag, p, q](LamPtr r) { return cblock(tag, {cint(ic), p, q, r}); });
          break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': case 'F': case 'h': case 'H': {
          int flag = plus ? 1 : space ? 2 : 0;
          int kind = conv == 'f' ? 0 : conv == 'e' ? 1 : conv == 'E' ? 2 : conv == 'g' ? 3
                   : conv == 'G' ? 4 : conv == 'F' ? 5 : conv == 'h' ? 6 : 7;
          auto fconv = cblock(0, {cint(flag), cint(kind)});
          auto p = padding(), q = precision();
          elems.push_back([this, fconv, p, q](LamPtr r) { return cblock(8, {fconv, p, q, r}); });
          break;
        }
        default: return nullptr;  // unhandled directive: fall back to plain string
      }
    }
    if (!run.empty()) lit(run);
    LamPtr fmt = cint(0);  // End_of_format
    for (auto it = elems.rbegin(); it != elems.rend(); ++it) fmt = (*it)(fmt);
    return cblock(0, {fmt, cstr(s)});  // Format (fmt, original)
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
    if (auto sf = stdlib_fields.find(name); sf != stdlib_fields.end())
      return field_of("Stdlib", sf->second);
    return nullptr;
  }
  // Compile a `try ... with` handler body: an if-chain testing the caught
  // exception `exn` against each case, falling through to (reraise exn).
  LamPtr exn_dispatch(const Ident& exn, const std::vector<Case>& cases, size_t i) {
    if (i >= cases.size()) {
      auto rr = mk(Lam::K::Prim); rr->prim = Prim::Reraise;
      auto v = mk(Lam::K::Var); v->var = exn; rr->args = {v}; return rr;
    }
    const Case& c = cases[i];
    if (!c.guard) {
      if (is_catchall(c.lhs)) {  // `_`/var: handle unconditionally
        if (auto* pv = std::get_if<Ppat_var>(&c.lhs.desc)) scope.back()[pv->name.txt] = exn;
        return expr(*c.rhs);
      }
      if (auto* k = std::get_if<Ppat_construct>(&c.lhs.desc))
        if (LamPtr id = exn_value(lid_last(k->id.txt))) {
          auto exv = [&] { auto v = mk(Lam::K::Var); v->var = exn; return v; };
          LamPtr lhs;
          if (k->arg) {  // exn carries data: compare its identity field
            lhs = mk(Lam::K::Prim); lhs->prim = Prim::FieldImm; lhs->prim_arg = 0;
            lhs->args = {exv()};
          } else {
            lhs = exv();
          }
          auto test = mk(Lam::K::Prim); test->prim = Prim::IntCmp; test->prim_id = "==";
          test->args = {lhs, id};
          auto iff = mk(Lam::K::IfThenElse);
          iff->cond = test; iff->then_ = expr(*c.rhs);
          iff->else_ = exn_dispatch(exn, cases, i + 1);
          return iff;
        }
    }
    return exn_dispatch(exn, cases, i + 1);  // unsupported case: skip
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
  static int count_var(const LamPtr& l, const Ident& id) {
    if (!l) return 0;
    if (l->k == Lam::K::Var)
      return (l->var.stamp == id.stamp && l->var.name == id.name) ? 1 : 0;
    int c = count_var(l->fn, id) + count_var(l->body, id) + count_var(l->cond, id) +
            count_var(l->then_, id) + count_var(l->else_, id) + count_var(l->sw_default, id);
    for (auto& a : l->args) c += count_var(a, id);
    for (auto& b : l->bindings) c += count_var(b.val, id);
    for (auto& sc : l->sw_consts) c += count_var(sc.body, id);
    for (auto& sc : l->sw_blocks) c += count_var(sc.body, id);
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
  static bool is_field_access(const LamPtr& l) {
    return l->k == Lam::K::Prim &&
           (l->prim == Prim::FieldImm || l->prim == Prim::FieldInt || l->prim == Prim::FieldMut);
  }

  // A match row as a *borrowed* view into the AST (the Structure outlives the
  // translation), so sub-matches can be built from inner sub-patterns without
  // copying the move-only Case.  guard==nullptr means no `when`.
  struct Row { const Pattern* lhs; const Expression* rhs; const Expression* guard; };
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
    std::vector<std::pair<int, Ident>> binders;
    bool ok = true;
    auto bind_field = [&](int idx, const Pattern& p) {
      if (std::holds_alternative<Ppat_any>(p.desc)) return;             // wildcard: no binder
      if (auto* pv = std::get_if<Ppat_var>(&p.desc)) {
        Ident id = fresh(pv->name.txt);
        scope.back()[pv->name.txt] = id;
        binders.push_back({idx, id});
        return;
      }
      ok = false;  // a deeper sub-pattern -- not handled here
    };
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
    for (auto& [idx, id] : binders) {
      LamPtr fa = fieldimm(idx, scrut);
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
    if (auto sw = const_switch(scrut, rows)) return sw;
    if (auto cm = ctor_match(scrut, rows, mloc)) return cm;
    if (auto nm = nested_match(scrut, rows, mloc)) return nm;
    return int_cases(scrut, rows, 0);
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
    // A guard on a catch-all (var/`_`) pattern inlines: the pattern always matches,
    // so guard-failure just falls through to the rest -> `(if guard body <rest>)`.
    // (A guard on a constant/ctor pattern would share <rest> across pattern- and
    // guard-failure, needing the matcher's catch/exit; left to the best-effort tail.)
    if (r.guard && is_catchall(*r.lhs)) {
      bind_catchall(*r.lhs, scrut);
      auto iff = mk(Lam::K::IfThenElse);
      iff->cond = expr(*r.guard); iff->then_ = expr(*r.rhs);
      iff->else_ = int_cases(scrut, rows, i + 1);
      return iff;
    }
    if (!r.guard && (is_catchall(*r.lhs) || i + 1 == rows.size())) {
      bind_catchall(*r.lhs, scrut);
      return expr(*r.rhs);
    }
    // An integer literal, or a constant constructor (matched by its integer tag),
    // tested against the scrutinee with the rest of the rows as the fall-through.
    if (!r.guard) {
      bool isint = false, ctor = false; long long val = 0;
      if (auto* pc = std::get_if<Ppat_constant>(&r.lhs->desc)) {
        if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc)) { isint = true; val = parse_ocaml_int(pi->value); }
      } else if (auto* k = std::get_if<Ppat_construct>(&r.lhs->desc); k && !k->arg) {
        auto it = ctor_info_.find(ctor_of(*r.lhs));
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
        iff->then_ = int_cases(scrut, rows, i + 1); iff->else_ = expr(*r.rhs);
        return iff;
      }
    }
    return expr(*r.rhs);  // unsupported pattern: best-effort
  }

  LamPtr expr(const Expression& e) {
    // `M.(body)` / `let open M in body`: resolve `body`'s unqualified names in M.
    if (auto* si = std::get_if<Pexp_struct_item>(&e.desc)) {
      if (auto* op = std::get_if<Pstr_open>(&si->item->desc))
        if (auto* mi = std::get_if<Pmod_ident>(&op->expr.desc)) {
          opened_.push_back(lid_last(mi->id.txt));
          LamPtr b = expr(*si->body);
          opened_.pop_back();
          return b;
        }
      // `let module M = me in body`: bind M (value + layout), then the body.
      if (auto* pm = std::get_if<Pstr_module>(&si->item->desc))
        if (pm->binding.name.txt) {
          auto& mb = pm->binding;
          Ident mid = fresh(*mb.name.txt);
          LamPtr modval = compile_module_expr(mb.expr);
          module_ident_[*mb.name.txt] = mid;
          auto& lay = module_layout_[*mb.name.txt]; lay.clear();
          auto rl = module_result_layout(mb.expr);
          for (int i = 0; i < (int)rl.size(); ++i) lay[rl[i]] = i;
          auto l = mk(Lam::K::Let);
          l->bindings = {{mid, ValueKind::Gen, modval}};
          l->body = expr(*si->body);
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
      auto* ic = std::get_if<Pexp_construct>(&as->e->desc);
      if (ic && lid_last(ic->id.txt) == "false") return raise_predef("Assert_failure", e.loc);
      auto i = mk(Lam::K::IfThenElse);
      i->cond = expr(*as->e); i->then_ = cint(0);
      i->else_ = raise_predef("Assert_failure", e.loc);
      return i;
    }
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) return compile_match(expr(*m->e), m->cases, e.loc);
    if (auto* tu = std::get_if<Pexp_tuple>(&e.desc)) {
      std::vector<LamPtr> es;
      std::vector<ValueKind> shape;
      for (auto& el : tu->elems) { es.push_back(expr(*el)); shape.push_back(expr_kind(el.get())); }
      auto b = block(0, std::move(es));
      if (b->k == Lam::K::Prim) b->blk_shape = std::move(shape);  // dynamic block -> field shape
      return b;
    }
    if (auto* rc = std::get_if<Pexp_record>(&e.desc)) {
      if (!rc->base && !rc->fields.empty())  // not a functional update `{e with ..}`
        if (auto* f0 = find_field(lid_last(rc->fields[0].first.txt))) {
          auto rt = rec_types_.find(f0->type);
          if (rt != rec_types_.end()) {
            std::vector<LamPtr> vals(rt->second.labels.size());
            bool ok = vals.size() == rc->fields.size();
            for (auto& [lid, ve] : rc->fields) {
              auto* fi = find_field(lid_last(lid.txt));
              if (!fi || fi->type != f0->type) { ok = false; break; }
              vals[fi->index] = expr(*ve);
            }
            if (ok) {
              if (!rt->second.mut) {
                auto b = block(0, std::move(vals));
                if (b->k == Lam::K::Prim) b->blk_shape = rt->second.shape;  // field kinds
                return b;
              }
              auto m = mk(Lam::K::Prim);  // any mutable field -> makemutable
              m->prim = Prim::Makemutable; m->prim_arg = 0;
              m->blk_shape = rt->second.shape; m->args = std::move(vals);
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
      if (auto ci = ctor_info_.find(n); ci != ctor_info_.end()) {  // local variant ctor
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
      if (auto ei = exn_ident_.find(n); ei != exn_ident_.end() && !k->arg) {
        auto v = mk(Lam::K::Var); v->var = ei->second; return v;  // local exception value
      }
      if (!k->arg)  // a predefined exception (Not_found, ...) is a Stdlib field
        if (auto sf = stdlib_fields.find(n); sf != stdlib_fields.end())
          return field_of("Stdlib", sf->second);
      auto v = mk(Lam::K::Var); v->var = fresh("?" + n);  // user ctor: needs its tag (defer)
      return v;
    }
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* l = std::get_if<Lident>(&id->id.txt.v)) {
        if (auto* b = lookup(l->name)) { auto v = mk(Lam::K::Var); v->var = *b; return v; }
        auto sf = stdlib_fields.find(l->name);  // unqualified pervasive
        if (sf != stdlib_fields.end()) return field_of("Stdlib", sf->second);
        if (auto pi = stdlib_prims.find(l->name); pi != stdlib_prims.end())  // prim as value
          if (LamPtr s = prim_stub(pi->second)) return s;
        // an `open M` brings M's exported values into scope (innermost first)
        for (auto it = opened_.rbegin(); it != opened_.rend(); ++it) {
          if (LamPtr base = module_base(*it)) {  // local module (binding or alias)
            auto& lay = module_layout_[*it];
            if (auto f = lay.find(l->name); f != lay.end()) {
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {base};
              return fi;
            }
          }
          auto& fm = fields_of(*it);  // stdlib module
          if (auto f = fm.find(l->name); f != fm.end())
            return field_of(global_of(*it), f->second);
        }
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
          if (auto pv = prim_value(value_prim(pl->name, d->name).name)) return pv;
        }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + lid_last(id->id.txt));  // unresolved (will DIFF)
      return v;
    }
    if (auto* ap = std::get_if<Pexp_apply>(&e.desc)) {
      Prim p;
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
              if (auto r = prim_to_lam(prim.name, prim.arity, *ap, e)) return r;
          }
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* l = std::get_if<Lident>(&fid->id.txt.v))
          if (!lookup(l->name)) {  // an unshadowed pervasive operator
            const auto& n = l->name;
            auto& as = ap->args;
            if (auto ex = externals_.find(n); ex != externals_.end()) {  // C external
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Ccall; pr->prim_id = ex->second;
              for (auto& a : as) pr->args.push_back(expr(*a.second));
              return pr;
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
              // The operand kind drives the spelling: int -> `==`, float -> `==.`,
              // int64/int32/nativeint -> `Int64.==` etc.; two generics fall back to
              // the polymorphic caml_* compare.
              ValueKind k = ValueKind::Gen;
              for (ValueKind kk : {expr_kind(as[0].second.get()), expr_kind(as[1].second.get())})
                if (kk != ValueKind::Gen) k = kk;
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
              pr->args = {expr(*as[0].second), expr(*as[1].second)};
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
              if (auto r = prim_to_lam(pi->second.name, pi->second.arity, *ap, e)) return r;
            // an `open M`'d external (e.g. Marshal.(to_string ...)): M's prim.
            for (auto it = opened_.rbegin(); it != opened_.rend(); ++it)
              if (auto p = value_prim(*it, n); !p.name.empty())
                if (auto r = prim_to_lam(p.name, p.arity, *ap, e)) return r;
          }
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
        sq->else_ = expr(*le->body);
        return sq;
      }
      // `let x = E in x` -> E: a linear alias binding ocamlc's simplif drops.
      if (le->rf != RecFlag::Recursive && le->bindings.size() == 1)
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
            recs.push_back({&b, id});
          }
        std::vector<LamPtr> vals; bool all_data = !recs.empty();
        for (auto& [b, id] : recs) { LamPtr v = expr(*b->expr); vals.push_back(v); if (!is_rec_data(v)) all_data = false; }
        if (all_data) {  // recursive data: `(let <dummies> (seq <updates> body))`
          auto l = mk(Lam::K::Let);
          std::vector<LamPtr> updates;
          for (size_t i = 0; i < recs.size(); ++i) {
            l->bindings.push_back({recs[i].second, ValueKind::Gen, alloc_dummy((int)vals[i]->args.size())});
            updates.push_back(update_dummy(recs[i].second, vals[i]));
          }
          LamPtr body = expr(*le->body);
          for (auto u = updates.rbegin(); u != updates.rend(); ++u) {
            auto sq = mk(Lam::K::Sequence); sq->cond = *u; sq->else_ = body; body = sq;
          }
          l->body = body; scope.pop_back(); return l;
        }
        auto l = mk(Lam::K::Letrec);
        for (size_t i = 0; i < recs.size(); ++i)
          l->bindings.push_back({recs[i].second, pat_kind(&recs[i].first->pat), vals[i]});
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
        l->var = fresh("param");  // `for _ = ...` (rare)
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
        l->then_ = exn_dispatch(l->var, tr->cases, 0);
      }
      caught_exn_.pop_back();
      scope.pop_back();
      return l;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) return expr(*ct->e);
    if (auto* co = std::get_if<Pexp_coerce>(&e.desc)) return expr(*co->e);  // (e :> t) erased
    if (auto* pp = std::get_if<Pexp_pack>(&e.desc))   // (module ME): the module value
      return compile_module_expr(*pp->me);
    return mk(Lam::K::ConstInt);  // unsupported: placeholder (will DIFF)
  }

  LamPtr function(const Pexp_function& f, const Location& floc) {
    scope.emplace_back();
    auto l = mk(Lam::K::Function);
    std::vector<std::pair<Ident, LamPtr>> binders;  // sub-vars of destructured params
    const Pattern* refut = nullptr; Ident refut_pid; Location refut_loc; int nrefut = 0;
    for (auto& fp : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        // Every parameter gets a binder; a non-variable pattern (a constructor,
        // record or tuple) is named "param" like ocamlc.  An irrefutable one has its
        // variables bound to field reads in the body; a refutable one (a partial
        // pattern, e.g. `(Some x)`) is matched in the body, raising Match_failure on
        // the missing cases -- supported for at most one such parameter.
        const Pattern* pat = &pv->pat;
        while (auto* pc = std::get_if<Ppat_constraint>(&pat->desc)) pat = pc->p.get();
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
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      LamPtr body;
      if (refut && nrefut == 1) {  // a partial parameter pattern -> match in the body
        auto sv = mk(Lam::K::Var); sv->var = refut_pid;
        std::vector<Row> rows = {{refut, fb->e.get(), nullptr}};
        body = compile_match(sv, rows, refut_loc);
      } else {
        body = expr(*fb->e);
      }
      l->body = wrap_binders(body, binders);
    } else if (auto* fc = std::get_if<Pfunction_cases>(&f.body->v)) {
      // `function P -> ...` adds an implicit final parameter matched on.
      Ident pid = fresh("param");
      l->params.push_back({pid, ValueKind::Gen});
      auto scrut = mk(Lam::K::Var); scrut->var = pid;
      l->body = wrap_binders(compile_match(scrut, fc->cases, floc), binders);
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
          opened_.push_back(lid_last(mi->id.txt)); ++n_opens;
        }
        continue;
      }
      if (auto* pmt = std::get_if<Pstr_modtype>(&it.desc)) {  // module type S = mty (no slot)
        if (pmt->type) modtype_layout_[pmt->name.txt] = sig_layout(*pmt->type);
        continue;
      }
      if (auto* pp = std::get_if<Pstr_primitive>(&it.desc)) {  // external f = "cname"
        auto& pd = pp->prim;
        if (!pd.prims.empty() && pd.prims[0][0] != '%')  // C call (not a %-builtin)
          externals_[pd.name.txt] = pd.prims[0];
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
          add_export(nm, id);
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
            LamPtr body = build_module(ps->items, &sub, coerce);
            mod_path_ = saved;
            Ident mid = fresh(*mb.name.txt);
            cur.push_back({mid, ValueKind::Gen, body});
            module_ident_[*mb.name.txt] = mid;
            auto& lay = module_layout_[*mb.name.txt]; lay.clear();
            for (int i = 0; i < (int)sub.size(); ++i) lay[sub[i]] = i;
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
            recs.push_back({&b, id});
          }
        std::vector<LamPtr> vals;
        bool all_data = !recs.empty();
        for (auto& [b, id] : recs) {
          LamPtr v = with_inline(expr(*b->expr), b->attrs);
          vals.push_back(v);
          if (!is_rec_data(v)) all_data = false;
          add_export(std::get_if<Ppat_var>(&b->pat.desc)->name.txt, id);
        }
        if (all_data) {  // recursive data: alloc dummies, then update in place
          std::vector<Lam::Binding> dummies; std::vector<LamPtr> updates;
          for (size_t i = 0; i < recs.size(); ++i) {
            dummies.push_back({recs[i].second, ValueKind::Gen, alloc_dummy((int)vals[i]->args.size())});
            updates.push_back(update_dummy(recs[i].second, vals[i]));
          }
          Seg s; s.seq = false; s.rec_ = false; s.binds = std::move(dummies); s.updates = std::move(updates);
          segs.push_back(std::move(s));
        } else {
          std::vector<Lam::Binding> binds;
          for (size_t i = 0; i < recs.size(); ++i)
            binds.push_back({recs[i].second, pat_kind(&recs[i].first->pat), vals[i]});
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
          add_export(pv->name.txt, id);
        } else if (std::holds_alternative<Ppat_any>(b.pat.desc)) {
          flush(); segs.push_back({true, false, {}, expr(*b.expr)});  // `let _ = e` -> seq
        } else {
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
      else if (!it->updates.empty()) {  // recursive data: let dummies in (seq updates body)
        LamPtr body = acc;
        for (auto u = it->updates.rbegin(); u != it->updates.rend(); ++u) {
          auto sq = mk(Lam::K::Sequence); sq->cond = *u; sq->else_ = body; body = sq;
        }
        auto l = mk(Lam::K::Let); l->bindings = std::move(it->binds); l->body = body; acc = l;
      } else {
        auto l = mk(it->rec_ ? Lam::K::Letrec : Lam::K::Let);
        l->bindings = std::move(it->binds); l->body = acc; acc = l;
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
  sg->args.push_back(t.build_module(s, nullptr));
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

}  // namespace cppcaml::lambda
