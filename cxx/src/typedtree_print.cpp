// Render the typedtree in `ocamlc -dtypedtree` format (printtyped.ml).
// Mirrors that file's 2*depth indentation and node spellings exactly.  Differs
// from printast in two ways that matter: locations print the filename on BOTH
// ends, and typed constants are Const_int/Const_string/... (converted here from
// the parsed ast::Constant).
#include "cppcaml/typedtree.hpp"

#include <cstdio>
#include <string>

namespace cppcaml::typedtree {
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
        if (c < 32 || c >= 127) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03d", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  o += '"';
  return o;
}

// Parse an OCaml integer literal (allowing _ separators and 0x/0o/0b bases) to
// its decimal value, matching how the typer prints Const_int.
long long parse_int(const std::string& lit) {
  std::string d;
  for (char c : lit)
    if (c != '_') d += c;
  int base = 10;
  size_t i = 0;
  bool neg = false;
  if (i < d.size() && (d[i] == '+' || d[i] == '-')) { neg = d[i] == '-'; ++i; }
  if (i + 1 < d.size() && d[i] == '0') {
    char b = d[i + 1];
    if (b == 'x' || b == 'X') { base = 16; i += 2; }
    else if (b == 'o' || b == 'O') { base = 8; i += 2; }
    else if (b == 'b' || b == 'B') { base = 2; i += 2; }
  }
  long long v = static_cast<long long>(
      std::strtoull(d.c_str() + i, nullptr, base));
  return neg ? -v : v;
}

struct Printer {
  std::ostream& os;
  std::string fname;
  std::vector<std::string> dirfiles;

  void line(int i, const std::string& s) { os << std::string(2 * i, ' ') << s << '\n'; }

  const std::string& file_of(const ast::Position& p) const {
    if (p.file_id > 0 && p.file_id <= static_cast<int>(dirfiles.size()))
      return dirfiles[p.file_id - 1];
    return fname;
  }
  // printtyped fmt_position: filename on every position.
  std::string pos(const ast::Position& p) const {
    if (p.cnum == -1) return "_none_[0]";
    return file_of(p) + '[' + std::to_string(p.lnum) + ',' +
           std::to_string(p.bol) + '+' + std::to_string(p.cnum - p.bol) + ']';
  }
  std::string loc(const Location& l) const {
    std::string s = '(' + pos(l.start) + ".." + pos(l.end) + ')';
    if (l.ghost) s += " ghost";
    return s;
  }

  std::string ident(const Ident& id) const {
    if (id.global) return id.name + "!";
    return id.name + "/" + std::to_string(id.stamp);
  }
  std::string path_aux(const Path& p) const {
    if (auto* pi = std::get_if<Pident>(&p.v)) return ident(pi->id);
    auto& d = std::get<Pdot>(p.v);
    return path_aux(*d.prefix) + "." + d.name;
  }

  void constant(int i, const std::string& prefix, const Constant& c) {
    if (auto* p = std::get_if<ast::Pconst_integer>(&c.desc)) {
      const char* tag = "Const_int";
      if (p->suffix == 'l') tag = "Const_int32";
      else if (p->suffix == 'L') tag = "Const_int64";
      else if (p->suffix == 'n') tag = "Const_nativeint";
      line(i, prefix + tag + " " + std::to_string(parse_int(p->value)));
    } else if (auto* p = std::get_if<ast::Pconst_char>(&c.desc)) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "%02x", p->code & 0xff);
      line(i, prefix + "Const_char " + buf);
    } else if (auto* p = std::get_if<ast::Pconst_string>(&c.desc)) {
      if (!p->delim)
        line(i, prefix + "Const_string(" + ocaml_escape(p->s) + "," +
                    loc(p->strloc) + ",None)");
      else
        line(i, prefix + "Const_string (" + ocaml_escape(p->s) + "," +
                    loc(p->strloc) + ",Some " + ocaml_escape(*p->delim) + ")");
    } else {
      auto& f = std::get<ast::Pconst_float>(c.desc);
      line(i, prefix + "Const_float " + f.value);
    }
  }

  void arg_label(int i, const ArgLabel& l) {
    if (std::holds_alternative<ast::Nolabel>(l)) line(i, "Nolabel");
    else if (auto* p = std::get_if<ast::Labelled>(&l)) line(i, "Labelled \"" + p->name + "\"");
    else line(i, "Optional \"" + std::get<ast::Optional>(l).name + "\"");
  }

  void pattern(int i, const Pattern& p) {
    line(i, "pattern " + loc(p.loc));
    int j = i + 1;
    if (std::holds_alternative<Tpat_any>(p.desc)) line(j, "Tpat_any");
    else line(j, "Tpat_var \"" + ident(std::get<Tpat_var>(p.desc).id) + "\"");
  }

  void expression(int i, const Expression& e) {
    line(i, "expression " + loc(e.loc));
    int j = i + 1;
    if (auto* c = std::get_if<Texp_constant>(&e.desc)) {
      constant(j, "Texp_constant ", c->c);
    } else if (auto* id = std::get_if<Texp_ident>(&e.desc)) {
      line(j, "Texp_ident \"" + path_aux(id->path) + "\"");
    } else if (auto* t = std::get_if<Texp_tuple>(&e.desc)) {
      line(j, "Texp_tuple");
      if (t->elems.empty()) { line(j, "[]"); return; }
      line(j, "[");
      for (auto& [label, ex] : t->elems) {
        line(j + 1, label ? "Label: Some \"" + *label + "\"" : "Label: None");
        expression(j + 2, *ex);
      }
      line(j, "]");
    } else if (auto* a = std::get_if<Texp_apply>(&e.desc)) {
      line(j, "Texp_apply");
      expression(j, *a->fn);
      if (a->args.empty()) { line(j, "[]"); return; }
      line(j, "[");
      for (auto& [label, ex] : a->args) {
        line(j + 1, "<arg>");
        arg_label(j + 2, label);
        expression(j + 2, *ex);
      }
      line(j, "]");
    } else {
      auto& fn = std::get<Texp_function>(e.desc);
      line(j, "Texp_function");
      if (fn.params.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& p : fn.params) {
          arg_label(j + 1, p.label);
          line(j + 1, "Param_pat");
          pattern(j + 2, *p.pat);
        }
        line(j, "]");
      }
      line(j, "Tfunction_body");
      expression(j + 1, *fn.body);
    }
  }

  void value_binding(int i, RecFlag rf, const ValueBinding& vb) {
    line(i, rf == RecFlag::Nonrecursive ? "<def>" : "<def_rec>");
    pattern(i + 1, vb.pat);
    expression(i + 1, vb.expr);
  }

  void structure_item(int i, const StructureItem& it) {
    line(i, "structure_item " + loc(it.loc));
    int j = i + 1;
    auto& sv = std::get<Tstr_value>(it.desc);
    line(j, std::string("Tstr_value ") +
                (sv.rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
    if (sv.bindings.empty()) { line(j, "[]"); return; }
    line(j, "[");
    for (auto& vb : sv.bindings) value_binding(j + 1, sv.rf, vb);
    line(j, "]");
  }

  void structure(const Structure& s) {
    if (s.empty()) { line(0, "[]"); return; }
    line(0, "[");
    for (auto& it : s) structure_item(1, it);
    line(0, "]");
  }
};

}  // namespace

void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles) {
  Printer p{os, std::string(fname), dirfiles};
  p.structure(s);
}

}  // namespace cppcaml::typedtree
