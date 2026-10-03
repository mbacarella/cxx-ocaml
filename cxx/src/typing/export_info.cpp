// Port of middle_end/flambda/export_info.ml (see export_info.hpp).
#include "cppcaml/typing/export_info.hpp"

#include <cstdio>
#include <cstring>

#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::export_info {

using format::Formatter;
using format::fprintf;

const T* empty() {
  static const T* e = permanent_zone().make<T>();
  return e;
}

bool equal_approx(const Approx& a1, const Approx& a2) {
  if (a1.kind != a2.kind) return false;
  switch (a1.kind) {
    case Approx::Kind::Value_unknown: return true;
    case Approx::Kind::Value_id: return unit_id::compare(a1.id, a2.id) == 0;
    case Approx::Kind::Value_symbol: return symbol::equal(a1.sym, a2.sym);
  }
  return false;
}

namespace {
bool equal_set_of_closures(const ValueSetOfClosures* s1, const ValueSetOfClosures* s2) {
  return unit_id::compare(s1->set_of_closures_id, s2->set_of_closures_id) == 0 &&
         variable::Map<Approx>::equal(equal_approx, s1->bound_vars, s2->bound_vars) &&
         variable::Map<Approx>::equal(equal_approx, s1->results, s2->results) &&
         ((!s1->aliased_symbol && !s2->aliased_symbol) ||
          (s1->aliased_symbol && s2->aliased_symbol && symbol::equal(s1->aliased_symbol, s2->aliased_symbol)));
}
// polymorphic equality on floats: IEEE
bool same_float_array(const ValueFloatArray& a, const ValueFloatArray& b) {
  if (a.contents_known != b.contents_known || a.size != b.size) return false;
  if (!a.contents_known) return true;
  if (a.contents.size() != b.contents.size()) return false;
  for (std::size_t k = 0; k < a.contents.size(); ++k) {
    const auto& x = a.contents[k];
    const auto& y = b.contents[k];
    if (x.has_value() != y.has_value() || (x && !(*x == *y))) return false;
  }
  return true;
}
}  // namespace

bool equal_descr(const Descr* d1, const Descr* d2) {
  using K = Descr::Kind;
  if (d1->kind != d2->kind) return false;
  switch (d1->kind) {
    case K::Value_unknown_descr: return true;
    case K::Value_block: {
      if (d1->tag != d2->tag || d1->fields.size() != d2->fields.size()) return false;
      for (std::size_t k = 0; k < d1->fields.size(); ++k)
        if (!equal_approx(d2->fields[k], d1->fields[k])) return false;
      return true;
    }
    case K::Value_mutable_block: return d1->tag == d2->tag && d1->n == d2->n;
    case K::Value_int: case K::Value_char: return d1->n == d2->n;
    case K::Value_float: return d1->f == d2->f;
    case K::Value_float_array: return same_float_array(d1->float_array, d2->float_array);
    case K::Value_boxed_int: return A::equal_boxed_int(d1->bi, d1->bival, d2->bi, d2->bival);
    case K::Value_string:
      return d1->str.size == d2->str.size && d1->str.contents == d2->str.contents;
    case K::Value_closure:
      return variable::equal(d1->closure_id, d2->closure_id) && equal_set_of_closures(d1->set, d2->set);
    case K::Value_set_of_closures: return equal_set_of_closures(d1->set, d2->set);
  }
  return false;
}

namespace {
// Identifiable.Make_map.disjoint_union ?eq ?print m1 m2
template <class K, class V, class Cmp, class Eq, class PrintK>
OMap<K, V, Cmp> disjoint_union(const OMap<K, V, Cmp>& m1, const OMap<K, V, Cmp>& m2, Eq&& eq, PrintK&& print_key) {
  return OMap<K, V, Cmp>::union_(
      [&](const K& id, const V& v1, const V& v2) -> std::optional<V> {
        if (!eq(v1, v2)) {
          Formatter f;
          fprintf(f, "Map.disjoint_union %a", [&](Formatter& g) { print_key(g, id); });
          misc::fatal_error(f.contents());
        }
        return v1;
      },
      m1, m2);
}
}  // namespace

const T* merge(const T* t1, const T* t2) {
  auto never = [](const auto&, const auto&) { return false; };
  T r;
  // (the record's fields right to left)
  r.recursive = disjoint_union(t1->recursive, t2->recursive,
                               [](const variable::Set& a, const variable::Set& b) { return variable::Set::equal(a, b); },
                               unit_id::print);
  r.invariant_params = disjoint_union(
      t1->invariant_params, t2->invariant_params,
      [](const variable::Map<variable::Set>& a, const variable::Map<variable::Set>& b) {
        return variable::Map<variable::Set>::equal(
            [](const variable::Set& x, const variable::Set& y) { return variable::Set::equal(x, y); }, a, b);
      },
      unit_id::print);
  r.constant_closures = variable::Set::union_(t1->constant_closures, t2->constant_closures);
  auto int_eq = [](long a, long b) { return a == b; };
  r.offset_fv = disjoint_union(t1->offset_fv, t2->offset_fv, int_eq, variable::print);
  r.offset_fun = disjoint_union(t1->offset_fun, t2->offset_fun, int_eq, variable::print);
  r.symbol_id = disjoint_union(t1->symbol_id, t2->symbol_id, never, symbol::print);
  r.sets_of_closures = disjoint_union(t1->sets_of_closures, t2->sets_of_closures, never, unit_id::print);
  r.values = CUMap<export_id::Map<const Descr*>>::merge(
      [&](compilation_unit::t, const export_id::Map<const Descr*>* map1,
          const export_id::Map<const Descr*>* map2) -> std::optional<export_id::Map<const Descr*>> {
        if (!map1 && !map2) return std::nullopt;
        if (!map1) return *map2;
        if (!map2) return *map1;
        return disjoint_union(*map1, *map2, equal_descr, unit_id::print);
      },
      t1->values, t2->values);
  return make<T>(r);
}

const Descr* find_description(const T* t, export_id::t eid) {
  const auto* unit_map = t->values.find_opt(eid->unit);
  if (!unit_map) return nullptr;
  const Descr* const* d = unit_map->find_opt(eid);
  return d ? *d : nullptr;
}

CUMap<export_id::Map<const Descr*>> nest_eid_map(const export_id::Map<const Descr*>& map) {
  return map.fold(
      [](export_id::t eid, const Descr* v, CUMap<export_id::Map<const Descr*>> acc) {
        compilation_unit::t unit = eid->unit;
        const export_id::Map<const Descr*>* m = acc.find_opt(unit);
        export_id::Map<const Descr*> m2 = m ? *m : export_id::Map<const Descr*>{};
        return acc.add(unit, m2.add(eid, v));
      },
      CUMap<export_id::Map<const Descr*>>{});
}

namespace {
// Identifiable's Map.print f: "@[<1>{@[%a@ @]}@]", each binding
// "@ (@[%a@ %a@])"
template <class M, class PK, class PV>
void print_map(Formatter& ppf, const M& m, PK&& print_key, PV&& print_value) {
  auto elts = [&](Formatter& f) {
    m.iter([&](const auto& k, const auto& v) {
      fprintf(f, "@ (@[%a@ %a@])", [&](Formatter& g) { print_key(g, k); }, [&](Formatter& g) { print_value(g, v); });
    });
  };
  fprintf(ppf, "@[<1>{@[%a@ @]}@]", elts);
}

std::string string_of_float(double f) {
  char b[64];
  std::snprintf(b, sizeof b, "%.12g", f);
  std::string s = b;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || c == '-')) return s;
  return s + ".";
}

struct ApproxPrinter {
  const symbol::Map<export_id::t>& symbol_id;
  const CUMap<export_id::Map<const Descr*>>& values;
  export_id::Set printed;
  symbol::Set recorded_symbol;
  std::vector<symbol::t> symbols_to_print;  // a queue
  std::size_t queue_head = 0;
  set_of_closures_id::Set printed_set_of_closures;

  void print_approx(Formatter& ppf, const Approx& approx) {
    switch (approx.kind) {
      case Approx::Kind::Value_unknown: fprintf(ppf, "?"); break;
      case Approx::Kind::Value_id: {
        export_id::t id = approx.id;
        if (printed.mem(id)) {
          fprintf(ppf, "(%a: _)", pr(unit_id::print, id));
          break;
        }
        const auto* unit_map = values.find_opt(id->unit);
        const Descr* const* d = unit_map ? unit_map->find_opt(id) : nullptr;
        if (!d) {
          fprintf(ppf, "(%a: Not available)", pr(unit_id::print, id));
          break;
        }
        printed = printed.add(id);
        const Descr* descr = *d;
        fprintf(ppf, "@[<hov 2>(%a:@ %a)@]", pr(unit_id::print, id), [&](Formatter& f) { print_descr(f, descr); });
        break;
      }
      case Approx::Kind::Value_symbol:
        if (!recorded_symbol.mem(approx.sym)) {
          recorded_symbol = recorded_symbol.add(approx.sym);
          symbols_to_print.push_back(approx.sym);
        }
        symbol::print(ppf, approx.sym);
        break;
    }
  }
  void print_descr(Formatter& ppf, const Descr* d) {
    using K = Descr::Kind;
    switch (d->kind) {
      case K::Value_int: fprintf(ppf, "%d", d->n); break;
      case K::Value_char: fprintf(ppf, "%c", static_cast<char>(d->n)); break;
      case K::Value_block:
        fprintf(ppf, "[%d:%a]", d->tag, [&](Formatter& f) {
          for (const Approx& a : d->fields) fprintf(f, "%a@ ", [&](Formatter& g) { print_approx(g, a); });
        });
        break;
      case K::Value_mutable_block: fprintf(ppf, "[mutable %d:%i]", d->tag, d->n); break;
      case K::Value_closure:
        fprintf(ppf, "(closure %a, %a)", pr(variable::print, d->closure_id),
                [&](Formatter& f) { print_set_of_closures(f, d->set); });
        break;
      case K::Value_set_of_closures:
        fprintf(ppf, "(set_of_closures %a)", [&](Formatter& f) { print_set_of_closures(f, d->set); });
        break;
      case K::Value_string:
        if (!d->str.contents) fprintf(ppf, "string %i", d->str.size);
        else {
          std::string s(*d->str.contents);
          if (d->str.size > 10) s = s.substr(0, 8) + "...";
          fprintf(ppf, "string %i %S", d->str.size, s);
        }
        break;
      case K::Value_float: fprintf(ppf, "%s", string_of_float(d->f)); break;
      case K::Value_float_array:
        fprintf(ppf, "float_array%s %i", d->float_array.contents_known ? "_imm" : "", d->float_array.size);
        break;
      case K::Value_boxed_int:
        switch (d->bi) {
          case A::BoxedInt::Int32: fprintf(ppf, "%li", static_cast<std::int32_t>(d->bival)); break;
          case A::BoxedInt::Int64: fprintf(ppf, "%Li", d->bival); break;
          case A::BoxedInt::Nativeint: fprintf(ppf, "%ni", d->bival); break;
        }
        break;
      case K::Value_unknown_descr: fprintf(ppf, "?"); break;
    }
  }
  void print_set_of_closures(Formatter& ppf, const ValueSetOfClosures* s) {
    if (printed_set_of_closures.mem(s->set_of_closures_id)) {
      fprintf(ppf, "%a", pr(unit_id::print, s->set_of_closures_id));
      return;
    }
    printed_set_of_closures = printed_set_of_closures.add(s->set_of_closures_id);
    auto binding = [&](Formatter& f) {
      s->bound_vars.iter([&](variable::t clos_id, const Approx& a) {
        fprintf(f, "%a -> %a,@ ", pr(variable::print, clos_id), [&](Formatter& g) { print_approx(g, a); });
      });
    };
    auto alias = [&](Formatter& f) {
      if (s->aliased_symbol) fprintf(f, "@ (alias: %a)", pr(symbol::print, s->aliased_symbol));
    };
    auto results = [&](Formatter& f) {
      print_map(f, s->results, variable::print, [&](Formatter& g, const Approx& a) { print_approx(g, a); });
    };
    fprintf(ppf, "{%a: %a%a => %a}", pr(unit_id::print, s->set_of_closures_id), binding, alias, results);
  }
  void print_recorded_symbols(Formatter& ppf) {
    while (queue_head < symbols_to_print.size()) {
      symbol::t sym = symbols_to_print[queue_head++];
      if (const export_id::t* id = symbol_id.find_opt(sym))
        fprintf(ppf, "@[<hov 2>%a:@ %a@];@ ", pr(symbol::print, sym),
                [&](Formatter& f) { print_approx(f, Approx{Approx::Kind::Value_id, *id, nullptr}); });
    }
  }
};
}  // namespace

void print_approx(Formatter& ppf, const T* t, const std::vector<symbol::t>& root_symbols) {
  ApproxPrinter p{t->symbol_id, t->values, {}, {}, root_symbols, 0, {}};
  fprintf(ppf, "@[<hov 2>Globals:@ ");
  fprintf(ppf, "@]@ @[<hov 2>Symbols:@ ");
  p.print_recorded_symbols(ppf);
  fprintf(ppf, "@]");
}

void print_functions(Formatter& ppf, const T* t) {
  print_map(ppf, t->sets_of_closures, unit_id::print,
            [](Formatter& f, const A::FunctionDeclarations* fd) { A::print_function_declarations(f, fd); });
}

}  // namespace cppcaml::typing::export_info
