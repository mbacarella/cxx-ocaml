#include "cppcaml/lambda.hpp"
#include <cstdio>

#include <algorithm>
#include <functional>
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
          case BoxT::Hov: nl = c->bsize > MARGIN - col; break;
          default:  // Pp_box (the @[<n>] default): format.ml's Pp_box rule.
            // pp_current_indent here is the CURRENT LINE's indent (updated only
            // on a newline), not the running column -- so we test cur_indent.
            if (is_new_line) nl = false;
            else if (c->bsize > MARGIN - col) nl = true;
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
        {text(pr.ident(b.id) + " =" + kind_suffix(b.kind)), brk(), to_doc(b.val, pr)}));
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
        case Prim::Makeblock: head = "(makeblock " + std::to_string(l->prim_arg); break;
        case Prim::Field: head = "(field " + std::to_string(l->prim_arg); break;
        case Prim::FieldImm: head = "(field_imm " + std::to_string(l->prim_arg); break;
        case Prim::Global: return text("(global " + l->prim_id + "!)");
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
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::Function: {
      std::vector<DocP> xs{text("(function")};
      for (auto& [id, k] : l->params) { xs.push_back(brk()); xs.push_back(text(pr.ident(id) + kind_suffix(k))); }
      xs.push_back(brk());
      if (l->ret_kind != ValueKind::Gen) { xs.push_back(text(ret_suffix(l->ret_kind))); xs.push_back(brk()); }
      xs.push_back(to_doc(l->body, pr));
      xs.push_back(text(")"));
      return box(BoxT::Box, 2, std::move(xs));
    }
    case Lam::K::IfThenElse:
      return box(BoxT::Box, 2, {text("(if"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->then_, pr), brk(), to_doc(l->else_, pr), text(")")});
    case Lam::K::Sequence:
      return box(BoxT::Box, 2, {text("(seq"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->else_, pr), text(")")});
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

LamPtr translate_const(const Constant& c) {
  if (auto* i = std::get_if<Pconst_integer>(&c.desc)) {
    auto l = mk(Lam::K::ConstInt); l->int_val = std::stoll(i->value);
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
  std::unordered_map<std::string, std::string> stdlib_prims;  // Stdlib value -> "%prim"
  // module name ("List", "Printf", ...) -> its value -> field index, cached.
  std::unordered_map<std::string, std::unordered_map<std::string, int>> mod_fields;
  // module name -> its value -> "%prim"/C-primitive name (external values), cached.
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> mod_prims;
  // Locally-defined submodules: name -> its binder, and name -> field layout
  // (export value/submodule name -> field index), for resolving `M.x`.
  std::unordered_map<std::string, Ident> module_ident_;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> module_layout_;
  // User C externals: value name -> C primitive name (the `external f = "cname"`
  // string).  Applying one emits (cname args); %-builtins are left for later.
  std::unordered_map<std::string, std::string> externals_;
  // Locally-declared exceptions: name -> its binder (the makeblock-248 value).
  std::unordered_map<std::string, Ident> exn_ident_;
  std::string mod_path_;  // dotted module path prefix for exception names

  // Locally-declared variant constructors: name -> {owning type, tag, is_block}.
  // Constant (nullary) and block (with-args) constructors are numbered
  // separately from 0 in declaration order, matching the runtime representation.
  struct CtorInfo { std::string type; int tag; bool is_block; int arity; };
  std::unordered_map<std::string, CtorInfo> ctor_info_;
  std::unordered_map<std::string, std::pair<int, int>> type_ctors_;  // type -> (n_const, n_block)
  std::set<std::string> immediate_local_;  // local all-constant variant type names

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
        for (auto& c : v->ctors) {
          bool block = true;
          int arity = 0;
          if (auto* t = std::get_if<Pcstr_tuple>(&c.args)) { arity = (int)t->elems.size(); block = arity > 0; }
          if (c.res) gadt = true;
          if (block) all_const = false;
          ctor_info_[c.name.txt] = {d.name.txt, block ? nb : nc, block, arity};
          if (block) ++nb; else ++nc;
        }
        type_ctors_[d.name.txt] = {nc, nb};
        if (all_const && !gadt) immediate_local_.insert(d.name.txt);
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
  // Field map of a stdlib (sub)module; empty if not a loadable stdlib module.
  const std::unordered_map<std::string, int>& fields_of(const std::string& mod) {
    auto it = mod_fields.find(mod);
    if (it != mod_fields.end()) return it->second;
    std::unordered_map<std::string, int> m;
    std::unordered_map<std::string, std::string> pr;
    try {
      auto cmi = cmi::CmiFile::load(mod == "Stdlib" ? stdlib_dir + "/stdlib.cmi"
                                                     : stdlib_dir + "/stdlib__" + mod + ".cmi");
      int i = 0;
      for (auto& f : cmi.sig().fields) m[f] = i++;
      for (auto& v : cmi.values()) if (!v.prim.empty()) pr[v.name] = v.prim;
    } catch (...) {}
    mod_prims[mod] = std::move(pr);
    return mod_fields[mod] = std::move(m);
  }
  // The "%prim"/C-primitive name of a (possibly stdlib) module's value, or "".
  std::string value_prim(const std::string& mod, const std::string& name) {
    fields_of(mod);  // ensures mod_prims[mod] is populated
    auto& pr = mod_prims[mod];
    auto it = pr.find(name);
    return it == pr.end() ? std::string() : it->second;
  }
  static std::string global_of(const std::string& mod) {
    return mod == "Stdlib" ? "Stdlib" : "Stdlib__" + mod;
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
  // Array element-kind annotation (array_kind in printlambda); a known-boxed vs
  // polymorphic element both read as gen here (we can't tell addr from gen).
  static std::string array_kind(ValueKind k) {
    if (k == ValueKind::Int) return "int";
    if (k == ValueKind::Float) return "float";
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
  LamPtr prim_to_lam(const std::string& prim, const Pexp_apply& ap, const Expression& e) {
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

  LamPtr cint(long long n) { auto z = mk(Lam::K::ConstInt); z->int_val = n; return z; }
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
    std::string run;
    size_t i = 0, n = s.size();
    while (i < n) {
      char ch = s[i];
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
  LamPtr compile_match(const LamPtr& scrut, const std::vector<Case>& cases) {
    // A non-variable scrutinee with a `| n -> ...` catch-all is bound to n first
    // (`let n = scrut in ...`), so n refers to it inside the arms (matches ocamlc).
    if (scrut->k != Lam::K::Var)
      for (auto& c : cases)
        if (!c.guard)
          if (auto* pv = std::get_if<Ppat_var>(&c.lhs.desc)) {
            Ident nid = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = nid;
            auto v = mk(Lam::K::Var); v->var = nid;
            auto body = compile_match(v, cases);
            auto l = mk(Lam::K::Let); l->bindings = {{nid, ValueKind::Gen, scrut}}; l->body = body;
            return l;
          }
    if (cases.size() == 2 && !cases[0].guard && !cases[1].guard) {
      std::string a = ctor_of(cases[0].lhs), b = ctor_of(cases[1].lhs);
      if ((a == "true" && b == "false") || (a == "false" && b == "true")) {
        auto i = mk(Lam::K::IfThenElse);
        i->cond = scrut;
        i->then_ = expr(*(a == "true" ? cases[0] : cases[1]).rhs);
        i->else_ = expr(*(a == "false" ? cases[0] : cases[1]).rhs);
        return i;
      }
    }
    if (auto sw = const_switch(scrut, cases)) return sw;
    return int_cases(scrut, cases, 0);
  }

  // Exhaustive match over a purely-constant variant type -> (switch* ...).
  LamPtr const_switch(const LamPtr& scrut, const std::vector<Case>& cases) {
    std::string type;
    std::vector<Lam::SwitchCase> arms;
    for (auto& c : cases) {
      if (c.guard) return nullptr;
      auto it = ctor_info_.find(ctor_of(c.lhs));
      if (it == ctor_info_.end() || it->second.is_block) return nullptr;
      auto* k = std::get_if<Ppat_construct>(&c.lhs.desc);
      if (k && k->arg) return nullptr;  // constant ctor must take no argument
      if (type.empty()) type = it->second.type;
      else if (type != it->second.type) return nullptr;
      arms.push_back({it->second.tag, expr(*c.rhs)});
    }
    auto t = type_ctors_.find(type);
    if (t == type_ctors_.end() || t->second.second != 0 ||
        (int)arms.size() != t->second.first)
      return nullptr;  // not exhaustive over a constant-only type
    std::sort(arms.begin(), arms.end(),
              [](auto& x, auto& y) { return x.tag < y.tag; });
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
  LamPtr int_cases(const LamPtr& scrut, const std::vector<Case>& cases, size_t i) {
    if (i >= cases.size()) return cint(0);
    const Case& c = cases[i];
    if (!c.guard && (is_catchall(c.lhs) || i + 1 == cases.size())) {
      bind_catchall(c.lhs, scrut);
      return expr(*c.rhs);
    }
    if (!c.guard)
      if (auto* pc = std::get_if<Ppat_constant>(&c.lhs.desc))
        if (auto* pi = std::get_if<Pconst_integer>(&pc->c.desc)) {
          auto ne = mk(Lam::K::Prim); ne->prim = Prim::NotEqInt;
          ne->args = {scrut, cint(std::stoll(pi->value))};
          auto iff = mk(Lam::K::IfThenElse);
          iff->cond = ne; iff->then_ = int_cases(scrut, cases, i + 1); iff->else_ = expr(*c.rhs);
          return iff;
        }
    return expr(*c.rhs);  // unsupported pattern: best-effort
  }

  LamPtr expr(const Expression& e) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      // A string literal the inferencer typed at a format type lowers to a
      // CamlinternalFormatBasics format value, not a plain string.
      if (auto* s = std::get_if<Pconst_string>(&c->c.desc); s && vk.format_lits.count(&e))
        if (auto fv = format_value(s->s)) return fv;
      return translate_const(c->c);
    }
    if (auto* m = std::get_if<Pexp_match>(&e.desc)) return compile_match(expr(*m->e), m->cases);
    if (auto* tu = std::get_if<Pexp_tuple>(&e.desc)) {
      std::vector<LamPtr> es;
      for (auto& el : tu->elems) es.push_back(expr(*el));
      return block(0, std::move(es));
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
              if (!rt->second.mut) return block(0, std::move(vals));
              auto m = mk(Lam::K::Prim);  // any mutable field -> makemutable
              m->prim = Prim::Makemutable; m->prim_arg = 0;
              m->blk_shape = rt->second.shape; m->args = std::move(vals);
              return m;
            }
          }
        }
    }
    if (auto* ar = std::get_if<Pexp_array>(&e.desc)) {  // [| ... |] -> makearray[k]
      ValueKind k = ar->elems.empty() ? ValueKind::Gen : expr_kind(ar->elems[0].get());
      auto m = mk(Lam::K::Prim); m->prim = Prim::IntCmp;
      m->prim_id = "makearray[" + array_kind(k) + "]";
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
    }
    if (auto* sf = std::get_if<Pexp_setfield>(&e.desc)) {
      if (auto* fi = find_field(lid_last(sf->field.txt))) {
        auto l = mk(Lam::K::Prim);
        l->prim = fi->kind == ValueKind::Int ? Prim::SetfieldImm : Prim::SetfieldPtr;
        l->prim_arg = fi->index; l->args = {expr(*sf->obj), expr(*sf->value)};
        return l;
      }
    }
    if (auto* k = std::get_if<Pexp_construct>(&e.desc)) {
      std::string n = lid_last(k->id.txt);
      if (n == "[]" || n == "None" || n == "false" || n == "()") { auto z = mk(Lam::K::ConstInt); z->int_val = 0; return z; }
      if (n == "true") { auto z = mk(Lam::K::ConstInt); z->int_val = 1; return z; }
      if (n == "::" && k->arg) {  // a :: b : block tag 0 of (head, tail)
        if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc); at && at->elems.size() == 2)
          return block(0, {expr(*at->elems[0]), expr(*at->elems[1])});
      }
      if (n == "Some" && k->arg) return block(0, {expr(**k->arg)});
      if (auto ci = ctor_info_.find(n); ci != ctor_info_.end()) {  // local variant ctor
        if (!ci->second.is_block) return cint(ci->second.tag);  // constant -> its tag
        std::vector<LamPtr> fs;
        if (k->arg) {  // `B of t1 * t2` flattens the tuple argument into fields
          if (auto* at = std::get_if<Pexp_tuple>(&(*k->arg)->desc);
              at && ci->second.arity > 1 && (int)at->elems.size() == ci->second.arity)
            for (auto& el : at->elems) fs.push_back(expr(*el));
          else
            fs.push_back(expr(**k->arg));
        }
        return block(ci->second.tag, std::move(fs));
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
      }
      if (auto* d = std::get_if<Ldot>(&id->id.txt.v))
        if (auto* pl = std::get_if<Lident>(&d->prefix->v)) {
          // Qualified M.x where M is a local submodule: field of its block.
          if (auto mi = module_ident_.find(pl->name); mi != module_ident_.end()) {
            auto& lay = module_layout_[pl->name];
            if (auto f = lay.find(d->name); f != lay.end()) {
              auto v = mk(Lam::K::Var); v->var = mi->second;
              auto fi = mk(Lam::K::Prim); fi->prim = Prim::FieldImm;
              fi->prim_arg = f->second; fi->args = {v};
              return fi;
            }
          }
          // Qualified M.x where M is a stdlib (sub)module: field of Stdlib[__M].
          auto& fm = fields_of(pl->name);
          auto sf = fm.find(d->name);
          if (sf != fm.end()) return field_of(global_of(pl->name), sf->second);
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
            if (m == "Array" && f == "get" && as.size() == 2)
              op = "array.get[" + array_kind(expr_kind(&e)) + "]";
            else if (m == "Array" && f == "unsafe_get" && as.size() == 2)
              op = "array.unsafe_get[" + array_kind(expr_kind(&e)) + "]";
            else if (m == "Array" && f == "set" && as.size() == 3)
              op = "array.set[" + array_kind(expr_kind(as[2].second.get())) + "]";
            else if (m == "Array" && f == "unsafe_set" && as.size() == 3)
              op = "array.unsafe_set[" + array_kind(expr_kind(as[2].second.get())) + "]";
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
            if (auto prim = value_prim(m, f); !prim.empty())
              if (auto r = prim_to_lam(prim, *ap, e)) return r;
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
              auto pr = mk(Lam::K::Prim); pr->prim = Prim::Raise;
              pr->args = {expr(*as[0].second)};
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
              bool isint = expr_kind(as[0].second.get()) == ValueKind::Int ||
                           expr_kind(as[1].second.get()) == ValueKind::Int;
              auto pr = mk(Lam::K::Prim);
              if (isint) { pr->prim = Prim::IntCmp; pr->prim_id = c.first; }
              else { pr->prim = Prim::Ccall; pr->prim_id = c.second; }
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
            // General: an unqualified pervasive `external` (e.g. compare, ignore)
            // resolved via its cmi prim_name.
            if (auto pi = stdlib_prims.find(n); pi != stdlib_prims.end())
              if (auto r = prim_to_lam(pi->second, *ap, e)) return r;
          }
      auto a = mk(Lam::K::Apply);
      a->fn = expr(*ap->fn);
      for (auto& [lbl, arg] : ap->args) a->args.push_back(expr(*arg));
      return a;
    }
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) return function(*f);
    if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      // `let _ = e in body` discards e -> seq, not a binding.
      if (le->bindings.size() == 1 &&
          std::holds_alternative<Ppat_any>(le->bindings[0].pat.desc)) {
        auto sq = mk(Lam::K::Sequence);
        sq->cond = expr(*le->bindings[0].expr);
        sq->else_ = expr(*le->body);
        return sq;
      }
      scope.emplace_back();
      if (le->rf == RecFlag::Recursive) {  // names in scope within their RHSs
        auto l = mk(Lam::K::Letrec);
        std::vector<std::pair<const ValueBinding*, Ident>> recs;
        for (auto& b : le->bindings)
          if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
            Ident id = fresh(pv->name.txt);
            scope.back()[pv->name.txt] = id;
            recs.push_back({&b, id});
          }
        for (auto& [b, id] : recs)
          l->bindings.push_back({id, pat_kind(&b->pat), expr(*b->expr)});
        l->body = expr(*le->body);
        scope.pop_back();
        return l;
      }
      auto l = mk(Lam::K::Let);
      for (auto& b : le->bindings) {
        if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
          Ident id = fresh(pv->name.txt);
          Lam::Binding bd{id, pat_kind(&b.pat), expr(*b.expr)};
          l->bindings.push_back(std::move(bd));
          scope.back()[pv->name.txt] = id;
        } else {  // `let () = e in ...` and other refutable patterns: *match* temp
          l->bindings.push_back({fresh("", true), ValueKind::Gen, expr(*b.expr)});
        }
      }
      l->body = expr(*le->body);
      scope.pop_back();
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
      l->var = fresh("exn");
      l->then_ = exn_dispatch(l->var, tr->cases, 0);
      scope.pop_back();
      return l;
    }
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) return expr(*ct->e);
    return mk(Lam::K::ConstInt);  // unsupported: placeholder (will DIFF)
  }

  LamPtr function(const Pexp_function& f) {
    scope.emplace_back();
    auto l = mk(Lam::K::Function);
    for (auto& fp : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc)) {
        // Every parameter gets a binder; a non-variable pattern (`()`, `_`, a
        // tuple) is named "param" like ocamlc and matched in the body (deferred).
        const Pattern* pat = &pv->pat;
        while (auto* pc = std::get_if<Ppat_constraint>(&pat->desc)) pat = pc->p.get();
        if (auto* var = std::get_if<Ppat_var>(&pat->desc)) {
          Ident id = fresh(var->name.txt);
          l->params.push_back({id, pat_kind(pat)});
          scope.back()[var->name.txt] = id;
        } else {
          l->params.push_back({fresh("param"), pat_kind(pat)});
        }
      }
    auto rk = vk.fn_ret.find(&f);
    l->ret_kind = rk == vk.fn_ret.end() ? ValueKind::Gen : vkind(rk->second);
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) {
      l->body = expr(*fb->e);
    } else if (auto* fc = std::get_if<Pfunction_cases>(&f.body->v)) {
      // `function P -> ...` adds an implicit final parameter matched on.
      Ident pid = fresh("param");
      l->params.push_back({pid, ValueKind::Gen});
      auto scrut = mk(Lam::K::Var); scrut->var = pid;
      l->body = compile_match(scrut, fc->cases);
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
  LamPtr build_module(const Structure& s, std::vector<std::string>* names) {
    scope.emplace_back();
    struct Seg { bool seq; bool rec_; std::vector<Lam::Binding> binds; LamPtr e; };
    std::vector<Seg> segs;
    std::vector<Lam::Binding> cur;
    std::vector<LamPtr> exports;
    auto flush = [&] { if (!cur.empty()) segs.push_back({false, false, std::move(cur), nullptr}), cur.clear(); };
    auto add_export = [&](const std::string& nm, const Ident& id) {
      auto v = mk(Lam::K::Var); v->var = id; exports.push_back(v);
      if (names) names->push_back(nm);
    };
    for (auto& it : s) {
      if (auto* pe = std::get_if<Pstr_eval>(&it.desc)) {  // bare `e;;` -> seq
        flush(); segs.push_back({true, false, {}, expr(*pe->e)}); continue;
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
      if (auto* pm = std::get_if<Pstr_module>(&it.desc)) {  // module M = struct ... end
        auto& mb = pm->binding;
        if (mb.name.txt)
          if (auto* ps = std::get_if<Pmod_structure>(&mb.expr.desc)) {
            std::vector<std::string> sub;
            std::string saved = mod_path_;
            mod_path_ += "." + *mb.name.txt;  // nested exceptions are "Outer.M.E"
            LamPtr body = build_module(ps->items, &sub);
            mod_path_ = saved;
            Ident mid = fresh(*mb.name.txt);
            cur.push_back({mid, ValueKind::Gen, body});
            module_ident_[*mb.name.txt] = mid;
            auto& lay = module_layout_[*mb.name.txt]; lay.clear();
            for (int i = 0; i < (int)sub.size(); ++i) lay[sub[i]] = i;
            add_export(*mb.name.txt, mid);
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
        std::vector<Lam::Binding> binds;
        for (auto& [b, id] : recs) {
          binds.push_back({id, pat_kind(&b->pat), expr(*b->expr)});
          add_export(std::get_if<Ppat_var>(&b->pat.desc)->name.txt, id);
        }
        segs.push_back({false, true, std::move(binds), nullptr});
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
          cur.push_back({id, pat_kind(&b.pat), expr(*b.expr)});
          scope.back()[pv->name.txt] = id;
          add_export(pv->name.txt, id);
        } else if (std::holds_alternative<Ppat_any>(b.pat.desc)) {
          flush(); segs.push_back({true, false, {}, expr(*b.expr)});  // `let _ = e` -> seq
        } else {  // `let () = e` and other refutable patterns: a *match* temp
          cur.push_back({fresh("", true), ValueKind::Gen, expr(*b.expr)});
        }
      }
    }
    flush();
    auto block = mk(Lam::K::Prim);
    block->prim = Prim::Makeblock; block->prim_arg = 0; block->args = std::move(exports);
    LamPtr acc = block;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
      if (it->seq) { auto sq = mk(Lam::K::Sequence); sq->cond = it->e; sq->else_ = acc; acc = sq; }
      else {
        auto l = mk(it->rec_ ? Lam::K::Letrec : Lam::K::Let);
        l->bindings = std::move(it->binds); l->body = acc; acc = l;
      }
    }
    scope.pop_back();
    return acc;
  }
};

}  // namespace

LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name,
                                const std::string& stdlib_dir) {
  Translator t;
  t.stdlib_dir = stdlib_dir;
  t.vk = infer_value_kinds(s);
  t.register_types(s);
  try {  // Stdlib value -> module field index, for pervasive resolution
    auto cmi = cmi::CmiFile::load(stdlib_dir + "/stdlib.cmi");
    int i = 0;
    for (auto& f : cmi.sig().fields) t.stdlib_fields[f] = i++;
    for (auto& v : cmi.values()) if (!v.prim.empty()) t.stdlib_prims[v.name] = v.prim;
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
