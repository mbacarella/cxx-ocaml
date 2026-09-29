// Port of middle_end/clambda.ml and the backend half of lambda/debuginfo.ml.
#include "cppcaml/typing/clambda.hpp"

#include <unordered_map>

#include <cstring>

namespace cppcaml::typing {

// ---- Debuginfo ------------------------------------------------------------------------------
namespace debuginfo {

static Item item_from_location(scopes sc, const Location& loc) {
  bool valid_endpos = loc.loc_end.pos_fname == loc.loc_start.pos_fname;
  Item d;
  d.dinfo_file = loc.loc_start.pos_fname;
  d.dinfo_line = loc.loc_start.pos_lnum;
  d.dinfo_char_start = loc.loc_start.pos_cnum - loc.loc_start.pos_bol;
  d.dinfo_char_end = valid_endpos ? loc.loc_end.pos_cnum - loc.loc_start.pos_bol
                                  : loc.loc_start.pos_cnum - loc.loc_start.pos_bol;
  d.dinfo_start_bol = loc.loc_start.pos_bol;
  d.dinfo_end_bol = valid_endpos ? loc.loc_end.pos_bol : loc.loc_start.pos_bol;
  d.dinfo_end_line = valid_endpos ? loc.loc_end.pos_lnum : loc.loc_start.pos_lnum;
  d.dinfo_scopes = sc;
  return d;
}

t from_location(const lambda::ScopedLocation& l) {
  if (!l.known) return {};
  return slice<Item>({item_from_location(l.sc, l.loc)});
}

Location to_location(const t& dbg) {
  if (dbg.empty()) return location::none();
  const Item& d = dbg[0];
  Location l;
  l.loc_ghost = false;
  l.loc_start = mkpos(d.dinfo_file, d.dinfo_line, d.dinfo_start_bol, d.dinfo_start_bol + d.dinfo_char_start);
  l.loc_end = mkpos(d.dinfo_file, d.dinfo_end_line, d.dinfo_end_bol, d.dinfo_start_bol + d.dinfo_char_end);
  return l;
}

namespace {
struct Shape {
  std::vector<std::uint64_t> cells, items;
};
std::unordered_map<const Item*, Shape>& shapes() {
  static std::unordered_map<const Item*, Shape> m;
  return m;
}
}  // namespace

std::uint64_t fresh_key() {
  static std::uint64_t n = 0;
  return ++n;
}
std::uint64_t cell_key(const t& d, std::size_t k) {
  if (auto it = shapes().find(d.p); it != shapes().end() && k < it->second.cells.size()) return it->second.cells[k];
  return reinterpret_cast<std::uint64_t>(d.p + k) | (std::uint64_t{1} << 63);
}
std::uint64_t item_key(const t& d, std::size_t k) {
  if (auto it = shapes().find(d.p); it != shapes().end() && k < it->second.items.size()) return it->second.items[k];
  return reinterpret_cast<std::uint64_t>(d.p + k) | (std::uint64_t{1} << 62);
}
void set_shape(const t& d, std::vector<std::uint64_t> cells, std::vector<std::uint64_t> items) {
  if (!d.empty()) shapes()[d.p] = Shape{std::move(cells), std::move(items)};
}
bool has_shape(const t& d) { return shapes().count(d.p) != 0; }

// dbg1 @ dbg2: dbg1's cells copied (its items kept), dbg2 the tail
t inline_(const t& dbg1, const t& dbg2) {
  if (dbg1.empty()) return dbg2;
  std::vector<Item> v(dbg1.begin(), dbg1.end());
  v.insert(v.end(), dbg2.begin(), dbg2.end());
  t r = slice(v);
  std::vector<std::uint64_t> cells, items;
  for (std::size_t k = 0; k < dbg1.size(); ++k) {
    cells.push_back(fresh_key());
    items.push_back(item_key(dbg1, k));
  }
  for (std::size_t k = 0; k < dbg2.size(); ++k) {
    cells.push_back(cell_key(dbg2, k));
    items.push_back(item_key(dbg2, k));
  }
  set_shape(r, std::move(cells), std::move(items));
  return r;
}

int compare(const t& dbg1, const t& dbg2) {
  auto cmp = [](long a, long b) { return a < b ? -1 : a > b ? 1 : 0; };
  // the lists compared from their ends (loop (List.rev dbg1) (List.rev dbg2))
  std::size_t i = dbg1.size(), j = dbg2.size();
  for (;;) {
    if (i == 0 && j == 0) return 0;
    if (j == 0) return 1;
    if (i == 0) return -1;
    const Item& d1 = dbg1[--i];
    const Item& d2 = dbg2[--j];
    int c = d1.dinfo_file.compare(d2.dinfo_file);
    if (c != 0) return c < 0 ? -1 : 1;
    if ((c = cmp(d1.dinfo_line, d2.dinfo_line))) return c;
    if ((c = cmp(d1.dinfo_char_end, d2.dinfo_char_end))) return c;
    if ((c = cmp(d1.dinfo_char_start, d2.dinfo_char_start))) return c;
    if ((c = cmp(d1.dinfo_start_bol, d2.dinfo_start_bol))) return c;
    if ((c = cmp(d1.dinfo_end_bol, d2.dinfo_end_bol))) return c;
    if ((c = cmp(d1.dinfo_end_line, d2.dinfo_end_line))) return c;
  }
}

std::string to_string(const t& dbg) {
  if (dbg.empty()) return "";
  std::string r = "{";
  for (std::size_t k = 0; k < dbg.size(); ++k) {
    const Item& d = dbg[k];
    if (k) r += ";";
    r += std::string(d.dinfo_file) + ":" + std::to_string(d.dinfo_line) + "," + std::to_string(d.dinfo_char_start) +
         "-" + std::to_string(d.dinfo_char_end);
  }
  return r + "}";
}

}  // namespace debuginfo

namespace clambda {

unsigned long fresh_uconstant_id() {
  static unsigned long n = 0;
  return ++n;
}

namespace {
template <class T>
T* node() {
  T* n = make<T>();
  n->kind = T::K;
  return n;
}
}  // namespace

ulambda uvar(Var id) {
  auto* n = node<Uvar>();
  n->id = id;
  return n;
}
ulambda uconst(const UConstant& c) {
  auto* n = node<Uconst>();
  n->c = c;
  return n;
}
ulambda udirect_apply(FunctionLabel f, Slice<ulambda> args, const debuginfo::t& dbg) {
  auto* n = node<Udirect_apply>();
  n->f = f;
  n->args = args;
  n->dbg = dbg;
  return n;
}
ulambda ugeneric_apply(ulambda f, Slice<ulambda> args, const debuginfo::t& dbg) {
  auto* n = node<Ugeneric_apply>();
  n->f = f;
  n->args = args;
  n->dbg = dbg;
  return n;
}
ulambda uclosure(Slice<const UFunction*> funs, Slice<ulambda> fv) {
  auto* n = node<Uclosure>();
  n->funs = funs;
  n->fv = fv;
  return n;
}
ulambda uoffset(ulambda l, long ofs) {
  auto* n = node<Uoffset>();
  n->l = l;
  n->ofs = ofs;
  return n;
}
ulambda ulet(MutableFlag mut, ValueKind k, VarWithProvenance id, ulambda arg, ulambda body) {
  auto* n = node<Ulet>();
  n->mut = mut;
  n->k = k;
  n->id = id;
  n->arg = arg;
  n->body = body;
  return n;
}
ulambda uphantom_let(VarWithProvenance id, const UPhantomDefiningExpr* def, ulambda body) {
  auto* n = node<Uphantom_let>();
  n->id = id;
  n->def = def;
  n->body = body;
  return n;
}
ulambda uprim(const Primitive& p, Slice<ulambda> args, const debuginfo::t& dbg) {
  auto* n = node<Uprim>();
  n->p = p;
  n->args = args;
  n->dbg = dbg;
  return n;
}
ulambda uswitch(ulambda arg, const USwitch& sw, const debuginfo::t& dbg) {
  auto* n = node<Uswitch>();
  n->arg = arg;
  n->sw = sw;
  n->dbg = dbg;
  return n;
}
ulambda ustringswitch(ulambda arg, Slice<UStringCase> cases, ulambda def) {
  auto* n = node<Ustringswitch>();
  n->arg = arg;
  n->cases = cases;
  n->def = def;
  return n;
}
ulambda ustaticfail(long i, Slice<ulambda> args) {
  auto* n = node<Ustaticfail>();
  n->i = i;
  n->args = args;
  return n;
}
ulambda ucatch(long i, Slice<UParam> vars, ulambda body, ulambda handler) {
  auto* n = node<Ucatch>();
  n->i = i;
  n->vars = vars;
  n->body = body;
  n->handler = handler;
  return n;
}
ulambda utrywith(ulambda body, VarWithProvenance exn, ulambda handler) {
  auto* n = node<Utrywith>();
  n->body = body;
  n->exn = exn;
  n->handler = handler;
  return n;
}
ulambda uifthenelse(ulambda c, ulambda a, ulambda b) {
  auto* n = node<Uifthenelse>();
  n->cond = c;
  n->ifso = a;
  n->ifnot = b;
  return n;
}
ulambda usequence(ulambda a, ulambda b) {
  auto* n = node<Usequence>();
  n->l1 = a;
  n->l2 = b;
  return n;
}
ulambda uwhile(ulambda c, ulambda b) {
  auto* n = node<Uwhile>();
  n->cond = c;
  n->body = b;
  return n;
}
ulambda ufor(VarWithProvenance id, ulambda lo, ulambda hi, parsetree::DirectionFlag dir, ulambda body) {
  auto* n = node<Ufor>();
  n->id = id;
  n->lo = lo;
  n->hi = hi;
  n->dir = dir;
  n->body = body;
  return n;
}
ulambda uassign(Var id, ulambda e) {
  auto* n = node<Uassign>();
  n->id = id;
  n->e = e;
  return n;
}
ulambda usend(lambda::MethKind k, ulambda met, ulambda obj, Slice<ulambda> args, const debuginfo::t& dbg) {
  auto* n = node<Usend>();
  n->k = k;
  n->met = met;
  n->obj = obj;
  n->args = args;
  n->dbg = dbg;
  return n;
}
ulambda uunreachable() { return node<Uunreachable>(); }

const ValueApproximation* value_unknown() {
  static const ValueApproximation* u = [] {
    ZoneScope perm(permanent_zone());
    auto* a = make<ValueApproximation>();
    a->kind = ValueApproximation::Kind::Value_unknown;
    return a;
  }();
  return u;
}

// ---- comparison functions for constants ------------------------------------------------------
namespace {
int icmp(std::int64_t a, std::int64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
std::int64_t bits_of_float(double x) {
  std::int64_t b;
  std::memcpy(&b, &x, sizeof b);
  return b;
}
int compare_floats(double x1, double x2) { return icmp(bits_of_float(x1), bits_of_float(x2)); }
int string_compare(std::string_view a, std::string_view b) {
  int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}
int compare_float_lists(const Slice<double>& l1, const Slice<double>& l2) {
  for (std::size_t k = 0;; ++k) {
    if (k == l1.size() && k == l2.size()) return 0;
    if (k == l1.size()) return -1;
    if (k == l2.size()) return 1;
    if (int c = compare_floats(l1[k], l2[k])) return c;
  }
}
int compare_constant_lists(const Slice<UConstant>& l1, const Slice<UConstant>& l2) {
  for (std::size_t k = 0;; ++k) {
    if (k == l1.size() && k == l2.size()) return 0;
    if (k == l1.size()) return -1;
    if (k == l2.size()) return 1;
    if (int c = compare_constants(l1[k], l2[k])) return c;
  }
}
}  // namespace

int compare_constants(const UConstant& c1, const UConstant& c2) {
  using K = UConstant::Kind;
  if (c1.kind == K::Uconst_ref && c2.kind == K::Uconst_ref) return string_compare(c1.sym, c2.sym);
  if (c1.kind == K::Uconst_int && c2.kind == K::Uconst_int) return icmp(c1.i, c2.i);
  if (c1.kind == K::Uconst_ref) return -1;
  return 1;
}

int compare_structured_constants(const UStructuredConstant* c1, const UStructuredConstant* c2) {
  using K = UStructuredConstant::Kind;
  if (c1->kind == c2->kind) {
    switch (c1->kind) {
      case K::Uconst_float: return compare_floats(c1->f, c2->f);
      case K::Uconst_int32:
      case K::Uconst_int64:
      case K::Uconst_nativeint: return icmp(c1->i, c2->i);
      case K::Uconst_block: {
        long c = c1->tag - c2->tag;  // no overflow possible here
        if (c != 0) return static_cast<int>(c);
        return compare_constant_lists(c1->fields, c2->fields);
      }
      case K::Uconst_float_array: return compare_float_lists(c1->floats, c2->floats);
      case K::Uconst_string: return string_compare(c1->s, c2->s);
      case K::Uconst_closure: return string_compare(c1->s, c2->s);
    }
  }
  // no overflow possible here
  return static_cast<int>(c1->kind) - static_cast<int>(c2->kind);
}

}  // namespace clambda
}  // namespace cppcaml::typing
