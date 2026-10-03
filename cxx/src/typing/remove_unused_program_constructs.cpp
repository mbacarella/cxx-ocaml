// Port of middle_end/flambda/remove_unused_program_constructs.ml (see
// remove_unused_program_constructs.hpp).
#include "cppcaml/typing/remove_unused_program_constructs.hpp"

#include <vector>

#include "cppcaml/typing/effect_analysis.hpp"

namespace cppcaml::typing::remove_unused_program_constructs {

using namespace flambda;

namespace {
symbol::Set dependency(t expr) { return free_symbols(expr); }

// CR-soon pchambart: copied from lift_constant.  Needs remerging
symbol::Set constant_dependencies(constant_defining_value c) {
  switch (c->kind) {
    case ConstantDefiningValue::Kind::Allocated_const: return {};
    case ConstantDefiningValue::Kind::Block: {
      std::vector<symbol::t> symbol_fields;
      for (const BlockField& f : c->fields)
        if (f.sym) symbol_fields.push_back(f.sym);
      return symbol::Set::of_list(symbol_fields);
    }
    case ConstantDefiningValue::Kind::Set_of_closures: return free_symbols_named(n_set_of_closures(c->set));
    case ConstantDefiningValue::Kind::Project_closure: return symbol::Set::singleton(c->sym);
  }
  return {};
}

symbol::Set let_rec_dep(Slice<SymbolBinding> defs, symbol::Set dep) {
  std::vector<std::pair<symbol::t, symbol::Set>> defs_deps;  // List.map: in order
  for (const SymbolBinding& b : defs) defs_deps.emplace_back(b.sym, constant_dependencies(b.def));
  for (;;) {
    symbol::Set new_dep = dep;
    for (const auto& [sym, sym_dep] : defs_deps)
      if (new_dep.mem(sym)) new_dep = symbol::Set::union_(new_dep, sym_dep);
    if (symbol::Set::equal(dep, new_dep)) return dep;
    dep = new_dep;
  }
}
}  // namespace

Program remove_unused_program_constructs(const Program& program) {
  // loop: the rest first (recursion), then the node
  std::vector<program_body> nodes;
  program_body p = program.program_body;
  for (; p->kind != ProgramBody::Kind::End; p = p->body) nodes.push_back(p);
  program_body body = p;  // End symbol: program, Symbol.Set.singleton symbol
  symbol::Set dep = symbol::Set::singleton(p->sym);
  for (std::size_t k = nodes.size(); k-- > 0;) {
    program_body n = nodes[k];
    switch (n->kind) {
      case ProgramBody::Kind::Let_symbol:
        if (dep.mem(n->sym)) {
          body = let_symbol(n->sym, n->def, body);
          dep = symbol::Set::union_(dep, constant_dependencies(n->def));
        }
        break;
      case ProgramBody::Kind::Let_rec_symbol: {
        dep = let_rec_dep(n->defs, dep);
        std::vector<SymbolBinding> defs;  // List.filter
        for (const SymbolBinding& b : n->defs)
          if (dep.mem(b.sym)) defs.push_back(b);
        if (!defs.empty()) body = let_rec_symbol(slice(defs), body);
        break;
      }
      case ProgramBody::Kind::Initialize_symbol:
        if (dep.mem(n->sym)) {
          for (t field : n->fields) dep = symbol::Set::union_(dep, dependency(field));
          body = initialize_symbol(n->sym, n->tag, n->fields, body);
        } else {
          for (t field : n->fields) {
            if (effect_analysis::no_effects(field)) continue;
            symbol::Set new_dep = dependency(field);
            dep = symbol::Set::union_(new_dep, dep);
            body = effect(field, body);
          }
        }
        break;
      case ProgramBody::Kind::Effect:
        if (!effect_analysis::no_effects(n->expr)) {
          symbol::Set new_dep = dependency(n->expr);
          dep = symbol::Set::union_(new_dep, dep);
          body = effect(n->expr, body);
        }
        break;
      case ProgramBody::Kind::End: break;
    }
  }
  return {program.imported_symbols, body};
}

}  // namespace cppcaml::typing::remove_unused_program_constructs
