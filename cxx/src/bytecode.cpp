// Bytegen: lower the Lambda IR to the stack+accumulator VM's instruction stream
// (a faithful port of bytecomp/bytegen.ml), validated against `ocamlc -dinstr`.
// The continuation-passing structure and its peephole helpers (add_pop,
// label_code, make_branch, discard_dead_code) are what make the output
// byte-identical, so they are reproduced exactly.
#include "cppcaml/bytecode.hpp"

#include <cctype>
#include <cstring>
#include <functional>
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
  // a static-catch handler: label + stack level + enclosing-try depth at the
  // catch (a raise inside deeper try blocks must Poptrap through each)
  struct sz_lbl { int lbl; int sz; size_t tb_depth; };
  std::unordered_map<int, sz_lbl> static_lbl_;  // exit id -> handler (ids are unique)
  std::vector<int> try_blocks_;  // stack size at each nested try block entry
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
  int max_stack_ = 0;  // deepest stack slot used in the current code block (bytegen's
                       // max_stack_used), for the caml_ensure_stack_capacity prologue.

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
    auto z = lambda::lam_alloc(); z->k = Lam::K::ConstInt; z->int_val = 0;
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
      case K::Mutvar:
        if (!bound.count(e->var.stamp) && !out.count(e->var.stamp)) out[e->var.stamp] = e->var;
        return;
      case K::Assign:  // reads the assigned variable and the value expression
        if (!bound.count(e->var.stamp) && !out.count(e->var.stamp)) out[e->var.stamp] = e->var;
        fvs(e->cond, bound, out); return;
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
      case K::Letrec: {  // all the rec idents are in scope in every RHS
        std::set<int> b = bound;
        for (auto& bd : e->bindings) b.insert(bd.id.stamp);
        for (auto& bd : e->bindings) fvs(bd.val, b, out);
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
      case K::Catch: {
        fvs(e->cond, bound, out);
        std::set<int> b = bound; for (auto& v : e->catch_vars) b.insert(v.stamp);
        fvs(e->then_, b, out);
        return;
      }
      case K::Staticraise: {
        for (auto& a : e->args) fvs(a, bound, out);
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
      case Prim::Global: {  // a predef exception carries its (non-zero) Predef stamp
        Instr i = I(Op::Getglobal);
        i.str = e->var.stamp ? e->prim_id + "/" + std::to_string(e->var.stamp) : e->prim_id;
        return i;
      }
      case Prim::Field: case Prim::FieldImm: case Prim::FieldMut: case Prim::FieldInt:
        return Iop(Op::Getfield, e->prim_arg);
      case Prim::SetfieldImm: case Prim::SetfieldPtr:
        return Iop(Op::Setfield, e->prim_arg);
      case Prim::Floatfield: return Iop(Op::Getfloatfield, e->prim_arg);
      case Prim::SetFloatfield: return Iop(Op::Setfloatfield, e->prim_arg);
      case Prim::Addint: return I(Op::Addint);
      case Prim::Subint: return I(Op::Subint);
      case Prim::Mulint: return I(Op::Mulint);
      case Prim::NotEqInt: return I(Op::Neqint);
      case Prim::EqInt: return I(Op::Eqint);
      case Prim::Offsetref: return Iop(Op::Offsetref, e->prim_arg);
      case Prim::Offsetint: return Iop(Op::Offsetint, e->prim_arg);
      case Prim::FieldComputed: return I(Op::Getvectitem);     // obj.(id)
      case Prim::SetfieldComputed: return I(Op::Setvectitem);  // obj.(id) <- v
      case Prim::Ccall: return cc(bint_cname(e->prim_id), (int)e->args.size());
      case Prim::IntCmp: return intcmp_or_ccall(e);
      default: break;
    }
    // makeblock/makemutable/raise handled in comp_expr; anything else: best effort
    Instr i = I(Op::Ccall); i.str = "?"; i.a = (int)e->args.size(); return i;
  }
  // IntCmp carries a spelling: integer comparison, or a float/string/array op
  // that lowers to a C call.
  Instr cc(const std::string& name, int n) {
    Instr i = I(Op::Ccall); i.str = name; i.a = n; return i;
  }
  // A Ccall prim_id printed by printlambda as a module-qualified name maps to its
  // runtime C primitive: `Int32.add` -> caml_int32_add (shifts are renamed).
  static std::string bint_cname(const std::string& s) {
    static const std::unordered_map<std::string, std::string> op = {
      {"add", "add"}, {"sub", "sub"}, {"mul", "mul"}, {"div", "div"}, {"mod", "mod"},
      {"and", "and"}, {"or", "or"}, {"xor", "xor"}, {"neg", "neg"},
      {"lsl", "shift_left"}, {"lsr", "shift_right_unsigned"}, {"asr", "shift_right"},
      {"of_int", "of_int"}, {"to_int", "to_int"},
      {"to_int32", "to_int32"}, {"of_int32", "of_int32"},
      {"of_nativeint", "of_nativeint"}, {"to_nativeint", "to_nativeint"},
      {"bswap", "bswap"},
    };
    // Pbintcomp compiles to the *generic* compare primitives (bytegen).
    static const std::unordered_map<std::string, std::string> cmp = {
      {"==", "caml_equal"}, {"!=", "caml_notequal"},
      {"<", "caml_lessthan"}, {">", "caml_greaterthan"},
      {"<=", "caml_lessequal"}, {">=", "caml_greaterequal"},
    };
    for (const char* m : {"Int32.", "Int64.", "Nativeint."})
      if (s.rfind(m, 0) == 0) {
        std::string mod(m, strlen(m) - 1);
        for (auto& c : mod) c = (char)std::tolower((unsigned char)c);
        std::string suf = s.substr(strlen(m));
        if (auto it = op.find(suf); it != op.end()) return "caml_" + mod + "_" + it->second;
        if (auto it = cmp.find(suf); it != cmp.end()) return it->second;
        return s;
      }
    return s;
  }
  Instr intcmp_or_ccall(const LamPtr& e) {
    const std::string& s = e->prim_id;
    int n = (int)e->args.size();
    // Integer comparisons and arithmetic/bitwise that compile to a dedicated op.
    static const std::unordered_map<std::string, Op> op = {
      {"==", Op::Eqint}, {"!=", Op::Neqint}, {"<", Op::Ltint}, {">", Op::Gtint},
      {"<=", Op::Leint}, {">=", Op::Geint}, {"isint", Op::Isint},
      {"mod", Op::Modint}, {"/", Op::Divint}, {"not", Op::Boolnot}, {"~", Op::Negint},
      {"and", Op::Andint}, {"or", Op::Orint}, {"xor", Op::Xorint},
      {"lsl", Op::Lslint}, {"lsr", Op::Lsrint}, {"asr", Op::Asrint},
    };
    if (auto it = op.find(s); it != op.end()) return I(it->second);
    // Array operations: `array.<op>[<kind>]`.  The kind (int/addr/float/gen)
    // selects the spelling: unsafe int/addr access is an op, float and generic a
    // C call; the safe versions are C calls (they bounds-check / dispatch).
    if (s.rfind("array.", 0) == 0) {
      auto br = s.find('[');
      std::string o = s.substr(6, br == std::string::npos ? std::string::npos : br - 6);
      std::string k = br == std::string::npos ? "" : s.substr(br + 1, s.find(']', br) - br - 1);
      bool flt = k.rfind("float", 0) == 0, gen = k.rfind("gen", 0) == 0;
      if (o == "length") return I(Op::Vectlength);
      if (o == "unsafe_get")
        return flt ? cc("caml_floatarray_unsafe_get", n)
             : gen ? cc("caml_array_unsafe_get", n) : I(Op::Getvectitem);
      if (o == "unsafe_set")
        return flt ? cc("caml_floatarray_unsafe_set", n)
             : gen ? cc("caml_array_unsafe_set", n) : I(Op::Setvectitem);
      if (o == "get")
        return cc(flt ? "caml_floatarray_get" : gen ? "caml_array_get" : "caml_array_get_addr", n);
      if (o == "set")
        return cc(flt ? "caml_floatarray_set" : gen ? "caml_array_set" : "caml_array_set_addr", n);
    }
    // unsafe string/bytes accesses are dedicated instructions (bytegen.ml
    // Pstringrefu/Pbytesrefu/Pbytessetu), not C calls
    if (s == "string.unsafe_get") return I(Op::Getstringchar);
    if (s == "bytes.unsafe_get") return I(Op::Getbyteschar);
    if (s == "bytes.unsafe_set") return I(Op::Setbyteschar);
    static const std::unordered_map<std::string, std::string> ccall = {
      {"+.", "caml_add_float"}, {"-.", "caml_sub_float"}, {"*.", "caml_mul_float"},
      {"/.", "caml_div_float"}, {"~.", "caml_neg_float"}, {"abs.", "caml_abs_float"},
      {"<.", "caml_lt_float"}, {">.", "caml_gt_float"}, {"<=.", "caml_le_float"},
      {">=.", "caml_ge_float"}, {"==.", "caml_eq_float"}, {"!=.", "caml_neq_float"},
      {"compare_ints", "caml_int_compare"}, {"compare_floats", "caml_float_compare"},
      {"float_of_int", "caml_float_of_int"}, {"int_of_float", "caml_int_of_float"},
      {"string.length", "caml_ml_string_length"}, {"string.get", "caml_string_get"},
      {"bytes.length", "caml_ml_bytes_length"}, {"bytes.get", "caml_bytes_get"},
      {"bytes.set", "caml_bytes_set"}, {"bswap16", "caml_bswap16"},
      // 16/32/64-bit accessors: safe and unsafe call the same C entry
      // (bytegen.ml's Pstring_load/Pbytes_load/Pbytes_set/Pbigstring_*)
      {"string.get16", "caml_string_get16"}, {"string.unsafe_get16", "caml_string_get16"},
      {"string.get32", "caml_string_get32"}, {"string.unsafe_get32", "caml_string_get32"},
      {"string.get64", "caml_string_get64"}, {"string.unsafe_get64", "caml_string_get64"},
      {"bytes.get16", "caml_bytes_get16"}, {"bytes.unsafe_get16", "caml_bytes_get16"},
      {"bytes.get32", "caml_bytes_get32"}, {"bytes.unsafe_get32", "caml_bytes_get32"},
      {"bytes.get64", "caml_bytes_get64"}, {"bytes.unsafe_get64", "caml_bytes_get64"},
      {"bytes.set16", "caml_bytes_set16"}, {"bytes.unsafe_set16", "caml_bytes_set16"},
      {"bytes.set32", "caml_bytes_set32"}, {"bytes.unsafe_set32", "caml_bytes_set32"},
      {"bytes.set64", "caml_bytes_set64"}, {"bytes.unsafe_set64", "caml_bytes_set64"},
      {"bigarray.array1.get16", "caml_ba_uint8_get16"},
      {"bigarray.array1.unsafe_get16", "caml_ba_uint8_get16"},
      {"bigarray.array1.get32", "caml_ba_uint8_get32"},
      {"bigarray.array1.unsafe_get32", "caml_ba_uint8_get32"},
      {"bigarray.array1.get64", "caml_ba_uint8_get64"},
      {"bigarray.array1.unsafe_get64", "caml_ba_uint8_get64"},
      {"bigarray.array1.set16", "caml_ba_uint8_set16"},
      {"bigarray.array1.unsafe_set16", "caml_ba_uint8_set16"},
      {"bigarray.array1.set32", "caml_ba_uint8_set32"},
      {"bigarray.array1.unsafe_set32", "caml_ba_uint8_set32"},
      {"bigarray.array1.set64", "caml_ba_uint8_set64"},
      {"bigarray.array1.unsafe_set64", "caml_ba_uint8_set64"},
    };
    if (auto it = ccall.find(s); it != ccall.end()) return cc(it->second, n);
    // Bigarray accessors compile to the generic C entry points whatever the
    // kind/layout annotation (bytegen's Pbigarrayref/set/dim).
    if (s.rfind("Bigarray.", 0) == 0) {
      auto br = s.find('[');
      std::string o = s.substr(9, br == std::string::npos ? std::string::npos : br - 9);
      if (o.rfind("dim_", 0) == 0) return cc("caml_ba_dim_" + o.substr(4), n);
      bool set = o == "set" || o == "unsafe_set";
      int N = set ? n - 2 : n - 1;
      return cc((set ? "caml_ba_set_" : "caml_ba_get_") + std::to_string(N), n);
    }
    // boxed-int comparisons arrive as IntCmp with a module-qualified prim_id
    if (std::string b = bint_cname(s); b != s) return cc(b, n);
    return cc("?" + s, n);
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
    if (sz > max_stack_) max_stack_ = sz;  // check_stack
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
      case K::Mutvar: {  // read a mutable local: same as a stack variable
        auto it = env.stack.find(exp->var.stamp);
        if (it != env.stack.end()) return cons(Iop(Op::Acc, sz - it->second), cont);
        return cons(Iop(Op::Acc, 0), cont);
      }
      case K::Assign: {  // (assign x e): eval e then ASSIGN its stack slot (-> unit)
        auto it = env.stack.find(exp->var.stamp);
        int ofs = it != env.stack.end() ? sz - it->second : 0;
        return comp_expr(env, exp->cond, sz, cons(Iop(Op::Assign, ofs), cont));
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
        for (auto& id : fv) { auto v = lambda::lam_alloc(); v->k = K::Var; v->var = id; fvargs.push_back(v); }
        Instr cl = I(Op::Closure); cl.a = lbl; cl.b = (int)fv.size();
        return comp_args(env, fvargs, sz, cons(cl, cont));
      }
      case K::Letrec: {  // mutually-recursive functions -> one closurerec block
        int ndecl = (int)exp->bindings.size();
        std::set<int> bound;
        for (auto& b : exp->bindings) bound.insert(b.id.stamp);
        std::map<int, Ident> fvm;
        for (auto& b : exp->bindings) fvs(b.val, bound, fvm);
        std::vector<Ident> fv;
        for (auto& [s, id] : fvm) fv.push_back(id);
        // closure_entries Multiple_recursive: functions at 0,3,6..; free vars after
        std::map<int, std::pair<bool, int>> entries;
        for (int i = 0; i < ndecl; ++i) entries[exp->bindings[i].id.stamp] = {true, 3 * i};
        int pos_end = 3 * ndecl;
        for (size_t i = 0; i < fv.size(); ++i) entries[fv[i].stamp] = {false, pos_end - 1 + (int)i};
        std::vector<int> labels;
        for (int i = 0; i < ndecl; ++i) {
          int lbl = new_label();
          const LamPtr& fn = exp->bindings[i].val;  // a Function
          std::vector<Ident> params;
          for (auto& p : fn->params) params.push_back(p.first);
          functions_to_compile.push_back({params, fn->body, lbl, entries, i});
          labels.push_back(lbl);
        }
        Env benv = env;  // rec idents live on the stack at sz+1..sz+ndecl
        for (int i = 0; i < ndecl; ++i) benv.stack[exp->bindings[i].id.stamp] = sz + 1 + i;
        std::vector<LamPtr> fvargs;
        for (auto& id : fv) { auto v = lambda::lam_alloc(); v->k = K::Var; v->var = id; fvargs.push_back(v); }
        Instr cr = I(Op::Closurerec); cr.a = ndecl; cr.b = (int)fv.size(); cr.labels = labels;
        return comp_args(env, fvargs, sz,
                 cons(cr, comp_expr(benv, exp->body, sz + ndecl, add_pop(ndecl, cont))));
      }
      case K::Prim: {
        switch (exp->prim) {
          case Prim::Makeblock: case Prim::Makemutable: {
            Instr mb = I(Op::Makeblock); mb.a = (int)exp->args.size(); mb.b = exp->prim_arg;
            return comp_args(env, exp->args, sz, cons(mb, cont));
          }
          case Prim::Makelazyblock: {  // a 1-field block tagged Lazy(246)/Forward(250)
            Instr mb = I(Op::Makeblock); mb.a = 1; mb.b = exp->prim_arg;
            return comp_args(env, exp->args, sz, cons(mb, cont));
          }
          case Prim::Ccall:
            if (exp->prim_id == "perform" && exp->args.size() == 1) {
              // Kperform pushes 4 words (bytegen's check_stack (sz + 4))
              if (sz + 4 > max_stack_) max_stack_ = sz + 4;
              return comp_expr(env, exp->args[0], sz, cons(I(Op::Perform), cont));
            }
            // runstack/resume (stack, fn, arg): Kresume, or Kresumeterm in tail
            // position (bytegen's Prunstack/Presume).
            if (exp->prim_id == "runstack" && exp->args.size() == 3) {
              if (is_tailcall(cont)) {
                Instr rt = I(Op::Resumeterm); rt.a = sz + 2;
                return comp_args(env, exp->args, sz, cons(rt, discard_dead_code(cont)));
              }
              if (sz + 3 > max_stack_) max_stack_ = sz + 3;
              return comp_args(env, exp->args, sz, cons(I(Op::Resume), cont));
            }
            // reperform (eff, cont): tail-only Kreperformterm (resets the stack).
            if (exp->prim_id == "reperform" && exp->args.size() == 2) {
              if (3 > max_stack_) max_stack_ = 3;
              Instr rp = I(Op::Reperformterm); rp.a = sz + 1;
              return comp_args(env, exp->args, sz, cons(rp, discard_dead_code(cont)));
            }
            break;
          case Prim::Send: {
            // Method dispatch (bytegen's Lsend).  Public `(send obj tag args..)`:
            // Kgetpubmet tag over (obj :: args).  Self `(sendself self m args..)`:
            // Kgetmethod over (m :: self :: args).  Then apply to nargs (incl. obj).
            bool self_send = exp->prim_id == "sendself";
            const LamPtr& obj = exp->args[0];
            const LamPtr& met = exp->args[1];
            std::vector<LamPtr> realargs(exp->args.begin() + 2, exp->args.end());
            int nargs = (int)realargs.size() + 1;
            std::vector<LamPtr> args2;
            Instr getm;
            if (self_send) {
              getm = I(Op::Getmethod);
              args2.push_back(met); args2.push_back(obj);
            } else {
              getm = Iop(Op::Getpubmet, (int)met->int_val);
              args2.push_back(obj);
            }
            for (auto& a : realargs) args2.push_back(a);
            if (is_tailcall(cont)) {
              Instr at = I(Op::Appterm); at.a = nargs; at.b = sz + nargs;
              return comp_args(env, args2, sz, cons(getm, cons(at, discard_dead_code(cont))));
            }
            if (nargs < 4)
              return comp_args(env, args2, sz, cons(getm, cons(Iop(Op::Apply, nargs), cont)));
            auto [lbl, cont1] = label_code(cont);
            return cons(Iop(Op::PushRetaddr, lbl),
                     comp_args(env, args2, sz + 3,
                       cons(getm, cons(Iop(Op::Apply, nargs), cont1))));
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
          case Prim::IntCmp:
            if (exp->prim_id == "&&" && exp->args.size() == 2)
              return comp_seq_and(env, exp->args[0], exp->args[1], sz, cont);
            if (exp->prim_id == "||" && exp->args.size() == 2)
              return comp_seq_or(env, exp->args[0], exp->args[1], sz, cont);
            if (exp->prim_id == "ignore" && exp->args.size() == 1)
              // (ignore x): evaluate x for effect; the unit result is elided when
              // the continuation immediately reloads the accumulator (add_const_unit).
              return comp_expr(env, exp->args[0], sz, add_const_unit(cont));
            if (exp->prim_id == "opaque" && exp->args.size() == 1)
              return comp_expr(env, exp->args[0], sz, cont);  // identity: no instruction
            if (exp->prim_id == "isout" && exp->args.size() == 2)
              // (isout span arg): push arg, span in accu, then ULTINT -- the
              // operand order the BOUNDSWITCH/branch-fusion peephole expects
              return comp_expr(env, exp->args[1], sz,
                       cons(I(Op::Push),
                         comp_expr(env, exp->args[0], sz + 1,
                           cons(I(Op::Isout), cont))));
            // (Pintcomp c [arg; const]) -> reorder to [const; arg] and swap the
            // comparison, so the constant lands in the accumulator (matches bytegen,
            // which does this to enable the emitcode branch/compare fusion).
            {
              static const std::unordered_map<std::string, std::string> swap_cmp = {
                {"==", "=="}, {"!=", "!="}, {"<", ">"}, {">", "<"}, {"<=", ">="}, {">=", "<="}};
              auto sw = swap_cmp.find(exp->prim_id);
              if (sw != swap_cmp.end() && exp->args.size() == 2 &&
                  (exp->args[1]->k == K::ConstInt || exp->args[1]->k == K::ConstChar)) {
                auto e2 = lambda::lam_alloc_copy(*exp);
                e2->prim_id = sw->second;
                e2->args = {exp->args[1], exp->args[0]};
                return comp_args(env, e2->args, sz, cons(comp_primitive(e2), cont));
              }
            }
            if (exp->prim_id.rfind("makearray", 0) == 0) {  // [| .. |] -> a block
              if (exp->args.empty()) {  // [||]: an empty block (ATOM0), not const 0
                Instr mb = I(Op::Makeblock); mb.a = 0; mb.b = 0;
                return cons(mb, cont);
              }
              bool flt = exp->prim_id.find("[float") != std::string::npos;
              Instr mb = I(flt ? Op::Makefloatblock : Op::Makeblock);
              mb.a = (int)exp->args.size(); mb.b = 0;
              return comp_args(env, exp->args, sz, cons(mb, cont));
            }
            break;
          default: break;
        }
        // generic: comp_args then the closing primitive instruction
        int nargs = (int)exp->args.size() - 1;
        (void)nargs;
        return comp_args(env, exp->args, sz, cons(comp_primitive(exp), cont));
      }
      case K::While: {
        int lbl_loop = new_label();
        int lbl_test = new_label();
        return cons(Iop(Op::Branch, lbl_test), cons(Iop(Op::Label, lbl_loop),
          cons(I(Op::CheckSignals),
            comp_expr(env, exp->body, sz,
              cons(Iop(Op::Label, lbl_test),
                comp_expr(env, exp->cond, sz,
                  cons(Iop(Op::Branchif, lbl_loop), add_const_unit(cont))))))));
      }
      case K::For: {
        int lbl_loop = new_label();
        int lbl_exit = new_label();
        int offset = exp->downto_ ? -1 : 1;
        Op comp = exp->downto_ ? Op::Ltint : Op::Gtint;
        Env body_env = add_var(exp->var, sz + 1, env);
        // body continuation: bump the counter, test against the limit, re-loop.
        Code after = add_const_unit(add_pop(2, cont));
        after = cons(Iop(Op::Label, lbl_exit), after);
        after = cons(Iop(Op::Branchif, lbl_loop), after);
        after = cons(I(Op::Neqint), after);
        after = cons(Iop(Op::Acc, 1), after);
        after = cons(Iop(Op::Assign, 2), after);
        after = cons(Iop(Op::Offsetint, offset), after);
        after = cons(I(Op::Push), after);
        after = cons(Iop(Op::Acc, 1), after);
        Code mid = cons(I(Op::CheckSignals), comp_expr(body_env, exp->body, sz + 2, after));
        mid = cons(Iop(Op::Label, lbl_loop), mid);
        mid = cons(Iop(Op::Branchif, lbl_exit), mid);
        mid = cons(I(comp), mid);
        mid = cons(Iop(Op::Acc, 2), mid);
        mid = cons(I(Op::Push), mid);
        mid = cons(I(Op::Push), mid);
        Code start_cont = cons(I(Op::Push), comp_expr(env, exp->else_, sz + 1, mid));
        return comp_expr(env, exp->then_, sz, start_cont);
      }
      case K::Try: {
        auto [branch1, cont1] = make_branch(cont);
        int lbl_handler = new_label();
        Env henv = add_var(exp->var, sz + 1, env);
        Code body_cont = cons(I(Op::Poptrap), cons(branch1,
          cons(Iop(Op::Label, lbl_handler), cons(I(Op::Push),
            comp_expr(henv, exp->then_, sz + 1, add_pop(1, cont1))))));
        try_blocks_.push_back(sz);  // a static raise from the body must Poptrap
        Code body = comp_expr(env, exp->body, sz + 4, body_cont);
        try_blocks_.pop_back();
        return cons(Iop(Op::Pushtrap, lbl_handler), body);
      }
      case K::Catch: {
        // Static catch: a label the body's (exit N ..) branches to; not a trap.
        // Handler vars live in stack slots: nvars dummies pushed under the body,
        // assigned by the raise site (nvars==1 passes the value in the accu).
        auto [branch1, cont1] = make_branch(cont);
        int nvars = (int)exp->catch_vars.size();
        int lbl = new_label();
        if (nvars == 0) {
          static_lbl_[exp->prim_arg] = sz_lbl{lbl, sz, try_blocks_.size()};
          Code hcode = cons(Iop(Op::Label, lbl), comp_expr(env, exp->then_, sz, cont1));
          return comp_expr(env, exp->cond, sz, cons(branch1, hcode));
        }
        if (nvars == 1) {
          static_lbl_[exp->prim_arg] = sz_lbl{lbl, sz, try_blocks_.size()};
          Env henv = add_var(exp->catch_vars[0], sz + 1, env);
          Code hcode = cons(Iop(Op::Label, lbl), cons(I(Op::Push),
              comp_expr(henv, exp->then_, sz + 1, add_pop(1, cont1))));
          return comp_expr(env, exp->cond, sz, cons(branch1, hcode));
        }
        static_lbl_[exp->prim_arg] = sz_lbl{lbl, sz + nvars, try_blocks_.size()};
        Env henv = env;
        for (int i = 0; i < nvars; ++i) henv = add_var(exp->catch_vars[i], sz + 1 + i, henv);
        Code hcode = cons(Iop(Op::Label, lbl),
            comp_expr(henv, exp->then_, sz + nvars, add_pop(nvars, cont1)));
        Code body = comp_expr(env, exp->cond, sz + nvars,
                              add_pop(nvars, cons(branch1, hcode)));
        for (int i = 0; i < nvars; ++i) {  // push_dummies
          auto z = lambda::lam_alloc(); z->k = K::ConstInt; z->int_val = 0;
          Instr c = I(Op::Const); c.cst = z;
          body = cons(c, cons(I(Op::Push), body));
        }
        return body;
      }
      case K::Staticraise: {
        // (exit N args..): unwind any try blocks entered since the catch
        // (pop to each trap's level + Poptrap), pop to the catch's stack level,
        // then branch.  One arg travels in the accumulator; several assign into
        // the catch's reserved slots.
        auto it = static_lbl_.find(exp->prim_arg);
        if (it == static_lbl_.end()) {  // shouldn't happen; degrade to unit
          auto z = lambda::lam_alloc(); z->k = K::ConstInt;
          Instr c = I(Op::Const); c.cst = z; return cons(c, cont);
        }
        Code c0 = branch_to(it->second.lbl, discard_dead_code(cont));
        std::vector<int> tbs(try_blocks_.begin() + it->second.tb_depth, try_blocks_.end());
        std::function<Code(int, int)> unwind = [&](int s, int i) -> Code {
          if (i == 0) return add_pop(s - it->second.sz, c0);
          int tsz = tbs[i - 1];
          return add_pop(s - tsz - 4, cons(I(Op::Poptrap), unwind(tsz, i - 1)));
        };
        Code tail = unwind(sz, (int)tbs.size());
        if (exp->args.size() == 1)  // optim: the argument travels in the accu
          return comp_expr(env, exp->args[0], sz, tail);
        if (!exp->args.empty()) {
          // comp_exit_args: args reversed, each assigned to its reserved slot
          std::function<Code(size_t, int)> assign = [&](size_t idx, int pos) -> Code {
            if (idx == exp->args.size()) return tail;
            return comp_expr(env, exp->args[exp->args.size() - 1 - idx], sz,
                             cons(Iop(Op::Assign, sz - pos), assign(idx + 1, pos - 1)));
          };
          return assign(0, it->second.sz);
        }
        return tail;
      }
      case K::Switch: return comp_switch(env, exp, sz, cont);
      default:
        { auto z = lambda::lam_alloc(); z->k = K::ConstInt; z->int_val = 0;
          Instr k = I(Op::Const); k.cst = z; return cons(k, cont); }
    }
  }

  // Lswitch: compile each action behind a label, then a Kswitch over the
  // const/block tag vectors.  (No action sharing -- our switches have distinct
  // arms -- and the only failaction case is sw_default.)
  Code comp_switch(const Env& env, const LamPtr& exp, int sz, Code cont) {
    auto [branch, cont1] = make_branch(cont);
    Code c = discard_dead_code(cont1);
    int nconsts = (int)exp->sw_consts.size();
    int nblocks = (int)exp->sw_blocks.size();
    std::vector<int> lbl_consts(nconsts, 0), lbl_blocks(nblocks, 0);
    // actions in reverse: blocks (high tag first) then consts, so the lowest
    // const tag's code ends up first -- matching the reverse loop in bytegen.
    for (int i = nblocks - 1; i >= 0; --i) {
      auto [lbl, c1] = label_code(comp_expr(env, exp->sw_blocks[i].body, sz, cons(branch, c)));
      lbl_blocks[i] = lbl; c = discard_dead_code(c1);
    }
    for (int i = nconsts - 1; i >= 0; --i) {
      auto [lbl, c1] = label_code(comp_expr(env, exp->sw_consts[i].body, sz, cons(branch, c)));
      lbl_consts[i] = lbl; c = discard_dead_code(c1);
    }
    Instr sw = I(Op::Switch);
    sw.nconsts = nconsts;
    sw.labels = lbl_consts;
    sw.labels.insert(sw.labels.end(), lbl_blocks.begin(), lbl_blocks.end());
    return comp_expr(env, exp->cond, sz, cons(sw, c));
  }

  // Short-circuit && / || (Psequand / Psequor).
  Code comp_seq_and(const Env& env, const LamPtr& e1, const LamPtr& e2, int sz, Code cont) {
    if (auto* h = head(cont); h && h->op == Op::Branchifnot) {
      int lbl = h->a;
      return comp_expr(env, e1, sz, cons(Iop(Op::Branchifnot, lbl), comp_expr(env, e2, sz, cont)));
    }
    if (auto* h = head(cont); h && h->op == Op::Branchif) {
      int lbl = h->a;
      auto [lbl2, cont2] = label_code(cont->tail);
      return comp_expr(env, e1, sz, cons(Iop(Op::Branchifnot, lbl2),
                       comp_expr(env, e2, sz, cons(Iop(Op::Branchif, lbl), cont2))));
    }
    auto [lbl, cont1] = label_code(cont);
    return comp_expr(env, e1, sz, cons(Iop(Op::Strictbranchifnot, lbl), comp_expr(env, e2, sz, cont1)));
  }
  Code comp_seq_or(const Env& env, const LamPtr& e1, const LamPtr& e2, int sz, Code cont) {
    if (auto* h = head(cont); h && h->op == Op::Branchif) {
      int lbl = h->a;
      return comp_expr(env, e1, sz, cons(Iop(Op::Branchif, lbl), comp_expr(env, e2, sz, cont)));
    }
    if (auto* h = head(cont); h && h->op == Op::Branchifnot) {
      int lbl = h->a;
      auto [lbl2, cont2] = label_code(cont->tail);
      return comp_expr(env, e1, sz, cons(Iop(Op::Branchif, lbl2),
                       comp_expr(env, e2, sz, cons(Iop(Op::Branchifnot, lbl), cont2))));
    }
    auto [lbl, cont1] = label_code(cont);
    return comp_expr(env, e1, sz, cons(Iop(Op::Strictbranchif, lbl), comp_expr(env, e2, sz, cont1)));
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
    int saved = max_stack_;
    max_stack_ = 0;
    Code code = comp_expr(env, exp, sz, cont);
    int used_safe = max_stack_ + 6;  // Config.stack_safety_margin
    max_stack_ = saved;
    if (used_safe > 32) {  // Config.stack_threshold: deep blocks check capacity first
      auto n = lambda::lam_alloc(); n->k = Lam::K::ConstInt; n->int_val = used_safe;
      Instr kconst = I(Op::Const); kconst.cst = n;
      return cons(kconst, cons(cc("caml_ensure_stack_capacity", 1), code));
    }
    return code;
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
    case Op::Closurerec: {  // printinstr: closurerec <lbl...>, <nfv>  (bare label nums)
      std::string s = "\tclosurerec";
      for (int lbl : i.labels) s += " " + n(lbl);
      return s + ", " + n(i.b);
    }
    case Op::Offsetclosure: return "\toffsetclosure " + n(i.a);
    case Op::Getglobal: return "\tgetglobal " + i.str + "!";
    case Op::Setglobal: return "\tsetglobal " + i.str + "!";
    case Op::Const: return lambda::const_instruction(i.cst);
    case Op::Makeblock: return "\tmakeblock " + n(i.a) + ", " + n(i.b);
    case Op::Makefloatblock: return "\tmakefloatblock " + n(i.a);
    case Op::Getfield: return "\tgetfield " + n(i.a);
    case Op::Setfield: return "\tsetfield " + n(i.a);
    case Op::Vectlength: return "\tvectlength";
    case Op::Getvectitem: return "\tgetvectitem";
    case Op::Setvectitem: return "\tsetvectitem";
    case Op::Getmethod: return "\tgetmethod";
    case Op::Getpubmet: return "\tgetpubmet " + n(i.a);
    case Op::Getdynmet: return "\tgetdynmet";
    case Op::Getfloatfield: return "\tgetfloatfield " + n(i.a);
    case Op::Setfloatfield: return "\tsetfloatfield " + n(i.a);
    case Op::Getstringchar: return "\tgetstringchar";
    case Op::Getbyteschar: return "\tgetbyteschar";
    case Op::Setbyteschar: return "\tsetbyteschar";
    case Op::Branch: return "\tbranch L" + n(i.a);
    case Op::Branchif: return "\tbranchif L" + n(i.a);
    case Op::Branchifnot: return "\tbranchifnot L" + n(i.a);
    case Op::Strictbranchif: return "\tstrictbranchif L" + n(i.a);
    case Op::Strictbranchifnot: return "\tstrictbranchifnot L" + n(i.a);
    case Op::Switch: {  // printinstr: \tswitch <consts.../<blocks...  (bare nums)
      std::string s = "\tswitch";
      for (int k = 0; k < i.nconsts; ++k) s += " " + n(i.labels[k]);
      s += "/";
      for (size_t k = i.nconsts; k < i.labels.size(); ++k) s += " " + n(i.labels[k]);
      return s;
    }
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
    case Op::Perform: return "\tperform";
    case Op::Resume: return "\tresume";
    case Op::Resumeterm: return "\tresumeterm " + n(i.a);
    case Op::Reperformterm: return "\treperformterm " + n(i.a);
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
