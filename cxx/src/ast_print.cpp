// -dparsetree printer: reproduces parsing/printast.ml byte-for-byte so the C++
// parser's output diffs cleanly against `ocamlc -dparsetree`.
#include <ostream>
#include <string>
#include <string_view>

#include "cppcaml/ast.hpp"

namespace cppcaml::ast {
namespace {

// OCaml %S: quote + escape like String.escaped (also escapes '"').
std::string ocaml_escape(std::string_view s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      case '\b': o += "\\b"; break;
      default:
        if (c >= 0x20 && c < 0x7f) {
          o += static_cast<char>(c);
        } else {
          char buf[5];
          std::snprintf(buf, sizeof buf, "\\%03d", c);
          o += buf;
        }
    }
  }
  o += '"';
  return o;
}

struct Printer {
  std::ostream& os;
  std::string fname;

  std::string ind(int i) const { return std::string((2 * i) % 72, ' '); }

  std::string pos(const Position& p, bool with_name) const {
    std::string s = with_name ? fname : "";
    s += '[' + std::to_string(p.lnum) + ',' + std::to_string(p.bol) + '+' +
         std::to_string(p.cnum - p.bol) + ']';
    return s;
  }
  std::string loc(const Location& l) const {
    std::string s = '(' + pos(l.start, true) + ".." + pos(l.end, false) + ')';
    if (l.ghost) s += " ghost";
    return s;
  }

  std::string lid_aux(const Longident& x) const {
    if (auto* p = std::get_if<Lident>(&x.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&x.v)) return lid_aux(*p->prefix) + '.' + p->name;
    auto& a = std::get<Lapply>(x.v);
    return lid_aux(*a.f) + '(' + lid_aux(*a.x) + ')';
  }
  std::string lid_loc(const LongidentLoc& x) const {
    return '"' + lid_aux(x.txt) + "\" " + loc(x.loc);
  }
  std::string str_loc(const StringLoc& x) const {
    return '"' + x.txt + "\" " + loc(x.loc);
  }
  std::string char_opt(const std::optional<char>& c) const {
    return c ? std::string("Some ") + *c : "None";
  }

  void line(int i, const std::string& s) { os << ind(i) << s << '\n'; }

  void constant(int i, const Constant& c) {
    line(i, "constant " + loc(c.loc));
    int j = i + 1;
    if (auto* p = std::get_if<Pconst_integer>(&c.desc))
      line(j, "PConst_int (" + p->value + "," + char_opt(p->suffix) + ")");
    else if (auto* p = std::get_if<Pconst_char>(&c.desc)) {
      char buf[3];
      std::snprintf(buf, sizeof buf, "%02x", p->code & 0xff);
      line(j, std::string("PConst_char ") + buf);
    } else if (auto* p = std::get_if<Pconst_string>(&c.desc)) {
      if (!p->delim)
        line(j, "PConst_string(" + ocaml_escape(p->s) + "," + loc(p->strloc) + ",None)");
      else
        line(j, "PConst_string (" + ocaml_escape(p->s) + "," + loc(p->strloc) +
                    ",Some " + ocaml_escape(*p->delim) + ")");
    } else {
      auto& pf = std::get<Pconst_float>(c.desc);
      line(j, "PConst_float (" + pf.value + "," + char_opt(pf.suffix) + ")");
    }
  }

  void arg_label(int i, const ArgLabel& l) {
    if (std::holds_alternative<Nolabel>(l)) line(i, "Nolabel");
    else if (auto* p = std::get_if<Labelled>(&l)) line(i, "Labelled \"" + p->name + "\"");
    else line(i, "Optional \"" + std::get<Optional>(l).name + "\"");
  }

  void pattern(int i, const Pattern& p) {
    line(i, "pattern " + loc(p.loc));
    int j = i + 1;
    if (std::holds_alternative<Ppat_any>(p.desc)) line(j, "Ppat_any");
    else if (auto* v = std::get_if<Ppat_var>(&p.desc))
      line(j, "Ppat_var " + str_loc(v->name));
    else if (auto* v = std::get_if<Ppat_constant>(&p.desc)) {
      line(j, "Ppat_constant");
      constant(j, v->c);
    }
  }

  void expression(int i, const Expression& e) {
    line(i, "expression " + loc(e.loc));
    int j = i + 1;
    if (auto* v = std::get_if<Pexp_ident>(&e.desc))
      line(j, "Pexp_ident " + lid_loc(v->id));
    else if (auto* v = std::get_if<Pexp_constant>(&e.desc)) {
      line(j, "Pexp_constant");
      constant(j, v->c);
    } else if (auto* v = std::get_if<Pexp_apply>(&e.desc)) {
      line(j, "Pexp_apply");
      expression(j, *v->fn);
      if (v->args.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& [lbl, arg] : v->args) {
          line(j + 1, "<arg>");
          arg_label(j + 1, lbl);
          expression(j + 2, *arg);
        }
        line(j, "]");
      }
    } else if (auto* v = std::get_if<Pexp_let>(&e.desc)) {
      line(j, std::string("Pexp_let ") + (v->rf == RecFlag::Recursive ? "Rec" : "Nonrec"));
      value_bindings(j, v->bindings);
      expression(j, *v->body);
    } else if (auto* v = std::get_if<Pexp_function>(&e.desc)) {
      line(j, "Pexp_function");
      if (v->params.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& fp : v->params) function_param(j + 1, fp);
        line(j, "]");
      }
      line(j, "None");  // type constraint (always None in fragment)
      function_body(j, *v->body);
    } else if (auto* v = std::get_if<Pexp_tuple>(&e.desc)) {
      line(j, "Pexp_tuple");
      line(j, "[");
      for (auto& el : v->elems) {
        line(j + 1, "None");  // labeled_tuple_element: label option
        expression(j + 1, *el);
      }
      line(j, "]");
    } else if (auto* v = std::get_if<Pexp_ifthenelse>(&e.desc)) {
      line(j, "Pexp_ifthenelse");
      expression(j, *v->cond);
      expression(j, *v->then_);
      if (v->else_) { line(j, "Some"); expression(j + 1, **v->else_); }
      else line(j, "None");
    }
  }

  void function_param(int i, const FunctionParam& fp) {
    auto& pv = std::get<Pparam_val>(fp.desc);
    line(i, "Pparam_val " + loc(pv.loc));
    arg_label(i + 1, pv.label);
    line(i + 1, "None");  // default expr (always None in fragment)
    pattern(i + 1, pv.pat);
  }

  void function_body(int i, const FunctionBody& b) {
    auto& fb = std::get<Pfunction_body>(b.v);
    line(i, "Pfunction_body");
    expression(i + 1, *fb.e);
  }

  void value_bindings(int i, const std::vector<ValueBinding>& l) {
    if (l.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& vb : l) {
      // list calls value_binding at i+1; <def> at i+1, its children at i+2.
      line(i + 1, "<def>");
      pattern(i + 2, vb.pat);
      expression(i + 2, *vb.expr);
    }
    line(i, "]");
  }

  void structure_item(int i, const StructureItem& s) {
    line(i, "structure_item " + loc(s.loc));
    int j = i + 1;
    if (auto* v = std::get_if<Pstr_eval>(&s.desc)) {
      line(j, "Pstr_eval");
      expression(j, *v->e);
    } else if (auto* v = std::get_if<Pstr_value>(&s.desc)) {
      line(j, std::string("Pstr_value ") + (v->rf == RecFlag::Recursive ? "Rec" : "Nonrec"));
      value_bindings(j, v->bindings);
    }
  }

  void structure(const Structure& s) {
    if (s.empty()) { line(0, "[]"); return; }
    line(0, "[");
    for (auto& it : s) structure_item(1, it);
    line(0, "]");
  }
};

}  // namespace

void print_dparsetree(const Structure& s, std::string_view fname, std::ostream& os) {
  Printer p{os, std::string(fname)};
  p.structure(s);
  os << '\n';  // driver flushes the dump with a trailing newline (@.)
}

}  // namespace cppcaml::ast
