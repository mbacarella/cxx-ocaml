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
    if (p.cnum == -1) return "_none_[0,0+-1]";  // the compiler's "none" position
    return file_of(p) + '[' + std::to_string(p.lnum) + ',' +
           std::to_string(p.bol) + '+' + std::to_string(p.cnum - p.bol) + ']';
  }
  std::string loc(const Location& l) const {
    std::string s = '(' + pos(l.start) + ".." + pos(l.end) + ')';
    if (l.ghost) s += " ghost";
    return s;
  }

  std::string ident(const Ident& id) const {
    switch (id.kind) {
      case Ident::Global: return id.name + "!";
      case Ident::Predef: return id.name + "/" + std::to_string(id.stamp) + "!";
      default: return id.name + "/" + std::to_string(id.stamp);
    }
  }
  std::string path_aux(const Path& p) const {
    if (auto* pi = std::get_if<Pident>(&p.v)) return ident(pi->id);
    if (auto* ap = std::get_if<Papply>(&p.v))
      return path_aux(*ap->fn) + "(" + path_aux(*ap->arg) + ")";
    auto& d = std::get<Pdot>(p.v);
    return path_aux(*d.prefix) + "." + d.name;
  }

  void constant(int i, const std::string& prefix, const Constant& c) {
    if (auto* p = std::get_if<ast::Pconst_integer>(&c.desc)) {
      const char* tag = "Const_int";
      long long v = parse_int(p->value);
      if (p->suffix == 'l') { tag = "Const_int32"; v = (int32_t)v; }  // signed 32-bit
      else if (p->suffix == 'L') tag = "Const_int64";
      else if (p->suffix == 'n') tag = "Const_nativeint";
      line(i, prefix + tag + " " + std::to_string(v));
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

  void core_type(int i, const CoreType& t) {
    line(i, "core_type " + loc(t.loc));
    if (t.attrs) attributes(i, *t.attrs);
    int j = i + 1;
    if (std::holds_alternative<Ttyp_any>(t.desc)) {
      line(j, "Ttyp_any");
    } else if (auto* v = std::get_if<Ttyp_var>(&t.desc)) {
      line(j, "Ttyp_var " + v->name);
    } else if (auto* a = std::get_if<Ttyp_arrow>(&t.desc)) {
      line(j, "Ttyp_arrow");
      arg_label(j, a->label);
      core_type(j, *a->dom);
      core_type(j, *a->cod);
    } else if (auto* tu = std::get_if<Ttyp_tuple>(&t.desc)) {
      line(j, "Ttyp_tuple");
      if (tu->elems.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& [label, el] : tu->elems) {
          line(j + 1, label ? "Label: Some \"" + *label + "\"" : "Label: None");
          core_type(j + 1, *el);  // labeled_core_type: type at the Label's depth
        }
        line(j, "]");
      }
    } else if (auto* c = std::get_if<Ttyp_constr>(&t.desc)) {
      line(j, "Ttyp_constr \"" + path_aux(c->path) + "\"");
      list_core_types(j, c->args);
    } else if (auto* cl = std::get_if<Ttyp_class>(&t.desc)) {
      line(j, "Ttyp_class \"" + path_aux(cl->path) + "\"");
      list_core_types(j, cl->args);
    } else if (auto* op = std::get_if<Ttyp_open>(&t.desc)) {
      line(j, "Ttyp_open \"" + path_aux(op->path) + "\"");
      core_type(j, *op->type);
    } else if (auto* al = std::get_if<Ttyp_alias>(&t.desc)) {
      line(j, "Ttyp_alias \"" + al->name + "\"");
      core_type(j, *al->type);
    } else if (auto* vr = std::get_if<Ttyp_variant>(&t.desc)) {
      line(j, std::string("Ttyp_variant closed=") + (vr->closed ? "Closed" : "Open"));
      if (vr->rows.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& r : vr->rows) {
          if (auto* tg = std::get_if<Ttag>(&r.v)) {
            line(j + 1, "Ttag \"" + tg->name + "\" " + (tg->constant ? "true" : "false"));
            list_core_types(j + 2, tg->types);
          } else {
            line(j + 1, "Tinherit");
            core_type(j + 2, *std::get<Tinherit>(r.v).type);
          }
        }
        line(j, "]");
      }
      if (vr->labels) { line(j, "Some"); list_strings(j + 1, *vr->labels); }
      else line(j, "None");
    } else if (auto* ob = std::get_if<Ttyp_object>(&t.desc)) {
      line(j, std::string("Ttyp_object ") + (ob->closed ? "Closed" : "Open"));
      for (auto& f : ob->fields) {
        if (auto* m = std::get_if<OTmethod>(&f.v)) {
          line(j + 1, "method " + m->name);
          core_type(j + 2, *m->type);
        } else {
          line(j + 1, "OTinherit");
          core_type(j + 2, *std::get<OTinherit>(f.v).type);
        }
      }
    } else if (auto* pk = std::get_if<Ttyp_package>(&t.desc)) {
      line(j, "Ttyp_package");
      package_type(j, pk->pkg);
    } else {
      auto& p = std::get<Ttyp_poly>(t.desc);
      std::string s = "Ttyp_poly";
      for (auto& v : p.vars) s += " '" + v;
      line(j, s);
      core_type(j, *p.type);
    }
  }

  void list_core_types(int i, const std::vector<CoreTypeBox>& ts) {
    if (ts.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& t : ts) core_type(i + 1, *t);
    line(i, "]");
  }

  void constructor_decl(int i, const ConstructorDecl& cd) {
    line(i, loc(cd.loc));
    line(i + 1, ident(cd.id));
    if (cd.attrs) attributes(i, *cd.attrs);
    if (!cd.labels.empty()) {  // inline record: label decls instead of types
      line(i + 1, "[");
      for (auto& l : cd.labels) label_decl(i + 2, l);
      line(i + 1, "]");
    } else {
      list_core_types(i + 1, cd.args);
    }
    if (cd.res) { line(i + 1, "Some"); core_type(i + 2, **cd.res); }
    else line(i + 1, "None");
  }

  void label_decl(int i, const LabelDecl& ld) {
    line(i, loc(ld.loc));
    if (ld.attrs) attributes(i, *ld.attrs);
    line(i + 1, ld.mutable_ ? "Mutable" : "Immutable");
    line(i + 1, ld.atomic ? "Atomic" : "Nonatomic");
    os << std::string(2 * (i + 1), ' ') << ident(ld.id);  // run-on (no newline)
    core_type(i + 1, ld.type);
  }

  void type_kind(int i, const TypeKind& k) {
    if (std::holds_alternative<Ttype_abstract>(k.v)) {
      line(i, "Ttype_abstract");
    } else if (auto* v = std::get_if<Ttype_variant>(&k.v)) {
      line(i, "Ttype_variant");
      if (v->ctors.empty()) line(i + 1, "[]");
      else {
        line(i + 1, "[");
        for (auto& c : v->ctors) constructor_decl(i + 2, c);
        line(i + 1, "]");
      }
    } else if (auto* r = std::get_if<Ttype_record>(&k.v)) {
      line(i, "Ttype_record");
      if (r->labels.empty()) line(i + 1, "[]");
      else {
        line(i + 1, "[");
        for (auto& l : r->labels) label_decl(i + 2, l);
        line(i + 1, "]");
      }
    } else {
      line(i, "Ttype_open");
    }
  }

  void type_declaration(int i, const TypeDeclaration& td) {
    line(i, "type_declaration " + ident(td.id) + " " + loc(td.loc));
    if (td.attrs) attributes(i, *td.attrs);
    int j = i + 1;
    line(j, "ptype_params =");
    list_core_types(j + 1, td.params);
    line(j, "ptype_constraints =");
    if (td.constraints.empty()) line(j + 1, "[]");
    else {
      line(j + 1, "[");
      for (auto& c : td.constraints) {
        line(j + 2, "<constraint> " + loc(c.loc));
        core_type(j + 3, *c.t1);
        core_type(j + 3, *c.t2);
      }
      line(j + 1, "]");
    }
    line(j, "ptype_kind =");
    type_kind(j + 1, td.kind);
    line(j, std::string("ptype_private = ") + (td.private_ ? "Private" : "Public"));
    line(j, "ptype_manifest =");
    if (td.manifest) { line(j + 1, "Some"); core_type(j + 2, **td.manifest); }
    else line(j + 1, "None");
  }

  void extension_constructor(int i, const ExtCtor& c) {
    line(i, "extension_constructor " + loc(c.loc));
    if (c.attrs) attributes(i, *c.attrs);
    line(i + 1, "pext_name = \"" + ident(c.id) + "\"");
    line(i + 1, "pext_kind =");
    if (c.rebind) {
      line(i + 2, "Text_rebind");
      line(i + 3, "\"" + path_aux(*c.rebind) + "\"");
      return;
    }
    line(i + 2, "Text_decl");
    if (!c.labels.empty()) {  // inline record: label decls instead of types
      line(i + 3, "[");
      for (auto& l : c.labels) label_decl(i + 4, l);
      line(i + 3, "]");
    } else {
      list_core_types(i + 3, c.args);
    }
    if (c.res) { line(i + 3, "Some"); core_type(i + 4, **c.res); }
    else line(i + 3, "None");
  }

  void list_strings(int i, const std::vector<std::string>& ss) {
    if (ss.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& s : ss) line(i + 1, ocaml_escape(s));
    line(i, "]");
  }

  // Mirrors printast/printtyped `attributes i`: each attribute prints at i+1,
  // its payload at i+2.  Payloads are parsetree, printed the printast way.
  void attributes(int i, const ast::Attributes& attrs) {
    for (auto& a : attrs) {
      line(i + 1, "attribute \"" + a.name + "\"");
      ast::print_attribute_payload(a, i + 2, os, fname, dirfiles);
    }
  }

  void pattern(int i, const Pattern& p) {
    line(i, "pattern " + loc(p.loc));
    if (p.attrs) attributes(i, *p.attrs);
    int j = i + 1;
    for (auto& ex : p.extras) {
      line(j, "extra " + loc(ex.loc));
      if (ex.kind == PatExtra::Kind::Unpack) {
        line(j + 1, "Tpat_extra_unpack");
        if (ex.pkg) { line(j + 1, "Some"); package_type(j + 2, *ex.pkg); }
        else line(j + 1, "None");
      } else if (ex.kind == PatExtra::Kind::Type) {
        line(j + 1, "Tpat_extra_type \"" + path_aux(ex.type_path) + "\"");
      } else if (ex.kind == PatExtra::Kind::Open) {
        line(j + 1, "Tpat_extra_open \"" + path_aux(ex.type_path) + "\"");
      } else {
        line(j + 1, "Tpat_extra_constraint");
        core_type(j + 1, ex.ctype);
      }
    }
    if (std::holds_alternative<Tpat_any>(p.desc)) {
      line(j, "Tpat_any");
    } else if (auto* v = std::get_if<Tpat_var>(&p.desc)) {
      line(j, "Tpat_var \"" + ident(v->id) + "\"");
    } else if (auto* c = std::get_if<Tpat_constant>(&p.desc)) {
      constant(j, "Tpat_constant ", c->c);
    } else if (auto* k = std::get_if<Tpat_construct>(&p.desc)) {
      line(j, "Tpat_construct \"" + k->name + "\"");
      if (k->args.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& a : k->args) pattern(j + 1, *a);
        line(j, "]");
      }
      line(j, "None");  // existential (vars, type) annotation: always None here
    } else if (auto* w = std::get_if<Tpat_value>(&p.desc)) {
      line(j, "Tpat_value");
      pattern(j, *w->inner);  // inner at same depth (printtyped: `pattern i`)
    } else if (auto* ex = std::get_if<Tpat_exception>(&p.desc)) {
      line(j, "Tpat_exception");
      pattern(j, *ex->inner);
    } else if (auto* o = std::get_if<Tpat_or>(&p.desc)) {
      line(j, "Tpat_or");
      pattern(j, *o->left);
      pattern(j, *o->right);
    } else if (auto* al = std::get_if<Tpat_alias>(&p.desc)) {
      line(j, "Tpat_alias \"" + ident(al->id) + "\"");
      pattern(j, *al->inner);
    } else if (auto* rc = std::get_if<Tpat_record>(&p.desc)) {
      line(j, "Tpat_record");
      line(j, "[");
      for (auto& [label, sub] : rc->fields) {
        line(j + 1, "\"" + label + "\"");
        pattern(j + 2, *sub);
      }
      line(j, "]");
    } else if (auto* ar = std::get_if<Tpat_array>(&p.desc)) {
      line(j, "Tpat_array Mutable");
      if (ar->elems.empty()) line(j, "[]");
      else { line(j, "["); for (auto& el : ar->elems) pattern(j + 1, *el); line(j, "]"); }
    } else if (auto* v = std::get_if<Tpat_variant>(&p.desc)) {
      line(j, "Tpat_variant \"" + v->label + "\"");
      if (v->arg) { line(j, "Some"); pattern(j + 1, *v->arg); }
      else line(j, "None");
    } else if (auto* lz = std::get_if<Tpat_lazy>(&p.desc)) {
      line(j, "Tpat_lazy");
      pattern(j, *lz->inner);
    } else {
      auto& tu = std::get<Tpat_tuple>(p.desc);
      line(j, "Tpat_tuple");
      if (tu.elems.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& [label, el] : tu.elems) {
          line(j + 1, label ? "Label: Some \"" + *label + "\"" : "Label: None");
          pattern(j + 1, *el);  // pattern at same depth as its Label
        }
        line(j, "]");
      }
    }
  }

  void case_(int i, const Case& c) {
    line(i, "<case>");
    pattern(i + 1, c.lhs);
    if (c.guard) {
      line(i + 1, "<when>");
      expression(i + 2, **c.guard);
    }
    expression(i + 1, *c.rhs);
  }

  void expr_extra(int j, const ExprExtra& ex) {
    line(j, "extra " + loc(ex.loc));
    if (ex.kind == ExprExtra::Kind::Coerce) {
      line(j + 1, "Texp_coerce");
      if (ex.from) { line(j + 1, "Some"); core_type(j + 2, *ex.from); }
      else line(j + 1, "None");
      core_type(j + 1, ex.ctype);
    } else if (ex.kind == ExprExtra::Kind::Poly) {
      line(j + 1, "Texp_poly");
      if (ex.poly_has_type) { line(j + 1, "Some"); core_type(j + 2, ex.ctype); }
      else line(j + 1, "None");
    } else if (ex.kind == ExprExtra::Kind::Newtype) {
      line(j + 1, "Texp_newtype \"" + ex.newtype + "\"");
    } else {
      line(j + 1, "Texp_constraint");
      core_type(j + 1, ex.ctype);
    }
  }
  void expression(int i, const Expression& e) {
    line(i, "expression " + loc(e.loc));
    if (e.attrs) attributes(i, *e.attrs);
    int j = i + 1;
    for (auto& ex : e.extras) expr_extra(j, ex);
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
        if (ex) expression(j + 2, *ex);  // an Omitted (eta) arg is the label alone
      }
      line(j, "]");
    } else if (auto* fn = std::get_if<Texp_function>(&e.desc)) {
      line(j, "Texp_function");
      if (fn->params.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& p : fn->params) {
          arg_label(j + 1, p.label);
          const char* part = p.partial ? " (Partial)" : "";
          if (p.default_) {
            line(j + 1, std::string("Param_optional_default") + part);
            pattern(j + 2, *p.pat);
            expression(j + 2, *p.default_);
          } else {
            line(j + 1, std::string("Param_pat") + part);
            pattern(j + 2, *p.pat);
          }
        }
        line(j, "]");
      }
      if (fn->is_cases) {
        line(j, std::string("Tfunction_cases") +
                    (fn->cases_partial ? " (Partial)" : "") + " " +
                    loc(fn->cases_loc));
        if (fn->cases_attrs) attributes(j + 1, *fn->cases_attrs);
        for (auto& ex : fn->cases_extras) expr_extra(j + 1, ex);
        list_cases(j + 1, fn->cases);
      } else {
        line(j, "Tfunction_body");
        expression(j + 1, *fn->body);
      }
    } else if (auto* l = std::get_if<Texp_let>(&e.desc)) {
      line(j, std::string("Texp_let ") +
                  (l->rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
      list_bindings(j, l->rf, l->bindings);
      expression(j, *l->body);
    } else if (auto* it = std::get_if<Texp_ifthenelse>(&e.desc)) {
      line(j, "Texp_ifthenelse");
      expression(j, *it->cond);
      expression(j, *it->then_);
      if (it->else_) { line(j, "Some"); expression(j + 1, **it->else_); }
      else line(j, "None");
    } else if (auto* s = std::get_if<Texp_sequence>(&e.desc)) {
      line(j, "Texp_sequence");
      expression(j, *s->e1);
      expression(j, *s->e2);
    } else if (auto* m = std::get_if<Texp_match>(&e.desc)) {
      line(j, m->partial ? "Texp_match (Partial)" : "Texp_match");
      expression(j, *m->scrut);
      list_cases(j, m->cases);
      if (m->eff_cases.empty()) line(j, "[]");
      else list_cases(j, m->eff_cases);
    } else if (auto* tr = std::get_if<Texp_try>(&e.desc)) {
      line(j, "Texp_try");
      expression(j, *tr->body);
      list_cases(j, tr->cases);
      line(j, "[]");
    } else if (auto* k = std::get_if<Texp_construct>(&e.desc)) {
      line(j, "Texp_construct \"" + k->name + "\"");
      if (k->args.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& a : k->args) expression(j + 1, *a);
        line(j, "]");
      }
    } else if (auto* ar = std::get_if<Texp_array>(&e.desc)) {
      line(j, "Texp_array Mutable");
      if (ar->elems.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& a : ar->elems) expression(j + 1, *a);
        line(j, "]");
      }
    } else if (auto* as = std::get_if<Texp_assert>(&e.desc)) {
      os << std::string(2 * j, ' ') << "Texp_assert";  // printtyped omits the \n
      expression(j, *as->e);
    } else if (auto* fo = std::get_if<Texp_for>(&e.desc)) {
      line(j, "Texp_for \"" + ident(fo->var) + "\" " +
                  (fo->dir == Direction::Up ? "Up" : "Down"));
      expression(j, *fo->lo);
      expression(j, *fo->hi);
      expression(j, *fo->body);
    } else if (auto* lz = std::get_if<Texp_lazy>(&e.desc)) {
      os << std::string(2 * j, ' ') << "Texp_lazy";  // run-on (no newline)
      expression(j, *lz->e);
    } else if (auto* wh = std::get_if<Texp_while>(&e.desc)) {
      line(j, "Texp_while");
      expression(j, *wh->cond);
      expression(j, *wh->body);
    } else if (auto* r = std::get_if<Texp_record>(&e.desc)) {
      line(j, "Texp_record");
      line(j + 1, "fields =");
      if (r->fields.empty()) line(j + 2, "[]");
      else {
        line(j + 2, "[");
        for (auto& f : r->fields) {
          if (f.kept) {
            os << std::string(2 * (j + 3), ' ') << "<kept>";  // run-on (no \n)
          } else {
            line(j + 3, "\"" + f.name + "\"");
            expression(j + 4, *f.value);
          }
        }
        line(j + 2, "]");
      }
      line(j + 1, "representation =");
      // printtyped's Record_inlined case shadows the indent with the tag
      // (`| Record_inlined i -> line i ppf ...`) -- reproduce bug-for-bug.
      if (r->representation.rfind("Record_inlined ", 0) == 0)
        line(std::atoi(r->representation.c_str() + 15), r->representation);
      else
        line(j + 2, r->representation);
      line(j + 1, "extended_expression =");
      if (r->extended) { line(j + 2, "Some"); expression(j + 3, **r->extended); }
      else line(j + 2, "None");
    } else if (auto* fd = std::get_if<Texp_field>(&e.desc)) {
      line(j, "Texp_field");
      expression(j, *fd->record);
      line(j, "\"" + fd->name + "\"");
    } else if (auto* sf = std::get_if<Texp_setfield>(&e.desc)) {
      line(j, "Texp_setfield");
      expression(j, *sf->record);
      line(j, "\"" + sf->name + "\"");
      expression(j, *sf->value);
    } else if (auto* vr = std::get_if<Texp_variant>(&e.desc)) {
      line(j, "Texp_variant \"" + vr->label + "\"");
      if (vr->arg) { line(j, "Some"); expression(j + 1, **vr->arg); }
      else line(j, "None");
    } else if (auto* iv = std::get_if<Texp_instvar>(&e.desc)) {
      line(j, "Texp_instvar \"" + ident(iv->id) + "\"");
    } else if (auto* sv = std::get_if<Texp_setinstvar>(&e.desc)) {
      line(j, "Texp_setinstvar \"" + ident(sv->id) + "\"");
      expression(j, *sv->value);
    } else if (auto* ov = std::get_if<Texp_override>(&e.desc)) {
      line(j, "Texp_override");
      line(j, "[");
      for (auto& [id, ex] : ov->fields) {
        line(j + 1, "<override> \"" + ident(id) + "\"");
        expression(j + 2, *ex);
      }
      line(j, "]");
    } else if (auto* sd = std::get_if<Texp_send>(&e.desc)) {
      // Tmeth_val/ancestor print the ident (name/stamp); Tmeth_name just the name.
      line(j, "Texp_send \"" + (sd->meth_id ? ident(*sd->meth_id) : sd->meth) + "\"");
      expression(j, *sd->obj);
    } else if (auto* pk = std::get_if<Texp_pack>(&e.desc)) {
      os << std::string(2 * j, ' ') << "Texp_pack";  // run-on: module_expr same line
      module_expr(j, *pk->me);
    } else if (std::holds_alternative<Texp_unreachable>(e.desc)) {
      os << std::string(2 * j, ' ') << "Texp_unreachable";  // run-on, no newline
    } else if (auto* xc = std::get_if<Texp_extension_ctor>(&e.desc)) {
      // run-on, no newline (printtyped has no trailing \n here)
      os << std::string(2 * j, ' ') << "Texp_extension_constructor \"" +
                                           xc->lid + "\"";
    } else if (auto* al = std::get_if<Texp_atomic_loc>(&e.desc)) {
      line(j, "Texp_atomic_loc");
      expression(j, *al->record);
      line(j, "\"" + al->field + "\"");
    } else if (auto* lo = std::get_if<Texp_letop>(&e.desc)) {
      os << std::string(2 * j, ' ')  // run-on: binding_op continues the line
         << (lo->partial ? "Texp_letop (Partial)" : "Texp_letop");
      binding_op(j + 1, lo->let_);
      if (lo->ands.empty()) line(j + 1, "[]");
      else {
        line(j + 1, "[");
        for (auto& a : lo->ands) binding_op(j + 2, a);
        line(j + 1, "]");
      }
      case_(j, *lo->body);
    } else if (auto* ob = std::get_if<Texp_object>(&e.desc)) {
      os << std::string(2 * j, ' ') << "Texp_object";  // run-on: class_structure same line
      line(j, "class_structure");
      class_structure_p(j, *ob->cs);
    } else if (auto* nw = std::get_if<Texp_new>(&e.desc)) {
      line(j, "Texp_new \"" + path_aux(nw->path) + "\"");
    } else {
      auto& si = std::get<Texp_struct_item>(e.desc);
      line(j, "Texp_struct_item");
      structure_item(j, *si.item);
      expression(j, *si.body);
    }
  }

  void binding_op(int i, const BindingOp& b) {
    line(i, "binding_op \"" + path_aux(b.path) + "\" " + loc(b.loc));
    expression(i, *b.exp);
  }

  void list_cases(int i, const std::vector<Case>& cases) {
    if (cases.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& c : cases) case_(i + 1, c);
    line(i, "]");
  }
  void list_bindings(int i, RecFlag rf, const std::vector<ValueBinding>& bs) {
    if (bs.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& b : bs) value_binding(i + 1, rf, b);
    line(i, "]");
  }

  void value_binding(int i, RecFlag rf, const ValueBinding& vb) {
    line(i, rf == RecFlag::Nonrecursive
                ? "<def>"
                : (vb.rec_dynamic ? "<def_rec_dynamic>" : "<def_rec>"));
    if (vb.attrs) attributes(i + 1, *vb.attrs);
    pattern(i + 1, vb.pat);
    expression(i + 1, vb.expr);
  }

  void module_expr(int i, const ModuleExpr& me) {
    line(i, "module_expr " + loc(me.loc));
    if (me.attrs) attributes(i, *me.attrs);
    int j = i + 1;
    if (auto* mi = std::get_if<Tmod_ident>(&me.desc)) {
      line(j, "Tmod_ident \"" + path_aux(mi->path) + "\"");
    } else if (auto* ms = std::get_if<Tmod_structure>(&me.desc)) {
      line(j, "Tmod_structure");
      if (ms->items.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& sit : ms->items) structure_item(j + 1, sit);
        line(j, "]");
      }
    } else if (auto* fn = std::get_if<Tmod_functor>(&me.desc)) {
      if (!fn->param_type) {  // generative: functor () -> ...
        line(j, "Tmod_functor ()");
      } else {
        line(j, "Tmod_functor \"" + (fn->param ? ident(*fn->param) : "_") + "\"");
        module_type(j, *fn->param_type);
      }
      module_expr(j, *fn->body);
    } else if (auto* ap = std::get_if<Tmod_apply>(&me.desc)) {
      line(j, "Tmod_apply");
      module_expr(j, *ap->fn);
      module_expr(j, *ap->arg);
    } else if (auto* au = std::get_if<Tmod_apply_unit>(&me.desc)) {
      line(j, "Tmod_apply_unit");
      module_expr(j, *au->fn);
    } else if (auto* up = std::get_if<Tmod_unpack>(&me.desc)) {
      line(j, "Tmod_unpack");
      expression(j, *up->e);
    } else {
      auto& cn = std::get<Tmod_constraint>(me.desc);
      if (cn.implicit) {  // transparent: just the inner module_expr (printtyped.ml)
        module_expr(j, *cn.expr);
      } else {
        line(j, "Tmod_constraint");
        module_expr(j, *cn.expr);
        module_type(j, *cn.type);
      }
    }
  }

  void value_desc(int i, const ValueDesc& vd) {
    line(i, "value_description " + ident(vd.id) + " " + loc(vd.loc));
    if (vd.attrs) attributes(i, *vd.attrs);
    core_type(i + 1, vd.type);
  }

  // module_declaration (printtyped): the name line, attributes, then the
  // declared module type one level deeper.  A None md_id prints as bare `_`.
  void module_declaration(int i, const ModuleDecl& md) {
    line(i, md.id.stamp < 0 ? "_" : ident(md.id));
    if (md.attrs) attributes(i, *md.attrs);
    module_type(i + 1, *md.type);
  }

  void primitive_description(int i, const Ident& id, const Location& l,
                             const ast::Attributes* attrs,
                             const std::optional<CoreType>& type,
                             const std::vector<std::string>& prims,
                             const std::optional<Path>& alias) {
    line(i, "primitive_description " + ident(id) + " " + loc(l));
    if (attrs) attributes(i, *attrs);
    if (alias) {
      line(i + 1, "Tprim_alias");
      if (type) { line(i + 2, "Some"); core_type(i + 3, *type); }
      else line(i + 2, "None");
      line(i + 2, "\"" + path_aux(*alias) + "\"");
    } else {
      line(i + 1, "Tprim_decl");
      core_type(i + 2, *type);
      list_strings(i + 2, prims);
    }
  }

  void signature_item(int i, const SignatureItem& si) {
    line(i, "signature_item " + loc(si.loc));
    int j = i + 1;
    if (auto* v = std::get_if<Tsig_value>(&si.desc)) {
      line(j, "Tsig_value");
      value_desc(j, v->vd);
    } else if (auto* ty = std::get_if<Tsig_type>(&si.desc)) {
      line(j, std::string("Tsig_type ") +
                  (ty->rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
      if (ty->decls.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& d : ty->decls) type_declaration(j + 1, d);
        line(j, "]");
      }
    } else if (auto* md = std::get_if<Tsig_module>(&si.desc)) {
      line(j, std::string("Tsig_module (") +
                  (md->md.present ? "Present" : "Absent") + ")");
      module_declaration(j, md->md);
    } else if (auto* rm = std::get_if<Tsig_recmodule>(&si.desc)) {
      line(j, "Tsig_recmodule");
      if (rm->decls.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& d : rm->decls) module_declaration(j + 1, d);
        line(j, "]");
      }
    } else if (auto* mt = std::get_if<Tsig_modtype>(&si.desc)) {
      line(j, "Tsig_modtype \"" + ident(mt->id) + "\"");
      if (mt->attrs) attributes(j, *mt->attrs);
      if (mt->type) module_type(j + 1, *mt->type);
      else os << std::string(2 * j, ' ') << "#abstract";  // run-on, no newline
    } else if (auto* inc = std::get_if<Tsig_include>(&si.desc)) {
      line(j, "Tsig_include");
      if (inc->attrs) attributes(j, *inc->attrs);
      module_type(j, *inc->mt);
    } else if (auto* ex = std::get_if<Tsig_exception>(&si.desc)) {
      line(j, "Tsig_exception");
      line(j, "type_exception");
      if (ex->attrs) attributes(j, *ex->attrs);
      line(j + 1, "ptyext_constructor =");
      extension_constructor(j + 2, ex->ctor);
    } else if (auto* tx = std::get_if<Tsig_typext>(&si.desc)) {
      line(j, "Tsig_typext");
      line(j, "type_extension");
      if (tx->attrs) attributes(j, *tx->attrs);
      line(j + 1, "ptyext_path = \"" + path_aux(tx->path) + "\"");
      line(j + 1, "ptyext_params =");
      list_core_types(j + 2, tx->params);
      line(j + 1, "ptyext_constructors =");
      if (tx->ctors.empty()) line(j + 2, "[]");
      else {
        line(j + 2, "[");
        for (auto& c : tx->ctors) extension_constructor(j + 3, c);
        line(j + 2, "]");
      }
      line(j + 1, std::string("ptyext_private = ") +
                      (tx->private_ ? "Private" : "Public"));
    } else if (auto* pr = std::get_if<Tsig_primitive>(&si.desc)) {
      line(j, "Tsig_primitive");
      primitive_description(j, pr->id, pr->loc, pr->attrs, pr->type, pr->prims,
                            pr->alias);
    } else if (auto* at = std::get_if<Tsig_attribute>(&si.desc)) {
      line(j, "Tsig_attribute \"" + at->name + "\"");
      if (at->payload)
        ast::print_payload_structure(*at->payload, j, os, fname, dirfiles);
      else
        line(j, "[]");
    } else if (auto* cl = std::get_if<Tsig_class>(&si.desc)) {
      line(j, "Tsig_class");
      line(j, "[");
      for (auto& d : cl->decls) class_description(j + 1, *d);
      line(j, "]");
    } else if (auto* clt = std::get_if<Tsig_class_type>(&si.desc)) {
      line(j, "Tsig_class_type");
      line(j, "[");
      for (auto& d : clt->decls) class_type_declaration(j + 1, *d);
      line(j, "]");
    } else {
      auto& op = std::get<Tsig_open>(si.desc);
      line(j, std::string("Tsig_open ") + (op.override_ ? "Override" : "Fresh") +
                  " \"" + path_aux(op.path) + "\"");
    }
  }

  void with_item(int i, const WithItem& w) {
    line(i, "\"" + path_aux(w.path) + "\"");
    int j = i + 1;
    if (auto* t = std::get_if<Twith_type>(&w.c)) {
      line(j, "Twith_type");
      type_declaration(j + 1, t->td);
    } else if (auto* t = std::get_if<Twith_typesubst>(&w.c)) {
      line(j, "Twith_typesubst");
      type_declaration(j + 1, t->td);
    } else if (auto* m = std::get_if<Twith_module>(&w.c)) {
      line(j, "Twith_module \"" + path_aux(m->path) + "\"");
    } else if (auto* m = std::get_if<Twith_modsubst>(&w.c)) {
      line(j, "Twith_modsubst \"" + path_aux(m->path) + "\"");
    } else {
      auto& mt = std::get<Twith_modtype>(w.c);
      line(j, "Twith_modtype");
      module_type(j + 1, *mt.mt);
    }
  }

  void package_type(int i, const PackageType& p) {
    line(i, "package_type \"" + path_aux(p.path) + "\"");
    if (p.constraints.empty()) line(i + 1, "[]");
    else {
      line(i + 1, "[");
      for (auto& [n, ct] : p.constraints) {
        line(i + 2, "with type \"" + n + "\"");  // fmt_longident quotes the name
        core_type(i + 2, *ct);
      }
      line(i + 1, "]");
    }
  }

  void module_type(int i, const ModuleType& mt) {
    line(i, "module_type " + loc(mt.loc));
    if (mt.attrs) attributes(i, *mt.attrs);
    int j = i + 1;
    if (auto* id = std::get_if<Tmty_ident>(&mt.desc)) {
      line(j, "Tmty_ident \"" + path_aux(id->path) + "\"");
    } else if (auto* sg = std::get_if<Tmty_signature>(&mt.desc)) {
      line(j, "Tmty_signature");
      if (sg->items.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& s : sg->items) signature_item(j + 1, s);
        line(j, "]");
      }
    } else if (auto* al = std::get_if<Tmty_alias>(&mt.desc)) {
      line(j, "Tmty_alias \"" + path_aux(al->path) + "\"");
    } else if (auto* fn = std::get_if<Tmty_functor>(&mt.desc)) {
      if (!fn->param_type) {  // generative: functor () -> ...
        line(j, "Tmty_functor ()");
      } else {
        line(j, "Tmty_functor \"" + (fn->param ? ident(*fn->param) : "_") + "\"");
        module_type(j, *fn->param_type);
      }
      module_type(j, *fn->body);
    } else if (auto* w = std::get_if<Tmty_with>(&mt.desc)) {
      line(j, "Tmty_with");
      module_type(j, *w->base);
      if (w->constraints.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& c : w->constraints) with_item(j + 1, c);
        line(j, "]");
      }
    } else {
      auto& to = std::get<Tmty_typeof>(mt.desc);
      line(j, "Tmty_typeof");
      module_expr(j, *to.expr);
    }
  }

  void class_field(int i, const ClassField& f) {
    line(i, "class_field " + loc(f.loc));
    if (f.attrs) attributes(i + 1, *f.attrs);
    if (auto* in = std::get_if<Tcf_inherit>(&f.desc)) {
      line(i + 1, std::string("Tcf_inherit ") + (in->override_ ? "Override" : "Fresh"));
      class_expr(i + 2, *in->ce);
      if (in->super) { line(i + 2, "Some"); line(i + 3, "\"" + *in->super + "\""); }
      else line(i + 2, "None");
      return;
    }
    if (auto* m = std::get_if<Tcf_method>(&f.desc)) {
      line(i + 1, "Tcf_method \"" + m->name + "\" " + (m->private_ ? "Private" : "Public"));
      if (m->virtual_) { line(i + 2, "Virtual"); core_type(i + 2, *m->vtype); }
      else {
        line(i + 2, std::string("Concrete ") + (m->override_ ? "Override" : "Fresh"));
        expression(i + 2, *m->expr);
      }
    } else if (auto* c = std::get_if<Tcf_constraint>(&f.desc)) {
      line(i + 1, "Tcf_constraint");
      core_type(i + 2, c->t1);
      core_type(i + 2, c->t2);
    } else if (auto* ini = std::get_if<Tcf_initializer>(&f.desc)) {
      line(i + 1, "Tcf_initializer");
      expression(i + 2, *ini->expr);
    } else if (auto* at = std::get_if<Tcf_attribute>(&f.desc)) {
      line(i + 1, "Tcf_attribute \"" + at->name + "\"");
      if (at->payload) ast::print_payload_structure(*at->payload, i + 1, os, fname, dirfiles);
      else line(i + 1, "[]");
    } else {
      auto& v = std::get<Tcf_val>(f.desc);
      line(i + 1, "Tcf_val \"" + v.name + "\" " + (v.mutable_ ? "Mutable" : "Immutable"));
      if (v.virtual_) { line(i + 2, "Virtual"); core_type(i + 2, *v.vtype); }
      else {
        line(i + 2, std::string("Concrete ") + (v.override_ ? "Override" : "Fresh"));
        expression(i + 2, *v.expr);
      }
    }
  }
  void class_expr(int i, const ClassExpr& ce) {
    line(i, "class_expr " + loc(ce.loc));
    if (auto* fn = std::get_if<Tcl_fun>(&ce.desc)) {
      line(i + 1, "Tcl_fun");
      arg_label(i + 1, fn->label);
      pattern(i + 1, *fn->pat);
      class_expr(i + 1, *fn->body);
      return;
    }
    if (auto* id = std::get_if<Tcl_ident>(&ce.desc)) {
      line(i + 1, "Tcl_ident \"" + path_aux(id->path) + "\"");
      list_core_types(i + 1, id->args);
      return;
    }
    if (auto* cn = std::get_if<Tcl_constraint>(&ce.desc)) {
      if (cn->ct) {  // explicit `(ce : ct)`: label + inner class_expr + class_type
        line(i + 1, "Tcl_constraint");
        class_expr(i + 1, *cn->ce);
        class_type(i + 1, *cn->ct);
      } else {
        class_expr(i + 1, *cn->ce);  // None constraint: nothing for itself, just recurse
      }
      return;
    }
    if (auto* op = std::get_if<Tcl_open>(&ce.desc)) {
      line(i + 1, std::string("Tcl_open ") + (op->override_ ? "Override" : "Fresh") +
                      " \"" + path_aux(op->path) + "\"");
      class_expr(i + 1, *op->body);
      return;
    }
    if (auto* ap = std::get_if<Tcl_apply>(&ce.desc)) {
      line(i + 1, "Tcl_apply");
      class_expr(i + 1, *ap->fn);
      if (ap->args.empty()) { line(i + 1, "[]"); return; }
      line(i + 1, "[");
      for (auto& [label, ex] : ap->args) {
        line(i + 2, "<arg>");
        arg_label(i + 3, label);
        if (ex) expression(i + 3, *ex);
      }
      line(i + 1, "]");
      return;
    }
    if (auto* lt = std::get_if<Tcl_let>(&ce.desc)) {
      line(i + 1, std::string("Tcl_let ") +
                      (lt->rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
      list_bindings(i + 1, lt->rf, lt->bindings);
      if (lt->ivars.empty()) line(i + 1, "[]");
      else {
        line(i + 1, "[");
        for (auto& [id, e] : lt->ivars) {
          line(i + 2, "<def> \"" + ident(id) + "\"");
          expression(i + 3, *e);
        }
        line(i + 1, "]");
      }
      class_expr(i + 1, *lt->body);
      return;
    }
    auto& st = std::get<Tcl_structure>(ce.desc);
    line(i + 1, "Tcl_structure");
    line(i + 1, "class_structure");
    class_structure_p(i + 1, st.cs);
  }
  void class_structure_p(int i, const ClassStructure& cs) {
    pattern(i + 1, *cs.self);
    if (cs.fields.empty()) { line(i + 1, "[]"); return; }
    line(i + 1, "[");
    for (auto& f : cs.fields) class_field(i + 2, f);
    line(i + 1, "]");
  }
  void pci_params(int i, const std::vector<CoreType>& params) {
    line(i, "pci_params =");
    if (params.empty()) { line(i + 1, "[]"); return; }
    line(i + 1, "[");
    for (auto& p : params) core_type(i + 2, p);
    line(i + 1, "]");
  }
  void class_declaration(int i, const ClassDeclaration& d) {
    line(i, "class_declaration " + loc(d.loc));
    line(i + 1, std::string("pci_virt = ") + (d.virt ? "Virtual" : "Concrete"));
    pci_params(i + 1, d.params);
    line(i + 1, "pci_name = \"" + d.name + "\"");
    line(i + 1, "pci_expr =");
    class_expr(i + 2, d.expr);
  }
  void class_type(int i, const ClassType& ct) {
    line(i, "class_type " + loc(ct.loc));
    if (auto* cn = std::get_if<Tcty_constr>(&ct.desc)) {
      line(i + 1, "Tcty_constr \"" + path_aux(cn->path) + "\"");
      if (cn->args.empty()) line(i + 1, "[]");
      else { line(i + 1, "["); for (auto& a : cn->args) core_type(i + 2, *a); line(i + 1, "]"); }
    } else if (auto* ar = std::get_if<Tcty_arrow>(&ct.desc)) {
      line(i + 1, "Tcty_arrow");
      arg_label(i + 1, ar->label);
      core_type(i + 1, *ar->dom);
      class_type(i + 1, *ar->cod);
    } else {
      auto& sg = std::get<Tcty_signature>(ct.desc);
      line(i + 1, "Tcty_signature");
      line(i + 1, "class_signature");
      core_type(i + 2, sg.cs.self);
      if (sg.cs.fields.empty()) { line(i + 2, "[]"); return; }
      line(i + 2, "[");
      for (auto& f : sg.cs.fields) class_type_field(i + 3, f);
      line(i + 2, "]");
    }
  }
  void class_type_field(int i, const ClassTypeField& f) {
    line(i, "class_type_field " + loc(f.loc));
    if (auto* in = std::get_if<Tctf_inherit>(&f.desc)) {
      line(i + 1, "Tctf_inherit");
      class_type(i + 1, *in->ct);
    } else if (auto* v = std::get_if<Tctf_val>(&f.desc)) {
      line(i + 1, "Tctf_val \"" + v->name + "\" " + (v->mutable_ ? "Mutable" : "Immutable") +
                      " " + (v->virtual_ ? "Virtual" : "Concrete"));
      core_type(i + 2, v->type);
    } else if (auto* m = std::get_if<Tctf_method>(&f.desc)) {
      line(i + 1, "Tctf_method \"" + m->name + "\" " + (m->private_ ? "Private" : "Public") +
                      " " + (m->virtual_ ? "Virtual" : "Concrete"));
      core_type(i + 2, m->type);
    } else {
      auto& c = std::get<Tctf_constraint>(f.desc);
      line(i + 1, "Tctf_constraint");
      core_type(i + 2, c.t1);
      core_type(i + 2, c.t2);
    }
  }
  // The class-infos body (shared by class_description / class_type_declaration).
  void class_infos_body(int i, const ClassTypeDeclaration& d) {
    line(i + 1, std::string("pci_virt = ") + (d.virt ? "Virtual" : "Concrete"));
    pci_params(i + 1, d.params);
    line(i + 1, "pci_name = \"" + d.name + "\"");
    line(i + 1, "pci_expr =");
    class_type(i + 2, d.expr);
  }
  void class_type_declaration(int i, const ClassTypeDeclaration& d) {
    line(i, "class_type_declaration " + loc(d.loc));
    class_infos_body(i, d);
  }
  // A class description (Tsig_class item): like class_type_declaration but the
  // header reads "class_description" and prints the declaration's attributes.
  void class_description(int i, const ClassTypeDeclaration& d) {
    line(i, "class_description " + loc(d.loc));
    if (d.attrs) attributes(i, *d.attrs);
    class_infos_body(i, d);
  }

  void structure_item(int i, const StructureItem& it) {
    line(i, "structure_item " + loc(it.loc));
    int j = i + 1;
    if (auto* sv = std::get_if<Tstr_value>(&it.desc)) {
      line(j, std::string("Tstr_value ") +
                  (sv->rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
      list_bindings(j, sv->rf, sv->bindings);
    } else if (auto* ev = std::get_if<Tstr_eval>(&it.desc)) {
      line(j, "Tstr_eval");
      expression(j, *ev->e);
    } else if (auto* ty = std::get_if<Tstr_type>(&it.desc)) {
      line(j, std::string("Tstr_type ") +
                  (ty->rf == RecFlag::Nonrecursive ? "Nonrec" : "Rec"));
      if (ty->decls.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& d : ty->decls) type_declaration(j + 1, d);
        line(j, "]");
      }
    } else if (auto* pr = std::get_if<Tstr_primitive>(&it.desc)) {
      line(j, "Tstr_primitive");
      primitive_description(j, pr->id, pr->loc, pr->attrs, pr->type, pr->prims,
                            pr->alias);
    } else if (auto* op = std::get_if<Tstr_open>(&it.desc)) {
      line(j, std::string("Tstr_open ") + (op->override_ ? "Override" : "Fresh"));
      module_expr(j, *op->expr);
    } else if (auto* md = std::get_if<Tstr_module>(&it.desc)) {
      line(j, std::string("Tstr_module (") + (md->present ? "Present" : "Absent") + ")");
      line(j, md->id.stamp < 0 ? "_" : ident(md->id));
      if (md->attrs) attributes(j, *md->attrs);
      module_expr(j + 1, *md->expr);
    } else if (auto* at = std::get_if<Tstr_attribute>(&it.desc)) {
      line(j, "Tstr_attribute \"" + at->name + "\"");
      if (at->payload)
        ast::print_payload_structure(*at->payload, j, os, fname, dirfiles);
      else
        line(j, "[]");
    } else if (auto* mt = std::get_if<Tstr_modtype>(&it.desc)) {
      line(j, "Tstr_modtype \"" + ident(mt->id) + "\"");
      if (mt->attrs) attributes(j, *mt->attrs);
      if (mt->type) module_type(j + 1, *mt->type);
      else os << std::string(2 * j, ' ') << "#abstract";  // run-on, no newline
    } else if (auto* inc = std::get_if<Tstr_include>(&it.desc)) {
      os << std::string(2 * j, ' ') << "Tstr_include";  // run-on: next line's indent follows
      if (inc->attrs) attributes(j, *inc->attrs);
      module_expr(j, *inc->expr);
    } else if (auto* rm = std::get_if<Tstr_recmodule>(&it.desc)) {
      line(j, "Tstr_recmodule");
      line(j, "[");
      for (auto& b : rm->bindings) {
        line(j + 1, b.id.stamp < 0 ? "_" : ident(b.id));
        if (b.attrs) attributes(j + 1, *b.attrs);
        module_expr(j + 2, *b.expr);
      }
      line(j, "]");
    } else if (auto* cl = std::get_if<Tstr_class>(&it.desc)) {
      line(j, "Tstr_class");
      line(j, "[");
      for (auto& d : cl->decls) class_declaration(j + 1, d);
      line(j, "]");
    } else if (auto* clt = std::get_if<Tstr_class_type>(&it.desc)) {
      line(j, "Tstr_class_type");
      line(j, "[");
      for (auto& d : clt->decls) class_type_declaration(j + 1, d);
      line(j, "]");
    } else if (auto* tx = std::get_if<Tstr_typext>(&it.desc)) {
      line(j, "Tstr_typext");
      line(j, "type_extension");
      if (tx->attrs) attributes(j, *tx->attrs);
      line(j + 1, "ptyext_path = \"" + path_aux(tx->path) + "\"");
      line(j + 1, "ptyext_params =");
      list_core_types(j + 2, tx->params);
      line(j + 1, "ptyext_constructors =");
      if (tx->ctors.empty()) line(j + 2, "[]");
      else {
        line(j + 2, "[");
        for (auto& c : tx->ctors) extension_constructor(j + 3, c);
        line(j + 2, "]");
      }
      line(j + 1, std::string("ptyext_private = ") +
                      (tx->private_ ? "Private" : "Public"));
    } else {
      auto& ex = std::get<Tstr_exception>(it.desc);
      line(j, "Tstr_exception");
      line(j, "type_exception");
      if (ex.attrs) attributes(j, *ex.attrs);
      line(j + 1, "ptyext_constructor =");
      extension_constructor(j + 2, ex.ctor);
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

void print_dtypedtree(const Structure& s, std::string_view fname, std::ostream& os,
                      const std::vector<std::string>& dirfiles) {
  Printer p{os, std::string(fname), dirfiles};
  p.structure(s);
}

}  // namespace cppcaml::typedtree
