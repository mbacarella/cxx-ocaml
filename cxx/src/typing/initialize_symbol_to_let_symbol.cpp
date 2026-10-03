// Port of middle_end/flambda/initialize_symbol_to_let_symbol.ml (see
// initialize_symbol_to_let_symbol.hpp).
#include "cppcaml/typing/initialize_symbol_to_let_symbol.hpp"

#include <optional>
#include <vector>

#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::initialize_symbol_to_let_symbol {

using namespace flambda;

namespace {

std::optional<BlockField> constant_field(t expr) {
  auto* l = as<Let>(expr);
  if (!l) return std::nullopt;
  auto* v = as<Var>(l->body);
  if (!v) return std::nullopt;
  if (auto* c = as<NConst>(l->defining_expr)) {
    // This must be true since var is the only variable in scope
    if (!variable::equal(l->var, v->var)) misc::fatal_error("Initialize_symbol_to_let_symbol.constant_field");
    return BlockField{nullptr, c->c};
  }
  if (auto* s = as<NSymbol>(l->defining_expr)) {
    if (!variable::equal(l->var, v->var)) misc::fatal_error("Initialize_symbol_to_let_symbol.constant_field");
    return BlockField{s->sym, {}};
  }
  return std::nullopt;
}

program_body loop(program_body program) {
  std::vector<program_body> nodes;
  program_body p = program;
  for (; p->kind != ProgramBody::Kind::End; p = p->body) nodes.push_back(p);
  program_body body = end(p->sym);
  for (std::size_t k = nodes.size(); k-- > 0;) {
    program_body n = nodes[k];
    switch (n->kind) {
      case ProgramBody::Kind::Initialize_symbol: {
        // Misc.Stdlib.List.some_if_all_elements_are_some
        std::vector<BlockField> fields;
        bool all = true;
        for (t f : n->fields) {
          std::optional<BlockField> c = constant_field(f);
          if (!c) {
            all = false;
            break;
          }
          fields.push_back(*c);
        }
        if (!all) {
          body = initialize_symbol(n->sym, n->tag, n->fields, body);
        } else {
          ConstantDefiningValue def{ConstantDefiningValue::Kind::Block};
          def.tag = n->tag;
          def.fields = slice(fields);
          body = let_symbol(n->sym, make<ConstantDefiningValue>(def), body);
        }
        break;
      }
      case ProgramBody::Kind::Let_symbol: body = let_symbol(n->sym, n->def, body); break;
      case ProgramBody::Kind::Let_rec_symbol: body = let_rec_symbol(n->defs, body); break;
      case ProgramBody::Kind::Effect: body = effect(n->expr, body); break;
      case ProgramBody::Kind::End: break;
    }
  }
  return body;
}

}  // namespace

Program run(const Program& program) { return Program{program.imported_symbols, loop(program.program_body)}; }

}  // namespace cppcaml::typing::initialize_symbol_to_let_symbol
