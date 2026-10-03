// Port of middle_end/flambda/ref_to_variables.ml (see ref_to_variables.hpp).
#include "cppcaml/typing/ref_to_variables.hpp"

#include <optional>
#include <vector>

#include "cppcaml/typing/flambda_iterators.hpp"
#include "cppcaml/typing/internal_variable_names.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::ref_to_variables {

using namespace flambda;
using K = clambda::Primitive::K;

namespace {

variable::Set variables_not_used_as_local_reference(t tree) {
  variable::Set set;
  std::function<void(t)> loop;
  auto loop_named = [&](named flam) {
    if (auto* p = as<NPrim>(flam)) {
      // Directly used block: does not prevent use as a variable
      if ((p->prim->kind == K::Pfield || p->prim->kind == K::Poffsetref) && p->args.size() == 1) return;
      if (p->prim->kind == K::Psetfield && p->args.size() == 2) {
        // block is not prevented to be used as a local reference, but v is
        set = set.add(p->args[1]);
        return;
      }
    }
    if (auto* s = as<NSet_of_closures>(flam)) {
      set = variable::Set::union_(set, free_variables_named(flam));
      s->set->function_decls->funs.iter([&](variable::t, const FunctionDeclaration* fd) { loop(fd->body); });
      return;
    }
    if (auto* e = as<NExpr>(flam)) {
      loop(e->expr);
      return;
    }
    set = variable::Set::union_(set, free_variables_named(flam));
  };
  loop = [&](t flam) {
    switch (flam->kind) {
      case EK::Let: {
        auto* l = as<Let>(flam);
        loop_named(l->defining_expr);
        loop(l->body);
        break;
      }
      case EK::Var: set = set.add(as<Var>(flam)->var); break;
      case EK::Let_mutable: {
        auto* lm = as<Let_mutable>(flam);
        set = set.add(lm->initial_value);
        loop(lm->body);
        break;
      }
      case EK::If_then_else: {
        auto* i = as<If_then_else>(flam);
        set = set.add(i->cond);
        loop(i->ifso);
        loop(i->ifnot);
        break;
      }
      case EK::Switch: {
        auto* sw = as<Switch>(flam);
        set = set.add(sw->scrutinee);
        for (const SwitchCase& c : sw->consts) loop(c.action);
        for (const SwitchCase& c : sw->blocks) loop(c.action);
        if (sw->failaction) loop(sw->failaction);
        break;
      }
      case EK::String_switch: {
        auto* ss = as<String_switch>(flam);
        set = set.add(ss->scrutinee);
        for (const StringCase& c : ss->cases) loop(c.action);
        if (ss->def) loop(ss->def);
        break;
      }
      case EK::Static_catch: {
        auto* sc = as<Static_catch>(flam);
        loop(sc->body);
        loop(sc->handler);
        break;
      }
      case EK::Try_with: {
        auto* tw = as<Try_with>(flam);
        loop(tw->body);
        loop(tw->handler);
        break;
      }
      case EK::While: {
        auto* w = as<While>(flam);
        loop(w->cond);
        loop(w->body);
        break;
      }
      case EK::For: {
        auto* f = as<For>(flam);
        set = set.add(f->from_value);
        set = set.add(f->to_value);
        loop(f->body);
        break;
      }
      case EK::Static_raise:
        set = variable::Set::union_(variable::Set::of_list(std::vector<variable::t>(as<Static_raise>(flam)->args.begin(), as<Static_raise>(flam)->args.end())), set);
        break;
      case EK::Proved_unreachable: case EK::Apply: case EK::Send: case EK::Assign:
        set = variable::Set::union_(set, free_variables(flam));
        break;
    }
  };
  loop(tree);
  return set;
}

bool is_mutable_ref_block(const clambda::Primitive* p) {
  return p->kind == K::Pmakeblock && p->n == 0 && p->mut == MutableFlag::Mutable;
}

variable::Map<long> variables_containing_ref(t flam) {
  variable::Map<long> map;
  auto aux = [&](t e) {
    auto* l = as<Let>(e);
    if (!l) return;
    auto* p = as<NPrim>(l->defining_expr);
    if (p && is_mutable_ref_block(p->prim)) map = map.add(l->var, static_cast<long>(p->args.size()));
  };
  auto aux_named = [](named) {};
  flambda_iterators::iter(aux, aux_named, flam);
  return map;
}

t eliminate_ref_of_expr(t flam) {
  variable::Set not_used = variables_not_used_as_local_reference(flam);
  variable::Map<long> convertible0 =
      variables_containing_ref(flam).filter([&](variable::t v, long) { return !not_used.mem(v); });
  if (convertible0.is_empty()) return flam;
  // Variable.Map.mapi (Array.init size (fun _ ->
  // Mutable_variable.create_from_variable v)): in key order, each field in
  // order
  variable::Map<Slice<variable::t>> convertible_variables = convertible0.mapi(
      [](variable::t v, long size) {
        std::vector<variable::t> arr;
        for (long k = 0; k < size; ++k) arr.push_back(variable::rename(v));
        return slice(arr);
      });
  auto convertible_variable = [&](variable::t v) { return convertible_variables.mem(v); };
  // (nullopt: None -- this case could apply when inlining code containing
  // GADTS)
  auto get_variable = [&](variable::t v, long field) -> std::optional<std::pair<variable::t, long>> {
    const Slice<variable::t>* arr = convertible_variables.find_opt(v);
    if (!arr) misc::fatal_error("Ref_to_variables.get_variable");  // (assert false)
    if (static_cast<long>(arr->size()) <= field) return std::nullopt;
    return std::pair<variable::t, long>{(*arr)[static_cast<std::size_t>(field)], static_cast<long>(arr->size())};
  };
  auto aux = [&](t e) -> t {
    auto* l = as<Let>(e);
    if (!l) return e;
    auto* p = as<NPrim>(l->defining_expr);
    if (!p || !is_mutable_ref_block(p->prim) || !convertible_variable(l->var)) return e;
    std::vector<lambda::ValueKind> shape;
    if (!p->prim->shape.some) shape.assign(p->args.size(), lambda::ValueKind{});  // Pgenval
    else shape.assign(p->prim->shape.kinds.begin(), p->prim->shape.kinds.end());
    if (shape.size() != p->args.size()) misc::fatal_error("Invalid_argument(\"List.fold_left2\")");
    t body = l->body;
    for (std::size_t field = 0; field < p->args.size(); ++field) {
      std::optional<std::pair<variable::t, long>> fv = get_variable(l->var, static_cast<long>(field));
      if (!fv) misc::fatal_error("Ref_to_variables.eliminate_ref_of_expr");  // (assert false)
      body = let_mutable(fv->first, p->args[field], shape[field], body);
    }
    return body;
  };
  auto aux_named = [&](named n) -> named {
    auto* p = as<NPrim>(n);
    if (!p) return n;
    if (p->prim->kind == K::Pfield && p->args.size() == 1 && convertible_variable(p->args[0])) {
      std::optional<std::pair<variable::t, long>> fv = get_variable(p->args[0], p->prim->n);
      if (!fv) return n_expr(proved_unreachable());
      return n_read_mutable(fv->first);
    }
    if (p->prim->kind == K::Poffsetref && p->args.size() == 1 && convertible_variable(p->args[0])) {
      std::optional<std::pair<variable::t, long>> fv = get_variable(p->args[0], 0);
      if (!fv) return n_expr(proved_unreachable());
      if (fv->second != 1) return n_expr(proved_unreachable());
      variable::t mut = variable::create(internal_variable_names::read_mutable);
      variable::t new_value = variable::create(internal_variable_names::offsetted);
      clambda::Primitive off = clambda::prim(K::Poffsetint);
      off.n = p->prim->n;
      t expr = create_let(mut, n_read_mutable(fv->first),
                          create_let(new_value, n_prim(off, slice({mut}), p->dbg), assign(fv->first, new_value)));
      return n_expr(expr);
    }
    if (p->prim->kind == K::Psetfield && p->args.size() == 2 && convertible_variable(p->args[0])) {
      std::optional<std::pair<variable::t, long>> fv = get_variable(p->args[0], p->prim->n);
      if (!fv) return n_expr(proved_unreachable());
      return n_expr(assign(fv->first, p->args[1]));
    }
    return n;
  };
  return flambda_iterators::map(aux, aux_named, flam);
}

}  // namespace

Program eliminate_ref(const Program& program) {
  return flambda_iterators::map_exprs_at_toplevel_of_program(program, eliminate_ref_of_expr);
}

}  // namespace cppcaml::typing::ref_to_variables
