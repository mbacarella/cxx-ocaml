// Bytegen: lower the Lambda IR to the stack+accumulator VM's instruction stream
// (a faithful port of bytecomp/bytegen.ml), validated against `ocamlc -dinstr`.
// The continuation-passing structure and its peephole helpers (add_pop,
// label_code, make_branch, discard_dead_code) are what make the output
// byte-identical, so they are reproduced exactly.
#include "cppcaml/bytecode.hpp"

#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <unordered_map>

namespace cppcaml::bytecode {
namespace {
using lambda::Lam;
using lambda::LamPtr;
using lambda::Ident;
using lambda::Prim;
using lambda::ValueKind;

Instr I(Op op) { Instr i; i.op = op; return i; }
Instr Iop(Op op, int a) { Instr i; i.op = op; i.a = a; return i; }
Code cons(Instr h, Code t) { return std::make_shared<const ICell>(ICell{std::move(h), std::move(t)}); }
bool empty(const Code& c) { return !c; }
const Instr* head(const Code& c) { return c ? &c->head : nullptr; }

bool is_immed(long long n) { return n >= -(1LL << 30) && n < (1LL << 30); }

// ---- continuation peephole helpers (bytegen.ml) ----
Code add_pop(int n, Code cont) {
  if (n == 0) return cont;
  if (auto* h = head(cont)) {
    if (h->op == Op::Pop) return add_pop(n + h->a, cont->tail);
    if (h->op == Op::Return) return cons(Iop(Op::Return, n + h->a), cont->tail);
    if (h->op == Op::Raise || h->op == Op::Reraise || h->op == Op::RaiseNotrace) return cont;
  }
  return cons(Iop(Op::Pop, n), cont);
}

// label_code: a label for the start of cont (reusing a leading branch/label).
struct Bytegen {
  int label_counter = 0;
  std::string compunit;
  int new_label() { return ++label_counter; }

  struct ToCompile {
    std::vector<Ident> params;
    LamPtr body;
    int label;
    // closure environment shared with the function body
    std::map<int, std::pair<bool, int>> entries;  // stamp -> {isFunction, pos}
    int rec_pos;
  };
  std::vector<ToCompile> functions_to_compile;  // a stack (LIFO)

  std::pair<int, Code> label_code(Code cont) {
    if (auto* h = head(cont)) {
      if (h->op == Op::Branch) return {h->a, cont};
      if (h->op == Op::Label) return {h->a, cont};
    }
    int lbl = new_label();
    return {lbl, cons(Iop(Op::Label, lbl), cont)};
  }

  // make_branch: an instruction that branches to / performs `cont`.
  std::pair<Instr, Code> make_branch_2(int lbl, bool has_lbl, int n, Code cont, Code c) {
    while (true) {
      const Instr* h = head(c);
      if (h && h->op == Op::Return) return {Iop(Op::Return, n + h->a), cont};
      if (h && h->op == Op::Label) { c = c->tail; continue; }
      if (h && h->op == Op::Pop) { n += h->a; c = c->tail; continue; }
      if (has_lbl) return {Iop(Op::Branch, lbl), cont};
      int nl = new_label();
      return {Iop(Op::Branch, nl), cons(Iop(Op::Label, nl), cont)};
    }
  }
  std::pair<Instr, Code> make_branch(Code cont) {
    if (auto* h = head(cont)) {
      if (h->op == Op::Branch) return {*h, cont};
      if (h->op == Op::Return) return {*h, cont};
      if (h->op == Op::Raise || h->op == Op::Reraise || h->op == Op::RaiseNotrace) return {*h, cont};
      if (h->op == Op::Label) return make_branch_2(h->a, true, 0, cont, cont);
    }
    return make_branch_2(0, false, 0, cont, cont);
  }

  Code branch_to(int label, Code cont) {
    if (auto* h = head(cont); h && h->op == Op::Label && h->a == label) return cont;
    return cons(Iop(Op::Branch, label), cont);
  }

  Code discard_dead_code(Code cont) {
    while (auto* h = head(cont)) {
      if (h->op == Op::Label || h->op == Op::Restart || h->op == Op::Setglobal) break;
      cont = cont->tail;
    }
    return cont;
  }

  Code add_const_unit(Code cont) {
    if (auto* h = head(cont))
      if (h->op == Op::Acc || h->op == Op::Const || h->op == Op::Getglobal || h->op == Op::PushRetaddr)
        return cont;
    auto z = std::make_shared<Lam>(); z->k = Lam::K::ConstInt; z->int_val = 0;
    Instr k = I(Op::Const); k.cst = z;
    return cons(k, cont);
  }

  static bool is_tailcall(const Code& c0) {
    Code c = c0;
    while (auto* h = head(c)) {
      if (h->op == Op::Return) return true;
      if (h->op == Op::Label || h->op == Op::Pop) { c = c->tail; continue; }
      return false;
    }
    return false;
  }

  // ---- environment ----
  struct Env {
    std::map<int, int> stack;                         // stamp -> stack position
    bool in_closure = false;
    std::map<int, std::pair<bool, int>> entries;      // stamp -> {isFunction, pos}
    int env_pos = 0;
  };
  static Env add_var(const Ident& id, int pos, Env env) { env.stack[id.stamp] = pos; return env; }

  // ---- free variables of a lambda term (Ident.Set order = by stamp) ----
  void fvs(const LamPtr& e, std::set<int>& bound, std::map<int, Ident>& out) {
    if (!e) return;
    using K = Lam::K;
    switch (e->k) {
      case K::Var:
        if (!bound.count(e->var.stamp) && !out.count(e->var.stamp)) out[e->var.stamp] = e->var;
        return;
      case K::ConstInt: case K::ConstChar: case K::ConstFloat:
      case K::ConstString: return;
      case K::ConstBlock: for (auto& a : e->args) fvs(a, bound, out); return;
      case K::Apply: fvs(e->fn, bound, out); for (auto& a : e->args) fvs(a, bound, out); return;
      case K::Prim: for (auto& a : e->args) fvs(a, bound, out); return;
      case K::IfThenElse: fvs(e->cond, bound, out); fvs(e->then_, bound, out); fvs(e->else_, bound, out); return;
      case K::Sequence: fvs(e->cond, bound, out); fvs(e->else_, bound, out); return;
      case K::While: fvs(e->cond, bound, out); fvs(e->body, bound, out); return;
      case K::Let: {
        std::set<int> b = bound;
        for (auto& bd : e->bindings) { fvs(bd.val, b, out); b.insert(bd.id.stamp); }
        fvs(e->body, b, out);
        return;
      }
      case K::Function: {
        std::set<int> b = bound;
        for (auto& p : e->params) b.insert(p.first.stamp);
        fvs(e->body, b, out);
        return;
      }
      case K::For: {
        fvs(e->then_, bound, out); fvs(e->else_, bound, out);
        std::set<int> b = bound; b.insert(e->var.stamp);
        fvs(e->body, b, out);
        return;
      }
      case K::Try: {
        fvs(e->body, bound, out);
        std::set<int> b = bound; b.insert(e->var.stamp);
        fvs(e->then_, b, out);
        return;
      }
      case K::Switch: {
        fvs(e->cond, bound, out);
        for (auto& c : e->sw_consts) fvs(c.body, bound, out);
        for (auto& c : e->sw_blocks) fvs(c.body, bound, out);
        fvs(e->sw_default, bound, out);
        return;
      }
    }
  }
  std::vector<Ident> free_vars(const LamPtr& e) {
    std::set<int> bound;
    std::map<int, Ident> out;  // ordered by stamp
    fvs(e, bound, out);
    std::vector<Ident> r;
    for (auto& [s, id] : out) r.push_back(id);
    return r;
  }

  // ---- primitive -> instruction ----
  // Map a Lam Prim that compiles through the generic comp_args path to its
  // single closing instruction.
  Instr comp_primitive(const LamPtr& e) {
    switch (e->prim) {
      case Prim::Setglobal: { Instr i = I(Op::Setglobal); i.str = e->prim_id; return i; }
      case Prim::Global:    { Instr i = I(Op::Getglobal); i.str = e->prim_id; return i; }
      case Prim::Field: case Prim::FieldImm: case Prim::FieldMut: case Prim::FieldInt:
        return Iop(Op::Getfield, e->prim_arg);
      case Prim::SetfieldImm: case Prim::SetfieldPtr:
        return Iop(Op::Setfield, e->prim_arg);
      case Prim::Addint: return I(Op::Addint);
      case Prim::Subint: return I(Op::Subint);
      case Prim::Mulint: return I(Op::Mulint);
      case Prim::NotEqInt: return I(Op::Neqint);
      case Prim::EqInt: return I(Op::Eqint);
      case Prim::Offsetref: return Iop(Op::Offsetref, e->prim_arg);
      case Prim::Ccall: { Instr i = I(Op::Ccall); i.str = e->prim_id; i.a = (int)e->args.size(); return i; }
      case Prim::IntCmp: return intcmp_or_ccall(e);
      default: break;
    }
    // makeblock/makemutable/raise handled in comp_expr; anything else: best effort
    Instr i = I(Op::Ccall); i.str = "?"; i.a = (int)e->args.size(); return i;
  }
  // IntCmp carries a spelling: integer comparison, or a float/string/array op
  // that lowers to a C call.
  Instr intcmp_or_ccall(const LamPtr& e) {
    const std::string& s = e->prim_id;
    if (s == "==") return I(Op::Eqint);
    if (s == "!=") return I(Op::Neqint);
    if (s == "<") return I(Op::Ltint);
    if (s == ">") return I(Op::Gtint);
    if (s == "<=") return I(Op::Leint);
    if (s == ">=") return I(Op::Geint);
    static const std::unordered_map<std::string, std::string> ccall = {
      {"+.", "caml_add_float"}, {"-.", "caml_sub_float"}, {"*.", "caml_mul_float"},
      {"/.", "caml_div_float"}, {"~.", "caml_neg_float"}, {"abs.", "caml_abs_float"},
      {"float_of_int", "caml_float_of_int"}, {"int_of_float", "caml_int_of_float"},
      {"string.length", "caml_ml_string_length"}, {"string.get", "caml_string_get"},
      {"bytes.length", "caml_ml_bytes_length"}, {"bytes.get", "caml_bytes_get"},
      {"bytes.set", "caml_bytes_set"},
    };
    if (auto it = ccall.find(s); it != ccall.end()) {
      Instr i = I(Op::Ccall); i.str = it->second; i.a = (int)e->args.size(); return i;
    }
    Instr i = I(Op::Ccall); i.str = "?" + s; i.a = (int)e->args.size(); return i;
  }

  // ---- comp_expr ----
  Code comp_args(const Env& env, const std::vector<LamPtr>& args, int sz, Code cont) {
    // comp_expr_list on the reversed list.
    std::vector<LamPtr> rev(args.rbegin(), args.rend());
    return comp_expr_list(env, rev, 0, sz, cont);
  }
  Code comp_expr_list(const Env& env, const std::vector<LamPtr>& xs, size_t i, int sz, Code cont) {
    if (i >= xs.size()) return cont;
    if (i + 1 == xs.size()) return comp_expr(env, xs[i], sz, cont);
    return comp_expr(env, xs[i], sz, cons(I(Op::Push), comp_expr_list(env, xs, i + 1, sz + 1, cont)));
  }

  Code comp_let(const Env& env, const LamPtr& e, size_t i, int sz, Code cont) {
    if (i >= e->bindings.size()) return comp_expr(env, e->body, sz, cont);
    const auto& bd = e->bindings[i];
    Env env2 = add_var(bd.id, sz + 1, env);
    return comp_expr(env, bd.val, sz,
                     cons(I(Op::Push), comp_let(env2, e, i + 1, sz + 1, add_pop(1, cont))));
  }

  Code comp_expr(const Env& env, const LamPtr& exp, int sz, Code cont) {
    using K = Lam::K;
    switch (exp->k) {
      case K::Var: {
        auto it = env.stack.find(exp->var.stamp);
        if (it != env.stack.end()) return cons(Iop(Op::Acc, sz - it->second), cont);
        if (env.in_closure) {
          auto e2 = env.entries.find(exp->var.stamp);
          if (e2 != env.entries.end()) {
            auto [isFun, pos] = e2->second;
            return cons(Iop(isFun ? Op::Offsetclosure : Op::Envacc, pos - env.env_pos), cont);
          }
        }
        return cons(Iop(Op::Acc, 0), cont);  // unresolved (will DIFF)
      }
      case K::ConstInt: case K::ConstChar: case K::ConstFloat:
      case K::ConstString: case K::ConstBlock: {
        Instr k = I(Op::Const); k.cst = exp; return cons(k, cont);
      }
      case K::Let: return comp_let(env, exp, 0, sz, cont);
      case K::Sequence:
        return comp_expr(env, exp->cond, sz, comp_expr(env, exp->else_, sz, cont));
      case K::IfThenElse:
        return comp_binary_test(env, exp->cond, exp->then_, exp->else_, sz, cont);
      case K::Apply: {
        int nargs = (int)exp->args.size();
        if (is_tailcall(cont)) {
          Instr at = I(Op::Appterm); at.a = nargs; at.b = sz + nargs;
          return comp_args(env, exp->args, sz,
                   cons(I(Op::Push), comp_expr(env, exp->fn, sz + nargs,
                     cons(at, discard_dead_code(cont)))));
        }
        if (nargs < 4) {
          return comp_args(env, exp->args, sz,
                   cons(I(Op::Push), comp_expr(env, exp->fn, sz + nargs,
                     cons(Iop(Op::Apply, nargs), cont))));
        }
        auto [lbl, cont1] = label_code(cont);
        return cons(Iop(Op::PushRetaddr, lbl),
                 comp_args(env, exp->args, sz + 3,
                   cons(I(Op::Push), comp_expr(env, exp->fn, sz + 3 + nargs,
                     cons(Iop(Op::Apply, nargs), cont1)))));
      }
      case K::Function: {
        int lbl = new_label();
        std::vector<Ident> fv = free_vars(exp);
        // closure_entries Single_non_recursive: free vars at pos 2,3,... delta 1
        std::map<int, std::pair<bool, int>> entries;
        for (size_t i = 0; i < fv.size(); ++i) entries[fv[i].stamp] = {false, 2 + (int)i};
        std::vector<Ident> params;
        for (auto& p : exp->params) params.push_back(p.first);
        functions_to_compile.push_back({params, exp->body, lbl, entries, 0});
        std::vector<LamPtr> fvargs;
        for (auto& id : fv) { auto v = std::make_shared<Lam>(); v->k = K::Var; v->var = id; fvargs.push_back(v); }
        Instr cl = I(Op::Closure); cl.a = lbl; cl.b = (int)fv.size();
        return comp_args(env, fvargs, sz, cons(cl, cont));
      }
      case K::Prim: {
        switch (exp->prim) {
          case Prim::Makeblock: case Prim::Makemutable: {
            Instr mb = I(Op::Makeblock); mb.a = (int)exp->args.size(); mb.b = exp->prim_arg;
            return comp_args(env, exp->args, sz, cons(mb, cont));
          }
          case Prim::Raise:
            return comp_expr(env, exp->args[0], sz, cons(I(Op::Raise), discard_dead_code(cont)));
          case Prim::Reraise:
            return comp_expr(env, exp->args[0], sz, cons(I(Op::Reraise), discard_dead_code(cont)));
          case Prim::Addint:
            if (exp->args.size() == 2 && exp->args[1]->k == K::ConstInt && is_immed(exp->args[1]->int_val))
              return comp_expr(env, exp->args[0], sz, cons(Iop(Op::Offsetint, (int)exp->args[1]->int_val), cont));
            break;
          case Prim::Subint:
            if (exp->args.size() == 2 && exp->args[1]->k == K::ConstInt && is_immed(-exp->args[1]->int_val))
              return comp_expr(env, exp->args[0], sz, cons(Iop(Op::Offsetint, -(int)exp->args[1]->int_val), cont));
            break;
          default: break;
        }
        // generic: comp_args then the closing primitive instruction
        int nargs = (int)exp->args.size() - 1;
        (void)nargs;
        return comp_args(env, exp->args, sz, cons(comp_primitive(exp), cont));
      }
      default:
        // Switch / For / While / Try not yet lowered: placeholder (will DIFF).
        { auto z = std::make_shared<Lam>(); z->k = K::ConstInt; z->int_val = 0;
          Instr k = I(Op::Const); k.cst = z; return cons(k, cont); }
    }
  }

  // if-then-else (code_as_jump is always None for us: no Lstaticraise).
  Code comp_binary_test(const Env& env, const LamPtr& cond, const LamPtr& ifso,
                        const LamPtr& ifnot, int sz, Code cont) {
    Code cont_cond;
    bool ifnot_unit = ifnot->k == Lam::K::ConstInt && ifnot->int_val == 0;
    if (ifnot_unit) {
      auto [lbl_end, cont1] = label_code(cont);
      cont_cond = cons(Iop(Op::Strictbranchifnot, lbl_end), comp_expr(env, ifso, sz, cont1));
    } else {
      auto [branch_end, cont1] = make_branch(cont);
      auto [lbl_not, cont2] = label_code(comp_expr(env, ifnot, sz, cont1));
      cont_cond = cons(Iop(Op::Branchifnot, lbl_not),
                       comp_expr(env, ifso, sz, cons(branch_end, cont2)));
    }
    return comp_expr(env, cond, sz, cont_cond);
  }

  // ---- functions & module ----
  Code comp_block(const Env& env, const LamPtr& exp, int sz, Code cont) {
    return comp_expr(env, exp, sz, cont);
  }
  Code comp_function(const ToCompile& tc, Code cont) {
    int arity = (int)tc.params.size();
    Env env;
    // params at positions arity, arity-1, ..., 1
    for (int i = 0; i < arity; ++i) env.stack[tc.params[i].stamp] = arity - i;
    env.in_closure = true;
    env.entries = tc.entries;
    env.env_pos = 3 * tc.rec_pos;
    cont = comp_block(env, tc.body, arity, cons(Iop(Op::Return, arity), cont));
    if (arity > 1)
      return cons(I(Op::Restart), cons(Iop(Op::Label, tc.label), cons(Iop(Op::Grab, arity - 1), cont)));
    return cons(Iop(Op::Label, tc.label), cont);
  }
  Code comp_remainder(Code cont) {
    while (!functions_to_compile.empty()) {
      ToCompile tc = functions_to_compile.back();
      functions_to_compile.pop_back();
      cont = comp_function(tc, cont);
    }
    return cont;
  }

  Code compile_implementation(const LamPtr& expr, const std::string& modname) {
    label_counter = 0;
    compunit = modname;
    functions_to_compile.clear();
    Env empty;
    Code init_code = comp_block(empty, expr, 0, nullptr);
    if (!functions_to_compile.empty()) {
      int lbl_init = new_label();
      return cons(Iop(Op::Branch, lbl_init), comp_remainder(cons(Iop(Op::Label, lbl_init), init_code)));
    }
    return init_code;
  }
};

// ---- printer (bytecomp/printinstr.ml) ----
std::string instr_text(const Instr& i) {
  auto n = [](int x) { return std::to_string(x); };
  switch (i.op) {
    case Op::Acc: return "\tacc " + n(i.a);
    case Op::Envacc: return "\tenvacc " + n(i.a);
    case Op::Push: return "\tpush";
    case Op::Pop: return "\tpop " + n(i.a);
    case Op::Assign: return "\tassign " + n(i.a);
    case Op::PushRetaddr: return "\tpush_retaddr L" + n(i.a);
    case Op::Apply: return "\tapply " + n(i.a);
    case Op::Appterm: return "\tappterm " + n(i.a) + ", " + n(i.b);
    case Op::Return: return "\treturn " + n(i.a);
    case Op::Restart: return "\trestart";
    case Op::Grab: return "\tgrab " + n(i.a);
    case Op::Closure: return "\tclosure L" + n(i.a) + ", " + n(i.b);
    case Op::Offsetclosure: return "\toffsetclosure " + n(i.a);
    case Op::Getglobal: return "\tgetglobal " + i.str + "!";
    case Op::Setglobal: return "\tsetglobal " + i.str + "!";
    case Op::Const: return "\tconst " + lambda::structured_constant(i.cst);
    case Op::Makeblock: return "\tmakeblock " + n(i.a) + ", " + n(i.b);
    case Op::Makefloatblock: return "\tmakefloatblock " + n(i.a);
    case Op::Getfield: return "\tgetfield " + n(i.a);
    case Op::Setfield: return "\tsetfield " + n(i.a);
    case Op::Getvectitem: return "\tgetvectitem";
    case Op::Setvectitem: return "\tsetvectitem";
    case Op::Getstringchar: return "\tgetstringchar";
    case Op::Branch: return "\tbranch L" + n(i.a);
    case Op::Branchif: return "\tbranchif L" + n(i.a);
    case Op::Branchifnot: return "\tbranchifnot L" + n(i.a);
    case Op::Strictbranchif: return "\tstrictbranchif L" + n(i.a);
    case Op::Strictbranchifnot: return "\tstrictbranchifnot L" + n(i.a);
    case Op::Boolnot: return "\tboolnot";
    case Op::Pushtrap: return "\tpushtrap L" + n(i.a);
    case Op::Poptrap: return "\tpoptrap";
    case Op::Raise: return "\traise";
    case Op::Reraise: return "\treraise";
    case Op::RaiseNotrace: return "\traise_notrace";
    case Op::CheckSignals: return "\tcheck_signals";
    case Op::Ccall: return "\tccall " + i.str + ", " + n(i.a);
    case Op::Negint: return "\tnegint";
    case Op::Addint: return "\taddint";
    case Op::Subint: return "\tsubint";
    case Op::Mulint: return "\tmulint";
    case Op::Divint: return "\tdivint";
    case Op::Modint: return "\tmodint";
    case Op::Andint: return "\tandint";
    case Op::Orint: return "\torint";
    case Op::Xorint: return "\txorint";
    case Op::Lslint: return "\tlslint";
    case Op::Lsrint: return "\tlsrint";
    case Op::Asrint: return "\tasrint";
    case Op::Eqint: return "\teqint";
    case Op::Neqint: return "\tneqint";
    case Op::Ltint: return "\tltint";
    case Op::Gtint: return "\tgtint";
    case Op::Leint: return "\tleint";
    case Op::Geint: return "\tgeint";
    case Op::Physeq: return "\tphyseq";
    case Op::Physneq: return "\tphysneq";
    case Op::Offsetint: return "\toffsetint " + n(i.a);
    case Op::Offsetref: return "\toffsetref " + n(i.a);
    case Op::Isint: return "\tisint";
    case Op::Isout: return "\tisout";
    case Op::Stop: return "\tstop";
    default: return "\t?";
  }
}

}  // namespace

Code compile_implementation(const lambda::LamPtr& code, const std::string& module_name) {
  Bytegen bg;
  return bg.compile_implementation(code, module_name);
}

void print_dinstr(const Code& code, std::ostream& out) {
  std::string s;
  for (Code c = code; c; c = c->tail) {
    if (c->head.op == Op::Label) s += "L" + std::to_string(c->head.a) + ":";
    else s += instr_text(c->head) + "\n";
  }
  // strip trailing whitespace
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  out << s << "\n";
}

}  // namespace cppcaml::bytecode
