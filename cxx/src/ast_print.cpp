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
  static const char* closed_flag(ClosedFlag c) { return c == ClosedFlag::Closed ? "Closed" : "Open"; }
  static const char* mutable_flag(MutableFlag m) { return m == MutableFlag::Mutable ? "Mutable" : "Immutable"; }
  static const char* private_flag(PrivateFlag p) { return p == PrivateFlag::Private ? "Private" : "Public"; }
  static const char* override_flag(OverrideFlag o) { return o == OverrideFlag::Override ? "Override" : "Fresh"; }
  static const char* rec_flag(RecFlag r) { return r == RecFlag::Recursive ? "Rec" : "Nonrec"; }

  void line(int i, const std::string& s) { os << ind(i) << s << '\n'; }

  void attributes(int i, const Attributes& attrs) {  // printast `attributes i`
    for (auto& a : attrs) {
      line(i + 1, "attribute \"" + a.name + "\"");
      structure_list(i + 2, a.payload);  // payload PStr
    }
  }

  void core_type(int i, const CoreType& t) {
    line(i, "core_type " + loc(t.loc));
    attributes(i, t.attrs);
    int j = i + 1;
    if (std::holds_alternative<Ptyp_any>(t.desc)) line(j, "Ptyp_any");
    else if (auto* v = std::get_if<Ptyp_var>(&t.desc)) line(j, "Ptyp_var " + v->name);
    else if (auto* v = std::get_if<Ptyp_arrow>(&t.desc)) {
      line(j, "Ptyp_arrow");
      arg_label(j, v->label);
      core_type(j, *v->dom);
      core_type(j, *v->cod);
    } else if (auto* v = std::get_if<Ptyp_tuple>(&t.desc)) {
      line(j, "Ptyp_tuple");
      line(j, "[");
      for (auto& el : v->elems) { line(j + 1, "None"); core_type(j + 1, *el); }
      line(j, "]");
    } else if (auto* v = std::get_if<Ptyp_constr>(&t.desc)) {
      line(j, "Ptyp_constr " + lid_loc(v->id));
      if (v->args.empty()) line(j, "[]");
      else { line(j, "["); for (auto& a : v->args) core_type(j + 1, *a); line(j, "]"); }
    } else if (auto* v = std::get_if<Ptyp_variant>(&t.desc)) {
      line(j, std::string("Ptyp_variant closed=") + closed_flag(v->closed));
      if (v->rows.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& row : v->rows) {
          if (auto* rt = std::get_if<Rtag>(&row)) {
            line(j + 1, "Rtag \"" + rt->name + "\" " + (rt->constant ? "true" : "false"));
            if (rt->types.empty()) line(j + 2, "[]");
            else { line(j + 2, "["); for (auto& c : rt->types) core_type(j + 3, *c); line(j + 2, "]"); }
          } else {
            line(j + 1, "Rinherit");
            core_type(j + 2, *std::get<Rinherit>(row).ct);
          }
        }
        line(j, "]");
      }
      if (v->labels) {
        line(j, "Some");
        if (v->labels->empty()) line(j + 1, "[]");
        else { line(j + 1, "["); for (auto& s : *v->labels) line(j + 2, '"' + s + '"'); line(j + 1, "]"); }
      } else line(j, "None");
    }
  }

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
    attributes(i, p.attrs);
    int j = i + 1;
    if (std::holds_alternative<Ppat_any>(p.desc)) line(j, "Ppat_any");
    else if (auto* v = std::get_if<Ppat_var>(&p.desc))
      line(j, "Ppat_var " + str_loc(v->name));
    else if (auto* v = std::get_if<Ppat_constant>(&p.desc)) {
      line(j, "Ppat_constant");
      constant(j, v->c);
    } else if (auto* v = std::get_if<Ppat_tuple>(&p.desc)) {
      os << ind(j) << "Ppat_tuple\n " << closed_flag(v->closed) << '\n';  // note: literal layout
      line(j, "[");
      for (auto& el : v->elems) { line(j + 1, "None"); pattern(j + 1, *el); }
      line(j, "]");
    } else if (auto* v = std::get_if<Ppat_construct>(&p.desc)) {
      line(j, "Ppat_construct " + lid_loc(v->id));
      if (v->arg) {
        line(j, "Some");
        line(j + 1, "[]");  // pcd_vars (locally abstract univars), empty in fragment
        pattern(j + 1, **v->arg);
      } else {
        line(j, "None");
      }
    } else if (auto* v = std::get_if<Ppat_or>(&p.desc)) {
      line(j, "Ppat_or");
      pattern(j, *v->l);
      pattern(j, *v->r);
    } else if (auto* v = std::get_if<Ppat_alias>(&p.desc)) {
      line(j, "Ppat_alias " + str_loc(v->name));
      pattern(j, *v->p);
    } else if (auto* v = std::get_if<Ppat_constraint>(&p.desc)) {
      line(j, "Ppat_constraint");
      pattern(j, *v->p);
      core_type(j, *v->t);
    } else if (auto* v = std::get_if<Ppat_record>(&p.desc)) {
      line(j, std::string("Ppat_record ") + closed_flag(v->closed));
      if (v->fields.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& [lbl, pat] : v->fields) { line(j + 1, lid_loc(lbl)); pattern(j + 2, *pat); }
        line(j, "]");
      }
    } else if (auto* v = std::get_if<Ppat_lazy>(&p.desc)) {
      line(j, "Ppat_lazy");
      pattern(j, *v->p);
    } else if (auto* v = std::get_if<Ppat_interval>(&p.desc)) {
      line(j, "Ppat_interval");
      constant(j, v->c1);
      constant(j, v->c2);
    } else if (auto* v = std::get_if<Ppat_variant>(&p.desc)) {
      line(j, "Ppat_variant \"" + v->label + "\"");
      if (v->arg) { line(j, "Some"); pattern(j + 1, **v->arg); }
      else line(j, "None");
    } else if (auto* v = std::get_if<Ppat_exception>(&p.desc)) {
      line(j, "Ppat_exception");
      pattern(j, *v->p);
    }
  }

  void expression(int i, const Expression& e) {
    line(i, "expression " + loc(e.loc));
    attributes(i, e.attrs);
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
    } else if (auto* v = std::get_if<Pexp_construct>(&e.desc)) {
      line(j, "Pexp_construct " + lid_loc(v->id));
      if (v->arg) { line(j, "Some"); expression(j + 1, **v->arg); }
      else line(j, "None");
    } else if (auto* v = std::get_if<Pexp_match>(&e.desc)) {
      line(j, "Pexp_match");
      expression(j, *v->e);
      cases(j, v->cases);
    } else if (auto* v = std::get_if<Pexp_try>(&e.desc)) {
      line(j, "Pexp_try");
      expression(j, *v->e);
      cases(j, v->cases);
    } else if (auto* v = std::get_if<Pexp_sequence>(&e.desc)) {
      line(j, "Pexp_sequence");
      expression(j, *v->e1);
      expression(j, *v->e2);
    } else if (auto* v = std::get_if<Pexp_constraint>(&e.desc)) {
      line(j, "Pexp_constraint");
      expression(j, *v->e);
      core_type(j, *v->t);
    } else if (auto* v = std::get_if<Pexp_field>(&e.desc)) {
      line(j, "Pexp_field");
      expression(j, *v->e);
      line(j, lid_loc(v->field));  // longident_loc prints at i with no node header
    } else if (auto* v = std::get_if<Pexp_record>(&e.desc)) {
      line(j, "Pexp_record");
      if (v->fields.empty()) line(j, "[]");
      else {
        line(j, "[");
        for (auto& [lbl, val] : v->fields) { line(j + 1, lid_loc(lbl)); expression(j + 2, *val); }
        line(j, "]");
      }
      if (v->base) { line(j, "Some"); expression(j + 1, **v->base); }
      else line(j, "None");
    } else if (auto* v = std::get_if<Pexp_assert>(&e.desc)) {
      line(j, "Pexp_assert");
      expression(j, *v->e);
    } else if (auto* v = std::get_if<Pexp_lazy>(&e.desc)) {
      line(j, "Pexp_lazy");
      expression(j, *v->e);
    } else if (auto* v = std::get_if<Pexp_while>(&e.desc)) {
      line(j, "Pexp_while");
      expression(j, *v->cond);
      expression(j, *v->body);
    } else if (auto* v = std::get_if<Pexp_for>(&e.desc)) {
      line(j, std::string("Pexp_for ") + (v->dir == DirectionFlag::Upto ? "Up" : "Down"));
      pattern(j, v->var);
      expression(j, *v->lo);
      expression(j, *v->hi);
      expression(j, *v->body);
    } else if (auto* v = std::get_if<Pexp_array>(&e.desc)) {
      line(j, "Pexp_array");
      if (v->elems.empty()) line(j, "[]");
      else { line(j, "["); for (auto& el : v->elems) expression(j + 1, *el); line(j, "]"); }
    } else if (auto* v = std::get_if<Pexp_variant>(&e.desc)) {
      line(j, "Pexp_variant \"" + v->label + "\"");
      if (v->arg) { line(j, "Some"); expression(j + 1, **v->arg); }
      else line(j, "None");
    } else if (auto* v = std::get_if<Pexp_newtype>(&e.desc)) {
      line(j, "Pexp_newtype \"" + v->name.txt + "\"");
      expression(j, *v->body);
    } else if (auto* v = std::get_if<Pexp_struct_item>(&e.desc)) {
      line(j, "Pexp_struct_item");
      structure_item(j, *v->item);
      expression(j, *v->body);
    }
  }

  void one_case(int i, const Case& c) {
    line(i, "<case>");
    pattern(i + 1, c.lhs);
    if (c.guard) { line(i + 1, "<when>"); expression(i + 2, **c.guard); }
    expression(i + 1, *c.rhs);
  }
  void cases(int i, const std::vector<Case>& cs) {
    if (cs.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& c : cs) one_case(i + 1, c);
    line(i, "]");
  }

  void function_param(int i, const FunctionParam& fp) {
    if (auto* nt = std::get_if<Pparam_newtype>(&fp.desc)) {
      line(i, "Pparam_newtype \"" + nt->name.txt + "\" " + loc(nt->loc));
      return;
    }
    auto& pv = std::get<Pparam_val>(fp.desc);
    line(i, "Pparam_val " + loc(pv.loc));
    arg_label(i + 1, pv.label);
    if (pv.default_) { line(i + 1, "Some"); expression(i + 2, **pv.default_); }
    else line(i + 1, "None");
    pattern(i + 1, pv.pat);
  }

  void function_body(int i, const FunctionBody& b) {
    if (auto* fb = std::get_if<Pfunction_body>(&b.v)) {
      line(i, "Pfunction_body");
      expression(i + 1, *fb->e);
    } else {
      auto& fc = std::get<Pfunction_cases>(b.v);
      line(i, "Pfunction_cases " + loc(fc.loc));
      cases(i + 1, fc.cases);
    }
  }

  void label_decl(int i, const LabelDecl& d) {
    line(i, loc(d.loc));
    line(i + 1, mutable_flag(d.mut));
    os << ind(i + 1) << str_loc(d.name);  // no newline: core_type runs onto this line
    core_type(i + 1, *d.type);
  }
  void ctor_args(int i, const ConstructorArguments& a) {
    if (auto* t = std::get_if<Pcstr_tuple>(&a)) {
      if (t->elems.empty()) { line(i, "[]"); return; }
      line(i, "["); for (auto& e : t->elems) core_type(i + 1, *e); line(i, "]");
    } else {
      auto& r = std::get<Pcstr_record>(a);
      if (r.fields.empty()) { line(i, "[]"); return; }
      line(i, "["); for (auto& f : r.fields) label_decl(i + 1, f); line(i, "]");
    }
  }
  void constructor_decl(int i, const ConstructorDecl& c) {
    line(i, loc(c.loc));
    line(i + 1, str_loc(c.name));
    ctor_args(i + 1, c.args);
    if (c.res) { line(i + 1, "Some"); core_type(i + 2, **c.res); }
    else line(i + 1, "None");
  }
  void type_kind(int i, const TypeKind& k) {
    if (std::holds_alternative<Ptype_abstract>(k)) line(i, "Ptype_abstract");
    else if (std::holds_alternative<Ptype_open>(k)) line(i, "Ptype_open");
    else if (auto* v = std::get_if<Ptype_variant>(&k)) {
      line(i, "Ptype_variant");
      if (v->ctors.empty()) line(i + 1, "[]");
      else { line(i + 1, "["); for (auto& c : v->ctors) constructor_decl(i + 2, c); line(i + 1, "]"); }
    } else {
      auto& r = std::get<Ptype_record>(k);
      line(i, "Ptype_record");
      if (r.fields.empty()) line(i + 1, "[]");
      else { line(i + 1, "["); for (auto& f : r.fields) label_decl(i + 2, f); line(i + 1, "]"); }
    }
  }
  void type_declaration(int i, const TypeDeclaration& d) {
    line(i, "type_declaration " + str_loc(d.name) + " " + loc(d.loc));
    int j = i + 1;
    line(j, "ptype_params =");
    if (d.params.empty()) line(j + 1, "[]");
    else { line(j + 1, "["); for (auto& p : d.params) core_type(j + 2, *p); line(j + 1, "]"); }
    line(j, "ptype_constraints =");
    line(j + 1, "[]");
    line(j, "ptype_kind =");
    type_kind(j + 1, d.kind);
    line(j, std::string("ptype_private = ") + private_flag(d.priv));
    line(j, "ptype_manifest =");
    if (d.manifest) { line(j + 1, "Some"); core_type(j + 2, **d.manifest); }
    else line(j + 1, "None");
  }
  std::string str_opt_loc(const StrOptLoc& s) const {
    return '"' + (s.txt ? *s.txt : std::string("_")) + "\" " + loc(s.loc);
  }
  void value_description(int i, const ValueDescription& v) {
    line(i, "value_description " + str_loc(v.name) + " " + loc(v.loc));
    core_type(i + 1, *v.type);
  }
  void signature_item(int i, const SignatureItem& s) {
    line(i, "signature_item " + loc(s.loc));
    int j = i + 1;
    if (auto* v = std::get_if<Psig_value>(&s.desc)) {
      line(j, "Psig_value");
      value_description(j, v->vd);
    } else if (auto* v = std::get_if<Psig_type>(&s.desc)) {
      line(j, std::string("Psig_type ") + rec_flag(v->rf));
      if (v->decls.empty()) line(j, "[]");
      else { line(j, "["); for (auto& d : v->decls) type_declaration(j + 1, d); line(j, "]"); }
    } else {
      auto& pm = std::get<Psig_module>(s.desc);
      line(j, "Psig_module " + str_opt_loc(pm.md.name));
      module_type(j, *pm.md.type);
    }
  }
  void module_type(int i, const ModuleType& m) {
    line(i, "module_type " + loc(m.loc));
    attributes(i, m.attrs);
    int j = i + 1;
    if (auto* v = std::get_if<Pmty_ident>(&m.desc)) {
      line(j, "Pmty_ident " + lid_loc(v->id));
    } else if (auto* v = std::get_if<Pmty_signature>(&m.desc)) {
      line(j, "Pmty_signature");
      if (v->items.empty()) line(j, "[]");
      else { line(j, "["); for (auto& si : v->items) signature_item(j + 1, si); line(j, "]"); }
    } else {
      auto& f = std::get<Pmty_functor>(m.desc);
      if (auto* named = std::get_if<Functor_named>(&f.param)) {
        line(j, "Pmty_functor " + str_opt_loc(named->name));
        module_type(j, *named->type);
      } else {
        line(j, "Pmty_functor ()");
      }
      module_type(j, *f.body);
    }
  }
  void module_expr(int i, const ModuleExpr& m) {
    line(i, "module_expr " + loc(m.loc));
    int j = i + 1;
    if (auto* id = std::get_if<Pmod_ident>(&m.desc)) {
      line(j, "Pmod_ident " + lid_loc(id->id));
    } else if (auto* s = std::get_if<Pmod_structure>(&m.desc)) {
      line(j, "Pmod_structure");
      structure_list(j, s->items);
    } else if (auto* f = std::get_if<Pmod_functor>(&m.desc)) {
      if (auto* named = std::get_if<Functor_named>(&f->param)) {
        line(j, "Pmod_functor " + str_opt_loc(named->name));
        module_type(j, *named->type);
      } else {
        line(j, "Pmod_functor ()");
      }
      module_expr(j, *f->body);
    } else {
      auto& c = std::get<Pmod_constraint>(m.desc);
      line(j, "Pmod_constraint");
      module_expr(j, *c.me);
      module_type(j, *c.mt);
    }
  }
  void module_binding(int i, const ModuleBinding& b) {
    line(i, str_opt_loc(b.name));
    module_expr(i + 1, b.expr);
  }
  void ext_kind(int i, const std::variant<Pext_decl, Pext_rebind>& k) {
    if (auto* d = std::get_if<Pext_decl>(&k)) {
      line(i, "Pext_decl");
      ctor_args(i + 1, d->args);
      if (d->res) { line(i + 1, "Some"); core_type(i + 2, **d->res); }
      else line(i + 1, "None");
    } else {
      line(i, "Pext_rebind");
      line(i + 1, lid_loc(std::get<Pext_rebind>(k).id));
    }
  }
  void extension_constructor(int i, const ExtensionConstructor& c) {
    line(i, "extension_constructor " + loc(c.loc));
    line(i + 1, "pext_name = \"" + c.name.txt + "\"");
    line(i + 1, "pext_kind =");
    ext_kind(i + 2, c.kind);
  }
  void type_exception(int i, const TypeException& e) {
    line(i, "type_exception");
    line(i + 1, "ptyext_constructor =");
    extension_constructor(i + 2, e.ctor);
  }
  void type_extension(int i, const TypeExtension& x) {
    line(i, "type_extension");
    int j = i + 1;
    line(j, "ptyext_path = " + lid_loc(x.path));
    line(j, "ptyext_params =");
    if (x.params.empty()) line(j + 1, "[]");
    else { line(j + 1, "["); for (auto& p : x.params) core_type(j + 2, *p); line(j + 1, "]"); }
    line(j, "ptyext_constructors =");
    if (x.ctors.empty()) line(j + 1, "[]");
    else { line(j + 1, "["); for (auto& c : x.ctors) extension_constructor(j + 2, c); line(j + 1, "]"); }
    line(j, std::string("ptyext_private = ") + private_flag(x.priv));
  }
  void primitive_description(int i, const PrimitiveDescription& p) {
    line(i, "primitive_description " + str_loc(p.name) + " " + loc(p.loc));
    line(i + 1, "Pprim_decl");
    core_type(i + 2, *p.type);
    if (p.prims.empty()) line(i + 2, "[]");
    else { line(i + 2, "["); for (auto& s : p.prims) line(i + 3, '"' + s + '"'); line(i + 2, "]"); }
  }

  void value_bindings(int i, const std::vector<ValueBinding>& l) {
    if (l.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& vb : l) {
      // list calls value_binding at i+1; <def> at i+1, its children at i+2.
      line(i + 1, "<def>");
      attributes(i + 2, vb.attrs);
      pattern(i + 2, vb.pat);
      if (vb.constraint_) core_type(i + 2, **vb.constraint_);  // Pvc_constraint (bare core_type)
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
      line(j, std::string("Pstr_value ") + rec_flag(v->rf));
      value_bindings(j, v->bindings);
    } else if (auto* v = std::get_if<Pstr_type>(&s.desc)) {
      line(j, std::string("Pstr_type ") + rec_flag(v->rf));
      if (v->decls.empty()) line(j, "[]");
      else { line(j, "["); for (auto& d : v->decls) type_declaration(j + 1, d); line(j, "]"); }
    } else if (auto* v = std::get_if<Pstr_open>(&s.desc)) {
      line(j, std::string("Pstr_open ") + override_flag(v->ovr));
      module_expr(j, v->expr);
    } else if (auto* v = std::get_if<Pstr_exception>(&s.desc)) {
      line(j, "Pstr_exception");
      type_exception(j, v->exn);
    } else if (auto* v = std::get_if<Pstr_typext>(&s.desc)) {
      line(j, "Pstr_typext");
      type_extension(j, v->ext);
    } else if (auto* v = std::get_if<Pstr_primitive>(&s.desc)) {
      line(j, "Pstr_primitive");
      primitive_description(j, v->prim);
    } else if (auto* v = std::get_if<Pstr_module>(&s.desc)) {
      line(j, "Pstr_module");
      module_binding(j, v->binding);
    } else if (auto* v = std::get_if<Pstr_attribute>(&s.desc)) {
      line(j, "Pstr_attribute \"" + v->name + "\"");
      structure_list(j, v->payload);
    } else if (auto* v = std::get_if<Pstr_extension>(&s.desc)) {
      line(j, "Pstr_extension \"" + v->name + "\"");
      structure_list(j, v->payload);  // extension payload at i
    } else if (auto* v = std::get_if<Pstr_include>(&s.desc)) {
      os << ind(j) << "Pstr_include";  // printast prints this with no trailing newline
      module_expr(j, v->expr);
    } else if (auto* v = std::get_if<Pstr_modtype>(&s.desc)) {
      line(j, "Pstr_modtype " + str_loc(v->name));
      if (v->type) module_type(j + 1, *v->type);  // modtype_declaration: Some -> module_type(i+1)
      else line(j, "#abstract");
    }
  }

  void structure_list(int i, const Structure& s) {
    if (s.empty()) { line(i, "[]"); return; }
    line(i, "[");
    for (auto& it : s) structure_item(i + 1, it);
    line(i, "]");
  }
  void structure(const Structure& s) { structure_list(0, s); }
};

}  // namespace

void print_dparsetree(const Structure& s, std::string_view fname, std::ostream& os) {
  Printer p{os, std::string(fname)};
  p.structure(s);
  os << '\n';  // driver flushes the dump with a trailing newline (@.)
}

}  // namespace cppcaml::ast
