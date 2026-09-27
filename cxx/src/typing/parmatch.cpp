// Port of typing/patterns.ml and typing/parmatch.ml: detection of partial
// matches and unused match cases, and the closing of polymorphic variant
// rows ("pressure").  Warnings are not emitted; the checks that only run for
// an enabled warning run under ocamlc's default warning set (warnings.hpp).
// The warning-only computations (fragile matches, ambiguous or-pattern
// bindings, the counter-example message) are left out: they have no effect
// on typing.
#include <set>
#include <cstdlib>
#include <cstring>

#include "cppcaml/typing/btype.hpp"
#include "cppcaml/typing/ctype.hpp"
#include "cppcaml/typing/parmatch.hpp"
#include "cppcaml/typing/printpat.hpp"
#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/location.hpp"
#include "cppcaml/typing/predef.hpp"
#include "cppcaml/typing/subst.hpp"
#include "cppcaml/typing/warnings.hpp"

namespace cppcaml::typing::parmatch {

using namespace types;
using PK = tt::PatternDesc::Kind;
using CK = tt::Constant::Kind;
using parsetree::as;

// ---- patterns.ml ---------------------------------------------------------------------------
namespace {

template <class D>
const D* mkd(D d) {
  return make<D>(std::move(d));
}

// make_pat desc ty tenv
const tt::Pattern* make_pat(const tt::PatternDesc* desc, TypeExpr* ty, env::t tenv) {
  return make<tt::Pattern>(desc, location::none(), Slice<tt::PatExtraItem>{}, ty, tenv, tt::Attributes{});
}
const tt::Pattern* with_desc(const tt::Pattern* p, const tt::PatternDesc* d) {
  tt::Pattern* q = make<tt::Pattern>(*p);
  q->pat_desc = d;
  return q;
}

// Patterns.omega (a module-initialization value)
const tt::Pattern* omega() {
  static const tt::Pattern* o = [] {
    ZoneScope perm(permanent_zone());
    return make<tt::Pattern>(make<tt::Tpat_any>(tt::Tpat_any{{PK::Tpat_any}}), location::none(),
                             Slice<tt::PatExtraItem>{}, ctype::none(), env::empty(), tt::Attributes{});
  }();
  return o;
}
std::vector<const tt::Pattern*> omegas(long i) {
  return std::vector<const tt::Pattern*>(i > 0 ? static_cast<std::size_t>(i) : 0, omega());
}
template <class L>
std::vector<const tt::Pattern*> omega_list(const L& l) {
  return std::vector<const tt::Pattern*>(l.size(), omega());
}

// Patterns.General.(view p |> strip_vars): the returned pattern is never a
// Tpat_var or a Tpat_alias (a Simple or Half_simple pattern)
const tt::Pattern* strip_vars(const tt::Pattern* p) {
  for (;;) {
    if (auto* a = as<tt::Tpat_alias>(p->pat_desc)) {
      p = a->pat;
      continue;
    }
    if (p->pat_desc->kind == PK::Tpat_var) return with_desc(p, mkd(tt::Tpat_any{{PK::Tpat_any}}));
    return p;
  }
}

// Patterns.Head
enum class HK { Any, Construct, Constant, Tuple, Record, Variant, Array, Lazy };
struct Head {
  HK kind;
  const ConstructorDescription* cstr = nullptr;    // Construct
  tt::Constant c{};                                // Constant
  std::vector<OptStr> tuple;                       // Tuple
  std::vector<const LabelDescription*> lbls;       // Record
  std::string_view tag;                            // Variant
  bool has_arg = false;                            // Variant
  tt::RowDescRef* cstr_row = nullptr;              // Variant
  MutableFlag am = MutableFlag::Immutable;         // Array
  long n = 0;                                      // Array
  // pattern_data
  Location pat_loc;
  Slice<tt::PatExtraItem> pat_extra;
  TypeExpr* pat_type = nullptr;
  env::t pat_env = nullptr;
  tt::Attributes pat_attributes;
  // the row of the type may evolve if [close_variant] is called, hence the
  // delay
  const RowDesc* type_row() const {
    auto* v = as<Tvariant>(get_desc(ctype::expand_head(pat_env, pat_type)));
    if (!v) throw std::logic_error("Patterns.Head.type_row");
    return v->row;
  }
};
Head* head_of(const tt::Pattern* q, HK k) {
  Head* h = make<Head>();
  h->kind = k;
  h->pat_loc = q->pat_loc;
  h->pat_extra = q->pat_extra;
  h->pat_type = q->pat_type;
  h->pat_env = q->pat_env;
  h->pat_attributes = q->pat_attributes;
  return h;
}
const Head* head_omega() {
  static const Head* h = [] {
    ZoneScope perm(permanent_zone());
    return head_of(omega(), HK::Any);
  }();
  return h;
}
using Pats = std::vector<const tt::Pattern*>;
struct Deconstructed {
  const Head* head;
  Pats args;
};
// [deconstruct p] returns the head of [p] and the list of sub patterns.
Deconstructed deconstruct(const tt::Pattern* q) {
  const tt::PatternDesc* d = q->pat_desc;
  switch (d->kind) {
    case PK::Tpat_any: return {head_of(q, HK::Any), {}};
    case PK::Tpat_constant: {
      Head* h = head_of(q, HK::Constant);
      h->c = as<tt::Tpat_constant>(d)->c;
      return {h, {}};
    }
    case PK::Tpat_tuple: {
      Head* h = head_of(q, HK::Tuple);
      Pats args;
      for (auto& x : as<tt::Tpat_tuple>(d)->pats) {
        h->tuple.push_back(x.label);
        args.push_back(x.pat);
      }
      return {h, args};
    }
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      Head* h = head_of(q, HK::Construct);
      h->cstr = c->cstr;
      return {h, Pats(c->args.begin(), c->args.end())};
    }
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      Head* h = head_of(q, HK::Variant);
      h->tag = v->label;
      h->has_arg = v->arg != nullptr;
      h->cstr_row = v->row;
      Pats args;
      if (v->arg) args.push_back(v->arg);
      return {h, args};
    }
    case PK::Tpat_array: {
      auto* a = as<tt::Tpat_array>(d);
      Head* h = head_of(q, HK::Array);
      h->am = a->mut;
      h->n = static_cast<long>(a->pats.size());
      return {h, Pats(a->pats.begin(), a->pats.end())};
    }
    case PK::Tpat_record: {
      Head* h = head_of(q, HK::Record);
      Pats pats;
      for (auto& f : as<tt::Tpat_record>(d)->fields) {
        h->lbls.push_back(f.label);
        pats.push_back(f.pat);
      }
      return {h, pats};
    }
    case PK::Tpat_lazy: return {head_of(q, HK::Lazy), {as<tt::Tpat_lazy>(d)->pat}};
    default: throw std::logic_error("Patterns.Head.deconstruct: not a simple pattern");
  }
}
// reconstructs a pattern, putting wildcards as sub-patterns.
const tt::Pattern* to_omega_pattern(const Head* t) {
  const tt::PatternDesc* pat_desc;
  switch (t->kind) {
    case HK::Any: pat_desc = mkd(tt::Tpat_any{{PK::Tpat_any}}); break;
    case HK::Lazy: pat_desc = mkd(tt::Tpat_lazy{{PK::Tpat_lazy}, omega()}); break;
    case HK::Constant: pat_desc = mkd(tt::Tpat_constant{{PK::Tpat_constant}, t->c}); break;
    case HK::Tuple: {
      std::vector<tt::LabeledPattern> l;
      for (auto& lbl : t->tuple) l.push_back({lbl, omega()});
      pat_desc = mkd(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(l)});
      break;
    }
    case HK::Array: pat_desc = mkd(tt::Tpat_array{{PK::Tpat_array}, t->am, slice(omegas(t->n))}); break;
    case HK::Construct: {
      tt::LidLoc lid_loc{Longident::lident(t->cstr->cstr_name), t->pat_loc};
      pat_desc = mkd(tt::Tpat_construct{{PK::Tpat_construct}, lid_loc, t->cstr, slice(omegas(t->cstr->cstr_arity)), nullptr});
      break;
    }
    case HK::Variant:
      pat_desc = mkd(tt::Tpat_variant{{PK::Tpat_variant}, t->tag, t->has_arg ? omega() : nullptr, t->cstr_row});
      break;
    case HK::Record: {
      std::vector<tt::RecordPatField> lst;
      for (auto* lbl : t->lbls) lst.push_back({tt::LidLoc{Longident::lident(lbl->lbl_name), t->pat_loc}, lbl, omega()});
      pat_desc = mkd(tt::Tpat_record{{PK::Tpat_record}, slice(lst), ClosedFlag::Closed});
      break;
    }
    default: throw std::logic_error("to_omega_pattern");
  }
  return make<tt::Pattern>(pat_desc, t->pat_loc, Slice<tt::PatExtraItem>{}, t->pat_type, t->pat_env,
                           t->pat_attributes);
}

// ---- OCaml's float_of_string / string_of_float (for float constants) ------------------------
double float_of_string(std::string_view s) {
  std::string buf;
  for (char c : s)
    if (c != '_') buf.push_back(c);
  char* end = nullptr;
  double d = std::strtod(buf.c_str(), &end);
  if (end != buf.c_str() + buf.size()) throw std::invalid_argument("float_of_string");
  return d;
}
std::string string_of_float(double f) {
  char b[64];
  std::snprintf(b, sizeof b, "%.12g", f);
  std::string s = b;
  // valid_float_lexem
  for (char c : s)
    if (!((c >= '0' && c <= '9') || c == '-')) return s;
  return s + ".";
}
// Stdlib.compare on floats
int compare_float(double a, double b) {
  if (a < b) return -1;
  if (a > b) return 1;
  if (a == b) return 0;
  // nan is equal to itself and smaller than every other float
  if (a != a) return b != b ? 0 : -1;
  return 1;
}
template <class T>
int cmp(T a, T b) {
  return a < b ? -1 : a > b ? 1 : 0;
}

}  // namespace

// ---- parmatch.ml ---------------------------------------------------------------------------
TypedCase typed_case(const tt::Case* c) {
  return {c->c_lhs, c->c_guard != nullptr, c->c_rhs->exp_desc->kind == tt::ExpressionDesc::Kind::Texp_unreachable};
}
UntypedCase untyped_case(const parsetree::Case* c) {
  return {c->pc_lhs, c->pc_guard != nullptr,
          c->pc_rhs->pexp_desc->kind == parsetree::ExpressionDesc::Kind::Pexp_unreachable};
}

namespace {

// extra_pat (a module-initialization value)
const tt::Pattern* extra_pat() {
  static const tt::Pattern* p = [] {
    ZoneScope perm(permanent_zone());
    return make_pat(mkd(tt::Tpat_var{{PK::Tpat_var}, Ident::create_local(OCAML_LIT("+")), tt::StrLoc{"+", location::none()},
                                     uid::internal_not_actually_unique()}),
                    ctype::none(), env::empty());
  }();
  return p;
}

// ---- coherence check ---------------------------------------------------------------------
// Given the first column of a simplified matrix, look for a
// "discriminating" pattern on that column (i.e. a non-omega one) and then
// check that every other head pattern in the column is coherent with it.
bool all_coherent(const std::vector<const Head*>& column) {
  auto coherent_heads = [](const Head* hp1, const Head* hp2) {
    if (hp1->kind == HK::Any || hp2->kind == HK::Any) return true;
    if (hp1->kind != hp2->kind) return false;
    switch (hp1->kind) {
      case HK::Construct:
        return hp1->cstr->cstr_consts == hp2->cstr->cstr_consts &&
               hp1->cstr->cstr_nonconsts == hp2->cstr->cstr_nonconsts;
      case HK::Constant: return hp1->c.kind == hp2->c.kind;
      case HK::Tuple: return hp1->tuple == hp2->tuple;
      case HK::Record:
        if (hp1->lbls.empty() && hp2->lbls.empty()) return true;
        if (hp1->lbls.empty() || hp2->lbls.empty()) return false;
        return hp1->lbls[0]->lbl_all.size() == hp2->lbls[0]->lbl_all.size();
      case HK::Array: return hp1->am == hp2->am;
      case HK::Variant:
      case HK::Lazy: return true;
      case HK::Any: return true;
    }
    return false;
  };
  const Head* discr_pat = nullptr;
  for (auto* h : column)
    if (h->kind != HK::Any) {
      discr_pat = h;
      break;
    }
  // only omegas on the column: the column is coherent.
  if (!discr_pat) return true;
  for (auto* h : column)
    if (!coherent_heads(discr_pat, h)) return false;
  return true;
}

}  // namespace

// the module-initialization values of parmatch.ml, created at startup (see
// typemod::install_forward_refs)
void module_init() { (void)extra_pat(); }

// ---- compatibility check -----------------------------------------------------------------
namespace {
bool is_absent(std::string_view tag, const tt::RowDescRef* row) {
  return row_field_repr(get_row_field(tag, row->contents)).kind == RowFieldView::Kind::Rabsent;
}
bool is_absent_pat(const Head* d) { return d->kind == HK::Variant && is_absent(d->tag, d->cstr_row); }
}  // namespace

int const_compare(const tt::Constant& x, const tt::Constant& y) {
  if (x.kind == CK::Const_float && y.kind == CK::Const_float)
    return compare_float(float_of_string(x.s), float_of_string(y.s));
  if (x.kind == CK::Const_string && y.kind == CK::Const_string) return x.s.compare(y.s) < 0 ? -1 : x.s == y.s ? 0 : 1;
  // Stdlib.compare x y: the constructor order first
  if (x.kind != y.kind) return cmp(static_cast<int>(x.kind), static_cast<int>(y.kind));
  switch (x.kind) {
    case CK::Const_int:
    case CK::Const_char: return cmp(x.i, y.i);
    case CK::Const_int32:
    case CK::Const_int64:
    case CK::Const_nativeint: return cmp(x.boxed, y.boxed);
    case CK::Const_string: {
      // (unreachable) Const_string (s, loc, delim): compared fieldwise
      int c = x.s.compare(y.s);
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    case CK::Const_float: {
      int c = x.s.compare(y.s);
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
  }
  return 0;
}

namespace {

// Invariant: fields are already sorted by Typecore.type_label_a_list
std::pair<Pats, Pats> records_args(Slice<tt::RecordPatField> l1, Slice<tt::RecordPatField> l2) {
  Pats r1, r2;
  std::size_t i = 0, j = 0;
  while (i < l1.size() || j < l2.size()) {
    if (i == l1.size()) {
      r1.push_back(omega());
      r2.push_back(l2[j++].pat);
    } else if (j == l2.size()) {
      r1.push_back(l1[i++].pat);
      r2.push_back(omega());
    } else if (l1[i].label->lbl_pos < l2[j].label->lbl_pos) {
      r1.push_back(l1[i++].pat);
      r2.push_back(omega());
    } else if (l1[i].label->lbl_pos > l2[j].label->lbl_pos) {
      r1.push_back(omega());
      r2.push_back(l2[j++].pat);
    } else {  // same label on both sides
      r1.push_back(l1[i++].pat);
      r2.push_back(l2[j++].pat);
    }
  }
  return {r1, r2};
}

}  // namespace

// module Compat (Constr : sig val equal : .. end)
bool compat_with(const std::function<bool(const ConstructorDescription*, const ConstructorDescription*)>& equal,
                 const tt::Pattern* p, const tt::Pattern* q);
static bool compats_with(const std::function<bool(const ConstructorDescription*, const ConstructorDescription*)>& equal,
                         const Pats& ps, const Pats& qs) {
  std::size_t k = 0;
  for (; k < ps.size() && k < qs.size(); ++k)
    if (!compat_with(equal, ps[k], qs[k])) return false;
  return ps.size() == qs.size();
}
bool compat_with(const std::function<bool(const ConstructorDescription*, const ConstructorDescription*)>& equal,
                 const tt::Pattern* p, const tt::Pattern* q) {
  const tt::PatternDesc* pd = p->pat_desc;
  const tt::PatternDesc* qd = q->pat_desc;
  // Variables match any value
  if (pd->kind == PK::Tpat_any || pd->kind == PK::Tpat_var || qd->kind == PK::Tpat_any || qd->kind == PK::Tpat_var)
    return true;
  // Structural induction
  if (auto* a = as<tt::Tpat_alias>(pd)) return compat_with(equal, a->pat, q);
  if (auto* a = as<tt::Tpat_alias>(qd)) return compat_with(equal, p, a->pat);
  if (auto* o = as<tt::Tpat_or>(pd)) return compat_with(equal, o->p1, q) || compat_with(equal, o->p2, q);
  if (auto* o = as<tt::Tpat_or>(qd)) return compat_with(equal, p, o->p1) || compat_with(equal, p, o->p2);
  if (pd->kind != qd->kind) return false;
  switch (pd->kind) {
    // Constructors, with special case for extension
    case PK::Tpat_construct: {
      auto* c1 = as<tt::Tpat_construct>(pd);
      auto* c2 = as<tt::Tpat_construct>(qd);
      return equal(c1->cstr, c2->cstr) &&
             compats_with(equal, Pats(c1->args.begin(), c1->args.end()), Pats(c2->args.begin(), c2->args.end()));
    }
    // More standard stuff
    case PK::Tpat_variant: {
      auto* v1 = as<tt::Tpat_variant>(pd);
      auto* v2 = as<tt::Tpat_variant>(qd);
      if (v1->label != v2->label) return false;
      if (!v1->arg && !v2->arg) return true;
      if (v1->arg && v2->arg) return compat_with(equal, v1->arg, v2->arg);
      return false;
    }
    case PK::Tpat_constant: return const_compare(as<tt::Tpat_constant>(pd)->c, as<tt::Tpat_constant>(qd)->c) == 0;
    case PK::Tpat_tuple: {
      auto ps = as<tt::Tpat_tuple>(pd)->pats;
      auto qs = as<tt::Tpat_tuple>(qd)->pats;
      std::size_t k = 0;
      for (; k < ps.size() && k < qs.size(); ++k)
        if (!(ps[k].label == qs[k].label && compat_with(equal, ps[k].pat, qs[k].pat))) return false;
      return ps.size() == qs.size();
    }
    case PK::Tpat_lazy: return compat_with(equal, as<tt::Tpat_lazy>(pd)->pat, as<tt::Tpat_lazy>(qd)->pat);
    case PK::Tpat_record: {
      auto [ps, qs] = records_args(as<tt::Tpat_record>(pd)->fields, as<tt::Tpat_record>(qd)->fields);
      return compats_with(equal, ps, qs);
    }
    case PK::Tpat_array: {
      auto* a1 = as<tt::Tpat_array>(pd);
      auto* a2 = as<tt::Tpat_array>(qd);
      return a1->mut == a2->mut && a1->pats.size() == a2->pats.size() &&
             compats_with(equal, Pats(a1->pats.begin(), a1->pats.end()), Pats(a2->pats.begin(), a2->pats.end()));
    }
    default: return false;
  }
}
// SyntacticCompat
bool compat(const tt::Pattern* p, const tt::Pattern* q) { return compat_with(data_types::equal_constr, p, q); }
bool compats(const Pats& ps, const Pats& qs) { return compats_with(data_types::equal_constr, ps, qs); }

namespace {

// ---- utilities for retrieving type paths --------------------------------------------------
// May need a clean copy, cf. PR#4745
TypeExpr* clean_copy(TypeExpr* ty) {
  if (get_level(ty) == btype::generic_level) return ty;
  return subst::type_expr(subst::identity(), ty);
}
Path::t get_constructor_type_path(TypeExpr* ty, env::t tenv) {
  TypeExpr* t = ctype::expand_head_nolink(tenv, clean_copy(ty));
  auto* tc = as<Tconstr>(get_desc(t));
  if (!tc) throw std::logic_error("get_constructor_type_path");
  return tc->path;
}

// ---- utilities for matching ------------------------------------------------------------------
// Check top matching
bool simple_match(const Head* d, const Head* h) {
  if (h->kind == HK::Any) return true;
  if (d->kind != h->kind) return false;
  switch (d->kind) {
    case HK::Construct: return data_types::equal_constr(d->cstr, h->cstr);
    case HK::Variant: return d->tag == h->tag;
    case HK::Constant: return const_compare(d->c, h->c) == 0;
    case HK::Lazy:
    case HK::Record: return true;
    case HK::Tuple: return d->tuple == h->tuple;
    case HK::Array: return d->am == h->am && d->n == h->n;
    case HK::Any: return false;
  }
  return false;
}

// extract record fields as a whole
std::vector<const LabelDescription*> record_arg(const Head* ph) {
  if (ph->kind == HK::Any) return {};
  if (ph->kind == HK::Record) return ph->lbls;
  throw std::logic_error("Parmatch.as_record");
}

Pats extract_fields(const std::vector<const LabelDescription*>& lbls,
                    const std::vector<std::pair<const LabelDescription*, const tt::Pattern*>>& arg) {
  Pats out;
  for (auto* lbl : lbls) {
    const tt::Pattern* p = omega();
    for (auto& [l, pat] : arg)
      if (l->lbl_pos == lbl->lbl_pos) {
        p = pat;
        break;
      }
    out.push_back(p);
  }
  return out;
}

// Build argument list when p2 >= p1, where p1 is a simple pattern
Pats simple_match_args(const Head* discr, const Head* head, const Pats& args) {
  switch (head->kind) {
    case HK::Constant: return {};
    case HK::Construct:
    case HK::Variant:
    case HK::Tuple:
    case HK::Array:
    case HK::Lazy: return args;
    case HK::Record: {
      if (head->lbls.size() != args.size()) throw std::invalid_argument("List.combine");
      std::vector<std::pair<const LabelDescription*, const tt::Pattern*>> c;
      for (std::size_t k = 0; k < args.size(); ++k) c.push_back({head->lbls[k], args[k]});
      return extract_fields(record_arg(discr), c);
    }
    case HK::Any:
      switch (discr->kind) {
        case HK::Construct: return omegas(discr->cstr->cstr_arity);
        case HK::Variant: return discr->has_arg ? Pats{omega()} : Pats{};
        case HK::Lazy: return {omega()};
        case HK::Record: return omega_list(discr->lbls);
        case HK::Array: return omegas(discr->n);
        case HK::Tuple: return omega_list(discr->tuple);
        case HK::Any:
        case HK::Constant: return {};
      }
  }
  return {};
}

// A simplified matrix row: ((pattern head, arguments), rest of row)
template <class R>
struct SRow {
  Deconstructed hd;
  R rest;
};
template <class R>
using SMatrix = std::vector<SRow<R>>;

template <class R>
std::vector<const Head*> first_column(const SMatrix<R>& m) {
  std::vector<const Head*> c;
  for (auto& r : m) c.push_back(r.hd.head);
  return c;
}

// We build a normalized /discriminating/ pattern from a pattern [q] by
// folding over the first column of the matrix, "refining" [q] as we go.
template <class R>
const Head* discr_pat(const tt::Pattern* q0, const SMatrix<R>& pss) {
  const Head* q = deconstruct(q0).head;
  // short-circuiting: if we have anything other than [Record] or [Any] to
  // start with, we're not going to be able refine at all.
  if (q->kind != HK::Any && q->kind != HK::Record) return q;
  const Head* acc = q;
  for (auto& row : pss) {
    const Head* head = row.hd.head;
    switch (head->kind) {
      case HK::Any: continue;
      case HK::Tuple:
      case HK::Lazy: return head;
      case HK::Record: {
        // List.fold_right (fun lbl r -> if exists .. then r else lbl :: r) lbls (record_arg acc)
        std::vector<const LabelDescription*> fields = record_arg(acc);
        for (std::size_t k = head->lbls.size(); k-- > 0;) {
          const LabelDescription* lbl = head->lbls[k];
          bool present = false;
          for (auto* l : fields)
            if (l->lbl_pos == lbl->lbl_pos) present = true;
          if (!present) fields.insert(fields.begin(), lbl);
        }
        Head* d = make<Head>(*head);
        d->lbls = fields;
        acc = d;
        continue;
      }
      default: return acc;
    }
  }
  return acc;
}

// In case a matching value is found, set actual arguments of the matching
// pattern.
std::pair<Pats, Pats> read_args(std::size_t n, const Pats& r) {
  if (r.size() < n) throw std::logic_error("Parmatch.read_args");
  return {Pats(r.begin(), r.begin() + static_cast<long>(n)), Pats(r.begin() + static_cast<long>(n), r.end())};
}

}  // namespace

Pats set_args(const tt::Pattern* q, const Pats& r) {
  const tt::PatternDesc* d = q->pat_desc;
  auto cons = [](const tt::Pattern* p, const Pats& rest) {
    Pats out{p};
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
  };
  switch (d->kind) {
    case PK::Tpat_tuple: {
      auto lbls_omegas = as<tt::Tpat_tuple>(d)->pats;
      auto [args, rest] = read_args(lbls_omegas.size(), r);
      std::vector<tt::LabeledPattern> l;
      for (std::size_t k = 0; k < args.size(); ++k) l.push_back({lbls_omegas[k].label, args[k]});
      return cons(make_pat(mkd(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(l)}), q->pat_type, q->pat_env), rest);
    }
    case PK::Tpat_record: {
      auto* rc = as<tt::Tpat_record>(d);
      auto [args, rest] = read_args(rc->fields.size(), r);
      std::vector<tt::RecordPatField> l;
      for (std::size_t k = 0; k < args.size(); ++k) l.push_back({rc->fields[k].lid, rc->fields[k].label, args[k]});
      return cons(make_pat(mkd(tt::Tpat_record{{PK::Tpat_record}, slice(l), rc->closed}), q->pat_type, q->pat_env), rest);
    }
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      auto [args, rest] = read_args(c->args.size(), r);
      return cons(make_pat(mkd(tt::Tpat_construct{{PK::Tpat_construct}, c->lid, c->cstr, slice(args), nullptr}),
                           q->pat_type, q->pat_env),
                  rest);
    }
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      const tt::Pattern* arg = nullptr;
      Pats rest = r;
      if (v->arg) {
        if (r.empty()) throw std::logic_error("set_args: variant");
        arg = r[0];
        rest = Pats(r.begin() + 1, r.end());
      }
      return cons(make_pat(mkd(tt::Tpat_variant{{PK::Tpat_variant}, v->label, arg, v->row}), q->pat_type, q->pat_env),
                  rest);
    }
    case PK::Tpat_lazy: {
      if (r.empty()) throw std::logic_error("Parmatch.do_set_args (lazy)");
      return cons(make_pat(mkd(tt::Tpat_lazy{{PK::Tpat_lazy}, r[0]}), q->pat_type, q->pat_env),
                  Pats(r.begin() + 1, r.end()));
    }
    case PK::Tpat_array: {
      auto* a = as<tt::Tpat_array>(d);
      auto [args, rest] = read_args(a->pats.size(), r);
      return cons(make_pat(mkd(tt::Tpat_array{{PK::Tpat_array}, a->mut, slice(args)}), q->pat_type, q->pat_env), rest);
    }
    case PK::Tpat_constant:
    case PK::Tpat_any: return cons(q, r);  // case any is used in matching.ml
    default: throw std::logic_error("Parmatch.set_args");
  }
}

namespace {

// Simplify the first column of a matrix of non-empty rows by splitting all
// or-patterns: a list of ((pattern head, arguments), rest of row).
template <class R, class AddColumn>
void simplify_head_pat(const AddColumn& add_column, const tt::Pattern* p, const R& ps, SMatrix<R>& out) {
  const tt::Pattern* v = strip_vars(p);
  if (auto* o = as<tt::Tpat_or>(v->pat_desc)) {
    simplify_head_pat(add_column, o->p1, ps, out);
    simplify_head_pat(add_column, o->p2, ps, out);
    return;
  }
  // a Simple view: Tpat_construct's existential annotation is dropped
  add_column(deconstruct(v), ps, out);
}

SMatrix<Pats> simplify_first_col(const std::vector<Pats>& rows) {
  SMatrix<Pats> out;
  for (auto& row : rows) {
    if (row.empty()) throw std::logic_error("simplify_first_col: empty row");  // the rows are non-empty!
    Pats ps(row.begin() + 1, row.end());
    simplify_head_pat<Pats>([](const Deconstructed& d, const Pats& rest, SMatrix<Pats>& o) { o.push_back({d, rest}); },
                            row[0], ps, out);
  }
  return out;
}

Pats append(const Pats& a, const Pats& b) {
  Pats r = a;
  r.insert(r.end(), b.begin(), b.end());
  return r;
}

// Builds the specialized matrix of [pss] according to the discriminating
// pattern head [d].
template <class R, class Extend>
std::vector<R> build_specialized_submatrix(const Extend& extend_row, const Head* discr, const SMatrix<R>& pss) {
  std::vector<R> out;
  for (auto& row : pss)
    if (simple_match(discr, row.hd.head)) out.push_back(extend_row(simple_match_args(discr, row.hd.head, row.hd.args), row.rest));
  return out;
}

// The "default" and "specialized" matrices of a given matrix.
template <class R>
struct SpecializedMatrices {
  std::vector<R> default_;
  std::vector<std::pair<const Head*, std::vector<R>>> constrs;
};

template <class R, class Extend>
SpecializedMatrices<R> build_specialized_submatrices(const Extend& extend_row, const Head* discr,
                                                     const SMatrix<R>& rows) {
  // the rows of each group, in the order of the OCaml list after List.rev
  std::vector<std::pair<const Head*, std::vector<R>>> groups;
  auto extend_group = [&](std::pair<const Head*, std::vector<R>>& g, const Head* p, const Pats& args, const R& r) {
    g.second.push_back(extend_row(simple_match_args(g.first, p, args), r));
  };
  // [discr] comes from [discr_pat], and in the Record / Tuple / Lazy case
  // subsumes any of the patterns we could find on the first column.
  if (discr->kind == HK::Record || discr->kind == HK::Tuple || discr->kind == HK::Lazy) groups.push_back({discr, {}});
  std::vector<R> omega_tails;  // source order
  for (auto& row : rows) {
    const Head* head = row.hd.head;
    if (head->kind == HK::Any) {
      // (calling insert_omega here would be wrong as some groups may not
      // have been formed yet)
      omega_tails.push_back(row.rest);
      continue;
    }
    // insert a row of head [p] and rest [r] into the right group (the order
    // of the groups is the order of their first row in the source order)
    bool found = false;
    for (auto& g : groups)
      if (simple_match(g.first, head)) {
        extend_group(g, head, row.hd.args, row.rest);
        found = true;
        break;
      }
    if (!found) {
      groups.push_back({head, {}});
      extend_group(groups.back(), head, row.hd.args, row.rest);
    }
  }
  // insert the rows of head omega into all groups
  for (auto& r : omega_tails)
    for (auto& g : groups) extend_group(g, head_omega(), {}, r);
  return {omega_tails, groups};
}

// ---- variant related functions ---------------------------------------------------------------
// mark constructor lines for failure when they are incomplete
SMatrix<Pats> mark_partial(const SMatrix<Pats>& pss) {
  static const tt::Pattern* zero = [] {
    ZoneScope perm(permanent_zone());
    tt::Constant c{CK::Const_int};
    c.i = 0;
    return make_pat(mkd(tt::Tpat_constant{{PK::Tpat_constant}, c}), ctype::none(), env::empty());
  }();
  SMatrix<Pats> out;
  for (auto& row : pss) {
    if (row.hd.head->kind == HK::Any) {
      out.push_back(row);
      continue;
    }
    // set_last zero
    if (row.rest.empty()) {
      out.push_back({deconstruct(zero), {}});
    } else {
      Pats rest = row.rest;
      rest.back() = zero;
      out.push_back({row.hd, rest});
    }
  }
  return out;
}

void close_variant(env::t env, const RowDesc* row) {
  RowDescRepr rr = row_repr(row);
  const PathArgs* orig_name = rr.name;
  const PathArgs* name = orig_name;
  bool static_ = true;
  for (auto& [tag, f] : rr.fields) {
    RowFieldView v = row_field_repr(f);
    if (v.kind == RowFieldView::Kind::Reither && !v.matched) {
      // fixed=false means that this tag is not explicitly matched
      link_row_field_ext(f, rf_absent());
      name = nullptr;
    } else if (v.kind == RowFieldView::Kind::Reither) {
      static_ = false;
    }
  }
  if (!rr.closed || name != orig_name) {
    TypeExpr* more2 = static_ ? btype::newgenty(tnil()) : btype::newgenvar();
    // this unification cannot fail
    ctype::unify(env, rr.more, btype::newgenty(tvariant(create_row({}, more2, true, rr.fixed, name))));
  }
}

// Check whether the first column of env makes up a complete signature or
// not.  We work on the discriminating pattern heads of each sub-matrix:
// they are not omega/Any.
template <class R>
bool full_match(bool closing, const std::vector<std::pair<const Head*, std::vector<R>>>& env) {
  if (env.empty()) return false;
  const Head* discr = env[0].first;
  switch (discr->kind) {
    case HK::Any: throw std::logic_error("full_match");
    case HK::Construct:
      if (discr->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension) return false;
      return static_cast<long>(env.size()) == discr->cstr->cstr_consts + discr->cstr->cstr_nonconsts;
    case HK::Variant: {
      std::vector<std::string_view> fields;
      for (auto& [d, _] : env) {
        if (d->kind != HK::Variant) throw std::logic_error("full_match: variant");
        fields.push_back(d->tag);
      }
      auto mem = [&](std::string_view t) { return std::find(fields.begin(), fields.end(), t) != fields.end(); };
      const RowDesc* row = discr->type_row();
      if (closing && !btype::has_fixed_explanation(row)) {
        // closing=true, we are considering the variant as closed
        for (auto& [tag, f] : row_fields(row)) {
          RowFieldView v = row_field_repr(f);
          if (v.kind == RowFieldView::Kind::Rabsent || (v.kind == RowFieldView::Kind::Reither && !v.matched)) continue;
          // m=true, do not discard matched tags, rather warn
          if (!mem(tag)) return false;
        }
        return true;
      }
      if (!row_closed(row)) return false;
      for (auto& [tag, f] : row_fields(row))
        if (!(row_field_repr(f).kind == RowFieldView::Kind::Rabsent || mem(tag))) return false;
      return true;
    }
    case HK::Constant:
      if (discr->c.kind == CK::Const_char) return env.size() == 256;
      return false;
    case HK::Array: return false;
    case HK::Tuple:
    case HK::Record:
    case HK::Lazy: return true;
  }
  return false;
}

// Written as a non-fragile matching, PR#7451 originated from a fragile
// matching below.
template <class R>
bool should_extend(Path::t ext, const std::vector<std::pair<const Head*, std::vector<R>>>& env) {
  if (!ext) return false;
  if (env.empty()) throw std::logic_error("should_extend");
  const Head* p = env[0].first;
  switch (p->kind) {
    case HK::Construct:
      if (p->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension) return false;
      return path::same(get_constructor_type_path(p->pat_type, p->pat_env), ext);
    case HK::Any: throw std::logic_error("should_extend: Any");
    default: return false;
  }
}

}  // namespace

// build a pattern from a constructor description
const tt::Pattern* pat_of_constr(const tt::Pattern* ex_pat, const ConstructorDescription* cstr) {
  return with_desc(ex_pat, mkd(tt::Tpat_construct{{PK::Tpat_construct},
                                                  tt::LidLoc{Longident::lident(cstr->cstr_name), location::none()}, cstr,
                                                  slice(omegas(cstr->cstr_arity)), nullptr}));
}

namespace {

const tt::Pattern* orify(const tt::Pattern* x, const tt::Pattern* y) {
  return make_pat(mkd(tt::Tpat_or{{PK::Tpat_or}, x, y, nullptr}), x->pat_type, x->pat_env);
}
const tt::Pattern* orify_many(const Pats& l) {
  if (l.empty()) throw std::logic_error("orify_many");
  const tt::Pattern* r = l.back();
  for (std::size_t k = l.size() - 1; k-- > 0;) r = orify(l[k], r);
  return r;
}

// build an or-pattern from a constructor list
const tt::Pattern* pat_of_constrs(const Head* ex_pat0, const std::vector<const ConstructorDescription*>& cstrs) {
  const tt::Pattern* ex_pat = to_omega_pattern(ex_pat0);
  if (cstrs.empty()) throw Empty{};
  Pats l;
  for (auto* c : cstrs) l.push_back(pat_of_constr(ex_pat, c));
  return orify_many(l);
}

}  // namespace

std::vector<const tt::Pattern*> pats_of_type(env::t env, TypeExpr* ty) {
  ctype::TypedeclExtraction te = ctype::extract_concrete_typedecl(env, ty);
  using TE = ctype::TypedeclExtraction::Kind;
  using TK = TypeKind::Kind;
  switch (te.kind) {
    case TE::Typedecl: {
      TK k = te.decl->type_kind->kind;
      if (k != TK::Type_variant && k != TK::Type_record) return {omega()};
      const env::TypeDescriptions* d = env::find_type_descrs(te.p2, env);
      if (d->kind == TK::Type_variant) {
        bool all_gadt = true;
        for (auto* cd : d->constructors) all_gadt = all_gadt && cd->cstr_generalized;
        // Only explode when all constructors are GADTs
        if (d->constructors.size() <= 1 || all_gadt) {
          const tt::Pattern* any = make_pat(mkd(tt::Tpat_any{{PK::Tpat_any}}), ty, env);
          Pats out;
          for (auto* c : d->constructors) out.push_back(pat_of_constr(any, c));
          return out;
        }
        return {omega()};
      }
      if (d->kind == TK::Type_record) {
        std::vector<tt::RecordPatField> fields;
        for (auto* ld : d->labels)
          fields.push_back({tt::LidLoc{Longident::lident(ld->lbl_name), location::none()}, ld, omega()});
        return {make_pat(mkd(tt::Tpat_record{{PK::Tpat_record}, slice(fields), ClosedFlag::Closed}), ty, env)};
      }
      return {omega()};
    }
    case TE::Has_no_typedecl: {
      if (auto* t = as<Ttuple>(get_desc(ctype::expand_head(env, ty)))) {
        std::vector<tt::LabeledPattern> l;
        for (auto& x : t->elems) l.push_back({x.label, omega()});
        return {make_pat(mkd(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(l)}), ty, env)};
      }
      return {omega()};
    }
    case TE::May_have_typedecl: return {omega()};
  }
  return {omega()};
}

std::vector<const ConstructorDescription*> get_variant_constructors(env::t env, TypeExpr* ty) {
  ctype::TypedeclExtraction te = ctype::extract_concrete_typedecl(env, ty);
  if (te.kind == ctype::TypedeclExtraction::Kind::Typedecl && te.decl->type_kind->kind == TypeKind::Kind::Type_variant) {
    const env::TypeDescriptions* d = env::find_type_descrs(te.p2, env);
    if (d->kind != TypeKind::Kind::Type_variant) throw std::logic_error("Parmatch.get_variant_constructors");
    return std::vector<const ConstructorDescription*>(d->constructors.begin(), d->constructors.end());
  }
  throw std::logic_error("Parmatch.get_variant_constructors");
}

// Sends back a pattern that complements the given constructors used_constrs
std::vector<const ConstructorDescription*> complete_constrs(
    env::t pat_env, const ConstructorDescription* c, const std::vector<const ConstructorDescription*>& used_constrs) {
  std::vector<const ConstructorDescription*> constrs = get_variant_constructors(pat_env, c->cstr_res);
  // ConstructorSet: constructors compared by name
  auto used = [&](const ConstructorDescription* cnstr) {
    for (auto* u : used_constrs)
      if (u->cstr_name == cnstr->cstr_name) return true;
    return false;
  };
  // Split constructors to put constant ones first
  std::vector<const ConstructorDescription*> const_, nonconst;
  for (auto* cnstr : constrs)
    if (!used(cnstr)) (cnstr->cstr_arity == 0 ? const_ : nonconst).push_back(cnstr);
  const_.insert(const_.end(), nonconst.begin(), nonconst.end());
  return const_;
}

namespace {

template <class R>
const tt::Pattern* build_other_constrs(const std::vector<std::pair<const Head*, std::vector<R>>>& env, const Head* p) {
  if (p->kind != HK::Construct || p->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension) return extra_pat();
  std::vector<const ConstructorDescription*> used_constrs;
  for (auto& [q, _] : env) {
    if (q->kind != HK::Construct) throw std::logic_error("Parmatch.get_constr");
    used_constrs.push_back(q->cstr);
  }
  return pat_of_constrs(p, complete_constrs(p->pat_env, p->cstr, used_constrs));
}

// Builds a pattern that is incompatible with all patterns in the first
// column of env
template <class R>
const tt::Pattern* build_other(Path::t ext, const std::vector<std::pair<const Head*, std::vector<R>>>& env) {
  if (env.empty()) return omega();
  const Head* d = env[0].first;
  auto heads = [&] {
    std::vector<const Head*> hs;
    for (auto& [h, _] : env) hs.push_back(h);
    return hs;
  };
  auto constant_pat = [&](const tt::Constant& c) {
    return make_pat(mkd(tt::Tpat_constant{{PK::Tpat_constant}, c}), d->pat_type, d->pat_env);
  };
  switch (d->kind) {
    case HK::Construct: {
      if (d->cstr->cstr_tag.kind == ConstructorTag::Kind::Cstr_extension) {
        // PR#7330
        return make_pat(mkd(tt::Tpat_var{{PK::Tpat_var}, Ident::create_local(OCAML_LIT("*extension*")),
                                         tt::StrLoc{"*extension*", d->pat_loc}, uid::internal_not_actually_unique()}),
                        ctype::none(), env::empty());
      }
      if (ext && path::same(ext, get_constructor_type_path(d->pat_type, d->pat_env))) return extra_pat();
      return build_other_constrs(env, d);
    }
    case HK::Variant: {
      std::vector<std::string_view> tags;
      for (auto* h : heads()) {
        if (h->kind != HK::Variant) throw std::logic_error("build_other: variant");
        tags.push_back(h->tag);
      }
      auto mem = [&](std::string_view t) { return std::find(tags.begin(), tags.end(), t) != tags.end(); };
      auto make_other_pat = [&](std::string_view tag, bool const_) {
        return make_pat(mkd(tt::Tpat_variant{{PK::Tpat_variant}, tag, const_ ? nullptr : omega(), d->cstr_row}),
                        d->pat_type, d->pat_env);
      };
      const RowDesc* row = d->type_row();
      Pats others;  // head first
      for (auto& [tag, f] : row_fields(row)) {
        if (mem(tag)) continue;
        RowFieldView v = row_field_repr(f);
        // (called after erasing pattern info)
        if (v.kind == RowFieldView::Kind::Reither) others.insert(others.begin(), make_other_pat(tag, v.constant));
        else if (v.kind == RowFieldView::Kind::Rpresent)
          others.insert(others.begin(), make_other_pat(tag, v.present == nullptr));
      }
      if (others.empty()) {
        std::string tag;
        if (btype::has_fixed_explanation(row)) {
          tag = some_private_tag;
        } else {
          tag = "AnyOtherTag";
          while (mem(tag)) tag += "'";
        }
        return make_other_pat(zborrow(tag), true);
      }
      const tt::Pattern* p_res = others[0];
      for (std::size_t k = 1; k < others.size(); ++k)
        p_res = make_pat(mkd(tt::Tpat_or{{PK::Tpat_or}, others[k], p_res, nullptr}), d->pat_type, d->pat_env);
      return p_res;
    }
    case HK::Constant: {
      std::vector<const Head*> hs = heads();
      switch (d->c.kind) {
        case CK::Const_char: {
          std::vector<long> all_chars;
          for (auto* h : hs) all_chars.push_back(h->c.i);
          auto mem = [&](long c) { return std::find(all_chars.begin(), all_chars.end(), c) != all_chars.end(); };
          const std::pair<long, long> ranges[] = {{'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {' ', '~'}, {0, 255}};
          for (auto [lo, hi] : ranges)
            for (long i = lo; i <= hi; ++i)
              if (!mem(i)) {
                tt::Constant c{CK::Const_char};
                c.i = i;
                return constant_pat(c);
              }
          return omega();
        }
        case CK::Const_int:
        case CK::Const_int32:
        case CK::Const_int64:
        case CK::Const_nativeint: {
          bool boxed = d->c.kind != CK::Const_int;
          std::vector<std::int64_t> all;
          for (auto* h : hs) all.push_back(boxed ? h->c.boxed : h->c.i);
          std::int64_t i = 0;
          while (std::find(all.begin(), all.end(), i) != all.end()) ++i;
          tt::Constant c{d->c.kind};
          if (boxed) c.boxed = i;
          else c.i = static_cast<long>(i);
          return constant_pat(c);
        }
        case CK::Const_string: {
          std::vector<std::size_t> all;
          for (auto* h : hs) all.push_back(h->c.s.size());
          std::size_t i = 0;
          while (std::find(all.begin(), all.end(), i) != all.end()) ++i;
          tt::Constant c{CK::Const_string};
          c.s = zborrow(std::string(i, '*'));
          c.str_loc = location::none();
          return constant_pat(c);
        }
        case CK::Const_float: {
          std::vector<double> all;
          for (auto* h : hs) all.push_back(float_of_string(h->c.s));
          double f = 0.0;
          auto mem = [&](double x) {
            for (double a : all)
              if (compare_float(a, x) == 0) return true;
            return false;
          };
          while (mem(f)) f += 1.0;
          tt::Constant c{CK::Const_float};
          c.s = zborrow(string_of_float(f));
          return constant_pat(c);
        }
      }
      return omega();
    }
    case HK::Array: {
      std::vector<long> all_lengths;
      for (auto* h : heads()) {
        if (!(h->kind == HK::Array && h->am == d->am)) throw std::logic_error("build_other: array");
        all_lengths.push_back(h->n);
      }
      long l = 0;
      while (std::find(all_lengths.begin(), all_lengths.end(), l) != all_lengths.end()) ++l;
      return make_pat(mkd(tt::Tpat_array{{PK::Tpat_array}, d->am, slice(omegas(l))}), d->pat_type, d->pat_env);
    }
    default: return omega();
  }
}

bool has_instance(const tt::Pattern* p);
bool has_instances(const Pats& ps) {
  for (auto* q : ps)
    if (!has_instance(q)) return false;
  return true;
}
bool has_instance(const tt::Pattern* p) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      if (is_absent(v->label, v->row)) return false;
      return v->arg ? has_instance(v->arg) : true;
    }
    case PK::Tpat_any:
    case PK::Tpat_var:
    case PK::Tpat_constant: return true;
    case PK::Tpat_alias: return has_instance(as<tt::Tpat_alias>(d)->pat);
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      return has_instance(o->p1) || has_instance(o->p2);
    }
    case PK::Tpat_construct: {
      auto a = as<tt::Tpat_construct>(d)->args;
      return has_instances(Pats(a.begin(), a.end()));
    }
    case PK::Tpat_array: {
      auto a = as<tt::Tpat_array>(d)->pats;
      return has_instances(Pats(a.begin(), a.end()));
    }
    case PK::Tpat_tuple: {
      Pats ps;
      for (auto& x : as<tt::Tpat_tuple>(d)->pats) ps.push_back(x.pat);
      return has_instances(ps);
    }
    case PK::Tpat_record: {
      Pats ps;
      for (auto& f : as<tt::Tpat_record>(d)->fields) ps.push_back(f.pat);
      return has_instances(ps);
    }
    case PK::Tpat_lazy: return has_instance(as<tt::Tpat_lazy>(d)->pat);
    default: return true;
  }
}

Pats cons(const tt::Pattern* p, const Pats& ps) {
  Pats r{p};
  r.insert(r.end(), ps.begin(), ps.end());
  return r;
}
Pats tail(const Pats& ps) { return Pats(ps.begin() + 1, ps.end()); }
auto extend_append = [](const Pats& args, const Pats& rest) { return append(args, rest); };

// Core function: is the last row of pattern matrix pss + qs satisfiable?
// That is: does there exist at least one value vector es such that
//  1- for all ps in pss ps # es (ps and es are not compatible)
//  2- qs <= es                  (es matches qs)
bool satisfiable(const std::vector<Pats>& pss, const Pats& qs) {
  if (pss.empty()) return has_instances(qs);
  if (qs.empty()) return false;
  const tt::Pattern* q = strip_vars(qs[0]);
  Pats rest = tail(qs);
  const tt::PatternDesc* d = q->pat_desc;
  if (auto* o = as<tt::Tpat_or>(d)) return satisfiable(pss, cons(o->p1, rest)) || satisfiable(pss, cons(o->p2, rest));
  if (d->kind == PK::Tpat_any) {
    SMatrix<Pats> spss = simplify_first_col(pss);
    if (!all_coherent(first_column(spss))) return false;
    const Head* q0 = discr_pat(omega(), spss);
    SpecializedMatrices<Pats> sm = build_specialized_submatrices<Pats>(extend_append, q0, spss);
    if (!full_match(false, sm.constrs)) return satisfiable(sm.default_, rest);
    for (auto& [p, m] : sm.constrs)
      if (!is_absent_pat(p) && satisfiable(m, append(simple_match_args(p, head_omega(), {}), rest))) return true;
    return false;
  }
  if (auto* v = as<tt::Tpat_variant>(d); v && is_absent(v->label, v->row)) return false;
  SMatrix<Pats> spss = simplify_first_col(pss);
  Deconstructed hq = deconstruct(q);
  std::vector<const Head*> col{hq.head};
  for (auto* h : first_column(spss)) col.push_back(h);
  if (!all_coherent(col)) return false;
  const Head* q0 = discr_pat(q, spss);
  return satisfiable(build_specialized_submatrix<Pats>(extend_append, q0, spss),
                     append(simple_match_args(q0, hq.head, hq.args), rest));
}

// While [satisfiable] only checks whether the last row of [pss + qs] is
// satisfiable, this function returns the (possibly empty) list of vectors
// [es] which verify the same conditions (for GADT handling).
std::vector<Pats> list_satisfying_vectors(const std::vector<Pats>& pss, const Pats& qs) {
  if (pss.empty()) return has_instances(qs) ? std::vector<Pats>{qs} : std::vector<Pats>{};
  if (qs.empty()) return {};
  const tt::Pattern* q = strip_vars(qs[0]);
  Pats rest = tail(qs);
  const tt::PatternDesc* d = q->pat_desc;
  auto concat = [](std::vector<Pats> a, const std::vector<Pats>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  if (auto* o = as<tt::Tpat_or>(d)) {
    // (lsv pss (q1::qs) @ lsv pss (q2::qs)): right to left
    std::vector<Pats> b = list_satisfying_vectors(pss, cons(o->p2, rest));
    std::vector<Pats> a = list_satisfying_vectors(pss, cons(o->p1, rest));
    return concat(a, b);
  }
  if (d->kind == PK::Tpat_any) {
    SMatrix<Pats> spss = simplify_first_col(pss);
    if (!all_coherent(first_column(spss))) return {};
    const Head* q0 = discr_pat(omega(), spss);
    auto wild = [&](const std::vector<Pats>& default_matrix, const tt::Pattern* p) {
      std::vector<Pats> out;
      for (auto& v : list_satisfying_vectors(default_matrix, rest)) out.push_back(cons(p, v));
      return out;
    };
    SpecializedMatrices<Pats> sm = build_specialized_submatrices<Pats>(extend_append, q0, spss);
    // first column of pss is made of variables only
    if (sm.constrs.empty()) return wild(sm.default_, omega());
    auto for_constrs = [&] {
      std::vector<Pats> out;
      for (auto& [p, m] : sm.constrs) {
        if (is_absent_pat(p)) continue;
        std::vector<Pats> witnesses = list_satisfying_vectors(m, append(simple_match_args(p, head_omega(), {}), rest));
        const tt::Pattern* p2 = to_omega_pattern(p);
        for (auto& w : witnesses) out.push_back(set_args(p2, w));
      }
      return out;
    };
    if (full_match(false, sm.constrs)) return for_constrs();
    if (sm.constrs[0].first->kind == HK::Construct) {
      // (wild default (build_other_constrs constrs p) @ for_constrs ()): right to left
      std::vector<Pats> b = for_constrs();
      std::vector<Pats> a = wild(sm.default_, build_other_constrs(sm.constrs, sm.constrs[0].first));
      return concat(a, b);
    }
    return wild(sm.default_, omega());
  }
  if (auto* v = as<tt::Tpat_variant>(d); v && is_absent(v->label, v->row)) return {};
  Deconstructed hq = deconstruct(q);
  SMatrix<Pats> spss = simplify_first_col(pss);
  std::vector<const Head*> col{hq.head};
  for (auto* h : first_column(spss)) col.push_back(h);
  if (!all_coherent(col)) return {};
  const Head* q0 = discr_pat(q, spss);
  std::vector<Pats> vs = list_satisfying_vectors(build_specialized_submatrix<Pats>(extend_append, q0, spss),
                                                 append(simple_match_args(q0, hq.head, hq.args), rest));
  const tt::Pattern* p2 = to_omega_pattern(q0);
  std::vector<Pats> out;
  for (auto& w : vs) out.push_back(set_args(p2, w));
  return out;
}

// ---- Seq (stdlib seq.ml, the part exhaust uses) ------------------------------------------
template <class T>
struct SeqNode;
template <class T>
using Seq = std::function<SeqNode<T>()>;
template <class T>
struct SeqNode {
  bool nil;
  T value{};
  Seq<T> next;
};
template <class T>
Seq<T> seq_empty() {
  return [] { return SeqNode<T>{true}; };
}
template <class T>
Seq<T> seq_return(T x) {
  return [x] { return SeqNode<T>{false, x, seq_empty<T>()}; };
}
template <class T, class F>
Seq<T> seq_map(F f, Seq<T> s) {
  return [f, s]() -> SeqNode<T> {
    SeqNode<T> n = s();
    if (n.nil) return n;
    return {false, f(n.value), seq_map<T>(f, n.next)};
  };
}
template <class T>
Seq<T> seq_append(Seq<T> s1, Seq<T> s2) {
  return [s1, s2]() -> SeqNode<T> {
    SeqNode<T> n = s1();
    if (n.nil) return s2();
    return {false, n.value, seq_append(n.next, s2)};
  };
}
template <class T>
Seq<T> seq_delay(std::function<Seq<T>()> f) {
  return [f] { return f()(); };
}
// Seq.flat_map f (List.to_seq l), from index k
template <class T, class X, class F>
Seq<T> seq_flat_map_list(F f, std::shared_ptr<const std::vector<X>> l, std::size_t k) {
  return [f, l, k]() -> SeqNode<T> {
    if (k == l->size()) return {true};
    return seq_append<T>(f((*l)[k]), seq_flat_map_list<T, X>(f, l, k + 1))();
  };
}

// Another satisfiable function that additionally supplies an example of a
// matching value (for the exhaustiveness check only).
Seq<Pats> exhaust_(Path::t ext, const std::vector<Pats>& pss, long n);
Seq<Pats> specialize_and_exhaust(Path::t ext, const std::vector<Pats>& pss, long n);

// Shortcut: in the single-row case p :: ps all counter-examples are either
// counter-example(p) :: omegas or p :: counter-examples(ps).
Seq<Pats> exhaust_single_row(Path::t ext, const tt::Pattern* p, const Pats& ps, long n) {
  Seq<Pats> sub_witnesses = seq_map<Pats>([p](const Pats& row) { return cons(p, row); }, exhaust_(ext, {ps}, n - 1));
  auto p_witnesses = [ext, p, n]() -> Seq<Pats> {
    Pats omega_row = omegas(n - 1);
    // note: calling [exhaust] recursively of p would result in an infinite
    // loop in the case n=1
    return seq_map<Pats>([omega_row](const Pats& p_row) { return append(p_row, omega_row); },
                         specialize_and_exhaust(ext, {{p}}, 1));
  };
  return seq_append<Pats>(sub_witnesses, seq_delay<Pats>(p_witnesses));
}

Seq<Pats> exhaust_(Path::t ext, const std::vector<Pats>& pss, long n) {
  if (pss.empty()) return seq_return(omegas(n));
  if (pss[0].empty()) return seq_empty<Pats>();
  if (pss.size() == 1) return exhaust_single_row(ext, pss[0][0], tail(pss[0]), n);
  return specialize_and_exhaust(ext, pss, n);
}

Seq<Pats> specialize_and_exhaust(Path::t ext, const std::vector<Pats>& pss0, long n) {
  SMatrix<Pats> pss = simplify_first_col(pss0);
  // We're considering an ill-typed branch, we won't actually be able to
  // produce a well typed value taking that branch.
  if (!all_coherent(first_column(pss))) return seq_empty<Pats>();
  const Head* q0 = discr_pat(omega(), pss);
  auto sm = std::make_shared<SpecializedMatrices<Pats>>(build_specialized_submatrices<Pats>(extend_append, q0, pss));
  if (sm->constrs.empty()) {
    // first column of pss is made of variables only
    Seq<Pats> sub_witnesses = exhaust_(ext, sm->default_, n - 1);
    const tt::Pattern* q = to_omega_pattern(q0);
    return seq_map<Pats>([q](const Pats& row) { return cons(q, row); }, sub_witnesses);
  }
  using Item = std::optional<std::size_t>;  // Some constr_mat (by index) | None
  auto items = std::make_shared<std::vector<Item>>();
  for (std::size_t k = 0; k < sm->constrs.size(); ++k) items->push_back(k);
  items->push_back(std::nullopt);
  // Lazily compute witnesses for all constructor submatrices, then the
  // wildcard/default submatrix; [try_omega] runs after all constructor
  // matrices have been traversed.
  auto f = [sm, ext, n](const Item& it) -> Seq<Pats> {
    if (it) {
      auto& [p, m] = sm->constrs[*it];
      if (is_absent_pat(p)) return seq_empty<Pats>();
      Seq<Pats> sub_witnesses =
          exhaust_(ext, m, static_cast<long>(simple_match_args(p, head_omega(), {}).size()) + n - 1);
      const tt::Pattern* p2 = to_omega_pattern(p);
      return seq_map<Pats>([p2](const Pats& w) { return set_args(p2, w); }, sub_witnesses);
    }
    if (full_match(false, sm->constrs) && !should_extend(ext, sm->constrs)) return seq_empty<Pats>();
    Seq<Pats> sub_witnesses = exhaust_(ext, sm->default_, n - 1);
    const tt::Pattern* p;
    try {
      p = build_other(ext, sm->constrs);
    } catch (const Empty&) {
      // cannot occur, since constructors don't make a full signature
      throw std::logic_error("Parmatch.exhaust");
    }
    return seq_map<Pats>([p](const Pats& t) { return cons(p, t); }, sub_witnesses);
  };
  return seq_flat_map_list<Pats, Item>(f, std::shared_ptr<const std::vector<Item>>(items), 0);
}

// Seq.map (function [x] -> x | _ -> assert false)
Seq<const tt::Pattern*> exhaust_unrow(Seq<Pats> s) {
  return [s]() -> SeqNode<const tt::Pattern*> {
    SeqNode<Pats> nd = s();
    if (nd.nil) return {true};
    if (nd.value.size() != 1) throw std::logic_error("Parmatch.exhaust: row");
    return {false, nd.value[0], exhaust_unrow(nd.next)};
  };
}

Seq<const tt::Pattern*> exhaust(Path::t ext, const std::vector<Pats>& pss, long n) {
  return exhaust_unrow(exhaust_(ext, pss, n));
}

// ---- pressure_variants --------------------------------------------------------------------
// Another exhaustiveness check, enforcing variant typing: whether a matching
// could be made exhaustive by closing all variant types.
bool pressure_variants_(env::t tdefs, const std::vector<Pats>& pss0) {
  if (pss0.empty()) return false;
  if (pss0[0].empty()) return true;
  SMatrix<Pats> pss = simplify_first_col(pss0);
  if (!all_coherent(first_column(pss))) return true;
  const Head* q0 = discr_pat(omega(), pss);
  SpecializedMatrices<Pats> sm = build_specialized_submatrices<Pats>(extend_append, q0, pss);
  if (sm.constrs.empty()) return pressure_variants_(tdefs, sm.default_);
  // [pressure_variants] is called on all the specialized submatrices: we
  // might close some variant in any of them regardless of [ok]
  auto try_non_omega = [&](const std::vector<std::pair<const Head*, std::vector<Pats>>>& cs) {
    bool all = true;
    for (auto& [p, m] : cs) all = pressure_variants_(tdefs, m) && all;
    return all;
  };
  if (full_match(tdefs == nullptr, sm.constrs)) return try_non_omega(sm.constrs);
  if (!tdefs) return pressure_variants_(nullptr, sm.default_);
  bool full = full_match(true, sm.constrs);
  bool ok;
  if (full) {
    ok = try_non_omega(sm.constrs);
  } else {
    SpecializedMatrices<Pats> partial = build_specialized_submatrices<Pats>(extend_append, q0, mark_partial(pss));
    ok = try_non_omega(partial.constrs);
  }
  const Head* d = sm.constrs[0].first;
  if (d->kind == HK::Variant) {
    const RowDesc* row = d->type_row();
    if (!(btype::has_fixed_explanation(row) || pressure_variants_(nullptr, sm.default_))) close_variant(tdefs, row);
  }
  return ok;
}

// ---- usefulness --------------------------------------------------------------------------
enum class AnswerKind { Used, Unused, Upartial };
struct Answer {
  AnswerKind kind;
  Pats partial;  // Upartial: the useless ones
};
// left: elements not to be processed; right: elements to be processed
struct URow {
  Pats no_ors;  // lists, head first
  Pats ors;
  Pats active;
};
URow make_row(const Pats& ps) { return {{}, {}, ps}; }

// Useful to detect and expand or pats inside as pats
bool is_var(const tt::Pattern* p) { return strip_vars(p)->pat_desc->kind == PK::Tpat_any; }
bool is_var_column(const std::vector<URow>& rs) {
  for (auto& r : rs) {
    if (r.active.empty()) throw std::logic_error("is_var_column");
    if (!is_var(r.active[0])) return false;
  }
  return true;
}
// Standard or-args for left-to-right matching
std::pair<const tt::Pattern*, const tt::Pattern*> or_args(const tt::Pattern* p) {
  if (auto* o = as<tt::Tpat_or>(p->pat_desc)) return {o->p1, o->p2};
  if (auto* a = as<tt::Tpat_alias>(p->pat_desc)) return or_args(a->pat);
  throw std::logic_error("or_args");
}
// Just remove current column
URow remove(URow r) {
  if (r.active.empty()) throw std::logic_error("remove");
  r.active = tail(r.active);
  return r;
}
// Current column has been processed
URow push_no_or(URow r) {
  if (r.active.empty()) throw std::logic_error("push_no_or");
  r.no_ors = cons(r.active[0], r.no_ors);
  r.active = tail(r.active);
  return r;
}
URow push_or(URow r) {
  if (r.active.empty()) throw std::logic_error("push_or");
  r.ors = cons(r.active[0], r.ors);
  r.active = tail(r.active);
  return r;
}
template <class F>
std::vector<URow> map_rows(const std::vector<URow>& rs, F f) {
  std::vector<URow> out;
  for (auto& r : rs) out.push_back(f(r));
  return out;
}

SMatrix<URow> simplify_first_usefulness_col(const std::vector<URow>& rows) {
  SMatrix<URow> out;
  for (auto& row : rows) {
    if (row.active.empty()) throw std::logic_error("simplify_first_usefulness_col");  // the rows are non-empty!
    URow r2 = row;
    r2.active = tail(row.active);
    simplify_head_pat<URow>([](const Deconstructed& d, const URow& rest, SMatrix<URow>& o) { o.push_back({d, rest}); },
                            row.active[0], r2, out);
  }
  return out;
}

// Back to normal matrices
Pats make_vector(const URow& r) { return Pats(r.no_ors.rbegin(), r.no_ors.rend()); }
std::vector<Pats> make_matrix(const std::vector<URow>& rs) {
  std::vector<Pats> out;
  for (auto& r : rs) out.push_back(make_vector(r));
  return out;
}

// Standard union on answers
Answer union_res(const Answer& r1, const Answer& r2) {
  if (r1.kind == AnswerKind::Unused || r2.kind == AnswerKind::Unused) return {AnswerKind::Unused, {}};
  if (r1.kind == AnswerKind::Used) return r2;
  if (r2.kind == AnswerKind::Used) return r1;
  return {AnswerKind::Upartial, append(r1.partial, r2.partial)};
}

// propose or pats for expansion
std::vector<URow> extract_elements(const URow& qs) {
  std::vector<URow> out;
  for (std::size_t k = 0; k < qs.ors.size(); ++k) {
    // List.rev_append seen rem @ qs.no_ors: the other ors, in order
    Pats no_ors;
    for (std::size_t j = 0; j < qs.ors.size(); ++j)
      if (j != k) no_ors.push_back(qs.ors[j]);
    no_ors.insert(no_ors.end(), qs.no_ors.begin(), qs.no_ors.end());
    out.push_back({no_ors, {}, {qs.ors[k]}});
  }
  return out;
}

// idem for matrices: column k holds the rows' elements k, last row first
std::vector<std::vector<URow>> transpose(const std::vector<std::vector<URow>>& rs) {
  if (rs.empty()) throw std::logic_error("transpose");
  std::vector<std::vector<URow>> cols;
  for (auto& x : rs[0]) cols.push_back({x});
  for (std::size_t i = 1; i < rs.size(); ++i) {
    if (rs[i].size() != cols.size()) throw std::invalid_argument("List.map2");
    for (std::size_t k = 0; k < cols.size(); ++k) cols[k].insert(cols[k].begin(), rs[i][k]);
  }
  return cols;
}
std::vector<std::vector<URow>> extract_columns(const std::vector<URow>& pss, const URow& qs) {
  if (pss.empty()) return std::vector<std::vector<URow>>(qs.ors.size());
  std::vector<std::vector<URow>> rows;
  for (auto& r : pss) rows.push_back(extract_elements(r));
  return transpose(rows);
}

Answer every_both(const std::vector<URow>& pss, const URow& qs, const tt::Pattern* q1, const tt::Pattern* q2);

// Core function: first look for or patterns (recursive case), then check
// or-patterns argument usefulness (terminal case)
Answer every_satisfiables(const std::vector<URow>& pss, const URow& qs) {
  if (qs.active.empty()) {
    // qs is now partitioned, check usefulness
    if (qs.ors.empty())  // no or-patterns
      return satisfiable(make_matrix(pss), make_vector(qs)) ? Answer{AnswerKind::Used, {}}
                                                            : Answer{AnswerKind::Unused, {}};
    // n or-patterns -> 2n expansions: List.fold_right2 (from the last)
    std::vector<URow> elems = extract_elements(qs);
    std::vector<std::vector<URow>> cols = extract_columns(pss, qs);
    if (cols.size() != elems.size()) throw std::invalid_argument("List.fold_right2");
    Answer r{AnswerKind::Used, {}};
    for (std::size_t k = elems.size(); k-- > 0;) {
      if (r.kind == AnswerKind::Unused) continue;
      if (elems[k].active.size() != 1) throw std::logic_error("every_satisfiables");
      auto [q1, q2] = or_args(elems[k].active[0]);
      Answer r_loc = every_both(cols[k], elems[k], q1, q2);
      r = union_res(r, r_loc);
    }
    return r;
  }
  const tt::Pattern* q = strip_vars(qs.active[0]);
  Pats rem = tail(qs.active);
  const tt::PatternDesc* d = q->pat_desc;
  if (d->kind == PK::Tpat_any) {
    if (is_var_column(pss))
      // forget about ``all-variable'' columns now
      return every_satisfiables(map_rows(pss, remove), remove(qs));
    // otherwise this is direct food for satisfiable
    return every_satisfiables(map_rows(pss, push_no_or), push_no_or(qs));
  }
  if (auto* o = as<tt::Tpat_or>(d)) {
    if (o->p1->pat_loc.loc_ghost && o->p2->pat_loc.loc_ghost)
      // syntactically generated or-pats should not be expanded
      return every_satisfiables(map_rows(pss, push_no_or), push_no_or(qs));
    // this is a real or-pattern
    return every_satisfiables(map_rows(pss, push_or), push_or(qs));
  }
  if (auto* v = as<tt::Tpat_variant>(d); v && is_absent(v->label, v->row)) return {AnswerKind::Unused, {}};
  // standard case, filter matrix
  SMatrix<URow> spss = simplify_first_usefulness_col(pss);
  Deconstructed hq = deconstruct(q);
  // The handling of incoherent matrices is kept in line with [satisfiable]
  std::vector<const Head*> col{hq.head};
  for (auto* h : first_column(spss)) col.push_back(h);
  if (!all_coherent(col)) return {AnswerKind::Unused, {}};
  const Head* q0 = discr_pat(q, spss);
  URow qs2 = qs;
  qs2.active = append(simple_match_args(q0, hq.head, hq.args), rem);
  return every_satisfiables(
      build_specialized_submatrix<URow>(
          [](const Pats& ps, const URow& r) {
            URow r2 = r;
            r2.active = append(ps, r.active);
            return r2;
          },
          q0, spss),
      qs2);
}

// The usefulness check of or-pat q1|q2: call every_satisfiables twice with
// the current active columns restricted to q1 and q2.
Answer every_both(const std::vector<URow>& pss, const URow& qs, const tt::Pattern* q1, const tt::Pattern* q2) {
  URow qs1 = qs;
  qs1.active = {q1};
  URow qs2 = qs;
  qs2.active = {q2};
  Answer r1 = every_satisfiables(pss, qs1);
  std::vector<URow> pss2 = pss;
  if (compat(q1, q2)) pss2.insert(pss2.begin(), qs1);
  Answer r2 = every_satisfiables(pss2, qs2);
  switch (r1.kind) {
    case AnswerKind::Unused:
      if (r2.kind == AnswerKind::Unused) return {AnswerKind::Unused, {}};
      if (r2.kind == AnswerKind::Used) return {AnswerKind::Upartial, {q1}};
      return {AnswerKind::Upartial, cons(q1, r2.partial)};
    case AnswerKind::Used:
      if (r2.kind == AnswerKind::Unused) return {AnswerKind::Upartial, {q2}};
      return r2;
    case AnswerKind::Upartial:
      if (r2.kind == AnswerKind::Unused) return {AnswerKind::Upartial, append(r1.partial, {q2})};
      if (r2.kind == AnswerKind::Used) return r1;
      return {AnswerKind::Upartial, append(r1.partial, r2.partial)};
  }
  return r1;
}

}  // namespace

// le_pat p q  means, forall V,  V matches q implies V matches p
bool le_pat(const tt::Pattern* p, const tt::Pattern* q) {
  const tt::PatternDesc* pd = p->pat_desc;
  const tt::PatternDesc* qd = q->pat_desc;
  if (pd->kind == PK::Tpat_var || pd->kind == PK::Tpat_any) return true;
  if (auto* a = as<tt::Tpat_alias>(pd)) return le_pat(a->pat, q);
  if (auto* a = as<tt::Tpat_alias>(qd)) return le_pat(p, a->pat);
  if (pd->kind == qd->kind) {
    switch (pd->kind) {
      case PK::Tpat_constant:
        return const_compare(as<tt::Tpat_constant>(pd)->c, as<tt::Tpat_constant>(qd)->c) == 0;
      case PK::Tpat_construct: {
        auto* c1 = as<tt::Tpat_construct>(pd);
        auto* c2 = as<tt::Tpat_construct>(qd);
        return data_types::equal_constr(c1->cstr, c2->cstr) &&
               le_pats(Pats(c1->args.begin(), c1->args.end()), Pats(c2->args.begin(), c2->args.end()));
      }
      case PK::Tpat_variant: {
        auto* v1 = as<tt::Tpat_variant>(pd);
        auto* v2 = as<tt::Tpat_variant>(qd);
        if (v1->arg && v2->arg) return v1->label == v2->label && le_pat(v1->arg, v2->arg);
        if (!v1->arg && !v2->arg) return v1->label == v2->label;
        return false;
      }
      case PK::Tpat_tuple: {
        auto ps = as<tt::Tpat_tuple>(pd)->pats;
        auto qs = as<tt::Tpat_tuple>(qd)->pats;
        for (std::size_t k = 0; k < ps.size() && k < qs.size(); ++k)
          if (!(ps[k].label == qs[k].label && le_pat(ps[k].pat, qs[k].pat))) return false;
        return true;
      }
      case PK::Tpat_lazy: return le_pat(as<tt::Tpat_lazy>(pd)->pat, as<tt::Tpat_lazy>(qd)->pat);
      case PK::Tpat_record: {
        auto [ps, qs] = records_args(as<tt::Tpat_record>(pd)->fields, as<tt::Tpat_record>(qd)->fields);
        return le_pats(ps, qs);
      }
      case PK::Tpat_array: {
        auto* a1 = as<tt::Tpat_array>(pd);
        auto* a2 = as<tt::Tpat_array>(qd);
        return a1->mut == a2->mut && a1->pats.size() == a2->pats.size() &&
               le_pats(Pats(a1->pats.begin(), a1->pats.end()), Pats(a2->pats.begin(), a2->pats.end()));
      }
      default: break;
    }
  }
  // In all other cases, enumeration is performed
  return !satisfiable({{p}}, {q});
}

bool le_pats(const Pats& ps, const Pats& qs) {
  for (std::size_t k = 0; k < ps.size() && k < qs.size(); ++k)
    if (!le_pat(ps[k], qs[k])) return false;
  return true;
}

namespace {
// [select_rec] removes the elements that are followed by a smaller element;
// two passes, the first returning the list reversed.
template <class T, class Le>
std::vector<T> get_mins(const Le& le, const std::vector<T>& ps) {
  auto select_rec = [&](const std::vector<T>& l) {
    std::vector<T> r;  // head first
    for (std::size_t k = 0; k < l.size(); ++k) {
      bool smaller_after = false;
      for (std::size_t j = k + 1; j < l.size(); ++j)
        if (le(l[j], l[k])) {
          smaller_after = true;
          break;
        }
      if (!smaller_after) r.insert(r.begin(), l[k]);
    }
    return r;
  };
  return select_rec(select_rec(ps));
}
}  // namespace

// lub p q is a pattern that matches all values matched by p and q; may
// raise Empty, when p and q are not compatible
const tt::Pattern* lub(const tt::Pattern* p, const tt::Pattern* q);
Pats lubs(const Pats& ps, const Pats& qs) {
  Pats out;
  for (std::size_t k = 0; k < ps.size() && k < qs.size(); ++k) out.push_back(lub(ps[k], qs[k]));
  return out;
}
static const tt::Pattern* orlub(const tt::Pattern* p1, const tt::Pattern* p2, const tt::Pattern* q) {
  const tt::Pattern* r1;
  try {
    r1 = lub(p1, q);
  } catch (const Empty&) {
    return lub(p2, q);
  }
  try {
    const tt::Pattern* r2 = lub(p2, q);
    return with_desc(q, mkd(tt::Tpat_or{{PK::Tpat_or}, r1, r2, nullptr}));
  } catch (const Empty&) {
    return r1;
  }
}
const tt::Pattern* lub(const tt::Pattern* p, const tt::Pattern* q) {
  const tt::PatternDesc* pd = p->pat_desc;
  const tt::PatternDesc* qd = q->pat_desc;
  if (auto* a = as<tt::Tpat_alias>(pd)) return lub(a->pat, q);
  if (auto* a = as<tt::Tpat_alias>(qd)) return lub(p, a->pat);
  if (pd->kind == PK::Tpat_any || pd->kind == PK::Tpat_var) return q;
  if (qd->kind == PK::Tpat_any || qd->kind == PK::Tpat_var) return p;
  if (auto* o = as<tt::Tpat_or>(pd)) return orlub(o->p1, o->p2, q);
  if (auto* o = as<tt::Tpat_or>(qd)) return orlub(o->p1, o->p2, p);  // Thanks god, lub is commutative
  if (pd->kind == qd->kind) {
    switch (pd->kind) {
      case PK::Tpat_constant:
        if (const_compare(as<tt::Tpat_constant>(pd)->c, as<tt::Tpat_constant>(qd)->c) == 0) return p;
        break;
      case PK::Tpat_tuple: {
        auto ps = as<tt::Tpat_tuple>(pd)->pats;
        auto qs = as<tt::Tpat_tuple>(qd)->pats;
        std::vector<tt::LabeledPattern> rs;
        std::size_t k = 0;
        for (; k < ps.size() && k < qs.size(); ++k) {
          if (!(ps[k].label == qs[k].label)) throw Empty{};
          rs.push_back({ps[k].label, lub(ps[k].pat, qs[k].pat)});
        }
        if (ps.size() != qs.size()) throw Empty{};
        return make_pat(mkd(tt::Tpat_tuple{{PK::Tpat_tuple}, slice(rs)}), p->pat_type, p->pat_env);
      }
      case PK::Tpat_lazy: {
        const tt::Pattern* ip = as<tt::Tpat_lazy>(pd)->pat;
        const tt::Pattern* r = lub(ip, as<tt::Tpat_lazy>(qd)->pat);
        // (the inner p shadows the outer one in `make_pat .. p.pat_type`)
        return make_pat(mkd(tt::Tpat_lazy{{PK::Tpat_lazy}, r}), ip->pat_type, ip->pat_env);
      }
      case PK::Tpat_construct: {
        auto* c1 = as<tt::Tpat_construct>(pd);
        auto* c2 = as<tt::Tpat_construct>(qd);
        if (data_types::equal_constr(c1->cstr, c2->cstr)) {
          Pats rs = lubs(Pats(c1->args.begin(), c1->args.end()), Pats(c2->args.begin(), c2->args.end()));
          return make_pat(mkd(tt::Tpat_construct{{PK::Tpat_construct}, c1->lid, c1->cstr, slice(rs), nullptr}),
                          p->pat_type, p->pat_env);
        }
        break;
      }
      case PK::Tpat_variant: {
        auto* v1 = as<tt::Tpat_variant>(pd);
        auto* v2 = as<tt::Tpat_variant>(qd);
        if (v1->arg && v2->arg && v1->label == v2->label) {
          const tt::Pattern* r = lub(v1->arg, v2->arg);
          return make_pat(mkd(tt::Tpat_variant{{PK::Tpat_variant}, v1->label, r, v1->row}), p->pat_type, p->pat_env);
        }
        if (!v1->arg && !v2->arg && v1->label == v2->label) return p;
        break;
      }
      case PK::Tpat_record: {
        auto* r1 = as<tt::Tpat_record>(pd);
        auto l1 = r1->fields;
        auto l2 = as<tt::Tpat_record>(qd)->fields;
        std::vector<tt::RecordPatField> rs;
        std::size_t i = 0, j = 0;
        while (i < l1.size() || j < l2.size()) {
          if (i == l1.size()) rs.push_back(l2[j++]);
          else if (j == l2.size()) rs.push_back(l1[i++]);
          else if (l1[i].label->lbl_pos < l2[j].label->lbl_pos) rs.push_back(l1[i++]);
          else if (l2[j].label->lbl_pos < l1[i].label->lbl_pos) rs.push_back(l2[j++]);
          else {
            rs.push_back({l1[i].lid, l1[i].label, lub(l1[i].pat, l2[j].pat)});
            ++i;
            ++j;
          }
        }
        return make_pat(mkd(tt::Tpat_record{{PK::Tpat_record}, slice(rs), r1->closed}), p->pat_type, p->pat_env);
      }
      case PK::Tpat_array: {
        auto* a1 = as<tt::Tpat_array>(pd);
        auto* a2 = as<tt::Tpat_array>(qd);
        if (a1->mut == a2->mut && a1->pats.size() == a2->pats.size()) {
          Pats rs = lubs(Pats(a1->pats.begin(), a1->pats.end()), Pats(a2->pats.begin(), a2->pats.end()));
          return make_pat(mkd(tt::Tpat_array{{PK::Tpat_array}, a1->mut, slice(rs)}), p->pat_type, p->pat_env);
        }
        break;
      }
      default: break;
    }
  }
  throw Empty{};
}

// ---- exported variant closing ------------------------------------------------------------
void pressure_variants(env::t env, const std::vector<const tt::Pattern*>& pats) {
  std::vector<Pats> pss;
  for (auto* p : pats) pss.push_back({p, omega()});
  pressure_variants_(env, pss);
}

void pressure_variants_in_computation_pattern(env::t env, const std::vector<const tt::Pattern*>& pats) {
  std::vector<const tt::Pattern*> val_pss, exn_pss;
  // List.fold_right: the rows keep their order
  for (std::size_t k = pats.size(); k-- > 0;) {
    auto [vp, ep] = tt::split_pattern(pats[k]);
    if (vp) val_pss.insert(val_pss.begin(), vp);
    if (ep) exn_pss.insert(exn_pss.begin(), ep);
  }
  pressure_variants(env, val_pss);
  pressure_variants(env, exn_pss);
}

// ---- exhaustiveness check ----------------------------------------------------------------
namespace {
// Build up a working pattern matrix by forgetting about guarded patterns
std::vector<Pats> initial_matrix(const std::vector<TypedCase>& casel) {
  std::vector<Pats> out;
  for (auto& c : casel)
    if (!c.has_guard) out.push_back({c.pattern});
  return out;
}

// Useful for seeing if the example of non-matched value can indeed be
// matched (by a guarded clause)
bool do_match(const std::vector<Pats>& pss, const Pats& qs, std::size_t k = 0) {
  if (k == qs.size()) return !pss.empty() && pss[0].empty();
  const tt::Pattern* q = strip_vars(qs[k]);
  if (auto* o = as<tt::Tpat_or>(q->pat_desc)) {
    Pats q1(qs.begin() + static_cast<long>(k), qs.end()), q2 = q1;
    q1[0] = o->p1;
    q2[0] = o->p2;
    return do_match(pss, q1) || do_match(pss, q2);
  }
  if (q->pat_desc->kind == PK::Tpat_any) {
    // remove_first_column: stops at the first empty row
    std::vector<Pats> rem;
    for (auto& ps : pss) {
      if (ps.empty()) break;
      rem.push_back(Pats(ps.begin() + 1, ps.end()));
    }
    return do_match(rem, qs, k + 1);
  }
  Deconstructed d = deconstruct(q);
  SMatrix<Pats> sm = simplify_first_col(pss);
  std::vector<Pats> sub = build_specialized_submatrix<Pats>([](const Pats& a, const Pats& b) { return append(a, b); },
                                                            d.head, sm);
  Pats rest = d.args;
  rest.insert(rest.end(), qs.begin() + static_cast<long>(k) + 1, qs.end());
  return do_match(sub, rest);
}

// initial_only_guarded: only the patterns which are guarded
std::vector<Pats> initial_only_guarded(const std::vector<TypedCase>& casel) {
  std::vector<Pats> out;
  for (auto& c : casel)
    if (c.has_guard) out.push_back({c.pattern});
  return out;
}

// Whether the counter-example contains an extension pattern
bool contains_extension(const tt::Pattern* pat) {
  return tt::exists_pattern(
      [](const tt::Pattern* p) {
        auto* v = as<tt::Tpat_var>(p->pat_desc);
        return v && v->name.txt == "*extension*";
      },
      pat);
}

tt::Partial do_check_partial(const std::function<const tt::Pattern*(const tt::Pattern*)>& pred, const Location& loc,
                             const std::vector<TypedCase>& casel, const std::vector<Pats>& pss) {
  if (pss.empty()) {
    // This can occur for empty matches generated by ocamlp4 (no warning), or
    // when all patterns have guards (then, casel <> []) (specific warning).
    // Then match MUST be considered non-exhaustive, otherwise compilation of
    // PM is broken.
    if (!casel.empty() && warnings::is_active(8))
      location::prerr_warning(loc, warnings::Warning::make(warnings::Warning::K::All_clauses_guarded));
    return tt::Partial::Partial;
  }
  Seq<const tt::Pattern*> s = exhaust(nullptr, pss, static_cast<long>(pss[0].size()));
  // Seq.filter_map pred, forced up to its first element
  for (;;) {
    SeqNode<const tt::Pattern*> n = s();
    if (n.nil) return tt::Partial::Total;
    if (const tt::Pattern* v = pred(n.value)) {
      if (warnings::is_active(8)) {
        format_doc::Formatter fmt;
        format_doc::fprintf(fmt, "@[<v>%a", [&](format_doc::Formatter& f) {
          misc::style::as_inline_code([](format_doc::Formatter& g, const tt::Pattern* x) { printpat::top_pretty(g, x); },
                                      f, v);
        });
        if (do_match(initial_only_guarded(casel), {v}))
          format_doc::fprintf(fmt, "@,(However, some guarded clause may match this value.)");
        if (contains_extension(v))
          format_doc::fprintf(fmt,
                              "@,@[Matching over values of extensible variant types (the *extension* above)@,must "
                              "include a wild card pattern@ in order to be exhaustive.@]");
        format_doc::fprintf(fmt, "@]");
        warnings::Warning w = warnings::Warning::make(warnings::Warning::K::Partial_match);
        w.doc = std::move(fmt.doc);
        location::prerr_warning(loc, w);
      }
      return tt::Partial::Partial;
    }
    s = n.next;
  }
}
}  // namespace

// Exported unused clause check
void check_unused(const std::function<const tt::Pattern*(bool refute, const tt::Pattern*)>& pred,
                  const std::vector<TypedCase>& casel) {
  bool any_refute = false;
  for (auto& vc : casel) any_refute = any_refute || vc.needs_refute;
  if (!(warnings::is_active(warnings::Redundant_case) || any_refute)) return;
  std::vector<Pats> pref;  // accumulated in reverse order (head first)
  for (auto& c : casel) {
    const tt::Pattern* q = c.pattern;
    bool refute = c.needs_refute;
    Pats qs{q};
    // prev was accumulated in reverse order; restore source order to get
    // ordered counter-examples
    std::vector<Pats> pss0;
    for (auto it = pref.rbegin(); it != pref.rend(); ++it)
      if (compats(qs, *it)) pss0.push_back(*it);
    std::vector<Pats> pss = get_mins<Pats>([](const Pats& a, const Pats& b) { return le_pats(a, b); }, pss0);
    try {
      // First look for redundant or partially redundant patterns
      std::vector<URow> rows;
      for (auto& ps : pss) rows.push_back(make_row(ps));
      Answer r = every_satisfiables(rows, make_row(qs));
      // Do not warn for unused [pat -> .]
      if (!(r.kind == AnswerKind::Unused && refute)) {
        bool skip = r.kind == AnswerKind::Unused || (!refute && pref.empty()) ||
                    !(refute || warnings::is_active(warnings::Unreachable_case));
        if (!skip) {
          // Then look for empty patterns
          std::vector<Pats> sfs = list_satisfying_vectors(pss, qs);
          if (!sfs.empty()) {
            Pats us;
            for (auto& v : sfs) {
              if (v.size() != 1) throw std::logic_error("check_unused");
              us.push_back(v[0]);
            }
            const tt::Pattern* u = orify_many(us);
            tt::Pattern* pattern = make<tt::Pattern>(*u);
            pattern->pat_loc = q->pat_loc;
            if (!pred(refute, pattern) && !refute) {
              location::prerr_warning(q->pat_loc, warnings::Warning::make(warnings::Warning::K::Unreachable_case));
              r = Answer{AnswerKind::Used, {}};
            }
          } else {
            r = Answer{AnswerKind::Unused, {}};
          }
        }
        if (r.kind == AnswerKind::Unused) {
          location::prerr_warning(q->pat_loc, warnings::Warning::make(warnings::Warning::K::Redundant_case));
        } else if (r.kind == AnswerKind::Upartial) {
          for (const tt::Pattern* p : r.partial)
            location::prerr_warning(p->pat_loc, warnings::Warning::make(warnings::Warning::K::Redundant_subpat));
        }
      }
    } catch (const Empty&) {
      throw std::logic_error("check_unused: Empty");
    } catch (const env::NotFound&) {
      throw std::logic_error("check_unused: Not_found");
    }
    if (!c.has_guard) pref.insert(pref.begin(), Pats{q});
  }
}

// ---- exported irrefutability tests -------------------------------------------------------
bool irrefutable(const tt::Pattern* pat) { return le_pat(pat, omega()); }

bool inactive(tt::Partial partial, const tt::Pattern* pat) {
  if (partial == tt::Partial::Partial) return false;
  std::function<bool(const tt::Pattern*)> loop = [&](const tt::Pattern* p) -> bool {
    const tt::PatternDesc* d = p->pat_desc;
    switch (d->kind) {
      case PK::Tpat_lazy: return false;
      case PK::Tpat_array: {
        auto* a = as<tt::Tpat_array>(d);
        if (a->mut == MutableFlag::Mutable) return false;
        for (auto* x : a->pats)
          if (!loop(x)) return false;
        return true;
      }
      case PK::Tpat_any:
      case PK::Tpat_var:
      case PK::Tpat_constant: return true;
      case PK::Tpat_variant: {
        auto* v = as<tt::Tpat_variant>(d);
        return v->arg ? loop(v->arg) : true;
      }
      case PK::Tpat_tuple:
        for (auto& x : as<tt::Tpat_tuple>(d)->pats)
          if (!loop(x.pat)) return false;
        return true;
      case PK::Tpat_construct:
        for (auto* x : as<tt::Tpat_construct>(d)->args)
          if (!loop(x)) return false;
        return true;
      case PK::Tpat_alias: return loop(as<tt::Tpat_alias>(d)->pat);
      case PK::Tpat_record:
        for (auto& f : as<tt::Tpat_record>(d)->fields)
          if (!(f.label->lbl_mut == MutableFlag::Immutable && loop(f.pat))) return false;
        return true;
      case PK::Tpat_or: {
        auto* o = as<tt::Tpat_or>(d);
        return loop(o->p1) && loop(o->p2);
      }
      default: return true;
    }
  };
  return loop(pat);
}

// ---- exported exhaustiveness check -------------------------------------------------------
// (the fragile check runs for warning 4 only, which is off by default)
// ---- the fragile check (warning 4) ----
// Collect all data types in a pattern
static std::vector<Path::t> add_path(Path::t path, std::vector<Path::t> paths) {
  for (Path::t x : paths)
    if (path::same(path, x)) return paths;
  paths.push_back(path);
  return paths;
}
static bool extendable_path(Path::t path) {
  const predef::Paths& p = predef::paths();
  return !(path::same(path, p.bool_) || path::same(path, p.list) || path::same(path, p.unit) ||
           path::same(path, p.option));
}
static std::vector<Path::t> collect_paths_from_pat(std::vector<Path::t> r, const tt::Pattern* p) {
  const tt::PatternDesc* d = p->pat_desc;
  switch (d->kind) {
    case PK::Tpat_construct: {
      auto* c = as<tt::Tpat_construct>(d);
      if (c->cstr->cstr_tag.kind != ConstructorTag::Kind::Cstr_extension) {
        Path::t path = get_constructor_type_path(p->pat_type, p->pat_env);
        if (extendable_path(path)) r = add_path(path, std::move(r));
      }
      for (const tt::Pattern* q : c->args) r = collect_paths_from_pat(std::move(r), q);
      return r;
    }
    case PK::Tpat_any: case PK::Tpat_var: case PK::Tpat_constant: return r;
    case PK::Tpat_variant: {
      auto* v = as<tt::Tpat_variant>(d);
      return v->arg ? collect_paths_from_pat(std::move(r), v->arg) : r;
    }
    case PK::Tpat_tuple:
      for (const tt::LabeledPattern& lp : as<tt::Tpat_tuple>(d)->pats) r = collect_paths_from_pat(std::move(r), lp.pat);
      return r;
    case PK::Tpat_array:
      for (const tt::Pattern* q : as<tt::Tpat_array>(d)->pats) r = collect_paths_from_pat(std::move(r), q);
      return r;
    case PK::Tpat_record:
      for (const tt::RecordPatField& f : as<tt::Tpat_record>(d)->fields) r = collect_paths_from_pat(std::move(r), f.pat);
      return r;
    case PK::Tpat_alias: return collect_paths_from_pat(std::move(r), as<tt::Tpat_alias>(d)->pat);
    case PK::Tpat_or: {
      auto* o = as<tt::Tpat_or>(d);
      return collect_paths_from_pat(collect_paths_from_pat(std::move(r), o->p1), o->p2);
    }
    case PK::Tpat_lazy: return collect_paths_from_pat(std::move(r), as<tt::Tpat_lazy>(d)->pat);
    default: throw std::logic_error("Parmatch.collect_paths_from_pat");
  }
}
// Actual fragile check: one exhaustivity check per collected datatype,
// considering that the type is extended.
static void do_check_fragile(const Location& loc, const std::vector<TypedCase>& casel, const std::vector<Pats>& pss) {
  std::vector<Path::t> exts;
  for (const TypedCase& c : casel) exts = collect_paths_from_pat(std::move(exts), c.pattern);
  if (exts.empty() || pss.empty()) return;
  for (Path::t ext : exts) {
    Seq<const tt::Pattern*> witnesses = exhaust(ext, pss, static_cast<long>(pss[0].size()));
    if (witnesses().nil)
      location::prerr_warning(loc, warnings::Warning::with_s(warnings::Warning::K::Fragile_match, path::name(ext)));
  }
}

tt::Partial check_partial(const std::function<const tt::Pattern*(const tt::Pattern*)>& pred, const Location& loc,
                          const std::vector<TypedCase>& casel) {
  std::vector<Pats> pss = initial_matrix(casel);
  pss = get_mins<Pats>([](const Pats& a, const Pats& b) { return le_pats(a, b); }, pss);
  tt::Partial total = do_check_partial(pred, loc, casel, pss);
  if (total == tt::Partial::Total && warnings::is_active(warnings::Fragile_match)) do_check_fragile(loc, casel, pss);
  return total;
}

// ---- ambiguous variables in or-patterns under a guard (warning 57) ----------------------
namespace {
struct IdCmp {
  bool operator()(Ident::t a, Ident::t b) const { return ident::compare(a, b) < 0; }
};
using IdSet = std::set<Ident::t, IdCmp>;

IdSet inter(const IdSet& a, const IdSet& b) {
  IdSet r;
  for (Ident::t x : a)
    if (b.count(x)) r.insert(x);
  return r;
}

// Row for ambiguous variable search (see parmatch.ml): the traditional
// pattern row, and the head variable sets
struct AmbRow {
  Pats row;
  std::vector<IdSet> varsets;
};
// ('a, 'b) signed = Positive of amb_row | Negative of pattern list
struct Signed {
  bool positive;
  AmbRow pos;
  Pats neg;
  bool empty() const { return positive ? pos.row.empty() : neg.empty(); }
};

// simplify_head_amb_pat: the variables of the head pattern are collected in
// a new varset
void simplify_head_amb_pat(IdSet head_bound_variables, const std::vector<IdSet>& varsets, const tt::Pattern* p,
                           const Pats& ps, SMatrix<Signed>& out) {
  const tt::PatternDesc* d = p->pat_desc;
  if (auto* a = as<tt::Tpat_alias>(d)) {
    head_bound_variables.insert(a->id);
    simplify_head_amb_pat(head_bound_variables, varsets, a->pat, ps, out);
  } else if (auto* v = as<tt::Tpat_var>(d)) {
    head_bound_variables.insert(v->id);
    simplify_head_amb_pat(head_bound_variables, varsets, omega(), ps, out);
  } else if (auto* o = as<tt::Tpat_or>(d)) {
    simplify_head_amb_pat(head_bound_variables, varsets, o->p1, ps, out);
    simplify_head_amb_pat(head_bound_variables, varsets, o->p2, ps, out);
  } else {
    std::vector<IdSet> vs{head_bound_variables};
    vs.insert(vs.end(), varsets.begin(), varsets.end());
    out.push_back({deconstruct(p), Signed{true, AmbRow{ps, vs}, {}}});
  }
}
// simplify_head_pat for a negative row
void simplify_head_neg_pat(const tt::Pattern* p, const Pats& ns, SMatrix<Signed>& out) {
  const tt::Pattern* v = strip_vars(p);
  if (auto* o = as<tt::Tpat_or>(v->pat_desc)) {
    simplify_head_neg_pat(o->p1, ns, out);
    simplify_head_neg_pat(o->p2, ns, out);
    return;
  }
  out.push_back({deconstruct(v), Signed{false, {}, ns}});
}

SMatrix<Signed> simplify_first_amb_col(const std::vector<Signed>& m) {
  SMatrix<Signed> out;
  for (const Signed& r : m) {
    if (r.empty()) throw std::logic_error("Parmatch.simplify_first_amb_col");
    if (!r.positive) {
      simplify_head_neg_pat(r.neg[0], Pats(r.neg.begin() + 1, r.neg.end()), out);
    } else {
      simplify_head_amb_pat({}, r.pos.varsets, r.pos.row[0], Pats(r.pos.row.begin() + 1, r.pos.row.end()), out);
    }
  }
  return out;
}

// stable_vars = All | Vars of Ident.Set.t
struct StableVars {
  bool all;
  IdSet vars;
};
StableVars stable_inter(const StableVars& a, const StableVars& b) {
  if (a.all) return b;
  if (b.all) return a;
  return {false, inter(a.vars, b.vars)};
}

StableVars matrix_stable_vars(const std::vector<Signed>& m) {
  if (m.empty()) return {true, {}};
  if (m[0].empty()) {
    // if at least one empty row is negative, the matrix matches no value
    std::vector<std::vector<IdSet>> rows_varsets;
    for (const Signed& r : m) {
      if (!r.positive) return {true, {}};
      rows_varsets.push_back(r.pos.varsets);
    }
    // reduce (List.map2 Ident.Set.inter)
    std::vector<IdSet> stables = rows_varsets[0];
    for (std::size_t k = 1; k < rows_varsets.size(); ++k) {
      if (rows_varsets[k].size() != stables.size()) throw std::invalid_argument("List.map2");
      for (std::size_t i = 0; i < stables.size(); ++i) stables[i] = inter(stables[i], rows_varsets[k][i]);
    }
    // The stable variables are those stable at any position
    IdSet u;
    for (auto& st : stables) u.insert(st.begin(), st.end());
    return {false, u};
  }
  bool all_negative = true;
  for (const Signed& r : m) all_negative = all_negative && !r.positive;
  // optimization: quit early if there are no positive rows
  if (all_negative) return {true, {}};
  SMatrix<Signed> sm = simplify_first_amb_col(m);
  if (!all_coherent(first_column(sm))) return {true, {}};
  auto extend_row = [](const Pats& columns, const Signed& r) {
    Signed x = r;
    if (x.positive) x.pos.row.insert(x.pos.row.begin(), columns.begin(), columns.end());
    else x.neg.insert(x.neg.begin(), columns.begin(), columns.end());
    return x;
  };
  const Head* q0 = discr_pat(omega(), sm);
  SpecializedMatrices<Signed> spec = build_specialized_submatrices<Signed>(extend_row, q0, sm);
  std::vector<std::vector<Signed>> submatrices;
  if (!full_match(false, spec.constrs)) submatrices.push_back(spec.default_);
  for (auto& [h, rows] : spec.constrs) submatrices.push_back(rows);
  // A stable variable must be stable in each submatrix.
  StableVars acc{true, {}};
  for (auto& sub : submatrices) acc = stable_inter(acc, matrix_stable_vars(sub));
  return acc;
}

StableVars pattern_stable_vars(const std::vector<Pats>& ns, const tt::Pattern* p) {
  // List.fold_left (fun m n -> Negative n :: m) [Positive ...] ns
  std::vector<Signed> m{Signed{true, AmbRow{{p}, {}}, {}}};
  for (const Pats& n : ns) m.insert(m.begin(), Signed{false, {}, n});
  return matrix_stable_vars(m);
}

// All identifier paths' heads that appear in an expression (Tast_iterator's
// default traversal down to Texp_ident)
struct RhsIdents {
  IdSet ids;
  void path(Path::t p) {
    for (Ident::t id : path::heads(p)) ids.insert(id);
  }
  void vbs(Slice<const tt::ValueBinding*> l) {
    for (auto* vb : l) expr(vb->vb_expr);
  }
  void cases(Slice<const tt::Case*> l) {
    for (auto* c : l) {
      if (c->c_guard) expr(c->c_guard);
      expr(c->c_rhs);
    }
  }
  void module_expr(const tt::ModuleExpr* me) {
    const tt::ModuleExprDesc* d = me->mod_desc;
    if (auto* x = as<tt::Tmod_structure>(d)) structure(x->str);
    else if (auto* x = as<tt::Tmod_functor>(d)) module_expr(x->body);
    else if (auto* x = as<tt::Tmod_apply>(d)) {
      module_expr(x->fn);
      module_expr(x->arg);
    } else if (auto* x = as<tt::Tmod_apply_unit>(d)) module_expr(x->fn);
    else if (auto* x = as<tt::Tmod_constraint>(d)) module_expr(x->me);
    else if (auto* x = as<tt::Tmod_unpack>(d)) expr(x->exp);
  }
  void class_expr(const tt::ClassExpr* ce) {
    const tt::ClassExprDesc* d = ce->cl_desc;
    if (auto* x = as<tt::Tcl_constraint>(d)) class_expr(x->ce);
    else if (auto* x = as<tt::Tcl_structure>(d)) class_structure(x->cs);
    else if (auto* x = as<tt::Tcl_fun>(d)) {
      for (auto& a : x->args) expr(a.exp);
      class_expr(x->ce);
    } else if (auto* x = as<tt::Tcl_apply>(d)) {
      class_expr(x->ce);
      for (auto& a : x->args)
        if (!a.arg.omitted) expr(a.arg.arg);
    } else if (auto* x = as<tt::Tcl_let>(d)) {
      vbs(x->vbs);
      for (auto& v : x->vals) expr(v.exp);
      class_expr(x->ce);
    } else if (auto* x = as<tt::Tcl_open>(d)) class_expr(x->ce);
  }
  void class_structure(const tt::ClassStructure* cs) {
    for (const tt::ClassField* f : cs->cstr_fields) {
      const tt::ClassFieldDesc* d = f->cf_desc;
      if (auto* x = as<tt::Tcf_inherit>(d)) class_expr(x->ce);
      else if (auto* x = as<tt::Tcf_val>(d)) {
        if (!x->kind_.is_virtual) expr(x->kind_.exp);
      } else if (auto* x = as<tt::Tcf_method>(d)) {
        if (!x->kind_.is_virtual) expr(x->kind_.exp);
      } else if (auto* x = as<tt::Tcf_initializer>(d)) expr(x->exp);
    }
  }
  void structure_item(const tt::StructureItem* it) {
    const tt::StructureItemDesc* d = it->str_desc;
    if (auto* x = as<tt::Tstr_eval>(d)) expr(x->exp);
    else if (auto* x = as<tt::Tstr_value>(d)) vbs(x->vbs);
    else if (auto* x = as<tt::Tstr_module>(d)) module_expr(x->mb->mb_expr);
    else if (auto* x = as<tt::Tstr_recmodule>(d)) {
      for (auto* mb : x->mbs) module_expr(mb->mb_expr);
    } else if (auto* x = as<tt::Tstr_open>(d)) module_expr(x->od->open_expr);
    else if (auto* x = as<tt::Tstr_include>(d)) module_expr(x->incl->incl_mod);
    else if (auto* x = as<tt::Tstr_class>(d)) {
      for (auto& c : x->classes) class_expr(c.decl->ci_expr);
    }
  }
  void structure(const tt::Structure* str) {
    for (auto* it : str->str_items) structure_item(it);
  }
  void expr(const tt::Expression* e) {
    const tt::ExpressionDesc* d = e->exp_desc;
    using XK = tt::ExpressionDesc::Kind;
    switch (d->kind) {
      case XK::Texp_ident: path(as<tt::Texp_ident>(d)->path); return;
      case XK::Texp_let: {
        auto* x = as<tt::Texp_let>(d);
        vbs(x->vbs);
        expr(x->body);
        return;
      }
      case XK::Texp_function: {
        auto* x = as<tt::Texp_function>(d);
        for (auto* fp : x->params)
          if (fp->fp_kind.kind == tt::FunctionParamKind::Kind::Tparam_optional_default) expr(fp->fp_kind.default_);
        if (x->body->kind == tt::FunctionBody::Kind::Tfunction_body) expr(x->body->body);
        else cases(x->body->cases);
        return;
      }
      case XK::Texp_apply: {
        auto* x = as<tt::Texp_apply>(d);
        expr(x->fn);
        for (auto& a : x->args)
          if (!a.arg.omitted) expr(a.arg.arg);
        return;
      }
      case XK::Texp_match: {
        auto* x = as<tt::Texp_match>(d);
        expr(x->exp);
        cases(x->comp_cases);
        cases(x->eff_cases);
        return;
      }
      case XK::Texp_try: {
        auto* x = as<tt::Texp_try>(d);
        expr(x->exp);
        cases(x->exn_cases);
        cases(x->eff_cases);
        return;
      }
      case XK::Texp_tuple:
        for (auto& le : as<tt::Texp_tuple>(d)->el) expr(le.exp);
        return;
      case XK::Texp_construct:
        for (auto* a : as<tt::Texp_construct>(d)->args) expr(a);
        return;
      case XK::Texp_variant:
        if (auto* a = as<tt::Texp_variant>(d)->arg) expr(a);
        return;
      case XK::Texp_record: {
        auto* x = as<tt::Texp_record>(d);
        for (auto& f : x->fields)
          if (!f.def.kept) expr(f.def.exp);
        if (x->extended_expression) expr(x->extended_expression);
        return;
      }
      case XK::Texp_atomic_loc: expr(as<tt::Texp_atomic_loc>(d)->exp); return;
      case XK::Texp_field: expr(as<tt::Texp_field>(d)->exp); return;
      case XK::Texp_setfield: {
        auto* x = as<tt::Texp_setfield>(d);
        expr(x->exp);
        expr(x->value);
        return;
      }
      case XK::Texp_array:
        for (auto* a : as<tt::Texp_array>(d)->el) expr(a);
        return;
      case XK::Texp_ifthenelse: {
        auto* x = as<tt::Texp_ifthenelse>(d);
        expr(x->cond);
        expr(x->then_);
        if (x->else_) expr(x->else_);
        return;
      }
      case XK::Texp_sequence: {
        auto* x = as<tt::Texp_sequence>(d);
        expr(x->e1);
        expr(x->e2);
        return;
      }
      case XK::Texp_while: {
        auto* x = as<tt::Texp_while>(d);
        expr(x->cond);
        expr(x->body);
        return;
      }
      case XK::Texp_for: {
        auto* x = as<tt::Texp_for>(d);
        expr(x->lo);
        expr(x->hi);
        expr(x->body);
        return;
      }
      case XK::Texp_send: expr(as<tt::Texp_send>(d)->obj); return;
      case XK::Texp_setinstvar: expr(as<tt::Texp_setinstvar>(d)->value); return;
      case XK::Texp_override:
        for (auto& f : as<tt::Texp_override>(d)->fields) expr(f.exp);
        return;
      case XK::Texp_assert: expr(as<tt::Texp_assert>(d)->exp); return;
      case XK::Texp_lazy: expr(as<tt::Texp_lazy>(d)->exp); return;
      case XK::Texp_object: class_structure(as<tt::Texp_object>(d)->cs); return;
      case XK::Texp_pack: module_expr(as<tt::Texp_pack>(d)->me); return;
      case XK::Texp_letop: {
        auto* x = as<tt::Texp_letop>(d);
        expr(x->let_->bop_exp);
        for (auto* a : x->ands) expr(a->bop_exp);
        if (x->body->c_guard) expr(x->body->c_guard);
        expr(x->body->c_rhs);
        return;
      }
      case XK::Texp_struct_item: {
        auto* x = as<tt::Texp_struct_item>(d);
        structure_item(x->item);
        expr(x->body);
        return;
      }
      default: return;
    }
  }
};
}  // namespace

void check_ambiguous_bindings(const std::vector<const tt::Case*>& cases) {
  if (!warnings::is_active(57)) return;
  std::vector<Pats> ns;  // head first
  for (const tt::Case* c : cases) {
    const tt::Pattern* p = c->c_lhs;
    if (!c->c_guard) {
      ns.insert(ns.begin(), Pats{p});
      continue;
    }
    RhsIdents ri;
    ri.expr(c->c_guard);
    IdSet pv;
    for (Ident::t id : tt::pat_bound_idents(p)) pv.insert(id);
    IdSet all = inter(pv, ri.ids);
    if (!all.empty()) {
      StableVars sv = pattern_stable_vars(ns, p);
      if (!sv.all) {
        std::vector<std::string> pps;
        for (Ident::t id : all)
          if (!sv.vars.count(id)) pps.push_back(std::string(ident::name(id)));
        if (!pps.empty())
          location::prerr_warning(p->pat_loc,
                                  warnings::Warning::with_l(warnings::Warning::K::Ambiguous_var_in_pattern_guard, pps));
      }
    }
  }
}

}  // namespace cppcaml::typing::parmatch
