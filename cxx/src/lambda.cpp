#include "cppcaml/lambda.hpp"

#include <functional>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <variant>

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
enum class BoxT { Hov, Hv, H, V };
struct Doc {
  enum T { Text, Break, Box } t;
  std::string s;            // Text content / Break separator
  BoxT bt = BoxT::Hov;      // Box
  int off = 0;              // Box offset
  std::vector<DocP> ch;     // Box children
};
DocP text(std::string s) { auto d = std::make_shared<Doc>(); d->t = Doc::Text; d->s = std::move(s); return d; }
DocP brk(std::string sep = " ") { auto d = std::make_shared<Doc>(); d->t = Doc::Break; d->s = std::move(sep); return d; }
DocP box(BoxT bt, int off, std::vector<DocP> ch) {
  auto d = std::make_shared<Doc>(); d->t = Doc::Box; d->bt = bt; d->off = off; d->ch = std::move(ch); return d;
}

constexpr int MARGIN = 78;

int flatw(const DocP& d) {
  if (d->t == Doc::Text || d->t == Doc::Break) return (int)d->s.size();
  int w = 0; for (auto& c : d->ch) w += flatw(c); return w;
}

struct Render {
  std::ostream& out;
  int col = 0;
  void go(const DocP& d) {
    if (d->t == Doc::Text) { out << d->s; col += (int)d->s.size(); return; }
    if (d->t == Doc::Break) { out << d->s; col += (int)d->s.size(); return; }  // bare break: flat
    int bi = col + d->off;
    bool hv_flat = (d->bt == BoxT::H) || (d->bt != BoxT::V && col + flatw(d) <= MARGIN);
    for (size_t i = 0; i < d->ch.size(); ++i) {
      auto& c = d->ch[i];
      if (c->t == Doc::Text) { out << c->s; col += (int)c->s.size(); }
      else if (c->t == Doc::Box) { go(c); }
      else {  // Break: decide per box policy
        bool b;
        if (d->bt == BoxT::H) b = false;
        else if (d->bt == BoxT::V) b = true;
        else if (d->bt == BoxT::Hv) b = !hv_flat;
        else {  // Hov: break if the next chunk (to the next break) doesn't fit
          int frag = 0;
          for (size_t j = i + 1; j < d->ch.size() && d->ch[j]->t != Doc::Break; ++j)
            frag += flatw(d->ch[j]);
          b = col + frag > MARGIN;
        }
        if (b) { out << '\n' << std::string(bi, ' '); col = bi; }
        else { out << c->s; col += (int)c->s.size(); }
      }
    }
  }
};

// ---- stamp normalization (by first appearance, like the typedtree harness) ----
struct Pr {
  std::unordered_map<int, int> stamps;
  int next = 274;
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

DocP to_doc(const LamPtr& l, Pr& pr);

// @[<2>(let@ @[<hv 1>(@[<2>id =vk@ val@] @ ...)@]@ body)@]
DocP let_doc(const LamPtr& l, Pr& pr) {
  std::vector<DocP> binds{text("(")};
  for (size_t i = 0; i < l->bindings.size(); ++i) {
    auto& b = l->bindings[i];
    if (i) binds.push_back(brk());
    binds.push_back(box(BoxT::Hov, 2,
        {text(pr.ident(b.id) + " =" + kind_suffix(b.kind)), brk(), to_doc(b.val, pr)}));
  }
  binds.push_back(text(")"));
  DocP bindings = box(BoxT::Hv, 1, std::move(binds));
  return box(BoxT::Hov, 2, {text("(let"), brk(), bindings, brk(), to_doc(l->body, pr), text(")")});
}

DocP to_doc(const LamPtr& l, Pr& pr) {
  switch (l->k) {
    case Lam::K::Var: return text(pr.ident(l->var));
    case Lam::K::ConstInt: return text(std::to_string(l->int_val));
    case Lam::K::ConstFloat: return text(l->str_val);
    case Lam::K::ConstString: return text("\"" + l->str_val + "\"");
    case Lam::K::Let: return let_doc(l, pr);
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
      }
      std::vector<DocP> xs{text(head)};
      for (auto& a : l->args) { xs.push_back(brk()); xs.push_back(to_doc(a, pr)); }
      xs.push_back(text(")"));
      return box(BoxT::Hov, 2, std::move(xs));
    }
    case Lam::K::Apply: {
      std::vector<DocP> xs{text("(apply"), brk(), to_doc(l->fn, pr)};
      for (auto& a : l->args) { xs.push_back(brk()); xs.push_back(to_doc(a, pr)); }
      xs.push_back(text(")"));
      return box(BoxT::Hov, 2, std::move(xs));
    }
    case Lam::K::Function: {
      std::vector<DocP> xs{text("(function")};
      for (auto& [id, k] : l->params) { xs.push_back(brk()); xs.push_back(text(pr.ident(id) + kind_suffix(k))); }
      xs.push_back(brk());
      if (l->ret_kind != ValueKind::Gen) { xs.push_back(text(ret_suffix(l->ret_kind))); xs.push_back(brk()); }
      xs.push_back(to_doc(l->body, pr));
      xs.push_back(text(")"));
      return box(BoxT::Hov, 2, std::move(xs));
    }
    case Lam::K::IfThenElse:
      return box(BoxT::Hov, 2, {text("(if"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->then_, pr), brk(), to_doc(l->else_, pr), text(")")});
    case Lam::K::Sequence:
      return box(BoxT::Hov, 2, {text("(seq"), brk(), to_doc(l->cond, pr), brk(),
                                to_doc(l->else_, pr), text(")")});
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

// The translator: holds the inferred value kinds and a value-variable scope so
// references resolve to their binder's stamp.
struct Translator {
  ValueKinds vk;
  int stamp = 300;  // arbitrary; normalized on print
  std::vector<std::unordered_map<std::string, Ident>> scope{{}};

  Ident fresh(const std::string& name, bool temp = false) { return Ident{name, stamp++, temp}; }
  ValueKind pat_kind(const Pattern* p) {
    auto it = vk.pat.find(p);
    return it == vk.pat.end() ? ValueKind::Gen : vkind(it->second);
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

  LamPtr expr(const Expression& e) {
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) return translate_const(c->c);
    if (auto* id = std::get_if<Pexp_ident>(&e.desc)) {
      if (auto* l = std::get_if<Lident>(&id->id.txt.v))
        if (auto* b = lookup(l->name)) { auto v = mk(Lam::K::Var); v->var = *b; return v; }
      auto v = mk(Lam::K::Var); v->var = fresh("?" + lid_last(id->id.txt));  // unresolved (will DIFF)
      return v;
    }
    if (auto* ap = std::get_if<Pexp_apply>(&e.desc)) {
      Prim p;
      if (auto* fid = std::get_if<Pexp_ident>(&ap->fn->desc))
        if (auto* l = std::get_if<Lident>(&fid->id.txt.v))
          if (!lookup(l->name) && int_op(l->name, p) && ap->args.size() == 2) {
            auto pr = mk(Lam::K::Prim); pr->prim = p;
            pr->args = {expr(*ap->args[0].second), expr(*ap->args[1].second)};
            return pr;
          }
      auto a = mk(Lam::K::Apply);
      a->fn = expr(*ap->fn);
      for (auto& [lbl, arg] : ap->args) a->args.push_back(expr(*arg));
      return a;
    }
    if (auto* f = std::get_if<Pexp_function>(&e.desc)) return function(*f);
    if (auto* le = std::get_if<Pexp_let>(&e.desc)) {
      scope.emplace_back();
      auto l = mk(Lam::K::Let);
      for (auto& b : le->bindings) {
        if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
          Ident id = fresh(pv->name.txt);
          Lam::Binding bd{id, pat_kind(&b.pat), expr(*b.expr)};
          l->bindings.push_back(std::move(bd));
          scope.back()[pv->name.txt] = id;
        } else {
          expr(*b.expr);  // unsupported pattern: best-effort
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
    if (auto* ct = std::get_if<Pexp_constraint>(&e.desc)) return expr(*ct->e);
    return mk(Lam::K::ConstInt);  // unsupported: placeholder (will DIFF)
  }

  LamPtr function(const Pexp_function& f) {
    scope.emplace_back();
    auto l = mk(Lam::K::Function);
    for (auto& fp : f.params)
      if (auto* pv = std::get_if<Pparam_val>(&fp.desc))
        if (auto* var = std::get_if<Ppat_var>(&pv->pat.desc)) {
          Ident id = fresh(var->name.txt);
          l->params.push_back({id, pat_kind(&pv->pat)});
          scope.back()[var->name.txt] = id;
        }
    auto rk = vk.fn_ret.find(&f);
    l->ret_kind = rk == vk.fn_ret.end() ? ValueKind::Gen : vkind(rk->second);
    if (auto* fb = std::get_if<Pfunction_body>(&f.body->v)) l->body = expr(*fb->e);
    else l->body = mk(Lam::K::ConstInt);  // function-cases: defer
    scope.pop_back();
    return l;
  }
};

}  // namespace

LamPtr translate_implementation(const ast::Structure& s, const std::string& module_name) {
  Translator t;
  t.vk = infer_value_kinds(s);
  auto root = mk(Lam::K::Let);
  std::vector<LamPtr> exports;  // export expressions (a var, or an alias target)
  for (auto& it : s) {
    auto* sv = std::get_if<Pstr_value>(&it.desc);
    if (!sv) continue;
    for (auto& b : sv->bindings) {
      if (auto* pv = std::get_if<Ppat_var>(&b.pat.desc)) {
        // alias elimination: `let x = <var v>` binds nothing; x exports as v.
        if (auto* rid = std::get_if<Pexp_ident>(&b.expr->desc))
          if (auto* rl = std::get_if<Lident>(&rid->id.txt.v))
            if (auto* tgt = t.lookup(rl->name)) {
              auto v = mk(Lam::K::Var); v->var = *tgt; exports.push_back(v);
              t.scope.back()[pv->name.txt] = *tgt;
              continue;
            }
        Ident id = t.fresh(pv->name.txt);
        root->bindings.push_back({id, t.pat_kind(&b.pat), t.expr(*b.expr)});
        t.scope.back()[pv->name.txt] = id;
        auto v = mk(Lam::K::Var); v->var = id; exports.push_back(v);
      } else {
        // `let () = e` / `let _ = e`: a *match* temp, not exported.
        Ident tmp = t.fresh("", true);
        root->bindings.push_back({tmp, ValueKind::Gen, t.expr(*b.expr)});
      }
    }
  }
  auto block = mk(Lam::K::Prim);
  block->prim = Prim::Makeblock; block->prim_arg = 0;
  block->args = std::move(exports);
  root->body = block;

  auto sg = mk(Lam::K::Prim);
  sg->prim = Prim::Setglobal; sg->prim_id = module_name;
  sg->args.push_back(root->bindings.empty() ? block : root);
  return sg;
}

void print_dlambda(const LamPtr& code, std::ostream& out) {
  Pr pr;
  std::ostringstream ss;
  Render r{ss};
  r.go(to_doc(code, pr));
  out << ss.str() << "\n";
}

}  // namespace cppcaml::lambda
