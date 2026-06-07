#include "cppcaml/lambda.hpp"

#include <functional>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <variant>

namespace cppcaml::lambda {
namespace {
using namespace ast;

// ---- a small Wadler/Leijen pretty-printer (matches Format's hv/hov boxes) ----
// Doc = text | line(flat-sep) | nest(indent, doc) | group(doc) | concat[...].
struct Doc;
using DocP = std::shared_ptr<Doc>;
struct Doc {
  enum T { Text, Line, Nest, Group, Concat } t;
  std::string s;            // Text; Line's flat separator
  int nest = 0;             // Nest
  std::vector<DocP> ch;     // Nest(1)/Group(1)/Concat(n)
};
DocP text(std::string s) { auto d = std::make_shared<Doc>(); d->t = Doc::Text; d->s = std::move(s); return d; }
DocP line(std::string flat = " ") { auto d = std::make_shared<Doc>(); d->t = Doc::Line; d->s = std::move(flat); return d; }
DocP nest(int n, DocP x) { auto d = std::make_shared<Doc>(); d->t = Doc::Nest; d->nest = n; d->ch = {std::move(x)}; return d; }
DocP group(DocP x) { auto d = std::make_shared<Doc>(); d->t = Doc::Group; d->ch = {std::move(x)}; return d; }
DocP concat(std::vector<DocP> xs) { auto d = std::make_shared<Doc>(); d->t = Doc::Concat; d->ch = std::move(xs); return d; }

constexpr int MARGIN = 78;

// Does the doc list fit flat in `w` remaining columns?
bool fits(int w, std::vector<std::pair<int, DocP>> work) {
  while (w >= 0 && !work.empty()) {
    auto [ind, d] = work.back();
    work.pop_back();
    switch (d->t) {
      case Doc::Text: w -= (int)d->s.size(); break;
      case Doc::Line: w -= (int)d->s.size(); break;  // flat: separator width
      case Doc::Nest: work.push_back({ind + d->nest, d->ch[0]}); break;
      case Doc::Group: work.push_back({ind, d->ch[0]}); break;
      case Doc::Concat:
        for (auto it = d->ch.rbegin(); it != d->ch.rend(); ++it) work.push_back({ind, *it});
        break;
    }
  }
  return w >= 0;
}

void render(const DocP& doc, std::ostream& out) {
  // work stack of (indent, flat?, doc)
  std::vector<std::tuple<int, bool, DocP>> work{{0, false, doc}};
  int col = 0;
  while (!work.empty()) {
    auto [ind, flat, d] = work.back();
    work.pop_back();
    switch (d->t) {
      case Doc::Text: out << d->s; col += (int)d->s.size(); break;
      case Doc::Line:
        if (flat) { out << d->s; col += (int)d->s.size(); }
        else { out << '\n' << std::string(ind, ' '); col = ind; }
        break;
      case Doc::Nest: work.push_back({ind + d->nest, flat, d->ch[0]}); break;
      case Doc::Group: {
        bool f = flat || fits(MARGIN - col, {{ind, d->ch[0]}});
        work.push_back({ind, f, d->ch[0]});
        break;
      }
      case Doc::Concat:
        for (auto it = d->ch.rbegin(); it != d->ch.rend(); ++it) work.push_back({ind, flat, *it});
        break;
    }
  }
}

// ---- stamp normalization (by first appearance, like the typedtree harness) ----
struct Pr {
  std::unordered_map<int, int> stamps;
  int next = 274;  // OCaml's first user stamp in these dumps tends to be 274
  int norm(int s) { auto it = stamps.find(s); if (it != stamps.end()) return it->second;
                    return stamps[s] = next++; }
  std::string ident(const Ident& i) {
    std::string base = i.temp ? "*match*" : i.name;
    return base + "/" + std::to_string(norm(i.stamp));
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

DocP to_doc(const LamPtr& l, Pr& pr);

DocP let_doc(const LamPtr& l, Pr& pr) {
  // (let (id1 =[k] v1 id2 =[k] v2 ...) body)
  std::vector<DocP> binds;
  for (size_t i = 0; i < l->bindings.size(); ++i) {
    auto& b = l->bindings[i];
    if (i) binds.push_back(line());
    std::string head = pr.ident(b.id) + " =" + kind_suffix(b.kind) + " ";
    binds.push_back(group(concat({text(head), to_doc(b.val, pr)})));
  }
  DocP bindings = concat({text("("), nest(1, concat(binds)), text(")")});
  return group(concat({text("(let "), nest(2, concat({bindings, line(), to_doc(l->body, pr)})),
                       text(")")}));
}

DocP to_doc(const LamPtr& l, Pr& pr) {
  switch (l->k) {
    case Lam::K::Var: return text(pr.ident(l->var));
    case Lam::K::ConstInt: return text(std::to_string(l->int_val));
    case Lam::K::ConstFloat: return text(l->str_val);
    case Lam::K::ConstString: return text("\"" + l->str_val + "\"");
    case Lam::K::Let: return let_doc(l, pr);
    case Lam::K::Prim: {
      switch (l->prim) {
        case Prim::Setglobal:
          return group(concat({text("(setglobal " + l->prim_id + "!"),
                               nest(2, concat({line(), to_doc(l->args[0], pr)})), text(")")}));
        case Prim::Makeblock: {
          std::vector<DocP> xs{text("(makeblock " + std::to_string(l->prim_arg))};
          for (auto& a : l->args) { xs.push_back(line()); xs.push_back(to_doc(a, pr)); }
          xs.push_back(text(")"));
          return group(nest(1, concat(xs)));
        }
        case Prim::Field: case Prim::FieldImm: {
          std::string nm = l->prim == Prim::FieldImm ? "field_imm " : "field ";
          return group(concat({text("(" + nm + std::to_string(l->prim_arg)),
                               nest(1, concat({line(), to_doc(l->args[0], pr)})), text(")")}));
        }
        case Prim::Global: return text("(global " + l->prim_id + "!)");
        case Prim::Addint: case Prim::Subint: case Prim::Mulint:
        case Prim::NotEqInt: case Prim::EqInt: {
          const char* op = l->prim == Prim::Addint ? "+" : l->prim == Prim::Subint ? "-"
                         : l->prim == Prim::Mulint ? "*" : l->prim == Prim::NotEqInt ? "!=" : "==";
          std::vector<DocP> xs{text(std::string("(") + op)};
          for (auto& a : l->args) { xs.push_back(line()); xs.push_back(to_doc(a, pr)); }
          xs.push_back(text(")"));
          return group(nest(1, concat(xs)));
        }
      }
      return text("?prim");
    }
    case Lam::K::Apply: {
      std::vector<DocP> xs{text("(apply"), line(), to_doc(l->fn, pr)};
      for (auto& a : l->args) { xs.push_back(line()); xs.push_back(to_doc(a, pr)); }
      xs.push_back(text(")"));
      return group(nest(1, concat(xs)));
    }
    case Lam::K::Function: {
      std::vector<DocP> xs{text("(function")};
      for (auto& [id, k] : l->params)
        xs.push_back(concat({line(), text(pr.ident(id) + kind_suffix(k))}));
      if (l->ret_kind != ValueKind::Gen)
        xs.push_back(text(" : " + std::string(kind_suffix(l->ret_kind)).substr(1, kind_suffix(l->ret_kind).size()-2)));
      xs.push_back(concat({line(), to_doc(l->body, pr), text(")")}));
      return group(nest(1, concat(xs)));
    }
    case Lam::K::IfThenElse:
      return group(nest(1, concat({text("(if"), line(), to_doc(l->cond, pr), line(),
                                   to_doc(l->then_, pr), line(), to_doc(l->else_, pr), text(")")})));
    case Lam::K::Sequence:
      return group(concat({text("(seq"), nest(1, concat({line(), to_doc(l->cond, pr), line(),
                                                         to_doc(l->else_, pr)})), text(")")}));
  }
  return text("?");
}

// ---- translation (parsetree -> Lambda), slice 1 ----
int stamp_counter = 300;  // arbitrary; normalized on print

LamPtr mk(Lam::K k) { auto l = std::make_shared<Lam>(); l->k = k; return l; }

ValueKind kind_of_const(const Constant& c) {
  if (std::holds_alternative<Pconst_integer>(c.desc)) return ValueKind::Int;
  if (std::holds_alternative<Pconst_float>(c.desc)) return ValueKind::Float;
  return ValueKind::Gen;
}

LamPtr translate_const(const Constant& c) {
  if (auto* i = std::get_if<Pconst_integer>(&c.desc)) {
    auto l = mk(Lam::K::ConstInt); l->int_val = std::stoll(i->value); return l;
  }
  if (auto* f = std::get_if<Pconst_float>(&c.desc)) {
    auto l = mk(Lam::K::ConstFloat); l->str_val = f->value; return l;
  }
  if (auto* s = std::get_if<Pconst_string>(&c.desc)) {
    auto l = mk(Lam::K::ConstString); l->str_val = s->s; return l;
  }
  return mk(Lam::K::ConstInt);
}

}  // namespace

LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name) {
  // Slice 1: collect top-level value bindings; each becomes a Let binding, and
  // the module exports them in a (makeblock 0 ...) (in binding order).
  auto root = mk(Lam::K::Let);
  std::vector<Ident> exports;
  int stamp = stamp_counter;
  for (auto& it : s) {
    auto* sv = std::get_if<Pstr_value>(&it.desc);
    if (!sv) continue;
    for (auto& b : sv->bindings) {
      auto* pv = std::get_if<Ppat_var>(&b.pat.desc);
      if (!pv) continue;
      Lam::Binding bind;
      bind.id = Ident{pv->name.txt, stamp++, false};
      if (auto* ce = std::get_if<Pexp_constant>(&b.expr->desc)) {
        bind.kind = kind_of_const(ce->c);
        bind.val = translate_const(ce->c);
      } else {
        bind.kind = ValueKind::Gen;
        bind.val = mk(Lam::K::ConstInt);  // placeholder for unsupported exprs
      }
      exports.push_back(bind.id);
      root->bindings.push_back(std::move(bind));
    }
  }
  auto block = mk(Lam::K::Prim);
  block->prim = Prim::Makeblock; block->prim_arg = 0;
  for (auto& e : exports) { auto v = mk(Lam::K::Var); v->var = e; block->args.push_back(v); }
  root->body = block;

  auto sg = mk(Lam::K::Prim);
  sg->prim = Prim::Setglobal; sg->prim_id = module_name;
  sg->args.push_back(root->bindings.empty() ? block : root);
  return sg;
}

void print_dlambda(const LamPtr& code, std::ostream& out) {
  Pr pr;
  std::ostringstream ss;
  render(to_doc(code, pr), ss);
  out << ss.str() << "\n";
}

}  // namespace cppcaml::lambda
