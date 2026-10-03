// Port of middle_end/flambda/share_constants.ml (see share_constants.hpp).
// Constant_defining_value.Tbl is only looked up and added to (once per
// key): an ordered map by Constant_defining_value.compare stands for it.
#include "cppcaml/typing/share_constants.hpp"

#include <map>
#include <vector>

#include "cppcaml/typing/flambda_iterators.hpp"

namespace cppcaml::typing::share_constants {

using namespace flambda;

namespace {
struct CdvLess {
  bool operator()(constant_defining_value a, constant_defining_value b) const {
    return compare_constant_defining_value(a, b) < 0;
  }
};
using SharingTbl = symbol::Tbl<symbol::t>;

symbol::t substitute_symbol(const SharingTbl& sharing_symbol_tbl, symbol::t sym) {
  const symbol::t* s = sharing_symbol_tbl.find_opt(sym);
  return s ? *s : sym;
}

constant_defining_value update_constant_for_sharing(const SharingTbl& sharing_symbol_tbl, constant_defining_value c) {
  using K = ConstantDefiningValue::Kind;
  switch (c->kind) {
    case K::Allocated_const: return c;
    case K::Block: {
      std::vector<BlockField> fields;  // List.map: in order
      for (const BlockField& f : c->fields) {
        if (!f.sym) fields.push_back(f);
        else {
          BlockField nf;
          nf.sym = substitute_symbol(sharing_symbol_tbl, f.sym);
          fields.push_back(nf);
        }
      }
      ConstantDefiningValue r = *c;
      r.fields = slice(fields);
      return make<ConstantDefiningValue>(r);
    }
    case K::Set_of_closures: {
      ConstantDefiningValue r = *c;
      r.set = flambda_iterators::map_symbols_on_set_of_closures(
          c->set, [&](symbol::t s) { return substitute_symbol(sharing_symbol_tbl, s); });
      return make<ConstantDefiningValue>(r);
    }
    case K::Project_closure: {
      ConstantDefiningValue r = *c;
      r.sym = substitute_symbol(sharing_symbol_tbl, c->sym);
      return make<ConstantDefiningValue>(r);
    }
  }
  return c;
}

bool cannot_share(constant_defining_value c) {
  // Strings and float arrays are mutable; we never share them.
  return c->kind == ConstantDefiningValue::Kind::Allocated_const &&
         (c->c->kind == AllocatedConst::Kind::String || c->c->kind == AllocatedConst::Kind::Float_array);
}
}  // namespace

Program share_constants(const Program& program) {
  program_body end_node = program.program_body;
  while (end_node->kind != ProgramBody::Kind::End) end_node = end_node->body;
  symbol::t end_symbol = end_node->sym;
  SharingTbl sharing_symbol_tbl(42);
  std::map<constant_defining_value, symbol::t, CdvLess> constant_to_symbol_tbl;
  // share_definition ... (None: the definition is dropped)
  auto share_definition = [&](symbol::t sym, constant_defining_value def) -> constant_defining_value {
    def = update_constant_for_sharing(sharing_symbol_tbl, def);
    // The symbol exported by the unit (end_symbol), cannot be removed from
    // the module.  We prevent it from being shared to avoid that.
    if (cannot_share(def) || symbol::equal(sym, end_symbol)) return def;
    auto it = constant_to_symbol_tbl.find(def);
    if (it == constant_to_symbol_tbl.end()) {
      constant_to_symbol_tbl.emplace(def, sym);
      return def;
    }
    sharing_symbol_tbl.add(sym, it->second);
    return nullptr;
  };
  auto map_symbols = [&](t e) {
    return flambda_iterators::map_symbols(e, [&](symbol::t s) { return substitute_symbol(sharing_symbol_tbl, s); });
  };
  // the loop top-down (each node's work before the rest's), the program
  // rebuilt bottom-up
  struct Node {
    program_body orig;
    constant_defining_value def;  // Let_symbol (null: dropped)
    std::vector<SymbolBinding> defs;
    std::vector<t> fields;
    t expr = nullptr;
  };
  std::vector<Node> nodes;
  for (program_body p = program.program_body; p->kind != ProgramBody::Kind::End; p = p->body) {
    Node n{p, nullptr, {}, {}, nullptr};
    switch (p->kind) {
      case ProgramBody::Kind::Let_symbol: n.def = share_definition(p->sym, p->def); break;
      case ProgramBody::Kind::Let_rec_symbol:
        for (const SymbolBinding& b : p->defs) n.defs.push_back({b.sym, update_constant_for_sharing(sharing_symbol_tbl, b.def)});
        break;
      case ProgramBody::Kind::Initialize_symbol:
        for (t field : p->fields) n.fields.push_back(map_symbols(field));
        break;
      case ProgramBody::Kind::Effect: n.expr = map_symbols(p->expr); break;
      case ProgramBody::Kind::End: break;
    }
    nodes.push_back(std::move(n));
  }
  program_body body = end(end_node->sym);
  for (std::size_t k = nodes.size(); k-- > 0;) {
    const Node& n = nodes[k];
    switch (n.orig->kind) {
      case ProgramBody::Kind::Let_symbol:
        if (n.def) body = let_symbol(n.orig->sym, n.def, body);
        break;
      case ProgramBody::Kind::Let_rec_symbol: body = let_rec_symbol(slice(n.defs), body); break;
      case ProgramBody::Kind::Initialize_symbol:
        body = initialize_symbol(n.orig->sym, n.orig->tag, slice(n.fields), body);
        break;
      case ProgramBody::Kind::Effect: body = effect(n.expr, body); break;
      case ProgramBody::Kind::End: break;
    }
  }
  return {program.imported_symbols, body};
}

}  // namespace cppcaml::typing::share_constants
