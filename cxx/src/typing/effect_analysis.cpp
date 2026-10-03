// Port of middle_end/flambda/effect_analysis.ml (see effect_analysis.hpp).
#include "cppcaml/typing/effect_analysis.hpp"

#include "cppcaml/typing/convert_primitives.hpp"

namespace cppcaml::typing::effect_analysis {

using namespace flambda;

bool no_effects_prim(const clambda::Primitive& prim) {
  using E = semantics_of_primitives::Effects;
  E e = semantics_of_primitives::for_primitive(prim).first;
  return e == E::No_effects || e == E::Only_generative_effects;
}

bool no_effects(t flam) {
  switch (flam->kind) {
    case EK::Var: return true;
    case EK::Let: {
      auto* l = static_cast<const Let*>(flam);
      return no_effects_named(l->defining_expr) && no_effects(l->body);
    }
    case EK::Let_mutable: return no_effects(static_cast<const Let_mutable*>(flam)->body);
    case EK::If_then_else: {
      auto* i = static_cast<const If_then_else*>(flam);
      return no_effects(i->ifso) && no_effects(i->ifnot);
    }
    case EK::Switch: {
      auto* s = static_cast<const Switch*>(flam);
      for (const SwitchCase& c : s->blocks)
        if (!no_effects(c.action)) return false;
      for (const SwitchCase& c : s->consts)
        if (!no_effects(c.action)) return false;
      return !s->failaction || no_effects(s->failaction);
    }
    case EK::String_switch: {
      auto* s = static_cast<const String_switch*>(flam);
      for (const StringCase& c : s->cases)
        if (!no_effects(c.action)) return false;
      return !s->def || no_effects(s->def);
    }
    // If there is a [raise] in [body], the whole [Try_with] may have an
    // effect, so there is no need to test the handler.
    case EK::Static_catch: return no_effects(static_cast<const Static_catch*>(flam)->body);
    case EK::Try_with: return no_effects(static_cast<const Try_with*>(flam)->body);
    case EK::While: case EK::For: case EK::Apply: case EK::Send: case EK::Assign: case EK::Static_raise:
      return false;
    case EK::Proved_unreachable: return true;
  }
  return false;
}

bool no_effects_named(named n) {
  switch (n->kind) {
    case NK::Symbol: case NK::Const: case NK::Allocated_const: case NK::Read_mutable: case NK::Read_symbol_field:
    case NK::Set_of_closures: case NK::Project_closure: case NK::Project_var: case NK::Move_within_set_of_closures:
      return true;
    case NK::Prim: return no_effects_prim(*static_cast<const NPrim*>(n)->prim);
    case NK::Expr: return no_effects(static_cast<const NExpr*>(n)->expr);
  }
  return false;
}

}  // namespace cppcaml::typing::effect_analysis
