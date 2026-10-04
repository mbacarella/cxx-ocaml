// Port of middle_end/flambda/closure_conversion.ml and
// closure_conversion_aux.ml (see closure_conversion.hpp).
//
// Fresh variables (Variable's one stamp counter), set-of-closures ids,
// static exceptions and idents are created in ocamlopt's order: OCaml
// evaluates a constructor's or an application's arguments right to left
// and a record's fields right to left in definition order, so the port
// closes, say, a switch's failaction before its blocks and consts, and an
// [if]'s else branch before its then branch.  Each site says so.
#include "cppcaml/typing/closure_conversion.hpp"

#include <vector>

#include "cppcaml/typing/arg_helper.hpp"
#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/compilenv.hpp"
#include "cppcaml/typing/config.hpp"
#include "cppcaml/typing/convert_primitives.hpp"
#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/lift_code.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/simplif.hpp"

namespace cppcaml::typing::closure_conversion {

namespace L = lambda;
namespace F = flambda;
namespace Names = internal_variable_names;
using F::named;
using lift_code::EvaluationOrder;

namespace {

// Backend_intf.S for amd64 (Arch.size_int, Arch.big_endian)
constexpr long size_int = 8;
constexpr bool big_endian = false;
constexpr const char* target_os_type = "Unix";  // Config.target_os_type

// ---- Closure_conversion_aux.Env -------------------------------------------
struct Env {
  L::IdentPMap<variable::t> variables;          // Variable.t Ident.tbl
  L::IdentPMap<variable::t> mutable_variables;  // Mutable_variable.t Ident.tbl
  static_exception::Map<static_exception::t> static_exceptions;  // Numbers.Int.Map
  // (globals: never added to)

  static Env empty() { return {}; }
  // clear_local_bindings env = { empty with globals = env.globals }
  Env clear_local_bindings() const { return {}; }
  Env add_var(Ident::t id, variable::t var) const {
    Env e = *this;
    e.variables = variables.add(id, var);
    return e;
  }
  // add_vars t ids vars (List.fold_left2)
  Env add_vars(const std::vector<Ident::t>& ids, const std::vector<variable::t>& vars) const {
    Env e = *this;
    for (std::size_t k = 0; k < ids.size(); ++k) e = e.add_var(ids[k], vars[k]);
    return e;
  }
  variable::t find_var(Ident::t id) const {
    if (const variable::t* v = variables.find_opt(id)) return *v;
    misc::fatal_error("Closure_conversion.Env.find_var: " + ident::unique_name(id));
  }
  const variable::t* find_var_exn(Ident::t id) const { return variables.find_opt(id); }
  Env add_mutable_var(Ident::t id, variable::t mutable_var) const {
    Env e = *this;
    e.mutable_variables = mutable_variables.add(id, mutable_var);
    return e;
  }
  const variable::t* find_mutable_var_exn(Ident::t id) const { return mutable_variables.find_opt(id); }
  Env add_static_exception(long st_exn, static_exception::t fresh_st_exn) const {
    Env e = *this;
    e.static_exceptions = static_exceptions.add(st_exn, fresh_st_exn);
    return e;
  }
  static_exception::t find_static_exception(long st_exn) const {
    if (const static_exception::t* s = static_exceptions.find_opt(st_exn)) return *s;
    misc::fatal_error("Closure_conversion.Env.find_static_exception: exn " + std::to_string(st_exn));
  }
};

// ---- Closure_conversion_aux.Function_decls ----------------------------------
struct FunctionDecl {
  Ident::t let_rec_ident;
  variable::t closure_bound_var;
  L::FunctionKind kind;
  std::vector<Ident::t> params;
  L::lambda body;
  L::IdentSet free_idents_of_body;
  L::FunctionAttribute attr;
  L::ScopedLocation loc;
};

// Function_decl.create ~let_rec_ident ~closure_bound_var ~kind ~params
//   ~body ~attr ~loc
FunctionDecl function_decl_create(Ident::t let_rec_ident, variable::t closure_bound_var, L::FunctionKind kind,
                                  Slice<L::Param> params, L::lambda body, const L::FunctionAttribute& attr,
                                  const L::ScopedLocation& loc) {
  // (the record's fields right to left: Lambda.free_variables before the
  // fresh ident -- neither has an effect on the other)
  if (!let_rec_ident) let_rec_ident = Ident::create_local(OCAML_LIT("unnamed_function"));
  FunctionDecl d{let_rec_ident, closure_bound_var, kind, {}, body, L::free_variables(body), attr, loc};
  for (const L::Param& p : params) d.params.push_back(p.id);
  return d;
}

struct FunctionDecls {
  std::vector<FunctionDecl> function_decls;
  L::IdentSet all_free_idents;
};

// Function_decls.create function_decls
FunctionDecls function_decls_create(std::vector<FunctionDecl> decls) {
  // all_free_idents: the free idents of the bodies (Variable.Map.fold
  // Ident.Set.union over free_idents_by_function), minus all the
  // parameters, minus the let-rec idents
  L::IdentSet all;
  for (const FunctionDecl& d : decls) all.insert(d.free_idents_of_body.begin(), d.free_idents_of_body.end());
  for (const FunctionDecl& d : decls)
    for (Ident::t p : d.params) all.erase(p);
  for (const FunctionDecl& d : decls) all.erase(d.let_rec_ident);
  return {std::move(decls), std::move(all)};
}

// Function_decls.closure_env_without_parameters external_env t
Env closure_env_without_parameters(const Env& external_env, const FunctionDecls& t) {
  // For "let rec"-bound functions.  (List.fold_right: no effect)
  Env closure_env = external_env.clear_local_bindings();
  for (std::size_t k = t.function_decls.size(); k-- > 0;)
    closure_env = closure_env.add_var(t.function_decls[k].let_rec_ident, t.function_decls[k].closure_bound_var);
  // For free variables.  (Ident.Set.fold: in increasing order)
  for (Ident::t id : t.all_free_idents) closure_env = closure_env.add_var(id, variable::create_with_same_name_as_ident(id));
  return closure_env;
}

// ---- Closure_conversion ------------------------------------------------------------
struct T {
  Ident::t current_unit_id;
  symbol::Set imported_symbols;
  std::vector<F::SymbolBinding> declared_symbols;  // (consed: the newest last)
};

clambda::Primitive cl_prim(clambda::Primitive::K k) { return clambda::prim(k); }

F::t name_expr(named n, Names::t name) { return flambda_utils::name_expr(n, name); }

L::lambda add_default_argument_wrappers(L::lambda lam) {
  auto f = [](L::lambda l) -> L::lambda {
    if (auto* x = L::as<L::Llet>(l)) {
      auto* fn = L::as<L::Lfunction>(x->arg);
      if (!fn) return l;
      const L::LFunction* d = fn->f;
      Slice<L::RecBinding> r =
          simplif::split_default_wrapper(x->id, d->kind, d->params, L::ValueKind::gen(), d->body, d->attr, d->loc);
      if (r.size() == 1) return L::llet(L::LetKind::Alias, L::ValueKind::gen(), r[0].id, L::lfunction_node(r[0].def), x->body);
      if (r.size() == 2)
        return L::llet(L::LetKind::Alias, L::ValueKind::gen(), r[1].id, L::lfunction_node(r[1].def),
                       L::llet(L::LetKind::Alias, L::ValueKind::gen(), r[0].id, L::lfunction_node(r[0].def), x->body));
      misc::fatal_error("Closure_conversion.add_default_argument_wrappers");
    }
    if (auto* x = L::as<L::Lletrec>(l)) {
      // List.flatten (List.map ... defs): left to right
      std::vector<L::RecBinding> defs;
      for (const L::RecBinding& b : x->decl) {
        const L::LFunction* d = b.def;
        Slice<L::RecBinding> r =
            simplif::split_default_wrapper(b.id, d->kind, d->params, L::ValueKind::gen(), d->body, d->attr, d->loc);
        defs.insert(defs.end(), r.begin(), r.end());
      }
      return L::lletrec(slice(defs), x->body);
    }
    return l;
  };
  return L::map(f, lam);
}

// Generate a wrapper ("stub") function that accepts a tuple argument and
// calls another function with arguments extracted in the obvious manner
// from the tuple.
const F::FunctionDeclaration* tupled_function_call_stub(const std::vector<variable::t>& original_params,
                                                        variable::t unboxed_version, variable::t closure_bound_var) {
  variable::t tuple_param_var = variable::rename(unboxed_version);
  std::vector<variable::t> params;  // List.map: left to right
  for (variable::t p : original_params) params.push_back(variable::rename(p));
  F::t call = F::apply(unboxed_version, slice(params), F::CallKind{unboxed_version}, debuginfo::none(),
                       L::InlineAttribute{}, L::SpecialiseAttribute::Default_specialise);
  F::t body = call;
  for (std::size_t pos = 0; pos < params.size(); ++pos) {
    clambda::Primitive p = cl_prim(clambda::Primitive::K::Pfield);
    p.n = static_cast<long>(pos);
    p.ptr = L::ImmediateOrPointer::Pointer;
    p.mut = MutableFlag::Mutable;
    named lam = F::n_prim(p, slice(std::vector<variable::t>{tuple_param_var}), debuginfo::none());
    body = F::create_let(params[pos], lam, body);
  }
  Parameter tuple_param = parameter::wrap(tuple_param_var);
  return F::create_function_declaration(slice(std::vector<Parameter>{tuple_param}), body, true, debuginfo::none(),
                                        L::InlineAttribute{}, L::SpecialiseAttribute::Default_specialise, false,
                                        closure_bound_var,  // Closure_origin.create (Closure_id.wrap ...)
                                        L::PollAttribute::Default_poll);  // don't propagate attribute to wrappers
}

double float_of_string(std::string_view s) {
  double d;
  if (!arg_helper::float_of_string_opt(std::string(s), d)) misc::fatal_error("float_of_string");
  return d;
}

std::pair<F::BlockField, Names::t> register_const(T& t, F::constant_defining_value constant, Names::t name) {
  variable::t var = variable::create(name);
  symbol::t sym = symbol::of_variable(var);
  t.declared_symbols.push_back({sym, constant});
  F::BlockField f;
  f.sym = sym;
  return {f, name};
}

F::constant_defining_value cdv_allocated(allocated_const::t c) {
  F::ConstantDefiningValue v{F::ConstantDefiningValue::Kind::Allocated_const};
  v.c = c;
  return make<F::ConstantDefiningValue>(v);
}

std::pair<F::BlockField, Names::t> declare_const(T& t, const L::StructuredConstant* c) {
  using CK = L::StructuredConstant::Kind;
  switch (c->kind) {
    case CK::Const_int: {
      F::BlockField f;
      f.c = F::const_int(c->i);
      return {f, Names::const_int};
    }
    case CK::Const_char: {
      F::BlockField f;
      f.c = F::Const{F::Const::Kind::Char, c->i};
      return {f, Names::const_char};
    }
    case CK::Const_float:
      return register_const(t, cdv_allocated(allocated_const::float_(float_of_string(c->s))), Names::const_float);
    case CK::Const_int32:
      return register_const(t, cdv_allocated(allocated_const::int32(static_cast<std::int32_t>(c->boxed))),
                            Names::const_int32);
    case CK::Const_int64:
      return register_const(t, cdv_allocated(allocated_const::int64(c->boxed)), Names::const_int64);
    case CK::Const_nativeint:
      return register_const(t, cdv_allocated(allocated_const::nativeint(c->boxed)), Names::const_nativeint);
    case CK::Const_immstring:
      return register_const(t, cdv_allocated(allocated_const::immutable_string(c->s)), Names::const_immstring);
    case CK::Const_float_array: {
      std::vector<double> fl;  // List.map float_of_string
      for (std::string_view s : c->floats) fl.push_back(float_of_string(s));
      return register_const(t, cdv_allocated(allocated_const::immutable_float_array(slice(fl))),
                            Names::const_float_array);
    }
    case CK::Const_block: {
      // Block (Tag.create_exn tag, List.map (fun c -> fst (declare_const t c)) consts)
      std::vector<F::BlockField> fields;
      for (const L::StructuredConstant* f : c->fields) fields.push_back(declare_const(t, f).first);
      F::ConstantDefiningValue v{F::ConstantDefiningValue::Kind::Block};
      v.tag = tag::create_exn(c->i);
      v.fields = slice(fields);
      return register_const(t, make<F::ConstantDefiningValue>(v), Names::const_block);
    }
  }
  misc::fatal_error("Closure_conversion.declare_const");
}

std::pair<named, Names::t> close_const(T& t, const L::StructuredConstant* c) {
  auto [field, name] = declare_const(t, c);
  if (field.sym) return {F::n_symbol(field.sym), name};
  return {F::n_const(field.c), name};
}

struct Closer {
  T& t;

  F::t close(const Env& env, L::lambda lam);
  named close_functions(const Env& external_env, const FunctionDecls& function_declarations);
  std::vector<F::t> close_list(const Env& env, Slice<L::lambda> l) {
    std::vector<F::t> out;  // List.map: left to right
    out.reserve(l.size());
    for (L::lambda x : l) out.push_back(close(env, x));
    return out;
  }
  named close_let_bound_expression(variable::t let_bound_var, const Env& env, L::lambda lam);

  F::t close_prim(const Env& env, const L::Lprim* x);
};

F::t Closer::close(const Env& env, L::lambda lam) {
  switch (lam->kind) {
    case L::LK::Lvar: {
      Ident::t id = L::as<L::Lvar>(lam)->id;
      if (const variable::t* v = env.find_var_exn(id)) return F::var(*v);
      misc::fatal_error("Closure_conversion.close: unbound identifier " + ident::unique_name(id));
    }
    case L::LK::Lmutvar: {
      Ident::t id = L::as<L::Lmutvar>(lam)->id;
      if (const variable::t* v = env.find_mutable_var_exn(id)) return name_expr(F::n_read_mutable(*v), Names::read_mutable);
      misc::fatal_error("Closure_conversion.close: unbound mutable identifier " + ident::unique_name(id));
    }
    case L::LK::Lconst: {
      auto [cst, name] = close_const(t, L::as<L::Lconst>(lam)->c);
      return name_expr(cst, name);
    }
    case L::LK::Llet: {
      // (Strict | Alias | StrictOpt): TODO: keep value_kind in flambda
      auto* x = L::as<L::Llet>(lam);
      variable::t var = variable::create_with_same_name_as_ident(x->id);
      named defining_expr = close_let_bound_expression(var, env, x->arg);
      F::t body = close(env.add_var(x->id, var), x->body);
      return F::create_let(var, defining_expr, body);
    }
    case L::LK::Lmutlet: {
      auto* x = L::as<L::Lmutlet>(lam);
      variable::t mut_var = variable::create_with_same_name_as_ident(x->id);  // Mutable_variable
      variable::t var = variable::create_with_same_name_as_ident(x->id);
      named defining_expr = close_let_bound_expression(var, env, x->arg);
      F::t body = close(env.add_mutable_var(x->id, mut_var), x->body);
      return F::create_let(var, defining_expr, F::let_mutable(mut_var, var, x->k, body));
    }
    case L::LK::Lfunction: {
      const L::LFunction* fn = L::as<L::Lfunction>(lam)->f;
      Names::t name = Names::anon_fn_with_loc(fn->loc);
      variable::t closure_bound_var = variable::create(name);
      // CR-soon mshinwell: some of this is now very similar to the let rec
      // case below
      variable::t set_of_closures_var = variable::create(Names::set_of_closures);
      FunctionDecl decl =
          function_decl_create(nullptr, closure_bound_var, fn->kind, fn->params, fn->body, fn->attr, fn->loc);
      std::vector<FunctionDecl> decls;
      decls.push_back(std::move(decl));
      named set_of_closures = close_functions(env, function_decls_create(std::move(decls)));
      projection::ProjectClosure project_closure{set_of_closures_var, closure_bound_var};
      return F::create_let(set_of_closures_var, set_of_closures,
                           name_expr(F::n_project_closure(project_closure), name));
    }
    case L::LK::Lapply: {
      const L::LambdaApply& ap = L::as<L::Lapply>(lam)->ap;
      return lift_code::lifting_helper(
          close_list(env, ap.ap_args), EvaluationOrder::Right_to_left,
          [&](Slice<variable::t> args) {
            F::t func = close(env, ap.ap_func);
            variable::t func_var = variable::create(Names::apply_funct);
            return F::create_let(func_var, F::n_expr(func),
                                 F::apply(func_var, args, F::CallKind{}, debuginfo::from_location(ap.ap_loc),
                                          ap.ap_inlined, ap.ap_specialised));
          },
          Names::apply_arg);
    }
    case L::LK::Lletrec: {
      auto* x = L::as<L::Lletrec>(lam);
      // List.fold_right: the last binding's variable first
      Env env2 = env;
      for (std::size_t k = x->decl.size(); k-- > 0;)
        env2 = env2.add_var(x->decl[k].id, variable::create_with_same_name_as_ident(x->decl[k].id));
      // Name functions (List.map: left to right)
      std::vector<FunctionDecl> function_declarations;
      for (const L::RecBinding& b : x->decl) {
        variable::t closure_bound_var = variable::create_with_same_name_as_ident(b.id);
        const L::LFunction* d = b.def;
        function_declarations.push_back(
            function_decl_create(b.id, closure_bound_var, d->kind, d->params, d->body, d->attr, d->loc));
      }
      variable::t set_of_closures_var = variable::create(Names::set_of_closures);
      FunctionDecls fdecls = function_decls_create(function_declarations);
      named set_of_closures = close_functions(env2, fdecls);
      // List.fold_left ... (close t env body) function_declarations
      F::t body = close(env2, x->body);
      for (const FunctionDecl& decl : fdecls.function_decls) {
        variable::t let_bound_var = env2.find_var(decl.let_rec_ident);
        // Inside the body of the [let], each function is referred to by a
        // [Project_closure] expression, which projects from the set of
        // closures.
        body = F::create_let(let_bound_var,
                             F::n_project_closure(projection::ProjectClosure{set_of_closures_var, decl.closure_bound_var}),
                             body);
      }
      return F::create_let(set_of_closures_var, set_of_closures, body);
    }
    case L::LK::Lsend: {
      auto* x = L::as<L::Lsend>(lam);
      variable::t meth_var = variable::create(Names::meth);
      variable::t obj_var = variable::create(Names::obj);
      debuginfo::t dbg = debuginfo::from_location(x->loc);
      // create_let meth_var (Expr (close meth)) (create_let obj_var (Expr
      // (close obj)) (lifting_helper (close_list args) ...)): the
      // arguments, then obj, then meth
      F::t inner = lift_code::lifting_helper(
          close_list(env, x->args), EvaluationOrder::Right_to_left,
          [&](Slice<variable::t> args) { return F::send(x->k, meth_var, obj_var, args, dbg); }, Names::send_arg);
      F::t obj = close(env, x->obj);
      F::t with_obj = F::create_let(obj_var, F::n_expr(obj), inner);
      F::t meth = close(env, x->met);
      return F::create_let(meth_var, F::n_expr(meth), with_obj);
    }
    case L::LK::Lprim: return close_prim(env, L::as<L::Lprim>(lam));
    case L::LK::Lswitch: {
      auto* x = L::as<L::Lswitch>(lam);
      const L::LambdaSwitch& sw = x->sw;
      variable::t scrutinee = variable::create(Names::switch_);
      auto nums = [](long sw_num, Slice<L::SwitchCase> cases, L::lambda def) {
        F::IntSet s;
        if (def) {
          // Numbers.Int.zero_to_n (sw_num - 1)
          std::vector<long> rev;
          for (long n = sw_num - 1; n >= 0; --n) rev.push_back(n);
          for (std::size_t k = rev.size(); k-- > 0;) s = s.add(rev[k]);
          return s;
        }
        for (const L::SwitchCase& c : cases) s = s.add(c.key);
        return s;
      };
      auto aux = [&](Slice<L::SwitchCase> cases) {
        std::vector<F::SwitchCase> out;  // List.map: left to right
        for (const L::SwitchCase& c : cases) out.push_back({c.key, close(env, c.action)});
        return slice(out);
      };
      // create_let scrutinee (Expr (close arg)) (Switch (scrutinee, {...})):
      // the record first, its fields right to left (failaction, blocks,
      // numblocks, consts, numconsts), then the scrutinee
      F::t failaction = sw.sw_failaction ? close(env, sw.sw_failaction) : nullptr;
      Slice<F::SwitchCase> blocks = aux(sw.sw_blocks);
      F::IntSet numblocks = nums(sw.sw_numblocks, sw.sw_blocks, sw.sw_failaction);
      Slice<F::SwitchCase> consts = aux(sw.sw_consts);
      F::IntSet numconsts = nums(sw.sw_numconsts, sw.sw_consts, sw.sw_failaction);
      F::t sw_expr = F::switch_(scrutinee, numconsts, consts, numblocks, blocks, failaction);
      F::t arg = close(env, x->arg);
      return F::create_let(scrutinee, F::n_expr(arg), sw_expr);
    }
    case L::LK::Lstringswitch: {
      auto* x = L::as<L::Lstringswitch>(lam);
      variable::t scrutinee = variable::create(Names::string_switch);
      // String_switch (scrutinee, List.map ..., Option.map ...): the
      // default, the cases, then the scrutinee
      F::t def = x->def ? close(env, x->def) : nullptr;
      std::vector<F::StringCase> cases;
      for (const L::StringCase& c : x->cases) cases.push_back({c.s, close(env, c.action)});
      F::t sw = F::string_switch(scrutinee, slice(cases), def);
      F::t arg = close(env, x->arg);
      return F::create_let(scrutinee, F::n_expr(arg), sw);
    }
    case L::LK::Lstaticraise: {
      auto* x = L::as<L::Lstaticraise>(lam);
      return lift_code::lifting_helper(
          close_list(env, x->args), EvaluationOrder::Right_to_left,
          [&](Slice<variable::t> args) {
            static_exception::t static_exn = env.find_static_exception(x->i);
            return F::static_raise(static_exn, args);
          },
          Names::staticraise_arg);
    }
    case L::LK::Lstaticcatch: {
      auto* x = L::as<L::Lstaticcatch>(lam);
      static_exception::t st_exn = static_exception::create();
      Env env2 = env.add_static_exception(x->i, st_exn);
      std::vector<F::CatchVar> vars;  // List.map: left to right
      std::vector<Ident::t> ids;
      std::vector<variable::t> var_list;
      for (const L::Param& p : x->params) {
        variable::t v = variable::create_with_same_name_as_ident(p.id);
        vars.push_back({v, p.kind});
        ids.push_back(p.id);
        var_list.push_back(v);
      }
      Env env_handler = env2.add_vars(ids, var_list);
      // Static_catch (st_exn, vars, close body, close handler): the
      // handler first
      F::t handler = close(env_handler, x->handler);
      F::t body = close(env2, x->body);
      return F::static_catch(st_exn, slice(vars), body, handler);
    }
    case L::LK::Ltrywith: {
      auto* x = L::as<L::Ltrywith>(lam);
      variable::t var = variable::create_with_same_name_as_ident(x->exn);
      // Try_with (close body, var, close handler): the handler first
      F::t handler = close(env.add_var(x->exn, var), x->handler);
      F::t body = close(env, x->body);
      return F::try_with(body, var, handler);
    }
    case L::LK::Lifthenelse: {
      auto* x = L::as<L::Lifthenelse>(lam);
      F::t cond = close(env, x->cond);
      variable::t cond_var = variable::create(Names::cond);
      // If_then_else (cond_var, close ifso, close ifnot): ifnot first
      F::t ifnot = close(env, x->ifnot);
      F::t ifso = close(env, x->ifso);
      return F::create_let(cond_var, F::n_expr(cond), F::if_then_else(cond_var, ifso, ifnot));
    }
    case L::LK::Lsequence: {
      auto* x = L::as<L::Lsequence>(lam);
      variable::t var = variable::create(Names::sequence);
      named lam1 = F::n_expr(close(env, x->l1));
      F::t lam2 = close(env, x->l2);
      return F::create_let(var, lam1, lam2);
    }
    case L::LK::Lwhile: {
      auto* x = L::as<L::Lwhile>(lam);
      // While (close cond, close body): the body first
      F::t body = close(env, x->body);
      F::t cond = close(env, x->cond);
      return F::while_(cond, body);
    }
    case L::LK::Lfor: {
      auto* x = L::as<L::Lfor>(lam);
      variable::t bound_var = variable::create_with_same_name_as_ident(x->id);
      variable::t from_value = variable::create(Names::for_from);
      variable::t to_value = variable::create(Names::for_to);
      F::t body = close(env.add_var(x->id, bound_var), x->body);
      // create_let from_value (Expr (close lo)) (create_let to_value (Expr
      // (close hi)) (For ...)): hi first
      F::t hi = close(env, x->hi);
      F::t inner = F::create_let(to_value, F::n_expr(hi), F::for_(bound_var, from_value, to_value, x->dir, body));
      F::t lo = close(env, x->lo);
      return F::create_let(from_value, F::n_expr(lo), inner);
    }
    case L::LK::Lassign: {
      auto* x = L::as<L::Lassign>(lam);
      const variable::t* being_assigned = env.find_mutable_var_exn(x->id);
      if (!being_assigned)
        misc::fatal_error("Closure_conversion.close: unbound mutable variable " + ident::unique_name(x->id) +
                          " in assignment");
      variable::t new_value_var = variable::create(Names::new_value);
      F::t nv = close(env, x->e);
      return F::create_let(new_value_var, F::n_expr(nv), F::assign(*being_assigned, new_value_var));
    }
    case L::LK::Levent: return close(env, L::as<L::Levent>(lam)->l);
    case L::LK::Lifused:
      // [Lifused] is used to mark that this expression should be alive
      // only if an identifier is.  Every use should have been removed by
      // [Simplif.simplify_lets], either by replacing by the inner
      // expression, or by completely removing it (replacing by unit).
      misc::fatal_error("[Lifused] should have been removed by [Simplif.simplify_lets]");
  }
  misc::fatal_error("Closure_conversion.close");
}

F::t Closer::close_prim(const Env& env, const L::Lprim* x) {
  using PK = L::Primitive::K;
  using CK = clambda::Primitive::K;
  const L::Primitive& lp = x->p;
  bool safe_div = (lp.kind == PK::Pdivint || lp.kind == PK::Pmodint || lp.kind == PK::Pdivbint ||
                   lp.kind == PK::Pmodbint) &&
                  lp.safe == L::IsSafe::Safe;
  if (safe_div && !clflags::unsafe) {
    if (x->args.size() != 2) misc::fatal_error("Pdivint / Pmodint must have exactly two arguments");
    F::t arg2 = close(env, x->args[1]);
    F::t arg1 = close(env, x->args[0]);
    variable::t numerator = variable::create(Names::numerator);
    variable::t denominator = variable::create(Names::denominator);
    variable::t zero = variable::create(Names::zero);
    variable::t is_zero = variable::create(Names::is_zero);
    variable::t exn = variable::create(Names::division_by_zero);
    symbol::t exn_symbol = compilenv::symbol_for_global_prime(predef::idents().division_by_zero);
    debuginfo::t dbg = debuginfo::from_location(x->loc);
    named zero_const;
    if (lp.kind == PK::Pdivint || lp.kind == PK::Pmodint) zero_const = FLAMBDA_NAMED_INT_LITERAL(0);
    else if (lp.bi == BoxedInteger::Pint32)
      zero_const = F::n_allocated_const_literal(__FILE__, [] { return allocated_const::int32(0); }, "Int32 0l");
    else if (lp.bi == BoxedInteger::Pint64)
      zero_const = F::n_allocated_const_literal(__FILE__, [] { return allocated_const::int64(0); }, "Int64 0L");
    else zero_const = F::n_allocated_const_literal(__FILE__, [] { return allocated_const::nativeint(0); }, "Nativeint 0n");
    clambda::Primitive prim = cl_prim(lp.kind == PK::Pdivint   ? CK::Pdivint
                                      : lp.kind == PK::Pmodint ? CK::Pmodint
                                      : lp.kind == PK::Pdivbint ? CK::Pdivbint
                                                                : CK::Pmodbint);
    prim.safe = L::IsSafe::Unsafe;
    prim.bi = lp.bi;
    // ([Pdivint Unsafe], [Pmodint Unsafe] and [Pintcomp Ceq] are literals
    // of closure_conversion.ml)
    if (lp.kind == PK::Pdivint) prim = CLAMBDA_PRIM_LITERAL(prim, "Pdivint Unsafe");
    else if (lp.kind == PK::Pmodint) prim = CLAMBDA_PRIM_LITERAL(prim, "Pmodint Unsafe");
    clambda::Primitive comparison = cl_prim(lp.kind == PK::Pdivint || lp.kind == PK::Pmodint ? CK::Pintcomp : CK::Pbintcomp);
    comparison.icmp = L::IntegerComparison::Ceq;
    if (comparison.kind == CK::Pbintcomp) comparison.bi = lp.bi;
    else comparison = CLAMBDA_PRIM_LITERAL(comparison, "Pintcomp Ceq");
    t.imported_symbols = t.imported_symbols.add(exn_symbol);
    // The nested create_lets' arguments right to left: the else branch's
    // name_expr (result) before the then branch's (dummy).
    F::t result = name_expr(F::n_prim(prim, slice(std::vector<variable::t>{numerator, denominator}), dbg), Names::result);
    clambda::Primitive raise = cl_prim(CK::Praise);
    raise.raise = L::RaiseKind::Raise_regular;
    raise = CLAMBDA_PRIM_LITERAL(raise, "Praise Raise_regular");
    F::t dummy = name_expr(F::n_prim(raise, slice(std::vector<variable::t>{exn}), dbg), Names::dummy);
    F::t ite = F::if_then_else(is_zero, dummy, result);
    F::t e = F::create_let(is_zero, F::n_prim(comparison, slice(std::vector<variable::t>{zero, denominator}), dbg), ite);
    e = F::create_let(numerator, F::n_expr(arg1), e);
    e = F::create_let(denominator, F::n_expr(arg2), e);
    e = F::create_let(exn, F::n_symbol(exn_symbol), e);
    return F::create_let(zero, zero_const, e);
  }
  if ((lp.kind == PK::Psequor || lp.kind == PK::Psequand) && x->args.size() != 2)
    misc::fatal_error("Psequand / Psequor must have exactly two arguments");
  if (lp.kind == PK::Psequor) {
    F::t arg1 = close(env, x->args[0]);
    F::t arg2 = close(env, x->args[1]);
    variable::t const_true = variable::create(Names::const_true);
    variable::t cond = variable::create(Names::cond_sequor);
    return F::create_let(const_true, FLAMBDA_NAMED_INT_LITERAL(1),
                         F::create_let(cond, F::n_expr(arg1), F::if_then_else(cond, F::var(const_true), arg2)));
  }
  if (lp.kind == PK::Psequand) {
    F::t arg1 = close(env, x->args[0]);
    F::t arg2 = close(env, x->args[1]);
    variable::t const_false = variable::create(Names::const_false);
    variable::t cond = variable::create(Names::const_sequand);
    return F::create_let(const_false, FLAMBDA_NAMED_INT_LITERAL(0),
                         F::create_let(cond, F::n_expr(arg1), F::if_then_else(cond, arg2, F::var(const_false))));
  }
  if ((lp.kind == PK::Pbytes_to_string || lp.kind == PK::Pbytes_of_string) && x->args.size() == 1)
    return close(env, x->args[0]);
  if (lp.kind == PK::Pignore && x->args.size() == 1) {
    variable::t var = variable::create(Names::ignore);
    named defining_expr = close_let_bound_expression(var, env, x->args[0]);
    return F::create_let(var, defining_expr, name_expr(FLAMBDA_NAMED_INT_LITERAL(0), Names::unit));
  }
  if (lp.kind == PK::Praise && x->args.size() == 1) {
    variable::t arg_var = variable::create(Names::raise_arg);
    debuginfo::t dbg = debuginfo::from_location(x->loc);
    // create_let arg_var (Expr (close arg)) (name_expr ...): the name first
    clambda::Primitive raise = cl_prim(CK::Praise);
    raise.raise = lp.raise;
    F::t named_raise = name_expr(F::n_prim(raise, slice(std::vector<variable::t>{arg_var}), dbg), Names::raise);
    F::t arg = close(env, x->args[0]);
    return F::create_let(arg_var, F::n_expr(arg), named_raise);
  }
  if (lp.kind == PK::Pctconst && x->args.size() == 1) {
    L::lambda arg = x->args[0];
    auto cst = [&](const L::StructuredConstant* c) {
      return close(env, L::llet(L::LetKind::Strict, L::ValueKind::gen(), Ident::create_local(OCAML_LIT("dummy")), arg, L::lconst(c)));
    };
    auto const_bool = [](bool b) { return L::const_int(b ? 1 : 0); };
    using CT = L::CompileTimeConstant;
    switch (lp.ctconst) {
      case CT::Big_endian: return cst(const_bool(big_endian));
      case CT::Word_size: return cst(L::const_int(8 * size_int));
      case CT::Int_size: return cst(L::const_int(8 * size_int - 1));
      case CT::Max_wosize: return cst(L::const_int((1L << ((8 * size_int) - 10)) - 1));
      case CT::Ostype_unix: return cst(const_bool(std::string_view(target_os_type) == "Unix"));
      case CT::Ostype_win32: return cst(const_bool(std::string_view(target_os_type) == "Win32"));
      case CT::Ostype_cygwin: return cst(const_bool(std::string_view(target_os_type) == "Cygwin"));
      case CT::Backend_type: return cst(L::const_int(0));  // tag 0 is the same as Native
      case CT::Standard_library_default: {
        compilenv::need_stdlib_location();
        symbol::t sym = compilenv::symbol_for_global_prime(compilenv::stdlib_symbol_name());
        t.imported_symbols = t.imported_symbols.add(sym);
        return name_expr(F::n_symbol(sym), Names::pgetglobal);
      }
    }
  }
  if (lp.kind == PK::Pfield && x->args.size() == 1)
    if (auto* g = L::as<L::Lprim>(x->args[0]); g && g->p.kind == PK::Pgetglobal && g->args.empty() &&
                                                ident::same(g->p.id, t.current_unit_id))
      misc::fatal_error(
          "[Pfield (Pgetglobal ...)] for the current compilation unit is forbidden upon entry to the middle end");
  if (lp.kind == PK::Psetfield && x->args.size() == 2)
    if (auto* g = L::as<L::Lprim>(x->args[0]); g && g->p.kind == PK::Pgetglobal && g->args.empty())
      misc::fatal_error("[Psetfield (Pgetglobal ...)] is forbidden upon entry to the middle end");
  if (lp.kind == PK::Pgetglobal && x->args.empty()) {
    if (ident::is_predef(lp.id)) {
      symbol::t sym = compilenv::symbol_for_global_prime(lp.id);
      t.imported_symbols = t.imported_symbols.add(sym);
      return name_expr(F::n_symbol(sym), Names::predef_exn);
    }
    if (ident::same(lp.id, t.current_unit_id)) misc::fatal_error("Closure_conversion: Pgetglobal of the current unit");
    symbol::t sym = compilenv::symbol_for_global_prime(lp.id);
    t.imported_symbols = t.imported_symbols.add(sym);
    return name_expr(F::n_symbol(sym), Names::pgetglobal);
  }
  // One of the important consequences of the ANF-like representation here
  // is that we obtain names corresponding to the components of blocks being
  // made (with [Pmakeblock]).  This information can be used by the
  // simplification pass to increase the likelihood of eliminating the
  // allocation, since some field accesses can be tracked back to known
  // field values.
  debuginfo::t dbg = debuginfo::from_location(x->loc);
  clambda::Primitive p = convert_primitives::convert(lp);
  return lift_code::lifting_helper(
      close_list(env, x->args), EvaluationOrder::Right_to_left,
      [&](Slice<variable::t> args) { return name_expr(F::n_prim(p, args, dbg), Names::of_primitive(lp)); },
      Names::of_primitive_arg(lp));
}

// Perform closure conversion on a set of function declarations, returning
// a set of closures.  (The set will often only contain a single function;
// the only case where it cannot is for "let rec".)
named Closer::close_functions(const Env& external_env, const FunctionDecls& function_declarations) {
  Env closure_env_without_params = closure_env_without_parameters(external_env, function_declarations);
  const L::IdentSet& all_free_idents = function_declarations.all_free_idents;
  auto close_one_function = [&](variable::Map<const F::FunctionDeclaration*> map, const FunctionDecl& decl) {
    L::lambda body = decl.body;
    debuginfo::t dbg = debuginfo::from_location(decl.loc);
    // Create fresh variables for the elements of the closure (cf. the
    // comment on [Function_decl.closure_env_without_parameters], above).
    // This induces a renaming on [Function_decl.free_idents]; the results
    // of that renaming are stored in [free_variables].  (List.fold_right:
    // the last parameter first)
    Env closure_env = closure_env_without_params;
    for (std::size_t k = decl.params.size(); k-- > 0;)
      closure_env = closure_env.add_var(decl.params[k], variable::create_with_same_name_as_ident(decl.params[k]));
    // If the function is the wrapper for a function with an optional
    // argument with a default value, make sure it always gets inlined.
    bool stub = decl.attr.stub;
    std::vector<variable::t> param_vars;
    for (Ident::t p : decl.params) param_vars.push_back(closure_env.find_var(p));
    std::vector<Parameter> params;
    for (variable::t v : param_vars) params.push_back(parameter::wrap(v));
    variable::t closure_bound_var = decl.closure_bound_var;
    variable::t unboxed_version = variable::rename(closure_bound_var);
    F::t fbody = close(closure_env, body);
    variable::t closure_origin = unboxed_version;  // Closure_origin.create (Closure_id.wrap ...)
    const F::FunctionDeclaration* fun_decl =
        F::create_function_declaration(slice(params), fbody, stub, dbg, decl.attr.inline_, decl.attr.specialise,
                                       decl.attr.is_a_functor, closure_origin, decl.attr.poll);
    if (decl.kind == L::FunctionKind::Curried) return map.add(closure_bound_var, fun_decl);
    variable::t unboxed_version2 = variable::rename(closure_bound_var);
    const F::FunctionDeclaration* generic_function_stub =
        tupled_function_call_stub(param_vars, unboxed_version2, closure_bound_var);
    return map.add(closure_bound_var, generic_function_stub).add(unboxed_version2, fun_decl);
  };
  bool is_classic_mode = clflags::classic_inlining;
  variable::Map<const F::FunctionDeclaration*> funs;
  for (const FunctionDecl& d : function_declarations.function_decls) funs = close_one_function(funs, d);
  const F::FunctionDeclarations* function_decls = F::create_function_declarations(is_classic_mode, funs);
  // The closed representation of a set of functions is a "set of closures".
  // (For avoidance of doubt, the runtime representation of the *whole set*
  // is a single block with tag [Closure_tag].)
  variable::Map<F::SpecialisedTo> free_vars;
  for (Ident::t var : all_free_idents) {
    variable::t internal_var = closure_env_without_params.find_var(var);
    F::SpecialisedTo external_var{external_env.find_var(var), nullptr};
    free_vars = free_vars.add(internal_var, external_var);
  }
  const F::SetOfClosures* set_of_closures = F::create_set_of_closures(function_decls, free_vars, {}, {});
  return F::n_set_of_closures(set_of_closures);
}

named Closer::close_let_bound_expression(variable::t let_bound_var, const Env& env, L::lambda lam) {
  if (auto* fn = L::as<L::Lfunction>(lam)) {
    const L::LFunction* d = fn->f;
    // Ensure that [let] and [let rec]-bound functions have appropriate
    // names.
    variable::t closure_bound_var = variable::rename(let_bound_var);
    FunctionDecl decl = function_decl_create(nullptr, closure_bound_var, d->kind, d->params, d->body, d->attr, d->loc);
    variable::t set_of_closures_var = variable::rename(let_bound_var);
    std::vector<FunctionDecl> decls;
    decls.push_back(std::move(decl));
    named set_of_closures = close_functions(env, function_decls_create(std::move(decls)));
    projection::ProjectClosure project_closure{set_of_closures_var, closure_bound_var};
    return F::n_expr(F::create_let(set_of_closures_var, set_of_closures,
                                   flambda_utils::name_expr_from_var(F::n_project_closure(project_closure),
                                                                     let_bound_var)));
  }
  return F::n_expr(close(env, lam));
}

}  // namespace

F::Program lambda_to_flambda(Ident::t module_ident, long size, L::lambda lam) {
  lam = add_default_argument_wrappers(lam);
  compilation_unit::t compilation_unit = compilation_unit::get_current_exn();
  T t{compilation_unit->id, {}, {}};
  symbol::t module_symbol = compilenv::symbol_for_global_prime(module_ident);
  symbol::t block_symbol = symbol::of_variable(variable::create(Names::module_as_block));
  // The global module block is built by accessing the fields of all the
  // introduced symbols.  (Array.init: in order)
  std::vector<F::t> fields;
  for (long pos = 0; pos < size; ++pos) {
    variable::t sym_v = variable::create(Names::block_symbol);
    variable::t result_v = variable::create(Names::block_symbol_get);
    variable::t value_v = variable::create(Names::block_symbol_get_field);
    clambda::Primitive f0 = cl_prim(clambda::Primitive::K::Pfield);
    f0.n = 0;
    f0.ptr = L::ImmediateOrPointer::Pointer;
    f0.mut = MutableFlag::Mutable;
    f0 = CLAMBDA_PRIM_LITERAL(f0, "Pfield (0, Pointer, Mutable)");
    clambda::Primitive fpos = f0;
    fpos.n = pos;
    fpos.id = clambda::fresh_uconstant_id();
    fields.push_back(F::create_let(
        sym_v, F::n_symbol(block_symbol),
        F::create_let(result_v, F::n_prim(f0, slice(std::vector<variable::t>{sym_v}), debuginfo::none()),
                      F::create_let(value_v, F::n_prim(fpos, slice(std::vector<variable::t>{result_v}), debuginfo::none()),
                                    F::var(value_v)))));
  }
  // Initialize_symbol (block_symbol, 0, [close t Env.empty lam],
  //   Initialize_symbol (module_symbol, 0, fields, End module_symbol))
  F::program_body inner = F::initialize_symbol(module_symbol, tag::create_exn(0), slice(fields), F::end(module_symbol));
  Closer c{t};
  F::t body = c.close(Env::empty(), lam);
  F::program_body module_initializer =
      F::initialize_symbol(block_symbol, tag::create_exn(0), slice(std::vector<F::t>{body}), inner);
  // List.fold_left over t.declared_symbols (the newest first): the oldest
  // declaration outermost
  F::program_body program_body = module_initializer;
  for (auto it = t.declared_symbols.rbegin(); it != t.declared_symbols.rend(); ++it)
    program_body = F::let_symbol(it->sym, it->def, program_body);
  return {t.imported_symbols, program_body};
}

}  // namespace cppcaml::typing::closure_conversion
