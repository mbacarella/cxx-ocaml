#include "cppcaml/typer.hpp"

namespace cppcaml {
namespace {

namespace tt = typedtree;
using namespace ast;

struct Typer {
  // Local ident stamps start at an arbitrary base; the parity harness
  // normalizes stamps, so only their relative order matters for now.
  long long next_stamp = 274;

  tt::Ident fresh_local(const std::string& name) {
    return tt::Ident{name, next_stamp++, false};
  }

  tt::Pattern pattern(const Pattern& p) {
    tt::Pattern out;
    out.loc = p.loc;
    if (std::holds_alternative<Ppat_any>(p.desc)) {
      out.desc = tt::Tpat_any{};
    } else if (auto* v = std::get_if<Ppat_var>(&p.desc)) {
      out.desc = tt::Tpat_var{fresh_local(v->name.txt)};
    } else {
      throw TypeError("unsupported pattern");
    }
    return out;
  }

  tt::Expression expr(const Expression& e) {
    tt::Expression out;
    out.loc = e.loc;
    if (auto* c = std::get_if<Pexp_constant>(&e.desc)) {
      out.desc = tt::Texp_constant{c->c};
    } else if (auto* t = std::get_if<Pexp_tuple>(&e.desc)) {
      tt::Texp_tuple tup;
      for (size_t k = 0; k < t->elems.size(); ++k) {
        std::optional<std::string> label;
        if (k < t->labels.size()) label = t->labels[k];
        tup.elems.emplace_back(
            label, std::make_unique<tt::Expression>(expr(*t->elems[k])));
      }
      out.desc = std::move(tup);
    } else {
      throw TypeError("unsupported expression");
    }
    return out;
  }

  tt::ValueBinding value_binding(const ValueBinding& vb) {
    tt::ValueBinding out;
    // Bind the RHS first, then the pattern, matching ident-allocation order.
    out.expr = expr(*vb.expr);
    out.pat = pattern(vb.pat);
    return out;
  }

  tt::StructureItem structure_item(const StructureItem& it) {
    auto* sv = std::get_if<Pstr_value>(&it.desc);
    if (!sv) throw TypeError("unsupported structure item");
    tt::Tstr_value out;
    out.rf = sv->rf;
    for (auto& vb : sv->bindings) out.bindings.push_back(value_binding(vb));
    tt::StructureItem si;
    si.loc = it.loc;
    si.desc = std::move(out);
    return si;
  }
};

}  // namespace

typedtree::Structure type_structure(const ast::Structure& s) {
  Typer t;
  typedtree::Structure out;
  for (auto& it : s) out.push_back(t.structure_item(it));
  return out;
}

}  // namespace cppcaml
