// Port of middle_end/flambda/lift_let_to_initialize_symbol.ml (see
// lift_let_to_initialize_symbol.hpp).
#include "cppcaml/typing/lift_let_to_initialize_symbol.hpp"

#include <vector>

#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::lift_let_to_initialize_symbol {

using namespace flambda;

namespace {
bool should_copy(named n) {
  return n->kind == NK::Symbol || n->kind == NK::Read_symbol_field || n->kind == NK::Const;
}

// extracted = Expr of Variable.t * Flambda.t | Block of Variable.t * Tag.t *
// Variable.t list
struct Extracted {
  bool is_block;
  variable::t var;
  t expr = nullptr;              // Expr
  tag::t tag = 0;                // Block
  std::vector<variable::t> fields;  // Block
};

struct Accumulated {
  std::vector<std::pair<variable::t, named>> copied_lets;  // (consed: the newest last)
  std::vector<Extracted> extracted_lets;                   // (consed: the newest last)
  t terminator = nullptr;
};

Accumulated accumulate(t expr) {
  variable::Map<variable::t> substitution;
  Accumulated acc;
  for (;;) {
    auto* l = as<Let>(expr);
    if (!l) break;
    if (auto* v = as<Var>(l->body); v && variable::equal(l->var, v->var)) break;
    if (auto* e = as<NExpr>(l->defining_expr)) {
      if (auto* alias_var = as<Var>(e->expr)) {
        variable::t alias = alias_var->var;
        if (const variable::t* original_alias = substitution.find_opt(alias)) alias = *original_alias;
        substitution = substitution.add(l->var, alias);
        expr = l->body;
        continue;
      }
    }
    if (should_copy(l->defining_expr)) {
      acc.copied_lets.emplace_back(l->var, l->defining_expr);
      expr = l->body;
      continue;
    }
    variable::t renamed = variable::rename(l->var);
    named n = l->defining_expr;
    auto* p = as<NPrim>(n);
    if (p && p->prim->kind == clambda::Primitive::K::Pmakeblock && p->prim->mut == MutableFlag::Immutable) {
      Extracted x{true, l->var};
      x.tag = tag::create_exn(p->prim->n);
      for (variable::t v : p->args) {  // List.map: in order
        const variable::t* s = substitution.find_opt(v);
        x.fields.push_back(s ? *s : v);
      }
      acc.extracted_lets.push_back(std::move(x));
    } else {
      Extracted x{false, l->var};
      x.expr = flambda_utils::toplevel_substitution(substitution, create_let(renamed, n, var(renamed)));
      acc.extracted_lets.push_back(std::move(x));
    }
    expr = l->body;
  }
  acc.terminator = flambda_utils::toplevel_substitution(substitution, expr);
  return acc;
}

t rebuild_expr(const variable::Map<const flambda_utils::SymbolPath*>& extracted_definitions,
               const variable::Map<named>& copied_definitions, bool substitute, t expr) {
  t expr_with_read_symbols = flambda_utils::substitute_read_symbol_field_for_variables(extracted_definitions, expr);
  variable::Set free_variables = flambda::free_variables(expr_with_read_symbols);
  // Variable.Map.of_set: in increasing order
  variable::Map<variable::t> substitution = free_variables.fold(
      [&](variable::t x, variable::Map<variable::t> m) { return m.add(x, substitute ? variable::rename(x) : x); },
      variable::Map<variable::t>{});
  expr_with_read_symbols = flambda_utils::toplevel_substitution(substitution, expr_with_read_symbols);
  return substitution.fold(
      [&](variable::t v, variable::t declaration, t body) {
        const named* definition = copied_definitions.find_opt(v);
        if (!definition) misc::fatal_error("Lift_let_to_initialize_symbol.rebuild_expr");  // (Not_found)
        return create_let(declaration, *definition, body);
      },
      expr_with_read_symbols);
}

// kind = Initialisation of (Symbol.t * Tag.t * Flambda.t list) | Effect
struct Introduced {
  bool initialisation;
  symbol::t sym = nullptr;
  tag::t tag = 0;
  std::vector<t> fields;
  t effect = nullptr;
};

std::pair<std::vector<Introduced>, t> rebuild(const variable::Set& used_variables, const Accumulated& accumulated) {
  // Variable.Map.of_list copied_lets: the list (newest first) folded
  variable::Map<named> copied_definitions;
  for (std::size_t k = accumulated.copied_lets.size(); k-- > 0;)
    copied_definitions = copied_definitions.add(accumulated.copied_lets[k].first, accumulated.copied_lets[k].second);
  // List.map over the extracted lets (newest first): a symbol each
  std::vector<std::pair<symbol::t, const Extracted*>> accumulated_extracted_lets;
  for (std::size_t k = accumulated.extracted_lets.size(); k-- > 0;) {
    const Extracted& decl = accumulated.extracted_lets[k];
    accumulated_extracted_lets.emplace_back(symbol::of_variable(variable::rename(decl.var)), &decl);
  }
  // Blocks are lifted to direct top-level Initialize_block: accessing the
  // value be done directly through the symbol.  Other let bound variables
  // are initialized inside a size one static block: accessing the value is
  // done directly through the field 0 of the symbol.
  variable::Map<const flambda_utils::SymbolPath*> extracted_definitions;
  for (const auto& [sym, decl] : accumulated_extracted_lets) {
    auto* path = make<flambda_utils::SymbolPath>();
    path->sym = sym;
    if (!decl->is_block) path->path = {0};
    extracted_definitions = extracted_definitions.add(decl->var, path);
  }
  std::vector<Introduced> extracted;  // List.map: in order
  for (const auto& [sym, decl] : accumulated_extracted_lets) {
    if (!decl->is_block) {
      t expr = rebuild_expr(extracted_definitions, copied_definitions, true, decl->expr);
      if (used_variables.mem(decl->var)) extracted.push_back({true, sym, tag::create_exn(0), {expr}, nullptr});
      else extracted.push_back({false, nullptr, 0, {}, expr});
    } else {
      std::vector<t> fields;  // List.map: in order
      for (variable::t v : decl->fields) fields.push_back(rebuild_expr(extracted_definitions, copied_definitions, true, var(v)));
      extracted.push_back({true, sym, decl->tag, std::move(fields), nullptr});
    }
  }
  // We don't need to substitute the variables in the terminator, we
  // suppose that we did for every other occurrence.  Avoiding this
  // substitution allows this transformation to be idempotent.
  t terminator = rebuild_expr(extracted_definitions, copied_definitions, false, accumulated.terminator);
  return {std::vector<Introduced>(extracted.rbegin(), extracted.rend()), terminator};
}

std::pair<std::vector<Introduced>, t> introduce_symbols(t expr) {
  Accumulated accumulated = accumulate(expr);
  variable::Set used = used_variables(expr);
  return rebuild(used, accumulated);
}

program_body add_extracted(const std::vector<Introduced>& introduced, program_body program) {
  // List.fold_right: the last first
  for (std::size_t k = introduced.size(); k-- > 0;) {
    const Introduced& x = introduced[k];
    if (x.initialisation) program = initialize_symbol(x.sym, x.tag, slice(x.fields), program);
    else program = effect(x.effect, program);
  }
  return program;
}
}  // namespace

Program lift(const Program& program) {
  // split_program: the rest first (the recursion), then the node
  std::vector<program_body> nodes;
  program_body p = program.program_body;
  for (; p->kind != ProgramBody::Kind::End; p = p->body) nodes.push_back(p);
  program_body body = end(p->sym);
  for (std::size_t k = nodes.size(); k-- > 0;) {
    program_body n = nodes[k];
    switch (n->kind) {
      case ProgramBody::Kind::Let_symbol: body = let_symbol(n->sym, n->def, body); break;
      case ProgramBody::Kind::Let_rec_symbol: body = let_rec_symbol(n->defs, body); break;
      case ProgramBody::Kind::Effect: {
        auto [introduced, expr] = introduce_symbols(n->expr);
        body = add_extracted(introduced, effect(expr, body));
        break;
      }
      case ProgramBody::Kind::Initialize_symbol:
        if (n->fields.size() >= 2) {
          // CR-someday pchambart: currently the only initialize_symbol with
          // more than 1 field is the module block.
          body = initialize_symbol(n->sym, n->tag, n->fields, body);
        } else if (n->fields.empty()) {
          ConstantDefiningValue b{ConstantDefiningValue::Kind::Block};
          b.tag = n->tag;
          body = let_symbol(n->sym, make<ConstantDefiningValue>(b), body);
        } else {
          auto [introduced, field] = introduce_symbols(n->fields[0]);
          body = add_extracted(introduced, initialize_symbol(n->sym, n->tag, slice(std::vector<t>{field}), body));
        }
        break;
      case ProgramBody::Kind::End: break;
    }
  }
  return {program.imported_symbols, body};
}

}  // namespace cppcaml::typing::lift_let_to_initialize_symbol
